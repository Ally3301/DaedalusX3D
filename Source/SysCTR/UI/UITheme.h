#pragma once

#include <stdint.h>

namespace UI
{

// Little-endian ABGR8888 (matches PICA200 DaedalusVtx / c32 memory layout).
static inline uint32_t RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
	return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static inline uint32_t WithAlpha(uint32_t col, uint8_t a)
{
	return (col & 0x00FFFFFFu) | ((uint32_t)a << 24);
}

// Convert ABGR8888 (DaedalusVtx) to 0xRRGGBBAA (C3D_RenderTargetClear).
static inline uint32_t ToClearRGBA8(uint32_t col)
{
	return ((col & 0xFFu) << 24) |
	       (((col >> 8) & 0xFFu) << 16) |
	       (((col >> 16) & 0xFFu) << 8) |
	       ((col >> 24) & 0xFFu);
}

static inline uint32_t LerpColor(uint32_t a, uint32_t b, float t)
{
	if (t <= 0.0f) return a;
	if (t >= 1.0f) return b;
	const auto lerpCh = [t](uint32_t ca, uint32_t cb) -> uint8_t {
		return (uint8_t)((float)ca + ((float)cb - (float)ca) * t + 0.5f);
	};
	return RGBA(
		lerpCh(a & 0xFFu, b & 0xFFu),
		lerpCh((a >> 8) & 0xFFu, (b >> 8) & 0xFFu),
		lerpCh((a >> 16) & 0xFFu, (b >> 16) & 0xFFu),
		lerpCh((a >> 24) & 0xFFu, (b >> 24) & 0xFFu));
}

enum EThemeId
{
	THEME_DARK = 0,
	THEME_LIGHT,
	THEME_MIDNIGHT,
	THEME_STEALTH,
	THEME_COUNT
};

// Design tokens modeled after native Nintendo 3DS / CTR64 UI.
struct ThemePalette
{
	const char * name;
	bool         isLight;

	// Canvas & surface hierarchy
	uint32_t bg;           // Main screen background
	uint32_t bgStripe;     // Subtle horizontal 3DS micro-stripe
	uint32_t headerBg;     // Top bar fill
	uint32_t headerRule;   // 1px hairline under header
	uint32_t cardBg;       // Elevated rounded card fill
	uint32_t cardBgAlt;    // Pressed / subtle secondary card fill
	uint32_t cardBorder;   // Hairline border around cards
	uint32_t cardShadow;   // Soft drop shadow beneath cards

	// Focus & selection (signature Nintendo 3DS cyan-blue)
	uint32_t focusFill;    // Highlighted card fill
	uint32_t focusBorder;  // 2px crisp selection outline
	uint32_t focusGlow;    // Outer ambient focus halo

	// Typography
	uint32_t text;         // Primary readable text
	uint32_t textMuted;    // Secondary labels & subtitles
	uint32_t textDim;      // Hints & disabled items

	// Brand & N64 hardware accents
	uint32_t accent;       // Primary interactive icon / CTA accent
	uint32_t iconMuted;    // Calm neutral icon (Reset, Exit)
	uint32_t n64Yellow;    // C-Buttons gold
	uint32_t n64Green;     // N64 B / Success / Live badge
	uint32_t n64Blue;      // N64 A / Info badge
	uint32_t n64Red;       // N64 Start / Danger / Warning
	uint32_t badgeBg;      // Cartridge/icon square badge fill
	uint32_t badgeBgSel;   // Cartridge/icon square badge fill when focused

	// Bottom dark hint bar (consistent across both Light & Dark modes)
	uint32_t hintBg;
	uint32_t hintRule;
	uint32_t hintText;
	uint32_t hintGlyphBg;
	uint32_t hintGlyphText;
};

const ThemePalette & GetTheme(int themeIdx);
const ThemePalette & ActiveTheme();
int  GetThemeIndex();
void SetThemeIndex(int idx);
void ToggleLightDark();
void CycleTheme(int dir);

} // namespace UI
