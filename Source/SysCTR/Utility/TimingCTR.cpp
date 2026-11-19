/*
Copyright (C) 2005 StrmnNrmn

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "stdafx.h"
#include "Utility/Timing.h"

#include <3ds.h>

namespace NTiming {

bool GetPreciseFrequency( u64 * p_freq )
{
	*p_freq = SYSCLOCK_ARM11;
	return true;
}

bool GetPreciseTime( u64 * p_time )
{
	*p_time = svcGetSystemTick();
	return true;
}

u64 ToMilliseconds( u64 ticks )
{
	// Divide first to avoid overflowing when converting a long tick interval.
	return (ticks / SYSCLOCK_ARM11) * 1000ULL +
		(ticks % SYSCLOCK_ARM11) * 1000ULL / SYSCLOCK_ARM11;
}

} // NTiming
