#pragma once

#include <stdint.h>

namespace UI
{

struct Rect
{
	float x;
	float y;
	float w;
	float h;

	bool Contains(float px, float py) const
	{
		return px >= x && px < (x + w) && py >= y && py < (y + h);
	}
};

// Pure, host-testable 3DS touch state machine:
// - Filters spurious (0,0) hardware frames during press/release transitions.
// - Distinguishes clean taps (triggered on release) from drags (8px threshold).
// - Tracks vertical scroll deltas, kinetic inertia velocity, and hold frames
//   (for hold-to-confirm quick save / quick load in Phase 2).
struct TouchTracker
{
	bool  active;
	bool  justPressed;
	bool  justReleased;
	bool  dragged;
	bool  scrollbarDrag;
	int   startX;
	int   startY;
	int   curX;
	int   curY;
	int   prevX;
	int   prevY;
	int   releaseX;
	int   releaseY;
	float deltaX;
	float deltaY;
	float velY;
	int   holdFrames;

	void Reset();

	// Platform-independent update (testable on host without libctru).
	void UpdateRaw(bool touchHeld, int px, int py);

	bool TappedIn(const Rect & r) const
	{
		return justReleased && !dragged && !scrollbarDrag &&
		       r.Contains((float)releaseX, (float)releaseY);
	}

	bool PressedIn(const Rect & r) const
	{
		return active && !dragged && !scrollbarDrag &&
		       r.Contains((float)curX, (float)curY);
	}

	float HoldProgress(int targetFrames) const
	{
		if (!active || targetFrames <= 0) return 0.0f;
		float p = (float)holdFrames / (float)targetFrames;
		if (p < 0.0f) p = 0.0f;
		if (p > 1.0f) p = 1.0f;
		return p;
	}
};

// Key-repeat helper for D-Pad / Circle Pad menu navigation.
struct RepeatTimer
{
	int timer[4];

	void Reset();
	bool Poll(uint32_t down, uint32_t held, uint32_t mask, int slot);
};

} // namespace UI
