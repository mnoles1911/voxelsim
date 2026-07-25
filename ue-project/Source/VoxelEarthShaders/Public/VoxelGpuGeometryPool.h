// Suballocator for the persistent GPU geometry pool (ADR-0006, G2).
//
// THE PROBLEM THIS SOLVES. Today every voxel chunk owns its own component, its
// own vertex/index buffers, and its own scene primitive — so streaming a chunk
// in or out means FScene::AddPrimitive / RemovePrimitive, and that funnel is
// what ADR-0006 measured as the frame-time ceiling. Under G2 there is ONE big
// GPU buffer of packed quads and ONE primitive. Streaming a chunk in becomes
// "find a free run of quads in that buffer and fill it"; streaming out becomes
// "give the run back". Neither touches the renderer's primitive list.
//
// That makes this class the thing standing between the streaming system and
// the scene: if it hands out overlapping ranges, two chunks silently corrupt
// each other's geometry and the symptom is flickering terrain, not a crash.
// Hence the deliberately boring implementation and the real test coverage.
//
// DESIGN: first-fit over an offset-sorted free list that is ALWAYS fully
// coalesced. Allocation granularity is one quad (8 bytes) and every allocation
// is contiguous, so a chunk's geometry can be drawn as a single range.
//
// Not thread-safe. Call it from one thread (the streaming path owns it).

#pragma once

#include "CoreMinimal.h"

// A quad-range handout. Offset is in QUADS, not bytes.
struct FVoxelGpuPoolAllocation
{
	static constexpr uint32 kInvalidOffset = TNumericLimits<uint32>::Max();

	uint32 Offset = kInvalidOffset;
	uint32 NumQuads = 0;

	bool IsValid() const { return Offset != kInvalidOffset; }
};

class VOXELEARTHSHADERS_API FVoxelGpuGeometryPool
{
public:
	// CapacityQuads is the total size of the backing GPU buffer, in quads.
	// For scale: the whole live 2 km cascade measured 9,441,170 quads
	// (docs/gpu-g0-sizing.md), which is 75.5 MB at 8 bytes per quad.
	void Init(uint32 InCapacityQuads);

	// Hands out a contiguous run of NumQuads. Returns an invalid allocation if
	// no single free run is large enough — note that this can happen while
	// plenty of total space remains, which is exactly what fragmentation is.
	// Callers must check IsValid(); there is no partial success.
	FVoxelGpuPoolAllocation Alloc(uint32 NumQuads);

	// Returns a run to the pool and coalesces it with any adjacent free runs.
	// Double-freeing or freeing a range that was never handed out corrupts the
	// free list, so both are checked.
	void Free(const FVoxelGpuPoolAllocation& Allocation);

	// Drops every allocation. The backing buffer is untouched.
	void Reset();

	uint32 GetCapacityQuads() const { return CapacityQuads; }
	uint32 GetUsedQuads() const { return UsedQuads; }
	uint32 GetFreeQuads() const { return CapacityQuads - UsedQuads; }

	// The largest run Alloc could currently satisfy. The gap between this and
	// GetFreeQuads() IS the fragmentation, and it is the number worth watching
	// in a HUD: when it grows, allocations start failing while the pool still
	// looks half empty.
	uint32 GetLargestFreeRun() const;

	// Number of separate free runs. 1 means perfectly unfragmented.
	int32 GetFreeRunCount() const { return FreeRuns.Num(); }

	// Highest quad index ever handed out plus one. Everything above this has
	// never been written, so a draw only has to cover [0, HighWaterMark).
	uint32 GetHighWaterMark() const { return HighWaterMark; }

	// Verifies the free list is sorted, non-overlapping, fully coalesced, and
	// that the used-quad tally agrees with it. Cheap enough for tests and for
	// a debug command; not meant for every frame.
	bool CheckInvariants(FString& OutError) const;

private:
	struct FRun
	{
		uint32 Offset = 0;
		uint32 Size = 0;

		uint32 End() const { return Offset + Size; }
	};

	// Sorted by Offset, never overlapping, never adjacent (adjacency is always
	// merged on Free). Those three properties together are what make first-fit
	// correct and GetLargestFreeRun a simple max.
	TArray<FRun> FreeRuns;

	uint32 CapacityQuads = 0;
	uint32 UsedQuads = 0;
	uint32 HighWaterMark = 0;
};
