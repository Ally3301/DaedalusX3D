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

#include <cstring>
#include <new>

#include "AudioPluginCTR.h"
#include "AudioOutput.h"
#include "HLEAudio/audiohle.h"

#include "Config/ConfigOptions.h"
#include "Core/Memory.h"
#include "Core/RSP_HLE.h"

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
	return true;
}

//*****************************************************************************
//
//*****************************************************************************
void	CAudioPluginCTR::StopEmulation()
{
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

	if( gAudioPluginEnabled > APM_DISABLED )
	{
		u32		address( Memory_AI_GetRegister(AI_DRAM_ADDR_REG) & 0xFFFFFF );
		u32		length(Memory_AI_GetRegister(AI_LEN_REG));
		if (address >= gRamSize)
			return;
		if (length > gRamSize - address)
			length = gRamSize - address;

		mAudioOutput->AddBuffer( g_pu8RamBase + address, length );
	}
	else
	{
		mAudioOutput->StopAudio();
	}
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
		case APM_ENABLED_SYNC:
			// Synchronous Audio_Ucode matches upstream DaedalusX64 on Old 3DS
			// (avoids SP task yielding freezes and Core 1 syscore starvation).
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
