#include "RomSelector.h"
#include "UserInterface.h"
#include "FrontendCTR.h"

#include <3ds.h>
#include <string>

extern bool shouldQuit;

namespace UI
{
std::string DrawRomSelector()
{
    return FrontendCTR::RunRomSelector();
}
}
