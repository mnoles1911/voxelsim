#pragma once
// vxc::Counters -- plain, engine-free performance counters (docs/debug-tooling-plan.md
// P1 "Instrumentation plumbing" row: "a plain vxc::Counters struct (atomics,
// header-only)"). Header-only, no UE types, and voxel-core's own internals
// stay untouched this pass by design -- the UE layer instantiates one of
// these and increments it AROUND its calls into voxel-core (worker mesh
// jobs, the edit-log apply path), rather than voxel-core incrementing itself.
//
// Every counter is a monotonically-increasing cumulative total for the
// process lifetime (mirrors the existing UE-layer counters
// TotalChunksLoaded/TotalQuadsLoaded in VoxelWorldSubsystem.cpp); the UE perf
// HUD/stat group derives per-second rates by sampling once per second and
// diffing against the previous sample.

#include <atomic>
#include <cstdint>

namespace vxc {

struct Counters {
    std::atomic<uint64_t> bricksGenerated{0}; // bricks meshed (worker + game-thread paths)
    std::atomic<uint64_t> cellsWritten{0};    // voxel cells written/sampled while building a brick's sampler grid
    std::atomic<uint64_t> quadsEmitted{0};    // mesher output quads
    std::atomic<uint64_t> editsApplied{0};    // per-brick World::applyEdit calls (edit-log authority path)
    std::atomic<uint64_t> columnEvals{0};     // Amplifier::column evaluations

    void incBricksGenerated(uint64_t n = 1) { bricksGenerated.fetch_add(n, std::memory_order_relaxed); }
    void incCellsWritten(uint64_t n = 1) { cellsWritten.fetch_add(n, std::memory_order_relaxed); }
    void incQuadsEmitted(uint64_t n = 1) { quadsEmitted.fetch_add(n, std::memory_order_relaxed); }
    void incEditsApplied(uint64_t n = 1) { editsApplied.fetch_add(n, std::memory_order_relaxed); }
    void incColumnEvals(uint64_t n = 1) { columnEvals.fetch_add(n, std::memory_order_relaxed); }

    uint64_t getBricksGenerated() const { return bricksGenerated.load(std::memory_order_relaxed); }
    uint64_t getCellsWritten() const { return cellsWritten.load(std::memory_order_relaxed); }
    uint64_t getQuadsEmitted() const { return quadsEmitted.load(std::memory_order_relaxed); }
    uint64_t getEditsApplied() const { return editsApplied.load(std::memory_order_relaxed); }
    uint64_t getColumnEvals() const { return columnEvals.load(std::memory_order_relaxed); }

    // Zeroes every counter. Not used by the UE layer in steady state (it
    // wants cumulative process-lifetime totals to diff against), but useful
    // for tests and for a future "reset stats" debug command.
    void reset() {
        bricksGenerated.store(0, std::memory_order_relaxed);
        cellsWritten.store(0, std::memory_order_relaxed);
        quadsEmitted.store(0, std::memory_order_relaxed);
        editsApplied.store(0, std::memory_order_relaxed);
        columnEvals.store(0, std::memory_order_relaxed);
    }
};

} // namespace vxc
