#include "stdafx.h"

#include <string.h>

#include "audiohle.h"
#include "AudioHLEProcessor.h"

#include "Core/Memory.h"
#include "Math/MathUtil.h"

#include "Debug/DBGConsole.h"

extern bool isMKABI;
extern bool isZeldaABI;

inline s32		FixedPointMul16( s32 a, s32 b )
{
	return s32( ( a * b ) >> 16 );
}

void RESAMPLE(AudioHLECommand command)
{
	#ifdef DEBUG_AUDIO
		DBGConsole_Msg(0, "RESAMPLE");
		#endif
  u8 flags(command.Abi1Resample.Flags);
	u32 pitch(command.Abi1Resample.Pitch);
	u32 address(command.Abi1Resample.Address);// + gAudioHLEState.Segments[(command.cmd1>>24)&0xf];

	gAudioHLEState.Resample( flags, pitch, address );
}

void RESAMPLE2(AudioHLECommand command)
{
	#ifdef DEBUG_AUDIO
		DBGConsole_Msg(0, "RESAMPLE2");
		#endif
u8 flags(command.Abi2Resample.Flags);
u32 pitch(command.Abi2Resample.Pitch);
u32 address(command.Abi2Resample.Address);// + gAudioHLEState.Segments[(command.cmd1>>24)&0xf];

gAudioHLEState.Resample( flags, pitch, address );
}

void RESAMPLE3(AudioHLECommand command)
{
	#ifdef DEBUG_AUDIO
		DBGConsole_Msg(0, "RESAMPLE3");
		#endif
  u8 Flags=(u8)((command.cmd1>>0x1e));
  	u32 Pitch=((command.cmd1>>0xe)&0xffff) << 1;
  	u32 addy = (command.cmd0 & 0xffffff);
  	u32 Accum;
  	s16 *dst;
  	s16 *src;
  	dst=(s16 *)(gAudioHLEState.Buffer);
  	src=(s16 *)(gAudioHLEState.Buffer);
  	u32 srcPtr=((((command.cmd1>>2)&0xfff)+0x4f0)/2);
  	u32 dstPtr;//=(gAudioHLEState.OutBuffer/2);

  	srcPtr -= 1;

  	if (command.cmd1 & 0x3) {
  		dstPtr = 0x660/2;
  	} else {
  		dstPtr = 0x4f0/2;
  	}

  	if ((Flags & 0x1) == 0) {
  		src[srcPtr^1] = ((u16 *)rdram)[((addy/2))^1];
  		Accum = *(u16 *)(rdram+addy+10);
  	} else {
  		src[(srcPtr)^1] = 0;
  		Accum = 0;
  	}

  	s16 * outPtr = dst + dstPtr;
  	for(u32 i=0;i < 0x170/4;i++)
  	{
  		const s32 s0 = src[srcPtr^1];
  		const s32 s1 = src[(srcPtr+1)^1];
  		const s16 v0 = static_cast<s16>(s0 + FixedPointMul16( s1 - s0, Accum ));
  		Accum += Pitch;
  		srcPtr += (Accum>>16);
  		Accum &= 0xFFFF;

  		const s32 s2 = src[srcPtr^1];
  		const s32 s3 = src[(srcPtr+1)^1];
  		const s16 v1 = static_cast<s16>(s2 + FixedPointMul16( s3 - s2, Accum ));
  		Accum += Pitch;
  		srcPtr += (Accum>>16);
  		Accum &= 0xFFFF;

  		outPtr[1] = v0;
  		outPtr[0] = v1;
  		outPtr += 2;
  	}

  	((u16 *)rdram)[((addy/2))^1] = src[srcPtr^1];
  	*(u16 *)(rdram+addy+10) = u16( Accum );
}
