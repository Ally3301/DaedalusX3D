/*
 * Tracks the RDRAM ranges covered by translated N64 code.
 *
 * The previous 4 KiB boolean map caused a write anywhere in the same page as
 * translated code to flush the whole dynarec. A 64-byte bitmap keeps that
 * conservative invalidation safe while avoiding most unrelated page writes.
 * Physical, KSEG0 and KSEG1 addresses are normalized to the same RDRAM offset.
 */
#pragma once

#include "Utility/DaedalusTypes.h"
#include <string.h>

class CFragmentCacheCoverage
{
public:
	CFragmentCacheCoverage() { Reset(); }

	void ExtendCoverage(u32 address, u32 length)
	{
		u32 offset = 0;
		if (length == 0 || !AddressToOffset(address, &offset))
			return;

		u64 end = static_cast<u64>(offset) + static_cast<u64>(length);
		if (end > RDRAM_SIZE)
			end = RDRAM_SIZE;
		if (end <= offset)
			return;

		SetBlocks(offset >> BLOCK_SHIFT, static_cast<u32>((end - 1) >> BLOCK_SHIFT));
	}

	bool IsCovered(u32 address, u32 length) const
	{
		u32 offset = 0;
		if (length == 0 || !AddressToOffset(address, &offset))
			return false;

		u64 end = static_cast<u64>(offset) + static_cast<u64>(length);
		if (end > RDRAM_SIZE)
			end = RDRAM_SIZE;
		if (end <= offset)
			return false;

		const u32 first = offset >> BLOCK_SHIFT;
		const u32 last = static_cast<u32>((end - 1) >> BLOCK_SHIFT);
		const u32 first_word = first >> 5;
		const u32 last_word = last >> 5;

		for (u32 word = first_word; word <= last_word; ++word)
		{
			const u32 low = word == first_word ? (first & 31u) : 0u;
			const u32 high = word == last_word ? (last & 31u) : 31u;
			u32 mask = 0xffffffffu << low;
			if (high != 31u)
				mask &= (1u << (high + 1u)) - 1u;
			if ((mCoverage[word] & mask) != 0)
				return true;
		}
		return false;
	}

	void Reset()
	{
		memset(mCoverage, 0, sizeof(mCoverage));
	}

private:
	static const u32 RDRAM_SIZE = 8u * 1024u * 1024u;
	static const u32 BLOCK_SHIFT = 6; // 64-byte blocks
	static const u32 BLOCK_COUNT = RDRAM_SIZE >> BLOCK_SHIFT;
	static const u32 WORD_COUNT = BLOCK_COUNT >> 5;

	static bool AddressToOffset(u32 address, u32 *offset)
	{
		// Physical RDRAM, KSEG0 (cached) and KSEG1 (uncached) aliases.
		if (address < RDRAM_SIZE)
		{
			*offset = address;
			return true;
		}

		const u32 segment = address & 0xe0000000u;
		if (segment != 0x80000000u && segment != 0xa0000000u)
			return false;

		const u32 physical = address & 0x1fffffffu;
		if (physical >= RDRAM_SIZE)
			return false;

		*offset = physical;
		return true;
	}

	void SetBlocks(u32 first, u32 last)
	{
		const u32 first_word = first >> 5;
		const u32 last_word = last >> 5;
		for (u32 word = first_word; word <= last_word; ++word)
		{
			const u32 low = word == first_word ? (first & 31u) : 0u;
			const u32 high = word == last_word ? (last & 31u) : 31u;
			u32 mask = 0xffffffffu << low;
			if (high != 31u)
				mask &= (1u << (high + 1u)) - 1u;
			mCoverage[word] |= mask;
		}
	}

	u32 mCoverage[WORD_COUNT];
};
