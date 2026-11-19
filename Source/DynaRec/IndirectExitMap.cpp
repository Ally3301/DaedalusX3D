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
#include "IndirectExitMap.h"

#include "DynaRecProfile.h"
#include "FragmentCache.h"
#include "Fragment.h"

#include "Debug/DBGConsole.h"


//

CIndirectExitMap::CIndirectExitMap()
:	mpCache( nullptr )
,	mMruAddr0( 0 )
,	mMruTarget0( nullptr )
,	mMruAddr1( 0 )
,	mMruTarget1( nullptr )
{
}


//

CIndirectExitMap::~CIndirectExitMap()
{
}


//

CFragment *	CIndirectExitMap::LookupIndirectExit( u32 exit_address )
{
	#ifdef DAEDALUS_DEBUG_CONSOLE
	DAEDALUS_ASSERT( mpCache != nullptr, "Why do we have no cache?" );
	#endif
	CFragment * p( mpCache->LookupFragmentQ( exit_address ) );

	DYNAREC_PROFILE_LOGLOOKUP( exit_address, p );

	return p;
}


//

const void * CIndirectExitMap::LookupIndirectTarget( u32 exit_address )
{
	if (mMruTarget0 != nullptr && mMruAddr0 == exit_address)
		return mMruTarget0;
	if (mMruTarget1 != nullptr && mMruAddr1 == exit_address)
	{
		// Promote hit to slot 0
		const u32 a0 = mMruAddr0; const void * t0 = mMruTarget0;
		mMruAddr0 = mMruAddr1; mMruTarget0 = mMruTarget1;
		mMruAddr1 = a0;        mMruTarget1 = t0;
		return mMruTarget0;
	}

	CFragment * p_fragment( LookupIndirectExit( exit_address ) );
	if (p_fragment != nullptr)
	{
		const void * target = p_fragment->GetEntryTarget().GetTarget();
		if (target != nullptr)
		{
			mMruAddr1 = mMruAddr0; mMruTarget1 = mMruTarget0;
			mMruAddr0 = exit_address; mMruTarget0 = target;
		}
		return target;
	}
	return nullptr;
}

extern "C"
{

const void *	R4300_CALL_TYPE IndirectExitMap_Lookup( CIndirectExitMap * p_map, u32 exit_address )
{
	return p_map->LookupIndirectTarget( exit_address );
}

}
