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

#ifndef DYNAREC_FRAGMENTCACHE_H_
#define DYNAREC_FRAGMENTCACHE_H_

#include "Utility/DaedalusTypes.h"

class	CFragment;
struct STraceEntry;
class	CJumpLocation;
class	CCodeBufferManager;

#include <map>
#include <vector>

struct FHashT
{
	u32	addr;
	u32 ptr;
};

#include "FragmentCacheCoverage.h"

class CFragmentCache
{
public:
	CFragmentCache();
	~CFragmentCache();

	// On failure `error` receives the reason: 0, a negative errno-like value, or
	// the platform Result. It may be null.
	bool					InitialiseCodeBuffer( int * error = nullptr );

#ifdef DAEDALUS_DEBUG_DYNAREC
	CFragment *				LookupFragment( u32 address ) const;
#endif
	CFragment *				LookupFragmentQ( u32 address ) const;
	// Pass the original trace for precise code coverage. Synthetic HLE
	// fragments omit it and retain their contiguous function-range coverage.
	void					InsertFragment( CFragment * p_fragment,
										const std::vector<STraceEntry> * trace = nullptr );

	u32						GetCacheSize() const					{ return mFragments.size(); }
	void					Clear();

#ifdef DAEDALUS_DEBUG_DYNAREC
	void					DumpStats( const char * outputdir ) const;
#endif

	u32						GetMemoryUsage() const					{ return mMemoryUsage; }

	CCodeBufferManager *	GetCodeBufferManager() const			{ return mpCodeBufferManager; }

	bool					ShouldInvalidateOnWrite( u32 address, u32 length ) const;

private:
	struct SFragmentEntry
	{
		SFragmentEntry( u32 address, CFragment * fragment )
			:	Address( address )
			,	Fragment( fragment )
		{
		}

		bool operator<( const SFragmentEntry & rhs ) const
		{
			return Address < rhs.Address;
		}

		u32			Address;
		CFragment *	Fragment;
	};

	typedef std::vector< SFragmentEntry >	FragmentVec;
	FragmentVec				mFragments;			// Sorted on Address

	u32						mMemoryUsage;
	u32						mInputLength;
	u32						mOutputLength;

	typedef std::vector< CJumpLocation >	JumpList;
	typedef std::map< u32, JumpList >		JumpMap;
	JumpMap					mJumpMap;

	mutable u32				mCachedFragmentAddress;
	mutable CFragment *		mpCachedFragment;

	static const u32 HASH_TABLE_BITS = 15;
	static const u32 HASH_TABLE_SIZE = 1<<HASH_TABLE_BITS;

	//Low 2 bits will always be 0, remove this redundancy (the amount of folding depends on the hash table size)
	//#define MakeHashIdx( addr ) (((addr >> (2 * HASH_TABLE_BITS + 2)) ^ (addr >> (HASH_TABLE_BITS + 2)) ^ addr >> 2 ) & (HASH_TABLE_SIZE-1))
	#define MakeHashIdx( addr ) (((addr >> (HASH_TABLE_BITS + 2)) ^ addr >> 2 ) & (HASH_TABLE_SIZE-1))

	mutable FHashT			mpCacheHashTable[HASH_TABLE_SIZE];

	CCodeBufferManager *	mpCodeBufferManager;

	CFragmentCacheCoverage	mCacheCoverage;
};

extern CFragmentCache				gFragmentCache;
#endif // DYNAREC_FRAGMENTCACHE_H_
