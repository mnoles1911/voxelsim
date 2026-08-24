// VoxelRingOrder.cpp -- the session table cache. See VoxelRingOrder.h for why
// the ordering exists and what its precision bound is.
//
// Deliberately the ONLY thing in this file. Everything with a decision in it
// lives in the header, so tools/test-voxel-ring-order.cpp compiles the same
// code the engine runs rather than a copy of it -- the rule that cost this
// project three green probes measuring a world the engine was not running.

#include "VoxelRingOrder.h"

#include <memory>

namespace VoxelRingOrder
{
	const FTable* Get(int32_t Span)
	{
		if (Span < 0 || Span > kMaxCachedSpan)
		{
			return nullptr; // caller falls back to row-major AND counts it
		}
		// Game thread only (every caller is inside RecomputeDesiredSet), so a
		// plain vector of owned tables needs no lock. Indexed by span: spans
		// are small and dense across a cascade, and a direct index keeps the
		// lookup off the profile entirely.
		static std::vector<std::unique_ptr<FTable>> Cache;
		const size_t Index = size_t(Span);
		if (Cache.size() <= Index)
		{
			Cache.resize(Index + 1);
		}
		if (!Cache[Index])
		{
			Cache[Index] = std::make_unique<FTable>(Span);
		}
		return Cache[Index].get();
	}
} // namespace VoxelRingOrder
