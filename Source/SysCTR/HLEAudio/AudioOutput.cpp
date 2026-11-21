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

#include "stdafx.h"
#include "AudioOutput.h"

#include <stdio.h>
#include <new>

#include <3ds.h>

#include "Config/ConfigOptions.h"
#include "Debug/DBGConsole.h"
#include "HLEAudio/AudioBuffer.h"
#include "Utility/FramerateLimiter.h"
#include "Utility/Thread.h"

extern u32 gSoundSync;

static const u32 DESIRED_OUTPUT_FREQUENCY = 44100;

// Keep enough queued PCM to absorb frame-time variance; allow only a subtle
// <=6% playback-rate glide so minor dips below 30 FPS stay smooth without
// audible pitch distortion.
static const u32 CTR_BUFFER_SIZE        = 1024 * 8;
static const u32 CTR_BUFFER_COUNT       = 6;
static const u32 CTR_NUM_SAMPLES        = 512;
static const u32 CTR_AUDIO_MIN_RATE     = (DESIRED_OUTPUT_FREQUENCY * 94) / 100; // ~41454 Hz (-6% max)
static const u32 CTR_AUDIO_TARGET_FILL  = CTR_BUFFER_SIZE / 2;
static const u32 CTR_AUDIO_LOW_FILL     = CTR_BUFFER_SIZE / 8;


static ndspWaveBuf waveBuf[CTR_BUFFER_COUNT];
static unsigned int waveBuf_id;

bool audioOpen = false;

static LightLock sNdspLock;
static bool      sNdspLockInit   = false;
static int       sNdspRefCount   = 0;
static bool      sNdspInitFailed = false;

static AudioOutput * ac = nullptr;

CAudioBuffer *mAudioBuffer = nullptr;

bool AudioOutput::AcquireNdsp()
{
	if (!sNdspLockInit)
	{
		LightLock_Init(&sNdspLock);
		sNdspLockInit = true;
	}

	LightLock_Lock(&sNdspLock);
	if (sNdspRefCount > 0)
	{
		++sNdspRefCount;
		LightLock_Unlock(&sNdspLock);
		return true;
	}
	if (sNdspInitFailed)
	{
		LightLock_Unlock(&sNdspLock);
		return false;
	}
	if (ndspInit() != 0)
	{
		sNdspInitFailed = true;
		LightLock_Unlock(&sNdspLock);
		return false;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	sNdspRefCount = 1;
	LightLock_Unlock(&sNdspLock);
	return true;
}

void AudioOutput::ReleaseNdsp()
{
	if (!sNdspLockInit)
		return;

	LightLock_Lock(&sNdspLock);
	if (sNdspRefCount > 0)
	{
		--sNdspRefCount;
		if (sNdspRefCount == 0)
		{
			ndspExit();
		}
	}
	LightLock_Unlock(&sNdspLock);
}

void AudioOutput::SetGamePaused( bool paused )
{
	if (audioOpen)
	{
		ndspChnSetPaused(0, paused);
	}
}

static void audioCallback(void *arg)
{
	(void)arg;

	if (!audioOpen || mAudioBuffer == nullptr || !mAudioBuffer->IsValid())
		return;

	for (u32 i = 0; i < CTR_BUFFER_COUNT; ++i)
	{
		if (waveBuf[waveBuf_id].status != NDSP_WBUF_DONE || waveBuf[waveBuf_id].data_pcm16 == nullptr)
			break;

		// Drain() copies available frames and zero-fills any underrun. Always
		// submit a full block so short reads do not shorten the NDSP clock.
		mAudioBuffer->Drain( reinterpret_cast< Sample * >( waveBuf[waveBuf_id].data_pcm16 ), CTR_NUM_SAMPLES );
		waveBuf[waveBuf_id].nsamples = CTR_NUM_SAMPLES;

		DSP_FlushDataCache(waveBuf[waveBuf_id].data_pcm16, CTR_NUM_SAMPLES * sizeof(Sample));
		ndspChnWaveBufAdd( 0, &waveBuf[waveBuf_id] );

		waveBuf_id = (waveBuf_id + 1) % CTR_BUFFER_COUNT;
	}
}

static bool AudioInit()
{
	for (u32 i = 0; i < CTR_BUFFER_COUNT; ++i)
		waveBuf[i] = {};

	if (!AudioOutput::AcquireNdsp())
		return false;

	ndspChnReset(0);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	ndspChnSetRate(0, DESIRED_OUTPUT_FREQUENCY);
	float mix[12] = {};
	mix[0] = 1.0f;
	mix[1] = 1.0f;
	ndspChnSetMix(0, mix);

	for (u32 i = 0; i < CTR_BUFFER_COUNT; ++i)
	{
		waveBuf[i].data_vaddr = linearAlloc(CTR_NUM_SAMPLES * 4);
		if (waveBuf[i].data_vaddr == nullptr)
		{
			ndspChnWaveBufClear(0);
			for (int wait = 0; wait < 20 && ndspChnIsPlaying(0); ++wait)
				svcSleepThread(1000000LL);
			AudioOutput::ReleaseNdsp();
			for (u32 j = 0; j < i; ++j)
			{
				linearFree(const_cast<void *>(waveBuf[j].data_vaddr));
				waveBuf[j] = {};
			}
			return false;
		}

		waveBuf[i].nsamples = CTR_NUM_SAMPLES;
		waveBuf[i].status = 0;
		memset(waveBuf[i].data_pcm16, 0, CTR_NUM_SAMPLES * sizeof(Sample));
		DSP_FlushDataCache(waveBuf[i].data_pcm16, CTR_NUM_SAMPLES * sizeof(Sample));
		ndspChnWaveBufAdd(0, &waveBuf[i]);
	}

	waveBuf_id = 0;

	ndspSetCallback(&audioCallback, nullptr);

	// Everything OK
	audioOpen = true;
	return true;
}

static void AudioExit()
{
	// Stop stream
	if (audioOpen)
	{
		audioOpen = false;
		ndspSetCallback(nullptr, nullptr);
		ndspChnSetPaused(0, false);
		ndspChnWaveBufClear(0);
		// Wait briefly for the DSP frame to process the clear command before
		// freeing Linear RAM when NDSP remains active for UI SFX channels.
		for (int wait = 0; wait < 20 && ndspChnIsPlaying(0); ++wait)
			svcSleepThread(1000000LL);
		AudioOutput::ReleaseNdsp();
	}

	for (u32 i = 0; i < CTR_BUFFER_COUNT; ++i)
	{
		if (waveBuf[i].data_vaddr != nullptr)
			linearFree(const_cast<void *>(waveBuf[i].data_vaddr));
		waveBuf[i] = {};
	}

	audioOpen = false;
}

AudioOutput::AudioOutput()
:	mAudioPlaying( false )
,	mFrequency( 44100 )
,	mPlaybackRate( DESIRED_OUTPUT_FREQUENCY )
{
	mAudioBuffer = new (std::nothrow) CAudioBuffer( CTR_BUFFER_SIZE );
	if (mAudioBuffer != nullptr && !mAudioBuffer->IsValid())
	{
		delete mAudioBuffer;
		mAudioBuffer = nullptr;
	}
}

AudioOutput::~AudioOutput( )
{
	StopAudio();
	delete mAudioBuffer;
	mAudioBuffer = nullptr;
	if (ac == this)
		ac = nullptr;
}

void AudioOutput::SetFrequency( u32 frequency )
{
	mFrequency = frequency;
}

void AudioOutput::UpdatePlaybackRate()
{
	if (!mAudioPlaying || !audioOpen || mAudioBuffer == nullptr)
		return;

	const u32 queued = mAudioBuffer->GetNumBufferedSamples();
	u32 target_rate = DESIRED_OUTPUT_FREQUENCY;
	if (queued <= CTR_AUDIO_LOW_FILL)
	{
		target_rate = CTR_AUDIO_MIN_RATE;
	}
	else if (queued < CTR_AUDIO_TARGET_FILL)
	{
		const u32 range = CTR_AUDIO_TARGET_FILL - CTR_AUDIO_LOW_FILL;
		const u32 fill = queued - CTR_AUDIO_LOW_FILL;
		target_rate = CTR_AUDIO_MIN_RATE +
			((DESIRED_OUTPUT_FREQUENCY - CTR_AUDIO_MIN_RATE) * fill) / range;
	}

	const s32 delta = static_cast<s32>(target_rate) - static_cast<s32>(mPlaybackRate);
	if (delta > -300 && delta < 300)
		return;

	s32 next_rate = static_cast<s32>(mPlaybackRate) + delta / 4;
	if (next_rate < static_cast<s32>(CTR_AUDIO_MIN_RATE))
		next_rate = CTR_AUDIO_MIN_RATE;
	else if (next_rate > static_cast<s32>(DESIRED_OUTPUT_FREQUENCY))
		next_rate = DESIRED_OUTPUT_FREQUENCY;

	if (static_cast<u32>(next_rate) != mPlaybackRate)
	{
		mPlaybackRate = static_cast<u32>(next_rate);
		ndspChnSetRate(0, mPlaybackRate);
	}
}

void AudioOutput::AddBuffer( u8 *start, u32 length )
{
	if (length == 0 || mAudioBuffer == nullptr || !mAudioBuffer->IsValid())
		return;

	u32 num_samples = length / sizeof( Sample );
	if (start == nullptr || num_samples < 2 || mFrequency == 0)
		return;

	if (!mAudioPlaying)
		StartAudio();
	// With no callback to drain the ring, AddSamples would spin forever.
	if (!mAudioPlaying)
		return;

	// Natural 44.1 kHz output unless user explicitly turns on AudioRateMatch.
	u32 output_freq = DESIRED_OUTPUT_FREQUENCY;
	if (gAudioRateMatch)
	{
		if (gSoundSync > DESIRED_OUTPUT_FREQUENCY * 2)
			output_freq = DESIRED_OUTPUT_FREQUENCY * 2;
		else if (gSoundSync > DESIRED_OUTPUT_FREQUENCY)
			output_freq = gSoundSync;
	}

	mAudioBuffer->AddSamples( reinterpret_cast< const Sample * >( start ), num_samples, mFrequency, output_freq );
	UpdatePlaybackRate();
}

void AudioOutput::StartAudio()
{
	if (mAudioPlaying || mAudioBuffer == nullptr || !mAudioBuffer->IsValid())
		return;

	ac = this;
	mPlaybackRate = DESIRED_OUTPUT_FREQUENCY;
	mAudioPlaying = AudioInit();
}

void AudioOutput::StopAudio()
{
	if (!mAudioPlaying)
		return;

	mAudioPlaying = false;

	AudioExit();
	mPlaybackRate = DESIRED_OUTPUT_FREQUENCY;
}
