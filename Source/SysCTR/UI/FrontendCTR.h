// FrontendCTR: the DaedalusX64 3DS frontend, written directly against
// picaGL — no citro2d/citro3d and no ported PSX3D code. The PSX3D-based
// frontend mixed two GPU libraries into one GX command queue and crashed
// on hardware in every configuration; this one uses the single library the
// emulator already owns, so there is nothing to interleave.
//
// Scope: ROM list (touch + d-pad), game info, pause menu (resume, save /
// load state, options, close ROM). Drawn with immediate-mode GL quads and
// the system font rendered by ImGui-free simple bitmap text.
//
// All draws go through ui_begin_frame() / ui_end_frame(): a plain picaGL
// frame on both screens, exactly the pattern the pre-PSX3D frontend used
// (and which ran on hardware).
#ifndef FRONTEND_CTR_H
#define FRONTEND_CTR_H

#include <3ds.h>
#include <string>

class RomID;

namespace FrontendCTR
{
	// One-time setup/teardown of the frontend's own resources.
	bool  Initialise();
	void  Shutdown();

	// Blocking ROM selector. Returns the full path of the game to launch,
	// or an empty string to quit the app.
	std::string RunRomSelector();

	// In-game pause menu (called from the emulator's frame loop when the
	// user presses SELECT). Returns when the user resumes or closes.
	void RunPauseMenu(const char * gameName, const RomID & romId);

	// Per-ROM options editor (reached from the pause menu). Blocking.
	void RunOptionsPage(const RomID & romId, bool applyRuntime);

	// True while the pause menu is open (input quarantine window).
	bool IsMenuOpen();

	// Release any resources that must not be held during emulation.
	void ReleaseAssets();
}

#endif // FRONTEND_CTR_H
