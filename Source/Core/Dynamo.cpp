/*
Copyright (C) 2009 StrmnNrmn

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
#include "Dynamo.h"

#include <stdio.h>

#include <algorithm>

#include "CPU.h"
#include "Registers.h"					// For REG_?? defines
#include "Memory.h"
#include "Interrupt.h"
#include "R4300.h"

#include "Config/ConfigOptions.h"
#include "Debug/DBGConsole.h"
#include "Debug/DebugLog.h"
#include "DynaRec/DynaRecProfile.h"
#include "DynaRec/Fragment.h"
#include "DynaRec/FragmentCache.h"
#include "DynaRec/CodeBufferManager.h"
#include "DynaRec/TraceRecorder.h"
#include "OSHLE/patch.h"				// GetCorrectOp
#include "OSHLE/ultra_R4300.h"
#include "Utility/IO.h"
#include "Utility/Macros.h"
#include "Utility/Profiler.h"
#include "Utility/Synchroniser.h"
#ifdef DAEDALUS_CTR_XRAY
#include "SysCTR/Utility/PerfProfilerCTR.h"
#endif

#ifdef DAEDALUS_ENABLE_DYNAREC

// These values are very sensitive to change in some games so be carefull!!! //Corn
// War God is sensitive to gHotTraceThreshold
// PD is sensitive to gMaxHotTraceMapSize
//
// Banjo Tooie needs a larger cache size
// BUT leave PSP cache size untouched for now
#ifdef DAEDALUS_PSP
#define TRACE_SIZE 512
#else
#define TRACE_SIZE 1024
#endif

static const u32					gMaxFragmentCacheSize {(8192 + 1024)}; //Maximum amount of fragments in the cache
static const u32					gMaxHotTraceMapSize {(2048 + TRACE_SIZE)};
static const u32					gHotTraceThreshold {10};	//How many times interpreter has to loop a trace before it becomes hot and sent to dynarec

#if defined(DAEDALUS_CTR) && !defined(DAEDALUS_DEBUG_DYNAREC) && !defined(DAEDALUS_ENABLE_DYNAREC_PROFILE)
// Fixed-size open-addressed hash table for hot-trace counters on 3DS.
// Eliminates per-node heap allocations and red-black tree traversal on every
// branch target before fragments become hot, avoiding heap fragmentation on
// Old 3DS.
class CHotTraceCountTable
{
	static const u32 kCapacity = 4096; // Power of 2, > gMaxHotTraceMapSize (3072)
	static const u32 kMask     = kCapacity - 1;
	static const u32 kEmpty    = 0xFFFFFFFFu;
	static const u32 kDeleted  = 0xFFFFFFFEu;

	struct Entry
	{
		u32 addr;
		u32 count;
	};

	Entry mEntries[kCapacity];
	u32   mCount;
	u32   mUsedSlots; // live + tombstones

	static inline u32 Hash(u32 addr)
	{
		return ((addr >> 2) * 2654435761u) & kMask;
	}

public:
	CHotTraceCountTable() : mCount(0), mUsedSlots(0)
	{
		clear();
	}

	void clear()
	{
		for (u32 i = 0; i < kCapacity; ++i)
			mEntries[i].addr = kEmpty;
		mCount = 0;
		mUsedSlots = 0;
	}

	u32 size() const { return mCount; }

	u32 increment(u32 addr)
	{
		if (mUsedSlots >= kCapacity - 64)
			clear();

		u32 idx = Hash(addr);
		u32 first_deleted = kCapacity;
		for (u32 probe = 0; probe < kCapacity; ++probe)
		{
			Entry & e = mEntries[idx];
			if (e.addr == addr)
				return ++e.count;
			if (e.addr == kEmpty)
			{
				const u32 dst = (first_deleted < kCapacity) ? first_deleted : idx;
				if (first_deleted == kCapacity)
					++mUsedSlots;
				mEntries[dst].addr = addr;
				mEntries[dst].count = 1;
				++mCount;
				return 1;
			}
			if (e.addr == kDeleted && first_deleted == kCapacity)
				first_deleted = idx;
			idx = (idx + 1) & kMask;
		}
		clear();
		return increment(addr);
	}

	void erase(u32 addr)
	{
		u32 idx = Hash(addr);
		for (u32 probe = 0; probe < kCapacity; ++probe)
		{
			Entry & e = mEntries[idx];
			if (e.addr == addr)
			{
				e.addr = kDeleted;
				if (mCount > 0) --mCount;
				return;
			}
			if (e.addr == kEmpty)
				return;
			idx = (idx + 1) & kMask;
		}
	}
};

static CHotTraceCountTable			gHotTraceCountMap {};
#define HOT_TRACE_INCREMENT(pc)		gHotTraceCountMap.increment(pc)
#else
std::map< u32, u32 >				gHotTraceCountMap {};
#define HOT_TRACE_INCREMENT(pc)		(++gHotTraceCountMap[(pc)])
#endif
CFragmentCache						gFragmentCache {};
static bool							gResetFragmentCache {false};
static bool gResetFragmentCacheForCapacity {false};

struct SPendingInvalRange
{
	u32 addr;
	u32 len;
};
static const u32					kMaxPendingInvals = 8;
static SPendingInvalRange			gPendingInvals[kMaxPendingInvals];
static u32							gPendingInvalCount = 0;

#ifdef DAEDALUS_DEBUG_DYNAREC
std::map< u32, u32 >				gAbortedTraceReasons;

void								CPU_DumpFragmentCache();
#endif

static void							CPU_HandleDynaRecOnBranch( bool backwards, bool trace_already_enabled );
static void							CPU_UpdateTrace( u32 address, OpCode op_code, bool branch_delay_slot, bool branch_taken );
static void							CPU_CreateAndAddFragment();


#ifdef DAEDALUS_PROFILE_EXECUTION
u32 gFragmentLookupFailure {};
u32 gFragmentLookupSuccess {};
#endif

//*****************************************************************************
//	Indicate that the instruction cache is invalid
//	(we have to dump the dynarec contents and start over, but this is
//	better than crashing :) )
//*****************************************************************************
void R4300_CALL_TYPE CPU_InvalidateICache()
{
#ifdef DAEDALUS_CTR_XRAY
	static u32 sAllLogs = 0;
	if (sAllLogs < 24)
	{
		++sAllLogs;
		if (FILE * f = fopen("sdmc:/DaedalusX64.log", "a"))
		{
			fprintf(f, "  inval[ALL]: pc=0x%08lx a0=0x%08lx a1=0x%08lx frags=%lu\n",
				(unsigned long)gCPUState.CurrentPC,
				(unsigned long)gGPR[REG_a0]._u32_0,
				(unsigned long)gGPR[REG_a1]._u32_0,
				(unsigned long)gFragmentCache.GetCacheSize());
			fclose(f);
		}
	}
#endif
	CPU_ResetFragmentCache();
}

//*****************************************************************************
//
//*****************************************************************************
void CPU_DynarecEnable()
{
	gDynarecEnabled = true;
	gCPUState.AddJob(CPU_CHANGE_CORE);
}

//*****************************************************************************
// If fragments overlap dynarec has to start all over which is very costly
//*****************************************************************************
void R4300_CALL_TYPE CPU_InvalidateICacheRange( u32 address, u32 length )
{
	if( gFragmentCache.ShouldInvalidateOnWrite( address, length ) )
	{
#ifndef DAEDALUS_SILENT
		printf( "Write to %08lx (%lu bytes) overlaps fragment cache entries\n", address, length );
#endif
#ifdef DAEDALUS_CTR
		if (!gResetFragmentCache)
		{
			// Coalesce identical or adjacent pending invalidation ranges
			for (u32 i = 0; i < gPendingInvalCount; ++i)
			{
				const u32 s1 = gPendingInvals[i].addr;
				const u32 e1 = s1 + gPendingInvals[i].len;
				const u32 s2 = address;
				const u32 e2 = address + length;
				if (s2 <= e1 && e2 >= s1)
				{
					const u32 ns = (s1 < s2) ? s1 : s2;
					const u32 ne = (e1 > e2) ? e1 : e2;
					gPendingInvals[i].addr = ns;
					gPendingInvals[i].len  = ne - ns;
					return;
				}
			}
			if (gPendingInvalCount < kMaxPendingInvals)
			{
				gPendingInvals[gPendingInvalCount].addr = address;
				gPendingInvals[gPendingInvalCount].len  = length;
				++gPendingInvalCount;
				return;
			}
		}
#endif
		CPU_ResetFragmentCache();
	}
}


//*****************************************************************************
//	Execute a single MIPS op. The conditionals for the templated arguments
//	are completely optimised away by the compiler.
//
//	DynaRec:		Run this function with dynarec enabled
//	TranslateOp:	Use this to translate breakpoints/patches to original op
//					before execution.
//*****************************************************************************
template< bool TraceEnabled > DAEDALUS_FORCEINLINE void CPU_EXECUTE_OP()
{

	u8 * p_Instruction {};
	CPU_FETCH_INSTRUCTION( p_Instruction, gCPUState.CurrentPC );
	OpCode op_code = *(OpCode*)p_Instruction;

	// Cache instruction base pointer (used for SpeedHack() @ R4300.0)
	gLastAddress = p_Instruction;

#ifdef DAEDALUS_BREAKPOINTS_ENABLED
	op_code = GetCorrectOp( op_code );
#endif

	#ifdef DAEDALUS_ENABLE_SYNCHRONISATION
	SYNCH_POINT( DAED_SYNC_REG_PC, gCPUState.CurrentPC, "Program Counter doesn't match" );
	SYNCH_POINT( DAED_SYNC_FRAGMENT_PC, gCPUState.CurrentPC + gCPUState.Delay, "Program Counter/Delay doesn't match while interpreting" );
	SYNCH_POINT( DAED_SYNC_REG_PC, gCPUState.CPUControl[C0_COUNT]._u32, "Count doesn't match" );
	#endif
	if( TraceEnabled )
	{
		#ifdef DAEDALUS_ENABLE_ASSERTS
		DAEDALUS_ASSERT( gTraceRecorder.IsTraceActive(), "If TraceEnabled is set, trace should be active" );
		#endif
		u32		pc( gCPUState.CurrentPC ) ;
		bool	branch_delay_slot( gCPUState.Delay == EXEC_DELAY );

		R4300_ExecuteInstruction(op_code);
		gGPR[0]._u64 = 0;	//Ensure r0 is zero

		bool	branch_taken( gCPUState.Delay == DO_DELAY );

		CPU_UpdateTrace( pc, op_code, branch_delay_slot, branch_taken );
	}
	else
	{
		#ifdef DAEDALUS_ENABLE_ASSERTS
		DAEDALUS_ASSERT( !gTraceRecorder.IsTraceActive(), "If TraceEnabled is not set, trace should be inactive" );
		#endif
		R4300_ExecuteInstruction(op_code);
		gGPR[0]._u64 = 0;	//Ensure r0 is zero

#ifdef DAEDALUS_PROFILE_EXECUTION
		gTotalInstructionsEmulated++;
#endif
	}
	#ifdef DAEDALUS_ENABLE_SYNCHRONISATION
	SYNCH_POINT( DAED_SYNC_REGS, CPU_ProduceRegisterHash(), "Registers don't match" );
	#endif
	// Increment count register
	gCPUState.CPUControl[C0_COUNT]._u32 = gCPUState.CPUControl[C0_COUNT]._u32 + COUNTER_INCREMENT_PER_OP;

	if (CPU_ProcessEventCycles( COUNTER_INCREMENT_PER_OP ) )
	{
		CPU_HANDLE_COUNT_INTERRUPT();
	}

	switch (gCPUState.Delay)
	{
	case DO_DELAY:
		// We've got a delayed instruction to execute. Increment
		// PC as normal, so that subsequent instruction is executed
		INCREMENT_PC();
		gCPUState.Delay = EXEC_DELAY;

		break;
	case EXEC_DELAY:
		{
			bool	backwards( gCPUState.TargetPC <= gCPUState.CurrentPC );

			// We've just executed the delayed instr. Now carry out jump as stored in gCPUState.TargetPC;
			CPU_SetPC(gCPUState.TargetPC);
			gCPUState.Delay = NO_DELAY;

			CPU_HandleDynaRecOnBranch( backwards, TraceEnabled );
		}
		break;
	case NO_DELAY:
		// Normal operation - just increment the PC
		INCREMENT_PC();
		break;
	default:
		NODEFAULT;
	}
}


//*****************************************************************************
//
//*****************************************************************************
void	CPU_ResetFragmentCache()
{
	// Need to make sure this happens at a safe point, so we use a flag
	gResetFragmentCache	= true;
}

//*****************************************************************************
// Keep executing instructions until there are other tasks to do (i.e. gCPUState.GetStuffToDo() is set)
// Process these tasks and loop
//*****************************************************************************
template < bool DynaRec, bool TraceEnabled > void CPU_Go()
{
	DAEDALUS_PROFILE( __FUNCTION__ );

	while (CPU_KeepRunning())
	{
		//
		// Keep executing ops as long as there's nothing to do
		//
		u32	stuff_to_do( gCPUState.GetStuffToDo() );
		while(stuff_to_do == 0)
		{
			CPU_EXECUTE_OP< TraceEnabled >();

			stuff_to_do = gCPUState.GetStuffToDo();
		}

		if( TraceEnabled && (stuff_to_do != CPU_CHANGE_CORE) )
		{
			if(gTraceRecorder.IsTraceActive())
			{
#ifdef DAEDALUS_DEBUG_DYNAREC
				u32 start_address( gTraceRecorder.GetStartTraceAddress() );
				//DBGConsole_Msg( 0, "Aborting tracing of [R%08x] - StuffToDo is %08x", start_address, stuff_to_do );

				gAbortedTraceReasons[ start_address ] = stuff_to_do;
#endif

#ifdef ALLOW_TRACES_WHICH_EXCEPT
				if(stuff_to_do == CPU_CHECK_INTERRUPTS && gCPUState.Delay == NO_DELAY )		// Note checking for exactly equal, not just that it's set
				{
					//DBGConsole_Msg( 0, "Adding chunk at %08x after interrupt\n", gTraceRecorder.GetStartTraceAddress() );
					gTraceRecorder.StopTrace( gCPUState.CurrentPC );
					CPU_CreateAndAddFragment();
				}
#endif

				gTraceRecorder.AbortTrace();		// Abort any traces that were terminated through an interrupt etc
			}
			CPU_SelectCore();
		}

		if (CPU_CheckStuffToDo())
			break;
	}
}

#ifdef DAEDALUS_DEBUG_DYNAREC

struct SAddressHitCount
{
	u32		Address {};
	u32		HitCount {};

	SAddressHitCount( u32 address, u32 hitcount ) : Address( address ), HitCount( hitcount ) {}

	u32		GetAbortReason() const
	{
		std::map<u32, u32>::const_iterator		it( gAbortedTraceReasons.find( Address ) );
		if( it != gAbortedTraceReasons.end() )
		{
			return it->second;
		}

		return 0;
	}
};

bool SortByHitCount( const SAddressHitCount & a, const SAddressHitCount & b )
{
	return a.HitCount > b.HitCount;
}

//*****************************************************************************
//
//*****************************************************************************
void	CPU_DumpFragmentCache()
{
	IO::Directory::EnsureExists( "DynarecDump" );

	FILE  * fh( fopen( "DynarecDump/hot_trace_map.html", "w" ) );
	if( fh != nullptr )
	{
		std::vector< SAddressHitCount >	hit_counts;

		hit_counts.reserve( gHotTraceCountMap.size() );

		for(std::map<u32,u32>::const_iterator it = gHotTraceCountMap.begin(); it != gHotTraceCountMap.end(); ++it )
		{
			hit_counts.push_back( SAddressHitCount( it->first, it->second ) );
		}

		std::sort( hit_counts.begin(), hit_counts.end(), SortByHitCount );

		fputs( "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">", fh );
		fputs( "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n", fh );
		fputs( "<head><title>Hot Trace Map</title>\n", fh );
		fputs( "<link rel=\"stylesheet\" href=\"default.css\" type=\"text/css\" media=\"all\" />\n", fh );
		fputs( "</head><body>\n", fh );
		fputs( "<h1>Hot Trace Map</h1>\n", fh );
		fputs( "<div align=\"center\"><table>\n", fh );
		fputs( "<tr><th>Address</th><th>Hit Count</th><th>Abort Reason</th></tr>\n", fh );

		for( u32 i = 0; i < hit_counts.size(); ++i )
		{
			const SAddressHitCount & info( hit_counts[ i ] );

			u32		abort_reason( info.GetAbortReason() );

			fprintf( fh, "<tr><td>%08x</td><td>%d</td>\n", info.Address, info.HitCount );

			fputs( "<td>", fh );
			if(abort_reason & CPU_CHECK_EXCEPTIONS)		{ fputs( " Exception", fh ); }
			if(abort_reason & CPU_CHECK_INTERRUPTS)		{ fputs( " Interrupt", fh ); }
			if(abort_reason & CPU_STOP_RUNNING)			{ fputs( " StopRunning", fh ); }
			if(abort_reason & CPU_CHANGE_CORE)			{ fputs( " ChangeCore", fh ); }
			fputs( "</td></tr>\n", fh );

			//if( info.HitCount >= gHotTraceThreshold )
		}
		fputs( "</table></div>\n", fh );
		fputs( "</body></html>\n", fh );

		fclose(fh);
	}

	gFragmentCache.DumpStats( "DynarecDump/" );
}
#endif

//*****************************************************************************
//
//*****************************************************************************
void CPU_CreateAndAddFragment()
{
#ifdef DAEDALUS_CTR
	// Keep 512 KiB primary / 128 KiB secondary reserve for the next complete
	// trace and its literals; reset safely before overflowing fixed JIT regions.
	CCodeBufferManager * manager = gFragmentCache.GetCodeBufferManager();
	if (manager != nullptr && !manager->HasSpaceForBlock(512 * 1024))
	{
		gResetFragmentCache = true;
		gResetFragmentCacheForCapacity = true;
		return;
	}
#endif
#ifdef DAEDALUS_CTR_XRAY
	const u64 comp_t0 = svcGetSystemTick();
#endif
	CFragment * p_fragment( gTraceRecorder.CreateFragment( gFragmentCache.GetCodeBufferManager(), &gFragmentCache ) );
#ifdef DAEDALUS_CTR_XRAY
	PerfCTR::AddComp(svcGetSystemTick() - comp_t0);
#endif

	if( p_fragment != nullptr )
	{
		gHotTraceCountMap.erase( p_fragment->GetEntryAddress() );

		//DBGConsole_Msg( 0, "Inserted hot trace at [R%08x]! (size is %d. %dKB)", p_fragment->GetEntryAddress(), gFragmentCache.GetCacheSize(), gFragmentCache.GetMemoryUsage() / 1024 );
	}
}

//*****************************************************************************
//
//*****************************************************************************
void CPU_UpdateTrace( u32 address, OpCode op_code, bool branch_delay_slot, bool branch_taken )
{
	#ifdef DAEDALUS_PROFILE
	DAEDALUS_PROFILE( "CPU_UpdateTrace" );
		#endif
	#ifdef DAEDALUS_ENABLE_ASSERTS
	DAEDALUS_ASSERT_Q( (gCPUState.Delay == EXEC_DELAY) == branch_delay_slot );
	#endif


#ifdef DAEDALUS_DEBUG_DYNAREC
	CFragment * p_address_fragment( gFragmentCache.LookupFragment( address ) );
#else
	CFragment * p_address_fragment( gFragmentCache.LookupFragmentQ( address ) );
#endif
	if( gTraceRecorder.UpdateTrace( address, branch_delay_slot, branch_taken, op_code, p_address_fragment ) == CTraceRecorder::UTS_CREATE_FRAGMENT )
	{
		CPU_CreateAndAddFragment();
		#ifdef DAEDALUS_ENABLE_ASSERTS
		DAEDALUS_ASSERT( !gTraceRecorder.IsTraceActive(), "Why is a trace still active?" );
		#endif
		CPU_SelectCore();
	}
	#ifdef DAEDALUS_ENABLE_ASSERTS
	else
	{
		DAEDALUS_ASSERT( gTraceRecorder.IsTraceActive(), "The trace should still be enabled" );
	}
	#endif
}

//*****************************************************************************
//
//*****************************************************************************
void CPU_HandleDynaRecOnBranch( bool backwards, bool trace_already_enabled )
{
	#ifdef DAEDALUS_PROFILE
	DAEDALUS_PROFILE( "CPU_HandleDynaRecOnBranch" );
	#endif
	bool	start_of_trace( false );

	if( backwards )
	{
		start_of_trace = true;
	}

	bool	change_core( false );
	#ifdef DAEDALUS_LOG
	DAED_LOG( DEBUG_DYNAREC_CACHE, "CPU_HandleDynaRecOnBranch" );
	#endif

	while( gCPUState.GetStuffToDo() == 0 && gCPUState.Delay == NO_DELAY )
	{
		if (!gResetFragmentCache && gPendingInvalCount > 0)
		{
#ifdef DAEDALUS_ENABLE_OS_HOOKS
			if (gNumOfOSFunctions == 0)
			{
				// During early ROM boot (before libultra OS functions are discovered),
				// fall back to full Clear() + Patch_PatchAll() so OSHLE hooks install immediately.
				gPendingInvalCount = 0;
				gResetFragmentCache = true;
			}
			else
#endif
			{
				// Safe branch boundary (outside _EnterDynaRec): selectively evict only
				// fragments overlapping the modified RDRAM ranges instead of flushing the
				// entire 6 MiB cache!
				if (gTraceRecorder.IsTraceActive())
				{
					gTraceRecorder.AbortTrace();
					change_core = true;
				}
				bool all_selective_ok = true;
				for (u32 i = 0; i < gPendingInvalCount; ++i)
				{
					u32 evicted = 0;
					if (!gFragmentCache.InvalidateRange(gPendingInvals[i].addr, gPendingInvals[i].len, &evicted))
					{
						all_selective_ok = false;
						break;
					}
#ifdef DAEDALUS_CTR_XRAY
					static u32 sSelLogs = 0;
					if (evicted > 0 && sSelLogs < 24)
					{
						++sSelLogs;
						if (FILE * f = fopen("sdmc:/DaedalusX64.log", "a"))
						{
							fprintf(f, "  inval[SEL]: addr=0x%08lx len=0x%lx evicted=%lu kept=%lu\n",
								(unsigned long)gPendingInvals[i].addr,
								(unsigned long)gPendingInvals[i].len,
								(unsigned long)evicted,
								(unsigned long)gFragmentCache.GetCacheSize());
							fclose(f);
						}
					}
#endif
				}
				gPendingInvalCount = 0;
				if (all_selective_ok)
				{
					gHotTraceCountMap.clear();
				}
				else
				{
					gResetFragmentCache = true;
				}
			}
		}

		#ifdef DAEDALUS_ENABLE_DYNAREC_PROFILE
		DAEDALUS_ASSERT( gCPUState.Delay == NO_DELAY, "Why are we entering with a delay slot active?" );
		u32			entry_count( gCPUState.CPUControl[C0_COUNT]._u32 ); // Just used DYNAREC_PROFILE_ENTEREXIT
#endif
		u32			entry_address( gCPUState.CurrentPC );
#ifdef DAEDALUS_DEBUG_DYNAREC
		CFragment * p_fragment( gFragmentCache.LookupFragment( entry_address ) );
#else
		CFragment * p_fragment( gFragmentCache.LookupFragmentQ( entry_address ) );
#endif
		if( p_fragment != nullptr )
		{
		#ifdef DAEDALUS_PROFILE_EXECUTION
			gFragmentLookupSuccess++;
		#endif

		// Check if another trace is active and we're about to enter
			if( gTraceRecorder.IsTraceActive() )
			{
				gTraceRecorder.StopTrace( gCPUState.CurrentPC );
				CPU_CreateAndAddFragment();

				// We need to change the core when exiting
				change_core = true;
			}

			p_fragment->Execute();
#ifdef DAEDALUS_CTR_XRAY
			if (gCPUState.GetStuffToDo() != 0)
				PerfCTR::CountExitReason(0);
			else if (gCPUState.Delay != NO_DELAY)
				PerfCTR::CountExitReason(1);
			else
				PerfCTR::CountExitReason(2);
#endif

			DYNAREC_PROFILE_ENTEREXIT( entry_address, gCPUState.CurrentPC, gCPUState.CPUControl[C0_COUNT]._u32 - entry_count );

			start_of_trace = true;
		}
		else
		{
		#ifdef DAEDALUS_PROFILE_EXECUTION
			gFragmentLookupFailure++;
		#endif
			if( start_of_trace )
			{
				start_of_trace = false;

				if (gResetFragmentCacheForCapacity)
				{
					// At this branch boundary no cached fragment is executing.
#ifdef DAEDALUS_CTR_XRAY
					PerfCTR::CountResetReason(0);
#endif
					gFragmentCache.Clear();
					gHotTraceCountMap.clear();
#ifdef DAEDALUS_ENABLE_OS_HOOKS
					Patch_PatchAll();
#endif
					gResetFragmentCacheForCapacity = false;
					gResetFragmentCache = false;
					gPendingInvalCount = 0;
				}

				if( !gTraceRecorder.IsTraceActive() )
				{
					if (gResetFragmentCache)
					{
#ifdef DAEDALUS_ENABLE_OS_HOOKS
						//Don't reset the cache if there is no fragment except OSHLE function stubs
						if (gFragmentCache.GetCacheSize() >= gNumOfOSFunctions)
#else
						if(true)
#endif
						{
#ifdef DAEDALUS_CTR_XRAY
							PerfCTR::CountResetReason(1);
#endif
							gFragmentCache.Clear();
							gHotTraceCountMap.clear();		// Makes sense to clear this now, to get accurate usage stats
#ifdef DAEDALUS_ENABLE_OS_HOOKS
							Patch_PatchAll();
#endif
						}
#ifdef DAEDALUS_DEBUG_CONSOLE
						else
						{
							DBGConsole_Msg(0, "Safely skipped one flush");
						}
#endif
						gResetFragmentCache = false;
					}

					if( gFragmentCache.GetCacheSize() > gMaxFragmentCacheSize)
					{
#ifdef DAEDALUS_CTR_XRAY
						PerfCTR::CountResetReason(2);
#endif
						gFragmentCache.Clear();
						gHotTraceCountMap.clear();		// Makes sense to clear this now, to get accurate usage stats
#ifdef DAEDALUS_ENABLE_OS_HOOKS
						Patch_PatchAll();
#endif
					}

					// If there is no fragment for this target, start tracing
					u32 trace_count( HOT_TRACE_INCREMENT( gCPUState.CurrentPC ) );
					if( gHotTraceCountMap.size() >= gMaxHotTraceMapSize )
					{
						#ifdef DAEDALUS_DEBUG_CONSOLE
						DBGConsole_Msg( 0, "Hot trace cache hit %d, dumping", gHotTraceCountMap.size() );
						#endif
#ifdef DAEDALUS_CTR_XRAY
						PerfCTR::CountResetReason(3);
#endif
						// Only clear the cold-target counter table; never nuke compiled fragments
						// just because 3072 cold addresses were visited (fixes SSB fight cache thrash!).
						gHotTraceCountMap.clear();
					}
					else if( trace_count == gHotTraceThreshold )
					{
						//DBGConsole_Msg( 0, "Identified hot trace at [R%08x]! (size is %d)", gCPUState.CurrentPC, gHotTraceCountMap.size() );
						gTraceRecorder.StartTrace( gCPUState.CurrentPC );

						if(!trace_already_enabled)
						{
							change_core = true;
						}
						DAED_LOG( DEBUG_DYNAREC_CACHE, "StartTrace( %08x )", gCPUState.CurrentPC );
					}
#ifdef DAEDALUS_DEBUG_DYNAREC
					else if( trace_count > gHotTraceThreshold )
					{
						if(gAbortedTraceReasons.find( gCPUState.CurrentPC ) != gAbortedTraceReasons.end() )
						{
							u32 reason( gAbortedTraceReasons[ gCPUState.CurrentPC ] );
							//use( reason );
							//DBGConsole_Msg( 0, "Hot trace at [R%08x] has count of %d! (reason is %x) size %d", gCPUState.CurrentPC, trace_count, reason, gHotTraceCountMap.size( ) );
							DAED_LOG( DEBUG_DYNAREC_CACHE, "Hot trace at %08x has count of %d! (reason is %x) size %d", gCPUState.CurrentPC, trace_count, reason, gHotTraceCountMap.size( ) );
						}
						else
						{
							DAED_LOG( DEBUG_DYNAREC_CACHE, "Hot trace at %08x has count of %d! (reason is UNKNOWN!)", gCPUState.CurrentPC, trace_count );
						}
					}
#endif //DAEDALUS_DEBUG_DYNAREC
				}
			}
			#ifdef DAEDALUS_LOG
			else
			{
				DAED_LOG( DEBUG_DYNAREC_CACHE, "Not start of trace" );
			}
			#endif
			break;
		}
	}

	if(change_core)
	{
		CPU_SelectCore();
	}
}

void Dynamo_Reset()
{
	gHotTraceCountMap.clear();
	gFragmentCache.Clear();
	gResetFragmentCache = false;
	gResetFragmentCacheForCapacity = false;
	gPendingInvalCount = 0;
	gTraceRecorder.AbortTrace();
#ifdef DAEDALUS_DEBUG_DYNAREC
	gAbortedTraceReasons.clear();
#endif
}

void Dynamo_SelectCore()
{
	bool trace_enabled = gTraceRecorder.IsTraceActive();

	if (trace_enabled)
	{
		g_pCPUCore = CPU_Go< true, true >;
	}
	else
	{
		g_pCPUCore = CPU_Go< true, false >;
	}
}

#else

void CPU_ResetFragmentCache() {}
void Dynamo_Reset() {}
void R4300_CALL_TYPE CPU_InvalidateICacheRange( u32 address, u32 length ) {}

#endif //DAEDALUS_ENABLE_DYNAREC
