#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelBathyField.generated.h"

// ============================================================================
// THE BATHYMETRY FIELD -- getting a baked, per-tile 2D int16 plane to a MATERIAL
// ============================================================================
//
// bake_ver 27 ships two planes per fine tile (docs and voxelcore/tilestore.h):
//
//   bathy_depth -- lake water depth, 10 mm units, -1 == not inside any basin
//   bathy_shore -- SIGNED distance to the nearest shoreline, 100 mm units,
//                  POSITIVE inside water, NEGATIVE on land, +/-100 m saturating
//
// Both are CPU-side, tile-local, compressed and streamed. A material can read
// none of that. This subsystem is the transport, and it is deliberately the
// same shape as the only shipped solution to the identical problem: Epic's
// Water plugin renders a per-zone WATER INFO TEXTURE (RGBA16f, world-space,
// Engine/Shaders/Private/WaterInfoTextureMerge.usf) that the water material
// samples by world XY. We do the same thing with three differences, each forced:
//
//   * CAMERA-CENTRED, not zone-centred. Epic's zones are hand-authored volumes;
//     we have 2,049 baked basins and no authored anything. A window that
//     follows the camera needs no authoring and no per-basin bookkeeping.
//   * FILLED FROM RESIDENT TILES ON THE CPU, not rendered by a scene capture.
//     The data already exists as numbers; rendering it back out of a depth
//     buffer would reintroduce exactly the view dependence baking it removed.
//   * ONE GLOBAL TEXTURE ASSET, not a per-body texture parameter. See "WHY AN
//     ASSET" below -- this is the constraint that shaped the whole design.
//
// --- THE SHAPE ------------------------------------------------------------
//
// A square, axis-aligned, world-space window that follows the camera:
//
//   kSize x kSize texels, one texel per FINE TILE PIXEL (1.875 m), PF_FloatRGBA
//   R = water depth, metres, 0 where dry
//   G = signed distance to shore, metres, + in water, - on land
//   B = validity, 1 where the bake answered and 0 where it did not
//   A = reserved
//
// ONE TEXEL PER SOURCE PIXEL, exactly. The source is 1.875 m/px and nothing
// downstream may pretend otherwise; the sub-pixel precision the material needs
// comes from the SIGNED DISTANCE FIELD's 100 mm LSB, not from a finer raster.
// That is the whole reason the bake ships a signed distance rather than a
// binary mask: the material can find where the field crosses zero to a
// decimetre and put the waterline there, instead of on a 1.875 m raster step.
//
// --- WHY A FULL REFILL RATHER THAN A SCROLLING (TOROIDAL) WINDOW -----------
//
// The obvious optimisation is to address the texture toroidally and refill only
// the band that scrolled in. It was rejected, twice over:
//
//   1. A toroidal window has a WRAP SEAM at world coordinates that are
//      multiples of the window size, and that seam can be anywhere -- including
//      ten metres in front of the camera. Hardware bilinear across it blends
//      two texels that are a kilometre apart in world space. Fixing that means
//      four point taps and a hand-written bilinear in the material, on every
//      water pixel, forever.
//   2. It does not even save the work it looks like it saves. A one-texel band
//      still spans whole 256x256 SOURCE BLOCKS, and a block is the unit of
//      decode -- so scrolling by one texel costs the same eight block decodes a
//      full refill costs. The saving is the upload alone, and at kSize 512 the
//      upload is 2 MB.
//
// So: refill the whole window, synchronously, on the game thread, whenever the
// camera has left the central kRecentreFraction of it. Eight block decodes and
// a 2 MB upload, once per kRecentreFraction * kSize * 1.875 m of travel (120 m
// at the defaults). Affordable because this project's game thread is idle ~75%
// of the frame (docs: the 2K frame anatomy) and the cost lands there, not on
// the render thread that is actually the bottleneck.
//
// SAME-FRAME PUBLICATION, and it is why the fill is synchronous rather than
// async. The pixels and the origin they are relative to MUST change together:
// a frame that samples new pixels through an old origin shifts every lake by up
// to 120 m for one frame, which reads as a flash. Doing both in one Tick makes
// that unrepresentable -- the UpdateTextureRegions render command is enqueued
// before the frame's scene render, and the MPC value is read from game-thread
// state for the same frame.
//
// --- WHY AN ASSET AND NOT A DYNAMIC MATERIAL INSTANCE ----------------------
//
// A UMaterialInstanceDynamic is the normal way to hand a runtime texture to a
// material, and it is not available here. The far-field lake sheet
// (VoxelWaterSheetActor.h:47-52) deliberately assigns the SHARED
// /Game/Voxel/M_WaterVoxel to every section with no MID at all, precisely so the
// sheet and the near-field voxel water cannot diverge; and a Material Parameter
// Collection, which is this project's existing CPU->material channel, cannot
// hold a texture.
//
// So the material samples a NAMED ASSET, /Game/Voxel/T_VoxelBathyInfo, and this
// subsystem overwrites that asset's PIXELS at runtime. Every consumer of the
// texture -- the water material on the sheet, the same material on the pooled
// voxel quads, and the terrain material's wet-shore term -- sees one field with
// no per-consumer plumbing, which is correct: the field IS global.
//
// THE ASSET IS NEVER SAVED AND ITS PLATFORM DATA IS NEVER REBUILT. We only ever
// call UpdateTextureRegions, which writes the RHI texture directly and touches
// neither the package nor the texture's Source. If the asset on disk is not
// already 512x512 PF_FloatRGBA with no mips, this subsystem REFUSES to write
// anything and says so once -- see the guard in Initialize. Silence there would
// mean writing 8-byte pixels into a 4-byte-per-pixel allocation.
//
// --- WHAT IS PUBLISHED, AND WHAT READS IT ----------------------------------
//
// Three MPC_VoxelSky parameters, written every time the window moves:
//
//   BathyFieldOrigin   (vector) world UU of the window's minimum corner, in xy
//   BathyFieldInvSize  (scalar) 1 / (window size in UU) -- so the material's UV
//                      is (WorldXY - Origin) * InvSize, one multiply-add
//   BathyFieldValid    (scalar) 1 when the texture holds a published window,
//                      0 otherwise. Zero must make every consumer fall back.
//
// A CONSUMER MUST HONOUR BOTH BathyFieldValid AND the texture's B CHANNEL. The
// scalar says "there is a window"; the channel says "this cell inside it was
// answered". They fail independently: no fine tiles at all versus a lake at the
// edge of the streamed set. Reading only the first draws unbaked water as
// infinitely shallow; reading only the second draws every lake that way when
// the run has no fine tier.
// ============================================================================

UCLASS()
class VOXELEARTH_API UVoxelBathyFieldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// Texels on a side. One texel per 1.875 m fine tile pixel, so this is also
	// the window's width in source pixels: 512 * 1.875 m = 960 m across, i.e.
	// +/-480 m around the camera. Chosen against the two costs it trades: the
	// upload (512^2 * 8 B = 2 MB) and the decode (512 texels = two 256-px source
	// blocks per axis, so four blocks, times two planes = eight decodes). 1024
	// would quadruple both for reach that the fallback already covers acceptably
	// -- water a kilometre out is reflection-dominated and grazing, which is
	// exactly where a depth-graded volume term contributes least.
	static constexpr int32 kSize = 512;

	// Source pixel pitch, in UE units (1 UU = 1 cm). 1.875 m = 187.5 UU. NOT
	// derived from a resident tile: the fine tier is 1875 mm/px unconditionally
	// (vxc::tilePixelSizeMm(kFineTileScale)), and a window whose scale depended
	// on what happened to be loaded would change size as tiles streamed.
	static constexpr double kTexelUU = 187.5;

	// Refill when the camera leaves the central fraction of the window. 0.25
	// means the camera may wander a quarter of the window (120 m) before we
	// spend anything, and leaves 3/8 of the window (360 m) of valid data ahead
	// of it in the worst direction. Smaller values buy reach and pay refills;
	// larger ones let the useful radius collapse toward the fade band.
	static constexpr double kRecentreFraction = 0.25;

	// Snap the window origin to a multiple of this many texels. Quantising the
	// origin is what stops the field SHIMMERING: with an unsnapped origin every
	// refill lands the raster on a different sub-texel phase, so the bilinear
	// reconstruction of the shoreline moves under a stationary camera. 8 texels
	// is 15 m, well inside the recentre threshold, so it costs no extra refills.
	static constexpr int32 kSnapTexels = 8;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	// --- diagnostics (the HUD/log surface, same convention as the streamer) ---
	//
	// PublishedWindows is the number of refills since start; a value stuck at 0
	// with lakes on screen is the whole diagnosis. HoleFraction is the share of
	// the last window's cells that came back as vxc::kBathyMissing -- expected
	// to be small and nonzero at the edge of the streamed set, and expected to
	// be 1.0 in a run with no fine tier.
	uint64 PublishedWindows() const { return PublishedWindows_; }
	double LastHoleFraction() const { return LastHoleFraction_; }
	double LastFillMs() const { return LastFillMs_; }
	bool IsArmed() const { return bArmed_; }

private:
	// True once the asset has been found and PASSED the size/format/mip guard.
	// False disables everything, permanently, for this world.
	bool bArmed_ = false;
	// Set once we have published at least one window, so BathyFieldValid is only
	// ever raised over pixels that exist.
	bool bPublished_ = false;

	// The asset whose pixels we overwrite. A UPROPERTY so it cannot be collected
	// out from under the render command that is writing to it.
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> InfoTexture_;

	// Window origin, in FINE TILE PIXELS, of the currently published window's
	// minimum corner. Pixel space rather than UU because the snap, the fill and
	// the tile lookup are all integer pixel arithmetic and only the publish step
	// is metric -- keeping the authority in pixels means there is exactly one
	// conversion and it cannot drift.
	int64 OriginPx_ = 0;
	int64 OriginPy_ = 0;

	// Scratch, reused across refills so a 120 m step allocates nothing:
	// the raw int16 planes as sampleBathyRect fills them, and the packed
	// half-float RGBA the RHI wants. Sized once in Initialize.
	TArray<int16> DepthUnits_;
	TArray<int16> ShoreUnits_;
	TArray<FFloat16Color> Pixels_;

	uint64 PublishedWindows_ = 0;
	double LastHoleFraction_ = 1.0;
	double LastFillMs_ = 0.0;
	// Refill accounting is per-world and cheap; this only exists so the "no fine
	// tier in this run" message is logged once rather than every tick.
	bool bLoggedNoStreamer_ = false;

	// Camera XY in UU for this frame, or false when there is no view to follow
	// (no player controller yet, or a world type we do not run in).
	bool GetCameraXY(double& OutX, double& OutY) const;
	// Fills Pixels_ for a window whose minimum corner is the given fine pixel,
	// and returns the fraction of cells that had no baked answer.
	double FillWindow(int64 Px0, int64 Py0);
	// Uploads Pixels_ and writes the three MPC parameters. Same tick, always.
	void PublishWindow(int64 Px0, int64 Py0);
	// Drops BathyFieldValid to 0 so every consumer falls back. Called when we
	// disarm and on Deinitialize -- a stale window left published over a world
	// that no longer has it is worse than no window at all.
	void PublishInvalid();
};
