#pragma once

class RomID;

namespace UI
{
    void BeginRomSession();
    void EndRomSession();
    void RunOptionsPage(RomID romId, bool applyRuntime);
    // Opens the pause menu when requested. Returns true when the menu
    // presented its own frame (the caller must skip its swap this frame).
    bool DrawInGameMenu();
}
