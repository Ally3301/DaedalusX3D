// FrontendCTR: Official Nintendo 3DS / Virtual Console dual-screen frontend built on Citro3D.
// License: GNU GPL v2.0 or later (GPL-2.0-or-later) — see copying.txt & CREDITS.md.
//
// Features a 3D perspective-projected N64 Retail Box & N64 Game Pak Cartridge Virtual Console
// top-screen stage, a 2x3 Nintendo 3DS HOME Menu cartridge grid with a glowing 4-corner
// cyan selection cursor, and a unified L/R tabbed pause & settings menu with 5 visual
// Controller Pak save-state cards.

#include "stdafx.h"
#include "FrontendCTR.h"
#include "FrontendAudio.h"
#include "UITheme.h"
#include "UIAtlas.h"
#include "UIRenderer.h"
#include "UITouchInput.h"
#include "TouchControllerCTR.h"
#include "CoverScraperCTR.h"
#include "N64CartridgeMesh.h"
#include "UserInterface.h"

#include <3ds.h>
#include <citro3d.h>
#include <algorithm>
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

enum FilterTab { FILTER_ALL = 0, FILTER_FAV, FILTER_RECENT, FILTER_COUNT };
static const char * const kFilterNames[FILTER_COUNT] = { "All Games", "Favorites", "Recent" };
int   s_sortMode       = FILTER_ALL;
int   s_selected       = 0;
float s_hlPos          = 0.0f;
float s_hlVel          = 0.0f;
float s_hlX            = 16.0f, s_hlVX = 0.0f;
float s_hlY            = 38.0f, s_hlVY = 0.0f;
float s_scrollPos      = 0.0f;
float s_vcAnimTime     = 0.0f;
bool  s_initialised    = false;
bool  s_resetRequested = false;

CRefPtr<CNativeTexture> s_previewTex;
int s_previewRomIdx        = -1;
int s_previewPendingRomIdx = -1;
int s_previewDelayFrames   = 0;

// 6-slot page cover cache for the 2x3 3DS HOME Menu grid (freed before gameplay)
constexpr int kGridPageSize = 6;
CRefPtr<CNativeTexture> s_gridTex[kGridPageSize];
int s_gridTexRomIdx[kGridPageSize] = { -1, -1, -1, -1, -1, -1 };

const char * const kFrontendConfigPath = DAEDALUS_CTR_PATH("frontend.ini");
char s_lastSelectedFile[96] = "";
constexpr int kMaxFavs = 128;
char s_favList[kMaxFavs][96];
int  s_favCount = 0;
constexpr int kMaxRecents = 16;
char s_recentList[kMaxRecents][96];
int  s_recentCount = 0;

bool IsRecentRom(const char * filename, int * outRank = nullptr)
{
	for (int i = 0; i < s_recentCount; ++i)
	{
		if (strcmp(s_recentList[i], filename) == 0)
		{
			if (outRank) *outRank = i;
			return true;
		}
	}
	return false;
}

void PushRecentRom(const char * filename)
{
	if (!filename || !filename[0]) return;
	int existing = -1;
	for (int i = 0; i < s_recentCount; ++i)
	{
		if (strcmp(s_recentList[i], filename) == 0)
		{
			existing = i;
			break;
		}
	}
	if (existing == 0) return;
	if (existing > 0)
	{
		for (int i = existing; i > 0; --i)
			CopyStr(s_recentList[i], sizeof(s_recentList[i]), s_recentList[i - 1]);
		CopyStr(s_recentList[0], sizeof(s_recentList[0]), filename);
		return;
	}
	const int limit = (s_recentCount < kMaxRecents) ? s_recentCount : (kMaxRecents - 1);
	for (int i = limit; i > 0; --i)
		CopyStr(s_recentList[i], sizeof(s_recentList[i]), s_recentList[i - 1]);
	CopyStr(s_recentList[0], sizeof(s_recentList[0]), filename);
	if (s_recentCount < kMaxRecents)
		++s_recentCount;
}

void LoadFrontendConfig()
{
	s_favCount = 0;
	s_recentCount = 0;
	s_lastSelectedFile[0] = '\0';
	// Default to authentic 3DS Home Menu Light theme on the bottom screen
	SetThemeIndex(THEME_LIGHT);

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
			if (v >= 0 && v < FILTER_COUNT)
				s_sortMode = v;
		}
		else if (!strncmp(line, "last=", 5))
		{
			CopyStr(s_lastSelectedFile, sizeof(s_lastSelectedFile), line + 5);
			if (line[5]) PushRecentRom(line + 5);
		}
		else if (!strncmp(line, "fav=", 4) && line[4] && s_favCount < kMaxFavs)
		{
			CopyStr(s_favList[s_favCount++], sizeof(s_favList[0]), line + 4);
		}
		else if (!strncmp(line, "recent=", 7) && line[7] && s_recentCount < kMaxRecents)
		{
			CopyStr(s_recentList[s_recentCount++], sizeof(s_recentList[0]), line + 7);
		}
	}
	fclose(f);
}

void SaveFrontendConfig()
{
	FILE * f = fopen(kFrontendConfigPath, "w");
	if (!f) return;
	fprintf(f, "theme=%d\nsort=%d\nlast=%s\n", GetThemeIndex(), s_sortMode, s_lastSelectedFile);
	for (int i = 0; i < s_romCount; ++i)
	{
		if (s_roms[i].favorite)
			fprintf(f, "fav=%s\n", s_roms[i].filename);
	}
	for (int i = 0; i < s_recentCount; ++i)
	{
		if (s_recentList[i][0])
			fprintf(f, "recent=%s\n", s_recentList[i]);
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
		if (s_sortMode == FILTER_FAV && !s_roms[i].favorite)
			continue;
		if (s_sortMode == FILTER_RECENT && !IsRecentRom(s_roms[i].filename))
			continue;
		s_viewOrder[s_viewCount++] = i;
	}
	if (s_viewCount == 0)
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
			if (s_sortMode == FILTER_RECENT)
			{
				int rPrev = 999, rCur = 999;
				IsRecentRom(prev.filename, &rPrev);
				IsRecentRom(cur.filename, &rCur);
				swap = (rPrev > rCur) || (rPrev == rCur && strcasecmp(prev.title, cur.title) > 0);
			}
			else if (s_sortMode == FILTER_ALL && prev.favorite != cur.favorite)
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
	for (int i = 0; i < kGridPageSize; ++i)
	{
		s_gridTex[i] = nullptr;
		s_gridTexRomIdx[i] = -1;
	}
}

void RequestPreviewForRom(int romIdx, bool immediate = false)
{
	if (romIdx == s_previewRomIdx && s_previewPendingRomIdx == -1)
		return;

	// If the requested ROM's cover is already resident in the visible 2x3 grid cache,
	// adopt it immediately with zero delay and zero disk I/O (prevents previous cover ghosting).
	for (int slot = 0; slot < kGridPageSize; ++slot)
	{
		if (s_gridTexRomIdx[slot] == romIdx && s_gridTex[slot] != nullptr && s_gridTex[slot]->HasData())
		{
			s_previewTex           = s_gridTex[slot];
			s_previewRomIdx        = romIdx;
			s_previewPendingRomIdx = -1;
			s_previewDelayFrames   = 0;
			return;
		}
	}

	// Never leave the previous ROM's texture bound while waiting for a different ROM to load.
	if (s_previewRomIdx != romIdx)
	{
		s_previewTex    = nullptr;
		s_previewRomIdx = -1;
	}

	if (immediate)
	{
		s_previewPendingRomIdx = romIdx;
		s_previewDelayFrames   = 0;
		return;
	}
	if (s_previewPendingRomIdx != romIdx)
	{
		s_previewPendingRomIdx = romIdx;
		s_previewDelayFrames   = 2;
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

void TickGridPageCovers(int pageStart)
{
	// Load at most 1 missing tile thumbnail per frame so the 2x3 3DS Home Menu grid stays 60 FPS
	for (int slot = 0; slot < kGridPageSize; ++slot)
	{
		const int viewIdx = pageStart + slot;
		const int wantRom = (viewIdx >= 0 && viewIdx < s_viewCount) ? s_viewOrder[viewIdx] : -1;
		if (s_gridTexRomIdx[slot] == wantRom)
			continue;
		s_gridTex[slot] = nullptr;
		s_gridTexRomIdx[slot] = wantRom;
		if (wantRom < 0 || wantRom >= s_romCount)
			continue;
		if (wantRom == s_previewRomIdx && s_previewTex != nullptr)
		{
			s_gridTex[slot] = s_previewTex;
			continue;
		}
		IO::Filename p;
		BuildPreviewPathForRom(s_roms[wantRom], p);
		if (IO::File::Exists(p))
		{
			s_gridTex[slot] = CNativeTexture::CreateFromPng(p, TexFmt_5551);
			if (s_gridTex[slot] != nullptr && s_gridTex[slot]->HasData())
				s_gridTex[slot]->ApplyParams(GPU_LINEAR, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			return; // max 1 PNG load per frame
		}
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

// ============================================================================
// 3D Perspective Virtual Console Showcase (3D N64 Box + 3D N64 Game Pak)
// ============================================================================
struct Vec3
{
	float x, y, z;
};

static inline Vec3 RotateYX(Vec3 p, float yaw, float pitch)
{
	const float cy = cosf(yaw), sy = sinf(yaw);
	const float x1 = p.x * cy + p.z * sy;
	const float z1 = -p.x * sy + p.z * cy;
	const float cp = cosf(pitch), sp = sinf(pitch);
	const float y2 = p.y * cp - z1 * sp;
	const float z2 = p.y * sp + z1 * cp;
	return { x1, y2, z2 };
}

static inline void Project3D(Vec3 p, float cx, float cy, float fov, float camDist, float & sx, float & sy)
{
	const float z = p.z + camDist;
	const float invZ = (z > 24.0f) ? (fov / z) : (fov / 24.0f);
	sx = cx + p.x * invZ;
	sy = cy + p.y * invZ;
}

static inline Vec3 LerpVec3(Vec3 a, Vec3 b, float t)
{
	return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}

static inline Vec3 BilerpVec3(Vec3 tl, Vec3 tr, Vec3 br, Vec3 bl, float u, float v)
{
	return LerpVec3(LerpVec3(tl, tr, u), LerpVec3(bl, br, u), v);
}

static void DrawQuad3D(Vec3 tl, Vec3 tr, Vec3 br, Vec3 bl,
                       float cx, float cy, float fov, float camDist,
                       uint32_t cTL, uint32_t cTR, uint32_t cBR, uint32_t cBL)
{
	float x0, y0, x1, y1, x2, y2, x3, y3;
	Project3D(tl, cx, cy, fov, camDist, x0, y0);
	Project3D(tr, cx, cy, fov, camDist, x1, y1);
	Project3D(br, cx, cy, fov, camDist, x2, y2);
	Project3D(bl, cx, cy, fov, camDist, x3, y3);
	PolyQuad(x0, y0, x1, y1, x2, y2, x3, y3, cTL, cTR, cBR, cBL);
}

// Back-face culled 3D quad: draws only when vertices wind clockwise in screen space (+X right, +Y down).
static bool DrawQuad3DCulled(Vec3 tl, Vec3 tr, Vec3 br, Vec3 bl,
                             float cx, float cy, float fov, float camDist,
                             uint32_t cTL, uint32_t cTR, uint32_t cBR, uint32_t cBL)
{
	float x0, y0, x1, y1, x2, y2, x3, y3;
	Project3D(tl, cx, cy, fov, camDist, x0, y0);
	Project3D(tr, cx, cy, fov, camDist, x1, y1);
	Project3D(br, cx, cy, fov, camDist, x2, y2);
	Project3D(bl, cx, cy, fov, camDist, x3, y3);
	const float cross = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
	if (cross <= 0.0f) return false;
	PolyQuad(x0, y0, x1, y1, x2, y2, x3, y3, cTL, cTR, cBR, cBL);
	return true;
}

// 4x4 perspective-subdivided textured 3D surface (eliminates affine warp on rotated 3D faces).
static void DrawTexturedMesh3D(C3D_Tex * tex,
                               Vec3 tl, Vec3 tr, Vec3 br, Vec3 bl,
                               float u0, float v0, float u1, float v1,
                               float cx, float cy, float fov, float camDist,
                               uint32_t cTop, uint32_t cBot)
{
	if (!tex) return;
	constexpr int kSub = 4;
	const float inv = 1.0f / (float)kSub;
	for (int r = 0; r < kSub; ++r)
	{
		const float vA = r * inv;
		const float vB = (r + 1) * inv;
		const float tv0 = v0 + (v1 - v0) * vA;
		const float tv1 = v0 + (v1 - v0) * vB;
		const uint32_t colA = LerpColor(cTop, cBot, vA);
		const uint32_t colB = LerpColor(cTop, cBot, vB);
		for (int c = 0; c < kSub; ++c)
		{
			const float uA = c * inv;
			const float uB = (c + 1) * inv;
			const float tu0 = u0 + (u1 - u0) * uA;
			const float tu1 = u0 + (u1 - u0) * uB;

			const Vec3 p00 = BilerpVec3(tl, tr, br, bl, uA, vA);
			const Vec3 p10 = BilerpVec3(tl, tr, br, bl, uB, vA);
			const Vec3 p11 = BilerpVec3(tl, tr, br, bl, uB, vB);
			const Vec3 p01 = BilerpVec3(tl, tr, br, bl, uA, vB);

			float x0, y0, x1, y1, x2, y2, x3, y3;
			Project3D(p00, cx, cy, fov, camDist, x0, y0);
			Project3D(p10, cx, cy, fov, camDist, x1, y1);
			Project3D(p11, cx, cy, fov, camDist, x2, y2);
			Project3D(p01, cx, cy, fov, camDist, x3, y3);
			TexturedPolyQuad(tex, x0, y0, x1, y1, x2, y2, x3, y3,
			                 tu0, tv0, tu1, tv1, colA, colA, colB, colB);
		}
	}
}

static void Draw3DN64Box(const CRefPtr<CNativeTexture> & coverTex,
                         float cx, float cy, float yaw, float pitch, float alphaMul)
{
	// Authentic Nintendo 64 Retail Box proportions (~180mm x 126mm x 34mm -> 1.42 : 1.0 : 0.27)
	const float fov     = 290.0f;
	const float camDist = 290.0f;
	const float hw      = 51.0f;
	const float hh      = 36.0f;
	const float hd      = 14.5f;

	auto Xf = [&](float x, float y, float z) -> Vec3 {
		return RotateYX({ x, y, z }, yaw, pitch);
	};

	// 1. Soft Virtual Console stage contact shadow & subtle mirror reflection
	{
		const float floorY = hh + 1.0f;
		const float reflH  = 24.0f;

		// Soft contact shadow directly under the box footprint
		DrawQuad3D(Xf(-hw - 3.0f, hh + 0.5f, -hd - 2.0f), Xf( hw + 3.0f, hh + 0.5f, -hd - 2.0f),
		           Xf( hw + 3.0f, hh + 6.5f,  hd + 2.0f), Xf(-hw - 3.0f, hh + 6.5f,  hd + 2.0f),
		           cx, cy, fov, camDist,
		           ScaleAlpha(RGBA(4, 6, 12, 110), alphaMul),
		           ScaleAlpha(RGBA(4, 6, 12, 110), alphaMul),
		           RGBA(4, 6, 12, 0), RGBA(4, 6, 12, 0));

		// Subtle mirrored reflection of left spine
		DrawQuad3DCulled(Xf(-hw, floorY,         hd), Xf(-hw, floorY,        -hd),
		                 Xf(-hw, floorY + reflH,-hd), Xf(-hw, floorY + reflH, hd),
		                 cx, cy, fov, camDist,
		                 ScaleAlpha(RGBA(140, 18, 28, 34), alphaMul),
		                 ScaleAlpha(RGBA(185, 24, 34, 44), alphaMul),
		                 RGBA(185, 24, 34, 0), RGBA(140, 18, 28, 0));

		// Subtle mirrored reflection of front box art
		const Vec3 rTL = Xf(-hw, floorY,        -hd);
		const Vec3 rTR = Xf( hw, floorY,        -hd);
		const Vec3 rBR = Xf( hw, floorY + reflH,-hd);
		const Vec3 rBL = Xf(-hw, floorY + reflH,-hd);
		if (coverTex != nullptr && coverTex->HasData())
		{
			const float u1 = (float)coverTex->GetWidth() * coverTex->GetScaleX();
			const float v1 = (float)coverTex->GetHeight() * coverTex->GetScaleY();
			DrawTexturedMesh3D(coverTex->GetC3DTexture(), rTL, rTR, rBR, rBL,
			                   0.0f, v1, u1, v1 * 0.52f, cx, cy, fov, camDist,
			                   ScaleAlpha(RGBA(190, 200, 220, 40), alphaMul),
			                   RGBA(190, 200, 220, 0));
		}
		else
		{
			DrawQuad3D(rTL, rTR, rBR, rBL, cx, cy, fov, camDist,
			           ScaleAlpha(RGBA(42, 62, 108, 38), alphaMul),
			           ScaleAlpha(RGBA(42, 62, 108, 38), alphaMul),
			           RGBA(20, 30, 50, 0), RGBA(20, 30, 50, 0));
		}
	}

	// 2. Top 3D Cardboard Lid (visible when looking slightly down: pitch > 0)
	if (DrawQuad3DCulled(Xf(-hw, -hh,  hd), Xf( hw, -hh,  hd),
	                     Xf( hw, -hh, -hd), Xf(-hw, -hh, -hd),
	                     cx, cy, fov, camDist,
	                     ScaleAlpha(RGBA(34, 40, 52), alphaMul),
	                     ScaleAlpha(RGBA(26, 30, 40), alphaMul),
	                     ScaleAlpha(RGBA(50, 58, 72), alphaMul),
	                     ScaleAlpha(RGBA(64, 72, 88), alphaMul)))
	{
		// Subtle cardboard tuck-flap seam line along top lid
		DrawQuad3DCulled(Xf(-hw + 3.0f, -hh,  3.5f), Xf( hw - 3.0f, -hh,  3.5f),
		                 Xf( hw - 3.0f, -hh,  2.2f), Xf(-hw + 3.0f, -hh,  2.2f),
		                 cx, cy, fov, camDist,
		                 ScaleAlpha(RGBA(18, 22, 30, 160), alphaMul),
		                 ScaleAlpha(RGBA(18, 22, 30, 160), alphaMul),
		                 ScaleAlpha(RGBA(82, 92, 112, 90), alphaMul),
		                 ScaleAlpha(RGBA(82, 92, 112, 90), alphaMul));
	}

	// 3. Left 3D Spine Face (signature Nintendo 64 Red spine, visible when yaw < 0)
	if (DrawQuad3DCulled(Xf(-hw, -hh,  hd), Xf(-hw, -hh, -hd),
	                     Xf(-hw,  hh, -hd), Xf(-hw,  hh,  hd),
	                     cx, cy, fov, camDist,
	                     ScaleAlpha(RGBA(158, 16, 26), alphaMul),
	                     ScaleAlpha(RGBA(224, 26, 38), alphaMul),
	                     ScaleAlpha(RGBA(184, 18, 28), alphaMul),
	                     ScaleAlpha(RGBA(126, 12, 20), alphaMul)))
	{
		// Top dark N64 header cap on spine
		const float spSplit = -hh + 16.0f;
		DrawQuad3DCulled(Xf(-hw, -hh,     hd), Xf(-hw, -hh,    -hd),
		                 Xf(-hw, spSplit,-hd), Xf(-hw, spSplit, hd),
		                 cx, cy, fov, camDist,
		                 ScaleAlpha(RGBA(22, 26, 36), alphaMul),
		                 ScaleAlpha(RGBA(38, 44, 58), alphaMul),
		                 ScaleAlpha(RGBA(32, 38, 52), alphaMul),
		                 ScaleAlpha(RGBA(18, 22, 30), alphaMul));
		// Miniature 4-color N64 badge on the spine top cap
		DrawQuad3DCulled(Xf(-hw, -hh + 4.5f,  4.5f), Xf(-hw, -hh + 4.5f, -4.5f),
		                 Xf(-hw, -hh + 12.0f,-4.5f), Xf(-hw, -hh + 12.0f, 4.5f),
		                 cx, cy, fov, camDist,
		                 ScaleAlpha(RGBA(24, 184, 88), alphaMul),
		                 ScaleAlpha(RGBA(248, 202, 24), alphaMul),
		                 ScaleAlpha(RGBA(28, 102, 236), alphaMul),
		                 ScaleAlpha(RGBA(232, 36, 46), alphaMul));
		// Crisp gold divider rule between dark cap and red spine
		DrawQuad3DCulled(Xf(-hw, spSplit,        hd), Xf(-hw, spSplit,       -hd),
		                 Xf(-hw, spSplit + 2.2f,-hd), Xf(-hw, spSplit + 2.2f, hd),
		                 cx, cy, fov, camDist,
		                 ScaleAlpha(RGBA(200, 156, 18), alphaMul),
		                 ScaleAlpha(RGBA(252, 210, 42), alphaMul),
		                 ScaleAlpha(RGBA(232, 186, 26), alphaMul),
		                 ScaleAlpha(RGBA(175, 134, 14), alphaMul));
		// Subtle specular edge highlight along the front-left cardboard crease
		DrawQuad3DCulled(Xf(-hw, -hh, -hd + 1.4f), Xf(-hw, -hh, -hd),
		                 Xf(-hw,  hh, -hd),        Xf(-hw,  hh, -hd + 1.4f),
		                 cx, cy, fov, camDist,
		                 ScaleAlpha(RGBA(255, 255, 255, 10), alphaMul),
		                 ScaleAlpha(RGBA(255, 255, 255, 58), alphaMul),
		                 ScaleAlpha(RGBA(255, 255, 255, 28), alphaMul),
		                 ScaleAlpha(RGBA(255, 255, 255, 0),  alphaMul));
	}

	// Right 3D Spine Face (watertight fallback if yaw ever crosses > 0)
	DrawQuad3DCulled(Xf(hw, -hh, -hd), Xf(hw, -hh,  hd),
	                 Xf(hw,  hh,  hd), Xf(hw,  hh, -hd),
	                 cx, cy, fov, camDist,
	                 ScaleAlpha(RGBA(210, 22, 34), alphaMul),
	                 ScaleAlpha(RGBA(145, 14, 22), alphaMul),
	                 ScaleAlpha(RGBA(115, 10, 18), alphaMul),
	                 ScaleAlpha(RGBA(175, 18, 28), alphaMul));

	// 4. Front 3D Cover Face (full-bleed landscape cover art — no duplicate red bar!)
	const Vec3 fTL = Xf(-hw, -hh, -hd);
	const Vec3 fTR = Xf( hw, -hh, -hd);
	const Vec3 fBR = Xf( hw,  hh, -hd);
	const Vec3 fBL = Xf(-hw,  hh, -hd);

	// Crisp outer dark cardboard rim
	DrawQuad3DCulled(fTL, fTR, fBR, fBL, cx, cy, fov, camDist,
	                 ScaleAlpha(RGBA(44, 50, 64), alphaMul),
	                 ScaleAlpha(RGBA(30, 35, 46), alphaMul),
	                 ScaleAlpha(RGBA(22, 26, 36), alphaMul),
	                 ScaleAlpha(RGBA(32, 38, 50), alphaMul));

	const Vec3 aTL = Xf(-hw + 1.4f, -hh + 1.4f, -hd);
	const Vec3 aTR = Xf( hw - 1.4f, -hh + 1.4f, -hd);
	const Vec3 aBR = Xf( hw - 1.4f,  hh - 1.4f, -hd);
	const Vec3 aBL = Xf(-hw + 1.4f,  hh - 1.4f, -hd);

	if (coverTex != nullptr && coverTex->HasData())
	{
		const float u1 = (float)coverTex->GetWidth() * coverTex->GetScaleX();
		const float v1 = (float)coverTex->GetHeight() * coverTex->GetScaleY();
		DrawTexturedMesh3D(coverTex->GetC3DTexture(), aTL, aTR, aBR, aBL,
		                   0.0f, 0.0f, u1, v1, cx, cy, fov, camDist,
		                   ScaleAlpha(RGBA(255, 255, 255, 255), alphaMul),
		                   ScaleAlpha(RGBA(232, 238, 248, 255), alphaMul));
	}
	else
	{
		// Stylized N64 4-color cover placeholder when no boxart is cached
		DrawQuad3D(aTL, aTR, aBR, aBL, cx, cy, fov, camDist,
		           ScaleAlpha(RGBA(26, 52, 110), alphaMul),
		           ScaleAlpha(RGBA(18, 36, 78), alphaMul),
		           ScaleAlpha(RGBA(14, 24, 52), alphaMul),
		           ScaleAlpha(RGBA(22, 42, 88), alphaMul));
		DrawQuad3D(Xf(-18.0f, -14.0f, -hd), Xf(18.0f, -14.0f, -hd),
		           Xf(18.0f,  14.0f, -hd), Xf(-18.0f, 14.0f, -hd),
		           cx, cy, fov, camDist,
		           ScaleAlpha(RGBA(24, 184, 88), alphaMul),
		           ScaleAlpha(RGBA(247, 198, 22), alphaMul),
		           ScaleAlpha(RGBA(30, 90, 232), alphaMul),
		           ScaleAlpha(RGBA(232, 46, 56), alphaMul));
	}

	// Dynamic diagonal studio specular gloss sweeping subtly across the glossy box
	const float sheenX = -6.0f + sinf(yaw * 2.5f) * 14.0f;
	DrawQuad3D(Xf(sheenX - 10.0f, -hh + 1.4f, -hd), Xf(sheenX + 8.0f, -hh + 1.4f, -hd),
	           Xf(sheenX - 18.0f,  hh - 1.4f, -hd), Xf(sheenX - 36.0f, hh - 1.4f, -hd),
	           cx, cy, fov, camDist,
	           ScaleAlpha(RGBA(255, 255, 255, 26), alphaMul),
	           ScaleAlpha(RGBA(255, 255, 255, 8),  alphaMul),
	           RGBA(255, 255, 255, 0),
	           ScaleAlpha(RGBA(255, 255, 255, 14), alphaMul));
}

struct VisCartTri
{
	float    zAvg;
	uint16_t triIdx;
	float    x0, y0, x1, y1, x2, y2;
	uint32_t c0, c1, c2;
};

static VisCartTri s_visCartTris[N64CartMesh::kNumCartTris];

static inline uint32_t ShadeCartVertex(Vec3 n, uint8_t mat, float u, float v, bool hasTex, float alphaMul)
{
	// Normalized studio key light, rim light, and Blinn-Phong half-vector
	constexpr float kLx = -0.422f, kLy = -0.583f, kLz = -0.694f;
	constexpr float kRx =  0.761f, kRy = -0.250f, kRz =  0.599f;
	constexpr float kHx = -0.299f, kHy = -0.413f, kHz = -0.860f;

	float br = 150.0f, bg = 156.0f, bb = 168.0f;
	switch (mat)
	{
	case N64CartMesh::MAT_SHELL_MAIN:   br = 152.0f; bg = 158.0f; bb = 170.0f; break;
	case N64CartMesh::MAT_SHELL_WING:   br = 134.0f; bg = 140.0f; bb = 152.0f; break;
	case N64CartMesh::MAT_SHELL_BEVEL:  br = 168.0f; bg = 174.0f; bb = 186.0f; break;
	case N64CartMesh::MAT_SHELL_GROOVE: br =  54.0f; bg =  60.0f; bb =  70.0f; break;
	case N64CartMesh::MAT_CARTOUCHE:    br =  92.0f; bg =  98.0f; bb = 110.0f; break;
	case N64CartMesh::MAT_STICKER_RIM:  br =  72.0f; bg =  78.0f; bb =  90.0f; break;
	case N64CartMesh::MAT_PCB_CHAMBER:  br =  22.0f; bg =  26.0f; bb =  34.0f; break;
	case N64CartMesh::MAT_PCB_PINS:     br = 228.0f; bg = 182.0f; bb =  52.0f; break;
	case N64CartMesh::MAT_STICKER_TEX:
		if (hasTex)
		{
			br = 248.0f; bg = 250.0f; bb = 255.0f;
		}
		else
		{
			br = 38.0f + 160.0f * u;
			bg = 72.0f + 100.0f * (1.0f - v);
			bb = 210.0f - 95.0f * u;
		}
		break;
	}

	const float ndotl = fmaxf(0.0f, n.x * kLx + n.y * kLy + n.z * kLz);
	const float ndotr = fmaxf(0.0f, n.x * kRx + n.y * kRy + n.z * kRz);
	const float ndoth = fmaxf(0.0f, n.x * kHx + n.y * kHy + n.z * kHz);
	const float h2    = ndoth * ndoth;
	const float h4    = h2 * h2;
	const float h16   = h4 * h4;

	if (mat == N64CartMesh::MAT_STICKER_TEX && hasTex)
	{
		// Keep sticker texture vivid while modulating subtly with 3D surface lighting + gloss
		const float diff = fminf(1.06f, 0.72f + 0.32f * ndotl + 0.10f * ndotr + 0.20f * h16);
		const int r = (int)fminf(255.0f, br * diff);
		const int g = (int)fminf(255.0f, bg * diff);
		const int b = (int)fminf(255.0f, bb * diff);
		return ScaleAlpha(RGBA((u8)r, (u8)g, (u8)b, 255), alphaMul);
	}

	const float specMul = (mat == N64CartMesh::MAT_SHELL_GROOVE || mat == N64CartMesh::MAT_PCB_CHAMBER) ? 0.04f :
	                      (mat == N64CartMesh::MAT_PCB_PINS) ? 0.75f : 0.44f;
	const float spec = h16 * specMul * 255.0f;
	const float diff = 0.36f + 0.58f * ndotl + 0.22f * ndotr;

	const int r = (int)fminf(255.0f, br * diff + spec);
	const int g = (int)fminf(255.0f, bg * diff + spec * 0.97f);
	const int b = (int)fminf(255.0f, bb * diff + spec);
	return ScaleAlpha(RGBA((u8)r, (u8)g, (u8)b, 255), alphaMul);
}

static void Draw3DN64Cartridge(const CRefPtr<CNativeTexture> & coverTex,
                               float cx, float cy, float yaw, float pitch, float alphaMul)
{
	const float fov     = 295.0f;
	const float camDist = 285.0f;
	const float hw      = 49.0f;
	const float hh      = 32.5f;
	const float hd      = 9.8f;

	auto Xf = [&](float x, float y, float z) -> Vec3 {
		return RotateYX({ x, y, z }, yaw, pitch);
	};

	// 1. Soft Virtual Console stage contact shadow & subtle floor reflection
	{
		const float floorY = hh + 1.5f;
		const float reflH  = 20.0f;

		DrawQuad3D(Xf(-hw - 2.0f, hh + 0.5f, -hd - 2.0f), Xf( hw + 2.0f, hh + 0.5f, -hd - 2.0f),
		           Xf( hw + 2.0f, hh + 6.0f,  hd + 2.0f), Xf(-hw - 2.0f, hh + 6.0f,  hd + 2.0f),
		           cx, cy, fov, camDist,
		           ScaleAlpha(RGBA(4, 6, 12, 108), alphaMul),
		           ScaleAlpha(RGBA(4, 6, 12, 108), alphaMul),
		           RGBA(4, 6, 12, 0), RGBA(4, 6, 12, 0));

		DrawQuad3D(Xf(-hw, floorY,        -hd), Xf( hw, floorY,        -hd),
		           Xf( hw, floorY + reflH,-hd), Xf(-hw, floorY + reflH,-hd),
		           cx, cy, fov, camDist,
		           ScaleAlpha(RGBA(126, 134, 148, 34), alphaMul),
		           ScaleAlpha(RGBA(126, 134, 148, 34), alphaMul),
		           RGBA(64, 68, 78, 0), RGBA(64, 68, 78, 0));
	}

	// 2. Transform, back-face cull, Blinn-Phong light, and depth-sort the 3D N64 Cartridge Mesh!
	const bool hasTex = (coverTex != nullptr && coverTex->HasData());
	int visCount = 0;

	for (int i = 0; i < N64CartMesh::kNumCartTris; ++i)
	{
		const N64CartMesh::CartTri & t = N64CartMesh::kCartTris[i];
		const Vec3 p0 = RotateYX({ t.x0, t.y0, t.z0 }, yaw, pitch);
		const Vec3 p1 = RotateYX({ t.x1, t.y1, t.z1 }, yaw, pitch);
		const Vec3 p2 = RotateYX({ t.x2, t.y2, t.z2 }, yaw, pitch);

		float x0, y0, x1, y1, x2, y2;
		Project3D(p0, cx, cy, fov, camDist, x0, y0);
		Project3D(p1, cx, cy, fov, camDist, x1, y1);
		Project3D(p2, cx, cy, fov, camDist, x2, y2);

		const float cross = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
		if (cross <= 0.0f)
			continue;

		const Vec3 n0 = RotateYX({ t.nx0, t.ny0, t.nz0 }, yaw, pitch);
		const Vec3 n1 = RotateYX({ t.nx1, t.ny1, t.nz1 }, yaw, pitch);
		const Vec3 n2 = RotateYX({ t.nx2, t.ny2, t.nz2 }, yaw, pitch);

		VisCartTri & v = s_visCartTris[visCount++];
		v.zAvg   = (p0.z + p1.z + p2.z); // 3x average Z (avoids divide)
		v.triIdx = (uint16_t)i;
		v.x0 = x0; v.y0 = y0;
		v.x1 = x1; v.y1 = y1;
		v.x2 = x2; v.y2 = y2;
		v.c0 = ShadeCartVertex(n0, t.mat, t.u0, t.v0, hasTex, alphaMul);
		v.c1 = ShadeCartVertex(n1, t.mat, t.u1, t.v1, hasTex, alphaMul);
		v.c2 = ShadeCartVertex(n2, t.mat, t.u2, t.v2, hasTex, alphaMul);
	}

	std::sort(s_visCartTris, s_visCartTris + visCount,
	          [](const VisCartTri & a, const VisCartTri & b) { return a.zAvg > b.zAvg; });

	C3D_Tex * c3dTex = hasTex ? coverTex->GetC3DTexture() : nullptr;
	const float uMax = hasTex ? ((float)coverTex->GetWidth()  * coverTex->GetScaleX()) : 1.0f;
	const float vMax = hasTex ? ((float)coverTex->GetHeight() * coverTex->GetScaleY()) : 1.0f;

	for (int i = 0; i < visCount; ++i)
	{
		const VisCartTri & v = s_visCartTris[i];
		const N64CartMesh::CartTri & t = N64CartMesh::kCartTris[v.triIdx];
		if (t.mat == N64CartMesh::MAT_STICKER_TEX && c3dTex != nullptr)
		{
			TexturedPolyTri(c3dTex,
			                v.x0, v.y0, v.x1, v.y1, v.x2, v.y2,
			                t.u0 * uMax, t.v0 * vMax,
			                t.u1 * uMax, t.v1 * vMax,
			                t.u2 * uMax, t.v2 * vMax,
			                v.c0, v.c1, v.c2);
		}
		else
		{
			PolyTri(v.x0, v.y0, v.x1, v.y1, v.x2, v.y2, v.c0, v.c1, v.c2);
		}
	}
}

static void DrawTopRightStatusPill(const ThemePalette & t)
{
	const float px = 212.0f, py = 9.0f, pw = 178.0f, ph = 20.0f;
	const uint32_t pillBg  = t.isLight ? RGBA(255, 255, 255, 224) : WithAlpha(t.headerBg, 205);
	const uint32_t pillRim = WithAlpha(t.cardBorder, t.isLight ? 210 : 145);
	RoundedRect(px, py, pw, ph, 10.0f, pillBg);
	RoundedBorder1(px, py, pw, ph, 10.0f, pillRim);

	Text(FONT_BODY, px + 11.0f, py + 3.5f, "DaedalusX3D", t.text, 0.80f);
	Quad(306.0f, py + 4.5f, 1.0f, 11.0f, WithAlpha(t.cardBorder, 190));
	if (s_sysStatus.clockText[0])
		TextRight(FONT_BODY, 358.0f, py + 3.5f, s_sysStatus.clockText, t.textMuted, 0.82f);
	DrawBatteryIcon(364.0f, py + 6.0f, s_sysStatus.batteryLevel, s_sysStatus.charging, t.n64Green);
}

static void DrawVirtualConsoleTopStage(const FrontendRomInfo * rom,
                                       const CRefPtr<CNativeTexture> & coverTex,
                                       float infoEase, float animTime,
                                       const ThemePalette & t)
{
	// Theme-aware reflective studio backdrop with soft atmospheric horizon glow
	const uint32_t skyTop   = t.isLight ? RGBA(238, 242, 248) : LerpColor(t.headerBg, RGBA(0, 0, 0), 0.25f);
	const uint32_t skyBot   = t.isLight ? RGBA(218, 225, 238) : LerpColor(t.bg, t.cardBg, 0.45f);
	const uint32_t floorTop = t.isLight ? RGBA(208, 216, 230) : LerpColor(t.bg, t.cardBg, 0.65f);
	const uint32_t floorBot = t.isLight ? RGBA(232, 236, 244) : LerpColor(t.headerBg, RGBA(0, 0, 0), 0.35f);
	const uint32_t glowCol  = WithAlpha(t.accent, t.isLight ? 26 : 30);

	QuadVGrad(0.0f, 0.0f, 400.0f, 146.0f, skyTop, skyBot);
	QuadVGrad(0.0f, 146.0f, 400.0f, 94.0f, floorTop, floorBot);
	QuadVGrad(0.0f, 134.0f, 400.0f, 12.0f, WithAlpha(t.accent, 0), glowCol);
	QuadVGrad(0.0f, 146.0f, 400.0f, 16.0f, WithAlpha(t.accent, t.isLight ? 20 : 24), WithAlpha(t.accent, 0));

	DrawTopRightStatusPill(t);

	if (!rom)
	{
		TextCentered(FONT_TITLE, 200.0f, 102.0f, "DaedalusX3D", t.text, 1.25f);
		TextCentered(FONT_BODY, 200.0f, 132.0f, "Nintendo 64 Emulator for Nintendo 3DS", t.textMuted, 0.92f);
		return;
	}

	// Smooth 3D entrance & floating Idle Animation
	// Box turns inward to the right (yaw < 0 -> shows Left Red N64 Spine + Front Cover + Top Lid)
	// Cartridge turns inward to the left (yaw > 0 -> shows Right Grooved Side Wall + Arch + Label)
	const float slideOffset = (1.0f - infoEase) * 20.0f;
	const float bobBox  = sinf(animTime * 2.0f) * 1.9f;
	const float bobCart = cosf(animTime * 1.85f + 0.7f) * 1.7f;
	const float yawBox  = -0.44f + 0.052f * sinf(animTime * 1.25f) - (1.0f - infoEase) * 0.30f;
	const float pitBox  =  0.12f + 0.020f * cosf(animTime * 1.05f);
	const float yawCart =  0.37f + 0.052f * cosf(animTime * 1.20f) + (1.0f - infoEase) * 0.30f;
	const float pitCart =  0.12f + 0.020f * sinf(animTime * 1.00f);

	// Render 3D N64 Retail Box on the left & 3D N64 Game Pak Cartridge on the right
	Draw3DN64Box(coverTex, 122.0f - slideOffset, 93.0f + bobBox, yawBox, pitBox, infoEase);
	Draw3DN64Cartridge(coverTex, 282.0f + slideOffset, 98.0f + bobCart, yawCart, pitCart, infoEase);

	// Centered Game Title with subtle legibility shadow & 3 themed glass metadata pills
	if (!t.isLight)
	{
		TextCenteredEllipsis(FONT_TITLE, 200.0f, 173.0f, 368.0f,
		                     rom->title, ScaleAlpha(RGBA(4, 7, 14, 185), infoEase), 1.12f);
	}
	TextCenteredEllipsis(FONT_TITLE, 200.0f, 172.0f, 368.0f,
	                     rom->title, ScaleAlpha(t.text, infoEase), 1.12f);

	char sizeStr[24];
	snprintf(sizeStr, sizeof(sizeStr), "%.0f MB", fmaxf(1.0f, rom->size / (1024.0f * 1024.0f)));
	const uint32_t pillFill   = ScaleAlpha(WithAlpha(t.cardBg, t.isLight ? 235 : 222), infoEase);
	const uint32_t pillBorder = ScaleAlpha(WithAlpha(t.cardBorder, 185), infoEase);
	const uint32_t pillText   = ScaleAlpha(t.text, infoEase);

	DrawPill(94.0f,  204.0f, 64.0f, 19.0f, sizeStr, pillFill, pillBorder, pillText, 0.80f);
	DrawPill(166.0f, 204.0f, 68.0f, 19.0f, rom->country[0] ? rom->country : "N64", pillFill, pillBorder, pillText, 0.80f);
	DrawPill(242.0f, 204.0f, 64.0f, 19.0f, "60 FPS",
	         ScaleAlpha(t.isLight ? RGBA(220, 248, 232, 235) : WithAlpha(LerpColor(t.cardBg, t.n64Green, 0.18f), 225), infoEase),
	         ScaleAlpha(WithAlpha(t.n64Green, 160), infoEase),
	         ScaleAlpha(t.isLight ? RGBA(20, 128, 66) : LerpColor(t.n64Green, RGBA(255, 255, 255), 0.35f), infoEase), 0.80f);
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

} // namespace

namespace FrontendCTR
{

bool Initialise()
{
	if (s_initialised) return true;
	LoadFrontendConfig();
	TouchController::Init();
	if (!EnsureMenuPool())
		return false;
	TickSystemStatus(true);
	s_initialised = true;
	return true;
}

void Shutdown()
{
	FrontendAudio::End();
	CoverScraper::Stop();
	ReleasePreview();
	UI::Renderer::Shutdown();
	s_romCount = 0;
	s_viewCount = 0;
	s_romsScanned = false;
	s_initialised = false;
}

void ReleaseAssets()
{
	// Release preview texture, frontend DSP audio, and menu vertex pool before emulation so
	// Old 3DS retains maximum Linear RAM and 0 DSP/CPU overhead for N64 gameplay.
	FrontendAudio::End();
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

// ============================================================================
// Unified Nintendo 3DS Virtual Console L/R Tabbed Menu (Pause & Settings)
// Replaces CTR64's 3x3 Pause Grid and 21-item flat list with 5 L/R Tabs:
//   [L]  States  |  CPU  |  Graphics  |  Audio  |  Touch  [R]
// plus the left vertical quick-action rail and 5 visual Controller Pak slots.
// ============================================================================
static void RunUnifiedMenu(const char * gameName, const RomID & romId, bool inGame,
                           const CRefPtr<CNativeTexture> & freezeSnap)
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
	TouchPadConfig touchCfg = TouchController::Engine().GetConfig();

	// Category mapping into kOptions[] (tabs 1..3)
	static const int kTabCpuOpts[]  = { 0, 1, 3, 4, 2, 20 };             // 6 items
	static const int kTabGfxOpts[]  = { 9, 10, 13, 14, 7, 8, 6, 12 };    // 8 items
	static const int kTabAudOpts[]  = { 15, 16, 5, 18, 19, 17 };         // 6 items
	static const int kOptionImpact[kNumOptions] = {
		96, 84, 65, 88, 78, 40, 35, 30, 30, 88, 72, 35, 30, 20, 15, 85, 30, 10, 15, 10, 75
	};
	static const char * const kTouchLabels[6] = {
		"Bottom Screen Mode",
		"Handedness",
		"Z-Trigger Mode",
		"Stick Deadzone",
		"Shooter Sensitivity",
		"Invert Camera Y"
	};

	enum EMenuTab { TAB_STATES = 0, TAB_CPU, TAB_GFX, TAB_AUDIO, TAB_TOUCH, TAB_COUNT };
	static const char * const kTabTitles[TAB_COUNT] = { "States", "CPU", "Graphics", "Audio", "Touch" };

	int   activeTab    = inGame ? TAB_STATES : TAB_CPU;
	int   rowSel       = inGame ? TouchController::GetActiveSlot() : 0;
	int   slotBtnCol   = 0; // 0 = Save button, 1 = Load button on the active Save State card
	float hlY          = 0.0f, hlVY = 0.0f;
	float scrollPos    = 0.0f, scrollVel = 0.0f;
	float scrollTarget = 0.0f;
	float enterT       = 0.0f;
	char  toast[64]    = "";
	int   toastFrames  = 0;

	bool slotExists[5] = {};
	auto RefreshSlots = [&]() {
		for (int i = 0; i < 5; ++i)
		{
			IO::Filename check;
			snprintf(check, sizeof(check), "%s%s.ss%d",
			         DAEDALUS_CTR_PATH("SaveStates/"), (gameName && gameName[0]) ? gameName : "rom", i);
			slotExists[i] = IO::File::Exists(check);
		}
	};
	RefreshSlots();

	auto CommitAllPrefs = [&]() {
		if (CPreferences::Get())
		{
			CPreferences::Get()->SetRomPreferences(romId, prefs);
			gGlobalPreferences.DisplayFramerate = pending_fps;
			gGlobalPreferences.ViewportType = pending_viewport;
			CPreferences::Get()->Commit();
		}
		SaveFrontendConfig();
		TouchController::Engine().SetConfig(touchCfg);
		TouchController::SaveConfig();
		if (inGame)
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
	};

	auto GetTabItemCount = [&](int tab) -> int {
		switch (tab)
		{
		case TAB_STATES: return 5;
		case TAB_CPU:    return (int)(sizeof(kTabCpuOpts) / sizeof(kTabCpuOpts[0]));
		case TAB_GFX:    return (int)(sizeof(kTabGfxOpts) / sizeof(kTabGfxOpts[0]));
		case TAB_AUDIO:  return (int)(sizeof(kTabAudOpts) / sizeof(kTabAudOpts[0]));
		case TAB_TOUCH:  return 6;
		default:         return 5;
		}
	};

	auto GetMappedOptIdx = [&](int tab, int row) -> int {
		if (tab == TAB_CPU)   return kTabCpuOpts[row];
		if (tab == TAB_GFX)   return kTabGfxOpts[row];
		if (tab == TAB_AUDIO) return kTabAudOpts[row];
		return 0;
	};

	RepeatTimer rep;
	rep.Reset();
	TouchTracker touch;
	touch.Reset();

	if (inGame)
	{
		while (aptMainLoop() && (hidKeysHeld() & KEY_SELECT))
		{
			gspWaitForVBlank();
			hidScanInput();
		}
	}

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
			enterT += 1.0f / 12.0f;
			if (enterT > 1.0f) enterT = 1.0f;
		}
		const float ease = EaseOutCubic(enterT);
		const int itemCount = GetTabItemCount(activeTab);

		// Execute Save or Load on slotIdx (0..4)
		auto TriggerSlotAction = [&](int slotIdx, bool doLoad) {
			TouchController::SetActiveSlot(slotIdx);
			if (!inGame)
			{
				FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
				snprintf(toast, sizeof(toast), "Default Slot %d selected", slotIdx + 1);
				toastFrames = 65;
				return;
			}
			char path[256];
			snprintf(path, sizeof(path), "%s%s.ss%d",
			         DAEDALUS_CTR_PATH("SaveStates/"), (gameName && gameName[0]) ? gameName : "rom", slotIdx);
			if (!doLoad)
			{
				FrontendAudio::PlaySfx(FrontendAudio::SFX_SELECT);
				CommitAllPrefs();
				CPU_RequestSaveState(path);
				exitMenu = true;
			}
			else if (slotExists[slotIdx])
			{
				FrontendAudio::PlaySfx(FrontendAudio::SFX_SELECT);
				CommitAllPrefs();
				CPU_RequestLoadState(path);
				exitMenu = true;
			}
			else
			{
				FrontendAudio::PlaySfx(FrontendAudio::SFX_ERROR);
				snprintf(toast, sizeof(toast), "Slot %d is empty", slotIdx + 1);
				toastFrames = 75;
			}
		};

		// Touch hit-testing for Top Tab Bar, Left Action Rail, and Right Stage
		int pressedRail = -1;
		if (touch.active && !touch.dragged && touch.curX >= 6 && touch.curX < 50 && touch.curY >= 34 && touch.curY < 234)
		{
			pressedRail = (touch.curY - 34) / 40;
			if (pressedRail < 0 || pressedRail > 4) pressedRail = -1;
		}

		// Touch drag / scrollbar interaction for Settings tabs
		if (touch.active && activeTab != TAB_STATES && itemCount > 5 && touch.curX >= 56 && touch.curY >= 36 && touch.curY < 232)
		{
			const float maxSc = (float)(itemCount - 5);
			if (touch.curX >= 304)
			{
				// Direct tap/drag on right scrollbar track
				const float tNorm = fminf(1.0f, fmaxf(0.0f, ((float)touch.curY - 46.0f) / 172.0f));
				scrollTarget = tNorm * maxSc;
				scrollPos    = scrollTarget;
			}
			else if (touch.dragged && touch.deltaY != 0)
			{
				scrollTarget = fminf(maxSc, fmaxf(0.0f, scrollTarget - (float)touch.deltaY / 36.0f));
				scrollPos    = scrollTarget;
			}
		}

		if (touch.justReleased && !touch.dragged)
		{
			const int tx = touch.releaseX;
			const int ty = touch.releaseY;

			// 1) Top L/R Tab Bar (y = 0..31)
			if (ty < 31)
			{
				if (tx < 26)
					down |= KEY_L;
				else if (tx >= 294)
					down |= KEY_R;
				else
				{
					const int clickedTab = ((tx - 26) * TAB_COUNT) / 268;
					if (clickedTab >= 0 && clickedTab < TAB_COUNT && clickedTab != activeTab)
					{
						activeTab    = clickedTab;
						rowSel       = (activeTab == TAB_STATES) ? TouchController::GetActiveSlot() : 0;
						scrollPos    = 0.0f;
						scrollTarget = 0.0f;
						hlY          = (float)rowSel;
						FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
					}
				}
			}
			// 2) Left Vertical Action Rail (x = 6..50, y = 34..234)
			else if (tx >= 6 && tx < 50 && ty >= 34 && ty < 234)
			{
				const int railIdx = (ty - 34) / 40;
				switch (railIdx)
				{
				case 0: // Save States tab
					activeTab    = TAB_STATES;
					rowSel       = TouchController::GetActiveSlot();
					scrollPos    = 0.0f;
					scrollTarget = 0.0f;
					FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
					break;
				case 1: // Stop / Return to Game List (in-game) or Toggle Theme (launcher)
					if (inGame)
					{
						FrontendAudio::PlaySfx(FrontendAudio::SFX_SELECT);
						CommitAllPrefs();
						s_resetRequested = false;
						CPU_Halt("Closed from menu");
						exitMenu = true;
					}
					else
					{
						down |= KEY_Y;
					}
					break;
				case 2: // Resume Game / Save & Close
					FrontendAudio::PlaySfx(FrontendAudio::SFX_SELECT);
					CommitAllPrefs();
					exitMenu = true;
					break;
				case 3: // Reset ROM (in-game) or CPU Settings tab
					if (inGame)
					{
						FrontendAudio::PlaySfx(FrontendAudio::SFX_SELECT);
						CommitAllPrefs();
						s_resetRequested = true;
						CPU_Halt("Reset from menu");
						exitMenu = true;
					}
					else
					{
						activeTab    = TAB_CPU;
						rowSel       = 0;
						scrollPos    = 0.0f;
						scrollTarget = 0.0f;
						FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
					}
					break;
				case 4: // Back / Exit
					down |= KEY_B;
					break;
				}
			}
			// 3) Right Stage Cards (x >= 56, y = 35..234)
			else if (tx >= 56 && tx < 304 && ty >= 35 && ty < 232)
			{
				if (activeTab == TAB_STATES)
				{
					const int sIdx = (ty - 36) / 39;
					if (sIdx >= 0 && sIdx < 5)
					{
						rowSel = sIdx;
						TouchController::SetActiveSlot(sIdx);
						if (tx >= 242)
						{
							slotBtnCol = 1;
							TriggerSlotAction(sIdx, true);  // Direct tap on [Load]
						}
						else if (tx >= 176)
						{
							slotBtnCol = 0;
							TriggerSlotAction(sIdx, false); // Direct tap on [Save]
						}
						else
						{
							FrontendAudio::PlaySfx(FrontendAudio::SFX_MOVE);
						}
					}
				}
				else
				{
					constexpr float kRowStride = 36.0f;
					const int idx = (int)floorf(scrollPos + ((float)ty - 36.0f) / kRowStride);
					if (idx >= 0 && idx < itemCount)
					{
						rowSel = idx;
						down |= KEY_A;
					}
				}
			}
		}

		if (down & (KEY_B | KEY_SELECT | KEY_START))
		{
			FrontendAudio::PlaySfx((down & KEY_START) ? FrontendAudio::SFX_SELECT : FrontendAudio::SFX_BACK);
			CommitAllPrefs();
			exitMenu = true;
			break;
		}
		if (down & KEY_Y)
		{
			CycleTheme(1);
			SaveFrontendConfig();
			FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
			snprintf(toast, sizeof(toast), "Theme: %s", ActiveTheme().name);
			toastFrames = 65;
		}
		if (down & (KEY_L | KEY_R))
		{
			const int step = (down & KEY_R) ? 1 : (TAB_COUNT - 1);
			activeTab    = (activeTab + step) % TAB_COUNT;
			rowSel       = (activeTab == TAB_STATES) ? TouchController::GetActiveSlot() : 0;
			scrollPos    = 0.0f;
			scrollTarget = 0.0f;
			hlY          = (float)rowSel;
			FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
		}

		const int prevRow = rowSel;
		if (rep.Poll(down, held, KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN, 0))
			rowSel = (rowSel + 1) % itemCount;
		if (rep.Poll(down, held, KEY_UP | KEY_DUP | KEY_CPAD_UP, 1))
			rowSel = (rowSel + itemCount - 1) % itemCount;
		if (rowSel != prevRow)
		{
			FrontendAudio::PlaySfx(FrontendAudio::SFX_MOVE);
			if (activeTab == TAB_STATES)
				TouchController::SetActiveSlot(rowSel);
			// Update persistent scrollTarget whenever selection moves outside visible 5-row window
			constexpr int kVisRows = 5;
			if ((float)rowSel < scrollTarget)
				scrollTarget = (float)rowSel;
			else if ((float)rowSel >= scrollTarget + kVisRows)
				scrollTarget = (float)(rowSel - kVisRows + 1);
		}

		if (activeTab == TAB_STATES)
		{
			if (down & (KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT | KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT))
			{
				slotBtnCol ^= 1;
				FrontendAudio::PlaySfx(FrontendAudio::SFX_MOVE);
			}
			if (down & KEY_A)
				TriggerSlotAction(rowSel, slotBtnCol == 1);
			if (down & KEY_X)
				TriggerSlotAction(rowSel, true);
		}
		else if (activeTab == TAB_TOUCH)
		{
			const int dir = (down & (KEY_A | KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT)) ? 1 :
			                (down & (KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT)) ? -1 : 0;
			if (dir != 0)
			{
				FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
				switch (rowSel)
				{
				case 0: touchCfg.layout = (ETouchPadLayout)(((int)touchCfg.layout + dir + TOUCH_LAYOUT_COUNT) % TOUCH_LAYOUT_COUNT); break;
				case 1: touchCfg.leftHanded = !touchCfg.leftHanded; break;
				case 2: touchCfg.zToggleMode = !touchCfg.zToggleMode; break;
				case 3:
					touchCfg.stickDeadzone = fminf(0.40f, fmaxf(0.15f, touchCfg.stickDeadzone + dir * 0.05f));
					break;
				case 4:
					touchCfg.stickSens = fminf(1.60f, fmaxf(0.60f, touchCfg.stickSens + dir * 0.10f));
					break;
				case 5: touchCfg.invertY = !touchCfg.invertY; break;
				}
				TouchController::Engine().SetConfig(touchCfg);
				TouchController::SaveConfig();
			}
		}
		else
		{
			const int dir = (down & (KEY_A | KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT)) ? 1 :
			                (down & (KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT)) ? -1 : 0;
			if (dir != 0)
			{
				FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
				const int optIdx = GetMappedOptIdx(activeTab, rowSel);
				switch (optIdx)
				{
				case 0:  prefs.DynarecEnabled = !prefs.DynarecEnabled; break;
				case 1:  prefs.DynarecLoopOptimisation = !prefs.DynarecLoopOptimisation; break;
				case 2:  prefs.DynarecDoublesOptimisation = !prefs.DynarecDoublesOptimisation; break;
				case 3:  prefs.MemoryAccessOptimisation = !prefs.MemoryAccessOptimisation; break;
				case 4:  prefs.PatchesEnabled = !prefs.PatchesEnabled; break;
				case 5:  prefs.SpeedSyncEnabled = prefs.SpeedSyncEnabled ? 0u : 1u; break;
				case 6:  prefs.DoubleDisplayEnabled = !prefs.DoubleDisplayEnabled; break;
				case 7:  prefs.CleanSceneEnabled = !prefs.CleanSceneEnabled; break;
				case 8:  prefs.ClearDepthFrameBuffer = !prefs.ClearDepthFrameBuffer; break;
				case 9:  prefs.Frameskip = (EFrameskipValue)((prefs.Frameskip + dir + NUM_FRAMESKIP_VALUES) % NUM_FRAMESKIP_VALUES); break;
				case 10: prefs.CheckTextureHashFrequency = (ETextureHashFrequency)((prefs.CheckTextureHashFrequency + dir + NUM_THF) % NUM_THF); break;
				case 11: prefs.VideoRateMatch = !prefs.VideoRateMatch; break;
				case 12: prefs.FogEnabled = !prefs.FogEnabled; break;
				case 13: pending_viewport = (EViewportType)((pending_viewport + dir + 4) % 4); break;
				case 14:
					prefs.ZoomX = fminf(2.0f, fmaxf(0.5f, prefs.ZoomX + dir * 0.1f));
					break;
				case 15: prefs.AudioEnabled = (EAudioPluginMode)((prefs.AudioEnabled + dir + 3) % 3); break;
				case 16: prefs.AudioRateMatch = !prefs.AudioRateMatch; break;
				case 17: prefs.CheatsEnabled = !prefs.CheatsEnabled; break;
				case 18: pending_fps = !pending_fps; break;
				case 19: CycleTheme(dir); break;
				case 20:
				{
					int cur = (prefs.DynarecCycleMultiplier >= 3) ? 3 : ((prefs.DynarecCycleMultiplier == 2) ? 2 : 1);
					cur = ((cur - 1 + dir + 3) % 3) + 1;
					prefs.DynarecCycleMultiplier = (u32)cur;
					break;
				}
				}
			}
		}

		// Keep selected row visible in 5-row viewport using persistent scrollTarget
		// (Never re-seed scrollTarget from scrollPos per-frame, so spring animation completes all the way to row 5/Cheats!)
		constexpr int kVisRows = 5;
		const float maxScroll = (itemCount > kVisRows) ? (float)(itemCount - kVisRows) : 0.0f;
		if (scrollTarget < 0.0f) scrollTarget = 0.0f;
		if (scrollTarget > maxScroll) scrollTarget = maxScroll;
		SpringStep(scrollPos, scrollVel, scrollTarget, 260.0f, 26.0f);
		SpringStep(hlY, hlVY, (float)rowSel, 320.0f, 26.0f);

		// Theme-driven palette for the Suspend & System Settings Screen (supports Dark, Light, Midnight, Stealth)
		const ThemePalette & t    = ActiveTheme();
		const uint32_t vcBg       = t.bg;
		const uint32_t vcHeader   = t.headerBg;
		const uint32_t vcRail     = t.bgStripe;
		const uint32_t vcTabIdle  = t.cardBg;
		const uint32_t vcTabAct   = t.focusFill;
		const uint32_t vcBorder   = t.cardBorder;
		const uint32_t vcSlotCard = t.cardBg;
		const uint32_t vcSlotSel  = t.focusFill;
		const uint32_t vcSlotTxt  = t.text;
		const uint32_t vcCyan     = t.focusBorder;

		BeginFrame();

		// ====================================================================
		// BOTTOM SCREEN (320x240): L/R Tab Bar + Left Rail + 5 Slot Cards / Options
		// ====================================================================
		BeginScreen(GFX_BOTTOM, 320, 240, vcBg);
		Quad(0.0f, 0.0f, 320.0f, 240.0f, vcBg);
		DrawBackdrop(320.0f, 240.0f, t);
		Quad(0.0f, 0.0f, 320.0f, 31.0f, vcHeader);
		Quad(0.0f, 31.0f, 320.0f, 1.0f, t.headerRule);

		// Left [L] and Right [R] shoulder badges (themed)
		RoundedRect(4.0f, 6.0f, 20.0f, 20.0f, 5.0f, t.cardBgAlt);
		RoundedBorder1(4.0f, 6.0f, 20.0f, 20.0f, 5.0f, vcBorder);
		TextCentered(FONT_BODY, 14.0f, 9.5f, "L", t.text, 0.80f);
		RoundedRect(296.0f, 6.0f, 20.0f, 20.0f, 5.0f, t.cardBgAlt);
		RoundedBorder1(296.0f, 6.0f, 20.0f, 20.0f, 5.0f, vcBorder);
		TextCentered(FONT_BODY, 306.0f, 9.5f, "R", t.text, 0.80f);

		// 5 Folder Tabs across x = 27..293
		for (int tb = 0; tb < TAB_COUNT; ++tb)
		{
			const float tx = 27.0f + tb * 53.2f;
			const float tw = 51.2f;
			const bool isAct = (tb == activeTab);
			RoundedRect(tx, isAct ? 3.0f : 6.0f, tw, isAct ? 28.0f : 24.0f, 6.0f, isAct ? vcTabAct : vcTabIdle);
			RoundedBorder1(tx, isAct ? 3.0f : 6.0f, tw, isAct ? 28.0f : 24.0f, 6.0f, isAct ? vcCyan : vcBorder);
			TextCentered(FONT_BODY, tx + tw * 0.5f, isAct ? 10.5f : 11.5f, kTabTitles[tb],
			             isAct ? t.text : t.textMuted, 0.78f);
		}

		// Left Vertical Action Rail (x = 0..52, y = 32..240)
		Quad(0.0f, 32.0f, 52.0f, 208.0f, vcRail);
		Quad(52.0f, 32.0f, 1.0f, 208.0f, t.headerRule);

		const EIconId railIcons[5] = {
			ICON_SAVE,
			inGame ? ICON_GRID : ICON_MOON,
			ICON_PLAY,
			inGame ? ICON_RESET : ICON_GEAR,
			ICON_POWER
		};
		for (int ri = 0; ri < 5; ++ri)
		{
			const float ry = 38.0f + ri * 40.0f;
			const bool railSel = (ri == 0 && activeTab == TAB_STATES) ||
			                     (ri == 3 && !inGame && activeTab != TAB_STATES);
			const bool railPrs = (ri == pressedRail);
			RoundedRect(7.0f, ry, 38.0f, 32.0f, 12.0f,
			            railPrs ? t.focusFill : (railSel ? t.cardBgAlt : t.cardBg));
			RoundedBorder1(7.0f, ry, 38.0f, 32.0f, 12.0f,
			               railSel ? vcCyan : vcBorder);
			Icon(railIcons[ri], 17.0f, ry + 7.0f, 18.0f,
			     railSel ? t.accent : t.iconMuted);
		}

		// Right Stage (x = 58..314, y = 36..234)
		if (activeTab == TAB_STATES)
		{
			// 5 Tactile Save-State Cards (fully theme-aware — sleek dark in dark themes, crisp white in light)
			for (int s = 0; s < 5; ++s)
			{
				const float sy = 36.0f + s * 39.0f;
				const bool isSel = (s == rowSel);
				const bool saved = slotExists[s];

				if ((t.cardShadow >> 24) > 0)
					RoundedRect(60.0f, sy + 2.0f, 252.0f, 34.0f, 9.0f, t.cardShadow);
				RoundedRect(60.0f, sy, 252.0f, 34.0f, 9.0f, isSel ? vcSlotSel : vcSlotCard);
				if (isSel)
					RoundedBorder2(60.0f, sy, 252.0f, 34.0f, 9.0f, vcCyan);
				else
					RoundedBorder1(60.0f, sy, 252.0f, 34.0f, 9.0f, vcBorder);

				char slotLbl[16];
				snprintf(slotLbl, sizeof(slotLbl), "Slot %d", s + 1);
				Text(FONT_TITLE, 72.0f, sy + 9.0f, slotLbl, vcSlotTxt, 0.96f);

				// Miniature N64 Controller Pak badge
				RoundedRect(138.0f, sy + 6.0f, 22.0f, 22.0f, 5.0f,
				            saved ? t.n64Green : t.badgeBg);
				Icon(ICON_SAVE, 141.0f, sy + 9.0f, 16.0f, saved ? RGBA(255, 255, 255) : t.iconMuted);

				// Direct [Save] Pill Button (x = 174..236)
				const bool focSave = isSel && (slotBtnCol == 0);
				DrawPill(174.0f, sy + 6.0f, 62.0f, 22.0f, "Save",
				         focSave ? t.n64Blue : t.cardBgAlt,
				         focSave ? vcCyan : vcBorder,
				         focSave ? RGBA(255, 255, 255) : t.text, 0.84f);

				// Direct [Load] Pill Button (x = 242..304)
				const bool focLoad = isSel && (slotBtnCol == 1);
				DrawPill(242.0f, sy + 6.0f, 62.0f, 22.0f, "Load",
				         focLoad ? t.n64Green : (saved ? t.cardBgAlt : t.bg),
				         focLoad ? vcCyan : vcBorder,
				         focLoad ? RGBA(255, 255, 255) : (saved ? t.text : t.textDim), 0.84f);
			}
		}
		else
		{
			// Categorized Settings Rows (max 5 visible per tab, smoothly scrollable to reveal row 6/Cheats & Graphics options)
			constexpr float kListY     = 36.0f;
			constexpr float kRowStride = 36.0f;
			constexpr float kRowH      = 31.0f;
			// Note: SetScissorLandscape expects portrait Y from bottom (240 - 236 = 4 .. 206 -> landscape Y in [34..236])
			SetClipRect(true, 54, 4, 266, 202, 320, 240);

			for (int r = 0; r < itemCount; ++r)
			{
				const float ry = kListY + ((float)r - scrollPos) * kRowStride;
				if (ry + kRowH < 33.0f || ry > 236.0f)
					continue;
				const bool isSel = (r == rowSel);
				RoundedRect(60.0f, ry, 244.0f, kRowH, 8.0f, isSel ? t.focusFill : vcTabIdle);
				RoundedBorder1(60.0f, ry, 244.0f, kRowH, 8.0f, isSel ? vcCyan : vcBorder);

				const char * label = "";
				char valStr[40] = "";
				bool isBool = false;
				bool isOn   = false;

				if (activeTab == TAB_TOUCH)
				{
					label = kTouchLabels[r];
					switch (r)
					{
					case 0: snprintf(valStr, sizeof(valStr), "%s", TouchPadEngine::LayoutShortName(touchCfg.layout)); break;
					case 1: snprintf(valStr, sizeof(valStr), "%s", touchCfg.leftHanded ? "Left" : "Right"); break;
					case 2: snprintf(valStr, sizeof(valStr), "%s", touchCfg.zToggleMode ? "Toggle" : "Hold"); break;
					case 3: snprintf(valStr, sizeof(valStr), "%d%%", (int)(touchCfg.stickDeadzone * 100.0f + 0.5f)); break;
					case 4: snprintf(valStr, sizeof(valStr), "%.1fx", touchCfg.stickSens); break;
					case 5: isBool = true; isOn = touchCfg.invertY; break;
					}
				}
				else
				{
					const int optIdx = GetMappedOptIdx(activeTab, r);
					label = kOptions[optIdx].label;
					switch (optIdx)
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
					case 9:  snprintf(valStr, sizeof(valStr), "%s", FrameskipName(prefs.Frameskip)); break;
					case 10: snprintf(valStr, sizeof(valStr), "%s", TexHashName(prefs.CheckTextureHashFrequency)); break;
					case 11: isBool = true; isOn = prefs.VideoRateMatch; break;
					case 12: isBool = true; isOn = prefs.FogEnabled; break;
					case 13: snprintf(valStr, sizeof(valStr), "%s", ViewportName(pending_viewport)); break;
					case 14: snprintf(valStr, sizeof(valStr), "%.1fx", prefs.ZoomX); break;
					case 15: snprintf(valStr, sizeof(valStr), "%s", AudioName(prefs.AudioEnabled)); break;
					case 16: isBool = true; isOn = prefs.AudioRateMatch; break;
					case 17: isBool = true; isOn = prefs.CheatsEnabled; break;
					case 18: isBool = true; isOn = pending_fps; break;
					case 19: snprintf(valStr, sizeof(valStr), "%s", t.name); break;
					case 20: snprintf(valStr, sizeof(valStr),
						prefs.DynarecCycleMultiplier >= 3 ? "3x Turbo" :
						(prefs.DynarecCycleMultiplier == 2 ? "2x Fast" : "1x Normal")); break;
					}
				}

				Text(FONT_BODY, 70.0f, ry + 8.0f, label,
				     isSel ? t.text : (t.isLight ? t.text : t.textMuted), 0.92f);

				if (isBool)
				{
					DrawPill(248.0f, ry + 5.5f, 48.0f, 20.0f, isOn ? "ON" : "OFF",
					         isOn ? t.n64Green : t.cardBgAlt,
					         isOn ? WithAlpha(LerpColor(t.n64Green, RGBA(255, 255, 255), 0.35f), 200) : vcBorder,
					         isOn ? RGBA(255, 255, 255) : t.textMuted, 0.82f);
				}
				else
				{
					const float pw = fmaxf(56.0f, MeasureText(FONT_BODY, valStr, 0.82f) + 16.0f);
					DrawPill(296.0f - pw, ry + 5.5f, pw, 20.0f, valStr,
					         isSel ? t.n64Blue : t.cardBgAlt,
					         isSel ? vcCyan : vcBorder,
					         isSel ? RGBA(255, 255, 255) : t.text, 0.82f);
				}
			}

			const float hy = kListY + (hlY - scrollPos) * kRowStride;
			if (hy + kRowH >= 34.0f && hy <= 235.0f)
				RoundedBorder2(60.0f, hy, 244.0f, kRowH, 8.0f, vcCyan);
			SetClipRect(false, 0, 0, 320, 240, 320, 240);

			// Sleek Vertical Scrollbar Track & Translucent Thumb when tab has > 5 items
			if (itemCount > kVisRows)
			{
				const float trkX = 311.0f, trkY = 38.0f, trkW = 3.0f, trkH = 176.0f;
				RoundedRect(trkX, trkY, trkW, trkH, 1.5f, WithAlpha(vcBorder, 140));
				const float thumbH = fmaxf(26.0f, trkH * ((float)kVisRows / (float)itemCount));
				const float thumbY = trkY + (trkH - thumbH) * fminf(1.0f, fmaxf(0.0f, scrollPos / maxScroll));
				RoundedRect(trkX, thumbY, trkW, thumbH, 1.5f, WithAlpha(vcCyan, 215));
			}
		}

		if (toastFrames > 0)
		{
			RoundedRect(68.0f, 208.0f, 236.0f, 24.0f, 10.0f, WithAlpha(t.hintBg, 240));
			RoundedBorder1(68.0f, 208.0f, 236.0f, 24.0f, 10.0f, WithAlpha(vcCyan, 190));
			TextCentered(FONT_BODY, 186.0f, 213.0f, toast, t.hintText, 0.84f);
			--toastFrames;
		}

		// ====================================================================
		// TOP SCREEN (400x240): Frosted Gameplay / Settings Inspector
		// ====================================================================
		BeginScreen(GFX_TOP, 400, 240, vcBg);
		DrawFrostedGameBackdrop(freezeSnap, ease, t);
		if (freezeSnap == nullptr || !freezeSnap->HasData())
			QuadVGrad(0.0f, 0.0f, 400.0f, 240.0f, t.headerBg, t.bg);

		DrawTopRightStatusPill(t);

		const uint32_t cardFill   = WithAlpha(t.cardBg, t.isLight ? 245 : 238);
		const uint32_t cardBorder = t.cardBorder;
		const uint32_t cardShadow = t.cardShadow;

		if (activeTab == TAB_STATES)
		{
			const float banY = 122.0f + (1.0f - ease) * 18.0f;
			if ((cardShadow >> 24) > 0)
				RoundedRect(18.0f, banY + 2.0f, 364.0f, 98.0f, 12.0f, cardShadow);
			RoundedRect(18.0f, banY, 364.0f, 98.0f, 12.0f, cardFill);
			RoundedBorder1(18.0f, banY, 364.0f, 98.0f, 12.0f, cardBorder);

			RoundedRect(30.0f, banY + 17.0f, 64.0f, 64.0f, 10.0f,
			            slotExists[rowSel] ? t.n64Blue : t.badgeBg);
			Icon(ICON_SAVE, 48.0f, banY + 27.0f, 28.0f, slotExists[rowSel] ? RGBA(255, 255, 255) : t.iconMuted);
			TextCentered(FONT_BODY, 62.0f, banY + 59.0f, slotExists[rowSel] ? "SAVED" : "EMPTY",
			             slotExists[rowSel] ? t.n64Yellow : t.textMuted, 0.76f);

			TextEllipsis(FONT_TITLE, 108.0f, banY + 17.0f, 260.0f,
			             (gameName && gameName[0]) ? gameName : "Nintendo 64", t.text, 1.04f);
			char slotInfo[96];
			snprintf(slotInfo, sizeof(slotInfo), "Memory Pak Slot %d   |   %.1f FPS   |   %s",
			         rowSel + 1, gCurrentFramerate,
			         TouchPadEngine::LayoutShortName(touchCfg.layout));
			Text(FONT_BODY, 108.0f, banY + 43.0f, slotInfo, t.accent, 0.86f);
			Text(FONT_BODY, 108.0f, banY + 63.0f, "Tap Save or Load on any slot card below, or press (B) to resume.",
			     t.textMuted, 0.80f);
		}
		else if (activeTab == TAB_TOUCH)
		{
			const float cardY = 40.0f;
			if ((cardShadow >> 24) > 0)
				RoundedRect(20.0f, cardY + 2.0f, 360.0f, 182.0f, 12.0f, cardShadow);
			RoundedRect(20.0f, cardY, 360.0f, 182.0f, 12.0f, cardFill);
			RoundedBorder1(20.0f, cardY, 360.0f, 182.0f, 12.0f, cardBorder);
			RoundedRect(20.0f, cardY + 12.0f, 3.0f, 20.0f, 1.5f, t.accent);
			Text(FONT_TITLE, 34.0f, cardY + 14.0f, TouchPadEngine::LayoutName(touchCfg.layout), t.text, 1.0f);
			TextRight(FONT_BODY, 364.0f, cardY + 16.0f, touchCfg.leftHanded ? "Left-Handed" : "Right-Handed", t.accent, 0.86f);
			Quad(34.0f, cardY + 39.0f, 330.0f, 1.0f, WithAlpha(vcBorder, 160));

			const float pvX = 36.0f, pvY = cardY + 49.0f, pvW = 136.0f, pvH = 102.0f;
			RoundedRect(pvX, pvY, pvW, pvH, 8.0f, t.bg);
			RoundedBorder1(pvX, pvY, pvW, pvH, 8.0f, vcBorder);
			if (touchCfg.layout != TOUCH_LAYOUT_SLEEP)
			{
				const float zPx   = touchCfg.leftHanded ? (pvX + pvW - 44.0f) : (pvX + 8.0f);
				const float padPx = touchCfg.leftHanded ? (pvX + 44.0f)       : (pvX + pvW - 44.0f);
				const float padPy = pvY + 54.0f;
				RoundedRect(zPx, pvY + 18.0f, 36.0f, 32.0f, 5.0f, vcTabAct);
				TextCentered(FONT_BODY, zPx + 18.0f, pvY + 27.0f, "Z", vcCyan, 0.86f);
				Circle(padPx, padPy, 30.0f, t.cardBgAlt);
				if (touchCfg.layout == TOUCH_LAYOUT_CCROSS)
				{
					Circle(padPx, padPy - 17.0f, 8.5f, t.n64Yellow);
					Circle(padPx, padPy + 17.0f, 8.5f, t.n64Yellow);
					Circle(padPx - 17.0f, padPy, 8.5f, t.n64Yellow);
					Circle(padPx + 17.0f, padPy, 8.5f, t.n64Yellow);
				}
				else
				{
					Ring(padPx, padPy, 22.0f, vcCyan);
					Circle(padPx, padPy, 10.0f, vcCyan);
				}
			}
			const char * desc =
				(touchCfg.layout == TOUCH_LAYOUT_CCROSS)       ? "Minimalist 4-way Yellow N64 C-Camera buttons with dedicated Z trigger, Z-Lock slider, and Memory Pak Quick Save/Load." :
				(touchCfg.layout == TOUCH_LAYOUT_CSTICK)       ? "Virtual analog C-Stick for 360-degree camera panning." :
				(touchCfg.layout == TOUCH_LAYOUT_DUAL_SHOOTER) ? "Twin-stick FPS mode: Touch aims N64 stick while Circle Pad strafes." :
				(touchCfg.layout == TOUCH_LAYOUT_DPAD)         ? "Full N64 D-Pad Cross on touch screen." :
				                                                 "Turns bottom screen off during gameplay to save battery.";
			TextWrapped(FONT_BODY, 186.0f, cardY + 53.0f, 178.0f, 16.0f, desc, t.textMuted, 0.88f);
		}
		else
		{
			const int optIdx = GetMappedOptIdx(activeTab, rowSel);
			const float cardY = 44.0f;
			const bool isLive = kOptions[optIdx].live;
			const uint32_t badgeCol = isLive ? t.n64Green : t.n64Yellow;

			if ((cardShadow >> 24) > 0)
				RoundedRect(22.0f, cardY + 2.0f, 356.0f, 152.0f, 12.0f, cardShadow);
			RoundedRect(22.0f, cardY, 356.0f, 152.0f, 12.0f, cardFill);
			RoundedBorder1(22.0f, cardY, 356.0f, 152.0f, 12.0f, cardBorder);
			// Crisp left accent bar matching option apply mode
			RoundedRect(22.0f, cardY + 13.0f, 3.0f, 22.0f, 1.5f, badgeCol);

			Text(FONT_TITLE, 36.0f, cardY + 16.0f, kOptions[optIdx].label, t.text, 1.06f);
			DrawPill(306.0f, cardY + 15.0f, 56.0f, 19.0f, isLive ? "LIVE" : "BOOT",
			         WithAlpha(badgeCol, t.isLight ? 38 : 42),
			         WithAlpha(badgeCol, 190),
			         t.isLight ? LerpColor(badgeCol, RGBA(0, 0, 0), 0.35f) : badgeCol, 0.78f);

			Quad(36.0f, cardY + 44.0f, 326.0f, 1.0f, WithAlpha(vcBorder, 160));
			TextWrapped(FONT_BODY, 36.0f, cardY + 56.0f, 326.0f, 17.5f, kOptions[optIdx].help, t.textMuted, 0.93f);

			// Old 3DS Performance Impact Bar
			const int impact = kOptionImpact[optIdx];
			const uint32_t barCol = (impact >= 60) ? t.n64Green :
			                        (impact >= 25) ? t.accent : t.textMuted;
			Text(FONT_BODY, 36.0f, cardY + 118.0f, "Old 3DS Speed Impact", t.textMuted, 0.82f);
			RoundedRect(172.0f, cardY + 121.0f, 152.0f, 8.0f, 4.0f, t.bg);
			RoundedBorder1(172.0f, cardY + 121.0f, 152.0f, 8.0f, 4.0f, WithAlpha(vcBorder, 160));
			const float barW = fmaxf(8.0f, 152.0f * (impact / 100.0f));
			RoundedRect(172.0f, cardY + 121.0f, barW, 8.0f, 4.0f, barCol);
			char impPct[12];
			snprintf(impPct, sizeof(impPct), "%d%%", impact);
			TextRight(FONT_BODY, 362.0f, cardY + 118.0f, impPct, barCol, 0.82f);

			TextCentered(FONT_BODY, 200.0f, 212.0f, "(L/R) Switch Tab   |   (A) Toggle   |   (B) Save & Return", t.textMuted, 0.80f);
		}

		EndFrame();
	}

	UI::RestoreRenderState();
}

void RunOptionsPage(const RomID & romId, bool applyRuntime)
{
	RunUnifiedMenu(nullptr, romId, applyRuntime, nullptr);
}

std::string RunRomSelector()
{
	if (!EnsureMenuPool())
		return std::string();

	FrontendAudio::Begin(true, 0.18f);

	ScanRoms(false);
	RefreshMemorySummary();
	TickSystemStatus(true);

	RepeatTimer rep;
	rep.Reset();
	TouchTracker touch;
	touch.Reset();

	char  toast[64]    = "";
	int   toastFrames  = 0;
	int   prevSelected = s_selected;
	float infoAnim     = 0.0f;
	float enterT       = 0.0f;

	// 2x3 3DS HOME Menu Cartridge Grid coordinates (3 columns x 2 rows = 6 tiles per page)
	static const float kGridColX[3] = { 16.0f, 116.0f, 216.0f };
	static const float kGridRowY[2] = { 38.0f, 118.0f };
	constexpr float kTileW = 88.0f;
	constexpr float kTileH = 72.0f;

	{
		const int slot0 = (s_selected >= 0) ? (s_selected % kGridPageSize) : 0;
		s_hlX = kGridColX[slot0 % 3];
		s_hlY = kGridRowY[slot0 / 3];
		s_hlVX = 0.0f;
		s_hlVY = 0.0f;
	}

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

	while (aptMainLoop() && !shouldQuit)
	{
		hidScanInput();
		u32 down = hidKeysDown();
		const u32 held = hidKeysHeld();
		UpdateTouchFromHid(touch, held);
		TickSystemStatus(false);

		s_vcAnimTime += 1.0f / 60.0f;
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

		const int pageIdx   = (s_viewCount > 0) ? (s_selected / kGridPageSize) : 0;
		const int pageStart = pageIdx * kGridPageSize;
		const int totalPages = (s_viewCount > 0) ? ((s_viewCount + kGridPageSize - 1) / kGridPageSize) : 1;

		int pressedTile = -1;
		int pressedBumper = -1;
		if (touch.active && !touch.dragged)
		{
			const int tx = touch.curX;
			const int ty = touch.curY;
			if (ty >= 202)
				pressedBumper = (tx < 152) ? 0 : 1;
			else if (ty >= 36 && ty < 196)
			{
				for (int slot = 0; slot < kGridPageSize; ++slot)
				{
					const float gx = kGridColX[slot % 3];
					const float gy = kGridRowY[slot / 3];
					if (tx >= (int)gx && tx < (int)(gx + kTileW) &&
					    ty >= (int)gy && ty < (int)(gy + kTileH) &&
					    (pageStart + slot) < s_viewCount)
					{
						pressedTile = pageStart + slot;
						break;
					}
				}
			}
		}

		if (touch.justReleased && !touch.dragged)
		{
			const int tx = touch.releaseX;
			const int ty = touch.releaseY;

			// 1) Top L/R Filter Bar (y = 0..32)
			if (ty < 32)
			{
				if (tx < 34)
					down |= KEY_L;
				else if (tx >= 286)
					down |= KEY_R;
				else
				{
					const int newFilter = (tx < 124) ? FILTER_ALL : ((tx < 210) ? FILTER_FAV : FILTER_RECENT);
					if (newFilter != s_sortMode)
					{
						s_sortMode = newFilter;
						RebuildView();
						SaveFrontendConfig();
						FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
					}
				}
			}
			// 2) Bottom Sculpted 3DS Bumper Buttons (y >= 202)
			else if (ty >= 202)
			{
				if (tx < 152) down |= KEY_X;
				else down |= KEY_A;
			}
			// 3) Center 2x3 3DS Home Menu Cartridge Tiles
			else if (s_viewCount > 0)
			{
				for (int slot = 0; slot < kGridPageSize; ++slot)
				{
					const float gx = kGridColX[slot % 3];
					const float gy = kGridRowY[slot / 3];
					const int idx  = pageStart + slot;
					if (idx < s_viewCount &&
					    tx >= (int)gx && tx < (int)(gx + kTileW) &&
					    ty >= (int)gy && ty < (int)(gy + kTileH))
					{
						if (idx == s_selected)
							down |= KEY_A;
						else
							s_selected = idx;
						break;
					}
				}
			}
		}

		if (down & KEY_START)
		{
			FrontendAudio::PlaySfx(FrontendAudio::SFX_BACK);
			shouldQuit = true;
			break;
		}

		if (down & KEY_Y)
		{
			CycleTheme(1);
			SaveFrontendConfig();
			FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
			snprintf(toast, sizeof(toast), "Theme: %s", ActiveTheme().name);
			toastFrames = 70;
		}

		if (down & (KEY_L | KEY_R))
		{
			s_sortMode = (s_sortMode + ((down & KEY_R) ? 1 : (FILTER_COUNT - 1))) % FILTER_COUNT;
			RebuildView();
			SaveFrontendConfig();
			FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
			snprintf(toast, sizeof(toast), "Filter: %s", kFilterNames[s_sortMode]);
			toastFrames = 70;
		}

		if (down & KEY_X)
		{
			RomID optRomId;
			const char * gTitle = nullptr;
			if (s_viewCount > 0 && s_roms[s_viewOrder[s_selected]].hasDetails)
			{
				optRomId = s_roms[s_viewOrder[s_selected]].romId;
				gTitle   = s_roms[s_viewOrder[s_selected]].title;
			}
			FrontendAudio::PlaySfx(FrontendAudio::SFX_SELECT);
			RunUnifiedMenu(gTitle, optRomId, false, nullptr);
			RefreshMemorySummary();
			touch.Reset();
		}

		if (s_viewCount > 0)
		{
			if (rep.Poll(down, held, KEY_RIGHT | KEY_DRIGHT | KEY_CPAD_RIGHT, 2))
				s_selected = (s_selected + 1) % s_viewCount;
			if (rep.Poll(down, held, KEY_LEFT | KEY_DLEFT | KEY_CPAD_LEFT, 3))
				s_selected = (s_selected + s_viewCount - 1) % s_viewCount;
			if (rep.Poll(down, held, KEY_DOWN | KEY_DDOWN | KEY_CPAD_DOWN, 0))
			{
				if (s_selected + 3 < s_viewCount) s_selected += 3;
				else if ((s_selected % 6) < 3 && (s_selected / 6) * 6 + 3 < s_viewCount) s_selected = s_viewCount - 1;
				else s_selected = (s_selected - 3 >= 0) ? (s_selected - 3) : 0;
			}
			if (rep.Poll(down, held, KEY_UP | KEY_DUP | KEY_CPAD_UP, 1))
			{
				if ((s_selected % 6) >= 3) s_selected -= 3;
				else if (s_selected + 3 < s_viewCount) s_selected += 3;
			}

			if (down & KEY_SELECT)
			{
				FrontendRomInfo & cur = s_roms[s_viewOrder[s_selected]];
				cur.favorite = !cur.favorite;
				SaveFrontendConfig();
				FrontendAudio::PlaySfx(FrontendAudio::SFX_TOGGLE);
				snprintf(toast, sizeof(toast), cur.favorite ? "Added to Favorites" : "Removed from Favorites");
				toastFrames = 70;
				if (s_sortMode == FILTER_FAV)
					RebuildView();
			}

			if (s_selected != prevSelected)
			{
				prevSelected = s_selected;
				infoAnim = 0.0f;
				FrontendAudio::PlaySfx(FrontendAudio::SFX_MOVE);
				IO::Filename selPath;
				BuildPreviewPathForRom(s_roms[s_viewOrder[s_selected]], selPath);
				CoverScraper::Prioritize(selPath);
			}
			RequestPreviewForRom(s_viewOrder[s_selected], false);
		}

		// Reload cover automatically as soon as the background scraper saves a new PNG
		const CoverScraper::ScraperStatus scrapeSt = CoverScraper::GetStatus();
		if (scrapeSt.revision != lastScrapeRev)
		{
			lastScrapeRev = scrapeSt.revision;
			for (int i = 0; i < kGridPageSize; ++i)
				s_gridTexRomIdx[i] = -1;
			if (s_viewCount > 0 && s_previewTex == nullptr)
			{
				s_previewRomIdx = -1;
				RequestPreviewForRom(s_viewOrder[s_selected], true);
			}
		}

		TickPreviewLoader();
		const int activePageStart = ((s_viewCount > 0) ? (s_selected / kGridPageSize) : 0) * kGridPageSize;
		TickGridPageCovers(activePageStart);

		const bool launch = (down & KEY_A) && s_viewCount > 0;

		const int selSlot = (s_viewCount > 0) ? (s_selected % kGridPageSize) : 0;
		SpringStep(s_hlX, s_hlVX, kGridColX[selSlot % 3], 340.0f, 26.0f);
		SpringStep(s_hlY, s_hlVY, kGridRowY[selSlot / 3], 340.0f, 26.0f);

		const ThemePalette & t = ActiveTheme();
		const float enterEase  = EaseOutCubic(enterT);
		const float infoEase   = EaseOutCubic(infoAnim);
		const float slideY     = (1.0f - enterEase) * 14.0f;

		BeginFrame();

		// ====================================================================
		// BOTTOM SCREEN (320x240): Authentic 3DS HOME Menu 2x3 Cartridge Shelf
		// ====================================================================
		const uint32_t homeBg       = t.bg;
		const uint32_t homeBorder   = t.cardBorder;
		const uint32_t homeTileOut  = t.cardBg;
		const uint32_t homeTileIn   = t.cardBgAlt;
		const uint32_t homeText     = t.text;
		const uint32_t homeCyan     = t.focusBorder;
		const uint32_t homeCyanGlow = WithAlpha(t.focusBorder, 34);

		BeginScreen(GFX_BOTTOM, 320, 240, homeBg);
		DrawBackdrop(320.0f, 240.0f, t);

		// 1. Top L / All Games / Favorites / Recent / R Segmented Tab Bar (y = 0..30)
		QuadVGrad(0.0f, 0.0f, 320.0f, 30.0f,
		          t.headerBg, LerpColor(t.headerBg, t.bg, 0.5f));
		Quad(0.0f, 30.0f, 320.0f, 1.0f, t.headerRule);

		// Left [L] and Right [R] sculpted shoulder pills
		RoundedRectVGrad(5.0f, 4.0f, 26.0f, 22.0f, 7.0f, t.cardBgAlt, t.cardBg);
		RoundedBorder1(5.0f, 4.0f, 26.0f, 22.0f, 7.0f, homeBorder);
		TextCentered(FONT_BODY, 18.0f, 8.0f, "L", homeText, 0.84f);

		RoundedRectVGrad(289.0f, 4.0f, 26.0f, 22.0f, 7.0f, t.cardBgAlt, t.cardBg);
		RoundedBorder1(289.0f, 4.0f, 26.0f, 22.0f, 7.0f, homeBorder);
		TextCentered(FONT_BODY, 302.0f, 8.0f, "R", homeText, 0.84f);

		for (int fi = 0; fi < FILTER_COUNT; ++fi)
		{
			const float fx = 36.0f + fi * 83.0f;
			const float fw = 80.0f;
			const bool isAct = (fi == s_sortMode);
			if (isAct)
			{
				RoundedRectVGrad(fx, 4.0f, fw, 22.0f, 7.0f,
				                 t.accent, LerpColor(t.n64Blue, t.accent, 0.25f));
				RoundedBorder1(fx, 4.0f, fw, 22.0f, 7.0f, WithAlpha(LerpColor(t.accent, RGBA(255, 255, 255), 0.45f), 175));
			}
			else
			{
				RoundedRect(fx, 4.0f, fw, 22.0f, 7.0f, WithAlpha(t.cardBg, 175));
			}
			TextCentered(FONT_BODY, fx + fw * 0.5f, 8.0f, kFilterNames[fi],
			             isAct ? RGBA(255, 255, 255) : t.textMuted, 0.84f);
		}

		// 2. Center 2x3 3DS Home Menu Cartridge Tile Shelf (y = 36..196)
		if (s_viewCount == 0)
		{
			DrawCard(18.0f, 54.0f + slideY, 284.0f, 120.0f, false, false, t, 10.0f);
			TextCentered(FONT_TITLE, 160.0f, 74.0f + slideY, "No N64 ROMs found", homeText, 1.0f);
			TextCentered(FONT_BODY, 160.0f, 102.0f + slideY, "Copy .z64 / .n64 / .v64 files to:", t.textMuted, 0.90f);
			TextCentered(FONT_BODY, 160.0f, 122.0f + slideY, "sdmc:/3ds/DaedalusX64/Roms", t.accent, 0.90f);
		}
		else
		{
			for (int slot = 0; slot < kGridPageSize; ++slot)
			{
				const float gx = kGridColX[slot % 3];
				const float gy = kGridRowY[slot / 3] + slideY;
				const int idx  = activePageStart + slot;

				if (idx >= s_viewCount)
				{
					// Subtle empty 3DS Home Menu slot socket
					RoundedRect(gx + 5.0f, gy + 5.0f, kTileW - 10.0f, kTileH - 10.0f, 12.0f,
					            WithAlpha(homeBorder, 48));
					RoundedBorder1(gx + 5.0f, gy + 5.0f, kTileW - 10.0f, kTileH - 10.0f, 12.0f,
					               WithAlpha(homeBorder, 72));
					continue;
				}

				const FrontendRomInfo & r = s_roms[s_viewOrder[idx]];
				const bool pressed = (idx == pressedTile);

				// Outer sculpted 3DS Home Menu squircle tile + soft drop shadow
				if ((t.cardShadow >> 24) > 0)
					RoundedRect(gx, gy + 2.5f, kTileW, kTileH, 14.0f, t.cardShadow);
				RoundedRectVGrad(gx, gy, kTileW, kTileH, 14.0f,
				                 pressed ? homeBorder : t.cardBgAlt,
				                 pressed ? homeTileOut : t.cardBg);
				RoundedBorder1(gx, gy, kTileW, kTileH, 14.0f, homeBorder);

				// Inner recessed artwork well
				const float inX = gx + 6.0f;
				const float inY = gy + 6.0f;
				const float inW = kTileW - 12.0f;
				const float inH = kTileH - 12.0f;
				RoundedRect(inX, inY, inW, inH, 8.0f, homeTileIn);

				CRefPtr<CNativeTexture> tileTex = s_gridTex[slot];
				if (idx == s_selected && s_previewRomIdx == s_viewOrder[idx] &&
				    s_previewTex != nullptr && s_previewTex->HasData())
					tileTex = s_previewTex;

				if (tileTex != nullptr && tileTex->HasData())
				{
					const float pw = (float)tileTex->GetWidth();
					const float ph = (float)tileTex->GetHeight();
					const float u1 = pw * tileTex->GetScaleX();
					const float v1 = ph * tileTex->GetScaleY();
					TexturedQuad(tileTex->GetC3DTexture(),
					             inX + 1.5f, inY + 1.5f, inW - 3.0f, inH - 3.0f,
					             0.0f, 0.0f, u1, v1, RGBA(255, 255, 255));
					// Subtle top glass sheen without washing out artwork
					QuadVGrad(inX + 1.5f, inY + 1.5f, inW - 3.0f, (inH - 3.0f) * 0.28f,
					          RGBA(255, 255, 255, 18), RGBA(255, 255, 255, 0));
					RoundedBorder1(inX + 1.0f, inY + 1.0f, inW - 2.0f, inH - 2.0f, 6.0f,
					               RGBA(255, 255, 255, 32));
				}
				else
				{
					// Stylized N64 Game Pak Cartridge tile with short game title
					static const uint32_t kCartCols[4] = {
						RGBA(245, 197, 24), RGBA(30, 90, 232), RGBA(232, 46, 56), RGBA(24, 184, 88)
					};
					const uint32_t cCol = kCartCols[s_viewOrder[idx] & 3];
					RoundedRect(inX + 12.0f, inY + 6.0f, inW - 24.0f, 30.0f, 6.0f, RGBA(74, 82, 96));
					RoundedRect(inX + 18.0f, inY + 10.0f, inW - 36.0f, 22.0f, 4.0f, cCol);
					Icon(ICON_CARTRIDGE, inX + (inW - 16.0f) * 0.5f, inY + 13.0f, 16.0f, RGBA(255, 255, 255));
					TextEllipsis(FONT_BODY, inX + 3.0f, inY + 39.0f, inW - 6.0f, r.title, homeText, 0.72f);
				}

				if (r.favorite)
				{
					Circle(gx + kTileW - 11.0f, gy + 11.0f, 7.5f, RGBA(22, 28, 40, 210));
					Icon(ICON_STAR, gx + kTileW - 16.5f, gy + 5.5f, 11.0f, RGBA(248, 202, 26));
				}
			}

			// Authentic Nintendo 3DS Glowing Cyan 4-Corner Selection Cursor!
			Draw3DSCornerCursor(s_hlX, s_hlY + slideY, kTileW, kTileH, 14.0f, homeCyan, homeCyanGlow);

			// Modern capsule page indicator dots if more than 6 ROMs
			if (totalPages > 1)
			{
				const int visPages = (totalPages < 12) ? totalPages : 12;
				const float totalW = (visPages - 1) * 9.0f + 14.0f;
				float px = 160.0f - totalW * 0.5f;
				for (int p = 0; p < visPages; ++p)
				{
					if (p == pageIdx)
					{
						RoundedRect(px, 193.5f, 12.0f, 4.0f, 2.0f, homeCyan);
						px += 16.0f;
					}
					else
					{
						Circle(px + 2.0f, 195.5f, 1.8f, homeBorder);
						px += 8.0f;
					}
				}
			}
		}

		// 3. Bottom Sculpted 3DS Bumper Buttons: [Settings (X)] & [Launch Game (A)] (y = 204..236)
		{
			// Left Bumper: Settings (X) — themed brushed pill
			const float bx0 = 8.0f, by0 = 204.0f, bw0 = 142.0f, bh0 = 32.0f;
			if ((t.cardShadow >> 24) > 0)
				RoundedRect(bx0, by0 + 2.0f, bw0, bh0, 11.0f, t.cardShadow);
			RoundedRectVGrad(bx0, by0, bw0, bh0, 11.0f,
			                 (pressedBumper == 0) ? homeBorder : t.cardBgAlt,
			                 (pressedBumper == 0) ? homeTileOut : t.cardBg);
			RoundedBorder1(bx0, by0, bw0, bh0, 11.0f, homeBorder);
			TextCentered(FONT_TITLE, bx0 + bw0 * 0.5f, by0 + 8.5f, "Settings (X)", homeText, 0.88f);

			// Right Bumper: Launch Game (A) — refined themed CTA pill
			const float bx1 = 156.0f, by1 = 204.0f, bw1 = 156.0f, bh1 = 32.0f;
			if ((t.cardShadow >> 24) > 0)
				RoundedRect(bx1, by1 + 2.0f, bw1, bh1, 11.0f, t.cardShadow);
			const uint32_t ctaTop = (pressedBumper == 1) ? LerpColor(t.accent, RGBA(0, 0, 0), 0.2f) : t.accent;
			const uint32_t ctaBot = (pressedBumper == 1) ? LerpColor(t.n64Blue, RGBA(0, 0, 0), 0.2f) : t.n64Blue;
			RoundedRectVGrad(bx1, by1, bw1, bh1, 11.0f, ctaTop, ctaBot);
			RoundedBorder1(bx1, by1, bw1, bh1, 11.0f,
			               (pressedBumper == 1) ? RGBA(255, 255, 255, 200) : WithAlpha(LerpColor(t.accent, RGBA(255, 255, 255), 0.45f), 175));
			TextCentered(FONT_TITLE, bx1 + bw1 * 0.5f, by1 + 9.5f, "Launch Game (A)", RGBA(8, 22, 48, 145), 0.88f);
			TextCentered(FONT_TITLE, bx1 + bw1 * 0.5f, by1 + 8.5f, "Launch Game (A)", RGBA(255, 255, 255), 0.88f);
		}

		if (toastFrames > 0)
		{
			RoundedRect(64.0f, 174.0f, 192.0f, 22.0f, 10.0f, WithAlpha(t.hintBg, 236));
			RoundedBorder1(64.0f, 174.0f, 192.0f, 22.0f, 10.0f, WithAlpha(homeCyan, 185));
			TextCentered(FONT_BODY, 160.0f, 178.5f, toast, t.hintText, 0.84f);
			--toastFrames;
		}

		// ====================================================================
		// TOP SCREEN (400x240): 3D N64 Box + 3D N64 Cartridge Stage
		// ====================================================================
		BeginScreen(GFX_TOP, 400, 240, t.headerBg);
		const FrontendRomInfo * selRom = (s_viewCount > 0) ? &s_roms[s_viewOrder[s_selected]] : nullptr;
		CRefPtr<CNativeTexture> stageCover = nullptr;
		if (s_viewCount > 0)
		{
			const int selRomIdx = s_viewOrder[s_selected];
			if (s_previewRomIdx == selRomIdx && s_previewTex != nullptr && s_previewTex->HasData())
				stageCover = s_previewTex;
			else if (s_gridTexRomIdx[selSlot] == selRomIdx && s_gridTex[selSlot] != nullptr && s_gridTex[selSlot]->HasData())
				stageCover = s_gridTex[selSlot];
		}
		DrawVirtualConsoleTopStage(selRom, stageCover, infoEase, s_vcAnimTime, t);

		if (scrapeSt.active && scrapeSt.total > 0)
		{
			char scrapeLine[48];
			snprintf(scrapeLine, sizeof(scrapeLine), "Covers %d/%d", scrapeSt.completed, scrapeSt.total);
			Text(FONT_BODY, 12.0f, 222.0f, scrapeLine, homeCyan, 0.74f);
		}

		EndFrame();

		if (launch)
		{
			FrontendAudio::SetAmbientGain(0.0f);
			FrontendAudio::PlaySfx(FrontendAudio::SFX_LAUNCH);
			CoverScraper::Stop();
			const FrontendRomInfo & chosen = s_roms[s_viewOrder[s_selected]];
			CopyStr(s_lastSelectedFile, sizeof(s_lastSelectedFile), chosen.filename);
			PushRecentRom(chosen.filename);
			SaveFrontendConfig();
			for (int f = 0; f < 14 && aptMainLoop(); ++f)
				gspWaitForVBlank();
			return std::string(chosen.path);
		}
	}

	CoverScraper::Stop();
	return std::string();
}

void RunPauseMenu(const char * gameName, const RomID & romId)
{
	CRefPtr<CNativeTexture> freezeSnap = CaptureTopScreenSnapshot();
	if (!EnsureMenuPool())
	{
		freezeSnap = nullptr;
		UI::RestoreRenderState();
		return;
	}

	FrontendAudio::Begin(true, 0.11f);
	FrontendAudio::PlaySfx(FrontendAudio::SFX_SELECT);

	RunUnifiedMenu(gameName, romId, true, freezeSnap);

	freezeSnap = nullptr;
	ReleaseAssets();
	UI::RestoreRenderState();
}

} // namespace FrontendCTR
