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
#include "Utility/Thread.h"
#include "Utility/Timing.h"

#include <3ds.h>
#include <new>
#include <limits>

static const int	gThreadPriorities[ TP_NUM_PRIORITIES ] =
{
	0x1b,		// TP_LOW
	0x1a,		// TP_NORMAL
	0x19,		// TP_HIGH
	0x18,		// TP_TIME_CRITICAL (highest userland priority)
};

const ThreadHandle	kInvalidThreadHandle = -1;

struct SDaedThreadDetails
{
	SDaedThreadDetails( DaedThread function, void * argument )
		:	ThreadFunction( function )
		,	Argument( argument )
	{
	}

	DaedThread		ThreadFunction;
	void *			Argument;
};

// The real thread is passed in as an argument. We call it and return the result
static void StartThreadFunc(  void *argp )
{
	SDaedThreadDetails * thread_details( static_cast< SDaedThreadDetails * >( argp ) );
	DaedThread function = thread_details->ThreadFunction;
	void * argument = thread_details->Argument;
	delete thread_details;
	function( argument );
}

ThreadHandle CreateThread( const char * name, DaedThread function, void * argument )
{
	(void)name; // libctru does not expose a thread naming API.
	SDaedThreadDetails * thread_details = new (std::nothrow) SDaedThreadDetails( function, argument );
	if (!thread_details)
		return kInvalidThreadHandle;

	Thread thid = threadCreate(StartThreadFunc, thread_details, 0x10000, gThreadPriorities[TP_NORMAL], -2, false);
	if (!thid)
		delete thread_details;

	return thid ? (ThreadHandle)thid : kInvalidThreadHandle;
}

void SetThreadPriority( ThreadHandle handle, EThreadPriority pri )
{
	if (handle != kInvalidThreadHandle && pri >= TP_LOW && pri < TP_NUM_PRIORITIES)
		svcSetThreadPriority(threadGetHandle((Thread)handle), gThreadPriorities[pri]);
}

void ReleaseThreadHandle( ThreadHandle handle )
{
	if (handle != kInvalidThreadHandle)
		threadFree((Thread)handle);
}


// Wait the specified time for the thread to finish.
// Returns false if the thread didn't terminate
bool JoinThread( ThreadHandle handle, s32 timeout )
{
	const u64 timeout_ns = timeout < 0 ? std::numeric_limits<u64>::max() :
		(u64)timeout * 1000000ULL;
	if (handle == kInvalidThreadHandle)
		return false;

	Result ret = threadJoin((Thread)handle, timeout_ns);

	return (ret >= 0);
}

void ThreadSleepMs( u32 ms )
{
	svcSleepThread( (s64)ms * 1000000LL ); // libctru expects nanoseconds
}

void ThreadSleepTicks( u32 ticks )
{
	// ticks is 32-bit, so this multiplication fits in 64 bits. Avoid the
	// software double-precision conversion formerly used on every frame.
	svcSleepThread( (u64)ticks * 1000000000ULL / SYSCLOCK_ARM11 );
}

void ThreadYield()
{
	svcSleepThread( 1 );				// Is 0 valid?
}
