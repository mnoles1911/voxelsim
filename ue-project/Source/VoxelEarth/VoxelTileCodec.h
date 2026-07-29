#pragma once

// The host boundary for `.vxtl` v2 CODEC_ZSTD (docs/vxtl-v2-format.md §3).
//
// WHAT THIS FILE IS FOR
// ---------------------
// voxel-core does not, and must not, contain zstd. It has zero third-party
// dependencies by doctrine, and it is compiled into a static library that is
// linked into this UE module -- so a zstd vendored inside it would be a SECOND
// copy of zstd's symbols in one binary if the engine or any plugin brings its
// own. That is an ODR/symbol-collision hazard, not merely bloat, and the
// failure mode of a silently-picked-wrong ZSTD_decompress is wrong terrain
// rather than a link error.
//
// So voxel-core takes an INJECTED decompressor (vxc::FineDecompressor in
// voxelcore/tilestore.h) and this module supplies it. voxel-core still builds,
// tests and ships standalone with no compression library present at all;
// CODEC_RAW tiles never touch any of this.
//
// WHERE THE ZSTD COMES FROM -- READ THIS BEFORE ASSUMING
// -----------------------------------------------------
// The plan assumed "UE 5.8 already ships zstd in Engine/Source/ThirdParty".
// That was CHECKED against the installed UE 5.8 (D:\UE_5.8, 5.8.0-55116800)
// and it is NOT TRUE of the binary/launcher distribution: that tree has no
// zstd.h, no zstd*.lib, no zstd module, and no .Build.cs anywhere under
// Engine/Source or Engine/Plugins that references one. What it does ship is
// Oodle and LZ4 (Runtime/Core/Public/Compression), a C# ZstdSharp.dll used by
// UnrealBuildTool/AutomationTool, and a handful of zstd .tps/LICENSE records
// belonging to other prebuilt third-party libraries. A from-source engine
// build, or a plugin that brings zstd, may well have one -- but nothing may
// assume it.
//
// That does not weaken the injection design; it strengthens the reason for it.
// voxel-core stays out of the argument entirely, and WHICH zstd this binary
// uses is a decision made here, in one place, at the boundary:
//
//   * VoxelEarth.Build.cs looks for a zstd module (engine ThirdParty first,
//     then a project-local ue-project/Source/ThirdParty/zstd) and defines
//     VOXELEARTH_WITH_ZSTD=1 only when it actually finds one;
//   * with it, this file wires ZSTD_decompress in and registers it at module
//     startup;
//   * without it, NOTHING is registered, and a CODEC_ZSTD tile is refused by
//     vxc::FineTile::parse with vxc::FineError::kNoDecompressor -- loudly,
//     whole, and never as a tile full of zeros. Silently flat terrain under a
//     client that believes it has the fine tier is a desync, not a glitch.
//
// Registering an alternative (a plugin's zstd, a test double) is a one-liner
// through SetFineTileDecompressor below.

#include "CoreMinimal.h"

// voxel-core is UE-header-free C++20. This header is not UHT-parsed (no
// UCLASS/USTRUCT), so including it here is safe.
#include "voxelcore/tilestore.h"

namespace VoxelEarth
{
	/**
	 * The process-wide fine-tile decompressor. Empty (fn == nullptr) when this
	 * build has no zstd -- callers hand it to vxc::FineTile::parse or
	 * vxc::FineTileSampler::setDecompressor either way, and an empty one means
	 * CODEC_ZSTD tiles are refused rather than mis-decoded.
	 *
	 * Thread-safe to READ from anywhere once the module has started; treat it
	 * as immutable after startup (see SetFineTileDecompressor).
	 */
	VOXELEARTH_API vxc::FineDecompressor GetFineTileDecompressor();

	/** True when this build can actually decode CODEC_ZSTD tiles. */
	VOXELEARTH_API bool HasFineTileDecompressor();

	/**
	 * Replaces the registered decompressor. For a plugin that brings its own
	 * zstd, or for a test double.
	 *
	 * Call it during startup, BEFORE any tile is loaded: a vxc::FineTile keeps
	 * the decompressor it was parsed with (blocks decode lazily, so the
	 * callback has to stay reachable for the tile's whole life), which makes
	 * this deliberately non-retroactive. Whatever `user` points at must outlive
	 * every tile parsed with it.
	 */
	VOXELEARTH_API void SetFineTileDecompressor(const vxc::FineDecompressor& Decompressor);

	/**
	 * Logs, once, which decompressor this build ended up with and what that
	 * means for CODEC_ZSTD tiles. Called from FVoxelEarthModule::StartupModule
	 * so the answer is in every log, rather than being discovered the first
	 * time a fine tile fails to load.
	 */
	VOXELEARTH_API void LogFineTileCodecStatus();
} // namespace VoxelEarth
