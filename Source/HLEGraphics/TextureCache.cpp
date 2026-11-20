/*
Copyright (C) 2001 StrmnNrmn

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

// Manages textures for RDP code
// Uses a HashTable (hashing on TImg) to allow quick access
//  to previously used textures

#include "stdafx.h"

#include "Core/ROM.h"
#include "HLEGraphics/DLDebug.h"
#include "HLEGraphics/TextureCache.h"
#include "HLEGraphics/TextureInfo.h"

#include "Utility/Profiler.h"

#include <vector>
#include <algorithm>

//#define PROFILE_TEXTURE_CACHE

template<> bool CSingleton< CTextureCache >::Create()
{
	#ifdef DAEDALUS_ENABLE_ASSERTS
	DAEDALUS_ASSERT_Q(mpInstance == nullptr);
#endif
	mpInstance = new CTextureCache();
	return mpInstance != nullptr;
}

CTextureCache::CTextureCache()
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
:	mDebugMutex("TextureCache")
#endif
{
	memset( mpCacheHashTable, 0, sizeof(mpCacheHashTable) );
}

CTextureCache::~CTextureCache()
{
	DropTextures();
}

inline u32 CTextureCache::MakeHashIdxA( const TextureInfo & ti )
{
	u32 address( ti.GetLoadAddress() );
	u32 hash( (address >> (HASH_TABLE_BITS*2)) ^ (address >> HASH_TABLE_BITS) ^ address );

	hash ^= ti.GetPalette() >> 2;			// Useful for palettised fonts, e.g in Starfox

	return hash & (HASH_TABLE_SIZE-1);
}

inline u32 CTextureCache::MakeHashIdxB( const TextureInfo & ti )
{
	return ti.GetHashCode() & (HASH_TABLE_SIZE-1);
}

struct SSortTextureEntries
{
public:
	bool operator()( const TextureInfo & a, const TextureInfo & b ) const
	{
		return a < b;
	}
	bool operator()( const CachedTexture * a, const TextureInfo & b ) const
	{
		return a->GetTextureInfo() < b;
	}
	bool operator()( const TextureInfo & a, const CachedTexture * b ) const
	{
		return a < b->GetTextureInfo();
	}
	bool operator()( const CachedTexture * a, const CachedTexture * b ) const
	{
		return a->GetTextureInfo() < b->GetTextureInfo();
	}
};

// Purge any textures that haven't been used recently, then merge the
// textures created during the previous RDP task into the sorted lookup vector.
void CTextureCache::PurgeOldTextures()
{
	MutexLock lock(GetDebugMutex());

	const bool high_pressure = (mTextures.size() + mPendingTextures.size()) >= 256;

#ifdef DAEDALUS_CTR
	// On Old 3DS, scanning hundreds of cached textures every single Display List wastes
	// ARM11 CPU time. Run full expiration sweeps every 8 RDP tasks unless under memory
	// pressure or running games that rely on per-frame texture invalidation hacks.
	static u32 sPurgeTick = 0;
	const bool force_sweep = high_pressure ||
		(g_ROM.GameHacks == WONDER_PROJECTJ2) ||
		(g_ROM.GameHacks == WORMS_ARMAGEDDON) ||
		((++sPurgeTick & 7u) == 0);
	if (force_sweep)
#endif
	{
		auto purge = [this, high_pressure](TextureVec & textures)
		{
			TextureVec::iterator out = textures.begin();
			for (TextureVec::iterator it = textures.begin(); it != textures.end(); ++it)
			{
				CachedTexture * texture = *it;
				if (texture->HasExpired(high_pressure))
				{
					const u32 ixa = MakeHashIdxA(texture->GetTextureInfo());
					const u32 ixb = MakeHashIdxB(texture->GetTextureInfo());

					if (mpCacheHashTable[ixa] == texture)
						mpCacheHashTable[ixa] = nullptr;
					if (mpCacheHashTable[ixb] == texture)
						mpCacheHashTable[ixb] = nullptr;

					delete texture;
				}
				else
				{
					*out++ = texture;
				}
			}
			textures.erase(out, textures.end());
		};

		purge(mTextures);
		purge(mPendingTextures);
	}

	if (!mPendingTextures.empty())
	{
		if (mPendingTextures.size() == 1)
		{
			CachedTexture * single = mPendingTextures[0];
			TextureVec::iterator pos = std::lower_bound(
				mTextures.begin(), mTextures.end(), single, SSortTextureEntries());
			mTextures.insert(pos, single);
		}
		else
		{
			// Inserting many new textures into mTextures shifts later pointers.
			// Collect new entries and merge once per RDP task.
			std::sort(mPendingTextures.begin(), mPendingTextures.end(), SSortTextureEntries());
			const TextureVec::size_type sorted_count = mTextures.size();
			mTextures.insert(mTextures.end(), mPendingTextures.begin(), mPendingTextures.end());
			std::inplace_merge(mTextures.begin(), mTextures.begin() + sorted_count,
				mTextures.end(), SSortTextureEntries());
		}
		mPendingTextures.clear();
	}
}

void CTextureCache::DropTextures()
{
	MutexLock lock(GetDebugMutex());

	for( u32 i = 0; i < mTextures.size(); ++i)
	{
		delete mTextures[i];
	}
	for( u32 i = 0; i < mPendingTextures.size(); ++i)
	{
		delete mPendingTextures[i];
	}
	mTextures.clear();
	mPendingTextures.clear();
	for( u32 i = 0; i < HASH_TABLE_SIZE; ++i )
	{
		mpCacheHashTable[i] = nullptr;
	}
}

#ifdef PROFILE_TEXTURE_CACHE
#define RECORD_CACHE_HIT( a, b )		TextureCacheStat( a, b, mTextures.size() + mPendingTextures.size() )

static void TextureCacheStat( u32 l1_hit, u32 l2_hit, u32 size )
{
	static u32 total_lookups = 0, total_l1_hits = 0, total_l2_hits = 0;

	total_l1_hits += l1_hit;
	total_l2_hits += l2_hit;
	++total_lookups;

	if( total_lookups == 1000 )
	{
		printf( "L1 hits[%d] L2 hits[%d] Miss[%d] (%d entries)\n", total_l1_hits, total_l2_hits, total_lookups - (total_l1_hits+total_l2_hits), size );
		total_lookups = total_l1_hits = total_l2_hits = 0;
	}
}
#else

#define RECORD_CACHE_HIT( a, b )

#endif

// If already in table, return cached copy
// Otherwise, create surfaces, and load texture into memory
CachedTexture * CTextureCache::GetOrCreateCachedTexture(const TextureInfo & ti)
{
	DAEDALUS_PROFILE( "CTextureCache::GetOrCreateCachedTexture" );

	if (ti.GetWidth() > 4096 || ti.GetHeight() > 4096)
	{
		DAEDALUS_ERROR("Texture is too large: %d x %d", ti.GetWidth(), ti.GetHeight());
		return nullptr;
	}

	// NB: this is a no-op in normal builds.
	MutexLock lock(GetDebugMutex());

	//
	// Retrieve the texture from the cache (if it already exists)
	//
	u32	ixa = MakeHashIdxA( ti );
	if( mpCacheHashTable[ixa] && mpCacheHashTable[ixa]->GetTextureInfo() == ti )
	{
		RECORD_CACHE_HIT( 1, 0 );
		mpCacheHashTable[ixa]->UpdateIfNecessary();

		return mpCacheHashTable[ixa];
	}

	u32 ixb = MakeHashIdxB( ti );
	if( mpCacheHashTable[ixb] && mpCacheHashTable[ixb]->GetTextureInfo() == ti )
	{
		RECORD_CACHE_HIT( 1, 0 );
		mpCacheHashTable[ixb]->UpdateIfNecessary();

		return mpCacheHashTable[ixb];
	}

	CachedTexture * texture = nullptr;
	TextureVec::iterator it = std::lower_bound(mTextures.begin(), mTextures.end(), ti, SSortTextureEntries());
	if (it != mTextures.end() && (*it)->GetTextureInfo() == ti)
	{
		texture = *it;
		RECORD_CACHE_HIT(0, 1);
	}
	else
	{
		// New textures are staged unsorted during the current RDP task. The
		// two-way hash catches common repeats; this scan is only the collision
		// fallback for entries not merged into the main sorted vector yet.
		for (TextureVec::iterator pending = mPendingTextures.begin(); pending != mPendingTextures.end(); ++pending)
		{
			if ((*pending)->GetTextureInfo() == ti)
			{
				texture = *pending;
				break;
			}
		}

		if (texture != nullptr)
		{
			RECORD_CACHE_HIT(0, 1);
		}
		else
		{
			texture = CachedTexture::Create(ti);
			if (texture != nullptr)
				mPendingTextures.push_back(texture);
			RECORD_CACHE_HIT(0, 0);
		}
	}

	// Update the hashtable
	if( texture )
	{
		texture->UpdateIfNecessary();

		mpCacheHashTable[ixa] = texture;
		mpCacheHashTable[ixb] = texture;
	}

	return texture;
}

CRefPtr<CNativeTexture> CTextureCache::GetOrCreateTexture(const TextureInfo & ti)
{
	CachedTexture * base_texture = GetOrCreateCachedTexture(ti);
	if (!base_texture)
		return nullptr;

	return base_texture->GetTexture();
}

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
void CTextureCache::Snapshot(const MutexLock & lock, std::vector< STextureInfoSnapshot > & snapshot) const
{
	DAEDALUS_ASSERT(lock.HasLock(mDebugMutex), "No debug lock");

	snapshot.erase( snapshot.begin(), snapshot.end() );

	for( TextureVec::const_iterator it = mTextures.begin(); it != mTextures.end(); ++it )
	{
		STextureInfoSnapshot	info( (*it)->GetTextureInfo(), (*it)->GetTexture() );
		snapshot.push_back( info );
	}
	for( TextureVec::const_iterator it = mPendingTextures.begin(); it != mPendingTextures.end(); ++it )
	{
		STextureInfoSnapshot	info( (*it)->GetTextureInfo(), (*it)->GetTexture() );
		snapshot.push_back( info );
	}
}
#endif // DAEDALUS_DEBUG_DISPLAYLIST
