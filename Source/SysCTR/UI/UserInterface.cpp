// Input plumbing and render-state handover between the emulator and the
// FrontendCTR menus. All drawing lives in FrontendCTR.cpp (picaGL only);
// nothing here touches citro2d/citro3d.
#include "stdafx.h"
#include "UserInterface.h"
#include "SysCTR/HLEGraphics/RendererCTR.h"

#include <3ds.h>
#include "SysCTR/Graphics/Citro3DContext.h"

namespace
{
    volatile bool s_pauseRequested = false;
    bool s_romActive = false;
    bool s_inputQuarantine = false;
    bool s_selectArmed = true;
    const u32 kFrontendKeys = KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT |
        KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_ZL | KEY_ZR | KEY_TOUCH;
}

namespace UI
{
void HandleGameInput(u32 keysDown, u32 keysHeld)
{
    (void)keysHeld;
    if (!s_romActive)
        return;

    // Select is a one-shot hotkey: require a physical release before it can
    // open the pause UI again (HID can be sampled more than once per frame).
    if ((keysHeld & KEY_SELECT) == 0)
        s_selectArmed = true;
    if (s_selectArmed && !s_pauseRequested && (keysDown & KEY_SELECT))
    {
        s_selectArmed = false;
        s_pauseRequested = true;
    }
}

bool IsCapturingInput()
{
    return s_pauseRequested;
}

bool ConsumeGameInput(u32 keysHeld)
{
    if (s_pauseRequested)
        return true;
    if (s_inputQuarantine)
    {
        // Do not leak the button that closed a menu into the game: release
        // all digital controls before gameplay resumes.
        if ((keysHeld & kFrontendKeys) == 0)
            s_inputQuarantine = false;
        return true;
    }
    return false;
}

void QuarantineGameInput()
{
    s_inputQuarantine = true;
}

void ResetInput()
{
    s_pauseRequested = false;
    s_selectArmed = true;
}

void BeginRomSession()
{
    s_romActive = true;
    s_inputQuarantine = true;
    ResetInput();
}

void EndRomSession()
{
    s_romActive = false;
    s_inputQuarantine = false;
    ResetInput();
}

void RestoreRenderState()
{
    // Back to the emulator's citro3d top-screen surface and default states after any menu pass.
    Citro3D::RestoreDefaultGameState();
    if (gRendererCTR != nullptr)
        gRendererCTR->RestoreRenderStates();
}
}
