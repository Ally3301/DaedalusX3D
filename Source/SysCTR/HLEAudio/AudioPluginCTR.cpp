/*
Copyright (C) 2003 Azimer
Copyright (C) 2001,2006 StrmnNrmn

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//
//	N.B. This source code is derived from Azimer's Audio plugin (v0.55?)
//	and modified by StrmnNrmn to work with Daedalus PSP. Thanks Azimer!
//	Drop me a line if you get chance :)
//

#include "stdafx.h"

#include <3ds.h>
#include <atomic>
#include <cstring>
#include <new>

#include "AudioPluginCTR.h"
#include "AudioOutput.h"
#include "HLEAudio/audiohle.h"

#include "Config/ConfigOptions.h"
#include "Core/Memory.h"
#include "Core/RSP_HLE.h"
#include "OSHLE/ultra_sptask.h"
#include "SysCTR/Utility/PerfProfilerCTR.h"

extern bool isN3DS;

//*****************************************************************************
// Core 1 (Old 3DS) / Core 2 (New 3DS) Async Audio RSP Worker:
// Offloads Audio_Ucode (ADPCM + Resample + EnvMixer + Interleave) off Core 0
// with a snapshot of the audio command list and deterministic completion
// barriers before AI DMA reads (LenChanged) or the next audio task.
//*****************************************************************************
namespace
{
	constexpr u32 kMaxAsyncAlistWords = 4096; // 16 KiB (2048 audio commands)
	constexpr u32 kMaxAsyncOutRanges  = 16;

	struct SOutRange
	{
		u32 start;
		u32 end;
	};

	Thread            sAudioWorkerThread = nullptr;
	LightEvent        sAudioStartEvent;
	LightEvent        sAudioDoneEvent;
	bool              sAudioEventsInit   = false;
	std::atomic<bool> sAudioWorkerQuit{ false };
	std::atomic<bool> sAudioTaskBusy{ false };

	OSTask            sAsyncTask;
	u32               sAsyncAlistWords[kMaxAsyncAlistWords];
	u32               sAsyncNumCmds      = 0;
	SOutRange         sAsyncOutRanges[kMaxAsyncOutRanges];
	u32               sAsyncOutCount     = 0;
	bool              sAsyncOutConservative = true;

	void RecordAsyncOutputRanges(const u32 * alist, u32 num_cmds)
	{
		sAsyncOutCount = 0;
		sAsyncOutConservative = false;
		u32 lastSetBuffCount = 0;
		for (u32 i = 0; i < num_cmds; ++i)
		{
			const u32 cmd0 = alist[i * 2];
			const u32 cmd1 = alist[i * 2 + 1];
			const u32 op   = (cmd0 >> 24) & 0x1Fu;
			// ABI1 SETBUFF / ABI2 SETBUFF2 = 0x08: track output byte count for ABI1 SAVEBUFF.
			if (op == 0x08)
			{
				const u32 flags = (cmd0 >> 16) & 0xFFu;
				if ((flags & A_AUX) == 0)
				{
					lastSetBuffCount = cmd1 & 0x7FFFu;
				}
			}
			// ABI1/ABI3 SAVEBUFF = 0x06, ABI2 SAVEBUFF2 = 0x15.
			else if (op == 0x06 || op == 0x15)
			{
				const u32 dst = cmd1 & 0x007FFFFFu;
				u32 len = 0;
				if (op == 0x15)
				{
					// SAbi2SaveBuffer: cmd0 bits 12..23 = Count, bits 0..11 = SrcAddr.
					len = (((cmd0 >> 12) & 0x0FFFu) + 3u) & ~3u;
				}
				else
				{
					// ABI3 SAVEBUFF3 encodes count in cmd0 >> 12 (non-zero low 24 bits);
					// ABI1 SAVEBUFF leaves cmd0 low 24 bits zero and uses SETBUFF Count.
					if ((cmd0 & 0x00FFFFFFu) != 0)
						len = (((cmd0 >> 12) + 3u) & 0x0FFCu);
					else
						len = (lastSetBuffCount + 3u) & ~3u;
				}
				const u32 span = (len > 0 && len <= 16384u) ? len : 4096u;
				if (sAsyncOutCount < kMaxAsyncOutRanges)
				{
					sAsyncOutRanges[sAsyncOutCount].start = dst;
					sAsyncOutRanges[sAsyncOutCount].end   = dst + span;
					++sAsyncOutCount;
				}
				else
				{
					sAsyncOutConservative = true;
					return;
				}
			}
		}
		if (sAsyncOutCount == 0)
			sAsyncOutConservative = true;
	}

	bool OverlapsAsyncAudioOutput(u32 address, u32 length)
	{
		if (sAsyncOutConservative)
			return true;
		const u32 reqEnd = address + length;
		for (u32 i = 0; i < sAsyncOutCount; ++i)
		{
			if (address < sAsyncOutRanges[i].end && reqEnd > sAsyncOutRanges[i].start)
				return true;
		}
		return false;
	}

	void AudioWorkerMain(void * arg)
	{
		(void)arg;
		while (true)
		{
			LightEvent_Wait(&sAudioStartEvent);
			if (sAudioWorkerQuit.load(std::memory_order_acquire))
				break;

			if (sAudioTaskBusy.load(std::memory_order_acquire))
			{
				const u64 t0 = svcGetSystemTick();
				Audio_Ucode_Task_WithAlist(sAsyncTask, sAsyncAlistWords, sAsyncNumCmds);
				PerfCTR::AddAudC1(svcGetSystemTick() - t0);
				sAudioTaskBusy.store(false, std::memory_order_release);
				LightEvent_Signal(&sAudioDoneEvent);
			}
		}
	}

	void WaitForAsyncAudioTask()
	{
		if (sAudioWorkerThread != nullptr && sAudioTaskBusy.load(std::memory_order_acquire))
		{
			LightEvent_Wait(&sAudioDoneEvent);
		}
	}

	void StartAsyncAudioWorker()
	{
		if (sAudioWorkerThread != nullptr)
			return;

		if (!sAudioEventsInit)
		{
			LightEvent_Init(&sAudioStartEvent, RESET_ONESHOT);
			LightEvent_Init(&sAudioDoneEvent, RESET_STICKY);
			LightEvent_Signal(&sAudioDoneEvent);
			sAudioEventsInit = true;
		}
		else
		{
			LightEvent_Clear(&sAudioStartEvent);
			LightEvent_Signal(&sAudioDoneEvent);
		}

		sAudioWorkerQuit.store(false, std::memory_order_release);
		sAudioTaskBusy.store(false, std::memory_order_release);

		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		// Pin to Core 2 on New 3DS (dedicated app core), or Core 1 (syscore) on Old 3DS.
		const int coreId = isN3DS ? 2 : 1;
		sAudioWorkerThread = threadCreate(AudioWorkerMain, nullptr, 32 * 1024, prio, coreId, false);
		if (sAudioWorkerThread == nullptr && coreId != 1)
		{
			sAudioWorkerThread = threadCreate(AudioWorkerMain, nullptr, 32 * 1024, prio, 1, false);
		}
	}

	void StopAsyncAudioWorker()
	{
		WaitForAsyncAudioTask();
		if (sAudioWorkerThread != nullptr)
		{
			sAudioWorkerQuit.store(true, std::memory_order_release);
			LightEvent_Signal(&sAudioStartEvent);
			threadJoin(sAudioWorkerThread, U64_MAX);
			threadFree(sAudioWorkerThread);
			sAudioWorkerThread = nullptr;
		}
	}
}

//*****************************************************************************
//
//*****************************************************************************
EAudioPluginMode gAudioPluginEnabled( APM_DISABLED );
//bool gAdaptFrequency( false );

//*****************************************************************************
//
//*****************************************************************************
CAudioPluginCTR::CAudioPluginCTR()
:	mAudioOutput( new (std::nothrow) AudioOutput )
{
}

//*****************************************************************************
//
//*****************************************************************************
CAudioPluginCTR::~CAudioPluginCTR()
{
	StopAsyncAudioWorker();
	delete mAudioOutput;
	mAudioOutput = nullptr;
}

//*****************************************************************************
//
//*****************************************************************************
CAudioPluginCTR *	CAudioPluginCTR::Create()
{
	return new (std::nothrow) CAudioPluginCTR();
}

//*****************************************************************************
//
//*****************************************************************************
/*
void	CAudioPluginCTR::SetAdaptFrequecy( bool adapt )
{
	mAudioOutput->SetAdaptFrequency( adapt );
}
*/
//*****************************************************************************
//
//*****************************************************************************
bool		CAudioPluginCTR::StartEmulation()
{
	if (gAudioPluginEnabled == APM_ENABLED_ASYNC)
		StartAsyncAudioWorker();
	return true;
}

//*****************************************************************************
//
//*****************************************************************************
void	CAudioPluginCTR::StopEmulation()
{
	StopAsyncAudioWorker();
	Audio_Reset();
	if (mAudioOutput != nullptr)
		mAudioOutput->StopAudio();
}

void	CAudioPluginCTR::DacrateChanged( int SystemType )
{
//	printf( "DacrateChanged( %s )\n", (SystemType == ST_NTSC) ? "NTSC" : "PAL" );
	if (mAudioOutput == nullptr)
		return;
	u32 type = (u32)((SystemType == ST_NTSC) ? VI_NTSC_CLOCK : VI_PAL_CLOCK);
	u32 dacrate = Memory_AI_GetRegister(AI_DACRATE_REG);
	u32	frequency = type / (dacrate + 1);

	mAudioOutput->SetFrequency( frequency );
}


//*****************************************************************************
//
//*****************************************************************************
void	CAudioPluginCTR::LenChanged()
{
	if (mAudioOutput == nullptr)
		return;

	const u64 t0 = svcGetSystemTick();

	if( gAudioPluginEnabled > APM_DISABLED )
	{
		u32		address( Memory_AI_GetRegister(AI_DRAM_ADDR_REG) & 0xFFFFFF );
		u32		length(Memory_AI_GetRegister(AI_LEN_REG));
		if (address >= gRamSize)
		{
			PerfCTR::AddAud0(svcGetSystemTick() - t0);
			return;
		}
		if (length > gRamSize - address)
			length = gRamSize - address;

		// Only wait for Core 1 if the AI DMA buffer being queued actually overlaps
		// the RDRAM buffer currently being synthesized by Core 1 (double-buffered
		// audio like Mario Kart 64 queues the previous frame's completed buffer).
		if (sAudioTaskBusy.load(std::memory_order_acquire) &&
			OverlapsAsyncAudioOutput(address, length))
		{
			WaitForAsyncAudioTask();
		}

		mAudioOutput->AddBuffer( g_pu8RamBase + address, length );
	}
	else
	{
		WaitForAsyncAudioTask();
		mAudioOutput->StopAudio();
	}
	PerfCTR::AddAud0(svcGetSystemTick() - t0);
}

//*****************************************************************************
//
//*****************************************************************************
u32		CAudioPluginCTR::ReadLength()
{
	return 0;
}

//*****************************************************************************
//
//*****************************************************************************
EProcessResult	CAudioPluginCTR::ProcessAList()
{
	Memory_SP_SetRegisterBits(SP_STATUS_REG, SP_STATUS_HALT);

	EProcessResult	result( PR_NOT_STARTED );

	switch( gAudioPluginEnabled )
	{
		case APM_DISABLED:
			result = PR_COMPLETED;
			break;
		case APM_ENABLED_ASYNC:
		{
			if (sAudioWorkerThread == nullptr)
				StartAsyncAudioWorker();

			WaitForAsyncAudioTask();

			const OSTask * pTask = reinterpret_cast<const OSTask *>(g_pu8SpMemBase + 0x0FC0);
			const u32 data_ptr   = static_cast<u32>(reinterpret_cast<uintptr_t>(pTask->t.data_ptr)) & 0x00FFFFFFu;
			const u32 max_cmds   = (data_ptr < gRamSize) ? ((gRamSize - data_ptr) >> 3) : 0u;
			u32 num_cmds         = pTask->t.data_size >> 3;
			if (num_cmds > max_cmds)
				num_cmds = max_cmds;

			if (sAudioWorkerThread != nullptr && num_cmds > 0 && (num_cmds * 2) <= kMaxAsyncAlistWords)
			{
				sAsyncTask    = *pTask;
				sAsyncNumCmds = num_cmds;
				std::memcpy(sAsyncAlistWords, g_pu8RamBase + data_ptr, num_cmds * 8);
				RecordAsyncOutputRanges(sAsyncAlistWords, num_cmds);
				LightEvent_Clear(&sAudioDoneEvent);
				sAudioTaskBusy.store(true, std::memory_order_release);
				LightEvent_Signal(&sAudioStartEvent);
			}
			else
			{
				Audio_Ucode();
			}
			result = PR_COMPLETED;
			break;
		}
		case APM_ENABLED_SYNC:
			WaitForAsyncAudioTask();
			Audio_Ucode();
			result = PR_COMPLETED;
			break;
	}

	return result;
}

//*****************************************************************************
//
//*****************************************************************************
CAudioPlugin *		CreateAudioPlugin()
{
	return CAudioPluginCTR::Create();
}
