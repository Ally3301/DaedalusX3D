#pragma once

#include <3ds.h>

namespace UI
{
    bool Initialize();
    void Shutdown();

    // Select or the bottom-screen Pause button opens the pause menu. Input is
    // quarantined until every digital control returns to neutral.
    void HandleGameInput(u32 keysDown, u32 keysHeld);
    void RequestPause();
    bool IsCapturingInput();
    bool ConsumeGameInput(u32 keysHeld);
    void QuarantineGameInput();
    void ResetInput();

    void BeginRomSession();
    void EndRomSession();

    void RestoreRenderState();
}
