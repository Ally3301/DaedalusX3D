/*
Copyright (C) 2020 MasterFeizz

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
#include <stdio.h>
#include <malloc.h>

#ifdef DAEDALUS_CTR
#include "SysCTR/Utility/MemoryCTR.h"
#endif

#include "DynaRec/CodeBufferManager.h"
#include "Debug/DBGConsole.h"
#include "CodeGeneratorARM.h"

// Old 3DS uses a 6 MiB primary cache and 2 MiB secondary cache (for cold/slow
// paths such as exceptions and memory fallbacks) so hot traces survive long
// gameplay sessions without running out of process heap under 64 MiB .3dsx.
static const u32 OLD3DS_PRIMARY_CODE_BUFFER_SIZE   = 6 * 1024 * 1024;
static const u32 OLD3DS_SECONDARY_CODE_BUFFER_SIZE = 2 * 1024 * 1024;
static const u32 NEW3DS_PRIMARY_CODE_BUFFER_SIZE   = 8 * 1024 * 1024;
static const u32 NEW3DS_SECONDARY_CODE_BUFFER_SIZE = 2 * 1024 * 1024;

extern bool isN3DS;

class CCodeBufferManagerARM : public CCodeBufferManager
{
public:
	CCodeBufferManagerARM()
		:	mpBuffer( NULL )
		,	mBufferPtr( 0 )
		,	mBufferSize( 0 )
		,	mpSecondBuffer( NULL )
		,	mSecondBufferPtr( 0 )
		,	mSecondBufferSize( 0 )
		,	mInitialiseError( 0 )
	{
	}

	virtual bool			Initialise();
	virtual int				InitialiseError() const { return mInitialiseError; }
	virtual void			Reset();
	virtual bool			HasSpaceForBlock(u32 reserveBytes) const;
	virtual void			Finalise();

	virtual CCodeGenerator *StartNewBlock();
	virtual u32				FinaliseCurrentBlock();

private:

	u8	*					mpBuffer;
	u32						mBufferPtr;
	u32						mBufferSize;

	u8 *					mpSecondBuffer;
	u32						mSecondBufferPtr;
	u32						mSecondBufferSize;

	int						mInitialiseError;

private:
	CAssemblyBuffer			mPrimaryBuffer;
	CAssemblyBuffer			mSecondaryBuffer;
};

//*****************************************************************************
//
//*****************************************************************************
CCodeBufferManager *	CCodeBufferManager::Create()
{
	return new CCodeBufferManagerARM;
}

//*****************************************************************************
//
//*****************************************************************************
bool	CCodeBufferManagerARM::Initialise()
{
	mInitialiseError = 0;

	#ifdef DAEDALUS_CTR
	const u32 primary_size   = isN3DS ? NEW3DS_PRIMARY_CODE_BUFFER_SIZE   : OLD3DS_PRIMARY_CODE_BUFFER_SIZE;
	const u32 secondary_size = isN3DS ? NEW3DS_SECONDARY_CODE_BUFFER_SIZE : OLD3DS_SECONDARY_CODE_BUFFER_SIZE;
	const u32 total_size     = primary_size + secondary_size;
	mBufferSize = primary_size;
	mSecondBufferSize = secondary_size;
	mpBuffer = (u8*)memalign(4096, total_size);
	if (mpBuffer == NULL)
	{
		mInitialiseError = -1;
		mBufferSize = 0;
		mSecondBufferSize = 0;
		return false;
	}

	mpSecondBuffer = mpBuffer + primary_size;
	int permission = _SetMemoryPermission(mpBuffer, total_size, 7);
	if (permission < 0)
	{
		mInitialiseError = permission;
		free(mpBuffer);
		mpBuffer = NULL;
		mpSecondBuffer = NULL;
		mBufferSize = 0;
		mSecondBufferSize = 0;
		return false;
	}
	#endif

	mBufferPtr = 0;
	mSecondBufferPtr = 0;
#ifndef DAEDALUS_CTR
	mBufferSize = 0;
	mSecondBufferSize = 0;
#endif

	return true;
}

//*****************************************************************************
//
//*****************************************************************************
void	CCodeBufferManagerARM::Reset()
{
	mBufferPtr = 0;
	mSecondBufferPtr = 0;
}

// Keep unused headroom for a complete trace and its literal pool. Dynamo
// requests a cache reset before either fixed-size region can be exhausted.
bool CCodeBufferManagerARM::HasSpaceForBlock(u32 reserveBytes) const
{
#ifdef DAEDALUS_CTR
	u32 secondary_reserve = reserveBytes / 4;
	if (secondary_reserve < 128 * 1024)
		secondary_reserve = 128 * 1024;
	if (mpBuffer == NULL || mpSecondBuffer == NULL ||
		reserveBytes > mBufferSize || secondary_reserve > mSecondBufferSize)
		return false;

	return mBufferPtr <= mBufferSize - reserveBytes &&
		mSecondBufferPtr <= mSecondBufferSize - secondary_reserve;
#else
	(void)reserveBytes;
	return true;
#endif
}

//*****************************************************************************
//
//*****************************************************************************
void	CCodeBufferManagerARM::Finalise()
{
	if (mpBuffer != NULL && mpSecondBuffer != NULL)
	{
		#ifdef DAEDALUS_CTR
		// Finalise at cache destruction; free even if permission reset fails.
		_SetMemoryPermission(mpBuffer, mBufferSize + mSecondBufferSize, 3);
		//_SetMemoryPermission((unsigned int*)mpSecondBuffer, CODE_BUFFER_SIZE / 4096, 3);
		#endif
		
		free(mpBuffer);
		//free(mpSecondBuffer);

		mpBuffer = NULL;
		mpSecondBuffer = NULL;
	}
}

//*****************************************************************************
//
//*****************************************************************************
CCodeGenerator * CCodeBufferManagerARM::StartNewBlock()
{
	// Round up to 16 byte boundry
	u32 aligned_ptr( (mBufferPtr + 15) & (~15) );

	mBufferPtr = aligned_ptr;

	u32 aligned_ptr2((mSecondBufferPtr + 15) & (~15));
	mSecondBufferPtr = aligned_ptr2;

	mPrimaryBuffer.SetBuffer( mpBuffer + mBufferPtr );
	mSecondaryBuffer.SetBuffer( mpSecondBuffer + mSecondBufferPtr );

	return new CCodeGeneratorARM( &mPrimaryBuffer, &mSecondaryBuffer );
}

//*****************************************************************************
//
//*****************************************************************************
u32 CCodeBufferManagerARM::FinaliseCurrentBlock()
{
	const u32 primary_start = mBufferPtr;
	const u32 secondary_start = mSecondBufferPtr;
	const u32 main_block_size( mPrimaryBuffer.GetSize() );
	const u32 secondary_block_size( mSecondaryBuffer.GetSize() );

	mBufferPtr += main_block_size;
	mSecondBufferPtr += secondary_block_size;

	#ifdef DAEDALUS_CTR
	if (main_block_size > 0)
		_InvalidateAndFlushCacheRange(mpBuffer + primary_start, main_block_size);
	if (secondary_block_size > 0)
		_InvalidateAndFlushCacheRange(mpSecondBuffer + secondary_start, secondary_block_size);
	#endif
	
	return main_block_size;
}
