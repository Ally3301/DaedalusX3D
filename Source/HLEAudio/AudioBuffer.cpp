/*
Copyright (C) 2007 StrmnNrmn

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
#include "AudioBuffer.h"

#include "Config/ConfigOptions.h"
#include "Debug/DBGConsole.h"
#include "Utility/Thread.h"

#include <new>

#ifdef DAEDALUS_PSP
#include <pspsdk.h>
#include <pspkernel.h>
#include "SysPSP/Utility/CacheUtil.h"
#endif

CAudioBuffer::CAudioBuffer( u32 buffer_size )
	:	mBufferBegin( buffer_size > 1 ? new (std::nothrow) Sample[ buffer_size ] : nullptr )
	,	mBufferSize( mBufferBegin != nullptr ? buffer_size : 0 )
	,	mReadIndex( 0 )
	,	mWriteIndex( 0 )
	,	mLastDrainSample{ 0, 0 }
	,	mInUnderrun( false )
{
}

CAudioBuffer::~CAudioBuffer()
{
	delete [] mBufferBegin;
	mBufferBegin = nullptr;
	mBufferSize = 0;
}

u32 CAudioBuffer::GetNumBufferedSamples() const
{
	if (mBufferBegin == nullptr || mBufferSize == 0)
		return 0;

	const u32 read_idx = mReadIndex.load( std::memory_order_acquire );
	const u32 write_idx = mWriteIndex.load( std::memory_order_acquire );

	if (write_idx >= read_idx)
		return write_idx - read_idx;

	return mBufferSize - (read_idx - write_idx);
}

void CAudioBuffer::AddSamples( const Sample * samples, u32 num_samples, u32 frequency, u32 output_freq )
{
	if (mBufferBegin == nullptr || mBufferSize <= 1)
		return;

	if (samples == nullptr || num_samples < 2 || frequency == 0 || output_freq == 0)
		return;

	const u64 step = (static_cast<u64>(frequency) << 12) / output_freq;
	const u64 output_count = (static_cast<u64>(num_samples - 1) * output_freq) / frequency;
	if (step == 0 || step > 0x7ffff000 || output_count > 0xffffffffu)
		return;
	const s32 r = static_cast<s32>(step);
	s32 s = 0;
	u32 in_idx = 0;
	u32 output_samples = static_cast<u32>(output_count);

	u32 read_idx = mReadIndex.load( std::memory_order_acquire );
	u32 write_idx = mWriteIndex.load( std::memory_order_relaxed );

#ifdef DAEDALUS_CTR
	// Compute free ring capacity once up front so the inner resampling loop
	// executes with zero per-sample atomic loads and zero ring-wrap checks.
	const u32 free_space = (read_idx > write_idx)
		? (read_idx - write_idx - 1)
		: (mBufferSize - write_idx + read_idx - 1);
	if (free_space == 0)
		return;
	if (output_samples > free_space)
		output_samples = free_space;

	const u32 max_in = num_samples - 1;
	u32 remaining = output_samples;
	while (remaining > 0 && in_idx < max_in)
	{
		const u32 to_end = mBufferSize - write_idx;
		const u32 chunk = (remaining < to_end) ? remaining : to_end;
		Sample * __restrict dst = mBufferBegin + write_idx;
		u32 written = 0;

		for (; written < chunk && in_idx < max_in; ++written)
		{
			const Sample s0 = samples[ in_idx ];
			const Sample s1 = samples[ in_idx + 1 ];
			dst[ written ].L = static_cast<s16>(s0.L + (((s1.L - s0.L) * s) >> 12));
			dst[ written ].R = static_cast<s16>(s0.R + (((s1.R - s0.R) * s) >> 12));
			s += r;
			in_idx += s >> 12;
			s &= 4095;
		}

		write_idx += written;
		if (write_idx >= mBufferSize)
			write_idx = 0;
		remaining -= written;
	}

	mWriteIndex.store( write_idx, std::memory_order_release );
#else
	for( u32 i = output_samples; i != 0 ; i-- )
	{
		if (in_idx >= num_samples - 1)
			break;

		// Resample in integer mode (faster & less ASM code) //Corn
		Sample	out;

		out.L = samples[ in_idx ].L + ((( samples[ in_idx + 1 ].L - samples[ in_idx ].L ) * s ) >> 12 );
		out.R = samples[ in_idx ].R + ((( samples[ in_idx + 1 ].R - samples[ in_idx ].R ) * s ) >> 12 );

		s += r;
		in_idx += s >> 12;
		s &= 4095;

		u32 next_write = write_idx + 1;
		if (next_write >= mBufferSize)
			next_write = 0;

		while (next_write == read_idx)
		{
			read_idx = mReadIndex.load( std::memory_order_acquire );
			if (next_write != read_idx)
				break;

			mWriteIndex.store( write_idx, std::memory_order_release );
			read_idx = mReadIndex.load( std::memory_order_acquire );
		}

		mBufferBegin[ write_idx ] = out;
		write_idx = next_write;
	}

	mWriteIndex.store( write_idx, std::memory_order_release );
#endif
}

u32	CAudioBuffer::Drain( Sample * samples, u32 num_samples )
{
	if (samples == nullptr || num_samples == 0)
		return 0;

	if (mBufferBegin == nullptr || mBufferSize <= 1)
	{
		memset( samples, 0, num_samples * sizeof( Sample ) );
		return 0;
	}

	u32 read_idx = mReadIndex.load( std::memory_order_relaxed );
	const u32 write_idx = mWriteIndex.load( std::memory_order_acquire );

	const u32 available = (write_idx >= read_idx)
		? (write_idx - read_idx)
		: (mBufferSize - read_idx + write_idx);
	const u32 to_copy = (available < num_samples) ? available : num_samples;

	if (to_copy > 0)
	{
		const u32 first_chunk = ((mBufferSize - read_idx) < to_copy)
			? (mBufferSize - read_idx)
			: to_copy;
		memcpy(samples, mBufferBegin + read_idx, first_chunk * sizeof(Sample));
		const u32 second_chunk = to_copy - first_chunk;
		if (second_chunk > 0)
		{
			memcpy(samples + first_chunk, mBufferBegin, second_chunk * sizeof(Sample));
		}
		read_idx += to_copy;
		if (read_idx >= mBufferSize)
			read_idx -= mBufferSize;
		mReadIndex.store( read_idx, std::memory_order_release );

		// If recovering from an underrun, smoothly crossfade the first 32 samples
		// from mLastDrainSample into the new audio stream to eliminate pops/clicks.
		if (mInUnderrun)
		{
			const u32 fade_len = (to_copy < 32) ? to_copy : 32;
			const s32 prevL = mLastDrainSample.L;
			const s32 prevR = mLastDrainSample.R;
			for (u32 i = 0; i < fade_len; ++i)
			{
				const s32 w = static_cast<s32>(i + 1);
				samples[i].L = static_cast<s16>(((prevL * (32 - w)) + (static_cast<s32>(samples[i].L) * w)) >> 5);
				samples[i].R = static_cast<s16>(((prevR * (32 - w)) + (static_cast<s32>(samples[i].R) * w)) >> 5);
			}
			mInUnderrun = false;
		}
		mLastDrainSample = samples[to_copy - 1];
	}

	//
	//	If there weren't enough samples, smoothly decay the last sample toward 0
	//	instead of hard-zeroing (which causes loud square-wave pops/crackles).
	//
	if (to_copy < num_samples)
	{
		mInUnderrun = true;
		s32 curL = mLastDrainSample.L;
		s32 curR = mLastDrainSample.R;
		for (u32 i = to_copy; i < num_samples; ++i)
		{
			curL = (curL * 248) >> 8;
			curR = (curR * 248) >> 8;
			samples[i].L = static_cast<s16>(curL);
			samples[i].R = static_cast<s16>(curR);
		}
		mLastDrainSample.L = static_cast<s16>(curL);
		mLastDrainSample.R = static_cast<s16>(curR);
	}

	// Return the number of samples written
	return to_copy;
}
