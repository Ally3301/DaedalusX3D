// UITheme: Color palettes for FrontendCTR (GPL-2.0-or-later).
// Current palette & styling derived from ctr64 by gl91306
// (https://github.com/gl91306/ctr64, GPL-2.0-or-later); scheduled for
// replacement with an original UI design — see CREDITS.md.
#include "UITheme.h"

namespace UI
{

namespace
{

static const ThemePalette kThemes[THEME_COUNT] = {
	// 0: 3DS Dark (CTR64-inspired matte charcoal + sky-blue focus ring)
	{
		"Dark",
		false,
		RGBA(27, 30, 35),        // bg
		RGBA(23, 26, 31),        // bgStripe
		RGBA(21, 24, 28),        // headerBg
		RGBA(44, 50, 58),        // headerRule
		RGBA(41, 46, 54),        // cardBg
		RGBA(52, 58, 68),        // cardBgAlt
		RGBA(58, 65, 76),        // cardBorder
		RGBA(0, 0, 0, 82),       // cardShadow
		RGBA(18, 52, 72),        // focusFill
		RGBA(29, 183, 242),      // focusBorder
		RGBA(29, 183, 242, 46),  // focusGlow
		RGBA(242, 245, 250),     // text
		RGBA(156, 165, 179),     // textMuted
		RGBA(106, 114, 128),     // textDim
		RGBA(29, 183, 242),      // accent
		RGBA(146, 154, 168),     // iconMuted
		RGBA(245, 197, 24),      // n64Yellow
		RGBA(38, 194, 102),      // n64Green
		RGBA(52, 126, 246),      // n64Blue
		RGBA(236, 72, 78),       // n64Red
		RGBA(56, 62, 72),        // badgeBg
		RGBA(29, 183, 242),      // badgeBgSel
		RGBA(18, 20, 24),        // hintBg
		RGBA(40, 45, 52),        // hintRule
		RGBA(236, 240, 246),     // hintText
		RGBA(236, 240, 246),     // hintGlyphBg
		RGBA(22, 25, 30),        // hintGlyphText
	},
	// 1: 3DS Light (Authentic 3DS System / CTR64 crisp white cards + cyan focus)
	{
		"Light",
		true,
		RGBA(236, 238, 242),     // bg
		RGBA(229, 232, 237),     // bgStripe
		RGBA(248, 249, 251),     // headerBg
		RGBA(212, 216, 224),     // headerRule
		RGBA(255, 255, 255),     // cardBg
		RGBA(242, 246, 250),     // cardBgAlt
		RGBA(218, 223, 230),     // cardBorder
		RGBA(24, 34, 52, 26),    // cardShadow
		RGBA(222, 244, 255),     // focusFill
		RGBA(24, 180, 242),      // focusBorder
		RGBA(24, 180, 242, 42),  // focusGlow
		RGBA(44, 48, 56),        // text
		RGBA(110, 118, 130),     // textMuted
		RGBA(154, 162, 174),     // textDim
		RGBA(22, 168, 236),      // accent
		RGBA(126, 134, 146),     // iconMuted
		RGBA(242, 188, 26),      // n64Yellow
		RGBA(30, 168, 86),       // n64Green
		RGBA(42, 108, 230),      // n64Blue
		RGBA(220, 56, 68),       // n64Red
		RGBA(226, 230, 238),     // badgeBg
		RGBA(24, 180, 242),      // badgeBgSel
		RGBA(46, 49, 54),        // hintBg
		RGBA(72, 76, 84),        // hintRule
		RGBA(244, 246, 250),     // hintText
		RGBA(244, 246, 250),     // hintGlyphBg
		RGBA(46, 49, 54),        // hintGlyphText
	},
	// 2: N64 Midnight (Deep navy slate + classic N64 cobalt & gold)
	{
		"Midnight",
		false,
		RGBA(14, 18, 28),        // bg
		RGBA(11, 15, 24),        // bgStripe
		RGBA(10, 13, 22),        // headerBg
		RGBA(34, 44, 66),        // headerRule
		RGBA(24, 31, 46),        // cardBg
		RGBA(32, 42, 62),        // cardBgAlt
		RGBA(44, 56, 82),        // cardBorder
		RGBA(0, 0, 0, 96),       // cardShadow
		RGBA(22, 54, 96),        // focusFill
		RGBA(56, 158, 255),      // focusBorder
		RGBA(56, 158, 255, 52),  // focusGlow
		RGBA(240, 244, 255),     // text
		RGBA(148, 164, 194),     // textMuted
		RGBA(96, 112, 142),      // textDim
		RGBA(56, 158, 255),      // accent
		RGBA(136, 150, 176),     // iconMuted
		RGBA(248, 202, 32),      // n64Yellow
		RGBA(40, 200, 112),      // n64Green
		RGBA(64, 132, 255),      // n64Blue
		RGBA(240, 74, 84),       // n64Red
		RGBA(34, 44, 66),        // badgeBg
		RGBA(56, 158, 255),      // badgeBgSel
		RGBA(10, 13, 20),        // hintBg
		RGBA(32, 42, 64),        // hintRule
		RGBA(236, 242, 252),     // hintText
		RGBA(236, 242, 252),     // hintGlyphBg
		RGBA(14, 18, 28),        // hintGlyphText
	},
	// 3: OLED Stealth (Pure black battery-saver canvas)
	{
		"Stealth",
		false,
		RGBA(0, 0, 0),           // bg
		RGBA(0, 0, 0),           // bgStripe
		RGBA(8, 10, 14),         // headerBg
		RGBA(28, 32, 40),        // headerRule
		RGBA(16, 19, 25),        // cardBg
		RGBA(24, 29, 38),        // cardBgAlt
		RGBA(34, 40, 50),        // cardBorder
		RGBA(0, 0, 0, 0),        // cardShadow
		RGBA(10, 38, 54),        // focusFill
		RGBA(46, 190, 245),      // focusBorder
		RGBA(46, 190, 245, 42),  // focusGlow
		RGBA(236, 240, 246),     // text
		RGBA(136, 146, 162),     // textMuted
		RGBA(84, 92, 106),       // textDim
		RGBA(46, 190, 245),      // accent
		RGBA(120, 128, 142),     // iconMuted
		RGBA(238, 190, 24),      // n64Yellow
		RGBA(34, 186, 98),       // n64Green
		RGBA(48, 122, 240),      // n64Blue
		RGBA(230, 68, 76),       // n64Red
		RGBA(24, 28, 36),        // badgeBg
		RGBA(46, 190, 245),      // badgeBgSel
		RGBA(6, 8, 10),          // hintBg
		RGBA(26, 30, 38),        // hintRule
		RGBA(224, 230, 238),     // hintText
		RGBA(224, 230, 238),     // hintGlyphBg
		RGBA(8, 10, 14),         // hintGlyphText
	},
};

int s_themeIdx = THEME_DARK;

} // namespace

const ThemePalette & GetTheme(int themeIdx)
{
	if (themeIdx < 0 || themeIdx >= THEME_COUNT)
		themeIdx = THEME_DARK;
	return kThemes[themeIdx];
}

const ThemePalette & ActiveTheme()
{
	return GetTheme(s_themeIdx);
}

int GetThemeIndex()
{
	return s_themeIdx;
}

void SetThemeIndex(int idx)
{
	if (idx < 0 || idx >= THEME_COUNT)
		idx = THEME_DARK;
	s_themeIdx = idx;
}

void ToggleLightDark()
{
	s_themeIdx = (s_themeIdx == THEME_LIGHT) ? THEME_DARK : THEME_LIGHT;
}

void CycleTheme(int dir)
{
	int next = (s_themeIdx + dir) % THEME_COUNT;
	if (next < 0) next += THEME_COUNT;
	s_themeIdx = next;
}

} // namespace UI
