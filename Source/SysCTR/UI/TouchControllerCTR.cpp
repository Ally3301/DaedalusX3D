#include "TouchControllerCTR.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef DAEDALUS_CTR
#include "stdafx.h"
#include <3ds.h>
#include "UITheme.h"
#include "UIAtlas.h"
#include "UIRenderer.h"
#include "UISound.h"
#include "Config/ConfigOptions.h"
#include "Core/CPU.h"
#include "SysCTR/HLEGraphics/RendererCTR.h"
#include "Utility/IO.h"
#include "Utility/Preferences.h"

extern float gCurrentFramerate;
#endif

namespace UI
{

namespace
{

static inline float ClampF(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

struct DeckLayout
{
	// Right-handed base coordinates (mirrored when leftHanded == true)
	float padCx, padCy;
	float zX, zY, zW, zH;
	float zLockX, zLockY, zLockW, zLockH;
	float qsX, qsY, qsW, qsH;
	float qlX, qlY, qlW, qlH;
};

DeckLayout ComputeDeck(bool leftHanded)
{
	DeckLayout d;
	if (!leftHanded)
	{
		d.zX = 10.0f;  d.zY = 38.0f;  d.zW = 94.0f;  d.zH = 78.0f;
		d.zLockX = 10.0f; d.zLockY = 123.0f; d.zLockW = 94.0f; d.zLockH = 26.0f;
		d.qsX = 10.0f; d.qsY = 158.0f; d.qsW = 44.0f; d.qsH = 54.0f;
		d.qlX = 60.0f; d.qlY = 158.0f; d.qlW = 44.0f; d.qlH = 54.0f;
		d.padCx = 216.0f; d.padCy = 132.0f;
	}
	else
	{
		d.zX = 216.0f; d.zY = 38.0f;  d.zW = 94.0f;  d.zH = 78.0f;
		d.zLockX = 216.0f; d.zLockY = 123.0f; d.zLockW = 94.0f; d.zLockH = 26.0f;
		d.qsX = 216.0f; d.qsY = 158.0f; d.qsW = 44.0f; d.qsH = 54.0f;
		d.qlX = 266.0f; d.qlY = 158.0f; d.qlW = 44.0f; d.qlH = 54.0f;
		d.padCx = 104.0f; d.padCy = 132.0f;
	}
	return d;
}

static inline bool InBox(int px, int py, float x, float y, float w, float h)
{
	return (float)px >= x && (float)px < (x + w) && (float)py >= y && (float)py < (y + h);
}

// Convert normalized stick (nx, ny) with Y-up into 8-way directional button mask.
uint32_t VectorToEightWay(float nx, float ny, float deadzone,
                          uint32_t upMask, uint32_t downMask,
                          uint32_t leftMask, uint32_t rightMask)
{
	const float mag = sqrtf(nx * nx + ny * ny);
	if (mag < deadzone)
		return 0u;

	// Use 22.5° sectors (tan(22.5°) ~= 0.4142) for crisp 8-way cardinal + diagonal feel
	const float ax = fabsf(nx);
	const float ay = fabsf(ny);
	uint32_t mask = 0u;
	if (ax >= ay * 0.4142f)
		mask |= (nx > 0.0f) ? rightMask : leftMask;
	if (ay >= ax * 0.4142f)
		mask |= (ny > 0.0f) ? upMask : downMask;
	return mask;
}

} // namespace

// ============================================================================
// Pure host-testable TouchPadEngine
// ============================================================================

TouchPadEngine::TouchPadEngine()
	: mWakeLayout(TOUCH_LAYOUT_CCROSS)
	, mDirty(true)
	, mPrevHeld(false)
	, mLastX(0)
	, mLastY(0)
	, mZHeld(false)
	, mZLatched(false)
	, mStickCaptured(false)
	, mStickVisX(0.0f)
	, mStickVisY(0.0f)
	, mActiveDirs(0u)
	, mQsHoldFrames(0)
	, mQlHoldFrames(0)
	, mQsFired(false)
	, mQlFired(false)
	, mQsProgress(0.0f)
	, mQlProgress(0.0f)
	, mPressedTopBtn(-1)
{
	mCfg.layout        = TOUCH_LAYOUT_CCROSS; // User preference: Cross C-buttons by default
	mCfg.leftHanded    = false;
	mCfg.zToggleMode   = false;
	mCfg.stickDeadzone = 0.22f;
	mCfg.stickSens     = 1.0f;
	mCfg.invertY       = false;
}

void TouchPadEngine::ResetSession()
{
	mDirty         = true;
	mPrevHeld      = false;
	mZHeld         = false;
	mZLatched      = false;
	mStickCaptured = false;
	mStickVisX     = 0.0f;
	mStickVisY     = 0.0f;
	mActiveDirs    = 0u;
	mQsHoldFrames  = 0;
	mQlHoldFrames  = 0;
	mQsFired       = false;
	mQlFired       = false;
	mQsProgress    = 0.0f;
	mQlProgress    = 0.0f;
	mPressedTopBtn = -1;
}

void TouchPadEngine::SetConfig(const TouchPadConfig & cfg)
{
	mCfg = cfg;
	if (mCfg.layout != TOUCH_LAYOUT_SLEEP)
		mWakeLayout = mCfg.layout;
	mCfg.stickDeadzone = ClampF(mCfg.stickDeadzone, 0.12f, 0.45f);
	mCfg.stickSens     = ClampF(mCfg.stickSens, 0.5f, 1.8f);
	mDirty = true;
}

const char * TouchPadEngine::LayoutName(ETouchPadLayout l)
{
	switch (l)
	{
	case TOUCH_LAYOUT_CCROSS:       return "C-Buttons Cross";
	case TOUCH_LAYOUT_CSTICK:       return "C-Stick Camera";
	case TOUCH_LAYOUT_DUAL_SHOOTER: return "Dual-Analog Shooter";
	case TOUCH_LAYOUT_DPAD:         return "N64 D-Pad";
	case TOUCH_LAYOUT_SLEEP:        return "Screen Off (Sleep)";
	default:                        return "C-Buttons Cross";
	}
}

const char * TouchPadEngine::LayoutShortName(ETouchPadLayout l)
{
	switch (l)
	{
	case TOUCH_LAYOUT_CCROSS:       return "C-Cross";
	case TOUCH_LAYOUT_CSTICK:       return "C-Stick";
	case TOUCH_LAYOUT_DUAL_SHOOTER: return "Shooter";
	case TOUCH_LAYOUT_DPAD:         return "D-Pad";
	case TOUCH_LAYOUT_SLEEP:        return "Sleep";
	default:                        return "C-Cross";
	}
}

bool TouchPadEngine::Update(bool touchHeld, int px, int py, TouchPadOutput & out)
{
	std::memset(&out, 0, sizeof(out));
	out.swapSticks = (mCfg.layout == TOUCH_LAYOUT_DUAL_SHOOTER);

	const bool validCoord = touchHeld && (px > 0 || py > 0) && px >= 0 && px < 320 && py >= 0 && py < 240;
	const bool justPressed  = validCoord && !mPrevHeld;
	const bool justReleased = !touchHeld && mPrevHeld;

	if (validCoord)
	{
		mLastX = px;
		mLastY = py;
	}

	const bool     prevZActive   = IsZActive();
	const uint32_t prevDirs      = mActiveDirs;
	const int      prevTopBtn    = mPressedTopBtn;
	const float    prevVisX      = mStickVisX;
	const float    prevVisY      = mStickVisY;
	const float    prevQsProg    = mQsProgress;
	const float    prevQlProg    = mQlProgress;
	const bool     prevCaptured  = mStickCaptured;

	// Sleep mode: tap anywhere to wake back to previous control layout
	if (mCfg.layout == TOUCH_LAYOUT_SLEEP)
	{
		mZHeld = false;
		mStickCaptured = false;
		mActiveDirs = 0u;
		mPressedTopBtn = -1;
		mQsHoldFrames = mQlHoldFrames = 0;
		mQsProgress = mQlProgress = 0.0f;
		if (justReleased)
		{
			mCfg.layout = (mWakeLayout == TOUCH_LAYOUT_SLEEP) ? TOUCH_LAYOUT_CCROSS : mWakeLayout;
			mDirty = true;
		}
		mPrevHeld = touchHeld;
		return mDirty;
	}

	const DeckLayout deck = ComputeDeck(mCfg.leftHanded);

	// Top HUD bar button hit regions (y = 0..28)
	auto HitTopBar = [](int x, int y) -> int {
		if (y < 0 || y >= 28) return -1;
		if (x >= 72  && x < 104) return 0; // Mute toggle
		if (x >= 146 && x < 234) return 1; // Layout mode cycle
		if (x >= 238 && x < 268) return 2; // Sleep / screen off
		if (x >= 272 && x < 316) return 3; // Pause menu
		return -1;
	};

	// Reset transient per-frame states
	mZHeld = false;
	mActiveDirs = 0u;
	mPressedTopBtn = -1;

	if (validCoord)
	{
		const int topHit = HitTopBar(px, py);
		if (justPressed && topHit >= 0)
		{
			mPressedTopBtn = topHit;
			mStickCaptured = false;
		}
		else if (!mStickCaptured && topHit >= 0 && prevTopBtn == topHit)
		{
			mPressedTopBtn = topHit;
		}
		else
		{
			// Check stick / C-pad capture radius (elastic capture keeps control locked while dragging)
			const float dx = (float)px - deck.padCx;
			const float dy = (float)py - deck.padCy;
			const float dist = sqrtf(dx * dx + dy * dy);
			constexpr float kPadVisualRadius  = 72.0f;
			constexpr float kPadCaptureRadius = 90.0f;

			if (justPressed && dist <= kPadCaptureRadius)
				mStickCaptured = true;

			if (mStickCaptured)
			{
				const float r = kPadVisualRadius;
				float nx = dx / r;
				float ny = -dy / r; // N64 Y+ is up
				const float mag = sqrtf(nx * nx + ny * ny);
				if (mag > 1.0f)
				{
					nx /= mag;
					ny /= mag;
				}
				mStickVisX = nx;
				mStickVisY = ny;

				const float effY = mCfg.invertY ? -ny : ny;

				if (mCfg.layout == TOUCH_LAYOUT_CCROSS)
				{
					// Cross 4-way C-Buttons with smooth slide-across & diagonal support
					mActiveDirs = VectorToEightWay(nx, effY, 0.24f,
					                               N64Mask::kCUp, N64Mask::kCDown,
					                               N64Mask::kCLeft, N64Mask::kCRight);
				}
				else if (mCfg.layout == TOUCH_LAYOUT_CSTICK)
				{
					mActiveDirs = VectorToEightWay(nx, effY, mCfg.stickDeadzone,
					                               N64Mask::kCUp, N64Mask::kCDown,
					                               N64Mask::kCLeft, N64Mask::kCRight);
				}
				else if (mCfg.layout == TOUCH_LAYOUT_DUAL_SHOOTER)
				{
					// True proportional N64 Main Stick output with smooth deadzone rescaling
					const float rawMag = fminf(1.0f, mag);
					if (rawMag > mCfg.stickDeadzone)
					{
						const float scaled = (rawMag - mCfg.stickDeadzone) / (1.0f - mCfg.stickDeadzone);
						// Gentle quadratic curve (0.45 linear + 0.55 quad) for precise aiming
						const float curved = (0.45f * scaled + 0.55f * scaled * scaled) * mCfg.stickSens;
						const float clamped = fminf(1.0f, curved);
						const float ux = (mag > 1e-4f) ? (nx / mag) : 0.0f;
						const float uy = (mag > 1e-4f) ? (effY / mag) : 0.0f;
						out.analogX = (int8_t)ClampF(ux * clamped * 80.0f, -80.0f, 80.0f);
						out.analogY = (int8_t)ClampF(uy * clamped * 80.0f, -80.0f, 80.0f);
						// Also light up direction indicators subtly in HUD
						mActiveDirs = VectorToEightWay(nx, effY, mCfg.stickDeadzone,
						                               N64Mask::kCUp, N64Mask::kCDown,
						                               N64Mask::kCLeft, N64Mask::kCRight);
					}
				}
				else if (mCfg.layout == TOUCH_LAYOUT_DPAD)
				{
					mActiveDirs = VectorToEightWay(nx, ny, 0.24f,
					                               N64Mask::kDUp, N64Mask::kDDown,
					                               N64Mask::kDLeft, N64Mask::kDRight);
				}
			}
			else
			{
				// Left (or mirrored) auxiliary column: Z Trigger, Z-Lock, QS, QL
				if (InBox(px, py, deck.zX, deck.zY, deck.zW, deck.zH))
				{
					if (mCfg.zToggleMode)
					{
						if (justPressed)
							mZLatched = !mZLatched;
					}
					else
					{
						mZHeld = true;
					}
				}
				else if (justPressed && InBox(px, py, deck.zLockX, deck.zLockY, deck.zLockW, deck.zLockH))
				{
					mZLatched = !mZLatched;
					mDirty = true;
				}
				else if (InBox(px, py, deck.qsX, deck.qsY, deck.qsW, deck.qsH))
				{
					if (!mQsFired)
					{
						++mQsHoldFrames;
						mQsProgress = ClampF((float)mQsHoldFrames / (float)kQuickHoldFrames, 0.0f, 1.0f);
						if (mQsHoldFrames >= kQuickHoldFrames)
						{
							mQsFired = true;
							out.requestQuickSave = true;
						}
					}
				}
				else if (InBox(px, py, deck.qlX, deck.qlY, deck.qlW, deck.qlH))
				{
					if (!mQlFired)
					{
						++mQlHoldFrames;
						mQlProgress = ClampF((float)mQlHoldFrames / (float)kQuickHoldFrames, 0.0f, 1.0f);
						if (mQlHoldFrames >= kQuickHoldFrames)
						{
							mQlFired = true;
							out.requestQuickLoad = true;
						}
					}
				}
			}
		}
	}
	else if (justReleased)
	{
		// Fire top-bar actions cleanly on release
		const int relTop = HitTopBar(mLastX, mLastY);
		if (relTop >= 0 && prevTopBtn == relTop)
		{
			if (relTop == 0)
			{
				out.requestMuteToggle = true;
			}
			else if (relTop == 1)
			{
				// Cycle active control layouts (skipping SLEEP, which has its own moon button)
				const int next = ((int)mCfg.layout + 1) % 4;
				mCfg.layout = (ETouchPadLayout)next;
				mWakeLayout = mCfg.layout;
				mDirty = true;
			}
			else if (relTop == 2)
			{
				mWakeLayout = mCfg.layout;
				mCfg.layout = TOUCH_LAYOUT_SLEEP;
				mDirty = true;
			}
			else if (relTop == 3)
			{
				out.requestPause = true;
			}
		}
		mStickCaptured = false;
		mStickVisX = 0.0f;
		mStickVisY = 0.0f;
		mQsHoldFrames = mQlHoldFrames = 0;
		mQsFired = mQlFired = false;
		mQsProgress = mQlProgress = 0.0f;
	}
	else if (!touchHeld)
	{
		mStickCaptured = false;
		mStickVisX = 0.0f;
		mStickVisY = 0.0f;
		mQsHoldFrames = mQlHoldFrames = 0;
		mQsFired = mQlFired = false;
		mQsProgress = mQlProgress = 0.0f;
	}

	// Reset QS/QL hold counters if finger slides out of the button
	if (validCoord && !InBox(px, py, deck.qsX, deck.qsY, deck.qsW, deck.qsH))
	{
		mQsHoldFrames = 0;
		mQsFired = false;
		mQsProgress = 0.0f;
	}
	if (validCoord && !InBox(px, py, deck.qlX, deck.qlY, deck.qlW, deck.qlH))
	{
		mQlHoldFrames = 0;
		mQlFired = false;
		mQlProgress = 0.0f;
	}

	// Assemble final N64 button output (never emit C-buttons in Shooter mode where touch = Main Stick)
	if (mCfg.layout != TOUCH_LAYOUT_DUAL_SHOOTER)
		out.n64Buttons |= mActiveDirs;
	if (mZHeld || mZLatched)
		out.n64Buttons |= N64Mask::kZTrig;

	// Mark dirty ONLY if visible HUD state actually changed!
	if (prevZActive != IsZActive() ||
	    prevDirs != mActiveDirs ||
	    prevTopBtn != mPressedTopBtn ||
	    prevCaptured != mStickCaptured ||
	    fabsf(prevVisX - mStickVisX) > 0.02f ||
	    fabsf(prevVisY - mStickVisY) > 0.02f ||
	    fabsf(prevQsProg - mQsProgress) > 0.04f ||
	    fabsf(prevQlProg - mQlProgress) > 0.04f)
	{
		mDirty = true;
	}

	mPrevHeld = touchHeld;
	return mDirty;
}

// ============================================================================
// CTR Hardware Integration & Dirty-Driven Citro3D Bottom Renderer
// ============================================================================
#ifdef DAEDALUS_CTR

namespace TouchController
{

namespace
{

TouchPadEngine s_engine;
const char * const kTouchConfigPath = DAEDALUS_CTR_PATH("touch_config.ini");
char     s_gameName[96] = "rom";
int      s_activeSlot   = 0;
char     s_toast[64]    = "";
int      s_toastFrames  = 0;
u64      s_lastHudTick  = 0;
u64      s_lastBatTick  = 0;
uint8_t  s_batteryLevel = 4;
bool     s_charging     = false;
bool     s_sessionOpen  = false;
EAudioPluginMode s_savedAudioMode = APM_ENABLED_ASYNC;

void RefreshBatteryIfDue(u64 now)
{
	if (s_lastBatTick != 0 && (now - s_lastBatTick) < (u64)SYSCLOCK_ARM11 * 10ULL)
		return;
	s_lastBatTick = now;
	if (R_SUCCEEDED(ptmuInit()))
	{
		u8 lvl = 4, chg = 0;
		if (R_SUCCEEDED(PTMU_GetBatteryLevel(&lvl)))
			s_batteryLevel = lvl;
		if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&chg)))
			s_charging = (chg != 0);
		ptmuExit();
	}
}

void DrawBottomHudNow()
{
	using namespace UI::Renderer;
	const ThemePalette & t = ActiveTheme();
	const TouchPadConfig & cfg = s_engine.GetConfig();

	// 1. Sleep / Battery Saver mode: 100% pure black + whisper-dim wake hint
	if (cfg.layout == TOUCH_LAYOUT_SLEEP)
	{
		Quad(0.0f, 0.0f, 320.0f, 240.0f, RGBA(0, 0, 0, 255));
		TextCentered(FONT_BODY, 160.0f, 114.0f, "Tap anywhere to show controls", RGBA(255, 255, 255, 48), 0.88f);
		return;
	}

	// 2. Opaque matte background quad (replaces GX_MemoryFill so zero C3D_FrameSplit occurs!)
	Quad(0.0f, 0.0f, 320.0f, 240.0f, t.bg);

	// 3. Compact Top HUD Strip (y = 0..27)
	Quad(0.0f, 0.0f, 320.0f, 27.0f, t.headerBg);
	Quad(0.0f, 27.0f, 320.0f, 1.0f, t.headerRule);

	// Live FPS pill
	char fpsBuf[20];
	const float fps = gCurrentFramerate;
	snprintf(fpsBuf, sizeof(fpsBuf), "%.1f FPS", fps);
	const uint32_t fpsCol = (fps >= 19.0f) ? t.n64Green : ((fps >= 13.0f) ? t.n64Yellow : t.n64Red);
	RoundedRect(6.0f, 4.0f, 62.0f, 19.0f, 6.0f, WithAlpha(fpsCol, 34));
	RoundedBorder1(6.0f, 4.0f, 62.0f, 19.0f, 6.0f, fpsCol);
	TextCentered(FONT_BODY, 37.0f, 7.0f, fpsBuf, fpsCol, 0.82f);

	// Mute toggle icon button (x = 72..102)
	const bool muted = (gAudioPluginEnabled == APM_DISABLED);
	const int topBtn = s_engine.PressedTopBarBtn();
	RoundedRect(72.0f, 4.0f, 30.0f, 19.0f, 6.0f,
	            (topBtn == 0) ? t.focusFill : (muted ? WithAlpha(t.n64Red, 36) : t.cardBgAlt));
	Icon(muted ? ICON_AUDIO_MUTE : ICON_AUDIO, 80.0f, 6.5f, 14.0f,
	     muted ? t.n64Red : t.accent);

	// Battery icon (x = 109..129)
	DrawBatteryIcon(110.0f, 9.0f, s_batteryLevel, s_charging, t.textMuted);

	// Mode switch pill (x = 146..234)
	RoundedRect(146.0f, 4.0f, 88.0f, 19.0f, 6.0f, (topBtn == 1) ? t.focusFill : t.cardBg);
	RoundedBorder1(146.0f, 4.0f, 88.0f, 19.0f, 6.0f, (topBtn == 1) ? t.focusBorder : t.cardBorder);
	Icon(ICON_CONTROLS, 152.0f, 6.5f, 14.0f, t.accent);
	Text(FONT_BODY, 170.0f, 7.0f, TouchPadEngine::LayoutShortName(cfg.layout), t.text, 0.82f);

	// Sleep / Moon button (x = 238..268)
	RoundedRect(238.0f, 4.0f, 30.0f, 19.0f, 6.0f, (topBtn == 2) ? t.focusFill : t.cardBg);
	RoundedBorder1(238.0f, 4.0f, 30.0f, 19.0f, 6.0f, (topBtn == 2) ? t.focusBorder : t.cardBorder);
	Icon(ICON_MOON, 246.0f, 6.5f, 14.0f, t.textMuted);

	// Pause Menu button (x = 272..314)
	RoundedRect(272.0f, 4.0f, 42.0f, 19.0f, 6.0f, (topBtn == 3) ? t.focusFill : t.cardBg);
	RoundedBorder1(272.0f, 4.0f, 42.0f, 19.0f, 6.0f, (topBtn == 3) ? t.focusBorder : t.cardBorder);
	Icon(ICON_MENU, 286.0f, 6.5f, 14.0f, t.text);

	// 4. Auxiliary Column: Dedicated Z Trigger, Z-Lock, Quick Save (QS), Quick Load (QL)
	const DeckLayout deck = ComputeDeck(cfg.leftHanded);
	const bool zAct = s_engine.IsZActive();
	DrawCard(deck.zX, deck.zY, deck.zW, deck.zH, zAct, false, t, 12.0f);
	TextCentered(FONT_TITLE, deck.zX + deck.zW * 0.5f, deck.zY + 18.0f, "Z",
	             zAct ? t.accent : t.text, 1.55f);
	TextCentered(FONT_BODY, deck.zX + deck.zW * 0.5f, deck.zY + 52.0f,
	             s_engine.IsZLatched() ? "LOCKED" : "TRIGGER",
	             s_engine.IsZLatched() ? t.n64Green : t.textDim, 0.80f);

	// Z-Lock toggle pill
	const bool zLock = s_engine.IsZLatched();
	DrawCard(deck.zLockX, deck.zLockY, deck.zLockW, deck.zLockH, zLock, false, t, 8.0f);
	Circle(deck.zLockX + 14.0f, deck.zLockY + deck.zLockH * 0.5f, 4.0f,
	       zLock ? t.n64Green : t.textDim);
	Text(FONT_BODY, deck.zLockX + 24.0f, deck.zLockY + 6.5f, "Z-Lock",
	     zLock ? t.text : t.textMuted, 0.86f);

	// QS & QL buttons with hold-to-confirm fill bar
	auto DrawQuickBtn = [&](float x, float y, float w, float h, EIconId icon,
	                        const char * label, float prog) {
		DrawCard(x, y, w, h, prog > 0.01f, false, t, 9.0f);
		if (prog > 0.01f)
		{
			const float fillW = (w - 8.0f) * prog;
			RoundedRect(x + 4.0f, y + h - 8.0f, fillW, 4.0f, 2.0f, t.accent);
		}
		Icon(icon, x + (w - 16.0f) * 0.5f, y + 8.0f, 16.0f, prog > 0.01f ? t.accent : t.textMuted);
		TextCentered(FONT_BODY, x + w * 0.5f, y + 29.0f, label, t.text, 0.84f);
	};
	DrawQuickBtn(deck.qsX, deck.qsY, deck.qsW, deck.qsH, ICON_SAVE, "Save", s_engine.QuickSaveProgress());
	DrawQuickBtn(deck.qlX, deck.qlY, deck.qlW, deck.qlH, ICON_LOAD, "Load", s_engine.QuickLoadProgress());

	// 5. Right (or mirrored) Primary Control Cluster: C-Cross / C-Stick / Shooter / D-Pad
	const float cx = deck.padCx;
	const float cy = deck.padCy;
	const uint32_t dirs = s_engine.ActiveDirMask();

	// Recessed circular well behind the camera/C cluster
	Circle(cx, cy + 2.0f, 84.0f, t.cardShadow);
	Circle(cx, cy, 83.0f, t.cardBg);
	Ring(cx, cy, 82.0f, s_engine.IsStickCaptured() ? WithAlpha(t.accent, 180) : t.cardBorder);

	if (cfg.layout == TOUCH_LAYOUT_CCROSS)
	{
		// Authentic 4-way N64 Yellow C-Buttons Cross (User's preferred default!)
		struct CBtnDef { float ox, oy; uint32_t mask; EIconId arrow; };
		static const CBtnDef kCBtns[4] = {
			{   0.0f, -49.0f, N64Mask::kCUp,    ICON_ARROW_UP    },
			{   0.0f,  49.0f, N64Mask::kCDown,  ICON_ARROW_DOWN  },
			{ -49.0f,   0.0f, N64Mask::kCLeft,  ICON_ARROW_LEFT  },
			{  49.0f,   0.0f, N64Mask::kCRight, ICON_ARROW_RIGHT },
		};
		// Subtle center "C" emblem
		Circle(cx, cy, 15.0f, t.cardBgAlt);
		TextCentered(FONT_BODY, cx, cy - 6.0f, "C", t.textDim, 0.90f);

		for (int i = 0; i < 4; ++i)
		{
			const float bx = cx + kCBtns[i].ox;
			const float by = cy + kCBtns[i].oy;
			const bool active = (dirs & kCBtns[i].mask) != 0;
			const uint32_t fillCol = active ? RGBA(255, 224, 72) : t.n64Yellow;

			Circle(bx, by + 2.0f, 26.0f, t.cardShadow);
			if (active)
				Circle(bx, by, 29.0f, WithAlpha(t.focusBorder, 120));
			Circle(bx, by, 25.5f, fillCol);
			Ring(bx, by, 25.0f, active ? RGBA(255, 255, 255, 230) : RGBA(185, 138, 12, 160));
			Icon(kCBtns[i].arrow, bx - 9.0f, by - 9.0f, 18.0f,
			     active ? RGBA(255, 255, 255) : RGBA(110, 78, 8, 220));
		}
	}
	else if (cfg.layout == TOUCH_LAYOUT_CSTICK || cfg.layout == TOUCH_LAYOUT_DUAL_SHOOTER)
	{
		// Smooth Virtual Analog Stick (C-Stick Camera OR Dual-Analog Shooter)
		const bool isShooter = (cfg.layout == TOUCH_LAYOUT_DUAL_SHOOTER);
		const uint32_t ringCol = isShooter ? t.accent : t.n64Yellow;

		// Inner track + cardinal direction pips
		Circle(cx, cy, 62.0f, t.cardBgAlt);
		Ring(cx, cy, 62.0f, WithAlpha(ringCol, s_engine.IsStickCaptured() ? 220 : 110));

		Icon(ICON_ARROW_UP,    cx - 7.0f,  cy - 76.0f, 14.0f, (dirs & N64Mask::kCUp)    ? ringCol : t.textDim);
		Icon(ICON_ARROW_DOWN,  cx - 7.0f,  cy + 62.0f, 14.0f, (dirs & N64Mask::kCDown)  ? ringCol : t.textDim);
		Icon(ICON_ARROW_LEFT,  cx - 76.0f, cy - 7.0f,  14.0f, (dirs & N64Mask::kCLeft)  ? ringCol : t.textDim);
		Icon(ICON_ARROW_RIGHT, cx + 62.0f, cy - 7.0f,  14.0f, (dirs & N64Mask::kCRight) ? ringCol : t.textDim);

		// Floating thumbstick knob
		const float kx = cx + s_engine.StickVisX() * 44.0f;
		const float ky = cy - s_engine.StickVisY() * 44.0f;
		Circle(kx, ky + 3.0f, 27.0f, t.cardShadow);
		if (s_engine.IsStickCaptured())
			Circle(kx, ky, 30.0f, WithAlpha(ringCol, 90));
		Circle(kx, ky, 26.0f, s_engine.IsStickCaptured() ? ringCol : t.cardBg);
		Ring(kx, ky, 25.0f, s_engine.IsStickCaptured() ? RGBA(255, 255, 255, 220) : ringCol);
		TextCentered(FONT_BODY, kx, ky - 6.0f, isShooter ? "AIM" : "C",
		             s_engine.IsStickCaptured() ? RGBA(20, 24, 30) : t.textMuted, 0.88f);
	}
	else if (cfg.layout == TOUCH_LAYOUT_DPAD)
	{
		// Classic N64 D-Pad Cross
		const uint32_t baseCol = t.isLight ? RGBA(108, 114, 126) : RGBA(72, 78, 90);
		RoundedRect(cx - 21.0f, cy - 66.0f, 42.0f, 132.0f, 8.0f, baseCol);
		RoundedRect(cx - 66.0f, cy - 21.0f, 132.0f, 42.0f, 8.0f, baseCol);
		Circle(cx, cy, 12.0f, WithAlpha(t.bg, 120));

		if (dirs & N64Mask::kDUp)    RoundedRect(cx - 19.0f, cy - 64.0f, 38.0f, 42.0f, 6.0f, t.accent);
		if (dirs & N64Mask::kDDown)  RoundedRect(cx - 19.0f, cy + 22.0f, 38.0f, 42.0f, 6.0f, t.accent);
		if (dirs & N64Mask::kDLeft)  RoundedRect(cx - 64.0f, cy - 19.0f, 42.0f, 38.0f, 6.0f, t.accent);
		if (dirs & N64Mask::kDRight) RoundedRect(cx + 22.0f, cy - 19.0f, 42.0f, 38.0f, 6.0f, t.accent);

		Icon(ICON_ARROW_UP,    cx - 8.0f,  cy - 52.0f, 16.0f, RGBA(240, 244, 250, 220));
		Icon(ICON_ARROW_DOWN,  cx - 8.0f,  cy + 36.0f, 16.0f, RGBA(240, 244, 250, 220));
		Icon(ICON_ARROW_LEFT,  cx - 52.0f, cy - 8.0f,  16.0f, RGBA(240, 244, 250, 220));
		Icon(ICON_ARROW_RIGHT, cx + 36.0f, cy - 8.0f,  16.0f, RGBA(240, 244, 250, 220));
	}

	// 6. Subtle Bottom Status / Toast Strip (y = 220..240)
	if (s_toastFrames > 0 && s_toast[0])
	{
		RoundedRect(54.0f, 217.0f, 212.0f, 20.0f, 8.0f, WithAlpha(t.hintBg, 235));
		RoundedBorder1(54.0f, 217.0f, 212.0f, 20.0f, 8.0f, t.accent);
		TextCentered(FONT_BODY, 160.0f, 220.5f, s_toast, t.hintText, 0.82f);
	}
	else
	{
		char foot[64];
		snprintf(foot, sizeof(foot), "Slot %d  |  Hold Save/Load 0.35s  |  SELECT: Pause", s_activeSlot);
		TextCentered(FONT_BODY, 160.0f, 223.0f, foot, t.textDim, 0.78f);
	}
}

} // namespace

void Init()
{
	LoadConfig();
}

void LoadConfig()
{
	TouchPadConfig cfg = s_engine.GetConfig();
	FILE * f = fopen(kTouchConfigPath, "r");
	if (!f) return;
	char line[128];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "layout=", 7))
		{
			int v = atoi(line + 7);
			if (v >= 0 && v < TOUCH_LAYOUT_COUNT)
				cfg.layout = (ETouchPadLayout)v;
		}
		else if (!strncmp(line, "left_handed=", 12))
			cfg.leftHanded = (atoi(line + 12) != 0);
		else if (!strncmp(line, "z_toggle=", 9))
			cfg.zToggleMode = (atoi(line + 9) != 0);
		else if (!strncmp(line, "deadzone=", 9))
			cfg.stickDeadzone = (float)atof(line + 9);
		else if (!strncmp(line, "sens=", 5))
			cfg.stickSens = (float)atof(line + 5);
		else if (!strncmp(line, "invert_y=", 9))
			cfg.invertY = (atoi(line + 9) != 0);
	}
	fclose(f);
	s_engine.SetConfig(cfg);
}

void SaveConfig()
{
	const TouchPadConfig & cfg = s_engine.GetConfig();
	FILE * f = fopen(kTouchConfigPath, "w");
	if (!f) return;
	fprintf(f, "layout=%d\nleft_handed=%d\nz_toggle=%d\ndeadzone=%.2f\nsens=%.2f\ninvert_y=%d\n",
	        (int)cfg.layout, cfg.leftHanded ? 1 : 0, cfg.zToggleMode ? 1 : 0,
	        cfg.stickDeadzone, cfg.stickSens, cfg.invertY ? 1 : 0);
	fclose(f);
}

void BeginSession(const char * gameName)
{
	snprintf(s_gameName, sizeof(s_gameName), "%s", (gameName && gameName[0]) ? gameName : "rom");
	s_engine.ResetSession();
	s_toast[0] = '\0';
	s_toastFrames = 0;
	s_lastHudTick = 0;
	s_sessionOpen = true;
	if (gAudioPluginEnabled != APM_DISABLED)
		s_savedAudioMode = gAudioPluginEnabled;
	UI::Renderer::EnsureHudPool();
}

void EndSession()
{
	s_sessionOpen = false;
	UI::Renderer::ReleaseHudPool();
}

void PollInput(uint32_t keysHeld, TouchPadOutput & out)
{
	if (!s_sessionOpen)
	{
		std::memset(&out, 0, sizeof(out));
		return;
	}

	const bool touchHeld = (keysHeld & KEY_TOUCH) != 0;
	int px = 0, py = 0;
	if (touchHeld)
	{
		touchPosition tp;
		hidTouchRead(&tp);
		px = tp.px;
		py = tp.py;
	}

	const ETouchPadLayout prevLayout = s_engine.GetConfig().layout;
	const bool prevZLatched = s_engine.IsZLatched();

	s_engine.Update(touchHeld, px, py, out);

	const ETouchPadLayout curLayout = s_engine.GetConfig().layout;
	if (curLayout != prevLayout)
	{
		if (curLayout == TOUCH_LAYOUT_SLEEP)
			Sound::Play(SFX_TOGGLE_OFF);
		else if (prevLayout == TOUCH_LAYOUT_SLEEP)
			Sound::Play(SFX_TOGGLE_ON);
		else
			Sound::Play(SFX_ADJUST, 1.04f);
	}
	if (s_engine.IsZLatched() != prevZLatched)
	{
		Sound::Play(s_engine.IsZLatched() ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
	}

	if (out.requestMuteToggle)
	{
		if (gAudioPluginEnabled == APM_DISABLED)
		{
			gAudioPluginEnabled = (s_savedAudioMode != APM_DISABLED) ? s_savedAudioMode : APM_ENABLED_ASYNC;
			Sound::Play(SFX_TOGGLE_ON);
			ShowToast("Audio On", 60);
		}
		else
		{
			s_savedAudioMode = gAudioPluginEnabled;
			gAudioPluginEnabled = APM_DISABLED;
			Sound::Play(SFX_TOGGLE_OFF);
			ShowToast("Audio Muted", 60);
		}
	}

	if (out.requestQuickSave)
	{
		char path[256];
		snprintf(path, sizeof(path), "%s%s.ss%d",
		         DAEDALUS_CTR_PATH("SaveStates/"), s_gameName, s_activeSlot);
		CPU_RequestSaveState(path);
		Sound::Play(SFX_SAVE_STATE);
		char msg[48];
		snprintf(msg, sizeof(msg), "Saved to Slot %d", s_activeSlot);
		ShowToast(msg, 90);
	}

	if (out.requestQuickLoad)
	{
		char path[256];
		snprintf(path, sizeof(path), "%s%s.ss%d",
		         DAEDALUS_CTR_PATH("SaveStates/"), s_gameName, s_activeSlot);
		if (IO::File::Exists(path))
		{
			CPU_RequestLoadState(path);
			Sound::Play(SFX_LOAD_STATE);
			char msg[48];
			snprintf(msg, sizeof(msg), "Loaded Slot %d", s_activeSlot);
			ShowToast(msg, 90);
		}
		else
		{
			Sound::Play(SFX_ERROR);
			char msg[48];
			snprintf(msg, sizeof(msg), "Slot %d is empty", s_activeSlot);
			ShowToast(msg, 75);
		}
	}
}

void RenderBottomIfDirty()
{
	if (!s_sessionOpen)
		return;

	const u64 now = svcGetSystemTick();
	RefreshBatteryIfDue(now);

	if (s_toastFrames > 0)
	{
		--s_toastFrames;
		if (s_toastFrames == 0)
			s_engine.MarkDirty();
	}

	// Periodic 2 Hz refresh for live FPS / battery when screen is awake (0 Hz in Sleep!)
	if (!s_engine.IsDirty())
	{
		if (s_engine.GetConfig().layout == TOUCH_LAYOUT_SLEEP)
			return;
		if (s_lastHudTick != 0 && (now - s_lastHudTick) < (u64)(SYSCLOCK_ARM11 / 2))
			return;
	}

	if (!UI::Renderer::BeginHudBottomPass())
		return;

	DrawBottomHudNow();
	UI::Renderer::EndHudBottomPass();

	// Restore N64 RendererCTR state cache for the next game frame
	if (gRendererCTR != nullptr)
		gRendererCTR->RestoreRenderStates();

	s_engine.ClearDirty();
	s_lastHudTick = now;
}

void MarkDirty()
{
	s_engine.MarkDirty();
}

void ShowToast(const char * msg, int frames)
{
	snprintf(s_toast, sizeof(s_toast), "%s", msg ? msg : "");
	s_toastFrames = frames;
	s_engine.MarkDirty();
}

int GetActiveSlot()
{
	return s_activeSlot;
}

void SetActiveSlot(int slot)
{
	if (slot < 0) slot = 0;
	if (slot > 4) slot = 4;
	s_activeSlot = slot;
	s_engine.MarkDirty();
}

TouchPadEngine & Engine()
{
	return s_engine;
}

} // namespace TouchController

#endif // DAEDALUS_CTR

} // namespace UI
