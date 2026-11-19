#pragma once

#include <3ds.h>
#include <citro2d.h>
extern "C" {
#include "theme.h"
}

#define PSX3DTheme() theme()

namespace UI
{
namespace Frontend
{
    // All frontend colours now come from the original PSX3D theme engine.
    // This keeps the palette roles and alpha values identical to PSX3D.
    #define BG (PSX3DTheme()->menu_bg)
    #define PANEL (PSX3DTheme()->card_top)
    #define PANEL2 (PSX3DTheme()->card_bottom)
    #define WHITE (PSX3DTheme()->white)
    #define MUTED (PSX3DTheme()->muted)
    #define DIM (theme_row(PSX3DTheme(), 0.0f, 255))
    #define ACCENT_R (PSX3DTheme()->accent[0])
    #define ACCENT_Y (PSX3DTheme()->accent[1])
    #define ACCENT_G (PSX3DTheme()->accent[2])
    #define ACCENT_B (PSX3DTheme()->accent[3])
    #define SELECTED (PSX3DTheme()->hl_top)
    #define SUCCESS (PSX3DTheme()->game.stats_focus)
    #define WARNING (PSX3DTheme()->message)
    #define VALUE (PSX3DTheme()->value)
    #define SECTION (PSX3DTheme()->section)
    #define RULE (PSX3DTheme()->section_rule)
    #define HIGHLIGHT_GLOW (PSX3DTheme()->hl_glow)
    #define HIGHLIGHT_TOP (PSX3DTheme()->hl_top)
    #define HIGHLIGHT_BOTTOM (PSX3DTheme()->hl_bottom)

    bool BeginFrame();
    void BeginTop();
    void BeginBottom();
    void EndFrame();

    void Rect(float x, float y, float w, float h, u32 color, float z = 0.8f);
    void GradientRect(float x, float y, float w, float h, u32 top, u32 bottom, float z = 0.8f);
    void Line(float x0, float y0, float x1, float y1, u32 color, float thickness = 1.0f);
    void Circle(float x, float y, float radius, u32 color);
    void Text(float x, float y, float scale, u32 color, const char *text);
    void TextCenter(float x, float y, float scale, u32 color, const char *text);
    void TextRight(float x, float y, float scale, u32 color, const char *text);
    void TextWrap(float x, float y, float scale, u32 color, float width, const char *text);

    void Header(const char *title, const char *right = nullptr);
    void Footer(const char *left, const char *middle = nullptr, const char *right = nullptr);
    void Row(float x, float y, float w, float h, const char *label,
        const char *value, bool selected, bool disabled = false);
    void ListHighlight(float x, float y, float w, float h, u32 accent);
    void RowText(float x, float y, float w, float h, const char *label,
        const char *value, u32 labelColor, u32 valueColor);
    void Pill(float x, float y, float w, const char *label, u32 color);
    void ActionBarButton(float x, float keyWidth, float w, const char *hint);
}
}
