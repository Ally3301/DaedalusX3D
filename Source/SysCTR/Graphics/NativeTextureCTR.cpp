/*
Copyright (C) 2005-2007 StrmnNrmn

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
#include "Graphics/NativeTexture.h"
#include "Graphics/NativePixelFormat.h"
#include "Graphics/ColourValue.h"
#include "SysCTR/Graphics/Citro3DContext.h"
#include "Utility/FastMemcpy.h"

#include "Math/MathUtil.h"

#include <png.h>
#include <cstring>
#include <new>
#include <stdio.h>
#include <3ds.h>
#include <citro3d.h>

namespace
{
	// 8x8 Morton Z-order lookup table for PICA200 tiled textures.
	static const u8 kMortonLut[64] = {
		 0,  1,  4,  5, 16, 17, 20, 21,
		 2,  3,  6,  7, 18, 19, 22, 23,
		 8,  9, 12, 13, 24, 25, 28, 29,
		10, 11, 14, 15, 26, 27, 30, 31,
		32, 33, 36, 37, 48, 49, 52, 53,
		34, 35, 38, 39, 50, 51, 54, 55,
		40, 41, 44, 45, 56, 57, 60, 61,
		42, 43, 46, 47, 58, 59, 62, 63,
	};

	static inline u16 Convert5650ToPica(u16 p)
	{
		// NativePf5650: B[15:11] G[10:5] R[4:0] -> PICA GPU_RGB565: R[15:11] G[10:5] B[4:0]
		return static_cast<u16>(((p & 0x001Fu) << 11) | (p & 0x07E0u) | ((p & 0xF800u) >> 11));
	}

	static inline u16 Convert5551ToPica(u16 p)
	{
		// NativePf5551: A[15] B[14:10] G[9:5] R[4:0] -> PICA GPU_RGBA5551: R[15:11] G[10:6] B[5:1] A[0]
		return static_cast<u16>(((p & 0x001Fu) << 11) | ((p & 0x03E0u) << 1) | ((p & 0x7C00u) >> 9) | ((p & 0x8000u) >> 15));
	}

	static inline u16 Convert4444ToPica(u16 p)
	{
		// NativePf4444: A[15:12] B[11:8] G[7:4] R[3:0] -> PICA GPU_RGBA4: R[15:12] G[11:8] B[7:4] A[3:0]
		return static_cast<u16>(((p & 0x000Fu) << 12) | ((p & 0x00F0u) << 4) | ((p & 0x0F00u) >> 4) | ((p & 0xF000u) >> 12));
	}

	static inline u32 Convert8888ToPica(u32 p)
	{
		// NativePf8888: A[31:24] B[23:16] G[15:8] R[7:0] -> PICA GPU_RGBA8: R[31:24] G[23:16] B[15:8] A[7:0]
		return __builtin_bswap32(p);
	}

	static inline void FlushTextureData(C3D_Tex * tex)
	{
		if (tex && tex->data && !((u32)tex->data >= OS_VRAM_VADDR && (u32)tex->data < OS_VRAM_VADDR + OS_VRAM_SIZE))
		{
			Citro3D::FlushDataCache(tex->data, C3D_TexCalcTotalSize(tex->size, tex->maxLevel));
		}
	}

	static inline GPU_TEXCOLOR SelectPicaFormat(ETextureFormat fmt)
	{
		switch (fmt)
		{
		case TexFmt_5650: return GPU_RGB565;
		case TexFmt_5551: return GPU_RGBA5551;
		case TexFmt_4444: return GPU_RGBA4;
		case TexFmt_8888:
		case TexFmt_CI4_8888:
		case TexFmt_CI8_8888:
		default:
			return GPU_RGBA8;
		}
	}

	static void LogTextureAllocationFailure(const char *reason, u32 width, u32 height, size_t bytes)
	{
		static u32 loggedFailures = 0;
		if (loggedFailures >= 8)
			return;
		++loggedFailures;

		FILE *log = fopen("sdmc:/DaedalusX64.log", "a");
		if (log != nullptr)
		{
			fprintf(log, "texture: %s %lux%lu (%lu bytes)\n",
				reason, static_cast<unsigned long>(width),
				static_cast<unsigned long>(height), static_cast<unsigned long>(bytes));
			fclose(log);
		}
	}
}

static u32 GetTextureBlockWidth( u32 dimension, ETextureFormat texture_format )
{
	DAEDALUS_ASSERT( GetNextPowerOf2( dimension ) == dimension, "This is not a power of 2" );

	// Ensure that the pitch is at least 16 bytes
	while( CalcBytesRequired( dimension, texture_format ) < 16 )
	{
		dimension *= 2;
	}

	return dimension;
}

static inline u32 CorrectDimension( u32 dimension )
{
	static const u32 MIN_TEXTURE_DIMENSION = 8;
	static const u32 MAX_TEXTURE_DIMENSION = 1024;
	u32 d = Max( GetNextPowerOf2( dimension ), MIN_TEXTURE_DIMENSION );
	return d > MAX_TEXTURE_DIMENSION ? MAX_TEXTURE_DIMENSION : d;
}

CRefPtr<CNativeTexture>	CNativeTexture::Create( u32 width, u32 height, ETextureFormat texture_format )
{
	if (width == 0 || height == 0 || width > 1024 || height > 1024)
	{
		LogTextureAllocationFailure("invalid dimensions", width, height, 0);
		return nullptr;
	}

	CRefPtr<CNativeTexture> texture =
		new (std::nothrow) CNativeTexture( width, height, texture_format );
	if (texture == nullptr || !texture->HasData())
		return nullptr;
	return texture;
}

CNativeTexture::CNativeTexture( u32 w, u32 h, ETextureFormat texture_format )
:	mTextureFormat( texture_format )
,	mWidth( w )
,	mHeight( h )
,	mCorrectedWidth( CorrectDimension( w ) )
,	mCorrectedHeight( CorrectDimension( h ) )
,	mTextureBlockWidth( GetTextureBlockWidth( mCorrectedWidth, texture_format ) )
,	mpData( NULL )
,	mpPalette( NULL )
,	mHasTex( false )
,	mOnVram( false )
,	mDynamic( false )
,	mQueuedForVram( false )
,	mBindFrames( 0 )
,	mLastBindFrame( 0 )
,	mCachedFilter( -1 )
,	mCachedWrapS( -1 )
,	mCachedWrapT( -1 )
{
	std::memset(&mTex, 0, sizeof(mTex));
	mScale.x = 1.0f / (float)mCorrectedWidth;
	mScale.y = 1.0f / (float)mCorrectedHeight;

	const GPU_TEXCOLOR picaFmt = SelectPicaFormat(texture_format);

	// Allocate textures in linear FCRAM (C3D_TexInit), matching sm64_3ds and ctr64:
	// - CPU swizzles Morton tiles directly into mTex.data in a single pass + C3D_TexFlush
	//   (zero staging copies, zero mid-frame C3D_FrameSplit stalls, and never overflows
	//   citro3d's 32-entry stopped gxQueue during heavy N64 scene loads).
	if (C3D_TexInit(&mTex, static_cast<u16>(mCorrectedWidth), static_cast<u16>(mCorrectedHeight), picaFmt))
	{
		mHasTex = true;
		mOnVram = false;
	}
	else
	{
		LogTextureAllocationFailure("C3D_TexInit failed", mCorrectedWidth, mCorrectedHeight, GetBytesRequired());
		return;
	}

	C3D_TexSetFilter(&mTex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&mTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	mCachedFilter = GPU_LINEAR;
	mCachedWrapS = GPU_CLAMP_TO_EDGE;
	mCachedWrapT = GPU_CLAMP_TO_EDGE;
}

CNativeTexture::~CNativeTexture()
{
	if (mQueuedForVram)
	{
		Citro3D::UnregisterTextureVramPromotion(this);
		mQueuedForVram = false;
	}
	if (mpData)
		free(mpData);
	if (mpPalette)
		free(mpPalette);

	if (mHasTex)
	{
		if (mTex.data != nullptr && Citro3D::IsInitialised() && !Citro3D::IsExiting())
		{
			// Defer freeing until the next C3D_FrameBegin waits for the in-flight GPU frame,
			// so PICA200 never samples a freed VRAM/FCRAM texture buffer on real 3DS hardware.
			Citro3D::DeferTexDataFree(mTex.data);
			mTex.data = nullptr;
		}
		else
		{
			C3D_TexDelete(&mTex);
		}
		mHasTex = false;
	}
}

bool CNativeTexture::HasData() const
{
	return mHasTex;
}

void CNativeTexture::InstallTexture() const
{
	if (mHasTex)
	{
		NoteBoundForVram();
		Citro3D::BindTexture(&mTex);
	}
}

void CNativeTexture::NoteBoundForVram() const
{
	if (!mHasTex || mOnVram || mDynamic || mQueuedForVram)
		return;
	const u32 frame = Citro3D::GetFrameCounter();
	if (frame == 0 || frame == mLastBindFrame)
		return;
	mLastBindFrame = frame;
	if (++mBindFrames >= 2)
	{
		// Promote static/reused textures (up to 128 KiB each) into the 3DS's dedicated
		// 6 MiB on-chip VRAM so PICA200 texture fetches stop competing with ARM11 for FCRAM.
		const u32 bytes = C3D_TexCalcTotalSize(mTex.size, mTex.maxLevel);
		if (bytes >= 16u && bytes <= 131072u && (bytes & 15u) == 0)
		{
			mQueuedForVram = true;
			Citro3D::QueueTextureVramPromotion(const_cast<CNativeTexture *>(this));
		}
	}
}

bool CNativeTexture::TryPromoteToVram()
{
	mQueuedForVram = false;
	if (!mHasTex || mOnVram || mDynamic || mTex.data == nullptr)
		return false;

	const u32 bytes = C3D_TexCalcTotalSize(mTex.size, mTex.maxLevel);
	// Always keep 512 KiB of VRAM headroom free for render targets and system stability.
	if (bytes < 16u || (bytes & 15u) != 0 || vramSpaceFree() < bytes + 512u * 1024u)
		return false;

	void * vramPtr = vramAlloc(bytes);
	if (vramPtr == nullptr)
		return false;

	void * oldLinear = mTex.data;
	// Called right after C3D_FrameBegin(0) before any draws (cmdBufUsage == 0):
	// C3D_SyncTextureCopy emits 1 async GX_TextureCopy DMA with zero C3D_FrameSplit!
	C3D_SyncTextureCopy(static_cast<u32 *>(oldLinear), 0, static_cast<u32 *>(vramPtr), 0, bytes, 8);
	mTex.data = vramPtr;
	mOnVram = true;
	Citro3D::DeferTexDataFree(oldLinear);
	return true;
}

bool CNativeTexture::EnsureLinearForCpuWrite()
{
	if (!mHasTex || mTex.data == nullptr)
		return false;
	if (mQueuedForVram)
	{
		Citro3D::UnregisterTextureVramPromotion(this);
		mQueuedForVram = false;
	}
	if (mOnVram)
	{
		// Texture contents changed dynamically: move back to linear FCRAM and mark
		// dynamic so it never ping-pongs with VRAM again.
		mDynamic = true;
		const u32 bytes = C3D_TexCalcTotalSize(mTex.size, mTex.maxLevel);
		void * newLinear = linearAlloc(bytes);
		if (newLinear == nullptr)
			return false;
		Citro3D::DeferTexDataFree(mTex.data);
		mTex.data = newLinear;
		mOnVram = false;
	}
	return true;
}

void CNativeTexture::ApplyParams( GPU_TEXTURE_FILTER_PARAM filter, GPU_TEXTURE_WRAP_PARAM wrap_s, GPU_TEXTURE_WRAP_PARAM wrap_t ) const
{
	if (!mHasTex)
		return;

	const bool filterChanged = (mCachedFilter != (int)filter);
	const bool wrapChanged   = (mCachedWrapS != (int)wrap_s || mCachedWrapT != (int)wrap_t);
	if (filterChanged || wrapChanged)
	{
		Citro3D::FlushPendingBatch();
		if (filterChanged)
		{
			C3D_TexSetFilter(&mTex, filter, filter);
			mCachedFilter = (int)filter;
		}
		if (wrapChanged)
		{
			C3D_TexSetWrap(&mTex, wrap_s, wrap_t);
			mCachedWrapS = (int)wrap_s;
			mCachedWrapT = (int)wrap_t;
		}
	}
}

namespace
{
	template< typename T >
	void ReadPngData( u32 width, u32 height, u32 stride, u8 ** p_row_table, int color_type, T * p_dest )
	{
		u8 r=0, g=0, b=0, a=0;

		for ( u32 y = 0; y < height; ++y )
		{
			const u8 * pRow = p_row_table[ y ];

			T * p_dest_row( p_dest );

			for ( u32 x = 0; x < width; ++x )
			{
				switch ( color_type )
				{
				case PNG_COLOR_TYPE_GRAY:
					r = g = b = *pRow++;
					if ( r == 0 && g == 0 && b == 0 )	a = 0x00;
					else								a = 0xff;
					break;
				case PNG_COLOR_TYPE_GRAY_ALPHA:
					r = g = b = *pRow++;
					if ( r == 0 && g == 0 && b == 0 )	a = 0x00;
					else								a = 0xff;
					pRow++;
					break;
				case PNG_COLOR_TYPE_RGB:
					b = *pRow++;
					g = *pRow++;
					r = *pRow++;
					if ( r == 0 && g == 0 && b == 0 )	a = 0x00;
					else								a = 0xff;
					break;
				case PNG_COLOR_TYPE_RGB_ALPHA:
					b = *pRow++;
					g = *pRow++;
					r = *pRow++;
					a = *pRow++;
					break;
				}

				p_dest_row[ x ] = T( r, g, b, a );
			}

			p_dest = reinterpret_cast< T * >( reinterpret_cast< u8 * >( p_dest ) + stride );
		}
	}

	CRefPtr<CNativeTexture>	LoadPng( const char * p_filename, ETextureFormat texture_format )
	{
		const size_t	SIGNATURE_SIZE = 8;
		u8	signature[ SIGNATURE_SIZE ];

		FILE * fh = fopen( p_filename,"rb" );
		if (fh == NULL)
			return NULL;

		if (fread( signature, sizeof(u8), SIGNATURE_SIZE, fh ) != SIGNATURE_SIZE)
		{
			fclose(fh);
			return NULL;
		}

		if (!png_check_sig( signature, SIGNATURE_SIZE ))
		{
			fclose(fh);
			return NULL;
		}

		png_struct * p_png_struct = png_create_read_struct( PNG_LIBPNG_VER_STRING, NULL, NULL, NULL );
		if (p_png_struct == NULL)
		{
			fclose(fh);
			return NULL;
		}

		png_info * p_png_info = png_create_info_struct( p_png_struct );
		if (p_png_info == NULL)
		{
			png_destroy_read_struct( &p_png_struct, NULL, NULL );
			fclose(fh);
			return NULL;
		}

		if (setjmp( png_jmpbuf(p_png_struct) ) != 0)
		{
			png_destroy_read_struct( &p_png_struct, &p_png_info, NULL );
			fclose(fh);
			return NULL;
		}

		png_init_io( p_png_struct, fh );
		png_set_sig_bytes( p_png_struct, SIGNATURE_SIZE );
		png_read_png( p_png_struct, p_png_info, PNG_TRANSFORM_STRIP_16 | PNG_TRANSFORM_PACKING | PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_BGR, NULL );

		png_uint_32 width  = png_get_image_width( p_png_struct, p_png_info );
		png_uint_32 height = png_get_image_height( p_png_struct, p_png_info );

		CRefPtr<CNativeTexture>	texture = CNativeTexture::Create( width, height, texture_format );
		if (texture == nullptr)
		{
			png_destroy_read_struct( &p_png_struct, &p_png_info, NULL );
			fclose(fh);
			return nullptr;
		}

		u8 * buffer = new (std::nothrow) u8[ texture->GetBytesRequired() ];
		if( !buffer )
		{
			texture = NULL;
		}
		else
		{
			std::memset( buffer, 0, texture->GetBytesRequired() );
			u32 	stride       = texture->GetStride();
			u8 ** 	row_pointers = png_get_rows( p_png_struct, p_png_info );
			int 	color_type   = png_get_color_type( p_png_struct, p_png_info );

			switch( texture_format )
			{
			case TexFmt_5650:
				ReadPngData< NativePf5650 >( width, height, stride, row_pointers, color_type, reinterpret_cast< NativePf5650 * >( buffer ) );
				break;
			case TexFmt_5551:
				ReadPngData< NativePf5551 >( width, height, stride, row_pointers, color_type, reinterpret_cast< NativePf5551 * >( buffer ) );
				break;
			case TexFmt_4444:
				ReadPngData< NativePf4444 >( width, height, stride, row_pointers, color_type, reinterpret_cast< NativePf4444 * >( buffer ) );
				break;
			case TexFmt_8888:
				ReadPngData< NativePf8888 >( width, height, stride, row_pointers, color_type, reinterpret_cast< NativePf8888 * >( buffer ) );
				break;
			default:
				DAEDALUS_ERROR( "Unhandled texture format" );
				break;
			}

			texture->SetData( buffer, NULL );
		}

		delete [] buffer;
		png_destroy_read_struct( &p_png_struct, &p_png_info, NULL );
		fclose(fh);

		return texture;
	}
}

CRefPtr<CNativeTexture>	CNativeTexture::CreateFromPng( const char * p_filename, ETextureFormat texture_format )
{
	return LoadPng( p_filename, texture_format );
}

void CNativeTexture::SetData( void * data, void * palette )
{
	if (!mHasTex || data == nullptr)
		return;
	if ((mTextureFormat == TexFmt_CI4_8888 || mTextureFormat == TexFmt_CI8_8888) &&
		palette == nullptr)
		return;
	if (!EnsureLinearForCpuWrite())
		return;

	const u32 w = mCorrectedWidth;
	const u32 h = mCorrectedHeight;
	const u32 tilesX = w >> 3;
	const u32 tilesY = h >> 3;
	const u32 srcStrideBytes = GetStride();
	void * const staging = mTex.data;

	// PICA200 texture convention (matching picaGL & ctr64):
	// UV v=0.0 samples tiled row (h - 1), and UV v=1.0 samples tiled row 0.
	// Therefore, tiled row (baseY + ly) reads source row y = (h - 1) - (baseY + ly).
	switch (mTextureFormat)
	{
	case TexFmt_5650:
	{
		u16 * dst = static_cast<u16 *>(staging);
		const u8 * srcBytes = static_cast<const u8 *>(data);
		for (u32 ty = 0; ty < tilesY; ++ty)
		{
			const u32 baseY = ty << 3;
			for (u32 tx = 0; tx < tilesX; ++tx)
			{
				const u32 baseX = tx << 3;
				u16 * tileDst = dst + ((ty * tilesX + tx) << 6);
				for (u32 ly = 0; ly < 8; ++ly)
				{
					const u32 srcY = (h - 1) - (baseY + ly);
					const u16 * row = reinterpret_cast<const u16 *>(srcBytes + srcY * srcStrideBytes) + baseX;
					const u8 * lut = &kMortonLut[ly << 3];
					tileDst[lut[0]] = Convert5650ToPica(row[0]);
					tileDst[lut[1]] = Convert5650ToPica(row[1]);
					tileDst[lut[2]] = Convert5650ToPica(row[2]);
					tileDst[lut[3]] = Convert5650ToPica(row[3]);
					tileDst[lut[4]] = Convert5650ToPica(row[4]);
					tileDst[lut[5]] = Convert5650ToPica(row[5]);
					tileDst[lut[6]] = Convert5650ToPica(row[6]);
					tileDst[lut[7]] = Convert5650ToPica(row[7]);
				}
			}
		}
		break;
	}

	case TexFmt_5551:
	{
		u16 * dst = static_cast<u16 *>(staging);
		const u8 * srcBytes = static_cast<const u8 *>(data);
		for (u32 ty = 0; ty < tilesY; ++ty)
		{
			const u32 baseY = ty << 3;
			for (u32 tx = 0; tx < tilesX; ++tx)
			{
				const u32 baseX = tx << 3;
				u16 * tileDst = dst + ((ty * tilesX + tx) << 6);
				for (u32 ly = 0; ly < 8; ++ly)
				{
					const u32 srcY = (h - 1) - (baseY + ly);
					const u16 * row = reinterpret_cast<const u16 *>(srcBytes + srcY * srcStrideBytes) + baseX;
					const u8 * lut = &kMortonLut[ly << 3];
					tileDst[lut[0]] = Convert5551ToPica(row[0]);
					tileDst[lut[1]] = Convert5551ToPica(row[1]);
					tileDst[lut[2]] = Convert5551ToPica(row[2]);
					tileDst[lut[3]] = Convert5551ToPica(row[3]);
					tileDst[lut[4]] = Convert5551ToPica(row[4]);
					tileDst[lut[5]] = Convert5551ToPica(row[5]);
					tileDst[lut[6]] = Convert5551ToPica(row[6]);
					tileDst[lut[7]] = Convert5551ToPica(row[7]);
				}
			}
		}
		break;
	}

	case TexFmt_4444:
	{
		u16 * dst = static_cast<u16 *>(staging);
		const u8 * srcBytes = static_cast<const u8 *>(data);
		for (u32 ty = 0; ty < tilesY; ++ty)
		{
			const u32 baseY = ty << 3;
			for (u32 tx = 0; tx < tilesX; ++tx)
			{
				const u32 baseX = tx << 3;
				u16 * tileDst = dst + ((ty * tilesX + tx) << 6);
				for (u32 ly = 0; ly < 8; ++ly)
				{
					const u32 srcY = (h - 1) - (baseY + ly);
					const u16 * row = reinterpret_cast<const u16 *>(srcBytes + srcY * srcStrideBytes) + baseX;
					const u8 * lut = &kMortonLut[ly << 3];
					tileDst[lut[0]] = Convert4444ToPica(row[0]);
					tileDst[lut[1]] = Convert4444ToPica(row[1]);
					tileDst[lut[2]] = Convert4444ToPica(row[2]);
					tileDst[lut[3]] = Convert4444ToPica(row[3]);
					tileDst[lut[4]] = Convert4444ToPica(row[4]);
					tileDst[lut[5]] = Convert4444ToPica(row[5]);
					tileDst[lut[6]] = Convert4444ToPica(row[6]);
					tileDst[lut[7]] = Convert4444ToPica(row[7]);
				}
			}
		}
		break;
	}

	case TexFmt_8888:
	{
		u32 * dst = static_cast<u32 *>(staging);
		const u8 * srcBytes = static_cast<const u8 *>(data);
		for (u32 ty = 0; ty < tilesY; ++ty)
		{
			const u32 baseY = ty << 3;
			for (u32 tx = 0; tx < tilesX; ++tx)
			{
				const u32 baseX = tx << 3;
				u32 * tileDst = dst + ((ty * tilesX + tx) << 6);
				for (u32 ly = 0; ly < 8; ++ly)
				{
					const u32 srcY = (h - 1) - (baseY + ly);
					const u32 * row = reinterpret_cast<const u32 *>(srcBytes + srcY * srcStrideBytes) + baseX;
					const u8 * lut = &kMortonLut[ly << 3];
					tileDst[lut[0]] = Convert8888ToPica(row[0]);
					tileDst[lut[1]] = Convert8888ToPica(row[1]);
					tileDst[lut[2]] = Convert8888ToPica(row[2]);
					tileDst[lut[3]] = Convert8888ToPica(row[3]);
					tileDst[lut[4]] = Convert8888ToPica(row[4]);
					tileDst[lut[5]] = Convert8888ToPica(row[5]);
					tileDst[lut[6]] = Convert8888ToPica(row[6]);
					tileDst[lut[7]] = Convert8888ToPica(row[7]);
				}
			}
		}
		break;
	}

	case TexFmt_CI4_8888:
	{
		u32 picaPal[16];
		const NativePf8888 * pal = static_cast<const NativePf8888 *>(palette);
		for (u32 i = 0; i < 16; ++i)
			picaPal[i] = Convert8888ToPica(pal[i].Bits);

		u32 * dst = static_cast<u32 *>(staging);
		const u8 * srcBytes = static_cast<const u8 *>(data);
		for (u32 ty = 0; ty < tilesY; ++ty)
		{
			const u32 baseY = ty << 3;
			for (u32 tx = 0; tx < tilesX; ++tx)
			{
				const u32 baseX = tx << 3;
				u32 * tileDst = dst + ((ty * tilesX + tx) << 6);
				for (u32 ly = 0; ly < 8; ++ly)
				{
					const u32 srcY = (h - 1) - (baseY + ly);
					const NativePfCI44 * row = reinterpret_cast<const NativePfCI44 *>(srcBytes + srcY * srcStrideBytes) + (baseX >> 1);
					const u8 * lut = &kMortonLut[ly << 3];
					for (u32 px = 0; px < 4; ++px)
					{
						const NativePfCI44 pair = row[px];
						tileDst[lut[px * 2 + 0]] = picaPal[pair.GetIdxB()];
						tileDst[lut[px * 2 + 1]] = picaPal[pair.GetIdxA()];
					}
				}
			}
		}
		break;
	}

	case TexFmt_CI8_8888:
	{
		u32 picaPal[256];
		const NativePf8888 * pal = static_cast<const NativePf8888 *>(palette);
		for (u32 i = 0; i < 256; ++i)
			picaPal[i] = Convert8888ToPica(pal[i].Bits);

		u32 * dst = static_cast<u32 *>(staging);
		const u8 * srcBytes = static_cast<const u8 *>(data);
		for (u32 ty = 0; ty < tilesY; ++ty)
		{
			const u32 baseY = ty << 3;
			for (u32 tx = 0; tx < tilesX; ++tx)
			{
				const u32 baseX = tx << 3;
				u32 * tileDst = dst + ((ty * tilesX + tx) << 6);
				for (u32 ly = 0; ly < 8; ++ly)
				{
					const u32 srcY = (h - 1) - (baseY + ly);
					const u8 * row = srcBytes + srcY * srcStrideBytes + baseX;
					const u8 * lut = &kMortonLut[ly << 3];
					tileDst[lut[0]] = picaPal[row[0]];
					tileDst[lut[1]] = picaPal[row[1]];
					tileDst[lut[2]] = picaPal[row[2]];
					tileDst[lut[3]] = picaPal[row[3]];
					tileDst[lut[4]] = picaPal[row[4]];
					tileDst[lut[5]] = picaPal[row[5]];
					tileDst[lut[6]] = picaPal[row[6]];
					tileDst[lut[7]] = picaPal[row[7]];
				}
			}
		}
		break;
	}

	default:
		break;
	}

	FlushTextureData(&mTex);
}

void CNativeTexture::SetDataRaw5551( const u16 * pixels, u32 src_w, u32 src_h )
{
	if (!mHasTex || pixels == nullptr || mTex.fmt != GPU_RGBA5551 || !EnsureLinearForCpuWrite())
		return;

	const u32 w = mCorrectedWidth;
	const u32 h = mCorrectedHeight;
	const u32 tilesX = w >> 3;
	const u32 tilesY = h >> 3;

	u16 * dst = static_cast<u16 *>(mTex.data);
	for (u32 ty = 0; ty < tilesY; ++ty)
	{
		const u32 baseY = ty << 3;
		for (u32 tx = 0; tx < tilesX; ++tx)
		{
			const u32 baseX = tx << 3;
			u16 * tileDst = dst + ((ty * tilesX + tx) << 6);
			for (u32 ly = 0; ly < 8; ++ly)
			{
				const u32 y = (h - 1) - (baseY + ly);
				const u8 * lut = &kMortonLut[ly << 3];
				if (y < src_h)
				{
					const u16 * row = pixels + y * src_w;
					for (u32 lx = 0; lx < 8; ++lx)
					{
						const u32 x = baseX + lx;
						tileDst[lut[lx]] = (x < src_w) ? row[x] : 0;
					}
				}
				else
				{
					for (u32 lx = 0; lx < 8; ++lx)
						tileDst[lut[lx]] = 0;
				}
			}
		}
	}

	FlushTextureData(&mTex);
}

bool CNativeTexture::UploadDirectN64RGBA16( const u8 * rdram, u32 load_addr, u32 pitch, bool swapped )
{
	if (!mHasTex || rdram == nullptr || mTex.fmt != GPU_RGBA5551 || mWidth == 0 || mHeight == 0 || !EnsureLinearForCpuWrite())
		return false;

	const u32 w = mCorrectedWidth;
	const u32 h = mCorrectedHeight;
	const u32 tilesX = w >> 3;
	const u32 tilesY = h >> 3;
	const u32 maxSrcY = mHeight - 1;

	// N64 RGBA16 (RRRRRGGGGGBBBBBA) is bit-exact with PICA200 GPU_RGBA5551:
	// read directly from RDRAM straight into 8x8 Morton tiles in linear FCRAM
	// with PICA200 vertical orientation (v=0 -> tiled row h-1).
	u16 * dst = static_cast<u16 *>(mTex.data);
	for (u32 ty = 0; ty < tilesY; ++ty)
	{
		const u32 baseY = ty << 3;
		for (u32 tx = 0; tx < tilesX; ++tx)
		{
			const u32 baseX = tx << 3;
			u16 * tileDst = dst + ((ty * tilesX + tx) << 6);
			for (u32 ly = 0; ly < 8; ++ly)
			{
				u32 y = (h - 1) - (baseY + ly);
				if (y > maxSrcY)
					y = maxSrcY;
				const u32 rowAddr = load_addr + y * pitch + (baseX << 1);
				if (y > 0)
					__builtin_prefetch(rdram + ((load_addr + (y - 1) * pitch + (baseX << 1)) & ~31u), 0, 0);
				const u8 * lut = &kMortonLut[ly << 3];
				if (swapped && (y & 1u))
				{
					tileDst[lut[2]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr +  0u) ^ 2u));
					tileDst[lut[3]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr +  2u) ^ 2u));
					tileDst[lut[0]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr +  4u) ^ 2u));
					tileDst[lut[1]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr +  6u) ^ 2u));
					tileDst[lut[6]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr +  8u) ^ 2u));
					tileDst[lut[7]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr + 10u) ^ 2u));
					tileDst[lut[4]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr + 12u) ^ 2u));
					tileDst[lut[5]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr + 14u) ^ 2u));
				}
				else
				{
					tileDst[lut[0]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr +  0u) ^ 2u));
					tileDst[lut[1]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr +  2u) ^ 2u));
					tileDst[lut[2]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr +  4u) ^ 2u));
					tileDst[lut[3]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr +  6u) ^ 2u));
					tileDst[lut[4]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr +  8u) ^ 2u));
					tileDst[lut[5]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr + 10u) ^ 2u));
					tileDst[lut[6]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr + 12u) ^ 2u));
					tileDst[lut[7]] = *reinterpret_cast<const u16 *>(rdram + ((rowAddr + 14u) ^ 2u));
				}
			}
		}
	}

	FlushTextureData(&mTex);
	return true;
}

bool CNativeTexture::UploadDirectN64CI( const u8 * rdram, u32 load_addr, u32 pitch, u32 tlut_addr, bool is_ci8, bool swapped )
{
	if (!mHasTex || rdram == nullptr || mTex.fmt != GPU_RGBA5551 || tlut_addr < 0x1000u || mWidth == 0 || mHeight == 0 || !EnsureLinearForCpuWrite())
		return false;

	const u32 w = mCorrectedWidth;
	const u32 h = mCorrectedHeight;
	const u32 tilesX = w >> 3;
	const u32 tilesY = h >> 3;
	const u32 maxSrcY = mHeight - 1;

	// Build direct PICA200 GPU_RGBA5551 palette (bit-exact with N64 RGBA16 TLUT).
	u16 pal[256];
	const u16 * srcPal = reinterpret_cast<const u16 *>(rdram + tlut_addr);
	const u32 palCount = is_ci8 ? 256u : 16u;
	for (u32 i = 0; i < palCount; ++i)
		pal[i] = srcPal[i ^ 1u];

	u16 * dst = static_cast<u16 *>(mTex.data);
	if (is_ci8)
	{
		for (u32 ty = 0; ty < tilesY; ++ty)
		{
			const u32 baseY = ty << 3;
			for (u32 tx = 0; tx < tilesX; ++tx)
			{
				const u32 baseX = tx << 3;
				u16 * tileDst = dst + ((ty * tilesX + tx) << 6);
				for (u32 ly = 0; ly < 8; ++ly)
				{
					u32 y = (h - 1) - (baseY + ly);
					if (y > maxSrcY)
						y = maxSrcY;
					const u32 f = (swapped && (y & 1u)) ? 7u : 3u;
					const u32 rowAddr = load_addr + y * pitch + baseX;
					if (y > 0)
						__builtin_prefetch(rdram + ((load_addr + (y - 1) * pitch + baseX) & ~31u), 0, 0);
					const u8 * lut = &kMortonLut[ly << 3];
					tileDst[lut[0]] = pal[rdram[(rowAddr + 0u) ^ f]];
					tileDst[lut[1]] = pal[rdram[(rowAddr + 1u) ^ f]];
					tileDst[lut[2]] = pal[rdram[(rowAddr + 2u) ^ f]];
					tileDst[lut[3]] = pal[rdram[(rowAddr + 3u) ^ f]];
					tileDst[lut[4]] = pal[rdram[(rowAddr + 4u) ^ f]];
					tileDst[lut[5]] = pal[rdram[(rowAddr + 5u) ^ f]];
					tileDst[lut[6]] = pal[rdram[(rowAddr + 6u) ^ f]];
					tileDst[lut[7]] = pal[rdram[(rowAddr + 7u) ^ f]];
				}
			}
		}
	}
	else
	{
		for (u32 ty = 0; ty < tilesY; ++ty)
		{
			const u32 baseY = ty << 3;
			for (u32 tx = 0; tx < tilesX; ++tx)
			{
				const u32 baseX = tx << 3;
				u16 * tileDst = dst + ((ty * tilesX + tx) << 6);
				for (u32 ly = 0; ly < 8; ++ly)
				{
					u32 y = (h - 1) - (baseY + ly);
					if (y > maxSrcY)
						y = maxSrcY;
					const u32 f = (swapped && (y & 1u)) ? 7u : 3u;
					const u32 rowAddr = load_addr + y * pitch + (baseX >> 1);
					const u8 * lut = &kMortonLut[ly << 3];
					const u8 b0 = rdram[(rowAddr + 0u) ^ f];
					const u8 b1 = rdram[(rowAddr + 1u) ^ f];
					const u8 b2 = rdram[(rowAddr + 2u) ^ f];
					const u8 b3 = rdram[(rowAddr + 3u) ^ f];
					tileDst[lut[0]] = pal[(b0 >> 4) & 0x0Fu];
					tileDst[lut[1]] = pal[b0 & 0x0Fu];
					tileDst[lut[2]] = pal[(b1 >> 4) & 0x0Fu];
					tileDst[lut[3]] = pal[b1 & 0x0Fu];
					tileDst[lut[4]] = pal[(b2 >> 4) & 0x0Fu];
					tileDst[lut[5]] = pal[b2 & 0x0Fu];
					tileDst[lut[6]] = pal[(b3 >> 4) & 0x0Fu];
					tileDst[lut[7]] = pal[b3 & 0x0Fu];
				}
			}
		}
	}

	FlushTextureData(&mTex);
	return true;
}

u32	CNativeTexture::GetStride() const
{
	return CalcBytesRequired( mTextureBlockWidth, mTextureFormat );
}

u32 CNativeTexture::GetBytesRequired() const
{
	return GetStride() * mCorrectedHeight;
}
