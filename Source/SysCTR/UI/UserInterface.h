#pragma once

#include <3ds.h>

namespace UI
{
    bool Initialize();
    void Shutdown();

    // Select opens the pause menu. Input is quarantined until every digital
    // control and the circle pad have returned to neutral.
    void HandleGameInput(u32 keysDown, u32 keysHeld);
    bool IsCapturingInput();
    bool ConsumeGameInput(u32 keysHeld);
    void QuarantineGameInput();
    void ResetInput();

    void BeginRomSession();
    void EndRomSession();

    void RestoreRenderState();
}
