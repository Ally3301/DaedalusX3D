#include "stdafx.h"
#include "UIRenderer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>

#include "HLEGraphics/DaedalusVtx.h"
#include "SysCTR/Graphics/Citro3DContext.h"

namespace UI
{
namespace Renderer
{

namespace
{

constexpr uint32_t kHalfVerts     = 12288;
constexpr uint32_t kMaxBatchVerts = kHalfVerts * 2;
constexpr uint32_t kHudHalfVerts  = 2560;
constexpr uint32_t kHudMaxVerts   = kHudHalfVerts * 2;

DaedalusVtx * s_menuPool   = nullptr;
DaedalusVtx * s_hudPool    = nullptr;
DaedalusVtx * s_vtxPool    = nullptr; // active pool (menu or hud)
uint32_t      s_activeHalf = kHalfVerts;
uint32_t      s_frameBank  = 0;
uint32_t      s_hudBank    = 0;
uint32_t      s_vtxOffset  = 0;
uint32_t      s_batchStart = 0;
uint32_t      s_batchCount = 0;
C3D_Tex *     s_batchTex   = nullptr;
bool          s_inHudPass  = false;

static inline void PushVert(float x, float y, float u, float v, uint32_t col)
{
	DaedalusVtx & dst = s_vtxPool[s_batchStart + s_batchCount++];
	dst.Texture.x  = u;
	dst.Texture.y  = v;
	dst.Colour     = c32(col);
	dst.Position.x = x;
	dst.Position.y = y;
	dst.Position.z = 0.0f;
}

bool ReserveVerts(uint32_t needed)
{
	if (!s_vtxPool || needed > s_activeHalf)
		return false;
	const uint32_t bank     = s_inHudPass ? s_hudBank : s_frameBank;
	const uint32_t bankBase = bank * s_activeHalf;
	const uint32_t bankEnd  = bankBase + s_activeHalf;
	if (s_batchStart + s_batchCount + needed > bankEnd)
	{
		if (s_inHudPass)
			return false; // Never split/sync mid-gameplay frame; HUD fits easily in 1024 verts.
		FlushBatch();
		if (s_vtxOffset > bankBase)
			Citro3D::FlushDataCache(&s_vtxPool[bankBase], (s_vtxOffset - bankBase) * sizeof(DaedalusVtx));
		C3D_FrameSplit(GX_CMDLIST_FLUSH);
		C3D_FrameSync();
		s_vtxOffset  = bankBase;
		s_batchStart = bankBase;
		s_batchCount = 0;
	}
	return true;
}

void SetBatchTexture(C3D_Tex * tex)
{
	if (tex == nullptr)
		tex = GetAtlas().GetTexture();
	if (s_batchTex != tex)
	{
		FlushBatch();
		s_batchTex = tex;
	}
}

void EmitUVQuad(float x, float y, float w, float h,
                float u0, float v0, float u1, float v1, uint32_t col)
{
	if (w <= 0.0f || h <= 0.0f) return;
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

// Generic 9-slice draw from a 32x32 SDF patch in UIAtlas (border = 11px).
void DrawNineSlice(float x, float y, float w, float h, float radius, const UVRect & uv, uint32_t col)
{
	C3D_Tex * atlasTex = GetAtlas().GetTexture();
	if (!atlasTex || w <= 0.0f || h <= 0.0f) return;
	SetBatchTexture(atlasTex);

	// Scale the 11px corner slice proportionally to desired corner radius (base radius ~8.5px).
	float b = radius * (UIAtlas::kSliceBorder / 8.5f);
	if (b > w * 0.5f) b = w * 0.5f;
	if (b > h * 0.5f) b = h * 0.5f;

	const float frac = UIAtlas::kSliceBorder / UIAtlas::kSliceSize;
	const float du   = (uv.u1 - uv.u0) * frac;
	const float dv   = (uv.v1 - uv.v0) * frac;

	const float xs[4] = { x, x + b, x + w - b, x + w };
	const float ys[4] = { y, y + b, y + h - b, y + h };
	const float us[4] = { uv.u0, uv.u0 + du, uv.u1 - du, uv.u1 };
	const float vs[4] = { uv.v0, uv.v0 + dv, uv.v1 - dv, uv.v1 };

	for (int row = 0; row < 3; ++row)
	{
		const float rh = ys[row + 1] - ys[row];
		if (rh <= 0.01f) continue;
		for (int c = 0; c < 3; ++c)
		{
			const float cw = xs[c + 1] - xs[c];
			if (cw <= 0.01f) continue;
			EmitUVQuad(xs[c], ys[row], cw, rh, us[c], vs[row], us[c + 1], vs[row + 1], col);
		}
	}
}

void DrawNineSliceVGrad(float x, float y, float w, float h, float radius, const UVRect & uv, uint32_t cTop, uint32_t cBot)
{
	C3D_Tex * atlasTex = GetAtlas().GetTexture();
	if (!atlasTex || w <= 0.0f || h <= 0.0f) return;
	SetBatchTexture(atlasTex);

	float b = radius * (UIAtlas::kSliceBorder / 8.5f);
	if (b > w * 0.5f) b = w * 0.5f;
	if (b > h * 0.5f) b = h * 0.5f;

	const float frac = UIAtlas::kSliceBorder / UIAtlas::kSliceSize;
	const float du   = (uv.u1 - uv.u0) * frac;
	const float dv   = (uv.v1 - uv.v0) * frac;

	const float xs[4] = { x, x + b, x + w - b, x + w };
	const float ys[4] = { y, y + b, y + h - b, y + h };
	const float us[4] = { uv.u0, uv.u0 + du, uv.u1 - du, uv.u1 };
	const float vs[4] = { uv.v0, uv.v0 + dv, uv.v1 - dv, uv.v1 };
	const float invH  = 1.0f / h;

	for (int row = 0; row < 3; ++row)
	{
		const float rh = ys[row + 1] - ys[row];
		if (rh <= 0.01f) continue;
		const uint32_t rTop = LerpColor(cTop, cBot, (ys[row] - y) * invH);
		const uint32_t rBot = LerpColor(cTop, cBot, (ys[row + 1] - y) * invH);
		for (int c = 0; c < 3; ++c)
		{
			const float cw = xs[c + 1] - xs[c];
			if (cw <= 0.01f) continue;
			if (!ReserveVerts(6)) return;
			PushVert(xs[c],     ys[row],     us[c],     vs[row],     rTop);
			PushVert(xs[c + 1], ys[row],     us[c + 1], vs[row],     rTop);
			PushVert(xs[c + 1], ys[row + 1], us[c + 1], vs[row + 1], rBot);
			PushVert(xs[c],     ys[row],     us[c],     vs[row],     rTop);
			PushVert(xs[c + 1], ys[row + 1], us[c + 1], vs[row + 1], rBot);
			PushVert(xs[c],     ys[row + 1], us[c],     vs[row + 1], rBot);
		}
	}
}

float DrawButtonGlyph(float x, float y, const char * badge, const ThemePalette & t)
{
	if (!badge || !badge[0]) return 0.0f;
	const size_t len = strlen(badge);
	if (len == 1)
	{
		// Circular 3DS face button badge: (A), (B), (X), (Y)
		const float r = 6.5f;
		Circle(x + r, y + r + 1.0f, r, t.hintGlyphBg);
		const float tw = MeasureText(FONT_BODY, badge, 0.78f);
		Text(FONT_BODY, x + r - tw * 0.5f, y + 1.5f, badge, t.hintGlyphText, 0.78f);
		return r * 2.0f;
	}
	else
	{
		// Compact rounded pill badge: L, R, START, SEL
		const float tw = MeasureText(FONT_BODY, badge, 0.72f);
		const float pw = tw + 8.0f;
		RoundedRect(x, y + 1.5f, pw, 12.0f, 3.5f, t.hintGlyphBg);
		Text(FONT_BODY, x + 4.0f, y + 2.0f, badge, t.hintGlyphText, 0.72f);
		return pw;
	}
}

} // namespace

bool EnsureMenuPool()
{
	if (!GetAtlas().EnsureLoaded())
		return false;

	if (s_menuPool == nullptr)
	{
		s_menuPool = static_cast<DaedalusVtx *>(linearAlloc(kMaxBatchVerts * sizeof(DaedalusVtx)));
		if (!s_menuPool)
			return false;
	}
	return true;
}

void ReleaseMenuPool()
{
	if (s_menuPool)
	{
		linearFree(s_menuPool);
		s_menuPool = nullptr;
	}
	if (!s_inHudPass)
		s_vtxPool = nullptr;
	s_frameBank  = 0;
	s_vtxOffset  = 0;
	s_batchStart = 0;
	s_batchCount = 0;
	s_batchTex   = nullptr;
}

bool EnsureHudPool()
{
	if (!GetAtlas().EnsureLoaded())
		return false;
	if (s_hudPool == nullptr)
	{
		s_hudPool = static_cast<DaedalusVtx *>(linearAlloc(kHudMaxVerts * sizeof(DaedalusVtx)));
		if (!s_hudPool)
			return false;
	}
	return true;
}

void ReleaseHudPool()
{
	if (s_hudPool)
	{
		linearFree(s_hudPool);
		s_hudPool = nullptr;
	}
	if (s_inHudPass)
	{
		s_vtxPool   = nullptr;
		s_inHudPass = false;
	}
	s_hudBank = 0;
}

void Shutdown()
{
	ReleaseMenuPool();
	ReleaseHudPool();
	GetAtlas().Release();
}

void FlushBatch()
{
	if (s_batchCount == 0 || !s_vtxPool || !GetAtlas().IsLoaded())
		return;

	C3D_Tex * tex = (s_batchTex != nullptr) ? s_batchTex : GetAtlas().GetTexture();
	Citro3D::SetTextureEnabled(true);
	Citro3D::BindTexture(tex);
	Citro3D::DrawArrays(GPU_TRIANGLES, (int)s_batchStart, (int)s_batchCount);
	s_vtxOffset += s_batchCount;
	s_batchStart = s_vtxOffset;
	s_batchCount = 0;
}

void BeginFrame()
{
	s_inHudPass  = false;
	s_vtxPool    = s_menuPool;
	s_activeHalf = kHalfVerts;
	s_frameBank ^= 1;
	const uint32_t bankBase = s_frameBank * kHalfVerts;
	s_vtxOffset  = bankBase;
	s_batchStart = bankBase;
	s_batchCount = 0;
	s_batchTex   = GetAtlas().GetTexture();
	Citro3D::BeginUIFrame();
}

void BeginScreen(gfxScreen_t screen, int w, int h, uint32_t clearColABGR)
{
	FlushBatch();
	Citro3D::BeginUIScreen(screen, w, h, ToClearRGBA8(clearColABGR));
	Citro3D::SetVertexBuffer(s_vtxPool, sizeof(DaedalusVtx));
	s_batchTex = GetAtlas().GetTexture();
	if (s_batchTex)
		Citro3D::BindTexture(s_batchTex);
}

void EndFrame()
{
	FlushBatch();
	const uint32_t bankBase = s_frameBank * kHalfVerts;
	if (s_vtxPool && s_vtxOffset > bankBase)
		Citro3D::FlushDataCache(&s_vtxPool[bankBase], (s_vtxOffset - bankBase) * sizeof(DaedalusVtx));
	Citro3D::EndUIFrame();
}

bool BeginHudBottomPass()
{
	if (!EnsureHudPool())
		return false;
	if (!Citro3D::BeginGameBottomPass())
		return false;

	s_inHudPass  = true;
	s_vtxPool    = s_hudPool;
	s_activeHalf = kHudHalfVerts;
	s_hudBank   ^= 1;
	const uint32_t bankBase = s_hudBank * kHudHalfVerts;
	s_vtxOffset  = bankBase;
	s_batchStart = bankBase;
	s_batchCount = 0;
	s_batchTex   = GetAtlas().GetTexture();
	Citro3D::SetVertexBuffer(s_vtxPool, sizeof(DaedalusVtx));
	if (s_batchTex)
		Citro3D::BindTexture(s_batchTex);
	return true;
}

void EndHudBottomPass()
{
	if (!s_inHudPass)
		return;
	FlushBatch();
	const uint32_t bankBase = s_hudBank * kHudHalfVerts;
	if (s_vtxPool && s_vtxOffset > bankBase)
		Citro3D::FlushDataCache(&s_vtxPool[bankBase], (s_vtxOffset - bankBase) * sizeof(DaedalusVtx));
	Citro3D::EndGameBottomPass();
	s_inHudPass  = false;
	s_vtxPool    = s_menuPool;
	s_activeHalf = kHalfVerts;
}

void SetClipRect(bool enable, int x, int y, int w, int h, int screenW, int screenH)
{
	FlushBatch();
	Citro3D::SetScissorLandscape(enable, x, y, w, h, screenW, screenH);
}

void QuadGrad(float x, float y, float w, float h,
              uint32_t cTL, uint32_t cTR, uint32_t cBL, uint32_t cBR)
{
	SetBatchTexture(GetAtlas().GetTexture());
	if (!ReserveVerts(6)) return;
	const float wu = GetAtlas().WhiteU();
	const float wv = GetAtlas().WhiteV();
	const float x1 = x + w;
	const float y1 = y + h;
	PushVert(x,  y,  wu, wv, cTL);
	PushVert(x1, y,  wu, wv, cTR);
	PushVert(x1, y1, wu, wv, cBR);
	PushVert(x,  y,  wu, wv, cTL);
	PushVert(x1, y1, wu, wv, cBR);
	PushVert(x,  y1, wu, wv, cBL);
}

void Quad(float x, float y, float w, float h, uint32_t col)
{
	QuadGrad(x, y, w, h, col, col, col, col);
}

void QuadVGrad(float x, float y, float w, float h, uint32_t topCol, uint32_t botCol)
{
	QuadGrad(x, y, w, h, topCol, topCol, botCol, botCol);
}

void PolyTri(float x0, float y0, float x1, float y1, float x2, float y2,
             uint32_t c0, uint32_t c1, uint32_t c2)
{
	SetBatchTexture(GetAtlas().GetTexture());
	if (!ReserveVerts(3)) return;
	const float wu = GetAtlas().WhiteU();
	const float wv = GetAtlas().WhiteV();
	PushVert(x0, y0, wu, wv, c0);
	PushVert(x1, y1, wu, wv, c1);
	PushVert(x2, y2, wu, wv, c2);
}

void TexturedPolyTri(C3D_Tex * tex,
                     float x0, float y0, float x1, float y1, float x2, float y2,
                     float u0, float v0, float u1, float v1, float u2, float v2,
                     uint32_t c0, uint32_t c1, uint32_t c2)
{
	if (!tex) return;
	SetBatchTexture(tex);
	if (!ReserveVerts(3)) return;
	PushVert(x0, y0, u0, v0, c0);
	PushVert(x1, y1, u1, v1, c1);
	PushVert(x2, y2, u2, v2, c2);
}

void PolyQuad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
              uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3)
{
	SetBatchTexture(GetAtlas().GetTexture());
	if (!ReserveVerts(6)) return;
	const float wu = GetAtlas().WhiteU();
	const float wv = GetAtlas().WhiteV();
	PushVert(x0, y0, wu, wv, c0);
	PushVert(x1, y1, wu, wv, c1);
	PushVert(x2, y2, wu, wv, c2);
	PushVert(x0, y0, wu, wv, c0);
	PushVert(x2, y2, wu, wv, c2);
	PushVert(x3, y3, wu, wv, c3);
}

void TexturedPolyQuad(C3D_Tex * tex,
                      float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
                      float u0, float v0, float u1, float v1,
                      uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3)
{
	if (!tex) return;
	SetBatchTexture(tex);
	if (!ReserveVerts(6)) return;
	PushVert(x0, y0, u0, v0, c0);
	PushVert(x1, y1, u1, v0, c1);
	PushVert(x2, y2, u1, v1, c2);
	PushVert(x0, y0, u0, v0, c0);
	PushVert(x2, y2, u1, v1, c2);
	PushVert(x3, y3, u0, v1, c3);
}

void Draw3DSCornerCursor(float x, float y, float w, float h, float radius,
                         uint32_t mainCol, uint32_t glowCol)
{
	// Outer soft cyan glow ring + 2px border
	RoundedRect(x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, radius + 2.0f, glowCol);
	RoundedBorder2(x - 1.0f, y - 1.0f, w + 2.0f, h + 2.0f, radius + 1.0f, mainCol);
	// 4 sculpted corner L-brackets (signature Nintendo 3DS Home Menu selection cursor)
	const float arm = 12.0f;
	const float th  = 3.0f;
	// Top-Left
	RoundedRect(x - 2.5f, y - 2.5f, arm, th, 1.5f, mainCol);
	RoundedRect(x - 2.5f, y - 2.5f, th, arm, 1.5f, mainCol);
	// Top-Right
	RoundedRect(x + w + 2.5f - arm, y - 2.5f, arm, th, 1.5f, mainCol);
	RoundedRect(x + w + 2.5f - th,  y - 2.5f, th, arm, 1.5f, mainCol);
	// Bottom-Left
	RoundedRect(x - 2.5f, y + h + 2.5f - th,  arm, th, 1.5f, mainCol);
	RoundedRect(x - 2.5f, y + h + 2.5f - arm, th, arm, 1.5f, mainCol);
	// Bottom-Right
	RoundedRect(x + w + 2.5f - arm, y + h + 2.5f - th,  arm, th, 1.5f, mainCol);
	RoundedRect(x + w + 2.5f - th,  y + h + 2.5f - arm, th, arm, 1.5f, mainCol);
}

void TexturedQuad(C3D_Tex * tex, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1, uint32_t col)
{
	if (!tex) return;
	SetBatchTexture(tex);
	EmitUVQuad(x, y, w, h, u0, v0, u1, v1, col);
}

void RoundedRect(float x, float y, float w, float h, float radius, uint32_t fillCol)
{
	DrawNineSlice(x, y, w, h, radius, GetAtlas().RoundedFillUV(), fillCol);
}

void RoundedRectVGrad(float x, float y, float w, float h, float radius, uint32_t cTop, uint32_t cBot)
{
	DrawNineSliceVGrad(x, y, w, h, radius, GetAtlas().RoundedFillUV(), cTop, cBot);
}

void RoundedBorder2(float x, float y, float w, float h, float radius, uint32_t borderCol)
{
	DrawNineSlice(x, y, w, h, radius, GetAtlas().RoundedBorder2UV(), borderCol);
}

void RoundedBorder1(float x, float y, float w, float h, float radius, uint32_t borderCol)
{
	DrawNineSlice(x, y, w, h, radius, GetAtlas().RoundedBorder1UV(), borderCol);
}

void Circle(float cx, float cy, float radius, uint32_t col)
{
	SetBatchTexture(GetAtlas().GetTexture());
	const UVRect uv = GetAtlas().CircleFillUV();
	const float d = radius * (48.0f / 44.0f); // account for 2px AA pad inside 48x48 cell
	EmitUVQuad(cx - d, cy - d, d * 2.0f, d * 2.0f, uv.u0, uv.v0, uv.u1, uv.v1, col);
}

void Ring(float cx, float cy, float radius, uint32_t col)
{
	SetBatchTexture(GetAtlas().GetTexture());
	const UVRect uv = GetAtlas().CircleRingUV();
	const float d = radius * (48.0f / 42.0f);
	EmitUVQuad(cx - d, cy - d, d * 2.0f, d * 2.0f, uv.u0, uv.v0, uv.u1, uv.v1, col);
}

void Icon(EIconId icon, float x, float y, float size, uint32_t col)
{
	if (icon < 0 || icon >= ICON_COUNT) return;
	SetBatchTexture(GetAtlas().GetTexture());
	const UVRect & uv = GetAtlas().GetIconUV(icon);
	EmitUVQuad(x, y, size, size, uv.u0, uv.v0, uv.u1, uv.v1, col);
}

float MeasureText(EFontSize font, const char * text, float scale)
{
	if (!text || !text[0]) return 0.0f;
	const FontData & fd = GetAtlas().GetFont(font);
	float w = 0.0f;
	for (const char * p = text; *p; ++p)
	{
		unsigned char ch = (unsigned char)*p;
		if (ch < 32 || ch > 126) ch = '?';
		w += fd.glyphs[ch - 32].advance * scale;
	}
	return w;
}

float LineHeight(EFontSize font, float scale)
{
	return GetAtlas().GetFont(font).lineHeight * scale;
}

void Text(EFontSize font, float x, float y, const char * text, uint32_t col, float scale)
{
	if (!text || !text[0] || !GetAtlas().IsLoaded()) return;
	SetBatchTexture(GetAtlas().GetTexture());
	const FontData & fd = GetAtlas().GetFont(font);
	const float baseline = y + fd.ascent * scale;
	float cx = x;

	for (const char * p = text; *p; ++p)
	{
		unsigned char ch = (unsigned char)*p;
		if (ch < 32 || ch > 126) ch = '?';
		const GlyphMetrics & g = fd.glyphs[ch - 32];
		if (g.w > 0.0f && g.h > 0.0f)
		{
			const float gx = floorf(cx + g.xoff * scale + 0.5f);
			const float gy = floorf(baseline + g.yoff * scale + 0.5f);
			EmitUVQuad(gx, gy, g.w * scale, g.h * scale, g.u0, g.v0, g.u1, g.v1, col);
		}
		cx += g.advance * scale;
	}
}

void TextCentered(EFontSize font, float cx, float y, const char * text, uint32_t col, float scale)
{
	Text(font, cx - MeasureText(font, text, scale) * 0.5f, y, text, col, scale);
}

void TextRight(EFontSize font, float rightX, float y, const char * text, uint32_t col, float scale)
{
	Text(font, rightX - MeasureText(font, text, scale), y, text, col, scale);
}

void TextEllipsis(EFontSize font, float x, float y, float maxW, const char * text, uint32_t col, float scale)
{
	if (!text || !text[0]) return;
	if (MeasureText(font, text, scale) <= maxW)
	{
		Text(font, x, y, text, col, scale);
		return;
	}
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", text);
	size_t n = strlen(buf);
	while (n > 1 && MeasureText(font, buf, scale) > maxW)
	{
		buf[--n] = '\0';
		if (n >= 3)
		{
			buf[n - 3] = '.';
			buf[n - 2] = '.';
			buf[n - 1] = '.';
		}
	}
	Text(font, x, y, buf, col, scale);
}

void TextCenteredEllipsis(EFontSize font, float cx, float y, float maxW, const char * text, uint32_t col, float scale)
{
	if (!text || !text[0]) return;
	const float fullW = MeasureText(font, text, scale);
	if (fullW <= maxW)
	{
		Text(font, cx - fullW * 0.5f, y, text, col, scale);
		return;
	}
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", text);
	size_t n = strlen(buf);
	while (n > 1 && MeasureText(font, buf, scale) > maxW)
	{
		buf[--n] = '\0';
		if (n >= 3)
		{
			buf[n - 3] = '.';
			buf[n - 2] = '.';
			buf[n - 1] = '.';
		}
	}
	const float tw = MeasureText(font, buf, scale);
	Text(font, cx - tw * 0.5f, y, buf, col, scale);
}

void TextWrapped(EFontSize font, float x, float y, float maxW, float lineH,
                 const char * text, uint32_t col, float scale)
{
	if (!text || !text[0] || maxW <= 8.0f) return;
	const char * p = text;
	float cy = y;
	char line[128];

	while (*p)
	{
		while (*p == ' ') ++p;
		if (!*p) break;

		int bestLen   = 0;
		int lastSpace = -1;
		int len       = 0;
		while (p[len] && p[len] != '\n' && len < (int)sizeof(line) - 1)
		{
			line[len] = p[len];
			line[len + 1] = '\0';
			if (MeasureText(font, line, scale) > maxW && len > 0)
				break;
			if (p[len] == ' ')
				lastSpace = len;
			bestLen = ++len;
		}
		if (p[bestLen] && p[bestLen] != '\n' && lastSpace > 0)
			bestLen = lastSpace;
		if (bestLen <= 0)
			bestLen = 1;

		memcpy(line, p, (size_t)bestLen);
		line[bestLen] = '\0';
		Text(font, x, cy, line, col, scale);
		p += bestLen;
		if (*p == '\n') ++p;
		cy += lineH;
	}
}

void DrawBackdrop(float w, float h, const ThemePalette & t)
{
	// Subtle authentic 3DS horizontal micro-stripes (only when bgStripe differs from bg).
	if (t.bgStripe != t.bg)
	{
		for (float y = 26.0f; y < h; y += 6.0f)
			Quad(0.0f, y, w, 2.0f, t.bgStripe);
	}
}

void DrawHeaderBar(float w, const char * leftTitle, const char * rightSub, const ThemePalette & t)
{
	Quad(0.0f, 0.0f, w, 24.0f, t.headerBg);
	Quad(0.0f, 24.0f, w, 1.0f, t.headerRule);
	if (leftTitle && leftTitle[0])
		Text(FONT_TITLE, 10.0f, 4.0f, leftTitle, t.text, 0.92f);
	if (rightSub && rightSub[0])
		TextRight(FONT_BODY, w - 10.0f, 6.0f, rightSub, t.textMuted, 0.90f);
}

void DrawHintBar(float w, const HintItem * items, int count, const ThemePalette & t,
                 const char * rightBadge, const char * rightLabel)
{
	const float barY = 218.0f;
	const float barH = 22.0f;
	Quad(0.0f, barY, w, 1.0f, t.hintRule);
	Quad(0.0f, barY + 1.0f, w, barH - 1.0f, t.hintBg);

	float cx = 8.0f;
	const float gy = barY + 3.5f;
	for (int i = 0; i < count; ++i)
	{
		if (items[i].badge && items[i].badge[0])
		{
			cx += DrawButtonGlyph(cx, gy, items[i].badge, t) + 4.0f;
		}
		if (items[i].label && items[i].label[0])
		{
			Text(FONT_BODY, cx, barY + 5.0f, items[i].label, t.hintText, 0.88f);
			cx += MeasureText(FONT_BODY, items[i].label, 0.88f) + 12.0f;
		}
	}

	if (rightLabel && rightLabel[0])
	{
		const float lw = MeasureText(FONT_BODY, rightLabel, 0.88f);
		float rx = w - 8.0f - lw;
		Text(FONT_BODY, rx, barY + 5.0f, rightLabel, t.hintText, 0.88f);
		if (rightBadge && rightBadge[0])
		{
			const float bw = (strlen(rightBadge) == 1) ? 13.0f : (MeasureText(FONT_BODY, rightBadge, 0.72f) + 8.0f);
			DrawButtonGlyph(rx - bw - 4.0f, gy, rightBadge, t);
		}
	}
}

void DrawCard(float x, float y, float w, float h, bool focused, bool pressed,
              const ThemePalette & t, float radius)
{
	// Soft shadow underneath card
	if ((t.cardShadow >> 24) > 0)
		RoundedRect(x, y + 2.0f, w, h, radius, t.cardShadow);

	if (focused)
	{
		RoundedRect(x - 1.0f, y - 1.0f, w + 2.0f, h + 2.0f, radius + 1.0f, t.focusGlow);
		RoundedRect(x, y, w, h, radius, t.focusFill);
		RoundedBorder2(x, y, w, h, radius, t.focusBorder);
	}
	else
	{
		RoundedRect(x, y, w, h, radius, pressed ? t.cardBgAlt : t.cardBg);
		RoundedBorder1(x, y, w, h, radius, t.cardBorder);
	}
}

void DrawPill(float x, float y, float w, float h, const char * label,
              uint32_t fillCol, uint32_t borderCol, uint32_t textCol, float scale)
{
	const float r = h * 0.45f;
	RoundedRect(x, y, w, h, r, fillCol);
	RoundedBorder1(x, y, w, h, r, borderCol);
	const float lh = LineHeight(FONT_BODY, scale);
	TextCentered(FONT_BODY, x + w * 0.5f, y + (h - lh) * 0.5f + 0.5f, label, textCol, scale);
}

void DrawBatteryIcon(float x, float y, uint8_t level, bool charging, uint32_t col)
{
	// Compact 18x9 3DS-style battery outline + 4 segments
	Quad(x, y, 16.0f, 1.0f, col);
	Quad(x, y + 8.0f, 16.0f, 1.0f, col);
	Quad(x, y, 1.0f, 9.0f, col);
	Quad(x + 15.0f, y, 1.0f, 9.0f, col);
	Quad(x + 16.0f, y + 2.0f, 2.0f, 5.0f, col);

	// 3DS PTMU battery levels: 0..5 (map 1..5 to 1..4 bars)
	int bars = (level > 4) ? 4 : (int)level;
	if (bars < 0) bars = 0;
	const uint32_t fill = charging ? RGBA(245, 197, 24) :
	                      (bars <= 1 ? RGBA(236, 72, 78) : col);
	for (int i = 0; i < bars; ++i)
		Quad(x + 2.0f + i * 3.0f, y + 2.0f, 2.0f, 5.0f, fill);
}

} // namespace Renderer
} // namespace UI
