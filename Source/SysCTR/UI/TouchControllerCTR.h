#pragma once

#include <stdint.h>

namespace UI
{

// Bottom-screen physical-extension layouts during N64 gameplay.
enum ETouchPadLayout
{
	TOUCH_LAYOUT_CCROSS = 0,   // Default: 4-way N64 Yellow C-Buttons Cross + Z trigger + QS/QL
	TOUCH_LAYOUT_CSTICK,       // Virtual Analog C-Stick (360° camera -> 8-way C-Buttons)
	TOUCH_LAYOUT_DUAL_SHOOTER, // Dual-Analog Shooter: Touch Stick -> N64 Main Stick, Circle Pad -> C-Buttons
	TOUCH_LAYOUT_DPAD,         // Virtual N64 D-Pad Cross (no Y+D-Pad combo needed on O3DS)
	TOUCH_LAYOUT_SLEEP,        // Pure black battery-saver screen (tap anywhere to wake)
	TOUCH_LAYOUT_COUNT
};

struct TouchPadConfig
{
	ETouchPadLayout layout;
	bool            leftHanded;     // Mirrors control deck horizontally
	bool            zToggleMode;    // When true, tapping Z latches/unlatches Z trigger
	float           stickDeadzone;  // Radial deadzone for C-Stick / Shooter (0.15..0.40)
	float           stickSens;      // Sensitivity scale for Shooter analog stick (0.5..1.8)
	bool            invertY;        // Invert Y axis on virtual camera/shooter stick
};

// Pure N64 button bitmasks matching OSContPad::button (Core/PIF.h) so the core
// touch controller engine can be unit-tested on host without libctru.
namespace N64Mask
{
	constexpr uint32_t kCUp    = 0x0008u;
	constexpr uint32_t kCDown  = 0x0004u;
	constexpr uint32_t kCLeft  = 0x0002u;
	constexpr uint32_t kCRight = 0x0001u;
	constexpr uint32_t kLTrig  = 0x0020u;
	constexpr uint32_t kRTrig  = 0x0010u;
	constexpr uint32_t kDUp    = 0x0800u;
	constexpr uint32_t kDDown  = 0x0400u;
	constexpr uint32_t kDLeft  = 0x0200u;
	constexpr uint32_t kDRight = 0x0100u;
	constexpr uint32_t kZTrig  = 0x2000u;
}

struct TouchPadOutput
{
	uint32_t n64Buttons;
	int8_t   analogX;          // [-80, +80] when swapSticks is true
	int8_t   analogY;          // [-80, +80] when swapSticks is true
	bool     swapSticks;       // True in TOUCH_LAYOUT_DUAL_SHOOTER
	bool     requestPause;     // One-shot pulse
	bool     requestQuickSave; // One-shot pulse (after 0.35s hold)
	bool     requestQuickLoad; // One-shot pulse (after 0.35s hold)
	bool     requestMuteToggle;// One-shot pulse
};

// Host-testable pure state machine for the in-game bottom-screen controller.
class TouchPadEngine
{
public:
	static constexpr int kQuickHoldFrames = 21; // ~0.35s hold guard for QS / QL

	TouchPadEngine();

	void ResetSession();
	void SetConfig(const TouchPadConfig & cfg);
	const TouchPadConfig & GetConfig() const { return mCfg; }
	TouchPadConfig &       MutableConfig()   { return mCfg; }

	// Step once per input poll. Returns true if visual state changed (needs bottom redraw).
	bool Update(bool touchHeld, int px, int py, TouchPadOutput & out);

	// Visual state accessors (used by the Citro3D renderer)
	bool  IsDirty() const { return mDirty; }
	void  MarkDirty()     { mDirty = true; }
	void  ClearDirty()    { mDirty = false; }

	bool  IsZActive() const      { return mZHeld || mZLatched; }
	bool  IsZLatched() const     { return mZLatched; }
	bool  IsStickCaptured() const{ return mStickCaptured; }
	float StickVisX() const      { return mStickVisX; } // [-1, 1]
	float StickVisY() const      { return mStickVisY; } // [-1, 1]
	uint32_t ActiveDirMask() const { return mActiveDirs; }
	float QuickSaveProgress() const { return mQsProgress; }
	float QuickLoadProgress() const { return mQlProgress; }
	int   PressedTopBarBtn() const  { return mPressedTopBtn; } // -1 none, 0 mute, 1 mode, 2 sleep, 3 menu

	static const char * LayoutName(ETouchPadLayout l);
	static const char * LayoutShortName(ETouchPadLayout l);

private:
	TouchPadConfig  mCfg;
	ETouchPadLayout mWakeLayout;
	bool            mDirty;
	bool            mPrevHeld;
	int             mLastX;
	int             mLastY;
	bool            mZHeld;
	bool            mZLatched;
	bool            mStickCaptured;
	float           mStickVisX;
	float           mStickVisY;
	uint32_t        mActiveDirs;
	int             mQsHoldFrames;
	int             mQlHoldFrames;
	bool            mQsFired;
	bool            mQlFired;
	float           mQsProgress;
	float           mQlProgress;
	int             mPressedTopBtn;
};

#ifdef DAEDALUS_CTR
namespace TouchController
{
	void Init();
	void LoadConfig();
	void SaveConfig();

	void BeginSession(const char * gameName);
	void EndSession();

	// Called from InputManagerCTR::GetState() every input sample.
	void PollInput(uint32_t keysHeld, TouchPadOutput & out);

	// Called from GraphicsContextCTR::UpdateFrame() right before EndGameFrame().
	// Zero GPU/DMA cost when the bottom screen is clean!
	void RenderBottomIfDirty();

	void MarkDirty();
	void ShowToast(const char * msg, int frames = 90);
	int  GetActiveSlot();
	void SetActiveSlot(int slot);

	TouchPadEngine & Engine();
} // namespace TouchController
#endif

} // namespace UI
