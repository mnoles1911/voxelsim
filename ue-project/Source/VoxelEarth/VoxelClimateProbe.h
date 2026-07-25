#pragma once

// Climate -> vertex colour, shared by the two things that draw terrain.
//
// WHY THIS EXISTS
// ---------------
// Every .vxtl tile carries four uint8 climate planes (temperature, seasonality,
// precipitation, precipitation variability) and until now NOTHING on the UE side
// consumed any of them -- a grep for climate/temperature/precip across
// Source/VoxelEarth returned exactly one comment. voxel-core reads them
// internally to pick a surface material id; at the time that classifier was
// degenerate on real diffusion tiles, so the material id alone could not drive
// appearance. Worldgen v8 fixed the classifier (see the v8 note further down),
// but this probe still earns its place: it hands climate to the renderers as a
// CONTINUOUS pair for smooth LUT blending, which a discrete material id cannot
// do however correct that id is.
//
// It is also what keeps AVoxelClipmapActor (the 50 km vista) and
// UVoxelChunkComponent (the near field) in agreement: both encode climate into
// vertex colour through the SAME two functions here, and both materials decode
// it through the same T_VoxelBiomeLUT. The seam cannot drift because there is
// only one mapping.
//
// FILE OWNERSHIP: this is a new file. It reads voxel-core headers (tiles.h,
// tilestore.h) but modifies nothing there, and it does not touch
// VoxelWorldSubsystem.* -- which is why it loads its own TileGridSampler rather
// than borrowing the subsystem's. Cost of that duplication is ~1.5 MB per tile
// (37 MB for the 25-tile set), read-only and immutable after init. The
// subsystem already loads the grid twice today (world + water), so this is a
// third copy of a pattern the project already accepts; folding all three into
// one shared sampler is a worthwhile follow-up that belongs in the file this
// change is not allowed to touch.

#include "CoreMinimal.h"

// The wire encoding the u8 window below is derived from. voxel-core is
// UE-header-free by doctrine, so this dependency runs one way only.
#include "voxelcore/climate.h"

// Encoded climate for one column, ready to drop into an FColor.
struct FVoxelClimateBytes
{
	// Remapped to 0..255 across this world's actual p1..p99 range -- NOT the raw
	// tile bytes. See kTempU8Lo etc.
	uint8 Temperature = 128;
	uint8 Precipitation = 128;
};

namespace VoxelClimate
{
	// Remap window, in RAW tile u8 units, measured over all 25 tiles of
	// tile-cache/terrain-diffusion-unlabeled-3e11cf157a836c70/000000000135276f/s1
	// (6.55M land pixels, elevation > 0):
	//
	//   temperature    u8 p1=100  p50=138  p99=189   -> -8.6 .. +19.3 degC
	//   precipitation  u8 p1= 14  p50= 22  p99= 32   ->  659 .. 1506 mm/yr
	//
	// Precipitation occupies a 19-value window out of 256 because terrain-service
	// quantizes bio_12 against a 0..12000 mm/yr full scale (diffusion.py
	// EXPECTED_CHANNELS) while this world is cool-temperate maritime. Feeding the
	// raw byte to a shader would waste 92% of the precision and, worse, is why
	// voxel-core biome.h classifies the ENTIRE world as DESERT: its
	// kBiomePrecipAridU8 = 60 sits above this world's maximum of 33.
	//
	// MUST MATCH ue-project/Tools/gen_terrain_textures.py's TEMP_U8_LO/HI and
	// PRECIP_U8_LO/HI exactly -- that script bakes the same window into
	// T_VoxelBiomeLUT's axes, and a mismatch silently shifts every biome. Each
	// file names the other as the second copy.
	//
	// DERIVED, NOT MEASURED, since worldgen v8. These were hand-measured u8
	// literals, which made this a THIRD independent climate calibration
	// alongside biome.h and gen_terrain_textures.py's LUT -- and four such
	// calibrations drifting apart is exactly what let the whole world classify
	// as desert. They are now the physical window converted through
	// voxelcore/climate.h, the same header biome.h's thresholds come from, so a
	// change to the wire quantization carries here automatically instead of
	// silently shifting the LUT axes.
	//
	// The physical endpoints are chosen to reproduce the measured values
	// EXACTLY (asserted below), so this change is a provable no-op on rendered
	// output -- a screenshot pair is its whole test. It is a display STRETCH,
	// not a classification: its job is to use the full 8-bit LUT axis for a
	// region that occupies ~7% of the wire range, and that job does not go away
	// just because biome.h is now correct.
	inline constexpr int32 kTempU8Lo = vxc::climateTempU8FromMilliC(-8'600);   // -8.6 C
	inline constexpr int32 kTempU8Hi = vxc::climateTempU8FromMilliC(19'200);   // +19.2 C
	inline constexpr int32 kPrecipU8Lo = vxc::climatePrecipU8FromMmPerYr(659);  // 659 mm/yr
	inline constexpr int32 kPrecipU8Hi = vxc::climatePrecipU8FromMmPerYr(1506); // 1506 mm/yr

	static_assert(kTempU8Lo == 100 && kTempU8Hi == 189,
	              "the temperature LUT axis must not move -- if this fires, either the "
	              "climate quantization range changed or someone retuned the window; "
	              "either way T_VoxelBiomeLUT must be regenerated in the same commit");
	static_assert(kPrecipU8Lo == 14 && kPrecipU8Hi == 32,
	              "the precipitation LUT axis must not move -- see above");

	// Samples the tile climate planes at a world XY (unreal units) and returns
	// the remapped bytes. Bilinear across tile pixels, so the 30 m raster does
	// not show up as 30 m colour blocks.
	//
	// Thread-safe: the underlying TileGridSampler is immutable after init and its
	// queries are pure reads. Safe to call from the meshing worker tasks.
	// Outside the loaded tile box (or with no -VoxelTileDir) it returns the
	// neutral 128/128, which lands mid-LUT rather than at a corner.
	FVoxelClimateBytes SampleClimateAtWorldUU(double WorldXUU, double WorldYUU);

	// Called once, early, from the game thread. Reads -VoxelTileDir /
	// -VoxelTileScale / -VoxelSeed exactly as VoxelWorldSubsystem does and loads
	// the same tiles. Idempotent; safe to call again.
	void EnsureInitialized();

	// Diagnostics for the log line, so a run can prove climate actually reached
	// the renderer rather than silently falling back to neutral.
	int32 GetLoadedTileCount();

	// Vertex colour R: 255 if this material id's appearance comes from the
	// surface biome, 0 if it keeps a subsurface (rock/soil) look.
	//
	// WHY A BINARY 0/255 AND NOT THE MATERIAL ID ITSELF
	// -------------------------------------------------
	// R used to carry the raw vxc::MaterialId for the material to index a
	// 16-entry palette texture and threshold into "is this a surface material".
	// That did not survive contact with the shader. An exposure-proof probe
	// (unlit emissive, with a known 0.5 reference in G so auto-exposure cancels
	// in the ratio) measured VertexColor.R * 255 arriving as ~6-8 where the CPU
	// had written 4, and every id threshold read 0 -- the tint weight rendered
	// pure black across the entire frame (p99 = 0). Whatever transform sits
	// between the FColor byte and the shader, thresholding a categorical id
	// through it is not reliable.
	//
	// 0 and 255 are the fixed points of any monotonic per-channel transform, so
	// a binary flag is exact regardless. That is the property being bought here,
	// and it is why this is worth the cost below.
	//
	// COST, STATED PLAINLY: per-material subsurface strata are gone. A cave wall
	// is one rock colour instead of bedrock/rock/gravel/subsoil/clay each having
	// their own. That is a real regression against the original design, though
	// not against main -- today every surface underground is the same flat beige
	// anyway. Restoring strata needs the R-channel transform understood first.
	//
	// WHY THIS IS GEOMETRIC AND NOT MATERIAL-ID BASED
	// -----------------------------------------------
	// It was material-id based, keyed off a table mirroring terrain_palette.py's
	// BIOME_TINT column. Then the ids voxel-core actually emits were MEASURED
	// (-VoxelMatHistogram, 2M quads, the real 25-tile diffusion set):
	//
	//     MAT_ROCK    (2)  302,018   15%
	//     MAT_SUBSOIL (5) 1,697,983  85%
	//     ...and nothing else. No surface material at all.
	//
	// FIXED AT WORLDGEN v8 -- this note is kept because the reasoning below is
	// still why the R channel is geometric, but its premise no longer holds.
	// The cause was the topsoil formula subtracting an absolute slope term that
	// swamped its base, so topsoilMm was 0 on 91% of land and stratigraphyAt
	// never reached col.surfaceMat. It now erodes by a retained fraction with a
	// one-voxel floor. Measured with vxc_climateprobe over the same 25 tiles:
	// zero-topsoil land 91.3% -> 0.00%, and the top voxel carries its biome's
	// surface material on 12.4% -> 100.00% of columns, spread across mud, sand,
	// grass, topsoil, podzol, permafrost and rock.
	//
	// So an id-keyed rule COULD work now, and restoring per-material subsurface
	// strata is worth revisiting. What has NOT been redone is the in-engine
	// -VoxelMatHistogram run that produced the numbers above -- the figures here
	// are the CPU-side equivalent (top-voxel census), not a quad census. Re-run
	// the switch before trusting an id-keyed appearance rule.
	//
	// Not one quad carries MAT_GRASS / MAT_TOPSOIL / MAT_SAND / MAT_PODZOL or
	// any other surface id, so no id-keyed rule can distinguish "hillside" from
	// "cave wall" on this data. (An earlier draft of this work asserted the
	// world was all MAT_SAND, reasoning from biome.h's thresholds rather than
	// measuring -- that was wrong, and the histogram switch exists so the next
	// person measures instead.) Whatever picks the surface material in
	// voxel-core is not reaching these quads; that is voxel-core's to fix and
	// this change does not touch it.
	//
	// So surface-ness comes from FACE DIRECTION instead, which is always
	// available and always correct:
	//   +Z (upward, exposed to sky)  -> full biome colour
	//   side faces                   -> partial; a 10 cm step riser on a grassy
	//                                   slope is neither pure turf nor pure
	//                                   rock, and a hard 0 here stripes every
	//                                   hillside light/dark at voxel pitch
	//   -Z (downward, ceilings)      -> none; rock
	//
	// MaterialId is still taken so the rule can grow back a material term once
	// voxel-core emits meaningful surface ids; it is deliberately unused today
	// rather than silently dropped from the signature.
	//
	// Returns 0..255 for VertexColor.R. 0 and 255 are exact under any monotonic
	// per-channel transform (see the measurement note above); the mid value only
	// controls a blend ratio, so its precision does not matter.
	uint8 BiomeTintForFace(uint8 MaterialId, int32 FaceAxis, bool bFacePositive);
}
