/*
Copyright (C) 2006 StrmnNrmn

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
#include "FragmentCache.h"

#include <stdio.h>

#include <algorithm>

#include "Fragment.h"
#include "IndirectExitMap.h"
#include "CodeBufferManager.h"
#include "DynaRecProfile.h"

#include "Debug/DBGConsole.h"

#include "Utility/Profiler.h"
#include "Utility/IO.h"

#include "AssemblyUtils.h"
#ifdef DAEDALUS_CTR
#include "SysCTR/Utility/PerfProfilerCTR.h"
#endif

SIndirectJumpEntry gIndirectJumpTable[INDIRECT_JUMP_TABLE_SIZE] = {};


//Define to show hash table statistics cache hit/miss
//#define HASH_TABLE_STATS

using namespace AssemblyUtils;

//*************************************************************************************
//
//*************************************************************************************
CFragmentCache::CFragmentCache()
:	mMemoryUsage( 0 )
,	mInputLength( 0 )
,	mOutputLength( 0 )
,	mCachedFragmentAddress( 0 )
,	mpCachedFragment( nullptr )
,	mpCodeBufferManager( nullptr )
{
	memset( mpCacheHashTable, 0, sizeof(mpCacheHashTable) );

	mFragments.reserve( 2000 );

#ifndef DAEDALUS_CTR
	InitialiseCodeBuffer();
#endif
}

bool CFragmentCache::InitialiseCodeBuffer( int * error )
{
	if (mpCodeBufferManager != nullptr)
	{
		if (error != nullptr)
			*error = 0;
		return true;
	}

	CCodeBufferManager * manager = CCodeBufferManager::Create();
	if (manager == nullptr)
	{
		if (error != nullptr)
			*error = -2;
		return false;
	}
	if (!manager->Initialise())
	{
		if (error != nullptr)
			*error = manager->InitialiseError();
		delete manager;
		return false;
	}
	mpCodeBufferManager = manager;
	if (error != nullptr)
		*error = 0;
	return true;
}

//*************************************************************************************
//
//*************************************************************************************
CFragmentCache::~CFragmentCache()
{
	FinaliseCodeBuffer();
}

void CFragmentCache::FinaliseCodeBuffer()
{
	Clear();

	if (mpCodeBufferManager != nullptr)
	{
		mpCodeBufferManager->Finalise();
		delete mpCodeBufferManager;
		mpCodeBufferManager = nullptr;
	}
}

//*************************************************************************************
//
//*************************************************************************************
#ifdef DAEDALUS_DEBUG_DYNAREC
CFragment * CFragmentCache::LookupFragment( u32 address ) const
{
	DAEDALUS_PROFILE( "CFragmentCache::LookupFragment" );

	if( address != mCachedFragmentAddress )
	{
		mCachedFragmentAddress = address;

		// check if in hash table
		u32 ix = MakeHashIdx( address );

		if ( address != mpCacheHashTable[ix].addr )
		{
			SFragmentEntry				entry( address, nullptr );
			FragmentVec::const_iterator	it( std::lower_bound( mFragments.begin(), mFragments.end(), entry ) );
			if( it != mFragments.end() && it->Address == address )
			{
				mpCachedFragment = it->Fragment;
			}
			else
			{
				mpCachedFragment = nullptr;
			}

			// put in hash table
			mpCacheHashTable[ix].addr = address;
			mpCacheHashTable[ix].ptr = reinterpret_cast< u32 >( mpCachedFragment );
		}
		else
		{
			mpCachedFragment = reinterpret_cast< CFragment * >( mpCacheHashTable[ix].ptr );
		}
	}

	CFragment * p = mpCachedFragment;

	DYNAREC_PROFILE_LOGLOOKUP( address, p );

	return p;
}
#endif
//*************************************************************************************
//
//*************************************************************************************
CFragment * CFragmentCache::LookupFragmentQ( u32 address ) const
{
	#ifdef DAEDALUS_DEBUG_DYNAREC
	DAEDALUS_PROFILE( "CFragmentCache::LookupFragmentQ" );
	#endif
#ifdef HASH_TABLE_STATS
	static u32 hit=0, miss=0;
#endif
	if( address != mCachedFragmentAddress )
	{
		mCachedFragmentAddress = address;

		// check if in hash table
		u32 ix {MakeHashIdx( address )};

		if ( address != mpCacheHashTable[ix].addr )
		{
#ifdef HASH_TABLE_STATS
			miss++;
#endif
			SFragmentEntry				entry( address, nullptr );
			FragmentVec::const_iterator	it( std::lower_bound( mFragments.begin(), mFragments.end(), entry ) );
			if( it != mFragments.end() && it->Address == address )
			{
				mpCachedFragment = it->Fragment;
			}
			else
			{
				mpCachedFragment = nullptr;
			}

			// put in hash table
			mpCacheHashTable[ix].addr = address;
			mpCacheHashTable[ix].ptr = reinterpret_cast< u32 >( mpCachedFragment );
		}
		else
		{
#ifdef HASH_TABLE_STATS
			hit++;
#endif
			mpCachedFragment = reinterpret_cast< CFragment * >( mpCacheHashTable[ix].ptr );
		}

#ifdef HASH_TABLE_STATS
		if(miss+hit==10000)
		{
			printf("Hit[%d]  Miss[%d]\n", hit, miss);
			miss=hit=0;
		}
#endif
	}

	return mpCachedFragment;
}

//*************************************************************************************
//
//*************************************************************************************
void CFragmentCache::InsertFragment( CFragment * p_fragment, const std::vector<STraceEntry> * trace )
{
	u32		fragment_address( p_fragment->GetEntryAddress() );

	const std::vector<SFragmentCoveredRange> & covered = p_fragment->GetCoveredRanges();
	if (!covered.empty())
	{
		for (size_t i = 0; i < covered.size(); ++i)
			mCacheCoverage.ExtendCoverage(covered[i].Start, covered[i].End - covered[i].Start);
	}
	else if (trace != nullptr && !trace->empty())
	{
		for (std::vector<STraceEntry>::const_iterator it = trace->begin(); it != trace->end(); ++it)
			mCacheCoverage.ExtendCoverage(it->Address, sizeof(OpCode));
	}
	else
	{
		// OS/HLE fragments are synthetic and have no interpreter trace; their
		// signature is a contiguous function range.
		mCacheCoverage.ExtendCoverage( fragment_address, p_fragment->GetInputLength() );
	}

	SFragmentEntry				entry( fragment_address, nullptr );
	FragmentVec::iterator		it( std::lower_bound( mFragments.begin(), mFragments.end(), entry ) );
	#ifdef DAEDALUS_ENABLE_ASSERTS
	DAEDALUS_ASSERT( it == mFragments.end() || it->Address != fragment_address, "A fragment with this address already exists" );
	#endif
	entry.Fragment = p_fragment;
	mFragments.insert( it, entry );

	// Update the hash table (it stores failed lookups now, so we need to be sure to purge any stale entries in there
	u32 ix {MakeHashIdx( fragment_address )};
	mpCacheHashTable[ix].addr = fragment_address;
	mpCacheHashTable[ix].ptr = reinterpret_cast< u32 >( p_fragment );

	// Populate global direct-mapped native target table for fast ARM indirect dispatch
	const u32 jump_ix = IndirectJumpTableIndex( fragment_address );
	gIndirectJumpTable[jump_ix].addr   = fragment_address;
	gIndirectJumpTable[jump_ix].target = p_fragment->GetEntryTarget().GetTarget();

	// Process any jumps for this before inserting new ones
	JumpMap::iterator	jump_it( mJumpMap.find( fragment_address ) );
	if( jump_it != mJumpMap.end() )
	{
		const JumpList &		jumps( jump_it->second );
		for( JumpList::const_iterator it = jumps.begin(); it != jumps.end(); ++it )
		{
			//DBGConsole_Msg( 0, "Inserting [R%08x], patching jump at %08x ", address, (*it) );
			PatchJumpLongAndFlush( it->Jump, p_fragment->GetEntryTarget() );
			p_fragment->AddIncomingLink( *it );
		}

		// All patched - clear
		mJumpMap.erase( jump_it );
	}

	// Finally register any links that this fragment may have
	const FragmentPatchList &	patch_list( p_fragment->GetPatchList() );
	for( FragmentPatchList::const_iterator it = patch_list.begin(); it != patch_list.end(); ++it )
	{
		u32				target_address( it->Address );
		CJumpLocation	jump( it->Jump );

	#ifdef DAEDALUS_ENABLE_ASSERTS
		DAEDALUS_ASSERT( jump.IsSet(), "No exit jump?" );
		#endif

		// Decode the unlinked fallback target (Buffer B interpret_next_fragment stub)
		// from the ARM B instruction before patching it to a direct fragment target.
		CCodeLabel fallback( nullptr );
#ifdef DAEDALUS_CTR
		{
			const u32 insn = *reinterpret_cast<const u32 *>( jump.GetTargetU8P() );
			const s32 rel_bytes = (static_cast<s32>(insn << 8) >> 6) + 8;
			fallback = CCodeLabel( jump.GetTargetU8P() + rel_bytes );
		}
#endif
		SFragmentJumpLink link = { jump, fallback, p_fragment };

#ifdef DAEDALUS_DEBUG_DYNAREC
		CFragment * p_target( LookupFragment( target_address ) );
#else
		CFragment * p_target( LookupFragmentQ( target_address ) );
#endif
		if( p_target != nullptr )
		{
			PatchJumpLongAndFlush( jump, p_target->GetEntryTarget() );
			p_target->AddIncomingLink( link );

	#ifdef DAEDALUS_ENABLE_ASSERTS
			DAEDALUS_ASSERT( mJumpMap.find( target_address ) == mJumpMap.end(), "Jump map still contains an entry for this" );
			#endif
		}
		else if( target_address != u32(~0) )
		{
			// Store the address for later processing
			mJumpMap[ target_address ].push_back( link );
		}
	}

	// Free memoire
	p_fragment->DiscardPatchList();

	// For simulation only
	p_fragment->SetCache( this );

	// Update memory usage etc after discarding patch list
	mMemoryUsage += p_fragment->GetMemoryUsage();
	mInputLength += p_fragment->GetInputLength();
	mOutputLength += p_fragment->GetOutputLength();

#ifdef DAEDALUS_DEBUG_CONSOLE
	if((mFragments.size() % 100) == 0)
	{
		u32		expansion = 1;
		if(mInputLength > 0)
		{
			expansion = (100 * mOutputLength) / mInputLength;
		}
		DBGConsole_Msg( 0, "Dynarec: %d fragments, %dKB (i:o %d:%d = %d%%)",
			mFragments.size(), mMemoryUsage / 1024, mInputLength / 1024, mOutputLength / 1024, expansion );
	}
#endif
}

//*************************************************************************************
//
//*************************************************************************************
void CFragmentCache::Clear()
{
#ifdef DAEDALUS_DEBUG_CONSOLE
	if(CDebugConsole::IsAvailable())
	{
		DBGConsole_Msg( 0, "Clearing fragment cache of %d fragments", mFragments.size() );
	}
#endif
	// Clear out all the framents
	for(FragmentVec::iterator it = mFragments.begin(); it != mFragments.end(); ++it)
	{
		delete it->Fragment;
	}

	mFragments.erase( mFragments.begin(), mFragments.end() );
	mMemoryUsage = 0;
	mInputLength = 0;
	mOutputLength = 0;
	mCachedFragmentAddress = 0;
	mpCachedFragment = nullptr;
	memset( mpCacheHashTable, 0, sizeof(mpCacheHashTable) );
	memset( gIndirectJumpTable, 0, sizeof(gIndirectJumpTable) );
	mJumpMap.clear();
#ifdef DAEDALUS_CTR
	PerfCTR::CountCacheReset();
#endif

	mCacheCoverage.Reset();

	if (mpCodeBufferManager != nullptr)
		mpCodeBufferManager->Reset();
}

//*************************************************************************************
//
//*************************************************************************************
bool CFragmentCache::InvalidateRange( u32 address, u32 length, u32 * out_evicted )
{
	if (out_evicted != nullptr)
		*out_evicted = 0;

#ifndef DAEDALUS_CTR
	(void)address;
	(void)length;
	return false;
#else
	static const u32 RDRAM_SIZE = 8u * 1024u * 1024u;
	if (length == 0)
		return true;

	u32 phys_start = 0;
	if (address < RDRAM_SIZE)
	{
		phys_start = address;
	}
	else
	{
		const u32 seg = address & 0xE0000000u;
		if (seg != 0x80000000u && seg != 0xA0000000u)
			return false;
		phys_start = address & 0x1FFFFFFFu;
		if (phys_start >= RDRAM_SIZE)
			return true;
	}
	u64 end64 = static_cast<u64>(phys_start) + static_cast<u64>(length);
	if (end64 > RDRAM_SIZE)
		end64 = RDRAM_SIZE;
	if (end64 <= phys_start)
		return true;
	const u32 phys_end = static_cast<u32>(end64);

	std::vector<CFragment *> evicted;
	evicted.reserve(64);
	for (size_t i = 0; i < mFragments.size(); ++i)
	{
		CFragment * f = mFragments[i].Fragment;
		if (f->OverlapsRange(phys_start, phys_end))
		{
			if (f->IsOshleFragment())
				return false;
			evicted.push_back(f);
			if (evicted.size() > 256 || evicted.size() * 2 >= mFragments.size())
				return false;
		}
	}

	if (evicted.empty())
		return true;

	// 1) Mark all evicted fragments first so cross-links among evicted fragments are skipped.
	for (size_t i = 0; i < evicted.size(); ++i)
		evicted[i]->SetEvicted(true);

	// 2) Unpatch incoming direct jumps from surviving fragments back to their Buffer B
	//    unlinked fallback stubs, and re-register them in mJumpMap for future re-linking.
	for (size_t i = 0; i < evicted.size(); ++i)
	{
		CFragment * f = evicted[i];
		const u32 entry_addr = f->GetEntryAddress();
		const std::vector<SFragmentJumpLink> & links = f->GetIncomingLinks();
		for (size_t j = 0; j < links.size(); ++j)
		{
			if (!links[j].Owner->IsEvicted() && links[j].Fallback.IsSet())
			{
				PatchJumpLongAndFlush(links[j].Jump, links[j].Fallback);
				mJumpMap[entry_addr].push_back(links[j]);
			}
		}
	}

	// 3) Purge pending jump-map entries owned by evicted fragments.
	for (JumpMap::iterator it = mJumpMap.begin(); it != mJumpMap.end(); )
	{
		JumpList & list = it->second;
		size_t write = 0;
		for (size_t i = 0; i < list.size(); ++i)
		{
			if (!list[i].Owner->IsEvicted())
			{
				if (write != i)
					list[write] = list[i];
				++write;
			}
		}
		if (write == 0)
		{
			JumpMap::iterator erase_it = it++;
			mJumpMap.erase(erase_it);
		}
		else
		{
			list.resize(write);
			++it;
		}
	}

	// 4) Compact mFragments, rebuild exact 64-byte coverage bitmap, clean surviving
	//    incoming link lists and indirect MRU caches.
	mCacheCoverage.Reset();
	size_t keep = 0;
	for (size_t i = 0; i < mFragments.size(); ++i)
	{
		CFragment * f = mFragments[i].Fragment;
		if (f->IsEvicted())
		{
			const u32 mu = f->GetMemoryUsage();
			const u32 il = f->GetInputLength();
			const u32 ol = f->GetOutputLength();
			mMemoryUsage  = (mMemoryUsage > mu)  ? (mMemoryUsage - mu)  : 0;
			mInputLength  = (mInputLength > il)  ? (mInputLength - il)  : 0;
			mOutputLength = (mOutputLength > ol) ? (mOutputLength - ol) : 0;
			delete f;
		}
		else
		{
			f->RemoveEvictedIncomingLinks();
			if (CIndirectExitMap * imap = f->GetIndirectExitMap())
				imap->ClearMru();
			const std::vector<SFragmentCoveredRange> & ranges = f->GetCoveredRanges();
			for (size_t r = 0; r < ranges.size(); ++r)
				mCacheCoverage.ExtendCoverage(ranges[r].Start, ranges[r].End - ranges[r].Start);
			if (keep != i)
				mFragments[keep] = mFragments[i];
			++keep;
		}
	}
	mFragments.erase(mFragments.begin() + keep, mFragments.end());

	// 5) Clear fragment lookup and global indirect jump caches.
	mCachedFragmentAddress = 0;
	mpCachedFragment = nullptr;
	memset( mpCacheHashTable, 0, sizeof(mpCacheHashTable) );
	memset( gIndirectJumpTable, 0, sizeof(gIndirectJumpTable) );

	if (out_evicted != nullptr)
		*out_evicted = static_cast<u32>(evicted.size());
	return true;
#endif
}

//*************************************************************************************
//
//*************************************************************************************
bool CFragmentCache::ShouldInvalidateOnWrite( u32 address, u32 length ) const
{
	return mCacheCoverage.IsCovered( address, length );
}

#ifdef DAEDALUS_DEBUG_DYNAREC
//*************************************************************************************
//
//*************************************************************************************
struct SDescendingCyclesSort
{
     bool operator()(CFragment* const & a, CFragment* const & b)
     {
		return b->GetCyclesExecuted() < a->GetCyclesExecuted();
     }
};

//*************************************************************************************
//
//*************************************************************************************
void CFragmentCache::DumpStats( const char * outputdir ) const
{
	typedef std::vector< CFragment * >		FragmentList;
	FragmentList		all_fragments;

	all_fragments.reserve( mFragments.size() );

	u32		total_cycles( 0 );

	// Sort in order of expended cycles
	for(FragmentVec::const_iterator it = mFragments.begin(); it != mFragments.end(); ++it)
	{
		all_fragments.push_back( it->Fragment );
		total_cycles += it->Fragment->GetCyclesExecuted();
	}

	std::sort( all_fragments.begin(), all_fragments.end(), SDescendingCyclesSort() );


	IO::Filename	filename;
	IO::Filename	fragments_dir;

	IO::Path::Assign( fragments_dir, outputdir );
	IO::Path::Append( fragments_dir, "fragments" );
	IO::Directory::EnsureExists( fragments_dir );

	IO::Path::Combine( filename, outputdir, "fragments.html" );

	FILE * fh( fopen( filename, "w" ) );
	if(fh)
	{
		fputs( "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">", fh );
		fputs( "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n", fh );
		fputs( "<head><title>Fragments</title>\n", fh );
		fputs( "<link rel=\"stylesheet\" href=\"default.css\" type=\"text/css\" media=\"all\" />\n", fh );
		fputs( "</head><body>\n", fh );


		fputs( "<h1>Fragments</h1>\n", fh );
		fputs( "<div align=\"center\"><table>\n", fh );
		fputs( "<tr><th>Address</th><th>Loops</th><th>Cycle Count</th><th>Cycle %</th><th>Hit Count</th><th>Input Bytes</th><th>Output Bytes</th><th>Expansion Ratio</th></tr>\n", fh );

		for(FragmentList::const_iterator it = all_fragments.begin(); it != all_fragments.end(); ++it)
		{
			const CFragment * fragment( *it );

			if (fragment->GetCyclesExecuted() == 0)
				continue;

			fputs( "<tr>", fh );
			fprintf( fh, "<td><a href=\"fragments//%08x.html\">0x%08x</a></td>", fragment->GetEntryAddress(), fragment->GetEntryAddress() );
			fprintf( fh, "<td>%s</td>", fragment->GetEntryAddress() == fragment->GetExitAddress() ? "*" : "&nbsp;" );
			fprintf( fh, "<td>%d</td>", fragment->GetCyclesExecuted() );
			fprintf( fh, "<td>%#.2f%%</td>", f32( fragment->GetCyclesExecuted() * 100.0f ) / f32( total_cycles ) );
			fprintf( fh, "<td>%d</td>", fragment->GetHitCount() );
			fprintf( fh, "<td>%d</td>", fragment->GetInputLength() );
			fprintf( fh, "<td>%d</td>", fragment->GetOutputLength() );
			fprintf( fh, "<td>%#.2f</td>", f32( fragment->GetOutputLength() ) / f32( fragment->GetInputLength() ) );
			fputs( "</tr>\n", fh );

			IO::Filename	fragment_path;
			char			fragment_name[ 32+1 ];
			sprintf( fragment_name, "%08x.html", fragment->GetEntryAddress() );
			IO::Path::Combine( fragment_path, fragments_dir, fragment_name );

			FILE * fragment_fh( fopen( fragment_path, "w" ) );
			if( fragment_fh != nullptr )
			{
				fragment->DumpFragmentInfoHtml( fragment_fh, total_cycles );
				fclose( fragment_fh );
			}
		}

		fputs( "</table></div>\n", fh );
		fputs( "</body></html>\n", fh );

		fclose( fh );
	}
}
#endif // DAEDALUS_DEBUG_DYNAREC


//*************************************************************************************
//
//*************************************************************************************
