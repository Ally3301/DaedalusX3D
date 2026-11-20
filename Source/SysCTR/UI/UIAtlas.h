#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdint.h>

#include "Graphics/NativeTexture.h"

namespace UI
{

enum EFontSize
{
	FONT_BODY = 0,  // ~12.5px Roboto Medium (lists, buttons, labels)
	FONT_TITLE,     // ~15.5px Roboto Medium (headers, card titles)
	FONT_COUNT
};

enum EIconId
{
	ICON_CARTRIDGE = 0,
	ICON_PLAY,
	ICON_SAVE,
	ICON_LOAD,
	ICON_AUDIO,
	ICON_AUDIO_MUTE,
	ICON_VIDEO,
	ICON_CONTROLS,
	ICON_RESET,
	ICON_GRID,
	ICON_POWER,
	ICON_STAR,
	ICON_STAR_OUTLINE,
	ICON_GEAR,
	ICON_CHEVRON_LEFT,
	ICON_CHEVRON_RIGHT,
	ICON_ARROW_UP,
	ICON_ARROW_DOWN,
	ICON_ARROW_LEFT,
	ICON_ARROW_RIGHT,
	ICON_MOON,
	ICON_MENU,
	ICON_COUNT
};

struct GlyphMetrics
{
	float u0, v0, u1, v1;
	float xoff, yoff;
	float w, h;
	float advance;
};

struct UVRect
{
	float u0, v0, u1, v1;
};

struct FontData
{
	float        pixelHeight;
	float        ascent;
	float        descent;
	float        lineHeight;
	GlyphMetrics glyphs[96]; // ASCII 32..126 + sentinel
};

class UIAtlas
{
public:
	static constexpr uint32_t kAtlasW = 256;
	static constexpr uint32_t kAtlasH = 256;

	bool EnsureLoaded();
	void Release();
	bool IsLoaded() const;

	C3D_Tex * GetTexture() const;

	const FontData & GetFont(EFontSize sz) const { return mFonts[sz]; }
	const UVRect &   GetIconUV(EIconId id) const { return mIcons[id]; }

	// Built-in anti-aliased SDF shape regions in the same atlas (1 draw call!).
	float WhiteU() const { return 6.0f / (float)kAtlasW; }
	float WhiteV() const { return 158.0f / (float)kAtlasH; }

	// 32x32 9-slice regions (inset = 11px for crisp 9px rounded corners).
	UVRect RoundedFillUV()    const { return MakeUV(16, 154, 32, 32); }
	UVRect RoundedBorder2UV() const { return MakeUV(52, 154, 32, 32); }
	UVRect RoundedBorder1UV() const { return MakeUV(88, 154, 32, 32); }

	// 48x48 anti-aliased circle and ring regions.
	UVRect CircleFillUV() const { return MakeUV(124, 154, 48, 48); }
	UVRect CircleRingUV() const { return MakeUV(176, 154, 48, 48); }

	static constexpr float kSliceSize   = 32.0f;
	static constexpr float kSliceBorder = 11.0f;

private:
	static UVRect MakeUV(int x, int y, int w, int h)
	{
		const float invW = 1.0f / (float)kAtlasW;
		const float invH = 1.0f / (float)kAtlasH;
		return { x * invW, y * invH, (x + w) * invW, (y + h) * invH };
	}

	CRefPtr<CNativeTexture> mTex;
	FontData                mFonts[FONT_COUNT] = {};
	UVRect                  mIcons[ICON_COUNT] = {};
};

UIAtlas & GetAtlas();

} // namespace UI
