#include "stdafx.h"
#include "InGameMenu.h"
#include "UserInterface.h"
#include "FrontendCTR.h"
#include "Core/ROM.h"

#include <3ds.h>

#include "BuildOptions.h"
#include "Config/ConfigOptions.h"
#include "Core/CPU.h"
#include "Core/ROM.h"
#include "Core/RomSettings.h"

extern bool shouldQuit;

namespace UI
{
// Returns true when the menu consumed the frame (the emulator must skip its
// swap this frame). The menu itself is implemented in FrontendCTR.cpp.
bool DrawInGameMenu()
{
    if (!UI::IsCapturingInput())
        return false;

    UI::ResetInput();
    FrontendCTR::RunPauseMenu(g_ROM.settings.GameName.c_str(), g_ROM.mRomID);
    UI::QuarantineGameInput();

    // The menu did its own presents; this frame is consumed.
    return true;
}
}
