#include "stdafx.h"
#include "UIAtlas.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#pragma GCC diagnostic pop

namespace UI
{

namespace
{

static inline float ClampF(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline float SmoothStep(float e0, float e1, float x)
{
	float t = ClampF((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Signed distance from point (px, py) to axis-aligned rounded box centered at (0, 0).
static inline float SdRoundedBox(float px, float py, float bx, float by, float r)
{
	const float qx = fabsf(px) - bx + r;
	const float qy = fabsf(py) - by + r;
	const float outX = qx > 0.0f ? qx : 0.0f;
	const float outY = qy > 0.0f ? qy : 0.0f;
	const float inside = fminf(fmaxf(qx, qy), 0.0f);
	return sqrtf(outX * outX + outY * outY) + inside - r;
}

// Signed distance to a line segment AB.
static inline float SdSegment(float px, float py, float ax, float ay, float bx, float by)
{
	const float pax = px - ax;
	const float pay = py - ay;
	const float bax = bx - ax;
	const float bay = by - ay;
	const float denom = bax * bax + bay * bay;
	const float h = (denom > 1e-6f) ? ClampF((pax * bax + pay * bay) / denom, 0.0f, 1.0f) : 0.0f;
	const float dx = pax - bax * h;
	const float dy = pay - bay * h;
	return sqrtf(dx * dx + dy * dy);
}

// Signed distance to a triangle (positive outside, negative inside).
static inline float SdTriangle(float px, float py,
                               float x0, float y0, float x1, float y1, float x2, float y2)
{
	const float d0 = SdSegment(px, py, x0, y0, x1, y1);
	const float d1 = SdSegment(px, py, x1, y1, x2, y2);
	const float d2 = SdSegment(px, py, x2, y2, x0, y0);
	const float d  = fminf(d0, fminf(d1, d2));

	const float c0 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
	const float c1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
	const float c2 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);
	const bool inside = (c0 >= 0.0f && c1 >= 0.0f && c2 >= 0.0f) ||
	                    (c0 <= 0.0f && c1 <= 0.0f && c2 <= 0.0f);
	return inside ? -d : d;
}

static inline void PlotAlpha(uint32_t * pixels, int x, int y, float alpha)
{
	if (x < 0 || x >= (int)UIAtlas::kAtlasW || y < 0 || y >= (int)UIAtlas::kAtlasH)
		return;
	const uint8_t a = (uint8_t)(ClampF(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
	uint32_t & dst = pixels[y * UIAtlas::kAtlasW + x];
	const uint8_t curA = (uint8_t)(dst >> 24);
	if (a > curA)
		dst = 0x00FFFFFFu | ((uint32_t)a << 24);
}

static inline void PlotSdf(uint32_t * pixels, int x, int y, float dist, float aa = 0.85f)
{
	PlotAlpha(pixels, x, y, 1.0f - SmoothStep(-aa, aa, dist));
}

// Rasterize procedural 9-slice rounded boxes, circle, and ring into y = 154..202.
void RasterizeShapes(uint32_t * pixels)
{
	// 1) 8x8 solid white swatch at (2, 154)
	for (int y = 154; y < 162; ++y)
		for (int x = 2; x < 10; ++x)
			pixels[y * UIAtlas::kAtlasW + x] = 0xFFFFFFFFu;

	// 2) 32x32 Rounded-rect fill at (16, 154) (r = 9px)
	for (int j = 0; j < 32; ++j)
	{
		for (int i = 0; i < 32; ++i)
		{
			const float px = (float)i - 15.5f;
			const float py = (float)j - 15.5f;
			const float d  = SdRoundedBox(px, py, 14.5f, 14.5f, 8.5f);
			PlotSdf(pixels, 16 + i, 154 + j, d, 0.75f);
		}
	}

	// 3) 32x32 Rounded-rect 2px focus border at (52, 154)
	for (int j = 0; j < 32; ++j)
	{
		for (int i = 0; i < 32; ++i)
		{
			const float px = (float)i - 15.5f;
			const float py = (float)j - 15.5f;
			const float dOuter = SdRoundedBox(px, py, 14.5f, 14.5f, 8.5f);
			const float dRing  = fabsf(dOuter + 1.0f) - 1.0f;
			PlotSdf(pixels, 52 + i, 154 + j, dRing, 0.70f);
		}
	}

	// 4) 32x32 Rounded-rect 1px hairline border at (88, 154)
	for (int j = 0; j < 32; ++j)
	{
		for (int i = 0; i < 32; ++i)
		{
			const float px = (float)i - 15.5f;
			const float py = (float)j - 15.5f;
			const float dOuter = SdRoundedBox(px, py, 14.5f, 14.5f, 8.5f);
			const float dRing  = fabsf(dOuter + 0.55f) - 0.55f;
			PlotSdf(pixels, 88 + i, 154 + j, dRing, 0.65f);
		}
	}

	// 5) 48x48 anti-aliased filled circle at (124, 154)
	for (int j = 0; j < 48; ++j)
	{
		for (int i = 0; i < 48; ++i)
		{
			const float px = (float)i - 23.5f;
			const float py = (float)j - 23.5f;
			const float d  = sqrtf(px * px + py * py) - 22.0f;
			PlotSdf(pixels, 124 + i, 154 + j, d, 0.90f);
		}
	}

	// 6) 48x48 anti-aliased ring at (176, 154) (stroke 2.5px)
	for (int j = 0; j < 48; ++j)
	{
		for (int i = 0; i < 48; ++i)
		{
			const float px = (float)i - 23.5f;
			const float py = (float)j - 23.5f;
			const float d  = fabsf(sqrtf(px * px + py * py) - 20.5f) - 1.25f;
			PlotSdf(pixels, 176 + i, 154 + j, d, 0.85f);
		}
	}
}

// Rasterize 22 crisp 20x20 SDF icons into y = 208..252 (2 rows of 11 icons, 22px cell stride).
void RasterizeIcons(uint32_t * pixels, UVRect outIcons[ICON_COUNT])
{
	const float invW = 1.0f / (float)UIAtlas::kAtlasW;
	const float invH = 1.0f / (float)UIAtlas::kAtlasH;

	for (int id = 0; id < ICON_COUNT; ++id)
	{
		const int col = id % 11;
		const int row = id / 11;
		const int ox  = 4 + col * 22;
		const int oy  = 208 + row * 22;
		outIcons[id]  = { ox * invW, oy * invH, (ox + 20) * invW, (oy + 20) * invH };

		for (int j = 0; j < 20; ++j)
		{
			for (int i = 0; i < 20; ++i)
			{
				// Normalized icon coordinates in [-10, +10]
				const float x = (float)i - 9.5f;
				const float y = (float)j - 9.5f;
				float d = 10.0f;

				switch (id)
				{
				case ICON_CARTRIDGE:
				{
					// N64 Game Pak silhouette: wide body, arched top, side vertical grooves, recessed center label
					const float body   = SdRoundedBox(x, y + 1.0f, 7.8f, 5.8f, 1.6f);
					const float arch   = SdRoundedBox(x, y - 1.2f, 7.4f, 5.2f, 3.6f);
					const float outer  = fminf(body, arch);
					const float label  = SdRoundedBox(x, y + 0.4f, 4.0f, 4.4f, 0.7f);
					const float grooveL = SdSegment(x, y, -5.6f, -3.6f, -5.6f, 5.4f) - 0.45f;
					const float grooveR = SdSegment(x, y,  5.6f, -3.6f,  5.6f, 5.4f) - 0.45f;
					d = fmaxf(outer, -fminf(label, fminf(grooveL, grooveR)));
					break;
				}
				case ICON_PLAY:
					d = SdTriangle(x, y, -4.5f, -6.2f, 6.5f, 0.0f, -4.5f, 6.2f) - 0.6f;
					break;
				case ICON_SAVE:
				{
					// N64 Controller Pak (Memory Pak) shell with top label window + downward save arrow
					const float pak   = SdRoundedBox(x, y - 0.4f, 6.8f, 6.8f, 1.8f);
					const float topW  = SdRoundedBox(x, y - 3.6f, 4.2f, 1.9f, 0.6f);
					const float shaft = SdSegment(x, y, 0.0f, -0.2f, 0.0f, 3.8f) - 1.05f;
					const float head  = SdTriangle(x, y, -3.4f, 2.4f, 3.4f, 2.4f, 0.0f, 6.4f) - 0.25f;
					const float arrow = fminf(shaft, head);
					d = fminf(fmaxf(pak, -fminf(topW, arrow - 0.9f)), arrow);
					break;
				}
				case ICON_LOAD:
				{
					// N64 Controller Pak (Memory Pak) shell with top label window + upward load arrow
					const float pak   = SdRoundedBox(x, y + 0.4f, 6.8f, 6.8f, 1.8f);
					const float topW  = SdRoundedBox(x, y - 3.2f, 4.2f, 1.8f, 0.6f);
					const float shaft = SdSegment(x, y, 0.0f, 1.6f, 0.0f, 5.6f) - 1.05f;
					const float head  = SdTriangle(x, y, -3.4f, 2.6f, 3.4f, 2.6f, 0.0f, -1.2f) - 0.25f;
					const float arrow = fminf(shaft, head);
					d = fminf(fmaxf(pak, -fminf(topW, arrow - 0.9f)), arrow);
					break;
				}
				case ICON_AUDIO:
				{
					const float box  = SdRoundedBox(x + 4.8f, y, 2.2f, 2.6f, 0.6f);
					const float cone = SdTriangle(x, y, -4.5f, 0.0f, 1.5f, -5.8f, 1.5f, 5.8f) - 0.3f;
					const float r    = sqrtf((x - 1.5f) * (x - 1.5f) + y * y);
					float w1 = fabsf(r - 3.6f) - 0.85f;
					float w2 = fabsf(r - 6.4f) - 0.85f;
					if (x < 2.2f || fabsf(y) > (x - 1.2f) * 1.15f) { w1 = 10.0f; w2 = 10.0f; }
					d = fminf(fminf(box, cone), fminf(w1, w2));
					break;
				}
				case ICON_AUDIO_MUTE:
				{
					const float box  = SdRoundedBox(x + 5.0f, y, 2.0f, 2.4f, 0.5f);
					const float cone = SdTriangle(x, y, -4.8f, 0.0f, 0.8f, -5.4f, 0.8f, 5.4f) - 0.3f;
					const float c1   = SdSegment(x, y, 3.0f, -2.8f, 7.6f, 2.8f) - 0.95f;
					const float c2   = SdSegment(x, y, 7.6f, -2.8f, 3.0f, 2.8f) - 0.95f;
					d = fminf(fminf(box, cone), fminf(c1, c2));
					break;
				}
				case ICON_VIDEO:
				{
					// Classic 4:3 CRT TV with rabbit-ear antenna and side tuner knobs
					const float outer = SdRoundedBox(x, y + 1.0f, 7.4f, 5.4f, 1.8f);
					const float inner = SdRoundedBox(x - 1.0f, y + 1.0f, 4.6f, 3.6f, 0.9f);
					const float antL  = SdSegment(x, y, -0.8f, -4.2f, -3.8f, -7.2f) - 0.75f;
					const float antR  = SdSegment(x, y,  0.8f, -4.2f,  3.8f, -7.2f) - 0.75f;
					const float footL = SdSegment(x, y, -4.5f,  6.4f, -3.0f,  6.4f) - 0.8f;
					const float footR = SdSegment(x, y,  3.0f,  6.4f,  4.5f,  6.4f) - 0.8f;
					const float tv    = fmaxf(outer, -inner);
					d = fminf(tv, fminf(fminf(antL, antR), fminf(footL, footR)));
					break;
				}
				case ICON_CONTROLS:
				{
					// Authentic 3-Prong N64 Trident Controller silhouette
					const float topBar = SdRoundedBox(x, y - 2.4f, 7.6f, 3.2f, 2.6f);
					const float prongL = SdSegment(x, y, -5.8f, -1.5f, -5.2f, 5.2f) - 1.85f;
					const float prongC = SdSegment(x, y,  0.0f, -0.5f,  0.0f, 6.4f) - 1.95f;
					const float prongR = SdSegment(x, y,  5.8f, -1.5f,  5.2f, 5.2f) - 1.85f;
					const float body   = fminf(topBar, fminf(prongC, fminf(prongL, prongR)));
					const float stick  = sqrtf(x * x + (y - 1.2f) * (y - 1.2f)) - 1.25f;
					const float dpadH  = SdSegment(x, y, -6.2f, -2.2f, -3.8f, -2.2f) - 0.65f;
					const float dpadV  = SdSegment(x, y, -5.0f, -3.4f, -5.0f, -1.0f) - 0.65f;
					const float btnA   = sqrtf((x - 4.4f) * (x - 4.4f) + (y + 1.8f) * (y + 1.8f)) - 0.9f;
					const float btnB   = sqrtf((x - 5.8f) * (x - 5.8f) + (y + 3.0f) * (y + 3.0f)) - 0.9f;
					d = fmaxf(body, -fminf(stick, fminf(fminf(dpadH, dpadV), fminf(btnA, btnB))));
					break;
				}
				case ICON_RESET:
				{
					const float r = sqrtf(x * x + y * y);
					const float ang = atan2f(y, x);
					float arc = fabsf(r - 5.6f) - 1.05f;
					if (ang > -0.75f && ang < 0.25f) arc = 10.0f;
					const float head = SdTriangle(x, y, 5.2f, -5.6f, 6.2f, -1.6f, 2.0f, -2.4f) - 0.2f;
					d = fminf(arc, head);
					break;
				}
				case ICON_GRID:
				{
					// Authentic 3x3 Nintendo 3DS Home Menu Grid (9 rounded mini-tiles)
					const float gx = fabsf(x) > 2.4f ? (fabsf(x) - 4.8f) : x;
					const float gy = fabsf(y) > 2.4f ? (fabsf(y) - 4.8f) : y;
					d = SdRoundedBox(gx, gy, 1.75f, 1.75f, 0.55f);
					break;
				}
				case ICON_POWER:
				{
					const float r = sqrtf(x * x + y * y);
					const float ang = atan2f(y, x);
					float arc = fabsf(r - 5.8f) - 1.05f;
					if (ang > -2.18f && ang < -0.96f) arc = 10.0f;
					const float stem = SdSegment(x, y, 0.0f, -7.2f, 0.0f, -0.8f) - 1.05f;
					d = fminf(arc, stem);
					break;
				}
				case ICON_STAR:
				case ICON_STAR_OUTLINE:
				{
					const float r = sqrtf(x * x + y * y);
					const float a = atan2f(y, x) + 1.5707963f;
					const float sector = fmodf(fabsf(a), 1.2566371f);
					const float t = fabsf(sector - 0.6283185f) / 0.6283185f;
					const float starR = 3.2f + 4.4f * (t * t);
					const float fill = r - starR;
					d = (id == ICON_STAR) ? fill : (fabsf(fill) - 0.9f);
					break;
				}
				case ICON_GEAR:
				{
					// Nintendo 3DS System Settings diagonal wrench + mini gear
					const float shaft = SdSegment(x, y, -5.6f, 5.6f, 2.6f, -2.6f) - 1.45f;
					const float head  = sqrtf((x - 3.6f) * (x - 3.6f) + (y + 3.6f) * (y + 3.6f)) - 3.4f;
					const float jaw   = SdSegment(x, y, 3.2f, -3.2f, 6.6f, -6.6f) - 1.25f;
					const float wrench = fmaxf(fminf(shaft, head), -jaw);
					const float gx = x - 3.4f, gy = y - 3.4f;
					const float gr = sqrtf(gx * gx + gy * gy);
					const float ga = atan2f(gy, gx);
					const float gear = fmaxf(gr - (2.8f + 0.65f * cosf(ga * 6.0f)), 1.1f - gr);
					d = fminf(wrench, gear);
					break;
				}
				case ICON_CHEVRON_LEFT:
				case ICON_ARROW_LEFT:
					d = SdTriangle(x, y, -5.5f, 0.0f, 4.2f, -5.8f, 4.2f, 5.8f) - 0.4f;
					break;
				case ICON_CHEVRON_RIGHT:
				case ICON_ARROW_RIGHT:
					d = SdTriangle(x, y, 5.5f, 0.0f, -4.2f, -5.8f, -4.2f, 5.8f) - 0.4f;
					break;
				case ICON_ARROW_UP:
					d = SdTriangle(x, y, 0.0f, -5.5f, 5.8f, 4.2f, -5.8f, 4.2f) - 0.4f;
					break;
				case ICON_ARROW_DOWN:
					d = SdTriangle(x, y, 0.0f, 5.5f, 5.8f, -4.2f, -5.8f, -4.2f) - 0.4f;
					break;
				case ICON_MOON:
				{
					const float c1 = sqrtf(x * x + y * y) - 6.2f;
					const float c2 = sqrtf((x - 3.0f) * (x - 3.0f) + (y + 2.2f) * (y + 2.2f)) - 5.4f;
					d = fmaxf(c1, -c2);
					break;
				}
				case ICON_MENU:
				{
					const float l0 = SdSegment(x, y, -5.8f, -4.2f, 5.8f, -4.2f) - 1.05f;
					const float l1 = SdSegment(x, y, -5.8f,  0.0f, 5.8f,  0.0f) - 1.05f;
					const float l2 = SdSegment(x, y, -5.8f,  4.2f, 5.8f,  4.2f) - 1.05f;
					d = fminf(l0, fminf(l1, l2));
					break;
				}
				default:
					break;
				}

				PlotSdf(pixels, ox + i, oy + j, d, 0.72f);
			}
		}
	}
}

// Emergency fallback 8x8 glyphs in case romfs:/Roboto-Medium.ttf is ever absent.
void RasterizeFallbackFont(uint32_t * pixels, FontData & out, int startY, float pxHeight)
{
	out.pixelHeight = pxHeight;
	out.ascent      = pxHeight * 0.8f;
	out.descent     = -pxHeight * 0.2f;
	out.lineHeight  = pxHeight * 1.15f;
	const float invW = 1.0f / (float)UIAtlas::kAtlasW;
	const float invH = 1.0f / (float)UIAtlas::kAtlasH;
	// Simple clean block fallback if TTF fails to load
	for (int i = 0; i < 96; ++i)
	{
		GlyphMetrics & g = out.glyphs[i];
		g.u0 = 6.0f * invW;
		g.v0 = 158.0f * invH;
		g.u1 = 6.0f * invW;
		g.v1 = 158.0f * invH;
		g.xoff = 0.0f;
		g.yoff = -out.ascent;
		g.w = (i == 0) ? 0.0f : pxHeight * 0.5f;
		g.h = (i == 0) ? 0.0f : pxHeight * 0.75f;
		g.advance = pxHeight * 0.6f;
	}
	(void)pixels;
	(void)startY;
}

bool PackRobotoFonts(uint32_t * pixels, FontData fonts[FONT_COUNT])
{
	FILE * f = fopen("romfs:/Roboto-Medium.ttf", "rb");
	if (!f)
		return false;

	fseek(f, 0, SEEK_END);
	const long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsize <= 0 || fsize > 512 * 1024)
	{
		fclose(f);
		return false;
	}

	uint8_t * ttfBuf = (uint8_t *)malloc((size_t)fsize);
	if (!ttfBuf)
	{
		fclose(f);
		return false;
	}
	const size_t readN = fread(ttfBuf, 1, (size_t)fsize, f);
	fclose(f);
	if (readN != (size_t)fsize)
	{
		free(ttfBuf);
		return false;
	}

	stbtt_fontinfo finfo;
	if (!stbtt_InitFont(&finfo, ttfBuf, 0))
	{
		free(ttfBuf);
		return false;
	}

	// Pack both Roboto sizes into the top 148 rows of a temporary 256x148 alpha buffer.
	const int packW = (int)UIAtlas::kAtlasW;
	const int packH = 148;
	uint8_t * alphaBuf = (uint8_t *)calloc((size_t)(packW * packH), 1);
	if (!alphaBuf)
	{
		free(ttfBuf);
		return false;
	}

	stbtt_pack_context pc;
	bool ok = stbtt_PackBegin(&pc, alphaBuf, packW, packH, packW, 1, nullptr) != 0;
	if (ok)
	{
		stbtt_PackSetOversampling(&pc, 1, 1);

		static const float kSizes[FONT_COUNT] = { 12.5f, 15.5f };
		stbtt_packedchar packed[FONT_COUNT][95];
		stbtt_pack_range ranges[FONT_COUNT];
		for (int s = 0; s < FONT_COUNT; ++s)
		{
			ranges[s].font_size                        = kSizes[s];
			ranges[s].first_unicode_codepoint_in_range = 32;
			ranges[s].array_of_unicode_codepoints      = nullptr;
			ranges[s].num_chars                        = 95;
			ranges[s].chardata_for_range               = packed[s];
			ranges[s].h_oversample                     = 0;
			ranges[s].v_oversample                     = 0;
		}

		ok = stbtt_PackFontRanges(&pc, ttfBuf, 0, ranges, FONT_COUNT) != 0;
		stbtt_PackEnd(&pc);

		if (ok)
		{
			// Blit packed alpha into white-RGB RGBA8888 atlas rows 0..147
			for (int y = 0; y < packH; ++y)
			{
				for (int x = 0; x < packW; ++x)
				{
					const uint8_t a = alphaBuf[y * packW + x];
					if (a > 0)
						pixels[y * UIAtlas::kAtlasW + x] = 0x00FFFFFFu | ((uint32_t)a << 24);
				}
			}

			int rawAscent = 0, rawDescent = 0, rawLineGap = 0;
			stbtt_GetFontVMetrics(&finfo, &rawAscent, &rawDescent, &rawLineGap);

			const float invW = 1.0f / (float)UIAtlas::kAtlasW;
			const float invH = 1.0f / (float)UIAtlas::kAtlasH;
			for (int s = 0; s < FONT_COUNT; ++s)
			{
				const float scale = stbtt_ScaleForPixelHeight(&finfo, kSizes[s]);
				fonts[s].pixelHeight = kSizes[s];
				fonts[s].ascent      = rawAscent * scale;
				fonts[s].descent     = rawDescent * scale;
				fonts[s].lineHeight  = (rawAscent - rawDescent + rawLineGap) * scale;

				for (int i = 0; i < 95; ++i)
				{
					const stbtt_packedchar & c = packed[s][i];
					GlyphMetrics & g = fonts[s].glyphs[i];
					g.u0      = (float)c.x0 * invW;
					g.v0      = (float)c.y0 * invH;
					g.u1      = (float)c.x1 * invW;
					g.v1      = (float)c.y1 * invH;
					g.xoff    = c.xoff;
					g.yoff    = c.yoff;
					g.w       = (float)(c.x1 - c.x0);
					g.h       = (float)(c.y1 - c.y0);
					g.advance = c.xadvance;
				}
			}
		}
	}

	free(alphaBuf);
	free(ttfBuf);
	return ok;
}

} // namespace

bool UIAtlas::EnsureLoaded()
{
	if (mTex != nullptr && mTex->HasData())
		return true;

	uint32_t * pixels = (uint32_t *)malloc(kAtlasW * kAtlasH * sizeof(uint32_t));
	if (!pixels)
		return false;

	// Transparent white everywhere eliminates dark halo fringes under GPU_LINEAR filtering.
	for (uint32_t i = 0; i < kAtlasW * kAtlasH; ++i)
		pixels[i] = 0x00FFFFFFu;

	if (!PackRobotoFonts(pixels, mFonts))
	{
		RasterizeFallbackFont(pixels, mFonts[FONT_BODY], 0, 12.5f);
		RasterizeFallbackFont(pixels, mFonts[FONT_TITLE], 64, 15.5f);
	}

	RasterizeShapes(pixels);
	RasterizeIcons(pixels, mIcons);

	mTex = CNativeTexture::Create(kAtlasW, kAtlasH, TexFmt_8888);
	if (mTex != nullptr && mTex->HasData())
	{
		mTex->SetData(pixels, nullptr);
		mTex->ApplyParams(GPU_LINEAR, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}

	free(pixels);
	return mTex != nullptr && mTex->HasData();
}

void UIAtlas::Release()
{
	mTex = nullptr;
}

bool UIAtlas::IsLoaded() const
{
	return mTex != nullptr && mTex->HasData();
}

C3D_Tex * UIAtlas::GetTexture() const
{
	return (mTex != nullptr && mTex->HasData()) ? mTex->GetC3DTexture() : nullptr;
}

UIAtlas & GetAtlas()
{
	static UIAtlas s_atlas;
	return s_atlas;
}

} // namespace UI
