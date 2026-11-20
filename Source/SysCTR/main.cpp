#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <dirent.h>

#include <3ds.h>
#include "SysCTR/Graphics/Citro3DContext.h"

#include "BuildOptions.h"
#include "Config/ConfigOptions.h"
#include "Core/Cheats.h"
#include "Core/CPU.h"
#include "DynaRec/FragmentCache.h"
#include "Core/Memory.h"
#include "Core/PIF.h"
#include "Core/RomSettings.h"
#include "Core/Save.h"
#include "Debug/DBGConsole.h"
#include "Debug/DebugLog.h"
#include "Graphics/GraphicsContext.h"
#include "HLEGraphics/TextureCache.h"
#include "Input/InputManager.h"
#include "Interface/RomDB.h"
#include "System/Paths.h"
#include "System/System.h"
#include "Test/BatchTest.h"

#include "UI/UserInterface.h"
#include "UI/RomSelector.h"
#include "UI/InGameMenu.h"
#include "UI/FrontendCTR.h"

#include "Utility/IO.h"
#include "Utility/Preferences.h"
#include "Utility/Profiler.h"
#include "Utility/Thread.h"
#include "Utility/Translate.h"
#include "Utility/Timer.h"
#include "Utility/ROMFile.h"
#include "Utility/MemoryCTR.h"

bool isN3DS = false;
bool shouldQuit = false;
bool gDynarecAvailable = false;

EAudioPluginMode enable_audio = APM_ENABLED_SYNC;

#ifdef DAEDALUS_LOG
void log2file(const char *format, ...) {
	char msg[512];
	va_list arg;
	va_start(arg, format);
	int written = vsnprintf(msg, sizeof(msg) - 1, format, arg);
	va_end(arg);

	if (written < 0)
		return;

	size_t used = static_cast<size_t>(written);
	if (used >= sizeof(msg) - 1)
		used = sizeof(msg) - 2;
	msg[used++] = '\n';
	msg[used] = '\0';

	FILE *log = fopen("sdmc:/DaedalusX64.log", "a");
	if (log != NULL) {
		fwrite(msg, 1, used, log);
		fclose(log);
	}
}
#endif

// Startup diagnostics are always written, not only in DAEDALUS_LOG builds:
// the executable-memory path depends on the firmware and CFW, so a failure
// has to be identifiable from the SD card.
static void LogStartup(const char * format, ...)
{
	char msg[256];
	va_list args;

	va_start(args, format);
	vsnprintf(msg, sizeof(msg), format, args);
	va_end(args);

	FILE * log = fopen("sdmc:/DaedalusX64.log", "a+");
	if (log != NULL)
	{
		fputs(msg, log);
		fputc('\n', log);
		fclose(log);
	}
}

// Reports why the dynarec is unavailable, then returns so the caller can
// continue in interpreter mode instead of exiting.
static void ReportInterpreterFallback(const char * reason, int code)
{
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);

	printf("Dynarec unavailable.\n\n%s\n", reason);
	if (code != 0)
	{
		printf("Code: %08x (%d)\n", (unsigned)code, code);
		printf("SVC access: %s\n", _DetectSvcAccess() ? "granted (Luma3DS)" : "not detected");
	}
	printf("\nContinuing in interpreter mode.\nGames will run slowly.\n");
	printf("Details: sdmc:/DaedalusX64.log\n");
	printf("\nPress START to continue.\n");

	u64 deadline = svcGetSystemTick() + SYSCLOCK_ARM11 * 8;
	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		if (svcGetSystemTick() > deadline)
			break;
		gspWaitForVBlank();
	}

	gfxExit();
}

//-----------------------------------------------------------------------------
// The syscore (core 1) is the only second core an Old 3DS title may use, and
// it is lent to us as a percentage of its time. Ask for as much as the
// exheader allows; the console refuses values above what the title was built
// with, so walk down until one is accepted.
//-----------------------------------------------------------------------------
static u32 gAppCpuLimit = 0;

static void BorrowSyscore()
{
	static const u32 want_old3ds[] = { 80, 70, 60, 50, 40, 35, 30, 25 };

	if (isN3DS)
	{
		APT_SetAppCpuTimeLimit(35);
	}
	else
	{
		for (u32 i = 0; i < sizeof(want_old3ds) / sizeof(want_old3ds[0]); i++)
		{
			if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(want_old3ds[i])))
				break;
		}
	}

	u32 granted = 0;
	APT_GetAppCpuTimeLimit(&granted);
	gAppCpuLimit = granted;
	LogStartup("startup: syscore time %u%%", granted);
}

//-----------------------------------------------------------------------------
// Hand the syscore back while we are in the background: the HOME Menu runs
// there and crawls otherwise. Not on sleep, which happens mid handshake.
//-----------------------------------------------------------------------------
static aptHookCookie gAptHookCookie;

static void OnAptHook(APT_HookType hook, void * param)
{
	if (gAppCpuLimit <= 30)
		return;

	if (hook == APTHOOK_ONSUSPEND)
		APT_SetAppCpuTimeLimit(30);
	else if (hook == APTHOOK_ONRESTORE)
		APT_SetAppCpuTimeLimit(gAppCpuLimit);
}

static bool CheckDSPFirmware()
{
	FILE *firmware = fopen("sdmc:/3ds/dspfirm.cdc", "rb");

	if(firmware != NULL)
	{
		fclose(firmware);
		return true;
	}

	// Continue without blocking startup if dspfirm.cdc is absent (e.g. fresh emulator setup).
	LogStartup("startup: sdmc:/3ds/dspfirm.cdc not found; audio disabled");
	enable_audio = APM_DISABLED;
	return true;
}

extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap_size;

static void EnableVfpRunFast()
{
	// Enable ARM11 VFPv2 RunFast mode (FZ=bit 24, DN=bit 25):
	// flushes denormals to zero and uses default NaNs, avoiding slow hardware
	// VFP support-code traps on denormal floats during N64 emulation.
	u32 fpscr = 0;
	__asm__ volatile("fmrx %0, fpscr" : "=r"(fpscr));
	fpscr |= (1u << 24) | (1u << 25);
	__asm__ volatile("fmxr fpscr, %0" : : "r"(fpscr));
}

static bool Initialize()
{
	EnableVfpRunFast();
	CheckDSPFirmware();

	// Detect the hardware before allocating the model-specific dynarec buffers.
	APT_CheckNew3DS(&isN3DS);

	// The executable code buffer must not be created by a global constructor:
	// svcControlProcessMemory is enabled only after this call.
	//
	// The SVC hack rewrites a kernel mask whose offset is not guaranteed by any
	// ABI, so its verdict is advisory: newer firmware and CFW already grant
	// these SVCs, and the buffer below is the authoritative test.
	const int svc_access = _DetectSvcAccess();
	const int svc_hack = _InitializeSvcHack();
	const int is_emulator = _RunningInEmulator();
	const int probe = _ProbeExecutableMemory();
	LogStartup("startup: svc access %d, svc hack %d, emulator %d, rwx probe %d, process heap %u KiB, linear heap %u KiB",
		svc_access, svc_hack, is_emulator, probe, (unsigned)(__ctru_heap_size / 1024),
		(unsigned)(__ctru_linear_heap_size / 1024));

	int code_buffer_error = 0;
	if (probe >= 0 && gFragmentCache.InitialiseCodeBuffer(&code_buffer_error))
	{
		gDynarecAvailable = true;
		LogStartup("startup: ARM dynarec enabled (emulator=%d)", is_emulator);
	}
	else
	{
		LogStartup("startup: code buffer failed (%08x), rwx probe %08x",
			(unsigned)code_buffer_error, (unsigned)probe);
		if (!is_emulator)
			ReportInterpreterFallback("Could not make memory executable.", code_buffer_error);
	}

	LogStartup("startup: dynarec buffers primary=%u MiB secondary=%u MiB",
		isN3DS ? 8u : 6u, 2u);
	romfsInit();
	
	osSetSpeedupEnable(true);

	aptHook(&gAptHookCookie, OnAptHook, nullptr);
	BorrowSyscore();
	
	gfxInitDefault();

	// Single GPU library: citro3d owns the GX queue end to end for both gameplay
	// (RendererCTR) and the two-screen frontend (FrontendCTR).
	if (!Citro3D::Init())
	{
		LogStartup("startup: Citro3D::Init failed");
		gfxExit();
		romfsExit();
		return false;
	}
	LogStartup("startup: Citro3D::Init ok");

	strcpy(gDaedalusExePath, DAEDALUS_CTR_PATH(""));
	strcpy(g_DaedalusConfig.mSaveDir, DAEDALUS_CTR_PATH("SaveGames/"));

	IO::Directory::EnsureExists( DAEDALUS_CTR_PATH("SaveStates/") );
	if (!System_Init())
	{
		LogStartup("startup: System_Init failed");
		Citro3D::Shutdown();
		gfxExit();
		romfsExit();
		return false;
	}
	LogStartup("startup: System_Init ok");

	if (!FrontendCTR::Initialise())
	{
		LogStartup("startup: FrontendCTR::Initialise failed");
		System_Finalize();
		Citro3D::Shutdown();
		gfxExit();
		romfsExit();
		return false;
	}
	LogStartup("startup: FrontendCTR::Initialise ok");
	return true;
}


void HandleEndOfFrame()
{
	shouldQuit = !aptMainLoop();
	
	if (shouldQuit)
	{
		CPU_Halt("Exiting");
	}
}

int main(int argc, char* argv[])
{
	IO::Filename fullpath;

	if (!Initialize())
		return 1;
	
	bool usedAutoboot = false;
	while(shouldQuit == false)
	{
		std::string rom;
		if (!usedAutoboot)
		{
			FILE * ab = fopen(DAEDALUS_CTR_PATH("autoboot.txt"), "r");
			if (ab != nullptr)
			{
				char line[256] = {};
				if (fgets(line, sizeof(line), ab) != nullptr)
				{
					size_t len = strlen(line);
					while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
						line[--len] = '\0';
					if (len > 0)
					{
						rom = line;
						usedAutoboot = true;
					}
				}
				fclose(ab);
			}
		}
		if (rom.empty())
			rom = UI::DrawRomSelector();
		if (rom.empty())
		{
			if (!aptMainLoop())
				shouldQuit = true;
			if (shouldQuit)
				break;
			continue;
		}
		int length = snprintf(fullpath, sizeof(fullpath), "%s", rom.c_str());
		if (length < 0 || static_cast<size_t>(length) >= sizeof(fullpath))
		{
			printf("ROM path is too long.\n");
			continue;
		}

		FrontendCTR::ReleaseAssets();
		bool keepRestarting = true;
		while (keepRestarting && !shouldQuit)
		{
			keepRestarting = false;
			if (System_Open(fullpath))
			{
				UI::BeginRomSession();
				CPU_Run();
				UI::EndRomSession();
				System_Close();
				if (!shouldQuit && FrontendCTR::ConsumeResetRequest())
					keepRestarting = true;
			}
			else
			{
				printf("Failed to open ROM: %s\n", fullpath);
			}
		}
	}
	
	aptUnhook(&gAptHookCookie);
	if (gAppCpuLimit > 30)
		APT_SetAppCpuTimeLimit(30);

	FrontendCTR::Shutdown();
	System_Finalize();
	gFragmentCache.FinaliseCodeBuffer();
	Citro3D::Shutdown();
	gfxExit();
	romfsExit();

	return 0;
}
