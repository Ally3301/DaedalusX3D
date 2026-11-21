// FrontendCTR: Modern 3DS-native dual-screen frontend built on Citro3D.
// Powered by modular UITheme, UIAtlas (Roboto-Medium + SDF shapes/icons),
// UIRenderer (single-batch 9-slice & typography), UITouchInput, and
// TouchControllerCTR (Phase 2 physical-extension bottom-screen controls).

#include "stdafx.h"
#include "FrontendCTR.h"
#include "UITheme.h"
#include "UIAtlas.h"
#include "UIRenderer.h"
#include "UITouchInput.h"
#include "TouchControllerCTR.h"
#include "CoverScraperCTR.h"
#include "UISound.h"
#include "UserInterface.h"
#include "SysCTR/HLEAudio/AudioOutput.h"

#include <3ds.h>
#include <citro3d.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <malloc.h>
#include <string>

#include "Core/CPU.h"
#include "Core/ROM.h"
#include "Core/RomSettings.h"
#include "Config/ConfigOptions.h"
#include "Graphics/NativeTexture.h"
#include "HLEGraphics/BaseRenderer.h"
#include "Interface/RomDB.h"
#include "Utility/IO.h"
#include "Utility/Preferences.h"
#include "Utility/ROMFile.h"

extern bool isN3DS;
extern bool shouldQuit;
extern bool gDynarecAvailable;
extern EFrameskipValue gFrameskipValue;
extern f32 gZoomX;
extern float gCurrentFramerate;

namespace
{

using namespace UI;
using namespace UI::Renderer;

// ---------------------------------------------------------------- helpers
static void CopyStr(char * dst, size_t dstSize, const char * src)
{
	if (!dst || dstSize == 0) return;
	if (!src) { dst[0] = '\0'; return; }
	size_t n = strlen(src);
	if (n >= dstSize) n = dstSize - 1;
	if (n > 0) memmove(dst, src, n);
	dst[n] = '\0';
}

static inline void SpringStep(float & x, float & v, float target, float k, float damp)
{
	const float dt = 1.0f / 60.0f;
	v += ((target - x) * k - v * damp) * dt;
	x += v * dt;
}

static inline float EaseOutCubic(float t)
{
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;
	const float inv = 1.0f - t;
	return 1.0f - inv * inv * inv;
}

static inline uint32_t ScaleAlpha(uint32_t col, float mul)
{
	if (mul <= 0.0f) return WithAlpha(col, 0);
	if (mul >= 1.0f) return col;
	const uint8_t a = (uint8_t)((float)(col >> 24) * mul + 0.5f);
	return WithAlpha(col, a);
}

static void UpdateTouchFromHid(TouchTracker & t, u32 held)
{
	if (held & KEY_TOUCH)
	{
		touchPosition tp;
		hidTouchRead(&tp);
		t.UpdateRaw(true, tp.px, tp.py);
	}
	else
	{
		t.UpdateRaw(false, 0, 0);
	}
}

// Capture a 2x box-filtered soft-focus snapshot (200x120 packed in 256x128 RGBA8888 = 128 KiB)
// of the current N64 top LCD framebuffer to display behind the animated Pause Menu overlay.
CRefPtr<CNativeTexture> CaptureTopScreenSnapshot()
{
	u16 fbW = 0, fbH = 0;
	const u8 * fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
	if (!fb || fbW < 240 || fbH < 400)
		return nullptr;

	constexpr u32 kTexW = 256;
	constexpr u32 kTexH = 128;
	u32 * pixels = (u32 *)calloc(kTexW * kTexH, sizeof(u32));
	if (!pixels)
		return nullptr;

	// 3DS top framebuffer is portrait-rotated (240 wide x 400 tall, BGR888).
	// Landscape (lx, ly) maps to fb[(lx * 240 + (239 - ly)) * 3].
	for (int dy = 0; dy < 120; ++dy)
	{
		const int ly0 = dy * 2;
		const int ly1 = ly0 + 1;
		const int py0 = 239 - ly0;
		const int py1 = 239 - ly1;
		for (int dx = 0; dx < 200; ++dx)
		{
			const int lx0 = dx * 2;
			const int lx1 = lx0 + 1;
			const u8 * p00 = fb + (lx0 * 240 + py0) * 3;
			const u8 * p10 = fb + (lx1 * 240 + py0) * 3;
			const u8 * p01 = fb + (lx0 * 240 + py1) * 3;
			const u8 * p11 = fb + (lx1 * 240 + py1) * 3;

			const u8 b = (u8)(((u32)p00[0] + p10[0] + p01[0] + p11[0]) >> 2);
			const u8 g = (u8)(((u32)p00[1] + p10[1] + p01[1] + p11[1]) >> 2);
			const u8 r = (u8)(((u32)p00[2] + p10[2] + p01[2] + p11[2]) >> 2);
			pixels[dy * kTexW + dx] = RGBA(r, g, b, 255);
		}
	}

	CRefPtr<CNativeTexture> tex = CNativeTexture::Create(kTexW, kTexH, TexFmt_8888);
	if (tex != nullptr && tex->HasData())
	{
		tex->SetData(pixels, nullptr);
		tex->ApplyParams(GPU_LINEAR, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	free(pixels);
	return tex;
}

// ---------------------------------------------------------------- system telemetry
struct SystemStatus
{
	uint8_t batteryLevel;
	bool    charging;
	char    clockText[16];
	char    memSummary[64];
	int     refreshCountdown;
};

SystemStatus s_sysStatus = { 4, false, "", "", 0 };

void RefreshMemorySummary()
{
	const struct mallinfo mi = mallinfo();
	const u32 heapCap = envGetHeapSize();
	const u32 heapFree = (heapCap > (u32)mi.uordblks) ? (heapCap - (u32)mi.uordblks) : 0;
	const u32 linFree = linearSpaceFree();
	snprintf(s_sysStatus.memSummary, sizeof(s_sysStatus.memSummary),
	         "RAM %.1f MB  |  Linear %.1f MB",
	         heapFree / (1024.0f * 1024.0f), linFree / (1024.0f * 1024.0f));
}

void TickSystemStatus(bool force = false)
{
	if (!force && s_sysStatus.refreshCountdown > 0)
	{
		--s_sysStatus.refreshCountdown;
		return;
	}
	s_sysStatus.refreshCountdown = 180;

	time_t now = time(nullptr);
	struct tm * lt = localtime(&now);
	if (lt)
		snprintf(s_sysStatus.clockText, sizeof(s_sysStatus.clockText), "%02d:%02d", lt->tm_hour, lt->tm_min);
	else
		s_sysStatus.clockText[0] = '\0';

	if (R_SUCCEEDED(ptmuInit()))
	{
		u8 lvl = 4, chg = 0;
		if (R_SUCCEEDED(PTMU_GetBatteryLevel(&lvl)))
			s_sysStatus.batteryLevel = lvl;
		if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&chg)))
			s_sysStatus.charging = (chg != 0);
		ptmuExit();
	}
}

// ---------------------------------------------------------------- ROM library
struct FrontendRomInfo
{
	RomID        romId;
	u32          size;
	bool         hasDetails;
	bool         favorite;
	char         filename[96];
	char         title[64];
	IO::Filename path;
	char         preview[96];
	char         comment[96];
	char         country[8];
};

constexpr int kMaxRoms = 256;
FrontendRomInfo s_roms[kMaxRoms];
int  s_romCount    = 0;
bool s_romsScanned = false;
int  s_viewOrder[kMaxRoms];
int  s_viewCount   = 0;

enum SortMode { SORT_AZ = 0, SORT_FAV, SORT_SIZE, SORT_COUNT };
static const char * const kSortNames[SORT_COUNT] = { "A-Z", "Favorites", "Size" };
int   s_sortMode       = SORT_AZ;
int   s_selected       = 0;
float s_hlPos          = 0.0f;
float s_hlVel          = 0.0f;
float s_scrollPos      = 0.0f;
float s_scrollVel      = 0.0f;
bool  s_initialised    = false;
bool  s_resetRequested = false;

CRefPtr<CNativeTexture> s_previewTex;
int s_previewRomIdx        = -1;
int s_previewPendingRomIdx = -1;
int s_previewDelayFrames   = 0;

const char * const kFrontendConfigPath = DAEDALUS_CTR_PATH("frontend.ini");
char s_lastSelectedFile[96] = "";
constexpr int kMaxFavs = 128;
char s_favList[kMaxFavs][96];
int  s_favCount = 0;

void LoadFrontendConfig()
{
	s_favCount = 0;
	s_lastSelectedFile[0] = '\0';
	SetThemeIndex(THEME_DARK);
	Sound::SetVolume(SFX_VOL_MEDIUM);

	FILE * f = fopen(kFrontendConfigPath, "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "theme=", 6))
		{
			const int v = atoi(line + 6);
			if (v >= 0 && v < THEME_COUNT)
				SetThemeIndex(v);
		}
		else if (!strncmp(line, "sort=", 5))
		{
			const int v = atoi(line + 5);
			if (v >= 0 && v < SORT_COUNT)
				s_sortMode = v;
		}
		else if (!strncmp(line, "ui_sfx=", 7))
		{
			const int v = atoi(line + 7);
			if (v >= 0 && v < SFX_VOL_COUNT)
				Sound::SetVolume((ESfxVolume)v);
		}
		else if (!strncmp(line, "last=", 5))
		{
			CopyStr(s_lastSelectedFile, sizeof(s_lastSelectedFile), line + 5);
		}
		else if (!strncmp(line, "fav=", 4) && line[4] && s_favCount < kMaxFavs)
		{
			CopyStr(s_favList[s_favCount++], sizeof(s_favList[0]), line + 4);
		}
	}
	fclose(f);
}

void SaveFrontendConfig()
{
	FILE * f = fopen(kFrontendConfigPath, "w");
	if (!f) return;
	fprintf(f, "theme=%d\nsort=%d\nui_sfx=%d\nlast=%s\n",
	        GetThemeIndex(), s_sortMode, (int)Sound::GetVolume(), s_lastSelectedFile);
	for (int i = 0; i < s_romCount; ++i)
	{
		if (s_roms[i].favorite)
			fprintf(f, "fav=%s\n", s_roms[i].filename);
	}
	fclose(f);
}

bool IsSavedFavorite(const char * filename)
{
	for (int i = 0; i < s_favCount; ++i)
		if (strcmp(s_favList[i], filename) == 0)
			return true;
	return false;
}

const char * CountryName(u8 country_id)
{
	switch (country_id)
	{
	case 'A': case 'E': return "USA";
	case 'J': return "JPN";
	case 'D': return "GER";
	case 'F': return "FRA";
	case 'I': return "ITA";
	case 'P': case 'S': case 'U': case 'X': case 'Y': return "EUR";
	default: return "---";
	}
}

void RebuildView()
{
	s_viewCount = 0;
	for (int i = 0; i < s_romCount; ++i)
	{
		if (s_sortMode == SORT_FAV && !s_roms[i].favorite)
			continue;
		s_viewOrder[s_viewCount++] = i;
	}
	if (s_sortMode == SORT_FAV && s_viewCount == 0)
	{
		for (int i = 0; i < s_romCount; ++i)
			s_viewOrder[s_viewCount++] = i;
	}

	for (int a = 1; a < s_viewCount; ++a)
	{
		const int key = s_viewOrder[a];
		int b = a;
		while (b > 0)
		{
			const FrontendRomInfo & prev = s_roms[s_viewOrder[b - 1]];
			const FrontendRomInfo & cur  = s_roms[key];
			bool swap = false;
			if (s_sortMode == SORT_SIZE)
			{
				swap = (prev.size < cur.size) ||
				       (prev.size == cur.size && strcasecmp(prev.title, cur.title) > 0);
			}
			else if (s_sortMode == SORT_AZ && prev.favorite != cur.favorite)
			{
				swap = !prev.favorite;
			}
			else
			{
				swap = strcasecmp(prev.title, cur.title) > 0;
			}
			if (!swap) break;
			s_viewOrder[b] = s_viewOrder[b - 1];
			--b;
		}
		s_viewOrder[b] = key;
	}

	if (s_selected >= s_viewCount) s_selected = s_viewCount > 0 ? s_viewCount - 1 : 0;
	if (s_selected < 0) s_selected = 0;
	s_hlPos = (float)s_selected;
	s_hlVel = 0.0f;
}

void ReleasePreview()
{
	s_previewTex = nullptr;
	s_previewRomIdx = -1;
	s_previewPendingRomIdx = -1;
	s_previewDelayFrames = 0;
}

void RequestPreviewForRom(int romIdx, bool immediate = false)
{
	if (romIdx == s_previewRomIdx && s_previewPendingRomIdx == -1)
		return;
	if (immediate)
	{
		s_previewPendingRomIdx = romIdx;
		s_previewDelayFrames = 0;
		return;
	}
	if (s_previewPendingRomIdx != romIdx)
	{
		s_previewPendingRomIdx = romIdx;
		s_previewDelayFrames = 8;
	}
}

void BuildPreviewPathForRom(const FrontendRomInfo & r, IO::Filename & outPath)
{
	if (r.preview[0] != '\0')
	{
		IO::Path::Combine(outPath, DAEDALUS_CTR_PATH("Resources/Preview/"), r.preview);
		return;
	}
	char stem[104];
	CopyStr(stem, sizeof(stem) - 4, r.filename);
	char * dot = strrchr(stem, '.');
	if (dot && dot != stem) *dot = '\0';
	strcat(stem, ".png");
	IO::Path::Combine(outPath, DAEDALUS_CTR_PATH("Resources/Preview/"), stem);
}

void TickPreviewLoader()
{
	if (s_previewPendingRomIdx == -1)
		return;
	if (s_previewDelayFrames > 0)
	{
		--s_previewDelayFrames;
		return;
	}
	const int romIdx = s_previewPendingRomIdx;
	s_previewPendingRomIdx = -1;
	if (romIdx == s_previewRomIdx && s_previewTex != nullptr)
		return;

	s_previewTex = nullptr;
	s_previewRomIdx = romIdx;
	if (romIdx < 0 || romIdx >= s_romCount)
		return;
	const FrontendRomInfo & r = s_roms[romIdx];

	IO::Filename previewPath;
	BuildPreviewPathForRom(r, previewPath);
	if (IO::File::Exists(previewPath))
	{
		s_previewTex = CNativeTexture::CreateFromPng(previewPath, TexFmt_5551);
		if (s_previewTex != nullptr && s_previewTex->HasData())
			s_previewTex->ApplyParams(GPU_LINEAR, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
}

void ScanRoms(bool force)
{
	if (s_romsScanned && !force)
		return;

	ReleasePreview();
	s_romCount = 0;
	IO::FindHandleT handle;
	IO::FindDataT data;
	const char * dir = DAEDALUS_CTR_PATH("Roms/");
	if (IO::FindFileOpen(dir, &handle, data))
	{
		do {
			if (!IsRomfilename(data.Name)) continue;
			if (s_romCount >= kMaxRoms) break;

			FrontendRomInfo & info = s_roms[s_romCount];
			info = FrontendRomInfo();
			CopyStr(info.filename, sizeof(info.filename), data.Name);
			IO::Path::Combine(info.path, dir, data.Name);
			info.favorite = IsSavedFavorite(info.filename);

			RomID rom_id;
			u32 rom_size = 0;
			ECicType cic = CIC_UNKNOWN;
			if (ROM_GetRomDetailsByFilename(info.path, &rom_id, &rom_size, &cic))
			{
				info.romId = rom_id;
				info.size = rom_size;
				info.hasDetails = true;
				CopyStr(info.country, sizeof(info.country), CountryName(rom_id.CountryID));

				RomSettings settings;
				if (CRomSettingsDB::Get() && CRomSettingsDB::Get()->GetSettings(rom_id, &settings))
				{
					CopyStr(info.title, sizeof(info.title), settings.GameName.c_str());
					CopyStr(info.preview, sizeof(info.preview), settings.Preview.c_str());
					CopyStr(info.comment, sizeof(info.comment), settings.Comment.c_str());
				}
				else
				{
					settings.Reset();
					std::string name;
					if (ROM_GetRomName(info.path, name) && !name.empty())
						CopyStr(info.title, sizeof(info.title), name.c_str());
					else
						CopyStr(info.title, sizeof(info.title), info.filename);
					settings.GameName = info.title;
					if (CRomSettingsDB::Get())
						CRomSettingsDB::Get()->SetSettings(rom_id, settings);
				}
			}
			else
			{
				CopyStr(info.title, sizeof(info.title), info.filename);
				CopyStr(info.country, sizeof(info.country), "---");
			}
			++s_romCount;
		} while (IO::FindFileNext(handle, data));
		IO::FindFileClose(handle);
	}

	s_romsScanned = true;
	RebuildView();

	if (s_lastSelectedFile[0] != '\0')
	{
		for (int i = 0; i < s_viewCount; ++i)
		{
			if (strcmp(s_roms[s_viewOrder[i]].filename, s_lastSelectedFile) == 0)
			{
				s_selected = i;
				s_hlPos = (float)i;
				if (s_selected >= 5)
					s_scrollPos = (float)(s_selected - 4);
				break;
			}
		}
	}
}

// ---------------------------------------------------------------- options metadata
struct OptionEntry
{
	const char * label;
	const char * help;
	bool         live;
};

static const OptionEntry kOptions[] = {
	{ "DynaRec",              "Dynamic ARM recompilation. Huge speedup; applies on next ROM launch.", false },
	{ "Loop optimisation",    "Optimises hot loops in generated ARM code for higher FPS.", false },
	{ "Double precision",     "Higher FPU precision. Disable on Old 3DS for extra speed.", false },
	{ "Memory access opt.",   "Fast direct RDRAM paths in DynaRec. Recommended On.", false },
	{ "OS patches",           "High-level N64 OS hooks. Big speedup on Old 3DS.", false },
	{ "Speed limiter",        "Cap emulation speed to native N64 refresh rate.", true  },
	{ "Double display",       "Extra display list path needed by a few games.", false },
	{ "Clean scene",          "Clears stale framebuffer content each scene.", true  },
	{ "Clear depth buffer",   "Clears depth buffer each configured frame.", true  },
	{ "Frameskip",            "Skip rendering frames when slow. Auto 2 is best on Old 3DS.", true  },
	{ "Texture checks",       "Re-hash cached textures interval. Every 8/16 saves ARM11 CPU.", true  },
	{ "Video rate match",     "Sync VI rate to actual emulation speed.", true  },
	{ "Fog",                  "Enable N64 hardware fog emulation.", true  },
	{ "Aspect ratio",         "Top screen picture aspect ratio (console-wide).", true  },
	{ "Zoom",                 "Horizontal picture zoom factor (live).", true  },
	{ "Audio output",         "Async offloads 44kHz mixing to Core 1; Off gives max FPS.", true  },
	{ "Audio rate match",     "Match audio pacing to emulation speed.", true  },
	{ "Cheats",               "Enable cheat codes for this ROM.", false },
	{ "Show FPS",             "Enable frame rate measurement & perf log (console-wide).", true  },
	{ "UI Theme",             "Switch between Dark, Light, Midnight, and Stealth themes.", true  },
	{ "CPU Speed Hack",       "2x/3x scales DynaRec cycle counter for big FPS boosts.", true  },
	{ "UI Sound FX",          "Frontend menu and touch HUD sound effects volume (console-wide).", true },
};
constexpr int kNumOptions = (int)(sizeof(kOptions) / sizeof(kOptions[0]));

const char * FrameskipName(int v)
{
	static const char * const names[] = { "Off", "Auto 1", "Auto 2", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
	return (v >= 0 && v < 12) ? names[v] : "?";
}
const char * TexHashName(int v)
{
	static const char * const names[] = { "Off", "Every 1", "Every 2", "Every 4", "Every 8", "Every 16", "Every 32" };
	return (v >= 0 && v < 7) ? names[v] : "?";
}
const char * AudioName(int v)
{
	static const char * const names[] = { "Off", "Async", "Sync" };
	return (v >= 0 && v < 3) ? names[v] : "?";
}
const char * ViewportName(int v)
{
	switch (v)
	{
	case VT_UNSCALED_4_3:  return "4:3";
	case VT_FULLSCREEN:    return "Full";
	case VT_FULLSCREEN_HD: return "Full HD";
	case VT_SCALED_4_3:    return "4:3 Scaled";
	}
	return "?";
}

void DrawTopStatusHeader(const char * leftTitle, const ThemePalette & t)
{
	Quad(0.0f, 0.0f, 400.0f, 24.0f, t.headerBg);
	Quad(0.0f, 24.0f, 400.0f, 1.0f, t.headerRule);
	Text(FONT_TITLE, 10.0f, 4.0f, leftTitle, t.text, 0.92f);

	DrawBatteryIcon(374.0f, 8.0f, s_sysStatus.batteryLevel, s_sysStatus.charging, t.textMuted);
	if (s_sysStatus.clockText[0])
		TextRight(FONT_BODY, 368.0f, 6.0f, s_sysStatus.clockText, t.textMuted, 0.88f);
	TextRight(FONT_BODY, 330.0f, 6.0f, isN3DS ? "N3DS" : "O3DS", t.textDim, 0.85f);
}

// Render the frozen game screenshot on the top screen with an animated frosted overlay.
void DrawFrostedGameBackdrop(const CRefPtr<CNativeTexture> & snap, float ease, const ThemePalette & t)
{
	if (snap != nullptr && snap->HasData())
	{
		const float u1 = 200.0f / 256.0f;
		const float v1 = 120.0f / 128.0f;
		TexturedQuad(snap->GetC3DTexture(), 0.0f, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, u1, v1, RGBA(255, 255, 255, 255));
		// Smooth animated vertical frosted vignette
		const uint8_t topA = (uint8_t)(130.0f * ease);
		const uint8_t botA = (uint8_t)(195.0f * ease);
		QuadVGrad(0.0f, 0.0f, 400.0f, 240.0f, RGBA(10, 14, 22, topA), RGBA(8, 11, 18, botA));
	}
	else
	{
		DrawBackdrop(400.0f, 240.0f, t);
	}
}

// ---------------------------------------------------------------- Live Touch Controls screen
void RunControlsPage(const char * gameName, const CRefPtr<CNativeTexture> & freezeSnap)
{
	TouchPadConfig cfg = TouchController::Engine().GetConfig();

	static const char * const kLabels[6] = {
		"Bottom Screen Mode",
		"Handedness",
		"Z-Trigger Mode",
		"Stick Deadzone",
		"Shooter Sensitivity",
		"Invert Camera Y"
	};
	constexpr int kCount = 6;
	constexpr float kListY     = 31.0f;
	constexpr float kRowStride = 30.0f;
	constexpr float kRowH      = 26.0f;

	int   sel    = 0;
	float hlPos  = 0.0f;
	float hlVel  = 0.0f;
	float enterT = 0.0f;

	RepeatTimer rep;
	rep.Reset();
	TouchTracker touch;
	touch.Reset();

	static const HintItem kCtrlHints[3] = {
		{ "A", "Change" },
		{ "B", "Back"   },
		{ "Y", "Theme"  }
	};

	while (aptMainLoop() && !shouldQuit)
	{
		hidScanInput();
		u32 down = hidKeysDown();
		const u32 held = hidKeysHeld();
		UpdateTouchFromHid(touch, held);
		TickSystemStatus(false);

		if (enterT < 1.0f)
		{
			enterT += 1.0f / 12.0f;
			if (enterT > 1.0f) enterT = 1.0f;
		}
		const float ease = EaseOutCubic(enterT);

		int pressedRow = -1;
		if (touch.active && !touch.dragged && touch.curY >= (int)kListY && touch.curY < (int)(kListY + kCount * kRowStride))
		{
			const int r = (touch.curY - (int)kListY) / (int)kRowStride;
			if (r >= 0 && r < kCount) pressedRow = r;
		}
		if (touch.justReleased && !touch.dragged)
		{
			if (touch.releaseY >= 218)
			{
				if (touch.releaseX < 105) down |= KEY_A;
				else if (touch.releaseX < 215) down |= KEY_B;
				else down |= KEY_Y;
			}
			else if (touch.releaseY >= (int)kListY && touch.releaseY < (int)(kListY + kCount * kRowStride))
			{
				const int r = (touch.releaseY - (int)kListY) / (int)kRowStride;
				if (r >= 0 && r < kCount)
				{
					sel = r;
					down |= KEY_A;
				}
			}
		}

		if (down & KEY_B)
		{
			Sound::Play(SFX_BACK);
			break;
		}
		if (down & KEY_Y)
		{
			ToggleLightDark();
			SaveFrontendConfig();
			Sound::Play(ActiveTheme().isLight ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
		}
		if (rep.Poll(down, held, KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN, 0))
		{
			sel = (sel + 1) % kCount;
			Sound::Play(SFX_NAVIGATE, 0.98f);
		}
		if (rep.Poll(down, held, KEY_UP | KEY_DUP | KEY_CPAD_UP, 1))
		{
			sel = (sel + kCount - 1) % kCount;
			Sound::Play(SFX_NAVIGATE, 1.04f);
		}

		const int dir = (down & (KEY_A | KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_R)) ? 1 :
		                (down & (KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT | KEY_L)) ? -1 : 0;
		if (dir != 0)
		{
			const float stepPitch = (dir > 0) ? 1.06f : 0.94f;
			switch (sel)
			{
			case 0:
				cfg.layout = (ETouchPadLayout)(((int)cfg.layout + dir + TOUCH_LAYOUT_COUNT) % TOUCH_LAYOUT_COUNT);
				Sound::Play(SFX_ADJUST, stepPitch);
				break;
			case 1:
				cfg.leftHanded = !cfg.leftHanded;
				Sound::Play(cfg.leftHanded ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
				break;
			case 2:
				cfg.zToggleMode = !cfg.zToggleMode;
				Sound::Play(cfg.zToggleMode ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
				break;
			case 3:
			{
				const float prevDz = cfg.stickDeadzone;
				cfg.stickDeadzone += dir * 0.05f;
				if (cfg.stickDeadzone < 0.15f) cfg.stickDeadzone = 0.15f;
				if (cfg.stickDeadzone > 0.40f) cfg.stickDeadzone = 0.40f;
				Sound::Play((fabsf(cfg.stickDeadzone - prevDz) > 0.001f) ? SFX_ADJUST : SFX_ERROR, stepPitch);
				break;
			}
			case 4:
			{
				const float prevSens = cfg.stickSens;
				cfg.stickSens += dir * 0.10f;
				if (cfg.stickSens < 0.60f) cfg.stickSens = 0.60f;
				if (cfg.stickSens > 1.60f) cfg.stickSens = 1.60f;
				Sound::Play((fabsf(cfg.stickSens - prevSens) > 0.001f) ? SFX_ADJUST : SFX_ERROR, stepPitch);
				break;
			}
			case 5:
				cfg.invertY = !cfg.invertY;
				Sound::Play(cfg.invertY ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
				break;
			}
			TouchController::Engine().SetConfig(cfg);
			TouchController::SaveConfig();
		}

		SpringStep(hlPos, hlVel, (float)sel, 320.0f, 26.0f);

		const ThemePalette & t = ActiveTheme();
		const float slideY = (1.0f - ease) * 14.0f;
		BeginFrame();

		// ---- Bottom screen: Touch extension settings
		BeginScreen(GFX_BOTTOM, 320, 240, t.bg);
		DrawBackdrop(320.0f, 240.0f, t);
		DrawHeaderBar(320.0f, "Controls", gameName ? gameName : "N64", t);

		// Draw base cards first, then spring-animated floating focus ring on top
		for (int i = 0; i < kCount; ++i)
		{
			const float y = kListY + i * kRowStride + slideY;
			const bool focused = (i == sel);
			DrawCard(8.0f, y, 304.0f, kRowH, false, (i == pressedRow && !focused), t, 7.0f);
			if (focused)
				RoundedRect(8.0f, y, 304.0f, kRowH, 7.0f, ScaleAlpha(t.focusFill, ease));

			Text(FONT_BODY, 18.0f, y + 6.5f, kLabels[i], focused ? t.text : t.textMuted, 0.92f);

			char val[32] = "";
			switch (i)
			{
			case 0: snprintf(val, sizeof(val), "%s", TouchPadEngine::LayoutShortName(cfg.layout)); break;
			case 1: snprintf(val, sizeof(val), "%s", cfg.leftHanded ? "Left" : "Right"); break;
			case 2: snprintf(val, sizeof(val), "%s", cfg.zToggleMode ? "Toggle" : "Hold"); break;
			case 3: snprintf(val, sizeof(val), "%d%%", (int)(cfg.stickDeadzone * 100.0f + 0.5f)); break;
			case 4: snprintf(val, sizeof(val), "%.1fx", cfg.stickSens); break;
			case 5: snprintf(val, sizeof(val), "%s", cfg.invertY ? "ON" : "OFF"); break;
			}
			const float pw = fmaxf(62.0f, MeasureText(FONT_BODY, val, 0.82f) + 16.0f);
			DrawPill(304.0f - pw, y + 4.0f, pw, 18.0f, val,
			         focused ? WithAlpha(t.accent, 36) : t.cardBgAlt,
			         focused ? t.accent : t.cardBorder,
			         focused ? t.text : t.textMuted, 0.82f);
		}
		// Smoothly gliding 2px cyan focus border
		const float hy = kListY + hlPos * kRowStride + slideY;
		RoundedBorder2(8.0f, hy, 304.0f, kRowH, 7.0f, ScaleAlpha(t.focusBorder, ease));

		DrawHintBar(320.0f, kCtrlHints, 3, t, "L/R", "Adjust");

		// ---- Top screen: Frozen game frame + live schematic of bottom touch layout
		BeginScreen(GFX_TOP, 400, 240, t.bg);
		DrawFrostedGameBackdrop(freezeSnap, 1.0f, t);

		const float cardY = 26.0f + (1.0f - ease) * 16.0f;
		DrawCard(20.0f, cardY, 360.0f, 188.0f, false, false, t, 10.0f);
		Text(FONT_TITLE, 36.0f, cardY + 14.0f, TouchPadEngine::LayoutName(cfg.layout), t.text, 1.0f);
		TextRight(FONT_BODY, 364.0f, cardY + 16.0f, cfg.leftHanded ? "Left-Handed" : "Right-Handed", t.accent, 0.86f);
		Quad(36.0f, cardY + 38.0f, 328.0f, 1.0f, t.headerRule);

		// Mini visual preview of the bottom screen layout on the left
		const float pvX = 36.0f;
		const float pvY = cardY + 50.0f;
		const float pvW = 136.0f;
		const float pvH = 102.0f;
		RoundedRect(pvX, pvY, pvW, pvH, 8.0f, t.bg);
		RoundedBorder1(pvX, pvY, pvW, pvH, 8.0f, t.cardBorder);

		if (cfg.layout == TOUCH_LAYOUT_SLEEP)
		{
			RoundedRect(pvX + 2.0f, pvY + 2.0f, pvW - 4.0f, pvH - 4.0f, 6.0f, RGBA(0, 0, 0, 255));
			TextCentered(FONT_BODY, pvX + pvW * 0.5f, pvY + 44.0f, "Screen Off", RGBA(255, 255, 255, 90), 0.82f);
		}
		else
		{
			const float zPx   = cfg.leftHanded ? (pvX + pvW - 44.0f) : (pvX + 8.0f);
			const float padPx = cfg.leftHanded ? (pvX + 44.0f)       : (pvX + pvW - 44.0f);
			const float padPy = pvY + 54.0f;
			RoundedRect(zPx, pvY + 18.0f, 36.0f, 34.0f, 5.0f, t.cardBg);
			TextCentered(FONT_BODY, zPx + 18.0f, pvY + 28.0f, "Z", t.accent, 0.88f);
			RoundedRect(zPx, pvY + 58.0f, 16.0f, 22.0f, 4.0f, t.cardBgAlt);
			RoundedRect(zPx + 20.0f, pvY + 58.0f, 16.0f, 22.0f, 4.0f, t.cardBgAlt);

			Circle(padPx, padPy, 32.0f, t.cardBg);
			if (cfg.layout == TOUCH_LAYOUT_CCROSS)
			{
				Circle(padPx, padPy - 18.0f, 9.5f, t.n64Yellow);
				Circle(padPx, padPy + 18.0f, 9.5f, t.n64Yellow);
				Circle(padPx - 18.0f, padPy, 9.5f, t.n64Yellow);
				Circle(padPx + 18.0f, padPy, 9.5f, t.n64Yellow);
			}
			else if (cfg.layout == TOUCH_LAYOUT_DPAD)
			{
				RoundedRect(padPx - 7.0f, padPy - 22.0f, 14.0f, 44.0f, 3.0f, t.textMuted);
				RoundedRect(padPx - 22.0f, padPy - 7.0f, 44.0f, 14.0f, 3.0f, t.textMuted);
			}
			else
			{
				const uint32_t col = (cfg.layout == TOUCH_LAYOUT_DUAL_SHOOTER) ? t.accent : t.n64Yellow;
				Ring(padPx, padPy, 24.0f, col);
				Circle(padPx, padPy, 11.0f, col);
			}
		}

		// Right-side explanation of physical + touch extension mapping
		const float rx = 186.0f;
		const char * desc =
			(cfg.layout == TOUCH_LAYOUT_CCROSS)       ? "4-way Yellow N64 C-Buttons Cross for camera & items, plus dedicated Z trigger and hold-guarded Save/Load." :
			(cfg.layout == TOUCH_LAYOUT_CSTICK)       ? "Virtual analog C-Stick for 360-degree camera panning (8-way C-Buttons) with elastic edge capture." :
			(cfg.layout == TOUCH_LAYOUT_DUAL_SHOOTER) ? "Twin-stick FPS mode: Touch Stick aims N64 Main Stick while 3DS Circle Pad strafes with C-Buttons." :
			(cfg.layout == TOUCH_LAYOUT_DPAD)         ? "Full N64 D-Pad Cross on touch screen without holding Y + D-Pad on Old 3DS." :
			                                            "Turns bottom screen black during gameplay to save battery. Tap anywhere to wake.";
		TextWrapped(FONT_BODY, rx, cardY + 52.0f, 176.0f, 16.0f, desc, t.textMuted, 0.88f);
		Text(FONT_BODY, rx, cardY + 144.0f, "Physical 3DS controls stay active.", t.textDim, 0.80f);

		EndFrame();
	}
}

} // namespace

namespace FrontendCTR
{

bool Initialise()
{
	if (s_initialised) return true;
	LoadFrontendConfig();
	Sound::Init();
	TouchController::Init();
	if (!EnsureMenuPool())
	{
		Sound::Shutdown();
		return false;
	}
	TickSystemStatus(true);
	s_initialised = true;
	return true;
}

void Shutdown()
{
	CoverScraper::Stop();
	ReleasePreview();
	Sound::Shutdown();
	UI::Renderer::Shutdown();
	s_romCount = 0;
	s_viewCount = 0;
	s_romsScanned = false;
	s_initialised = false;
}

void ReleaseAssets()
{
	// Release preview texture and menu vertex pool before emulation so Old 3DS
	// retains maximum Linear RAM for N64 textures (UIAtlas + 24 KiB HUD pool stay ready).
	ReleasePreview();
	ReleaseMenuPool();
}

bool IsMenuOpen()
{
	return false;
}

bool ConsumeResetRequest()
{
	const bool req = s_resetRequested;
	s_resetRequested = false;
	return req;
}

void RunOptionsPage(const RomID & romId, bool applyRuntime)
{
	if (!EnsureMenuPool())
		return;

	RefreshMemorySummary();
	TickSystemStatus(true);

	SRomPreferences prefs;
	if (CPreferences::Get())
		CPreferences::Get()->GetRomPreferences(romId, &prefs);

	bool pending_fps = gGlobalPreferences.DisplayFramerate;
	EViewportType pending_viewport = gGlobalPreferences.ViewportType;
	ESfxVolume pending_sfx = Sound::GetVolume();
	int   selection    = 0;
	float hlPos        = 0.0f;
	float hlVel        = 0.0f;
	float scrollPos    = 0.0f;
	float scrollVel    = 0.0f;
	float enterT       = 0.0f;
	bool  manualScroll = false;

	RepeatTimer rep;
	rep.Reset();
	TouchTracker touch;
	touch.Reset();

	constexpr int   kVisible   = 5;
	constexpr float kListY     = 32.0f;
	constexpr float kRowStride = 36.0f;
	constexpr float kRowH      = 32.0f;
	constexpr float kMaxScroll = (float)(kNumOptions - kVisible);

	static const HintItem kOptHints[3] = {
		{ "A", "Change" },
		{ "B", "Back"   },
		{ "Y", "Theme"  }
	};

	while (aptMainLoop() && !shouldQuit)
	{
		hidScanInput();
		u32 down = hidKeysDown();
		const u32 held = hidKeysHeld();
		UpdateTouchFromHid(touch, held);
		TickSystemStatus(false);

		if (enterT < 1.0f)
		{
			enterT += 1.0f / 12.0f;
			if (enterT > 1.0f) enterT = 1.0f;
		}
		const float ease = EaseOutCubic(enterT);

		int pressedRow = -1;
		if (touch.active)
		{
			if (touch.justPressed && touch.curX >= 300 && touch.curY >= (int)kListY && touch.curY < 214)
				touch.scrollbarDrag = true;

			if (touch.scrollbarDrag)
			{
				float frac = ((float)touch.curY - kListY) / (kVisible * kRowStride);
				if (frac < 0.0f) frac = 0.0f;
				if (frac > 1.0f) frac = 1.0f;
				scrollPos = frac * kMaxScroll;
				scrollVel = 0.0f;
				manualScroll = true;
			}
			else if (touch.dragged && touch.startY >= (int)kListY && touch.startY < 214)
			{
				scrollPos -= touch.deltaY / kRowStride;
				if (scrollPos < 0.0f) scrollPos = 0.0f;
				if (scrollPos > kMaxScroll) scrollPos = kMaxScroll;
				scrollVel = 0.0f;
				manualScroll = true;
			}
			else if (!touch.dragged && touch.curY >= (int)kListY && touch.curY < (int)(kListY + kVisible * kRowStride))
			{
				const int rIdx = (int)floorf(scrollPos + ((float)touch.curY - kListY) / kRowStride);
				if (rIdx >= 0 && rIdx < kNumOptions)
					pressedRow = rIdx;
			}
		}

		if (touch.justReleased)
		{
			if (touch.dragged && !touch.scrollbarDrag && touch.startY >= (int)kListY && touch.startY < 214)
			{
				scrollVel = -(touch.velY / kRowStride) * 60.0f * 0.45f;
				manualScroll = true;
			}
			else if (!touch.dragged && !touch.scrollbarDrag)
			{
				if (touch.releaseY >= 218)
				{
					if (touch.releaseX < 100) down |= KEY_A;
					else if (touch.releaseX < 200) down |= KEY_B;
					else down |= KEY_START;
				}
				else if (touch.releaseY >= (int)kListY && touch.releaseY < (int)(kListY + kVisible * kRowStride))
				{
					const int idx = (int)floorf(scrollPos + ((float)touch.releaseY - kListY) / kRowStride);
					if (idx >= 0 && idx < kNumOptions)
					{
						const bool activate = (touch.releaseX >= 205 || idx == selection);
						if (activate)
							down |= KEY_A;
						else if (idx != selection)
							Sound::Play(SFX_NAVIGATE, (idx < selection) ? 1.04f : 0.98f);
						selection = idx;
						manualScroll = false;
					}
				}
			}
		}

		if (down & KEY_Y)
		{
			ToggleLightDark();
			SaveFrontendConfig();
			Sound::Play(ActiveTheme().isLight ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
		}
		if (down & KEY_B)
		{
			Sound::Play(SFX_BACK);
			break;
		}
		if (down & KEY_START)
		{
			Sound::SetVolume(pending_sfx);
			if (CPreferences::Get())
			{
				CPreferences::Get()->SetRomPreferences(romId, prefs);
				gGlobalPreferences.DisplayFramerate = pending_fps;
				gGlobalPreferences.ViewportType = pending_viewport;
				CPreferences::Get()->Commit();
			}
			SaveFrontendConfig();
			if (applyRuntime)
			{
				gSpeedSyncEnabled = prefs.SpeedSyncEnabled;
				gVideoRateMatch = g_ROM.settings.VideoRateMatch || prefs.VideoRateMatch;
				static const u32 thf_frames[NUM_THF] = { 0, 1, 2, 4, 8, 16, 32 };
				gCheckTextureHashFrequency = thf_frames[prefs.CheckTextureHashFrequency];
				gFrameskipValue = prefs.Frameskip;
				gZoomX = prefs.ZoomX;
				gAudioPluginEnabled = prefs.AudioEnabled;
				gDynarecCycleMultiplier = (prefs.DynarecCycleMultiplier >= 3) ? 3u :
					((prefs.DynarecCycleMultiplier == 2) ? 2u : 1u);
			}
			Sound::Play(SFX_CONFIRM);
			break;
		}

		if (rep.Poll(down, held, KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN, 0))
		{
			selection = (selection + 1) % kNumOptions;
			manualScroll = false;
			Sound::Play(SFX_NAVIGATE, 0.98f);
		}
		if (rep.Poll(down, held, KEY_UP | KEY_DUP | KEY_CPAD_UP, 1))
		{
			selection = (selection + kNumOptions - 1) % kNumOptions;
			manualScroll = false;
			Sound::Play(SFX_NAVIGATE, 1.04f);
		}

		if (manualScroll)
		{
			if (!touch.active)
			{
				scrollPos += scrollVel * (1.0f / 60.0f);
				scrollVel *= 0.86f;
				if (fabsf(scrollVel) < 0.05f) scrollVel = 0.0f;
			}
			if (scrollPos < 0.0f) { scrollPos = 0.0f; scrollVel = 0.0f; }
			if (scrollPos > kMaxScroll) { scrollPos = kMaxScroll; scrollVel = 0.0f; }
		}
		else
		{
			float targetScroll = scrollPos;
			if ((float)selection < targetScroll) targetScroll = (float)selection;
			if ((float)selection >= targetScroll + kVisible) targetScroll = (float)(selection - kVisible + 1);
			if (targetScroll < 0.0f) targetScroll = 0.0f;
			if (targetScroll > kMaxScroll) targetScroll = kMaxScroll;
			SpringStep(scrollPos, scrollVel, targetScroll, 220.0f, 24.0f);
			if (scrollPos < 0.0f) scrollPos = 0.0f;
			if (scrollPos > kMaxScroll) scrollPos = kMaxScroll;
		}

		const int dir = (down & (KEY_A | KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_R)) ? 1 :
		                (down & (KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT | KEY_L)) ? -1 : 0;
		if (dir != 0)
		{
			const float stepPitch = (dir > 0) ? 1.06f : 0.94f;
			switch (selection)
			{
			case 0:  prefs.DynarecEnabled = !prefs.DynarecEnabled; Sound::Play(prefs.DynarecEnabled ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 1:  prefs.DynarecLoopOptimisation = !prefs.DynarecLoopOptimisation; Sound::Play(prefs.DynarecLoopOptimisation ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 2:  prefs.DynarecDoublesOptimisation = !prefs.DynarecDoublesOptimisation; Sound::Play(prefs.DynarecDoublesOptimisation ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 3:  prefs.MemoryAccessOptimisation = !prefs.MemoryAccessOptimisation; Sound::Play(prefs.MemoryAccessOptimisation ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 4:  prefs.PatchesEnabled = !prefs.PatchesEnabled; Sound::Play(prefs.PatchesEnabled ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 5:  prefs.SpeedSyncEnabled = prefs.SpeedSyncEnabled ? 0u : 1u; Sound::Play(prefs.SpeedSyncEnabled ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 6:  prefs.DoubleDisplayEnabled = !prefs.DoubleDisplayEnabled; Sound::Play(prefs.DoubleDisplayEnabled ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 7:  prefs.CleanSceneEnabled = !prefs.CleanSceneEnabled; Sound::Play(prefs.CleanSceneEnabled ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 8:  prefs.ClearDepthFrameBuffer = !prefs.ClearDepthFrameBuffer; Sound::Play(prefs.ClearDepthFrameBuffer ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 9:  prefs.Frameskip = (EFrameskipValue)((prefs.Frameskip + dir + NUM_FRAMESKIP_VALUES) % NUM_FRAMESKIP_VALUES); Sound::Play(SFX_ADJUST, stepPitch); break;
			case 10: prefs.CheckTextureHashFrequency = (ETextureHashFrequency)((prefs.CheckTextureHashFrequency + dir + NUM_THF) % NUM_THF); Sound::Play(SFX_ADJUST, stepPitch); break;
			case 11: prefs.VideoRateMatch = !prefs.VideoRateMatch; Sound::Play(prefs.VideoRateMatch ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 12: prefs.FogEnabled = !prefs.FogEnabled; Sound::Play(prefs.FogEnabled ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 13: pending_viewport = (EViewportType)((pending_viewport + dir + 4) % 4); Sound::Play(SFX_ADJUST, stepPitch); break;
			case 14:
			{
				const float prevZoom = prefs.ZoomX;
				prefs.ZoomX += dir * 0.1f;
				if (prefs.ZoomX < 0.5f) prefs.ZoomX = 0.5f;
				if (prefs.ZoomX > 2.0f) prefs.ZoomX = 2.0f;
				Sound::Play((fabsf(prefs.ZoomX - prevZoom) > 0.001f) ? SFX_ADJUST : SFX_ERROR, stepPitch);
				break;
			}
			case 15: prefs.AudioEnabled = (EAudioPluginMode)((prefs.AudioEnabled + dir + 3) % 3); Sound::Play(SFX_ADJUST, stepPitch); break;
			case 16: prefs.AudioRateMatch = !prefs.AudioRateMatch; Sound::Play(prefs.AudioRateMatch ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 17: prefs.CheatsEnabled = !prefs.CheatsEnabled; Sound::Play(prefs.CheatsEnabled ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 18: pending_fps = !pending_fps; Sound::Play(pending_fps ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
			case 19: CycleTheme(dir); Sound::Play(SFX_ADJUST, stepPitch); break;
			case 20:
			{
				int cur = (prefs.DynarecCycleMultiplier >= 3) ? 3 : ((prefs.DynarecCycleMultiplier == 2) ? 2 : 1);
				cur = ((cur - 1 + dir + 3) % 3) + 1;
				prefs.DynarecCycleMultiplier = (u32)cur;
				Sound::Play(SFX_ADJUST, stepPitch);
				break;
			}
			case 21:
			{
				pending_sfx = (ESfxVolume)(((int)pending_sfx + dir + SFX_VOL_COUNT) % SFX_VOL_COUNT);
				if (pending_sfx != SFX_VOL_OFF)
					Sound::PlayPreview(SFX_CONFIRM, pending_sfx);
				break;
			}
			}
		}

		SpringStep(hlPos, hlVel, (float)selection, 320.0f, 26.0f);

		const ThemePalette & t = ActiveTheme();
		const float slideY = (1.0f - ease) * 14.0f;
		BeginFrame();

		// ---- Bottom screen: crisp 3DS option cards with spring-gliding focus ring
		BeginScreen(GFX_BOTTOM, 320, 240, t.bg);
		DrawBackdrop(320.0f, 240.0f, t);
		char optCount[24];
		snprintf(optCount, sizeof(optCount), "%d / %d", selection + 1, kNumOptions);
		DrawHeaderBar(320.0f, "ROM Options", optCount, t);

		SetClipRect(true, 0, 26, 320, 190, 320, 240);
		int firstRow = (int)floorf(scrollPos);
		if (firstRow < 0) firstRow = 0;
		for (int idx = firstRow; idx < kNumOptions && idx <= firstRow + kVisible; ++idx)
		{
			const float y = kListY + ((float)idx - scrollPos) * kRowStride + slideY;
			if (y + kRowH < 26.0f || y > 218.0f)
				continue;

			const bool sel = (idx == selection);
			const bool pressed = (idx == pressedRow && !sel);
			DrawCard(8.0f, y, 294.0f, kRowH, false, pressed, t, 7.0f);
			if (sel)
				RoundedRect(8.0f, y, 294.0f, kRowH, 7.0f, t.focusFill);
			Text(FONT_BODY, 18.0f, y + 9.0f, kOptions[idx].label, sel ? t.text : t.textMuted, 0.96f);

			char value[48] = "";
			bool isBool = false;
			bool isOn   = false;
			switch (idx)
			{
			case 0:  isBool = true; isOn = prefs.DynarecEnabled; break;
			case 1:  isBool = true; isOn = prefs.DynarecLoopOptimisation; break;
			case 2:  isBool = true; isOn = prefs.DynarecDoublesOptimisation; break;
			case 3:  isBool = true; isOn = prefs.MemoryAccessOptimisation; break;
			case 4:  isBool = true; isOn = prefs.PatchesEnabled; break;
			case 5:  isBool = true; isOn = prefs.SpeedSyncEnabled != 0; break;
			case 6:  isBool = true; isOn = prefs.DoubleDisplayEnabled; break;
			case 7:  isBool = true; isOn = prefs.CleanSceneEnabled; break;
			case 8:  isBool = true; isOn = prefs.ClearDepthFrameBuffer; break;
			case 9:  snprintf(value, sizeof(value), "%s", FrameskipName(prefs.Frameskip)); break;
			case 10: snprintf(value, sizeof(value), "%s", TexHashName(prefs.CheckTextureHashFrequency)); break;
			case 11: isBool = true; isOn = prefs.VideoRateMatch; break;
			case 12: isBool = true; isOn = prefs.FogEnabled; break;
			case 13: snprintf(value, sizeof(value), "%s", ViewportName(pending_viewport)); break;
			case 14: snprintf(value, sizeof(value), "%.1fx", prefs.ZoomX); break;
			case 15: snprintf(value, sizeof(value), "%s", AudioName(prefs.AudioEnabled)); break;
			case 16: isBool = true; isOn = prefs.AudioRateMatch; break;
			case 17: isBool = true; isOn = prefs.CheatsEnabled; break;
			case 18: isBool = true; isOn = pending_fps; break;
			case 19: snprintf(value, sizeof(value), "%s", t.name); break;
			case 20: snprintf(value, sizeof(value),
				prefs.DynarecCycleMultiplier >= 3 ? "3x Turbo" :
				(prefs.DynarecCycleMultiplier == 2 ? "2x Fast" : "1x Normal")); break;
			case 21: snprintf(value, sizeof(value), "%s", Sound::VolumeName(pending_sfx)); break;
			}

			if (isBool)
			{
				DrawPill(248.0f, y + 6.0f, 46.0f, 20.0f, isOn ? "ON" : "OFF",
				         isOn ? WithAlpha(t.n64Green, 46) : t.cardBgAlt,
				         isOn ? t.n64Green : t.cardBorder,
				         isOn ? t.n64Green : t.textDim, 0.84f);
			}
			else if (value[0])
			{
				const float pw = fmaxf(58.0f, MeasureText(FONT_BODY, value, 0.84f) + 16.0f);
				DrawPill(294.0f - pw, y + 6.0f, pw, 20.0f, value,
				         sel ? WithAlpha(t.accent, 34) : t.cardBgAlt,
				         sel ? t.accent : t.cardBorder,
				         sel ? t.text : t.textMuted, 0.84f);
			}
		}
		// Smoothly gliding 2px cyan focus border
		const float hy = kListY + (hlPos - scrollPos) * kRowStride + slideY;
		if (hy + kRowH >= 26.0f && hy <= 218.0f)
			RoundedBorder2(8.0f, hy, 294.0f, kRowH, 7.0f, t.focusBorder);
		SetClipRect(false, 0, 0, 320, 240, 320, 240);

		// Touch scrollbar
		{
			const float trackH = 180.0f;
			RoundedRect(308.0f, kListY, 4.0f, trackH, 2.0f, WithAlpha(t.cardBorder, 140));
			const float thumbH = fmaxf(20.0f, trackH * (float)kVisible / (float)kNumOptions);
			const float frac = (kMaxScroll > 0.0f) ? (scrollPos / kMaxScroll) : 0.0f;
			RoundedRect(308.0f, kListY + (trackH - thumbH) * frac, 4.0f, thumbH, 2.0f,
			            touch.scrollbarDrag ? t.accent : t.textMuted);
		}

		DrawHintBar(320.0f, kOptHints, 3, t, "START", "Save");

		// ---- Top screen: clean contextual help card
		BeginScreen(GFX_TOP, 400, 240, t.bg);
		DrawBackdrop(400.0f, 240.0f, t);
		DrawTopStatusHeader("Configuration", t);

		DrawCard(24.0f, 46.0f + slideY * 0.6f, 352.0f, 132.0f, false, false, t, 10.0f);
		Text(FONT_TITLE, 40.0f, 62.0f + slideY * 0.6f, kOptions[selection].label, t.text, 1.05f);
		DrawPill(304.0f, 61.0f + slideY * 0.6f, 56.0f, 18.0f,
		         kOptions[selection].live ? "LIVE" : "REBOOT",
		         WithAlpha(kOptions[selection].live ? t.n64Green : t.n64Yellow, 40),
		         kOptions[selection].live ? t.n64Green : t.n64Yellow,
		         kOptions[selection].live ? t.n64Green : t.n64Yellow, 0.78f);

		Quad(40.0f, 88.0f + slideY * 0.6f, 320.0f, 1.0f, t.headerRule);
		TextWrapped(FONT_BODY, 40.0f, 100.0f + slideY * 0.6f, 320.0f, 17.0f, kOptions[selection].help, t.textMuted, 0.96f);

		Text(FONT_BODY, 26.0f, 206.0f, "Press START to apply and save changes.", t.textDim, 0.86f);
		TextRight(FONT_BODY, 376.0f, 206.0f, s_sysStatus.memSummary, t.textDim, 0.84f);

		EndFrame();
	}

	UI::RestoreRenderState();
}

std::string RunRomSelector()
{
	if (!EnsureMenuPool())
		return std::string();

	ScanRoms(false);
	RefreshMemorySummary();
	TickSystemStatus(true);

	constexpr int   kRowsVisible = 5;
	constexpr float kListY       = 30.0f;
	constexpr float kRowStride   = 36.0f;
	constexpr float kRowH        = 32.0f;

	RepeatTimer rep;
	rep.Reset();
	TouchTracker touch;
	touch.Reset();

	bool  manualScroll = false;
	char  toast[64]    = "";
	int   toastFrames  = 0;
	int   prevSelected = s_selected;
	float infoAnim     = 0.0f;
	float enterT       = 0.0f;

	if (s_viewCount > 0)
		RequestPreviewForRom(s_viewOrder[s_selected], true);

	// Queue missing covers for automatic background Wi-Fi download (selected ROM first!)
	uint32_t lastScrapeRev = CoverScraper::GetStatus().revision;
	{
		std::vector<CoverScraper::ScrapeJob> jobs;
		jobs.reserve((size_t)s_viewCount);
		auto TryQueue = [&](int viewIdx) {
			const FrontendRomInfo & r = s_roms[s_viewOrder[viewIdx]];
			IO::Filename destPath;
			BuildPreviewPathForRom(r, destPath);
			if (!IO::File::Exists(destPath))
			{
				CoverScraper::ScrapeJob j;
				j.romFilename = r.filename;
				j.romTitle    = r.title;
				j.regionCode  = r.country;
				j.destPath    = destPath;
				jobs.push_back(j);
			}
		};
		if (s_viewCount > 0)
		{
			TryQueue(s_selected);
			for (int i = 0; i < s_viewCount; ++i)
			{
				if (i != s_selected)
					TryQueue(i);
			}
		}
		if (!jobs.empty())
			CoverScraper::Start(jobs);
	}

	static bool s_playedStartupSfx = false;
	if (!s_playedStartupSfx)
	{
		s_playedStartupSfx = true;
		Sound::Play(SFX_STARTUP);
	}

	while (aptMainLoop() && !shouldQuit)
	{
		hidScanInput();
		u32 down = hidKeysDown();
		const u32 held = hidKeysHeld();
		UpdateTouchFromHid(touch, held);
		TickSystemStatus(false);

		if (enterT < 1.0f)
		{
			enterT += 1.0f / 14.0f;
			if (enterT > 1.0f) enterT = 1.0f;
		}
		if (infoAnim < 1.0f)
		{
			infoAnim += 1.0f / 10.0f;
			if (infoAnim > 1.0f) infoAnim = 1.0f;
		}

		const float maxScroll = (s_viewCount > kRowsVisible) ? (float)(s_viewCount - kRowsVisible) : 0.0f;
		int pressedRow = -1;

		if (touch.active)
		{
			if (s_viewCount > kRowsVisible && touch.justPressed &&
			    touch.curX >= 300 && touch.curY >= (int)kListY && touch.curY < 214)
			{
				touch.scrollbarDrag = true;
			}

			if (touch.scrollbarDrag && maxScroll > 0.0f)
			{
				float frac = ((float)touch.curY - kListY) / (kRowsVisible * kRowStride);
				if (frac < 0.0f) frac = 0.0f;
				if (frac > 1.0f) frac = 1.0f;
				s_scrollPos = frac * maxScroll;
				s_scrollVel = 0.0f;
				manualScroll = true;
			}
			else if (touch.dragged && s_viewCount > kRowsVisible &&
			         touch.startY >= (int)kListY && touch.startY < 214)
			{
				s_scrollPos -= touch.deltaY / kRowStride;
				if (s_scrollPos < 0.0f) s_scrollPos = 0.0f;
				if (s_scrollPos > maxScroll) s_scrollPos = maxScroll;
				s_scrollVel = 0.0f;
				manualScroll = true;
			}
			else if (!touch.dragged && s_viewCount > 0 &&
			         touch.curY >= (int)kListY && touch.curY < (int)(kListY + kRowsVisible * kRowStride))
			{
				const int rIdx = (int)floorf(s_scrollPos + ((float)touch.curY - kListY) / kRowStride);
				if (rIdx >= 0 && rIdx < s_viewCount)
					pressedRow = rIdx;
			}
		}

		if (touch.justReleased)
		{
			if (touch.dragged && !touch.scrollbarDrag && s_viewCount > kRowsVisible &&
			    touch.startY >= (int)kListY && touch.startY < 214)
			{
				s_scrollVel = -(touch.velY / kRowStride) * 60.0f * 0.45f;
				manualScroll = true;
			}
			else if (!touch.dragged && !touch.scrollbarDrag)
			{
				if (touch.releaseY >= 218)
				{
					if (touch.releaseX < 78) down |= KEY_A;
					else if (touch.releaseX < 156) down |= KEY_Y;
					else if (touch.releaseX < 238) down |= KEY_X;
					else down |= KEY_START;
				}
				else if (s_viewCount > 0 && touch.releaseY >= (int)kListY &&
				         touch.releaseY < (int)(kListY + kRowsVisible * kRowStride))
				{
					const int idx = (int)floorf(s_scrollPos + ((float)touch.releaseY - kListY) / kRowStride);
					if (idx >= 0 && idx < s_viewCount)
					{
						if (idx == s_selected) down |= KEY_A;
						s_selected = idx;
						manualScroll = false;
					}
				}
			}
		}

		if (down & KEY_START)
		{
			Sound::Play(SFX_BACK);
			svcSleepThread(90000000LL);
			shouldQuit = true;
			break;
		}

		if (down & KEY_Y)
		{
			ToggleLightDark();
			SaveFrontendConfig();
			Sound::Play(ActiveTheme().isLight ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
			snprintf(toast, sizeof(toast), "Theme: %s", ActiveTheme().name);
			toastFrames = 70;
		}

		if (down & (KEY_L | KEY_R))
		{
			s_sortMode = (s_sortMode + ((down & KEY_R) ? 1 : (SORT_COUNT - 1))) % SORT_COUNT;
			RebuildView();
			prevSelected = s_selected;
			SaveFrontendConfig();
			manualScroll = false;
			Sound::Play(SFX_ADJUST, (down & KEY_R) ? 1.06f : 0.94f);
			snprintf(toast, sizeof(toast), "Sort: %s", kSortNames[s_sortMode]);
			toastFrames = 70;
		}

		if (s_viewCount > 0)
		{
			bool navPageJump = false;
			bool navUp       = false;
			if (rep.Poll(down, held, KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN, 0))
			{
				s_selected = (s_selected + 1) % s_viewCount;
				manualScroll = false;
			}
			if (rep.Poll(down, held, KEY_UP | KEY_DUP | KEY_CPAD_UP, 1))
			{
				s_selected = (s_selected + s_viewCount - 1) % s_viewCount;
				manualScroll = false;
				navUp = true;
			}
			if (rep.Poll(down, held, KEY_RIGHT | KEY_DRIGHT, 2))
			{
				s_selected += kRowsVisible;
				if (s_selected >= s_viewCount) s_selected = s_viewCount - 1;
				manualScroll = false;
				navPageJump = true;
			}
			if (rep.Poll(down, held, KEY_LEFT | KEY_DLEFT, 3))
			{
				s_selected -= kRowsVisible;
				if (s_selected < 0) s_selected = 0;
				manualScroll = false;
				navPageJump = true;
				navUp = true;
			}

			if (down & KEY_SELECT)
			{
				FrontendRomInfo & cur = s_roms[s_viewOrder[s_selected]];
				cur.favorite = !cur.favorite;
				SaveFrontendConfig();
				Sound::Play(cur.favorite ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
				snprintf(toast, sizeof(toast), cur.favorite ? "Added to favorites" : "Removed from favorites");
				toastFrames = 70;
				if (s_sortMode == SORT_FAV)
				{
					RebuildView();
					prevSelected = s_selected;
				}
			}

			if (down & KEY_X)
			{
				const FrontendRomInfo & cur = s_roms[s_viewOrder[s_selected]];
				if (cur.hasDetails)
				{
					Sound::Play(SFX_CONFIRM);
					RunOptionsPage(cur.romId, false);
					RefreshMemorySummary();
					touch.Reset();
				}
				else
				{
					Sound::Play(SFX_ERROR);
				}
			}

			if (s_selected != prevSelected)
			{
				const bool movedUp = navUp || (s_selected < prevSelected);
				Sound::Play(navPageJump ? SFX_ADJUST : SFX_NAVIGATE, movedUp ? 1.04f : 0.98f);
				prevSelected = s_selected;
				infoAnim = 0.0f;
				IO::Filename selPath;
				BuildPreviewPathForRom(s_roms[s_viewOrder[s_selected]], selPath);
				CoverScraper::Prioritize(selPath);
			}
			RequestPreviewForRom(s_viewOrder[s_selected], false);
		}
		else if (down & (KEY_A | KEY_X))
		{
			Sound::Play(SFX_ERROR);
		}

		// Reload cover automatically as soon as the background scraper saves a new PNG!
		const CoverScraper::ScraperStatus scrapeSt = CoverScraper::GetStatus();
		if (scrapeSt.revision != lastScrapeRev)
		{
			lastScrapeRev = scrapeSt.revision;
			if (s_viewCount > 0 && s_previewTex == nullptr)
			{
				s_previewRomIdx = -1;
				RequestPreviewForRom(s_viewOrder[s_selected], true);
			}
		}

		TickPreviewLoader();

		const bool launch = (down & KEY_A) && s_viewCount > 0;

		SpringStep(s_hlPos, s_hlVel, (float)s_selected, 320.0f, 26.0f);

		if (manualScroll)
		{
			if (!touch.active)
			{
				s_scrollPos += s_scrollVel * (1.0f / 60.0f);
				s_scrollVel *= 0.86f;
				if (fabsf(s_scrollVel) < 0.05f) s_scrollVel = 0.0f;
			}
			if (s_scrollPos < 0.0f) { s_scrollPos = 0.0f; s_scrollVel = 0.0f; }
			if (s_scrollPos > maxScroll) { s_scrollPos = maxScroll; s_scrollVel = 0.0f; }
		}
		else
		{
			float targetTop = s_scrollPos;
			if ((float)s_selected < targetTop) targetTop = (float)s_selected;
			if ((float)s_selected >= targetTop + kRowsVisible) targetTop = (float)(s_selected - kRowsVisible + 1);
			if (targetTop < 0.0f) targetTop = 0.0f;
			if (targetTop > maxScroll) targetTop = maxScroll;
			SpringStep(s_scrollPos, s_scrollVel, targetTop, 220.0f, 24.0f);
			if (s_scrollPos < 0.0f) s_scrollPos = 0.0f;
			if (s_scrollPos > maxScroll) s_scrollPos = maxScroll;
		}

		const ThemePalette & t = ActiveTheme();
		const float enterEase = EaseOutCubic(enterT);
		const float infoEase  = EaseOutCubic(infoAnim);
		const float slideY    = (1.0f - enterEase) * 14.0f;

		BeginFrame();

		// ---- Bottom screen: CTR64-style rounded ROM cards + gliding cyan focus ring
		BeginScreen(GFX_BOTTOM, 320, 240, t.bg);
		DrawBackdrop(320.0f, 240.0f, t);

		char gameCount[32];
		snprintf(gameCount, sizeof(gameCount), "%d %s", s_viewCount, (s_viewCount == 1) ? "game" : "games");
		DrawHeaderBar(320.0f, "DaedalusX3D", gameCount, t);

		if (s_viewCount == 0)
		{
			DrawCard(18.0f, 56.0f + slideY, 284.0f, 118.0f, false, false, t, 10.0f);
			TextCentered(FONT_TITLE, 160.0f, 74.0f + slideY, "No N64 ROMs found", t.text, 1.0f);
			TextCentered(FONT_BODY, 160.0f, 102.0f + slideY, "Copy .z64 / .n64 / .v64 files to:", t.textMuted, 0.92f);
			TextCentered(FONT_BODY, 160.0f, 122.0f + slideY, "sdmc:/3ds/DaedalusX64/Roms", t.accent, 0.92f);
			TextCentered(FONT_BODY, 160.0f, 146.0f + slideY, "Press Y to toggle Light / Dark theme", t.textDim, 0.85f);
		}
		else
		{
			const float cardW = (s_viewCount > kRowsVisible) ? 294.0f : 304.0f;
			SetClipRect(true, 0, 26, 320, 190, 320, 240);
			int first = (int)floorf(s_scrollPos);
			if (first < 0) first = 0;
			for (int i = first; i < s_viewCount && i <= first + kRowsVisible; ++i)
			{
				const float ry = kListY + ((float)i - s_scrollPos) * kRowStride + slideY;
				if (ry + kRowH < 26.0f || ry > 218.0f)
					continue;

				const bool sel     = (i == s_selected);
				const bool pressed = (i == pressedRow && !sel);
				const FrontendRomInfo & r = s_roms[s_viewOrder[i]];

				DrawCard(8.0f, ry, cardW, kRowH, false, pressed, t, 8.0f);
				if (sel)
					RoundedRect(8.0f, ry, cardW, kRowH, 8.0f, t.focusFill);

				RoundedRect(14.0f, ry + 5.0f, 22.0f, 22.0f, 5.0f, sel ? t.badgeBgSel : t.badgeBg);
				Icon(ICON_CARTRIDGE, 17.0f, ry + 8.0f, 16.0f,
				     sel ? RGBA(255, 255, 255) : (t.isLight ? t.textMuted : RGBA(220, 226, 236)));

				const float titleMaxW = cardW - (r.favorite ? 88.0f : 72.0f);
				TextEllipsis(FONT_BODY, 43.0f, ry + 9.0f, titleMaxW, r.title, sel ? t.text : t.textMuted, 0.98f);

				if (r.favorite)
					Icon(ICON_STAR, cardW - 44.0f, ry + 10.0f, 12.0f, t.n64Yellow);
				TextRight(FONT_BODY, 8.0f + cardW - 10.0f, ry + 10.0f, r.country,
				          sel ? t.text : t.textDim, 0.85f);
			}
			// Smooth spring-gliding 3DS cyan focus border
			const float hy = kListY + (s_hlPos - s_scrollPos) * kRowStride + slideY;
			if (hy + kRowH >= 26.0f && hy <= 218.0f)
			{
				RoundedRect(7.0f, hy - 1.0f, cardW + 2.0f, kRowH + 2.0f, 9.0f, t.focusGlow);
				RoundedBorder2(8.0f, hy, cardW, kRowH, 8.0f, t.focusBorder);
			}
			SetClipRect(false, 0, 0, 320, 240, 320, 240);

			if (s_viewCount > kRowsVisible)
			{
				const float trackH = 180.0f;
				RoundedRect(308.0f, kListY, 4.0f, trackH, 2.0f, WithAlpha(t.cardBorder, 140));
				const float thumbH = fmaxf(20.0f, trackH * (float)kRowsVisible / (float)s_viewCount);
				const float frac = (maxScroll > 0.0f) ? (s_scrollPos / maxScroll) : 0.0f;
				RoundedRect(308.0f, kListY + (trackH - thumbH) * frac, 4.0f, thumbH, 2.0f,
				            touch.scrollbarDrag ? t.accent : t.textMuted);
			}
		}

		if (toastFrames > 0)
		{
			RoundedRect(64.0f, 186.0f, 192.0f, 24.0f, 10.0f, WithAlpha(t.hintBg, 235));
			RoundedBorder1(64.0f, 186.0f, 192.0f, 24.0f, 10.0f, t.accent);
			TextCentered(FONT_BODY, 160.0f, 191.0f, toast, t.hintText, 0.88f);
			--toastFrames;
		}

		const HintItem libHints[3] = {
			{ "A", "Start" },
			{ "Y", t.isLight ? "Dark" : "Light" },
			{ "X", "Options" }
		};
		DrawHintBar(320.0f, libHints, 3, t, "START", "Quit");

		// ---- Top screen: clean 3DS Hero Showcase with smooth selection transitions
		BeginScreen(GFX_TOP, 400, 240, t.bg);
		DrawBackdrop(400.0f, 240.0f, t);
		DrawTopStatusHeader("DaedalusX3D", t);

		if (s_viewCount > 0)
		{
			const FrontendRomInfo & r = s_roms[s_viewOrder[s_selected]];
			const float heroY = 38.0f + slideY * 0.6f;
			DrawCard(16.0f, heroY, 368.0f, 166.0f, false, false, t, 10.0f);

			const float artX = 30.0f;
			const float artY = heroY + 16.0f;
			const float artW = 128.0f;
			const float artH = 128.0f;
			RoundedRect(artX, artY, artW, artH, 8.0f, t.cardBgAlt);
			RoundedBorder1(artX, artY, artW, artH, 8.0f, t.cardBorder);

			if (s_previewTex != nullptr && s_previewTex->HasData())
			{
				const float pw = (float)s_previewTex->GetWidth();
				const float ph = (float)s_previewTex->GetHeight();
				const float u1 = pw * s_previewTex->GetScaleX();
				const float v1 = ph * s_previewTex->GetScaleY();
				const float sc = fminf((artW - 10.0f) / pw, (artH - 10.0f) / ph) * (0.94f + 0.06f * infoEase);
				const float dw = pw * sc;
				const float dh = ph * sc;
				TexturedQuad(s_previewTex->GetC3DTexture(),
				             artX + (artW - dw) * 0.5f, artY + (artH - dh) * 0.5f,
				             dw, dh, 0.0f, 0.0f, u1, v1, ScaleAlpha(RGBA(255, 255, 255), infoEase));
			}
			else
			{
				RoundedRect(artX + 28.0f, artY + 20.0f, 72.0f, 72.0f, 14.0f, ScaleAlpha(WithAlpha(t.accent, 28), infoEase));
				Icon(ICON_CARTRIDGE, artX + 44.0f, artY + 36.0f, 40.0f, ScaleAlpha(t.accent, infoEase));
				TextCentered(FONT_BODY, artX + artW * 0.5f, artY + 100.0f, r.country, ScaleAlpha(t.textMuted, infoEase), 0.90f);
			}

			const float infoX = 172.0f;
			const float infoW = 198.0f;
			const float infoSlide = (1.0f - infoEase) * 8.0f;
			TextWrapped(FONT_TITLE, infoX, heroY + 16.0f + infoSlide, infoW, 19.0f, r.title, ScaleAlpha(t.text, infoEase), 1.02f);

			char sizeBadge[24];
			snprintf(sizeBadge, sizeof(sizeBadge), "%.1f MB", r.size / (1024.0f * 1024.0f));
			DrawPill(infoX, heroY + 64.0f + infoSlide, 44.0f, 20.0f, r.country,
			         ScaleAlpha(WithAlpha(t.accent, 34), infoEase), ScaleAlpha(t.accent, infoEase), ScaleAlpha(t.accent, infoEase), 0.82f);
			DrawPill(infoX + 50.0f, heroY + 64.0f + infoSlide, 64.0f, 20.0f, sizeBadge,
			         ScaleAlpha(t.cardBgAlt, infoEase), ScaleAlpha(t.cardBorder, infoEase), ScaleAlpha(t.textMuted, infoEase), 0.82f);
			if (r.favorite)
				DrawPill(infoX + 120.0f, heroY + 64.0f + infoSlide, 52.0f, 20.0f, "Fav",
				         ScaleAlpha(WithAlpha(t.n64Yellow, 40), infoEase), ScaleAlpha(t.n64Yellow, infoEase), ScaleAlpha(t.n64Yellow, infoEase), 0.82f);

			Quad(infoX, heroY + 94.0f, infoW, 1.0f, t.headerRule);

			char touchLine[48];
			snprintf(touchLine, sizeof(touchLine), "Touch: %s",
			         TouchPadEngine::LayoutShortName(TouchController::Engine().GetConfig().layout));
			Text(FONT_BODY, infoX, heroY + 104.0f, touchLine, t.textMuted, 0.88f);
			Text(FONT_BODY, infoX, heroY + 122.0f,
			     gDynarecAvailable ? "Core: ARM DynaRec" : "Core: Interpreter",
			     gDynarecAvailable ? t.n64Green : t.n64Yellow, 0.88f);
			Text(FONT_BODY, infoX, heroY + 140.0f, "L/R: Sort  |  SEL: Favorite", t.textDim, 0.82f);
		}
		else
		{
			DrawCard(48.0f, 64.0f + slideY, 304.0f, 112.0f, false, false, t, 10.0f);
			TextCentered(FONT_TITLE, 200.0f, 92.0f + slideY, "DaedalusX3D", t.text, 1.25f);
			TextCentered(FONT_BODY, 200.0f, 124.0f + slideY, "Nintendo 64 Emulator for Old & New 3DS", t.textMuted, 0.94f);
		}

		if (scrapeSt.active && scrapeSt.total > 0)
		{
			char scrapeLine[48];
			snprintf(scrapeLine, sizeof(scrapeLine), "Fetching covers (%d/%d)...",
			         scrapeSt.completed, scrapeSt.total);
			Text(FONT_BODY, 16.0f, 214.0f, scrapeLine, t.accent, 0.82f);
		}
		else
		{
			Text(FONT_BODY, 16.0f, 214.0f, "ARM11 DynaRec + Native Citro3D", t.textDim, 0.82f);
		}
		TextRight(FONT_BODY, 384.0f, 214.0f, s_sysStatus.memSummary, t.textDim, 0.82f);

		EndFrame();

		if (launch)
		{
			Sound::Play(SFX_LAUNCH_GAME);
			CoverScraper::Stop();
			const FrontendRomInfo & chosen = s_roms[s_viewOrder[s_selected]];
			CopyStr(s_lastSelectedFile, sizeof(s_lastSelectedFile), chosen.filename);
			SaveFrontendConfig();
			return std::string(chosen.path);
		}
	}

	CoverScraper::Stop();
	return std::string();
}

void RunPauseMenu(const char * gameName, const RomID & romId)
{
	// Capture the live N64 top screen right before opening the Pause UI!
	CRefPtr<CNativeTexture> freezeSnap = CaptureTopScreenSnapshot();

	if (!EnsureMenuPool())
	{
		freezeSnap = nullptr;
		UI::RestoreRenderState();
		return;
	}

	AudioOutput::SetGamePaused(true);
	Sound::Play(SFX_PAUSE_OPEN);

	constexpr int kNumCells = 10;
	static const Rect kCells[kNumCells] = {
		{   8.0f,  31.0f, 304.0f, 36.0f }, // 0: Resume game
		{   8.0f,  74.0f,  96.0f, 42.0f }, // 1: Save
		{ 112.0f,  74.0f,  96.0f, 42.0f }, // 2: Load
		{ 216.0f,  74.0f,  96.0f, 42.0f }, // 3: Slot
		{   8.0f, 122.0f,  96.0f, 42.0f }, // 4: Audio
		{ 112.0f, 122.0f,  96.0f, 42.0f }, // 5: Video
		{ 216.0f, 122.0f,  96.0f, 42.0f }, // 6: Controls
		{   8.0f, 170.0f,  96.0f, 42.0f }, // 7: Reset
		{ 112.0f, 170.0f,  96.0f, 42.0f }, // 8: Game list
		{ 216.0f, 170.0f,  96.0f, 42.0f }, // 9: Exit
	};

	int   selection   = 0;
	int   slot        = TouchController::GetActiveSlot();
	bool  slotExists[5] = {};
	char  toast[64]   = "";
	int   toastFrames = 0;

	// Smooth 2D spring-animated focus ring rect + menu entry transition
	float hlX = kCells[0].x, hlVX = 0.0f;
	float hlY = kCells[0].y, hlVY = 0.0f;
	float hlW = kCells[0].w, hlVW = 0.0f;
	float hlH = kCells[0].h, hlVH = 0.0f;
	float enterT = 0.0f;

	auto RefreshSlots = [&]() {
		for (int i = 0; i < 5; ++i)
		{
			IO::Filename check;
			snprintf(check, sizeof(check), "%s%s.ss%d",
			         DAEDALUS_CTR_PATH("SaveStates/"), gameName ? gameName : "rom", i);
			slotExists[i] = IO::File::Exists(check);
		}
	};
	RefreshSlots();
	RefreshMemorySummary();
	TickSystemStatus(true);

	RepeatTimer rep;
	rep.Reset();
	TouchTracker touch;
	touch.Reset();

	// Wait for physical SELECT release before accepting pause menu input
	while (aptMainLoop() && (hidKeysHeld() & KEY_SELECT))
	{
		gspWaitForVBlank();
		hidScanInput();
	}

	static const HintItem kPauseHints[3] = {
		{ "A",   "OK"     },
		{ "B",   "Resume" },
		{ "L/R", "Slot"   }
	};

	bool exitMenu = false;
	while (aptMainLoop() && !shouldQuit && !exitMenu)
	{
		hidScanInput();
		u32 down = hidKeysDown();
		const u32 held = hidKeysHeld();
		UpdateTouchFromHid(touch, held);
		TickSystemStatus(false);

		if (enterT < 1.0f)
		{
			enterT += 1.0f / 14.0f;
			if (enterT > 1.0f) enterT = 1.0f;
		}
		const float ease = EaseOutCubic(enterT);

		int pressedCell = -1;
		if (touch.active && !touch.dragged)
		{
			for (int i = 0; i < kNumCells; ++i)
			{
				if (kCells[i].Contains((float)touch.curX, (float)touch.curY))
				{
					pressedCell = i;
					break;
				}
			}
		}

		if (touch.justReleased && !touch.dragged)
		{
			if (touch.releaseY >= 218)
			{
				if (touch.releaseX < 90) down |= KEY_A;
				else if (touch.releaseX < 180) down |= KEY_B;
				else if (touch.releaseX < 250) down |= KEY_R;
				else down |= KEY_Y;
			}
			else
			{
				for (int i = 0; i < kNumCells; ++i)
				{
					if (kCells[i].Contains((float)touch.releaseX, (float)touch.releaseY))
					{
						selection = i;
						if (i == 3)
						{
							const float mid = kCells[3].x + kCells[3].w * 0.5f;
							slot = (touch.releaseX < (int)mid) ? ((slot + 4) % 5) : ((slot + 1) % 5);
							TouchController::SetActiveSlot(slot);
							Sound::Play(SFX_ADJUST, 0.90f + slot * 0.05f, 0.22f);
						}
						else
						{
							down |= KEY_A;
						}
						break;
					}
				}
			}
		}

		if (down & (KEY_B | KEY_SELECT))
		{
			Sound::Play(SFX_PAUSE_CLOSE);
			exitMenu = true;
			break;
		}
		if (down & KEY_Y)
		{
			ToggleLightDark();
			SaveFrontendConfig();
			Sound::Play(ActiveTheme().isLight ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
		}
		if (down & KEY_L)
		{
			slot = (slot + 4) % 5;
			TouchController::SetActiveSlot(slot);
			Sound::Play(SFX_ADJUST, 0.90f + slot * 0.05f, -0.15f);
		}
		if (down & KEY_R)
		{
			slot = (slot + 1) % 5;
			TouchController::SetActiveSlot(slot);
			Sound::Play(SFX_ADJUST, 0.90f + slot * 0.05f, 0.15f);
		}

		auto CellPan = [](int cell) -> float {
			if (cell <= 0) return 0.0f;
			const int col = (cell - 1) % 3;
			return (col == 0) ? -0.22f : ((col == 2) ? 0.22f : 0.0f);
		};

		if (rep.Poll(down, held, KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN, 0))
		{
			if (selection == 0) selection = 1;
			else if (selection <= 6) selection += 3;
			else selection = 0;
			Sound::Play(SFX_NAVIGATE, 0.98f, CellPan(selection));
		}
		if (rep.Poll(down, held, KEY_UP | KEY_DUP | KEY_CPAD_UP, 1))
		{
			if (selection == 0) selection = 7;
			else if (selection <= 3) selection = 0;
			else selection -= 3;
			Sound::Play(SFX_NAVIGATE, 1.04f, CellPan(selection));
		}
		if (rep.Poll(down, held, KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT, 2))
		{
			if (selection == 3)
			{
				slot = (slot + 1) % 5;
				TouchController::SetActiveSlot(slot);
				Sound::Play(SFX_ADJUST, 0.90f + slot * 0.05f, 0.22f);
			}
			else if (selection > 0)
			{
				const int rowBase = 1 + ((selection - 1) / 3) * 3;
				const int col = (selection - 1) % 3;
				selection = rowBase + ((col + 1) % 3);
				Sound::Play(SFX_NAVIGATE, 1.02f, CellPan(selection));
			}
		}
		if (rep.Poll(down, held, KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT, 3))
		{
			if (selection == 3)
			{
				slot = (slot + 4) % 5;
				TouchController::SetActiveSlot(slot);
				Sound::Play(SFX_ADJUST, 0.90f + slot * 0.05f, 0.22f);
			}
			else if (selection > 0)
			{
				const int rowBase = 1 + ((selection - 1) / 3) * 3;
				const int col = (selection - 1) % 3;
				selection = rowBase + ((col + 2) % 3);
				Sound::Play(SFX_NAVIGATE, 0.98f, CellPan(selection));
			}
		}

		if (down & KEY_A)
		{
			char path[256];
			snprintf(path, sizeof(path), "%s%s.ss%d",
			         DAEDALUS_CTR_PATH("SaveStates/"), gameName ? gameName : "rom", slot);
			switch (selection)
			{
			case 0:
				Sound::Play(SFX_PAUSE_CLOSE);
				exitMenu = true;
				break;
			case 1:
				Sound::Play(SFX_SAVE_STATE);
				CPU_RequestSaveState(path);
				exitMenu = true;
				break;
			case 2:
				if (slotExists[slot])
				{
					Sound::Play(SFX_LOAD_STATE);
					CPU_RequestLoadState(path);
					exitMenu = true;
				}
				else
				{
					Sound::Play(SFX_ERROR);
					snprintf(toast, sizeof(toast), "Slot %d is empty", slot);
					toastFrames = 75;
				}
				break;
			case 3:
				slot = (slot + 1) % 5;
				TouchController::SetActiveSlot(slot);
				Sound::Play(SFX_ADJUST, 0.90f + slot * 0.05f, 0.22f);
				break;
			case 4:
			{
				const int nextAud = ((int)gAudioPluginEnabled + 1) % 3;
				gAudioPluginEnabled = (EAudioPluginMode)nextAud;
				if (CPreferences::Get())
				{
					SRomPreferences p;
					CPreferences::Get()->GetRomPreferences(romId, &p);
					p.AudioEnabled = gAudioPluginEnabled;
					CPreferences::Get()->SetRomPreferences(romId, p);
					CPreferences::Get()->Commit();
				}
				Sound::Play((nextAud == APM_DISABLED) ? SFX_TOGGLE_OFF : SFX_TOGGLE_ON);
				snprintf(toast, sizeof(toast), "Audio: %s", AudioName(nextAud));
				toastFrames = 75;
				break;
			}
			case 5:
				Sound::Play(SFX_CONFIRM);
				RunOptionsPage(romId, true);
				RefreshSlots();
				RefreshMemorySummary();
				touch.Reset();
				break;
			case 6:
				Sound::Play(SFX_CONFIRM);
				RunControlsPage(gameName, freezeSnap);
				touch.Reset();
				break;
			case 7:
				Sound::Play(SFX_LAUNCH_GAME);
				s_resetRequested = true;
				CPU_Halt("Reset from menu");
				exitMenu = true;
				break;
			case 8:
				Sound::Play(SFX_BACK);
				s_resetRequested = false;
				CPU_Halt("Closed from menu");
				exitMenu = true;
				break;
			case 9:
				Sound::Play(SFX_BACK);
				svcSleepThread(90000000LL);
				s_resetRequested = false;
				shouldQuit = true;
				CPU_Halt("Exit from menu");
				exitMenu = true;
				break;
			}
		}

		// Smooth 2D spring interpolation for the floating 3DS cyan focus border
		const Rect & targetCell = kCells[selection];
		SpringStep(hlX, hlVX, targetCell.x, 340.0f, 26.0f);
		SpringStep(hlY, hlVY, targetCell.y, 340.0f, 26.0f);
		SpringStep(hlW, hlVW, targetCell.w, 340.0f, 26.0f);
		SpringStep(hlH, hlVH, targetCell.h, 340.0f, 26.0f);

		const ThemePalette & t = ActiveTheme();
		const float botSlide = (1.0f - ease) * 18.0f;

		BeginFrame();

		// ---- Bottom screen: Animated CTR64 3x3 Pause Grid
		BeginScreen(GFX_BOTTOM, 320, 240, t.bg);
		DrawBackdrop(320.0f, 240.0f, t);
		DrawHeaderBar(320.0f, gameName ? gameName : "Nintendo 64", "Paused", t);

		// Cell 0: Full-width Resume game hero card
		{
			const Rect & r = kCells[0];
			const float y = r.y + botSlide;
			DrawCard(r.x, y, r.w, r.h, false, pressedCell == 0, t, 8.0f);
			if (selection == 0)
				RoundedRect(r.x, y, r.w, r.h, 8.0f, t.focusFill);
			const float textW = MeasureText(FONT_TITLE, "Resume game", 0.94f);
			const float groupW = 22.0f + 8.0f + textW;
			const float gx = r.x + (r.w - groupW) * 0.5f;
			Circle(gx + 11.0f, y + r.h * 0.5f, 10.5f, t.accent);
			Icon(ICON_PLAY, gx + 5.5f, y + r.h * 0.5f - 5.5f, 11.0f, RGBA(255, 255, 255));
			Text(FONT_TITLE, gx + 30.0f, y + 10.0f, "Resume game", t.text, 0.94f);
		}

		auto DrawActionTile = [&](int idx, EIconId icon, const char * label, uint32_t iconCol, const char * sub = nullptr) {
			const Rect & r = kCells[idx];
			const float y = r.y + botSlide;
			const bool sel = (selection == idx);
			DrawCard(r.x, y, r.w, r.h, false, pressedCell == idx, t, 8.0f);
			if (sel)
				RoundedRect(r.x, y, r.w, r.h, 8.0f, t.focusFill);
			if (sub && sub[0])
			{
				Icon(icon, r.x + 10.0f, y + 12.0f, 18.0f, iconCol);
				Text(FONT_BODY, r.x + 33.0f, y + 7.0f, label, t.text, 0.92f);
				Text(FONT_BODY, r.x + 33.0f, y + 22.0f, sub, t.textMuted, 0.80f);
			}
			else
			{
				const float tw = MeasureText(FONT_BODY, label, 0.94f);
				const float gw = 18.0f + 6.0f + tw;
				const float sx = r.x + (r.w - gw) * 0.5f;
				Icon(icon, sx, y + 12.0f, 18.0f, iconCol);
				Text(FONT_BODY, sx + 24.0f, y + 14.0f, label, t.text, 0.94f);
			}
		};

		// Row 1: Save | Load | Slot N
		DrawActionTile(1, ICON_SAVE, "Save", t.accent);
		DrawActionTile(2, ICON_LOAD, "Load", slotExists[slot] ? t.accent : t.iconMuted);
		{
			const Rect & r = kCells[3];
			const float y = r.y + botSlide;
			DrawCard(r.x, y, r.w, r.h, false, pressedCell == 3, t, 8.0f);
			if (selection == 3)
				RoundedRect(r.x, y, r.w, r.h, 8.0f, t.focusFill);
			Icon(ICON_CHEVRON_LEFT,  r.x + 5.0f,        y + 15.0f, 12.0f, t.textDim);
			Icon(ICON_CHEVRON_RIGHT, r.x + r.w - 17.0f, y + 15.0f, 12.0f, t.textDim);
			char slotTitle[16];
			snprintf(slotTitle, sizeof(slotTitle), "Slot %d", slot);
			TextCentered(FONT_BODY, r.x + r.w * 0.5f, y + 7.0f, slotTitle, t.text, 0.94f);
			TextCentered(FONT_BODY, r.x + r.w * 0.5f, y + 22.0f,
			             slotExists[slot] ? "saved" : "empty",
			             slotExists[slot] ? t.n64Green : t.textDim, 0.82f);
		}

		// Row 2: Audio | Video | Controls
		DrawActionTile(4, gAudioPluginEnabled == APM_DISABLED ? ICON_AUDIO_MUTE : ICON_AUDIO,
		               "Audio", t.accent, AudioName((int)gAudioPluginEnabled));
		DrawActionTile(5, ICON_VIDEO,    "Video",    t.accent);
		DrawActionTile(6, ICON_CONTROLS, "Controls", t.accent);

		// Row 3: Reset | Game list | Exit
		DrawActionTile(7, ICON_RESET, "Reset",     t.iconMuted);
		DrawActionTile(8, ICON_GRID,  "Game list", t.accent);
		DrawActionTile(9, ICON_POWER, "Exit",      t.iconMuted);

		// Floating 2D spring-animated cyan 3DS focus border!
		RoundedRect(hlX - 1.0f, hlY + botSlide - 1.0f, hlW + 2.0f, hlH + 2.0f, 9.0f, ScaleAlpha(t.focusGlow, ease));
		RoundedBorder2(hlX, hlY + botSlide, hlW, hlH, 8.0f, ScaleAlpha(t.focusBorder, ease));

		if (toastFrames > 0)
		{
			RoundedRect(46.0f, 188.0f, 228.0f, 24.0f, 10.0f, WithAlpha(t.hintBg, 238));
			RoundedBorder1(46.0f, 188.0f, 228.0f, 24.0f, 10.0f, t.accent);
			TextCentered(FONT_BODY, 160.0f, 193.0f, toast, t.hintText, 0.86f);
			--toastFrames;
		}

		DrawHintBar(320.0f, kPauseHints, 3, t, "Y", t.isLight ? "Dark" : "Light");

		// ---- Top screen: Frozen live game frame + animated frosted status overlay!
		BeginScreen(GFX_TOP, 400, 240, t.bg);
		DrawFrostedGameBackdrop(freezeSnap, ease, t);

		// Top floating PAUSED pill + system clock/battery sliding down gently
		const float topSlide = -(1.0f - ease) * 14.0f;
		RoundedRect(14.0f, 12.0f + topSlide, 74.0f, 22.0f, 11.0f, ScaleAlpha(RGBA(16, 20, 28, 205), ease));
		RoundedBorder1(14.0f, 12.0f + topSlide, 74.0f, 22.0f, 11.0f, ScaleAlpha(t.accent, ease));
		TextCentered(FONT_BODY, 51.0f, 16.5f + topSlide, "PAUSED", ScaleAlpha(RGBA(244, 248, 255), ease), 0.84f);

		RoundedRect(298.0f, 12.0f + topSlide, 88.0f, 22.0f, 11.0f, ScaleAlpha(RGBA(16, 20, 28, 195), ease));
		DrawBatteryIcon(360.0f, 18.5f + topSlide, s_sysStatus.batteryLevel, s_sysStatus.charging,
		                ScaleAlpha(RGBA(230, 236, 244), ease));
		if (s_sysStatus.clockText[0])
			TextRight(FONT_BODY, 354.0f, 16.5f + topSlide, s_sysStatus.clockText, ScaleAlpha(RGBA(230, 236, 244), ease), 0.84f);

		// Bottom floating translucent game banner sliding smoothly upward over the frozen game!
		const float banY = 148.0f + (1.0f - ease) * 28.0f;
		RoundedRect(16.0f, banY, 368.0f, 78.0f, 12.0f, ScaleAlpha(RGBA(16, 20, 28, 218), ease));
		RoundedBorder1(16.0f, banY, 368.0f, 78.0f, 12.0f, ScaleAlpha(WithAlpha(t.accent, 140), ease));

		RoundedRect(28.0f, banY + 14.0f, 46.0f, 46.0f, 10.0f, ScaleAlpha(t.accent, ease));
		Icon(ICON_CARTRIDGE, 37.0f, banY + 23.0f, 28.0f, ScaleAlpha(RGBA(255, 255, 255), ease));

		TextEllipsis(FONT_TITLE, 86.0f, banY + 14.0f, 284.0f,
		             gameName ? gameName : "Nintendo 64", ScaleAlpha(RGBA(246, 249, 255), ease), 1.02f);

		char statLine[96];
		snprintf(statLine, sizeof(statLine), "%.1f FPS  |  Slot %d (%s)  |  Touch: %s",
		         gCurrentFramerate, slot, slotExists[slot] ? "saved" : "empty",
		         TouchPadEngine::LayoutShortName(TouchController::Engine().GetConfig().layout));
		Text(FONT_BODY, 86.0f, banY + 37.0f, statLine, ScaleAlpha(t.accent, ease), 0.86f);
		Text(FONT_BODY, 86.0f, banY + 55.0f, s_sysStatus.memSummary, ScaleAlpha(RGBA(172, 182, 198), ease), 0.80f);

		EndFrame();
	}

	// Free temporary 128 KiB pause snapshot & menu pool immediately before resuming gameplay!
	freezeSnap = nullptr;
	ReleaseAssets();
	UI::RestoreRenderState();
	AudioOutput::SetGamePaused(false);
}

} // namespace FrontendCTR
