// FrontendCTR: PSX3D-inspired two-screen frontend built directly on picaGL.
//
// Design for Old 3DS stability and performance:
// - Single GPU owner (picaGL only; no Citro2D/Citro3D GX queue conflict).
// - Batched vertex arrays in linear RAM (2-3 draw calls per screen, never
//   overflowing picaGL's immediate vertex buffer).
// - 8x8 bitmap font uploaded once into a 128x64 GL_RGBA texture atlas (32 KiB).
// - Single pglSwapBuffersEx(1, 1) per frame after drawing both screens.
// - Strict OpenGL state setup and full RendererCTR state restoration on return.
// - Fixed-capacity ROM storage and on-demand single preview texture, released
//   before gameplay so maximum RAM stays available for N64 emulation.

#include "stdafx.h"
#include "FrontendCTR.h"

#include <3ds.h>
#include <citro3d.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>

#include "Core/CPU.h"
#include "Core/ROM.h"
#include "Core/RomSettings.h"
#include "Config/ConfigOptions.h"
#include "Graphics/NativeTexture.h"
#include "HLEGraphics/BaseRenderer.h"
#include "HLEGraphics/DaedalusVtx.h"
#include "Interface/RomDB.h"
#include "SysCTR/Graphics/Citro3DContext.h"
#include "Utility/IO.h"
#include "Utility/Preferences.h"
#include "Utility/ROMFile.h"
#include "SysCTR/UI/UserInterface.h"

extern bool isN3DS;
extern bool shouldQuit;
extern bool gDynarecAvailable;
extern EFrameskipValue gFrameskipValue;
extern f32 gZoomX;
extern float gCurrentFramerate;

namespace
{
	// ---------------------------------------------------------------- colour helpers
	// Little-endian RGBA8888 layout matching GL_UNSIGNED_BYTE glColorPointer.
	static inline u32 RGBA(u8 r, u8 g, u8 b, u8 a = 255)
	{
		return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
	}

	static inline u32 WithAlpha(u32 c, u8 a)
	{
		return (c & 0x00FFFFFFu) | ((u32)a << 24);
	}

	// ---------------------------------------------------------------- PSX3D themes
	struct Theme
	{
		const char * name;
		u32 bg;
		u32 panel;
		u32 cardTop;
		u32 cardBottom;
		u32 hlGlow;
		u32 hlTop;
		u32 hlBottom;
		u32 text;
		u32 textDim;
		u32 value;
		u32 rule;
		u32 accent[4];
		u32 skyTop;
		u32 skyMid;
		u32 skyHorizon;
		u32 floorTop;
		u32 floorBottom;
		u32 sunTop;
		u32 sunBottom;
		u32 grid;
	};

	static const Theme kThemes[] = {
		{
			"Original (PSX3D)",
			RGBA(6, 9, 24), RGBA(12, 18, 48), RGBA(20, 26, 60), RGBA(8, 10, 28),
			RGBA(60, 90, 200, 48), RGBA(40, 58, 130, 235), RGBA(24, 34, 84, 235),
			RGBA(235, 238, 255), RGBA(150, 150, 180), RGBA(130, 160, 230), RGBA(31, 42, 90),
			{ RGBA(230, 30, 60), RGBA(250, 205, 20), RGBA(30, 190, 120), RGBA(40, 110, 230) },
			RGBA(26, 6, 58), RGBA(92, 16, 104), RGBA(220, 42, 118),
			RGBA(36, 8, 58), RGBA(6, 3, 18),
			RGBA(255, 236, 110), RGBA(255, 80, 100), RGBA(150, 60, 255, 130)
		},
		{
			"N64 Classic",
			RGBA(13, 15, 22), RGBA(21, 25, 36), RGBA(34, 40, 56), RGBA(18, 22, 32),
			RGBA(38, 112, 232, 60), RGBA(34, 90, 192, 240), RGBA(20, 56, 132, 240),
			RGBA(244, 246, 252), RGBA(154, 166, 196), RGBA(246, 206, 56), RGBA(44, 54, 78),
			{ RGBA(228, 38, 38), RGBA(246, 198, 24), RGBA(32, 174, 78), RGBA(34, 108, 232) },
			RGBA(10, 14, 28), RGBA(18, 28, 54), RGBA(32, 52, 92),
			RGBA(16, 22, 38), RGBA(9, 11, 18),
			RGBA(246, 206, 48), RGBA(228, 44, 44), RGBA(62, 118, 212, 115)
		},
		{
			"Midnight Teal",
			RGBA(4, 14, 20), RGBA(8, 24, 34), RGBA(12, 42, 54), RGBA(4, 16, 22),
			RGBA(40, 180, 200, 55), RGBA(20, 92, 110, 235), RGBA(10, 50, 64, 235),
			RGBA(230, 246, 248), RGBA(110, 165, 175), RGBA(115, 220, 230), RGBA(24, 66, 78),
			{ RGBA(70, 215, 200), RGBA(120, 235, 225), RGBA(40, 165, 190), RGBA(60, 130, 215) },
			RGBA(3, 12, 24), RGBA(8, 40, 60), RGBA(26, 108, 128),
			RGBA(6, 26, 36), RGBA(3, 10, 16),
			RGBA(190, 250, 245), RGBA(60, 175, 205), RGBA(70, 210, 220, 120)
		},
		{
			"PS1 Gray",
			RGBA(24, 24, 28), RGBA(34, 34, 40), RGBA(54, 54, 62), RGBA(28, 28, 34),
			RGBA(200, 205, 220, 45), RGBA(84, 86, 98, 235), RGBA(54, 56, 66, 235),
			RGBA(238, 240, 248), RGBA(154, 156, 168), RGBA(205, 210, 230), RGBA(66, 68, 78),
			{ RGBA(220, 55, 70), RGBA(240, 195, 45), RGBA(50, 185, 125), RGBA(65, 125, 225) },
			RGBA(18, 18, 24), RGBA(44, 46, 56), RGBA(86, 90, 106),
			RGBA(32, 32, 40), RGBA(16, 16, 20),
			RGBA(235, 238, 248), RGBA(140, 148, 172), RGBA(180, 185, 210, 110)
		},
		{
			"Sunset Magenta",
			RGBA(16, 4, 26), RGBA(28, 8, 42), RGBA(50, 14, 58), RGBA(20, 5, 30),
			RGBA(255, 80, 200, 55), RGBA(120, 30, 120, 235), RGBA(60, 14, 74, 235),
			RGBA(250, 232, 248), RGBA(190, 130, 190), RGBA(255, 145, 215), RGBA(76, 30, 84),
			{ RGBA(255, 80, 165), RGBA(255, 190, 120), RGBA(220, 90, 230), RGBA(255, 120, 205) },
			RGBA(22, 4, 38), RGBA(104, 16, 96), RGBA(220, 62, 138),
			RGBA(36, 8, 48), RGBA(14, 3, 22),
			RGBA(255, 220, 145), RGBA(255, 70, 165), RGBA(255, 95, 205, 125)
		},
		{
			"High Contrast",
			RGBA(0, 0, 0), RGBA(14, 16, 24), RGBA(26, 28, 40), RGBA(4, 4, 8),
			RGBA(90, 150, 255, 70), RGBA(30, 74, 205, 255), RGBA(18, 48, 150, 255),
			RGBA(255, 255, 255), RGBA(195, 205, 225), RGBA(150, 210, 255), RGBA(72, 82, 110),
			{ RGBA(255, 75, 95), RGBA(255, 225, 65), RGBA(60, 230, 150), RGBA(80, 160, 255) },
			RGBA(0, 0, 0), RGBA(10, 16, 38), RGBA(28, 52, 118),
			RGBA(8, 12, 28), RGBA(0, 0, 0),
			RGBA(240, 248, 255), RGBA(95, 165, 255), RGBA(110, 180, 255, 130)
		},
		{
			"Phosphor",
			RGBA(2, 10, 5), RGBA(6, 22, 12), RGBA(12, 40, 22), RGBA(4, 14, 8),
			RGBA(60, 255, 120, 50), RGBA(20, 90, 45, 235), RGBA(8, 50, 24, 235),
			RGBA(225, 255, 235), RGBA(105, 175, 125), RGBA(115, 245, 155), RGBA(26, 76, 44),
			{ RGBA(80, 255, 140), RGBA(175, 255, 120), RGBA(45, 210, 120), RGBA(90, 235, 180) },
			RGBA(2, 10, 5), RGBA(8, 38, 20), RGBA(24, 96, 52),
			RGBA(6, 24, 12), RGBA(2, 8, 4),
			RGBA(200, 255, 180), RGBA(45, 200, 105), RGBA(75, 240, 135, 120)
		},
		{
			"Eclipse",
			RGBA(6, 5, 12), RGBA(16, 14, 24), RGBA(34, 28, 40), RGBA(10, 8, 18),
			RGBA(255, 220, 170, 45), RGBA(78, 62, 42, 235), RGBA(42, 32, 24, 235),
			RGBA(245, 240, 252), RGBA(165, 154, 178), RGBA(245, 215, 170), RGBA(64, 54, 72),
			{ RGBA(245, 205, 140), RGBA(255, 228, 185), RGBA(155, 130, 230), RGBA(210, 165, 115) },
			RGBA(6, 4, 14), RGBA(26, 18, 44), RGBA(76, 52, 68),
			RGBA(18, 14, 28), RGBA(5, 4, 10),
			RGBA(255, 236, 195), RGBA(185, 125, 160), RGBA(210, 175, 235, 115)
		},
		{
			"Ember",
			RGBA(14, 5, 3), RGBA(28, 10, 6), RGBA(52, 20, 10), RGBA(18, 6, 4),
			RGBA(255, 120, 40, 55), RGBA(120, 44, 16, 235), RGBA(66, 22, 8, 235),
			RGBA(252, 238, 230), RGBA(192, 140, 112), RGBA(255, 178, 112), RGBA(88, 38, 20),
			{ RGBA(255, 95, 40), RGBA(255, 195, 80), RGBA(230, 125, 45), RGBA(255, 70, 50) },
			RGBA(16, 4, 4), RGBA(68, 18, 10), RGBA(175, 58, 24),
			RGBA(32, 10, 6), RGBA(12, 4, 2),
			RGBA(255, 225, 130), RGBA(255, 82, 34), RGBA(255, 125, 55, 125)
		}
	};
	const int kThemeCount = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
	int s_themeIdx = 0;

	static inline const Theme & ActiveTheme()
	{
		if (s_themeIdx < 0 || s_themeIdx >= kThemeCount)
			s_themeIdx = 0;
		return kThemes[s_themeIdx];
	}

	// ---------------------------------------------------------------- 8x8 ASCII font
	// Clean 8x8 monochrome bitmap font (ASCII 32..126), rows top-to-bottom,
	// MSB on the left. Uploaded once into a 128x64 GL_RGBA atlas texture.
	static const u64 kFont[96] = {
		0x0000000000000000ULL, // ' '
		0x1818181818001800ULL, // !
		0x6666660000000000ULL, // "
		0x6666ff66ff666600ULL, // #
		0x183e603c067c1800ULL, // $
		0x62660c1830664600ULL, // %
		0x3c663c3867663b00ULL, // &
		0x1818300000000000ULL, // '
		0x0c18303030180c00ULL, // (
		0x30180c0c0c183000ULL, // )
		0x00663cff3c660000ULL, // *
		0x0018187e18180000ULL, // +
		0x0000000000181830ULL, // ,
		0x0000007e00000000ULL, // -
		0x0000000000181800ULL, // .
		0x060c183060c08000ULL, // /
		0x3c666e7666663c00ULL, // 0
		0x1838181818187e00ULL, // 1
		0x3c66060c18307e00ULL, // 2
		0x3c66061c06663c00ULL, // 3
		0x0c1c3c6c7e0c0c00ULL, // 4
		0x7e607c0606663c00ULL, // 5
		0x3c607c6666663c00ULL, // 6
		0x7e060c1830303000ULL, // 7
		0x3c66663c66663c00ULL, // 8
		0x3c66663e060c3800ULL, // 9
		0x0018180000181800ULL, // :
		0x0018180000181830ULL, // ;
		0x0c18306030180c00ULL, // <
		0x00007e007e000000ULL, // =
		0x30180c060c183000ULL, // >
		0x3c66061c18001800ULL, // ?
		0x3c666e6a6e603c00ULL, // @
		0x183c66667e666600ULL, // A
		0x7c66667c66667c00ULL, // B
		0x3c66606060663c00ULL, // C
		0x786c6666666c7800ULL, // D
		0x7e60607c60607e00ULL, // E
		0x7e60607c60606000ULL, // F
		0x3c66606e66663e00ULL, // G
		0x6666667e66666600ULL, // H
		0x3c18181818183c00ULL, // I
		0x1e0c0c0c0c6c3800ULL, // J
		0x666c7870786c6600ULL, // K
		0x6060606060607e00ULL, // L
		0x63777f6b63636300ULL, // M
		0x66767e7e6e666600ULL, // N
		0x3c66666666663c00ULL, // O
		0x7c66667c60606000ULL, // P
		0x3c6666666a6c3600ULL, // Q
		0x7c66667c6c666600ULL, // R
		0x3c66603c06663c00ULL, // S
		0x7e18181818181800ULL, // T
		0x6666666666663c00ULL, // U
		0x66666666663c1800ULL, // V
		0x6363636b7f776300ULL, // W
		0x66663c183c666600ULL, // X
		0x6666663c18181800ULL, // Y
		0x7e060c1830607e00ULL, // Z
		0x3c30303030303c00ULL, // [
		0x80c06030180c0600ULL, // backslash
		0x3c0c0c0c0c0c3c00ULL, // ]
		0x183c660000000000ULL, // ^
		0x00000000000000ffULL, // _
		0x30180c0000000000ULL, // `
		0x00003c063e663e00ULL, // a
		0x60607c6666667c00ULL, // b
		0x00003c6660663c00ULL, // c
		0x06063e6666663e00ULL, // d
		0x00003c667e603c00ULL, // e
		0x1c307c3030303000ULL, // f
		0x00003e66663e063cULL, // g
		0x60607c6666666600ULL, // h
		0x1800381818183c00ULL, // i
		0x0c001c0c0c0c6c38ULL, // j
		0x6060666c786c6600ULL, // k
		0x3818181818183c00ULL, // l
		0x0000667f7f6b6300ULL, // m
		0x00007c6666666600ULL, // n
		0x00003c6666663c00ULL, // o
		0x00007c66667c6060ULL, // p
		0x00003e66663e0606ULL, // q
		0x00007c6660606000ULL, // r
		0x00003e603c067c00ULL, // s
		0x30307c3030341800ULL, // t
		0x0000666666663e00ULL, // u
		0x00006666663c1800ULL, // v
		0x0000636b7f7f3600ULL, // w
		0x0000663c183c6600ULL, // x
		0x00006666663e063cULL, // y
		0x00007e0c18307e00ULL, // z
		0x0e18187018180e00ULL, // {
		0x1818181818181800ULL, // |
		0x7018180e18187000ULL, // }
		0x3b6e000000000000ULL, // ~
		0x0000000000000000ULL
	};

	// ---------------------------------------------------------------- batched citro3d renderer
	// Double-buffered linear DaedalusVtx pool holds all geometry for a full two-screen
	// frame so CPU writes to the next bank while GPU presents the previous one.
	const u32 kHalfVerts     = 4096;
	const u32 kMaxBatchVerts = kHalfVerts * 2;
	const float kWhiteU = 4.0f / 128.0f;
	const float kWhiteV = 4.0f / 64.0f;
	DaedalusVtx * s_vtxPool  = nullptr;
	u32           s_frameBank = 0;
	u32           s_vtxOffset = 0;      // vertices already submitted in current bank
	u32           s_batchStart = 0;     // start index of the current draw batch
	u32           s_batchCount = 0;     // vertices queued in the current draw batch
	C3D_Tex *     s_batchTex = nullptr; // active batch texture (s_fontTex or s_previewTex)
	CRefPtr<CNativeTexture> s_fontTex;

	bool EnsureGpuResources()
	{
		if (s_vtxPool == nullptr)
		{
			s_vtxPool = static_cast<DaedalusVtx *>(linearAlloc(kMaxBatchVerts * sizeof(DaedalusVtx)));
			if (!s_vtxPool)
				return false;
		}

		if (s_fontTex == nullptr)
		{
			const u32 atlasW = 128;
			const u32 atlasH = 64;
			u32 * pixels = (u32 *)malloc(atlasW * atlasH * sizeof(u32));
			if (!pixels)
				return false;
			// Transparent white background prevents dark halo bleed near glyphs.
			for (u32 i = 0; i < atlasW * atlasH; ++i)
				pixels[i] = 0x00FFFFFFu;

			// Reserve cell 0 (' ', 8x8 at top-left) as an opaque white swatch so
			// solid/gradient quads and glyph draws share one textured batch.
			for (u32 row = 0; row < 8; ++row)
				for (u32 col = 0; col < 8; ++col)
					pixels[row * atlasW + col] = 0xFFFFFFFFu;

			for (u32 g = 1; g < 96; ++g)
			{
				const u32 gx = (g % 16) * 8;
				const u32 gy = (g / 16) * 8;
				const u64 bits = kFont[g];
				for (u32 row = 0; row < 8; ++row)
				{
					const u8 rb = (u8)(bits >> (56 - row * 8));
					for (u32 col = 0; col < 8; ++col)
					{
						if ((rb >> (7 - col)) & 1)
							pixels[(gy + row) * atlasW + (gx + col)] = 0xFFFFFFFFu;
					}
				}
			}

			s_fontTex = CNativeTexture::Create(atlasW, atlasH, TexFmt_8888);
			if (s_fontTex != nullptr && s_fontTex->HasData())
			{
				s_fontTex->SetData(pixels, nullptr);
				s_fontTex->ApplyParams(GPU_NEAREST, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			}
			free(pixels);
		}
		return s_fontTex != nullptr && s_fontTex->HasData();
	}

	void FreeGpuResources()
	{
		s_fontTex = nullptr;
		if (s_vtxPool) { linearFree(s_vtxPool); s_vtxPool = nullptr; }
		s_frameBank = 0;
		s_vtxOffset = 0;
		s_batchStart = 0;
		s_batchCount = 0;
		s_batchTex = nullptr;
	}

	void FlushBatch()
	{
		if (s_batchCount == 0 || !s_vtxPool || s_fontTex == nullptr)
			return;

		C3D_Tex * tex = (s_batchTex != nullptr) ? s_batchTex : s_fontTex->GetC3DTexture();
		Citro3D::SetTextureEnabled(true);
		Citro3D::BindTexture(tex);
		Citro3D::DrawArrays(GPU_TRIANGLES, (int)s_batchStart, (int)s_batchCount);
		s_vtxOffset += s_batchCount;
		s_batchStart = s_vtxOffset;
		s_batchCount = 0;
	}

	void SetBatchTexture(C3D_Tex * tex)
	{
		if (tex == nullptr && s_fontTex != nullptr)
			tex = s_fontTex->GetC3DTexture();
		if (s_batchTex != tex)
		{
			FlushBatch();
			s_batchTex = tex;
		}
	}

	bool ReserveVerts(u32 needed)
	{
		if (!s_vtxPool || needed > kHalfVerts)
			return false;
		const u32 bankBase = s_frameBank * kHalfVerts;
		const u32 bankEnd  = bankBase + kHalfVerts;
		if (s_batchStart + s_batchCount + needed > bankEnd)
		{
			FlushBatch();
			if (s_vtxOffset > bankBase)
				Citro3D::FlushDataCache(&s_vtxPool[bankBase], (s_vtxOffset - bankBase) * sizeof(DaedalusVtx));
			C3D_FrameSplit(GX_CMDLIST_FLUSH);
			C3D_FrameSync();
			s_vtxOffset = bankBase;
			s_batchStart = bankBase;
			s_batchCount = 0;
		}
		return true;
	}

	static inline void PushVert(float x, float y, float u, float v, u32 col)
	{
		DaedalusVtx & dst = s_vtxPool[s_batchStart + s_batchCount++];
		dst.Texture.x  = u;
		dst.Texture.y  = v;
		dst.Colour     = c32(col);
		dst.Position.x = x;
		dst.Position.y = y;
		dst.Position.z = 0.0f;
	}

	void QuadGrad(float x, float y, float w, float h, u32 cTL, u32 cTR, u32 cBL, u32 cBR)
	{
		SetBatchTexture(s_fontTex ? s_fontTex->GetC3DTexture() : nullptr);
		if (!ReserveVerts(6)) return;
		const float x1 = x + w;
		const float y1 = y + h;
		PushVert(x,  y,  kWhiteU, kWhiteV, cTL);
		PushVert(x1, y,  kWhiteU, kWhiteV, cTR);
		PushVert(x1, y1, kWhiteU, kWhiteV, cBR);
		PushVert(x,  y,  kWhiteU, kWhiteV, cTL);
		PushVert(x1, y1, kWhiteU, kWhiteV, cBR);
		PushVert(x,  y1, kWhiteU, kWhiteV, cBL);
	}

	void Quad(float x, float y, float w, float h, u32 col)
	{
		QuadGrad(x, y, w, h, col, col, col, col);
	}

	void QuadVGrad(float x, float y, float w, float h, u32 top, u32 bot)
	{
		QuadGrad(x, y, w, h, top, top, bot, bot);
	}

	void DiagLine(float x0, float y0, float x1, float y1, float halfW, u32 col)
	{
		SetBatchTexture(s_fontTex ? s_fontTex->GetC3DTexture() : nullptr);
		if (!ReserveVerts(6)) return;
		PushVert(x0 - halfW, y0, kWhiteU, kWhiteV, col);
		PushVert(x0 + halfW, y0, kWhiteU, kWhiteV, col);
		PushVert(x1 + halfW, y1, kWhiteU, kWhiteV, col);
		PushVert(x0 - halfW, y0, kWhiteU, kWhiteV, col);
		PushVert(x1 + halfW, y1, kWhiteU, kWhiteV, col);
		PushVert(x1 - halfW, y1, kWhiteU, kWhiteV, col);
	}

	void DrawTexturedQuad(C3D_Tex * tex, float x, float y, float w, float h,
		float u0, float v0, float u1, float v1, u32 col = 0xFFFFFFFFu)
	{
		if (tex == nullptr) return;
		SetBatchTexture(tex);
		if (!ReserveVerts(6)) return;
		const float x1 = x + w;
		const float y1 = y + h;
		PushVert(x,  y,  u0, v0, col);
		PushVert(x1, y,  u1, v0, col);
		PushVert(x1, y1, u1, v1, col);
		PushVert(x,  y,  u0, v0, col);
		PushVert(x1, y1, u1, v1, col);
		PushVert(x,  y1, u0, v1, col);
	}

	void DrawText(float x, float y, const char * s, u32 color, float scale = 1.0f)
	{
		if (!s || !s[0] || s_fontTex == nullptr)
			return;
		SetBatchTexture(s_fontTex->GetC3DTexture());
		const float adv = 8.0f * scale;
		const float invW = 1.0f / 128.0f;
		const float invH = 1.0f / 64.0f;
		float cx = x;
		for (const char * p = s; *p; ++p)
		{
			unsigned char ch = (unsigned char)*p;
			if (ch == ' ')
			{
				cx += adv;
				continue;
			}
			if (ch < 32 || ch > 126) ch = '?';
			if (!ReserveVerts(6)) return;
			const u32 idx = ch - 32;
			const float u0 = (float)((idx % 16) * 8) * invW;
			const float v0 = (float)((idx / 16) * 8) * invH;
			const float u1 = u0 + 8.0f * invW;
			const float v1 = v0 + 8.0f * invH;
			const float x1 = cx + adv;
			const float y1 = y + adv;
			PushVert(cx, y,  u0, v0, color);
			PushVert(x1, y,  u1, v0, color);
			PushVert(x1, y1, u1, v1, color);
			PushVert(cx, y,  u0, v0, color);
			PushVert(x1, y1, u1, v1, color);
			PushVert(cx, y1, u0, v1, color);
			cx += adv;
		}
	}

	void DrawTextShadow(float x, float y, const char * s, u32 color, u32 shadow, float scale = 1.0f)
	{
		DrawText(x + 1.0f, y + 1.0f, s, shadow, scale);
		DrawText(x, y, s, color, scale);
	}

	float TextWidth(const char * s, float scale = 1.0f)
	{
		return s ? (float)strlen(s) * 8.0f * scale : 0.0f;
	}

	void DrawCenteredText(float cx, float y, const char * s, u32 color, float scale = 1.0f)
	{
		DrawText(cx - TextWidth(s, scale) * 0.5f, y, s, color, scale);
	}

	void DrawRightText(float rightX, float y, const char * s, u32 color, float scale = 1.0f)
	{
		DrawText(rightX - TextWidth(s, scale), y, s, color, scale);
	}

	void DrawTextEllipsis(float x, float y, float max_w, const char * s, u32 color, float scale = 1.0f)
	{
		if (!s || !s[0]) return;
		if (TextWidth(s, scale) <= max_w)
		{
			DrawText(x, y, s, color, scale);
			return;
		}
		char buf[128];
		snprintf(buf, sizeof(buf), "%s", s);
		size_t n = strlen(buf);
		while (n > 1 && TextWidth(buf, scale) > max_w)
		{
			buf[--n] = 0;
			if (n >= 3) { buf[n - 3] = '.'; buf[n - 2] = '.'; buf[n - 1] = '.'; }
		}
		DrawText(x, y, buf, color, scale);
	}

	void DrawTextWrapped(float x, float y, float max_w, float line_h, const char * s, u32 color, float scale = 1.0f)
	{
		if (!s || !s[0]) return;
		const int maxChars = (int)(max_w / (8.0f * scale));
		if (maxChars <= 1) return;
		const char * p = s;
		float cy = y;
		while (*p)
		{
			while (*p == ' ') ++p;
			if (!*p) break;
			int len = 0;
			int lastSpace = -1;
			while (p[len] && p[len] != '\n' && len < maxChars)
			{
				if (p[len] == ' ') lastSpace = len;
				++len;
			}
			if (p[len] && p[len] != '\n' && lastSpace > 0)
				len = lastSpace;
			char line[128];
			if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;
			memcpy(line, p, (size_t)len);
			line[len] = 0;
			DrawText(x, cy, line, color, scale);
			p += len;
			if (*p == '\n') ++p;
			cy += line_h;
		}
	}

	// ---------------------------------------------------------------- frame lifecycle
	void BeginFrame()
	{
		s_frameBank ^= 1;
		const u32 bankBase = s_frameBank * kHalfVerts;
		s_vtxOffset = bankBase;
		s_batchStart = bankBase;
		s_batchCount = 0;
		s_batchTex = s_fontTex ? s_fontTex->GetC3DTexture() : nullptr;
		Citro3D::BeginUIFrame();
	}

	void BeginScreen(gfxScreen_t screen, int w, int h)
	{
		FlushBatch();
		const u32 bg = ActiveTheme().bg;
		// Convert ABGR (RGBA helper) to 0xRRGGBBAA for C3D_RenderTargetClear.
		const u32 clearRGBA8 = ((bg & 0xFFu) << 24) |
		                       (((bg >> 8) & 0xFFu) << 16) |
		                       (((bg >> 16) & 0xFFu) << 8) |
		                       0xFFu;
		Citro3D::BeginUIScreen(screen, w, h, clearRGBA8);
		Citro3D::SetVertexBuffer(s_vtxPool, sizeof(DaedalusVtx));
		s_batchTex = s_fontTex ? s_fontTex->GetC3DTexture() : nullptr;
		if (s_batchTex)
			Citro3D::BindTexture(s_batchTex);
	}

	void PresentBothScreens()
	{
		FlushBatch();
		const u32 bankBase = s_frameBank * kHalfVerts;
		if (s_vtxPool && s_vtxOffset > bankBase)
			Citro3D::FlushDataCache(&s_vtxPool[bankBase], (s_vtxOffset - bankBase) * sizeof(DaedalusVtx));
		// Single C3D_FrameEnd submits both screens on the same VBlank.
		Citro3D::EndUIFrame();
	}

	// ---------------------------------------------------------------- PSX3D visuals
	static inline u32 DimColor(u32 c, float dim)
	{
		if (dim <= 0.001f) return c;
		if (dim > 0.92f) dim = 0.92f;
		const float keep = 1.0f - dim;
		const u8 r = (u8)((c & 255) * keep);
		const u8 g = (u8)(((c >> 8) & 255) * keep);
		const u8 b = (u8)(((c >> 16) & 255) * keep);
		const u8 a = (u8)((c >> 24) & 255);
		return RGBA(r, g, b, a);
	}

	void DrawSynthwaveBackdrop(float clock_s)
	{
		const Theme & t = ActiveTheme();
		// Authentic PSX3D synthwave sky (two smooth bands) & deep floor
		QuadVGrad(0, 0, 400, 74, t.skyTop, t.skyMid);
		QuadVGrad(0, 74, 400, 64, t.skyMid, t.skyHorizon);
		QuadVGrad(0, 138, 400, 102, t.floorTop, t.floorBottom);

		// Centered PSX3D retro sliced sun disc (14 precomputed slices; zero sqrtf)
		struct SunSlice { float dy, h, halfW, frac; };
		static const SunSlice kSunSlices[14] = {
			{ -40.0f, 5.0f, 20.0f, 0.03f },
			{ -35.0f, 5.0f, 27.4f, 0.09f },
			{ -30.0f, 5.0f, 32.5f, 0.15f },
			{ -25.0f, 5.0f, 36.0f, 0.21f },
			{ -20.0f, 5.0f, 38.8f, 0.27f },
			{ -15.0f, 5.0f, 40.6f, 0.33f },
			{ -10.0f, 5.0f, 41.9f, 0.39f },
			{  -5.0f, 5.0f, 42.5f, 0.45f },
			{   0.0f, 5.0f, 42.2f, 0.51f },
			{   5.0f, 5.0f, 41.2f, 0.58f },
			{  12.0f, 4.0f, 39.6f, 0.67f },
			{  18.0f, 3.0f, 36.8f, 0.74f },
			{  24.0f, 3.0f, 33.4f, 0.82f },
			{  30.0f, 2.0f, 28.8f, 0.90f }
		};
		const float sunCx = 200.0f;
		const float sunCy = 104.0f;
		const int r0 = (int)(t.sunTop & 255);
		const int g0 = (int)((t.sunTop >> 8) & 255);
		const int b0 = (int)((t.sunTop >> 16) & 255);
		const int dr = (int)(t.sunBottom & 255) - r0;
		const int dg = (int)((t.sunBottom >> 8) & 255) - g0;
		const int db = (int)((t.sunBottom >> 16) & 255) - b0;
		for (int i = 0; i < 14; ++i)
		{
			const SunSlice & s = kSunSlices[i];
			const u8 r = (u8)(r0 + (int)(dr * s.frac));
			const u8 g = (u8)(g0 + (int)(dg * s.frac));
			const u8 b = (u8)(b0 + (int)(db * s.frac));
			Quad(sunCx - s.halfW, sunCy + s.dy, s.halfW * 2.0f, s.h, RGBA(r, g, b, 210));
		}

		// Horizon glow band
		Quad(0, 137, 400, 2, WithAlpha(t.sunBottom, 175));

		// Animated PSX3D perspective grid
		const float phase = fmodf(clock_s * 0.45f, 1.0f);
		for (int i = 0; i < 8; ++i)
		{
			const float p = ((float)i + phase) / 8.0f;
			const float y = 138.0f + (p * p) * 102.0f;
			const u8 a = (u8)(45 + p * 125);
			Quad(0, y, 400, 1.0f, WithAlpha(t.grid, a));
		}
		for (int x = -180; x <= 580; x += 48)
		{
			DiagLine(200.0f, 138.0f, (float)x, 240.0f, 0.70f, WithAlpha(t.grid, 90));
		}
	}

	void DrawHeader(float w, const char * title, const char * right)
	{
		const Theme & t = ActiveTheme();
		Quad(0, 0, w, 30, WithAlpha(t.bg, 235));
		// PSX3D 4-colour accent bar
		for (int i = 0; i < 4; ++i)
			Quad(10.0f + i * 16.0f, 26.0f, 13.0f, 2.0f, t.accent[i]);
		Quad(76.0f, 27.0f, w - 86.0f, 1.0f, t.rule);
		DrawTextShadow(10.0f, 9.0f, title, t.text, WithAlpha(t.bg, 220), 1.0f);
		if (right && right[0])
			DrawRightText(w - 10.0f, 10.0f, right, t.textDim, 0.875f);
	}

	struct BarBtn
	{
		const char * key;
		const char * label;
		bool primary;
	};

	void DrawButtonBar(const BarBtn * btns, int count, int pressedIdx = -1)
	{
		const Theme & t = ActiveTheme();
		const float barY = 212.0f;
		const float barH = 28.0f;
		Quad(0, barY, 320.0f, barH, t.panel);
		Quad(0, barY, 320.0f, 1.0f, t.rule);
		const float bw = 320.0f / (float)count;
		for (int i = 0; i < count; ++i)
		{
			const float bx = bw * i;
			const bool pressed = (i == pressedIdx);
			if (pressed)
			{
				QuadVGrad(bx + 1.0f, barY + 1.0f, bw - 2.0f, barH - 2.0f, t.hlBottom, t.hlTop);
				Quad(bx + 1.0f, barY + 1.0f, bw - 2.0f, 2.0f, t.accent[1]);
			}
			else if (btns[i].primary)
			{
				QuadVGrad(bx, barY + 1.0f, bw, barH - 1.0f, t.hlTop, t.hlBottom);
				Quad(bx, barY + 1.0f, bw, 1.0f, WithAlpha(t.accent[3], 180));
			}
			else if (i > 0)
			{
				Quad(bx, barY + 5.0f, 1.0f, barH - 10.0f, t.rule);
			}

			const u32 col = (btns[i].primary || pressed) ? t.text : t.textDim;
			DrawCenteredText(bx + bw * 0.5f, barY + 5.0f, btns[i].key,
				(btns[i].primary || pressed) ? t.accent[1] : t.value, 0.75f);
			DrawCenteredText(bx + bw * 0.5f, barY + 16.0f, btns[i].label, col, 0.75f);
		}
	}

	int HitButtonBar(int px, int py, int count)
	{
		if (py < 212 || py >= 240 || px < 0 || px >= 320 || count <= 0)
			return -1;
		int idx = (px * count) / 320;
		if (idx < 0) idx = 0;
		if (idx >= count) idx = count - 1;
		return idx;
	}

	// ---------------------------------------------------------------- touch state tracker
	// Robust 3DS touch handling:
	// - Filters invalid (0,0) touch frames reported on edge transitions.
	// - Distinguishes taps (triggered on clean release) from drag scrolling.
	// - Tracks vertical drag deltas and smoothed kinetic release velocity.
	struct TouchState
	{
		bool  active;
		bool  justPressed;
		bool  justReleased;
		bool  dragged;
		bool  scrollbarDrag;
		int   startX, startY;
		int   curX, curY;
		int   prevX, prevY;
		int   releaseX, releaseY;
		float deltaY;
		float velY;

		void Reset()
		{
			std::memset(this, 0, sizeof(*this));
		}

		void Update(u32 down, u32 held)
		{
			justPressed = false;
			justReleased = false;
			deltaY = 0.0f;

			if (held & KEY_TOUCH)
			{
				touchPosition tp;
				hidTouchRead(&tp);
				const bool validCoord = (tp.px > 0 || tp.py > 0) && tp.px < 320 && tp.py < 240;
				if (!active)
				{
					if (validCoord)
					{
						active = true;
						justPressed = true;
						dragged = false;
						scrollbarDrag = false;
						startX = curX = prevX = tp.px;
						startY = curY = prevY = tp.py;
						velY = 0.0f;
					}
				}
				else if (validCoord)
				{
					prevX = curX;
					prevY = curY;
					curX = tp.px;
					curY = tp.py;
					const int dx = curX - startX;
					const int dy = curY - startY;
					if (!dragged && (dx * dx + dy * dy >= 64)) // 8px drag threshold
						dragged = true;
					deltaY = (float)(curY - prevY);
					velY = velY * 0.5f + deltaY * 0.5f;
				}
			}
			else
			{
				if (active)
				{
					justReleased = true;
					releaseX = curX;
					releaseY = curY;
				}
				active = false;
				scrollbarDrag = false;
			}
			(void)down;
		}
	};

	void DrawPill(float x, float y, float w, float h, const char * label, u32 fillCol, u32 borderCol, u32 textCol, float scale = 0.75f)
	{
		Quad(x, y, w, h, fillCol);
		Quad(x, y, w, 1.0f, borderCol);
		Quad(x, y + h - 1.0f, w, 1.0f, borderCol);
		Quad(x, y, 1.0f, h, borderCol);
		Quad(x + w - 1.0f, y, 1.0f, h, borderCol);
		DrawCenteredText(x + w * 0.5f, y + (h - 8.0f * scale) * 0.5f, label, textCol, scale);
	}

	// ---------------------------------------------------------------- input repeat
	struct Repeat { int timer[4]; };
	bool Pressed(Repeat & r, u32 down, u32 held, u32 mask, int idx)
	{
		if (down & mask) { r.timer[idx] = 0; return true; }
		if (held & mask)
		{
			if (++r.timer[idx] > 14 && (r.timer[idx] - 14) % 4 == 0)
				return true;
		}
		else
		{
			r.timer[idx] = 0;
		}
		return false;
	}

	// ---------------------------------------------------------------- ROM library (fixed storage)
	struct FrontendRomInfo
	{
		RomID    romId;
		u32      size;
		bool     hasDetails;
		bool     favorite;
		char     filename[96];
		char     title[64];
		IO::Filename path;
		char     preview[96];
		char     comment[96];
		char     country[8];
	};

	const int kMaxRoms = 256;
	FrontendRomInfo s_roms[kMaxRoms];
	int s_romCount = 0;
	bool s_romsScanned = false;
	int s_viewOrder[kMaxRoms];
	int s_viewCount = 0;
	enum SortMode { SORT_AZ = 0, SORT_FAV, SORT_SIZE, SORT_COUNT };
	static const char * kSortNames[SORT_COUNT] = { "A-Z", "Fav", "Size" };
	int s_sortMode = SORT_AZ;

	int   s_selected = 0;
	float s_hlPos = 0.0f;
	float s_hlVel = 0.0f;
	float s_scrollPos = 0.0f;
	float s_scrollVel = 0.0f;
	bool  s_initialised = false;

	// Single on-demand preview texture (released before emulation).
	CRefPtr<CNativeTexture> s_previewTex;
	int s_previewRomIdx = -1;

	const char * kFrontendConfigPath = DAEDALUS_CTR_PATH("frontend.ini");
	char s_lastSelectedFile[96] = "";
	const int kMaxFavs = 128;
	char s_favList[kMaxFavs][96];
	int s_favCount = 0;

	static void CopyStr(char * dst, size_t dstSize, const char * src)
	{
		if (!dst || dstSize == 0) return;
		if (!src) { dst[0] = '\0'; return; }
		size_t n = strlen(src);
		if (n >= dstSize) n = dstSize - 1;
		if (n > 0) memmove(dst, src, n);
		dst[n] = '\0';
	}

	void LoadFrontendConfig()
	{
		s_favCount = 0;
		s_lastSelectedFile[0] = 0;
		s_themeIdx = (int)gGlobalPreferences.GuiColor;
		if (s_themeIdx < 0 || s_themeIdx >= kThemeCount)
			s_themeIdx = 0;

		FILE * f = fopen(kFrontendConfigPath, "r");
		if (!f) return;
		char line[256];
		while (fgets(line, sizeof(line), f))
		{
			line[strcspn(line, "\r\n")] = 0;
			if (!strncmp(line, "theme=", 6))
			{
				int v = atoi(line + 6);
				if (v >= 0 && v < kThemeCount) s_themeIdx = v;
			}
			else if (!strncmp(line, "sort=", 5))
			{
				int v = atoi(line + 5);
				if (v >= 0 && v < SORT_COUNT) s_sortMode = v;
			}
			else if (!strncmp(line, "last=", 5))
			{
				CopyStr(s_lastSelectedFile, sizeof(s_lastSelectedFile), line + 5);
			}
			else if (!strncmp(line, "fav=", 4) && line[4] && s_favCount < kMaxFavs)
			{
				CopyStr(s_favList[s_favCount++], sizeof(s_favList[0]), line + 4);
			}
		}
		fclose(f);
	}

	void SaveFrontendConfig()
	{
		FILE * f = fopen(kFrontendConfigPath, "w");
		if (!f) return;
		fprintf(f, "theme=%d\nsort=%d\nlast=%s\n", s_themeIdx, s_sortMode, s_lastSelectedFile);
		for (int i = 0; i < s_romCount; ++i)
		{
			if (s_roms[i].favorite)
				fprintf(f, "fav=%s\n", s_roms[i].filename);
		}
		fclose(f);
	}

	bool IsSavedFavorite(const char * filename)
	{
		for (int i = 0; i < s_favCount; ++i)
			if (strcmp(s_favList[i], filename) == 0)
				return true;
		return false;
	}

	const char * CountryName(u8 country_id)
	{
		switch (country_id)
		{
		case 'A': case 'E': return "USA";
		case 'J': return "JPN";
		case 'D': return "GER";
		case 'F': return "FRA";
		case 'I': return "ITA";
		case 'P': case 'S': case 'U': case 'X': case 'Y': return "EUR";
		default: return "???";
		}
	}

	void RebuildView()
	{
		s_viewCount = 0;
		for (int i = 0; i < s_romCount; ++i)
		{
			if (s_sortMode == SORT_FAV && !s_roms[i].favorite)
				continue;
			s_viewOrder[s_viewCount++] = i;
		}
		// If Favorites view is empty, show all games so the screen is never misleading.
		if (s_sortMode == SORT_FAV && s_viewCount == 0)
		{
			for (int i = 0; i < s_romCount; ++i)
				s_viewOrder[s_viewCount++] = i;
		}

		// Insertion sort in place (zero heap allocations)
		for (int a = 1; a < s_viewCount; ++a)
		{
			const int key = s_viewOrder[a];
			int b = a;
			while (b > 0)
			{
				const FrontendRomInfo & prev = s_roms[s_viewOrder[b - 1]];
				const FrontendRomInfo & cur  = s_roms[key];
				bool swap = false;
				if (s_sortMode == SORT_SIZE)
				{
					swap = (prev.size < cur.size) ||
						(prev.size == cur.size && strcasecmp(prev.title, cur.title) > 0);
				}
				else if (s_sortMode == SORT_AZ && prev.favorite != cur.favorite)
				{
					swap = !prev.favorite; // favorites first
				}
				else
				{
					swap = strcasecmp(prev.title, cur.title) > 0;
				}
				if (!swap) break;
				s_viewOrder[b] = s_viewOrder[b - 1];
				--b;
			}
			s_viewOrder[b] = key;
		}

		if (s_selected >= s_viewCount) s_selected = s_viewCount > 0 ? s_viewCount - 1 : 0;
		if (s_selected < 0) s_selected = 0;
		s_hlPos = (float)s_selected;
		s_hlVel = 0.0f;
	}

	int s_previewPendingRomIdx = -1;
	int s_previewDelayFrames = 0;

	void ReleasePreview()
	{
		s_previewTex = nullptr;
		s_previewRomIdx = -1;
		s_previewPendingRomIdx = -1;
		s_previewDelayFrames = 0;
	}

	void RequestPreviewForRom(int romIdx, bool immediate = false)
	{
		if (romIdx == s_previewRomIdx && s_previewPendingRomIdx == -1)
			return;
		if (immediate)
		{
			s_previewPendingRomIdx = romIdx;
			s_previewDelayFrames = 0;
			return;
		}
		if (s_previewPendingRomIdx != romIdx)
		{
			s_previewPendingRomIdx = romIdx;
			s_previewDelayFrames = 8; // debounce SD PNG decode (~133ms) while scrolling fast
		}
	}

	void TickPreviewLoader()
	{
		if (s_previewPendingRomIdx == -1)
			return;
		if (s_previewDelayFrames > 0)
		{
			--s_previewDelayFrames;
			return;
		}
		const int romIdx = s_previewPendingRomIdx;
		s_previewPendingRomIdx = -1;
		if (romIdx == s_previewRomIdx)
			return;

		s_previewTex = nullptr;
		s_previewRomIdx = romIdx;
		if (romIdx < 0 || romIdx >= s_romCount)
			return;
		const FrontendRomInfo & r = s_roms[romIdx];
		if (r.preview[0] == '\0')
			return;

		IO::Filename previewPath;
		IO::Path::Combine(previewPath, DAEDALUS_CTR_PATH("Resources/Preview/"), r.preview);
		if (IO::File::Exists(previewPath))
		{
			s_previewTex = CNativeTexture::CreateFromPng(previewPath, TexFmt_5551);
			if (s_previewTex != nullptr && s_previewTex->HasData())
			{
				s_previewTex->ApplyParams(GPU_LINEAR, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			}
		}
	}

	void ScanRoms(bool force)
	{
		if (s_romsScanned && !force)
			return;

		ReleasePreview();
		s_romCount = 0;
		IO::FindHandleT handle;
		IO::FindDataT data;
		const char * dir = DAEDALUS_CTR_PATH("Roms/");
		if (IO::FindFileOpen(dir, &handle, data))
		{
			do {
				if (!IsRomfilename(data.Name)) continue;
				if (s_romCount >= kMaxRoms) break;

				FrontendRomInfo & info = s_roms[s_romCount];
				info = FrontendRomInfo();
				CopyStr(info.filename, sizeof(info.filename), data.Name);
				IO::Path::Combine(info.path, dir, data.Name);
				info.favorite = IsSavedFavorite(info.filename);

				RomID rom_id;
				u32 rom_size = 0;
				ECicType cic = CIC_UNKNOWN;
				if (ROM_GetRomDetailsByFilename(info.path, &rom_id, &rom_size, &cic))
				{
					info.romId = rom_id;
					info.size = rom_size;
					info.hasDetails = true;
					CopyStr(info.country, sizeof(info.country), CountryName(rom_id.CountryID));

					RomSettings settings;
					if (CRomSettingsDB::Get() && CRomSettingsDB::Get()->GetSettings(rom_id, &settings))
					{
						CopyStr(info.title, sizeof(info.title), settings.GameName.c_str());
						CopyStr(info.preview, sizeof(info.preview), settings.Preview.c_str());
						CopyStr(info.comment, sizeof(info.comment), settings.Comment.c_str());
					}
					else
					{
						settings.Reset();
						std::string name;
						if (ROM_GetRomName(info.path, name) && !name.empty())
							CopyStr(info.title, sizeof(info.title), name.c_str());
						else
							CopyStr(info.title, sizeof(info.title), info.filename);
						settings.GameName = info.title;
						if (CRomSettingsDB::Get())
							CRomSettingsDB::Get()->SetSettings(rom_id, settings);
					}
				}
				else
				{
					CopyStr(info.title, sizeof(info.title), info.filename);
					CopyStr(info.country, sizeof(info.country), "???");
				}
				++s_romCount;
			} while (IO::FindFileNext(handle, data));
			IO::FindFileClose(handle);
		}

		s_romsScanned = true;
		RebuildView();

		// Restore last selected ROM if present
		if (s_lastSelectedFile[0] != '\0')
		{
			for (int i = 0; i < s_viewCount; ++i)
			{
				if (strcmp(s_roms[s_viewOrder[i]].filename, s_lastSelectedFile) == 0)
				{
					s_selected = i;
					s_hlPos = (float)i;
					break;
				}
			}
		}
	}

	static inline void SpringStep(float & x, float & v, float target, float k, float damp)
	{
		const float dt = 1.0f / 60.0f;
		v += ((target - x) * k - v * damp) * dt;
		x += v * dt;
	}

	char s_memSummary[64] = "";

	void FormatMemorySummary(char * out, size_t len)
	{
		const struct mallinfo mi = mallinfo();
		const u32 heapCap = envGetHeapSize();
		const u32 heapFree = (heapCap > (u32)mi.uordblks) ? (heapCap - (u32)mi.uordblks) : 0;
		const u32 linFree = linearSpaceFree();
		snprintf(out, len, "RAM %.1fM | GPU %.1fM",
			heapFree / (1024.0f * 1024.0f), linFree / (1024.0f * 1024.0f));
	}

	void RefreshMemorySummary()
	{
		// mallinfo() traverses the full process heap; call only on menu transitions
		// rather than every rendered frame.
		FormatMemorySummary(s_memSummary, sizeof(s_memSummary));
	}

	// ---------------------------------------------------------------- options
	struct OptionEntry
	{
		const char * label;
		const char * help;
		int  kind;
	};

	const OptionEntry kOptions[] = {
		{ "DynaRec",              "Dynamic ARM recompilation. Huge speedup; applies on next ROM launch.", 0 },
		{ "Loop optimisation",    "Optimises hot loops in generated ARM code for higher FPS.", 0 },
		{ "Double precision",     "Higher FPU precision. Disable on Old 3DS for extra speed.", 0 },
		{ "Memory access opt.",   "Fast direct RDRAM paths in DynaRec. Recommended On.", 0 },
		{ "OS patches",           "High-level N64 OS hooks. Big speedup on Old 3DS.", 0 },
		{ "Speed limiter",        "Cap emulation speed to N64 refresh rate.", 0 },
		{ "Double display",       "Extra display list path needed by a few games.", 0 },
		{ "Clean scene",          "Clears stale framebuffer content each scene.", 0 },
		{ "Clear depth buffer",   "Clears depth buffer each configured frame.", 0 },
		{ "Frameskip",            "Skip rendering frames when slow. Auto 2 is best on Old 3DS.", 1 },
		{ "Texture checks",       "Re-hash cached textures interval. Every 8/16 saves ARM11 CPU.", 2 },
		{ "Video rate match",     "Sync VI rate to actual emulation speed.", 0 },
		{ "Fog",                  "Enable N64 hardware fog emulation.", 0 },
		{ "Aspect ratio",         "Top screen picture aspect ratio (console-wide).", 4 },
		{ "Zoom",                 "Horizontal picture zoom factor (live).", 5 },
		{ "Audio output",         "Async offloads 44kHz resampling to Core 1; Off gives max FPS.", 3 },
		{ "Audio rate match",     "Match audio pacing to emulation speed.", 0 },
		{ "Cheats",               "Enable cheat codes for this ROM.", 0 },
		{ "Show FPS",             "Enable frame rate measurement (console-wide).", 0 },
		{ "UI Theme",             "Change the PSX3D frontend colour theme.", 6 },
		{ "CPU Speed Hack",       "2x/3x scales DynaRec cycle counter for big FPS boosts (next launch).", 7 },
	};
	const int kNumOptions = (int)(sizeof(kOptions) / sizeof(kOptions[0]));

	const char * FrameskipName(int v)
	{
		static const char * names[] = { "Disabled", "Auto 1", "Auto 2", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
		return (v >= 0 && v < 12) ? names[v] : "?";
	}
	const char * TexHashName(int v)
	{
		static const char * names[] = { "Disabled", "Every 1", "Every 2", "Every 4", "Every 8", "Every 16", "Every 32" };
		return (v >= 0 && v < 7) ? names[v] : "?";
	}
	const char * AudioName(int v)
	{
		static const char * names[] = { "Off", "Async", "Sync" };
		return (v >= 0 && v < 3) ? names[v] : "?";
	}
	const char * ViewportName(int v)
	{
		switch (v)
		{
		case VT_UNSCALED_4_3:  return "4:3";
		case VT_FULLSCREEN:    return "Full";
		case VT_FULLSCREEN_HD: return "Full HD";
		case VT_SCALED_4_3:    return "4:3 Scaled";
		}
		return "?";
	}
}

namespace FrontendCTR
{
void RunOptionsPage(const RomID & romId, bool applyRuntime)
{
	if (!EnsureGpuResources())
		return;

	RefreshMemorySummary();
	SRomPreferences prefs;
	if (CPreferences::Get())
		CPreferences::Get()->GetRomPreferences(romId, &prefs);

	bool pending_fps = gGlobalPreferences.DisplayFramerate;
	EViewportType pending_viewport = gGlobalPreferences.ViewportType;
	int selection = 0;
	float scrollPos = 0.0f;
	float scrollVel = 0.0f;
	bool manualScroll = false;
	float hl = 0.0f, hl_v = 0.0f;
	float clock_s = 0.0f;
	Repeat rep = {};
	TouchState touch;
	touch.Reset();
	const int VISIBLE = 6;
	const float LIST_Y = 36.0f;
	const float ROW_H = 28.0f;
	const float MAX_SCROLL = (float)(kNumOptions - VISIBLE);

	static const BarBtn kOptBar[4] = {
		{ "B",     "Back", false },
		{ "L",     "-",    false },
		{ "R",     "+",    false },
		{ "START", "Save", true  }
	};

	while (aptMainLoop() && !shouldQuit)
	{
		// C3D_FrameEnd syncs to VBlank; avoid double gspWaitForVBlank() stall.
		hidScanInput();
		u32 down = hidKeysDown();
		const u32 held = hidKeysHeld();
		clock_s += 1.0f / 60.0f;
		touch.Update(down, held);

		int pressedBar = -1;
		int pressedRow = -1;
		if (touch.active)
		{
			if (!touch.dragged)
				pressedBar = HitButtonBar(touch.curX, touch.curY, 4);

			if (touch.justPressed && touch.curX >= 294 && touch.curY >= (int)LIST_Y && touch.curY < 210)
				touch.scrollbarDrag = true;

			if (touch.scrollbarDrag)
			{
				float frac = ((float)touch.curY - LIST_Y) / (VISIBLE * ROW_H);
				if (frac < 0.0f) frac = 0.0f;
				if (frac > 1.0f) frac = 1.0f;
				scrollPos = frac * MAX_SCROLL;
				scrollVel = 0.0f;
				manualScroll = true;
			}
			else if (touch.dragged && touch.startY >= (int)LIST_Y && touch.startY < 210)
			{
				scrollPos -= touch.deltaY / ROW_H;
				if (scrollPos < 0.0f) scrollPos = 0.0f;
				if (scrollPos > MAX_SCROLL) scrollPos = MAX_SCROLL;
				scrollVel = 0.0f;
				manualScroll = true;
			}
			else if (!touch.dragged && touch.curY >= (int)LIST_Y && touch.curY < (int)(LIST_Y + VISIBLE * ROW_H))
			{
				const int rIdx = (int)floorf(scrollPos + ((float)touch.curY - LIST_Y) / ROW_H);
				if (rIdx >= 0 && rIdx < kNumOptions)
					pressedRow = rIdx;
			}
		}

		if (touch.justReleased)
		{
			if (touch.dragged && !touch.scrollbarDrag && touch.startY >= (int)LIST_Y && touch.startY < 210)
			{
				scrollVel = -(touch.velY / ROW_H) * 60.0f * 0.45f;
				manualScroll = true;
			}
			else if (!touch.dragged && !touch.scrollbarDrag)
			{
				const int barHit = HitButtonBar(touch.releaseX, touch.releaseY, 4);
				if (barHit == 0) down |= KEY_B;
				else if (barHit == 1) down |= KEY_LEFT;
				else if (barHit == 2) down |= KEY_RIGHT;
				else if (barHit == 3) down |= KEY_START;
				else if (touch.releaseY >= (int)LIST_Y && touch.releaseY < (int)(LIST_Y + VISIBLE * ROW_H))
				{
					const int idx = (int)floorf(scrollPos + ((float)touch.releaseY - LIST_Y) / ROW_H);
					if (idx >= 0 && idx < kNumOptions)
					{
						// Tapping the value pill on the right toggles immediately; tapping label selects (or toggles if already selected).
						if (touch.releaseX >= 200 || idx == selection)
							down |= KEY_A;
						selection = idx;
						manualScroll = false;
					}
				}
			}
		}

		if (down & KEY_B) break;
		if (down & KEY_START)
		{
			if (CPreferences::Get())
			{
				CPreferences::Get()->SetRomPreferences(romId, prefs);
				gGlobalPreferences.DisplayFramerate = pending_fps;
				gGlobalPreferences.ViewportType = pending_viewport;
				gGlobalPreferences.GuiColor = (EGuiColor)(s_themeIdx % NUM_COLOR_TYPES);
				CPreferences::Get()->Commit();
			}
			SaveFrontendConfig();
			if (applyRuntime)
			{
				gSpeedSyncEnabled = prefs.SpeedSyncEnabled;
				gVideoRateMatch = g_ROM.settings.VideoRateMatch || prefs.VideoRateMatch;
				static const u32 thf_frames[NUM_THF] = { 0, 1, 2, 4, 8, 16, 32 };
				gCheckTextureHashFrequency = thf_frames[prefs.CheckTextureHashFrequency];
				gFrameskipValue = prefs.Frameskip;
				gZoomX = prefs.ZoomX;
				gAudioPluginEnabled = prefs.AudioEnabled;
				gDynarecCycleMultiplier = (prefs.DynarecCycleMultiplier >= 3) ? 3u :
					((prefs.DynarecCycleMultiplier == 2) ? 2u : 1u);
			}
			break;
		}

		if (Pressed(rep, down, held, KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN, 0))
		{
			selection = (selection + 1) % kNumOptions;
			manualScroll = false;
		}
		if (Pressed(rep, down, held, KEY_UP | KEY_DUP | KEY_CPAD_UP, 1))
		{
			selection = (selection + kNumOptions - 1) % kNumOptions;
			manualScroll = false;
		}

		if (manualScroll)
		{
			if (!touch.active)
			{
				scrollPos += scrollVel * (1.0f / 60.0f);
				scrollVel *= 0.86f;
				if (fabsf(scrollVel) < 0.05f) scrollVel = 0.0f;
			}
			if (scrollPos < 0.0f) { scrollPos = 0.0f; scrollVel = 0.0f; }
			if (scrollPos > MAX_SCROLL) { scrollPos = MAX_SCROLL; scrollVel = 0.0f; }
		}
		else
		{
			float targetScroll = scrollPos;
			if ((float)selection < targetScroll) targetScroll = (float)selection;
			if ((float)selection >= targetScroll + VISIBLE) targetScroll = (float)(selection - VISIBLE + 1);
			if (targetScroll < 0.0f) targetScroll = 0.0f;
			if (targetScroll > MAX_SCROLL) targetScroll = MAX_SCROLL;
			SpringStep(scrollPos, scrollVel, targetScroll, 220.0f, 24.0f);
			if (scrollPos < 0.0f) scrollPos = 0.0f;
			if (scrollPos > MAX_SCROLL) scrollPos = MAX_SCROLL;
		}

		const int dir = (down & (KEY_A | KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_R)) ? 1 :
						(down & (KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT | KEY_L)) ? -1 : 0;
		if (dir != 0)
		{
			switch (selection)
			{
			case 0: prefs.DynarecEnabled = !prefs.DynarecEnabled; break;
			case 1: prefs.DynarecLoopOptimisation = !prefs.DynarecLoopOptimisation; break;
			case 2: prefs.DynarecDoublesOptimisation = !prefs.DynarecDoublesOptimisation; break;
			case 3: prefs.MemoryAccessOptimisation = !prefs.MemoryAccessOptimisation; break;
			case 4: prefs.PatchesEnabled = !prefs.PatchesEnabled; break;
			case 5: prefs.SpeedSyncEnabled = prefs.SpeedSyncEnabled ? 0u : 1u; break;
			case 6: prefs.DoubleDisplayEnabled = !prefs.DoubleDisplayEnabled; break;
			case 7: prefs.CleanSceneEnabled = !prefs.CleanSceneEnabled; break;
			case 8: prefs.ClearDepthFrameBuffer = !prefs.ClearDepthFrameBuffer; break;
			case 9: prefs.Frameskip = (EFrameskipValue)((prefs.Frameskip + dir + NUM_FRAMESKIP_VALUES) % NUM_FRAMESKIP_VALUES); break;
			case 10: prefs.CheckTextureHashFrequency = (ETextureHashFrequency)((prefs.CheckTextureHashFrequency + dir + NUM_THF) % NUM_THF); break;
			case 11: prefs.VideoRateMatch = !prefs.VideoRateMatch; break;
			case 12: prefs.FogEnabled = !prefs.FogEnabled; break;
			case 13: pending_viewport = (EViewportType)((pending_viewport + dir + 4) % 4); break;
			case 14:
				prefs.ZoomX += dir * 0.1f;
				if (prefs.ZoomX < 0.5f) prefs.ZoomX = 0.5f;
				if (prefs.ZoomX > 2.0f) prefs.ZoomX = 2.0f;
				break;
			case 15: prefs.AudioEnabled = (EAudioPluginMode)((prefs.AudioEnabled + dir + 3) % 3); break;
			case 16: prefs.AudioRateMatch = !prefs.AudioRateMatch; break;
			case 17: prefs.CheatsEnabled = !prefs.CheatsEnabled; break;
			case 18: pending_fps = !pending_fps; break;
			case 19: s_themeIdx = (s_themeIdx + dir + kThemeCount) % kThemeCount; break;
			case 20:
			{
				int cur = (prefs.DynarecCycleMultiplier >= 3) ? 3 : ((prefs.DynarecCycleMultiplier == 2) ? 2 : 1);
				cur = ((cur - 1 + dir + 3) % 3) + 1;
				prefs.DynarecCycleMultiplier = (u32)cur;
				break;
			}
			}
		}

		SpringStep(hl, hl_v, (float)selection, 260.0f, 24.0f);

		const Theme & t = ActiveTheme();
		BeginFrame();

		// ---- Bottom screen
		BeginScreen(GFX_BOTTOM, 320, 240);
		char optHeaderRight[24];
		snprintf(optHeaderRight, sizeof(optHeaderRight), "%d / %d", selection + 1, kNumOptions);
		DrawHeader(320.0f, "ROM Options", optHeaderRight);

		// Spring highlight bar
		const float hy = LIST_Y + (hl - scrollPos) * ROW_H;
		if (hy >= LIST_Y - ROW_H && hy <= 204.0f)
		{
			Quad(4.0f, hy - 1.0f, 304.0f, ROW_H, t.hlGlow);
			QuadVGrad(6.0f, hy, 300.0f, ROW_H - 2.0f, t.hlTop, t.hlBottom);
			Quad(6.0f, hy, 3.0f, ROW_H - 2.0f, t.accent[3]);
		}

		char value[48];
		int firstRow = (int)floorf(scrollPos);
		if (firstRow < 0) firstRow = 0;
		for (int idx = firstRow; idx < kNumOptions && idx <= firstRow + VISIBLE; ++idx)
		{
			const float y = LIST_Y + ((float)idx - scrollPos) * ROW_H;
			if (y < LIST_Y - 4.0f || y > LIST_Y + (VISIBLE - 1) * ROW_H + 6.0f)
				continue;
			const bool sel = (idx == selection);
			const bool pressed = (idx == pressedRow && !sel);
			if (pressed)
				Quad(6.0f, y, 300.0f, ROW_H - 2.0f, WithAlpha(t.hlTop, 110));
			else if (!sel)
				Quad(10.0f, y + ROW_H - 3.0f, 292.0f, 1.0f, WithAlpha(t.rule, 110));

			DrawText(sel ? 16.0f : 14.0f, y + 9.0f, kOptions[idx].label, sel ? t.text : t.textDim, 1.0f);

			value[0] = 0;
			bool isOn = false;
			bool isBool = false;
			switch (idx)
			{
			case 0: isBool = true; isOn = prefs.DynarecEnabled; break;
			case 1: isBool = true; isOn = prefs.DynarecLoopOptimisation; break;
			case 2: isBool = true; isOn = prefs.DynarecDoublesOptimisation; break;
			case 3: isBool = true; isOn = prefs.MemoryAccessOptimisation; break;
			case 4: isBool = true; isOn = prefs.PatchesEnabled; break;
			case 5: isBool = true; isOn = prefs.SpeedSyncEnabled != 0; break;
			case 6: isBool = true; isOn = prefs.DoubleDisplayEnabled; break;
			case 7: isBool = true; isOn = prefs.CleanSceneEnabled; break;
			case 8: isBool = true; isOn = prefs.ClearDepthFrameBuffer; break;
			case 9: snprintf(value, sizeof(value), "%s", FrameskipName(prefs.Frameskip)); break;
			case 10: snprintf(value, sizeof(value), "%s", TexHashName(prefs.CheckTextureHashFrequency)); break;
			case 11: isBool = true; isOn = prefs.VideoRateMatch; break;
			case 12: isBool = true; isOn = prefs.FogEnabled; break;
			case 13: snprintf(value, sizeof(value), "%s", ViewportName(pending_viewport)); break;
			case 14: snprintf(value, sizeof(value), "%.1fx", prefs.ZoomX); break;
			case 15: snprintf(value, sizeof(value), "%s", AudioName(prefs.AudioEnabled)); break;
			case 16: isBool = true; isOn = prefs.AudioRateMatch; break;
			case 17: isBool = true; isOn = prefs.CheatsEnabled; break;
			case 18: isBool = true; isOn = pending_fps; break;
			case 19: snprintf(value, sizeof(value), "%s", t.name); break;
			case 20: snprintf(value, sizeof(value),
				prefs.DynarecCycleMultiplier >= 3 ? "3x Turbo" :
				(prefs.DynarecCycleMultiplier == 2 ? "2x Fast" : "1x Normal")); break;
			}
			if (isBool)
			{
				const char * lbl = isOn ? "ON" : "OFF";
				DrawPill(254.0f, y + 4.0f, 46.0f, 18.0f, lbl,
					isOn ? WithAlpha(t.accent[2], sel ? 95 : 55) : WithAlpha(t.bg, 190),
					isOn ? t.accent[2] : t.rule,
					isOn ? t.text : t.textDim, 0.75f);
			}
			else if (value[0])
			{
				const float pw = fmaxf(58.0f, TextWidth(value, 0.75f) + 14.0f);
				DrawPill(300.0f - pw, y + 4.0f, pw, 18.0f, value,
					WithAlpha(sel ? t.hlBottom : t.bg, 210),
					sel ? t.accent[1] : t.rule,
					sel ? t.accent[1] : t.value, 0.75f);
			}
		}

		// Touch scrollbar on right edge
		{
			const float trackH = VISIBLE * ROW_H - 4.0f;
			Quad(312.0f, LIST_Y, 4.0f, trackH, WithAlpha(t.rule, 90));
			const float thumbH = fmaxf(18.0f, trackH * (float)VISIBLE / (float)kNumOptions);
			const float frac = MAX_SCROLL > 0.0f ? (scrollPos / MAX_SCROLL) : 0.0f;
			Quad(312.0f, LIST_Y + (trackH - thumbH) * frac, 4.0f, thumbH,
				touch.scrollbarDrag ? t.accent[1] : t.value);
		}

		DrawButtonBar(kOptBar, 4, pressedBar);

		// ---- Top screen (PSX3D help panel)
		BeginScreen(GFX_TOP, 400, 240);
		DrawSynthwaveBackdrop(clock_s);
		DrawHeader(400.0f, "Configuration", t.name);

		Quad(14.0f, 136.0f, 372.0f, 90.0f, WithAlpha(t.bg, 225));
		Quad(14.0f, 136.0f, 3.0f, 90.0f, t.accent[3]);
		DrawTextShadow(26.0f, 146.0f, kOptions[selection].label, t.text, WithAlpha(t.bg, 220), 1.125f);
		DrawTextWrapped(26.0f, 168.0f, 348.0f, 13.0f, kOptions[selection].help, t.textDim, 0.875f);

		DrawRightText(384.0f, 212.0f, s_memSummary, t.value, 0.75f);

		PresentBothScreens();
	}

	UI::RestoreRenderState();
}

bool Initialise()
{
	if (s_initialised) return true;
	LoadFrontendConfig();
	if (!EnsureGpuResources())
		return false;
	s_initialised = true;
	return true;
}

void Shutdown()
{
	ReleasePreview();
	FreeGpuResources();
	s_romCount = 0;
	s_viewCount = 0;
	s_romsScanned = false;
	s_initialised = false;
}

void ReleaseAssets()
{
	// Release preview texture and UI vertex pool before emulation so Old 3DS
	// keeps all linear RAM and VRAM available for N64 textures.
	ReleasePreview();
	if (s_vtxPool) { linearFree(s_vtxPool); s_vtxPool = nullptr; }
	s_frameBank = 0;
	s_vtxOffset = 0;
	s_batchStart = 0;
	s_batchCount = 0;
	s_batchTex = nullptr;
}

bool IsMenuOpen()
{
	return false;
}

std::string RunRomSelector()
{
	if (!EnsureGpuResources())
		return std::string();

	ScanRoms(false);
	RefreshMemorySummary();

	const int ROWS_VISIBLE = 7;
	const float ROW_H = 26.0f;
	const float LIST_Y = 28.0f;

	Repeat rep = {};
	TouchState touch;
	touch.Reset();
	bool manualScroll = false;
	float clock_s = 0.0f;
	float pop = 1.0f, popVel = 0.0f;
	float titleT = 1.0f;
	int prevSel = s_selected;
	char toast[64] = "";
	int toastFrames = 0;

	static const BarBtn kLibBar[5] = {
		{ "START", "Exit",    false },
		{ "SEL",   "Sort",    false },
		{ "X",     "Options", false },
		{ "Y",     "Fav",     false },
		{ "A",     "Play",    true  }
	};

	if (s_viewCount > 0)
		RequestPreviewForRom(s_viewOrder[s_selected], true);

	while (aptMainLoop() && !shouldQuit)
	{
		// C3D_FrameEnd syncs to VBlank; avoid double gspWaitForVBlank() stall.
		hidScanInput();
		u32 down = hidKeysDown();
		const u32 held = hidKeysHeld();
		clock_s += 1.0f / 60.0f;
		touch.Update(down, held);

		const float maxScroll = (s_viewCount > ROWS_VISIBLE) ? (float)(s_viewCount - ROWS_VISIBLE) : 0.0f;
		int pressedBar = -1;
		int pressedRow = -1;
		bool pressedSortPill = false;
		bool pressedThemePill = false;

		if (touch.active)
		{
			if (!touch.dragged)
			{
				pressedBar = HitButtonBar(touch.curX, touch.curY, 5);
				if (touch.curY >= 2 && touch.curY < 24)
				{
					if (touch.curX >= 166 && touch.curX < 244) pressedSortPill = true;
					else if (touch.curX >= 248 && touch.curX < 316) pressedThemePill = true;
				}
			}

			if (s_viewCount > ROWS_VISIBLE && touch.justPressed &&
				touch.curX >= 294 && touch.curY >= (int)LIST_Y && touch.curY < 210)
			{
				touch.scrollbarDrag = true;
			}

			if (touch.scrollbarDrag && maxScroll > 0.0f)
			{
				float frac = ((float)touch.curY - LIST_Y) / (ROWS_VISIBLE * ROW_H);
				if (frac < 0.0f) frac = 0.0f;
				if (frac > 1.0f) frac = 1.0f;
				s_scrollPos = frac * maxScroll;
				s_scrollVel = 0.0f;
				manualScroll = true;
			}
			else if (touch.dragged && s_viewCount > ROWS_VISIBLE &&
				touch.startY >= (int)LIST_Y && touch.startY < 210)
			{
				s_scrollPos -= touch.deltaY / ROW_H;
				if (s_scrollPos < 0.0f) s_scrollPos = 0.0f;
				if (s_scrollPos > maxScroll) s_scrollPos = maxScroll;
				s_scrollVel = 0.0f;
				manualScroll = true;
			}
			else if (!touch.dragged && s_viewCount > 0 &&
				touch.curY >= (int)LIST_Y && touch.curY < (int)(LIST_Y + ROWS_VISIBLE * ROW_H))
			{
				const int rIdx = (int)floorf(s_scrollPos + ((float)touch.curY - LIST_Y) / ROW_H);
				if (rIdx >= 0 && rIdx < s_viewCount)
					pressedRow = rIdx;
			}
		}

		if (touch.justReleased)
		{
			if (touch.dragged && !touch.scrollbarDrag && s_viewCount > ROWS_VISIBLE &&
				touch.startY >= (int)LIST_Y && touch.startY < 210)
			{
				s_scrollVel = -(touch.velY / ROW_H) * 60.0f * 0.45f;
				manualScroll = true;
			}
			else if (!touch.dragged && !touch.scrollbarDrag)
			{
				const int barHit = HitButtonBar(touch.releaseX, touch.releaseY, 5);
				if (barHit == 0) down |= KEY_START;
				else if (barHit == 1) down |= KEY_SELECT;
				else if (barHit == 2) down |= KEY_X;
				else if (barHit == 3) down |= KEY_Y;
				else if (barHit == 4) down |= KEY_A;
				else if (touch.releaseY >= 2 && touch.releaseY < 24)
				{
					if (touch.releaseX >= 166 && touch.releaseX < 244) down |= KEY_SELECT;
					else if (touch.releaseX >= 248 && touch.releaseX < 316) down |= KEY_R;
				}
				else if (s_viewCount > 0 && touch.releaseY >= (int)LIST_Y && touch.releaseY < (int)(LIST_Y + ROWS_VISIBLE * ROW_H))
				{
					const int idx = (int)floorf(s_scrollPos + ((float)touch.releaseY - LIST_Y) / ROW_H);
					if (idx >= 0 && idx < s_viewCount)
					{
						if (idx == s_selected) down |= KEY_A;
						s_selected = idx;
						manualScroll = false;
					}
				}
			}
		}

		if (down & KEY_START)
		{
			shouldQuit = true;
			break;
		}

		if (down & KEY_SELECT)
		{
			s_sortMode = (s_sortMode + 1) % SORT_COUNT;
			RebuildView();
			SaveFrontendConfig();
			manualScroll = false;
			snprintf(toast, sizeof(toast), "View: %s", kSortNames[s_sortMode]);
			toastFrames = 80;
		}

		if (down & KEY_L)
		{
			s_themeIdx = (s_themeIdx + kThemeCount - 1) % kThemeCount;
			SaveFrontendConfig();
			snprintf(toast, sizeof(toast), "Theme: %s", ActiveTheme().name);
			toastFrames = 75;
		}
		if (down & KEY_R)
		{
			s_themeIdx = (s_themeIdx + 1) % kThemeCount;
			SaveFrontendConfig();
			snprintf(toast, sizeof(toast), "Theme: %s", ActiveTheme().name);
			toastFrames = 75;
		}

		if (s_viewCount > 0)
		{
			if (Pressed(rep, down, held, KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN, 0))
			{
				s_selected = (s_selected + 1) % s_viewCount;
				manualScroll = false;
			}
			if (Pressed(rep, down, held, KEY_UP | KEY_DUP | KEY_CPAD_UP, 1))
			{
				s_selected = (s_selected + s_viewCount - 1) % s_viewCount;
				manualScroll = false;
			}
			if (Pressed(rep, down, held, KEY_RIGHT | KEY_DRIGHT, 2))
			{
				s_selected += ROWS_VISIBLE;
				if (s_selected >= s_viewCount) s_selected = s_viewCount - 1;
				manualScroll = false;
			}
			if (Pressed(rep, down, held, KEY_LEFT | KEY_DLEFT, 3))
			{
				s_selected -= ROWS_VISIBLE;
				if (s_selected < 0) s_selected = 0;
				manualScroll = false;
			}

			if (down & KEY_Y)
			{
				FrontendRomInfo & cur = s_roms[s_viewOrder[s_selected]];
				cur.favorite = !cur.favorite;
				SaveFrontendConfig();
				snprintf(toast, sizeof(toast), cur.favorite ? "Added to favorites" : "Removed from favorites");
				toastFrames = 75;
				if (s_sortMode == SORT_FAV)
					RebuildView();
			}

			if ((down & KEY_X) && s_viewCount > 0)
			{
				const FrontendRomInfo & cur = s_roms[s_viewOrder[s_selected]];
				if (cur.hasDetails)
				{
					RunOptionsPage(cur.romId, false);
					RefreshMemorySummary();
					touch.Reset();
				}
			}

			if (s_viewCount > 0)
				RequestPreviewForRom(s_viewOrder[s_selected], false);
		}

		TickPreviewLoader();

		const bool launch = (down & KEY_A) && s_viewCount > 0;

		if (s_selected != prevSel)
		{
			prevSel = s_selected;
			pop = 0.90f;
			popVel = 2.6f;
			titleT = 0.0f;
		}
		SpringStep(pop, popVel, 1.0f, 240.0f, 18.0f);
		if (titleT < 1.0f)
		{
			titleT += 1.0f / 14.0f;
			if (titleT > 1.0f) titleT = 1.0f;
		}

		SpringStep(s_hlPos, s_hlVel, (float)s_selected, 260.0f, 24.0f);

		if (manualScroll)
		{
			if (!touch.active)
			{
				s_scrollPos += s_scrollVel * (1.0f / 60.0f);
				s_scrollVel *= 0.86f;
				if (fabsf(s_scrollVel) < 0.05f) s_scrollVel = 0.0f;
			}
			if (s_scrollPos < 0.0f) { s_scrollPos = 0.0f; s_scrollVel = 0.0f; }
			if (s_scrollPos > maxScroll) { s_scrollPos = maxScroll; s_scrollVel = 0.0f; }
		}
		else
		{
			int targetTop = (int)floorf(s_scrollPos + 0.5f);
			if (s_selected < targetTop + 1) targetTop = s_selected - 1;
			if (s_selected > targetTop + ROWS_VISIBLE - 2) targetTop = s_selected - ROWS_VISIBLE + 2;
			if (targetTop > s_viewCount - ROWS_VISIBLE) targetTop = s_viewCount - ROWS_VISIBLE;
			if (targetTop < 0) targetTop = 0;
			SpringStep(s_scrollPos, s_scrollVel, (float)targetTop, 200.0f, 24.0f);
			if (s_scrollPos < 0.0f) s_scrollPos = 0.0f;
			if (s_scrollPos > maxScroll) s_scrollPos = maxScroll;
		}

		const Theme & t = ActiveTheme();
		BeginFrame();

		// ---- Bottom screen: touch-friendly toolbar + 7-row scrollable ROM list + action bar
		BeginScreen(GFX_BOTTOM, 320, 240);

		// Top compact touch toolbar
		Quad(0, 0, 320.0f, 26.0f, t.panel);
		Quad(0, 25.0f, 320.0f, 1.0f, t.rule);
		char libLabel[32];
		snprintf(libLabel, sizeof(libLabel), "ROMS (%d)", s_viewCount);
		DrawTextShadow(8.0f, 9.0f, libLabel, t.text, WithAlpha(t.bg, 220), 0.875f);

		char sortPillLabel[24];
		snprintf(sortPillLabel, sizeof(sortPillLabel), "Sort:%s", kSortNames[s_sortMode]);
		DrawPill(166.0f, 4.0f, 78.0f, 18.0f, sortPillLabel,
			pressedSortPill ? t.hlTop : WithAlpha(t.bg, 210),
			pressedSortPill ? t.accent[1] : t.rule,
			pressedSortPill ? t.text : t.value, 0.75f);
		DrawPill(248.0f, 4.0f, 66.0f, 18.0f, "Theme",
			pressedThemePill ? t.hlTop : WithAlpha(t.bg, 210),
			pressedThemePill ? t.accent[1] : t.rule,
			pressedThemePill ? t.text : t.textDim, 0.75f);

		if (s_viewCount == 0)
		{
			QuadVGrad(24.0f, 64.0f, 272.0f, 108.0f, t.cardTop, t.cardBottom);
			Quad(24.0f, 64.0f, 3.0f, 108.0f, t.accent[0]);
			DrawCenteredText(160.0f, 84.0f, "No N64 ROMs found", t.text, 1.125f);
			DrawCenteredText(160.0f, 110.0f, "Copy .z64/.n64/.v64 files to:", t.textDim, 0.875f);
			DrawCenteredText(160.0f, 128.0f, "sdmc:/3ds/DaedalusX64/Roms", t.value, 0.875f);
			DrawCenteredText(160.0f, 150.0f, "Tap Theme or L/R to switch theme", t.textDim, 0.75f);
		}
		else
		{
			// Spring-animated selection highlight
			const float hy = LIST_Y + (s_hlPos - s_scrollPos) * ROW_H;
			if (hy > LIST_Y - ROW_H && hy < LIST_Y + ROWS_VISIBLE * ROW_H)
			{
				Quad(4.0f, hy - 1.0f, 304.0f, ROW_H + 1.0f, t.hlGlow);
				QuadVGrad(6.0f, hy, 300.0f, ROW_H - 1.0f, t.hlTop, t.hlBottom);
				Quad(6.0f, hy, 3.0f, ROW_H - 1.0f, t.accent[s_selected & 3]);
			}

			int first = (int)floorf(s_scrollPos);
			if (first < 0) first = 0;
			for (int i = first; i < s_viewCount && i <= first + ROWS_VISIBLE; ++i)
			{
				const float ry = LIST_Y + (i - s_scrollPos) * ROW_H;
				if (ry < LIST_Y - 4.0f || ry > LIST_Y + (ROWS_VISIBLE - 1) * ROW_H + 6.0f)
					continue;
				const bool sel = (i == s_selected);
				const bool pressed = (i == pressedRow && !sel);
				const FrontendRomInfo & r = s_roms[s_viewOrder[i]];

				if (pressed)
					Quad(6.0f, ry, 300.0f, ROW_H - 1.0f, WithAlpha(t.hlTop, 110));
				else if (!sel)
					Quad(10.0f, ry + ROW_H - 2.0f, 292.0f, 1.0f, WithAlpha(t.rule, 90));

				const float textX = sel ? 18.0f : 14.0f;
				DrawTextEllipsis(textX, ry + 9.0f,
					r.favorite ? 232.0f : 246.0f, r.title, sel ? t.text : t.textDim, 1.0f);

				if (r.favorite)
					DrawRightText(272.0f, ry + 9.0f, "*", t.accent[1], 1.0f);
				DrawRightText(302.0f, ry + 10.0f, r.country, sel ? t.accent[1] : t.textDim, 0.75f);
			}

			// Interactive touch scrollbar at x = 311
			if (s_viewCount > ROWS_VISIBLE)
			{
				const float trackH = ROWS_VISIBLE * ROW_H - 4.0f;
				Quad(311.0f, LIST_Y + 2.0f, 4.0f, trackH, WithAlpha(t.rule, 85));
				float thumbH = trackH * (float)ROWS_VISIBLE / (float)s_viewCount;
				if (thumbH < 18.0f) thumbH = 18.0f;
				float frac = maxScroll > 0.0f ? (s_scrollPos / maxScroll) : 0.0f;
				if (frac < 0.0f) frac = 0.0f;
				if (frac > 1.0f) frac = 1.0f;
				Quad(311.0f, LIST_Y + 2.0f + (trackH - thumbH) * frac, 4.0f, thumbH,
					touch.scrollbarDrag ? t.accent[1] : t.value);
			}
		}

		if (toastFrames > 0)
		{
			Quad(40.0f, 182.0f, 240.0f, 22.0f, WithAlpha(t.bg, 235));
			Quad(40.0f, 182.0f, 3.0f, 22.0f, t.accent[1]);
			DrawCenteredText(160.0f, 189.0f, toast, t.accent[1], 0.875f);
			--toastFrames;
		}

		DrawButtonBar(kLibBar, 5, pressedBar);

		// ---- Top screen: authentic PSX3D synthwave backdrop + 3D-feel cover carousel
		BeginScreen(GFX_TOP, 400, 240);
		DrawSynthwaveBackdrop(clock_s);

		// Subtle PSX3D wordmark top-left
		DrawTextShadow(10.0f, 7.0f, "DAEDALUS", WithAlpha(t.text, 220), WithAlpha(t.bg, 180), 1.0f);
		DrawTextShadow(78.0f, 7.0f, "X64", WithAlpha(t.accent[0], 235), WithAlpha(t.bg, 180), 1.0f);

		// Subtle view/system status top-right
		char topRight[40];
		snprintf(topRight, sizeof(topRight), "%s | %s",
			kSortNames[s_sortMode], isN3DS ? "N3DS" : "O3DS");
		DrawRightText(392.0f, 8.0f, topRight, WithAlpha(t.textDim, 210), 0.75f);

		if (s_viewCount > 0)
		{
			const float spacing = 114.0f;
			const float bob = sinf(clock_s * 2.0f) * 2.0f;
			const int centerIdx = (int)floorf(s_hlPos + 0.5f);

			// Draw neighbours back-to-front so the selected cover sits cleanly on top
			for (int k = 3; k >= 0; --k)
			{
				for (int side = -1; side <= 1; side += 2)
				{
					if (k == 0 && side == 1)
						continue;
					const int idx = centerIdx + k * side;
					if (idx < 0 || idx >= s_viewCount)
						continue;

					const float d = (float)idx - s_hlPos;
					const float ad = fabsf(d);
					float size = (ad < 1.0f) ? (122.0f - 40.0f * ad) : (82.0f - 8.0f * (ad - 1.0f));
					if (size < 48.0f) size = 48.0f;
					float cy = 92.0f;
					if (idx == s_selected)
					{
						size *= pop;
						cy += bob;
					}
					const float cx = 200.0f + d * spacing;
					float dim = (ad < 1.0f) ? (0.55f * ad) : (0.55f + 0.12f * (ad - 1.0f));
					if (dim > 0.85f) dim = 0.85f;

					const float x = cx - size * 0.5f;
					const float y = cy - size * 0.5f;
					const FrontendRomInfo & item = s_roms[s_viewOrder[idx]];

					// Soft drop shadow under cover card
					Quad(x + 4.0f, y + 6.0f, size, size, RGBA(0, 0, 0, (u8)(120 * (1.0f - dim * 0.6f))));

					if (idx == s_selected && s_previewTex != nullptr && s_previewTex->HasData())
					{
						// Framed box art preview
						Quad(x - 2.0f, y - 2.0f, size + 4.0f, size + 4.0f, WithAlpha(t.accent[idx & 3], 220));
						Quad(x, y, size, size, t.cardBottom);
						const float pw = (float)s_previewTex->GetWidth();
						const float ph = (float)s_previewTex->GetHeight();
						const float u1 = pw * s_previewTex->GetScaleX();
						const float v1 = ph * s_previewTex->GetScaleY();
						const float sc = fminf((size - 4.0f) / pw, (size - 4.0f) / ph);
						const float dw = pw * sc;
						const float dh = ph * sc;
						DrawTexturedQuad(s_previewTex->GetC3DTexture(),
							cx - dw * 0.5f, cy - dh * 0.5f, dw, dh, 0.0f, 0.0f, u1, v1);
					}
					else
					{
						// PSX3D cover placeholder card: gradient body, thin border, bold initial & bottom accent stripe
						Quad(x - 1.0f, y - 1.0f, size + 2.0f, size + 2.0f, DimColor(t.rule, dim));
						QuadVGrad(x, y, size, size, DimColor(t.cardTop, dim), DimColor(t.cardBottom, dim));
						Quad(x, y + size - 3.0f, size, 3.0f, DimColor(t.accent[idx & 3], dim));

						char initial[2] = { item.title[0] ? item.title[0] : 'N', 0 };
						const float initScale = (idx == s_selected) ? 3.0f : 2.0f;
						DrawCenteredText(cx, cy - initScale * 4.5f, initial,
							DimColor(t.text, dim), initScale);
						if (idx == s_selected)
							DrawCenteredText(cx, y + size - 18.0f, item.country, t.value, 0.875f);
					}

					if (item.favorite)
					{
						const u8 fa = (u8)(245 * (1.0f - dim * 0.7f));
						Quad(x + size - 16.0f, y + 4.0f, 12.0f, 12.0f, WithAlpha(t.bg, (u8)(195 * (1.0f - dim))));
						DrawCenteredText(x + size - 10.0f, y + 6.0f, "*", WithAlpha(t.accent[1], fa), 1.0f);
					}
				}
			}

			// Floating centered title + subtitle below carousel (PSX3D slide-up ease)
			const FrontendRomInfo & r = s_roms[s_viewOrder[s_selected]];
			const float ease = 1.0f - (1.0f - titleT) * (1.0f - titleT);
			const u8 ta = (u8)(255.0f * ease);
			const float ty = 172.0f + (1.0f - ease) * 8.0f;
			float titleScale = 1.25f;
			if (TextWidth(r.title, titleScale) > 372.0f) titleScale = 1.0f;
			if (TextWidth(r.title, titleScale) > 372.0f) titleScale = 0.875f;
			DrawCenteredText(201.0f, ty + 1.0f, r.title, RGBA(0, 0, 0, (u8)(ta * 5 / 8)), titleScale);
			DrawCenteredText(200.0f, ty, r.title, WithAlpha(t.text, ta), titleScale);

			char subLine[72];
			snprintf(subLine, sizeof(subLine), "%s  |  %.1f MB%s",
				r.country, r.size / (1024.0f * 1024.0f),
				r.favorite ? "  |  Favorite" : "");
			DrawCenteredText(200.0f, ty + 20.0f, subLine, WithAlpha(t.value, ta), 0.875f);
		}
		else
		{
			DrawCenteredText(200.0f, 92.0f, "DAEDALUS X64", t.text, 2.0f);
			DrawCenteredText(200.0f, 126.0f, "Nintendo 64 for Old & New 3DS", t.value, 0.875f);
		}

		// Quiet corner footer (PSX3D version_ink style)
		DrawText(8.0f, 226.0f, "L/R: Theme", WithAlpha(t.textDim, 165), 0.75f);
		DrawRightText(394.0f, 226.0f, s_memSummary, WithAlpha(t.textDim, 175), 0.75f);

		PresentBothScreens();

		if (launch)
		{
			const FrontendRomInfo & chosen = s_roms[s_viewOrder[s_selected]];
			snprintf(s_lastSelectedFile, sizeof(s_lastSelectedFile), "%s", chosen.filename);
			SaveFrontendConfig();
			return std::string(chosen.path);
		}
	}

	return std::string();
}

void RunPauseMenu(const char * gameName, const RomID & romId)
{
	if (!EnsureGpuResources())
	{
		UI::RestoreRenderState();
		return;
	}

	enum Page { PAGE_MAIN, PAGE_SLOTS, PAGE_CLOSE };
	static const char * main_items[] = { "Resume game", "Save state", "Load state", "ROM options", "Switch theme", "Close ROM" };
	static const int kMainCount = 6;

	int page = PAGE_MAIN;
	int selection = 0;
	int slot = 0;
	int close_sel = 1;
	float hl = 0.0f, hl_v = 0.0f;
	float clock_s = 0.0f;
	Repeat rep = {};

	// Check which save state slots exist once on entry (never inside draw loop)
	bool slotExists[5] = {};
	auto RefreshSlots = [&]() {
		for (int i = 0; i < 5; ++i)
		{
			IO::Filename check;
			snprintf(check, sizeof(check), "%s%s.ss%d",
				DAEDALUS_CTR_PATH("SaveStates/"), gameName ? gameName : "rom", i);
			slotExists[i] = IO::File::Exists(check);
		}
	};
	RefreshSlots();
	RefreshMemorySummary();

	static const BarBtn kPauseBar[2] = {
		{ "B", "Back",   false },
		{ "A", "Select", true  }
	};

	TouchState touch;
	touch.Reset();

	// SELECT opened the pause menu; wait for physical release first
	while (aptMainLoop() && (hidKeysHeld() & KEY_SELECT))
	{
		gspWaitForVBlank();
		hidScanInput();
	}

	bool exitMenu = false;
	while (aptMainLoop() && !shouldQuit && !exitMenu)
	{
		// C3D_FrameEnd syncs to VBlank; avoid double gspWaitForVBlank() stall.
		hidScanInput();
		u32 down = hidKeysDown();
		const u32 held = hidKeysHeld();
		clock_s += 1.0f / 60.0f;
		touch.Update(down, held);

		int pressedBar = -1;
		int pressedItem = -1;
		if (touch.active && !touch.dragged)
		{
			pressedBar = HitButtonBar(touch.curX, touch.curY, 2);
			if (pressedBar < 0)
			{
				if (page == PAGE_MAIN && touch.curY >= 38 && touch.curY < 38 + kMainCount * 28)
					pressedItem = (touch.curY - 38) / 28;
				else if (page == PAGE_SLOTS && touch.curY >= 42 && touch.curY < 42 + 5 * 32)
					pressedItem = (touch.curY - 42) / 32;
				else if (page == PAGE_CLOSE && touch.curY >= 72 && touch.curY < 72 + 2 * 38)
					pressedItem = (touch.curY - 72) / 38;
			}
		}

		if (touch.justReleased && !touch.dragged)
		{
			const int barHit = HitButtonBar(touch.releaseX, touch.releaseY, 2);
			if (barHit == 0) down |= KEY_B;
			else if (barHit == 1) down |= KEY_A;
			else if (page == PAGE_MAIN && touch.releaseY >= 38 && touch.releaseY < 38 + kMainCount * 28)
			{
				const int row = (touch.releaseY - 38) / 28;
				if (row >= 0 && row < kMainCount)
				{
					selection = row;
					down |= KEY_A; // clean single-tap activation on pause menu items
				}
			}
			else if (page == PAGE_SLOTS && touch.releaseY >= 42 && touch.releaseY < 42 + 5 * 32)
			{
				const int row = (touch.releaseY - 42) / 32;
				if (row >= 0 && row < 5)
				{
					slot = row;
					down |= KEY_A;
				}
			}
			else if (page == PAGE_CLOSE && touch.releaseY >= 72 && touch.releaseY < 72 + 2 * 38)
			{
				const int row = (touch.releaseY - 72) / 38;
				if (row >= 0 && row < 2)
				{
					close_sel = row;
					down |= KEY_A;
				}
			}
		}

		if (page == PAGE_MAIN)
		{
			if (down & (KEY_B | KEY_START | KEY_SELECT)) { exitMenu = true; break; }
			if (Pressed(rep, down, held, KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN, 0))
				selection = (selection + 1) % kMainCount;
			if (Pressed(rep, down, held, KEY_UP | KEY_DUP | KEY_CPAD_UP, 1))
				selection = (selection + kMainCount - 1) % kMainCount;
			if (down & KEY_A)
			{
				switch (selection)
				{
				case 0: exitMenu = true; break;
				case 1:
				case 2:
					RefreshSlots();
					RefreshMemorySummary();
					page = PAGE_SLOTS;
					slot = 0;
					hl = 0.0f;
					break;
				case 3:
					RunOptionsPage(romId, true);
					RefreshMemorySummary();
					touch.Reset();
					break;
				case 4:
					s_themeIdx = (s_themeIdx + 1) % kThemeCount;
					SaveFrontendConfig();
					break;
				case 5:
					page = PAGE_CLOSE;
					close_sel = 1;
					hl = 1.0f;
					break;
				}
			}
		}
		else if (page == PAGE_SLOTS)
		{
			if (down & KEY_B) { page = PAGE_MAIN; hl = (float)selection; continue; }
			if (Pressed(rep, down, held, KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN, 0))
				slot = (slot + 1) % 5;
			if (Pressed(rep, down, held, KEY_UP | KEY_DUP | KEY_CPAD_UP, 1))
				slot = (slot + 4) % 5;
			if (down & KEY_A)
			{
				char path[256];
				snprintf(path, sizeof(path), "%s%s.ss%d",
					DAEDALUS_CTR_PATH("SaveStates/"), gameName ? gameName : "rom", slot);
				if (selection == 1)
				{
					CPU_RequestSaveState(path);
					exitMenu = true;
					break;
				}
				else if (slotExists[slot])
				{
					CPU_RequestLoadState(path);
					exitMenu = true;
					break;
				}
			}
		}
		else // PAGE_CLOSE
		{
			if (down & KEY_B) { page = PAGE_MAIN; hl = (float)selection; continue; }
			if (down & (KEY_UP | KEY_DOWN | KEY_DUP | KEY_DDOWN)) close_sel = 1 - close_sel;
			if (down & KEY_A)
			{
				if (close_sel == 0) CPU_Halt("Closed from menu");
				exitMenu = true;
				break;
			}
		}

		const float targetHl = (page == PAGE_MAIN) ? (float)selection :
							   (page == PAGE_SLOTS) ? (float)slot : (float)close_sel;
		SpringStep(hl, hl_v, targetHl, 260.0f, 24.0f);

		const Theme & t = ActiveTheme();
		BeginFrame();

		// ---- Bottom screen
		BeginScreen(GFX_BOTTOM, 320, 240);
		if (page == PAGE_MAIN)
		{
			DrawHeader(320.0f, "Paused", gameName ? gameName : "N64");
			const float hy = 38.0f + hl * 28.0f;
			Quad(8.0f, hy - 1.0f, 304.0f, 27.0f, t.hlGlow);
			QuadVGrad(10.0f, hy, 300.0f, 25.0f, t.hlTop, t.hlBottom);
			Quad(10.0f, hy, 3.0f, 25.0f, t.accent[selection & 3]);

			for (int i = 0; i < kMainCount; ++i)
			{
				const float y = 38.0f + i * 28.0f;
				const bool sel = (i == selection);
				const bool pressed = (i == pressedItem && !sel);
				if (pressed)
					Quad(10.0f, y, 300.0f, 25.0f, WithAlpha(t.hlTop, 115));
				else if (!sel)
					Quad(14.0f, y + 25.0f, 292.0f, 1.0f, WithAlpha(t.rule, 100));

				if (sel || pressed)
					DrawTextShadow(22.0f, y + 9.0f, main_items[i], t.text, WithAlpha(t.bg, 220), 1.0f);
				else
					DrawText(18.0f, y + 9.0f, main_items[i], t.textDim, 1.0f);
				if (i == 4)
					DrawRightText(300.0f, y + 10.0f, t.name, sel ? t.accent[1] : t.value, 0.75f);
			}
		}
		else if (page == PAGE_SLOTS)
		{
			DrawHeader(320.0f, selection == 1 ? "Save State" : "Load State", gameName ? gameName : "N64");
			const float hy = 42.0f + hl * 32.0f;
			Quad(8.0f, hy - 1.0f, 304.0f, 30.0f, t.hlGlow);
			QuadVGrad(10.0f, hy, 300.0f, 28.0f, t.hlTop, t.hlBottom);
			Quad(10.0f, hy, 3.0f, 28.0f, t.accent[2]);

			for (int i = 0; i < 5; ++i)
			{
				const float y = 42.0f + i * 32.0f;
				const bool sel = (i == slot);
				const bool pressed = (i == pressedItem && !sel);
				if (pressed)
					Quad(10.0f, y, 300.0f, 28.0f, WithAlpha(t.hlTop, 115));
				else if (!sel)
					Quad(14.0f, y + 28.0f, 292.0f, 1.0f, WithAlpha(t.rule, 100));

				char label[24];
				snprintf(label, sizeof(label), "Slot %d", i + 1);
				DrawText(sel ? 22.0f : 18.0f, y + 10.0f, label, (sel || pressed) ? t.text : t.textDim, 1.0f);
				DrawPill(242.0f, y + 5.0f, 58.0f, 18.0f,
					slotExists[i] ? "SAVED" : "EMPTY",
					slotExists[i] ? WithAlpha(t.accent[2], 70) : WithAlpha(t.bg, 180),
					slotExists[i] ? t.accent[2] : t.rule,
					slotExists[i] ? t.text : t.textDim, 0.75f);
			}
		}
		else
		{
			DrawHeader(320.0f, "Close ROM", "Confirm");
			DrawCenteredText(160.0f, 48.0f, "Unsaved progress will be lost.", t.textDim, 0.875f);
			const float hy = 72.0f + hl * 38.0f;
			Quad(14.0f, hy - 1.0f, 292.0f, 34.0f, t.hlGlow);
			QuadVGrad(16.0f, hy, 288.0f, 32.0f, t.hlTop, t.hlBottom);
			Quad(16.0f, hy, 3.0f, 32.0f, close_sel == 0 ? t.accent[0] : t.accent[2]);
			DrawText(28.0f, 84.0f, "Close and return to library",
				(close_sel == 0 || pressedItem == 0) ? t.text : t.textDim, 1.0f);
			DrawText(28.0f, 122.0f, "Back to game",
				(close_sel == 1 || pressedItem == 1) ? t.text : t.textDim, 1.0f);
		}
		DrawButtonBar(kPauseBar, 2, pressedBar);

		// ---- Top screen
		BeginScreen(GFX_TOP, 400, 240);
		DrawSynthwaveBackdrop(clock_s);
		DrawHeader(400.0f, "Emulation Paused", isN3DS ? "New 3DS" : "Old 3DS");

		Quad(24.0f, 64.0f, 352.0f, 96.0f, WithAlpha(t.bg, 215));
		Quad(24.0f, 64.0f, 3.0f, 96.0f, t.accent[1]);
		DrawTextEllipsis(38.0f, 78.0f, 324.0f, gameName ? gameName : "Nintendo 64", t.text, 1.25f);

		char stats[64];
		snprintf(stats, sizeof(stats), "FPS: %.1f  |  %s",
			gCurrentFramerate, gDynarecAvailable ? "ARM DynaRec" : "Interpreter");
		DrawText(38.0f, 110.0f, stats, t.value, 0.875f);
		DrawText(38.0f, 132.0f, s_memSummary, t.textDim, 0.875f);

		PresentBothScreens();
	}

	// Release UI vertex buffers again before resuming gameplay
	ReleaseAssets();
	UI::RestoreRenderState();
}
}
