#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdint.h>

#include "UIAtlas.h"
#include "UITheme.h"

namespace UI
{

struct HintItem
{
	const char * badge; // e.g., "A", "B", "Y", "L/R", "START"
	const char * label; // e.g., "OK", "Resume", "Light", "Quit"
};

// High-efficiency single-batch Citro3D 2D renderer for DaedalusX3D menus & HUD.
// All solid fills, rounded cards, 2px focus borders, circles, icons, and Roboto
// proportional glyphs share the single UIAtlas texture (1 draw call per screen!).
namespace Renderer
{
	bool EnsureMenuPool();
	void ReleaseMenuPool();
	bool EnsureHudPool();
	void ReleaseHudPool();
	void Shutdown();

	// Frame lifecycle for full two-screen menus (ROM Browser, Pause, Settings).
	void BeginFrame();
	void BeginScreen(gfxScreen_t screen, int w, int h, uint32_t clearColABGR);
	void EndFrame();

	// Lightweight in-game bottom-screen pass (uses dedicated 24 KiB HUD vertex pool).
	bool BeginHudBottomPass();
	void EndHudBottomPass();

	void FlushBatch();
	void SetClipRect(bool enable, int x, int y, int w, int h, int screenW, int screenH);

	// Low-level batched primitives
	void Quad(float x, float y, float w, float h, uint32_t col);
	void QuadVGrad(float x, float y, float w, float h, uint32_t topCol, uint32_t botCol);
	void QuadGrad(float x, float y, float w, float h,
	              uint32_t cTL, uint32_t cTR, uint32_t cBL, uint32_t cBR);
	void TexturedQuad(C3D_Tex * tex, float x, float y, float w, float h,
	                  float u0, float v0, float u1, float v1, uint32_t col = 0xFFFFFFFFu);

	// Smooth SDF rounded primitives & circles (from UIAtlas, zero extra draw calls!)
	void RoundedRect(float x, float y, float w, float h, float radius, uint32_t fillCol);
	void RoundedBorder2(float x, float y, float w, float h, float radius, uint32_t borderCol);
	void RoundedBorder1(float x, float y, float w, float h, float radius, uint32_t borderCol);
	void Circle(float cx, float cy, float radius, uint32_t col);
	void Ring(float cx, float cy, float radius, uint32_t col);
	void Icon(EIconId icon, float x, float y, float size, uint32_t col);

	// Proportional Roboto typography
	float MeasureText(EFontSize font, const char * text, float scale = 1.0f);
	float LineHeight(EFontSize font, float scale = 1.0f);
	void  Text(EFontSize font, float x, float y, const char * text, uint32_t col, float scale = 1.0f);
	void  TextCentered(EFontSize font, float cx, float y, const char * text, uint32_t col, float scale = 1.0f);
	void  TextRight(EFontSize font, float rightX, float y, const char * text, uint32_t col, float scale = 1.0f);
	void  TextEllipsis(EFontSize font, float x, float y, float maxW, const char * text, uint32_t col, float scale = 1.0f);
	void  TextWrapped(EFontSize font, float x, float y, float maxW, float lineH, const char * text, uint32_t col, float scale = 1.0f);

	// High-level 3DS / CTR64-style design system components
	void DrawBackdrop(float w, float h, const ThemePalette & t);
	void DrawHeaderBar(float w, const char * leftTitle, const char * rightSub, const ThemePalette & t);
	void DrawHintBar(float w, const HintItem * items, int count, const ThemePalette & t,
	                 const char * rightBadge = nullptr, const char * rightLabel = nullptr);
	void DrawCard(float x, float y, float w, float h, bool focused, bool pressed,
	              const ThemePalette & t, float radius = 8.0f);
	void DrawPill(float x, float y, float w, float h, const char * label,
	              uint32_t fillCol, uint32_t borderCol, uint32_t textCol, float scale = 0.85f);
	void DrawBatteryIcon(float x, float y, uint8_t level, bool charging, uint32_t col);
} // namespace Renderer

} // namespace UI
