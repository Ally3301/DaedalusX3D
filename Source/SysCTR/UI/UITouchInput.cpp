#include "UITouchInput.h"

#include <cstring>

namespace UI
{

void TouchTracker::Reset()
{
	std::memset(this, 0, sizeof(*this));
}

void TouchTracker::UpdateRaw(bool touchHeld, int px, int py)
{
	justPressed  = false;
	justReleased = false;
	deltaX       = 0.0f;
	deltaY       = 0.0f;

	if (touchHeld)
	{
		const bool validCoord = (px > 0 || py > 0) && px >= 0 && px < 320 && py >= 0 && py < 240;
		if (!active)
		{
			if (validCoord)
			{
				active        = true;
				justPressed   = true;
				dragged       = false;
				scrollbarDrag = false;
				startX = curX = prevX = px;
				startY = curY = prevY = py;
				velY          = 0.0f;
				holdFrames    = 1;
			}
		}
		else
		{
			++holdFrames;
			if (validCoord)
			{
				prevX  = curX;
				prevY  = curY;
				curX   = px;
				curY   = py;
				const int dx = curX - startX;
				const int dy = curY - startY;
				if (!dragged && (dx * dx + dy * dy >= 64)) // 8px drag threshold
					dragged = true;
				deltaX = (float)(curX - prevX);
				deltaY = (float)(curY - prevY);
				velY   = velY * 0.5f + deltaY * 0.5f;
			}
		}
	}
	else
	{
		if (active)
		{
			justReleased = true;
			releaseX     = curX;
			releaseY     = curY;
		}
		active        = false;
		scrollbarDrag = false;
		holdFrames    = 0;
	}
}

void RepeatTimer::Reset()
{
	std::memset(this, 0, sizeof(*this));
}

bool RepeatTimer::Poll(uint32_t down, uint32_t held, uint32_t mask, int slot)
{
	if (slot < 0 || slot >= 4)
		return false;
	if (down & mask)
	{
		timer[slot] = 0;
		return true;
	}
	if (held & mask)
	{
		if (++timer[slot] > 14 && (timer[slot] - 14) % 4 == 0)
			return true;
	}
	else
	{
		timer[slot] = 0;
	}
	return false;
}

} // namespace UI
