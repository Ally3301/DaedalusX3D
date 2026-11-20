#include "stdafx.h"

#include <string.h>

#include "audiohle.h"
#include "AudioHLEProcessor.h"

#include "Core/Memory.h"
#include "Math/MathUtil.h"

#include "Debug/DBGConsole.h"

extern bool isMKABI;
extern bool isZeldaABI;

void ADPCM(AudioHLECommand command)
{
  u8		flags( command.Abi1ADPCM.Flags );
//u16	gain( command.Abi1ADPCM.Gain );		// Not used?
u32		address( command.Abi1ADPCM.Address );// + gAudioHLEState.Segments[(command.cmd1>>24)&0xf];

gAudioHLEState.ADPCMDecode( flags, address );

}

inline int Scale16( s16 in, int vscale )
{
	return ((int)in*vscale)>>16;
}

void Decode4_Scale( int (&inp1)[8], u32 icode_a, u32 icode_b, int vscale )
{
	inp1[0] = Scale16( (s16)((icode_a&0xC0) <<  8), vscale );
	inp1[1] = Scale16( (s16)((icode_a&0x30) << 10), vscale );
	inp1[2] = Scale16( (s16)((icode_a&0x0C) << 12), vscale );
	inp1[3] = Scale16( (s16)((icode_a&0x03) << 14), vscale );
	inp1[4] = Scale16( (s16)((icode_b&0xC0) <<  8), vscale );
	inp1[5] = Scale16( (s16)((icode_b&0x30) << 10), vscale );
	inp1[6] = Scale16( (s16)((icode_b&0x0C) << 12), vscale );
	inp1[7] = Scale16( (s16)((icode_b&0x03) << 14), vscale );
}

void Decode4( int (&inp1)[8], u32 icode_a, u32 icode_b )
{
	inp1[0] = (s16)((icode_a&0xC0) <<  8);
	inp1[1] = (s16)((icode_a&0x30) << 10);
	inp1[2] = (s16)((icode_a&0x0C) << 12);
	inp1[3] = (s16)((icode_a&0x03) << 14);
	inp1[4] = (s16)((icode_b&0xC0) <<  8);
	inp1[5] = (s16)((icode_b&0x30) << 10);
	inp1[6] = (s16)((icode_b&0x0C) << 12);
	inp1[7] = (s16)((icode_b&0x03) << 14);
}

void Decode8_Scale( int (&inp1)[8], u32 icode_a, u32 icode_b, u32 icode_c, u32 icode_d, int vscale )
{
	inp1[0] = Scale16( (s16)((icode_a&0xF0) <<  8), vscale );
	inp1[1] = Scale16( (s16)((icode_a&0x0F) << 12), vscale );
	inp1[2] = Scale16( (s16)((icode_b&0xF0) <<  8), vscale );
	inp1[3] = Scale16( (s16)((icode_b&0x0F) << 12), vscale );
	inp1[4] = Scale16( (s16)((icode_c&0xF0) <<  8), vscale );
	inp1[5] = Scale16( (s16)((icode_c&0x0F) << 12), vscale );
	inp1[6] = Scale16( (s16)((icode_d&0xF0) <<  8), vscale );
	inp1[7] = Scale16( (s16)((icode_d&0x0F) << 12), vscale );
}

void Decode8( int (&inp1)[8], u32 icode_a, u32 icode_b, u32 icode_c, u32 icode_d )
{
	inp1[0] = (s16)((icode_a&0xF0) <<  8);
	inp1[1] = (s16)((icode_a&0x0F) << 12);
	inp1[2] = (s16)((icode_b&0xF0) <<  8);
	inp1[3] = (s16)((icode_b&0x0F) << 12);
	inp1[4] = (s16)((icode_c&0xF0) <<  8);
	inp1[5] = (s16)((icode_c&0x0F) << 12);
	inp1[6] = (s16)((icode_d&0xF0) <<  8);
	inp1[7] = (s16)((icode_d&0x0F) << 12);
}

void ADPCM2_Decode4( int (&inp1)[8], int (&inp2)[8], u32 inPtr, u8 code )
{
	u32 icode_a=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+0)^3];
	u32 icode_b=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+1)^3];
	u32 icode_c=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+2)^3];
	u32 icode_d=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+3)^3];

	if( code < 0xE )
	{
		int vscale(0x8000>>((0xE - code)-1));
		Decode4_Scale( inp1, icode_a, icode_b, vscale );
		Decode4_Scale( inp2, icode_c, icode_d, vscale );
	}
	else
	{
		Decode4( inp1, icode_a, icode_b );
		Decode4( inp2, icode_c, icode_d );
	}
}

void ADPCM2_Decode8( int (&inp1)[8], int (&inp2)[8], u32 inPtr, u8 code )
{
	u32 icode_a=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+0)^3];
	u32 icode_b=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+1)^3];
	u32 icode_c=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+2)^3];
	u32 icode_d=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+3)^3];
	u32 icode_e=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+4)^3];
	u32 icode_f=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+5)^3];
	u32 icode_g=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+6)^3];
	u32 icode_h=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr+7)^3];

	if( code < 0xC )
	{
		int vscale(0x8000>>((0xC - code)-1));
		Decode8_Scale( inp1, icode_a, icode_b, icode_c, icode_d, vscale );
		Decode8_Scale( inp2, icode_e, icode_f, icode_g, icode_h, vscale );
	}
	else
	{
		Decode8( inp1, icode_a, icode_b, icode_c, icode_d );
		Decode8( inp2, icode_e, icode_f, icode_g, icode_h );
	}
}

void ADPCM2_Loop( s32 (&a)[8], int (&i1)[8], const s16 * b1, const s16 * b2, s16 * out )
{
	int l1( a[6] );
	int l2( a[7] );

	const int scl( 2048 );

	a[0] = ((int)b1[0]*l1) + ((int)b2[0]*l2) + ((int)i1[0]*scl);
	a[1] = ((int)b1[1]*l1) + ((int)b2[1]*l2) + ((int)b2[0]*i1[0]) + ((int)i1[1]*scl);
	a[2] = ((int)b1[2]*l1) + ((int)b2[2]*l2) + ((int)b2[1]*i1[0]) + ((int)b2[0]*i1[1]) + ((int)i1[2]*scl);
	a[3] = ((int)b1[3]*l1) + ((int)b2[3]*l2) + ((int)b2[2]*i1[0]) + ((int)b2[1]*i1[1]) + ((int)b2[0]*i1[2]) + ((int)i1[3]*scl);
	a[4] = ((int)b1[4]*l1) + ((int)b2[4]*l2) + ((int)b2[3]*i1[0]) + ((int)b2[2]*i1[1]) + ((int)b2[1]*i1[2]) + ((int)b2[0]*i1[3]) + ((int)i1[4]*scl);
	a[5] = ((int)b1[5]*l1) + ((int)b2[5]*l2) + ((int)b2[4]*i1[0]) + ((int)b2[3]*i1[1]) + ((int)b2[2]*i1[2]) + ((int)b2[1]*i1[3]) + ((int)b2[0]*i1[4]) + ((int)i1[5]*scl);
	a[6] = ((int)b1[6]*l1) + ((int)b2[6]*l2) + ((int)b2[5]*i1[0]) + ((int)b2[4]*i1[1]) + ((int)b2[3]*i1[2]) + ((int)b2[2]*i1[3]) + ((int)b2[1]*i1[4]) + ((int)b2[0]*i1[5]) + ((int)i1[6]*scl);
	a[7] = ((int)b1[7]*l1) + ((int)b2[7]*l2) + ((int)b2[6]*i1[0]) + ((int)b2[5]*i1[1]) + ((int)b2[4]*i1[2]) + ((int)b2[3]*i1[3]) + ((int)b2[2]*i1[4]) + ((int)b2[1]*i1[5]) + ((int)b2[0]*i1[6]) + ((int)i1[7]*scl);

	for(u32 j=0;j<8;j++)
	{
		s16 r = Saturate<s16>( a[j^1] >> 11 );
		a[j^1] = r;
		out[j]=r;		// XXXX endian issues
	}
}
void ADPCM2(AudioHLECommand command)
{

  // Verified to be 100% Accurate...
  	u8 Flags=(u8)((command.cmd0>>16)&0xff);
  	//u16 Gain=(u16)(command.cmd0&0xffff);	// XXXX Unused
  	u32 Address=(command.cmd1 & 0xffffff);// + gAudioHLEState.Segments[(command.cmd1>>24)&0xf];

  	bool init( (Flags&0x1) != 0 );
  	bool loop( (Flags&0x2) != 0 );
  	bool decode4( (Flags & 0x4) != 0 );		// 4 bytes -> 16 output samples

  	s16 * out( (s16 *)(gAudioHLEState.Buffer+gAudioHLEState.OutBuffer) );
  	if(init)
  	{
  		memset(out,0,32);
  	}
  	else
  	{
  		u32		src_addr( loop ? gAudioHLEState.LoopVal : Address );
  		memmove(out,&rdram[src_addr],32);		// XXXX Endian issues?
  	}

  	u16 inPtr=0;

  	s32 a[8] = { 0,0,0,0,0,0,out[15],out[14] };		// XXXX Endian issues - should be 14/15^TWIDDLE?

  	out+=16;
  	short count=gAudioHLEState.Count;
  	while(count>0)
  	{
  		u8 idx_code=gAudioHLEState.Buffer[(gAudioHLEState.InBuffer+inPtr)^3];
  		inPtr++;

  		u16 index((idx_code&0xf)<<4);
  		u8	code(idx_code>>=4);

  		s16 * book1=(s16 *)&gAudioHLEState.ADPCMTable[index];
  		s16 * book2=book1+8;

  		// Decode inputs
  		int inp1[8];
  		int inp2[8];

  		if( decode4 )
  		{
  			ADPCM2_Decode4( inp1, inp2, inPtr, code );
  			inPtr+=4;
  		}
  		else
  		{
  			ADPCM2_Decode8( inp1, inp2, inPtr, code );
  			inPtr+=8;
  		}

  		// Generate samples
  		ADPCM2_Loop( a, inp1, book1, book2, out );
  		ADPCM2_Loop( a, inp2, book1, book2, out+8 );

  		out += 16;
  		count-=32;
  	}
  	out-=16;
  	memmove(&rdram[Address],out,32);

}

static inline void ADPCM3_Predict8( s32 & l1, s32 & l2, const s32 * __restrict inp, const s16 * __restrict book1, const s16 * __restrict book2, s16 * __restrict out )
{
	const s32 b20 = book2[0], b21 = book2[1], b22 = book2[2], b23 = book2[3];
	const s32 b24 = book2[4], b25 = book2[5], b26 = book2[6], b27 = book2[7];
	const s32 i0 = inp[0], i1 = inp[1], i2 = inp[2], i3 = inp[3];
	const s32 i4 = inp[4], i5 = inp[5], i6 = inp[6], i7 = inp[7];

	const s32 a0 = book1[0]*l1 + b20*l2 + (i0 << 11);
	const s32 a1 = book1[1]*l1 + b21*l2 + b20*i0 + (i1 << 11);
	const s32 a2 = book1[2]*l1 + b22*l2 + b21*i0 + b20*i1 + (i2 << 11);
	const s32 a3 = book1[3]*l1 + b23*l2 + b22*i0 + b21*i1 + b20*i2 + (i3 << 11);
	const s32 a4 = book1[4]*l1 + b24*l2 + b23*i0 + b22*i1 + b21*i2 + b20*i3 + (i4 << 11);
	const s32 a5 = book1[5]*l1 + b25*l2 + b24*i0 + b23*i1 + b22*i2 + b21*i3 + b20*i4 + (i5 << 11);
	const s32 a6 = book1[6]*l1 + b26*l2 + b25*i0 + b24*i1 + b23*i2 + b22*i3 + b21*i4 + b20*i5 + (i6 << 11);
	const s32 a7 = book1[7]*l1 + b27*l2 + b26*i0 + b25*i1 + b24*i2 + b23*i3 + b22*i4 + b21*i5 + b20*i6 + (i7 << 11);

	out[0] = Saturate<s16>( a1 >> 11 );
	out[1] = Saturate<s16>( a0 >> 11 );
	out[2] = Saturate<s16>( a3 >> 11 );
	out[3] = Saturate<s16>( a2 >> 11 );
	out[4] = Saturate<s16>( a5 >> 11 );
	out[5] = Saturate<s16>( a4 >> 11 );
	const s16 s7 = Saturate<s16>( a7 >> 11 );
	const s16 s6 = Saturate<s16>( a6 >> 11 );
	out[6] = s7;
	out[7] = s6;
	l2 = s7;
	l1 = s6;
}

void ADPCM3(AudioHLECommand command)
{
	u8 Flags=(u8)(command.cmd1>>0x1c)&0xff;
	u32 Address=(command.cmd0 & 0xffffff);
	u32 inPtr=(command.cmd1>>12)&0xf;
	s16 *out=(s16 *)(gAudioHLEState.Buffer+(command.cmd1&0xfff)+0x4f0);
	s16 count=(s16)((command.cmd1 >> 16)&0xfff);
	const u8 * __restrict buf = gAudioHLEState.Buffer;

	if(Flags&0x1)
	{
		memset(out,0,32);
	}
	else
	{
		memmove(out,&rdram[(Flags&0x2) ? gAudioHLEState.LoopVal : Address],32);
	}

	s32 l1=out[15];
	s32 l2=out[14];
	s32 inp1[8];
	s32 inp2[8];
	out+=16;
	while(count>0)
	{
		const u8 hdr = buf[(0x4f0+inPtr)^3];
		inPtr++;
		const u16 index = (u16)((hdr & 0xf) << 4);
		const s16 * __restrict book1 = (const s16 *)&gAudioHLEState.ADPCMTable[index];
		const s16 * __restrict book2 = book1 + 8;
		const u8 code = hdr >> 4;

		const u32 b0 = buf[(0x4f0+inPtr+0)^3];
		const u32 b1 = buf[(0x4f0+inPtr+1)^3];
		const u32 b2 = buf[(0x4f0+inPtr+2)^3];
		const u32 b3 = buf[(0x4f0+inPtr+3)^3];
		const u32 b4 = buf[(0x4f0+inPtr+4)^3];
		const u32 b5 = buf[(0x4f0+inPtr+5)^3];
		const u32 b6 = buf[(0x4f0+inPtr+6)^3];
		const u32 b7 = buf[(0x4f0+inPtr+7)^3];
		inPtr += 8;

		if (code < 12)
		{
			const u32 rsh = 12u - code;
			inp1[0] = ((s32)(s16)((b0 & 0xf0u) << 8)) >> rsh;
			inp1[1] = ((s32)(s16)((b0 & 0x0fu) << 12)) >> rsh;
			inp1[2] = ((s32)(s16)((b1 & 0xf0u) << 8)) >> rsh;
			inp1[3] = ((s32)(s16)((b1 & 0x0fu) << 12)) >> rsh;
			inp1[4] = ((s32)(s16)((b2 & 0xf0u) << 8)) >> rsh;
			inp1[5] = ((s32)(s16)((b2 & 0x0fu) << 12)) >> rsh;
			inp1[6] = ((s32)(s16)((b3 & 0xf0u) << 8)) >> rsh;
			inp1[7] = ((s32)(s16)((b3 & 0x0fu) << 12)) >> rsh;

			inp2[0] = ((s32)(s16)((b4 & 0xf0u) << 8)) >> rsh;
			inp2[1] = ((s32)(s16)((b4 & 0x0fu) << 12)) >> rsh;
			inp2[2] = ((s32)(s16)((b5 & 0xf0u) << 8)) >> rsh;
			inp2[3] = ((s32)(s16)((b5 & 0x0fu) << 12)) >> rsh;
			inp2[4] = ((s32)(s16)((b6 & 0xf0u) << 8)) >> rsh;
			inp2[5] = ((s32)(s16)((b6 & 0x0fu) << 12)) >> rsh;
			inp2[6] = ((s32)(s16)((b7 & 0xf0u) << 8)) >> rsh;
			inp2[7] = ((s32)(s16)((b7 & 0x0fu) << 12)) >> rsh;
		}
		else
		{
			inp1[0] = (s16)((b0 & 0xf0u) << 8);
			inp1[1] = (s16)((b0 & 0x0fu) << 12);
			inp1[2] = (s16)((b1 & 0xf0u) << 8);
			inp1[3] = (s16)((b1 & 0x0fu) << 12);
			inp1[4] = (s16)((b2 & 0xf0u) << 8);
			inp1[5] = (s16)((b2 & 0x0fu) << 12);
			inp1[6] = (s16)((b3 & 0xf0u) << 8);
			inp1[7] = (s16)((b3 & 0x0fu) << 12);

			inp2[0] = (s16)((b4 & 0xf0u) << 8);
			inp2[1] = (s16)((b4 & 0x0fu) << 12);
			inp2[2] = (s16)((b5 & 0xf0u) << 8);
			inp2[3] = (s16)((b5 & 0x0fu) << 12);
			inp2[4] = (s16)((b6 & 0xf0u) << 8);
			inp2[5] = (s16)((b6 & 0x0fu) << 12);
			inp2[6] = (s16)((b7 & 0xf0u) << 8);
			inp2[7] = (s16)((b7 & 0x0fu) << 12);
		}

		ADPCM3_Predict8( l1, l2, inp1, book1, book2, out );
		ADPCM3_Predict8( l1, l2, inp2, book1, book2, out + 8 );
		out += 16;
		count -= 32;
	}
	out-=16;
	memmove(&rdram[Address],out,32);
}

void LOADADPCM(AudioHLECommand command)
{
  u32		address(command.Abi1LoadADPCM.Address );// + gAudioHLEState.Segments[(command.cmd1>>24)&0xf];
	u16		count( command.Abi1LoadADPCM.Count );

	gAudioHLEState.LoadADPCM( address, count );
}

void LOADADPCM2(AudioHLECommand command)
{
  // Loads an ADPCM table - Works 100% Now 03-13-01
	u32		address(command.Abi2LoadADPCM.Address );// + gAudioHLEState.Segments[(command.cmd1>>24)&0xf];
	u16		count( command.Abi2LoadADPCM.Count );

	gAudioHLEState.LoadADPCM( address, count );
}

void LOADADPCM3(AudioHLECommand command)
{
  u32		address(command.Abi3LoadADPCM.Address );// + gAudioHLEState.Segments[(command.cmd1>>24)&0xf];
	u16		count( command.Abi3LoadADPCM.Count );

	gAudioHLEState.LoadADPCM( address, count );
}
