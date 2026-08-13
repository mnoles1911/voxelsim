#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelRippleField.generated.h"

// ============================================================================
// THE RIPPLE FIELD -- local interactive ripples on the water surface
// ============================================================================
//
// WHAT IT DOES, in one sentence: things that enter the water near the camera
// make rings that spread out, run into the shore and fade, and the water surface
// tilts and lifts where they pass.
//
// It is a COSMETIC SURFACE LAYER and nothing more. It does not move water, does
// not know about basins, volumes, particles or the per-basin datum, and cannot
// change any of them -- docs/water-architecture.md §1 names the things that DO
// (baked basins, a scalar hydrology ledger, a GPU PBF solver), and this belongs
// to none of them. If a future change makes this field an input to anything but
// a material, that is the moment to stop and re-read that document.
//
// --- THE SHAPE, AND WHY IT IS THE SAME SHAPE AS THE BATHYMETRY FIELD --------
//
// A square, axis-aligned, world-space window that follows the camera. That is
// deliberately VoxelBathyField.h's design, reused rather than re-invented,
// because this project has exactly one solved instance of "get a
// camera-following 2D world-space field into a material" and a second
// convention would be a second set of edge cases:
//
//   kSize x kSize texels, ONE TEXEL PER 10 cm VOXEL, so a 51.2 m window
//   /Game/Voxel/RT_VoxelRippleField, RGBA16f
//     R = dH/dx, dimensionless      G = dH/dy, dimensionless
//     B = ripple height, METRES     A = not written, must not be read
//
//   RippleFieldOrigin   (MPC vector) world UU of the window's minimum corner
//   RippleFieldInvSize  (MPC scalar) 1 / (window width in UU)
//   RippleFieldGain     (MPC scalar) 0 until a frame has actually been
//                       simulated, so the water renders exactly as it does
//                       today whenever this subsystem is off, missing its
//                       assets, or has not run yet
//
// Three differences from the bathymetry field, each forced by the fact that this
// field is SIMULATED rather than read out of a bake:
//
//   * IT LIVES IN RENDER TARGETS, not a UTexture2D whose pixels the CPU writes.
//     The contents are produced by a shader from their own previous contents;
//     there is nothing on the CPU to upload.
//   * IT SCROLLS EVERY FRAME instead of being refilled every 120 m. A refill is
//     impossible -- the data exists nowhere else -- so the window's motion is
//     folded into the simulation step as a read-offset, which is free. See
//     "THE MOVING WINDOW" below.
//   * IT IS SMALL. 51.2 m across against the bathymetry window's 960 m. A ripple
//     is a near-field cue; at 30 m a 9 cm ring is under a pixel.
//
// --- WHY 10 cm PER TEXEL, WHICH SETS EVERYTHING ELSE ------------------------
//
// One texel per voxel column. Three reasons, in order of weight:
//
//   1. IT IS THE FINEST GRID THE REST OF THE WATER ALREADY USES. The wave field
//      in M_WaterVoxel quantises itself to the same 10 cm lattice by default
//      (WaveQuantPerVoxel = 1, create_water_voxel_material.py:2455-2472), so the
//      ripple cannot carry detail the surface it adds to would throw away.
//   2. IT BOUNDS THE SHORTEST WAVELENGTH SENSIBLY. Four samples per wavelength
//      is the floor below which a travelling wave reads as noise rather than as
//      a wave, which puts the shortest useful ripple at 40 cm -- about right for
//      a footstep, and small compared to the 8.6 cm-amplitude, 5 m-wavelength
//      wind wave it is added to.
//   3. 512 TEXELS AT 10 cm IS 51.2 m, i.e. +/-25.6 m, WHICH IS A NUMBER THIS
//      PROJECT ALREADY CHOSE. It is exactly the half-extent of the retired
//      near-field voxel water disc (VoxelWaterSheetActor.h:203,
//      VoxelWaterSubsystem.h:515) -- the radius at which this game previously
//      decided detailed water stops being worth drawing. Matching it is not a
//      coincidence to be tidied away; it means the ripple reach and the
//      historical detail reach agree.
//
// --- STABILITY, AND THE LONG-FRAME PROBLEM ----------------------------------
//
// The step is the explicit two-level wave equation (see
// Tools/create_ripple_field_materials.py for the shader), which is stable while
// the Courant number C = c*dt/dx stays below 1/sqrt(2) = 0.7071 in 2D and blows
// up in a handful of frames above it. The frame rate here is not fixed and the
// GPU spike tail is this project's known p95 problem
// (docs/water-architecture.md §3, "Key risks"), so dt CANNOT be the frame time:
// one 40 ms frame would multiply C by 2.4 and detonate the field.
//
// So: A FIXED TIMESTEP WITH AN ACCUMULATOR. kFixedDt is 1/60 s whatever the
// frame rate; leftover time carries to the next frame; and a frame that owes
// more than kMaxSubsteps steps DROPS the excess rather than trying to catch up
// (catching up after a hitch is how a fixed-step loop turns one long frame into
// several). Ripples run momentarily slow after a stall, which is invisible, and
// the simulation cannot spiral.
//
// At c = 1.6 m/s and dx = 0.1 m, C = 1.6/6 = 0.2667 -- a 2.65x margin under the
// limit. kCourantCeiling clamps the console variable so nobody can drive past
// 0.6 by typing a number, and says so in the log when it bites.
//
// --- THE MOVING WINDOW ------------------------------------------------------
//
// The window origin is SNAPPED TO WHOLE TEXELS and recomputed every frame that
// steps. The step material reads its source at Uv + ShiftUv, where ShiftUv is
// the origin's motion in UV -- always an exact multiple of 1/kSize, so a
// nearest-neighbour tap reproduces the previous texel bit for bit and the field
// SCROLLS rather than blurring. A sub-texel shift would run the whole field
// through a bilinear filter 60 times a second; the ripples would visibly
// dissolve whenever the player walked, and it would look like a damping bug.
//
// There is no hysteresis and no recentre threshold, and that is the opposite of
// VoxelBathyField's kRecentreFraction -- correctly, because the costs are
// opposite. Moving the bathymetry window costs eight block decodes and a 2 MB
// upload, so it is done rarely; moving this one costs an add on a UV that was
// already being computed, so it is done always, and the field stays centred on
// the camera to within one texel instead of wandering by 120 m.
//
// Texels scrolling in from outside the previous window have no history. The
// shader's `inWindow` guard writes flat water there rather than letting clamp
// addressing streak the border texel across the new band. This also means a
// TELEPORT NEEDS NO SPECIAL CASE: if the window jumps further than its own
// width, every texel is out of range and the field self-clears in one step.
//
// --- WHERE RIPPLES ARE ALLOWED ----------------------------------------------
//
// The bake already knows. bake_ver 27's bathy_shore plane is a SIGNED DISTANCE
// to the nearest shoreline, and the step multiplies the field by
// saturate(shore_m / kShoreMaskM) every step: zero on land, zero at the
// waterline, one 25 cm inside the water. That is both "no ripples on dry ground"
// and "a shore for a ripple to die against", from one number.
//
// Where the bake has no answer there is no mask -- the same lerp-back-to-1 on
// validity the wave field's shore damping uses
// (create_water_voxel_material.py:2614-2616), and it costs nothing here because
// the ripple field is only ever READ by the water material: a ripple over dry
// land is invisible whatever the simulation believes.
//
// --- COST -------------------------------------------------------------------
//
// 6 MB of render targets (two 512x512 RG32f state targets at 2 MB, one 512x512
// RGBA16f field at 2 MB) and, per frame, one to two 262,144-pixel step passes
// plus one derive pass. These are ESTIMATES, not measurements -- see
// docs/water-interactive-ripples.md for the arithmetic behind ~0.05-0.08 ms of
// GPU time and for the instrument that would settle it (ProfileGPU). The one
// cost that is NOT bounded by the field's size is the extra texture tap the
// water material takes on every water pixel, and water can be most of the
// screen.
// ============================================================================

class UMaterialInstanceDynamic;
class UMaterialParameterCollection;
class UTextureRenderTarget2D;

UCLASS()
class VOXELEARTH_API UVoxelRippleFieldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// --- the contract with Tools/ripple_field_graph.py -----------------------
	//
	// Every constant in this block is mirrored there, and the mirror is checked
	// only by a comment. What a drift can actually break is bounded, and it is
	// worth knowing which half is dangerous:
	//
	//   kSize      DANGEROUS. The subsystem refuses to run against a render
	//              target of a different size (see the guard in Initialize), so
	//              a drift disables the feature with one log line rather than
	//              corrupting anything.
	//   kTexelUU   SAFE-ish. It only reaches the shader through TexelM and
	//              Courant2, both of which this file computes and writes every
	//              step, so C++ is the authority and the Python default is a
	//              preview value.
	//   kStateBias DANGEROUS AND SILENT. It is the one number both sides encode
	//              with independently: the shader adds it, and ClearState writes
	//              it as a clear colour. A mismatch means every cleared texel is
	//              a step-function displacement of (their bias - our bias)
	//              metres -- i.e. the whole lake half a metre out of place,
	//              released at rest, on every clear. Nothing reports it.
	static constexpr int32 kSize = 512;
	static constexpr double kTexelUU = 10.0;                 // 10 cm, VoxelCoords::VoxelSizeUU
	static constexpr double kWindowUU = kSize * kTexelUU;    // 5120 UU = 51.2 m
	static constexpr int32 kSplatSlots = 8;                  // Splat0..Splat7 on the step material
	static constexpr float kStateBias = 0.5f;

	// --- the simulation's fixed parameters -----------------------------------

	// 1/60 s, and it is not the frame time. See "STABILITY" above.
	static constexpr double kFixedDt = 1.0 / 60.0;

	// The hard 2D stability limit for the 5-point explicit scheme, quoted so the
	// clamp below can be read against it rather than taken on trust.
	static constexpr double kCourantLimit = 0.7071067811865476;

	// Our own ceiling, well under the limit. A console variable may not push the
	// wave speed past this; the clamp logs when it engages, because "I set the
	// speed to 20 and the lake exploded" is a question that should answer itself.
	static constexpr double kCourantCeiling = 0.6;

	// Metres inside the water at which the shore mask reaches full strength.
	// 25 cm = 2.5 texels, so the mask is a soft absorber a couple of cells wide
	// rather than a hard wall; a hard wall on a 10 cm grid rings.
	static constexpr double kShoreMaskM = 0.25;

	// Smallest disturbance radius the field can represent honestly. Below about
	// three texels a raised-cosine bump is mostly the grid, and what radiates
	// from it is checkerboard noise rather than a ring. Requests smaller than
	// this are widened (and said so, once).
	static constexpr double kMinDisturbanceRadiusM = 0.3;

	// Pending disturbances waiting for a slot. 64 is eight full steps' worth --
	// at 60 steps a second nothing that is not a bug can outrun it, and a bug
	// that does is counted rather than allowed to grow without bound.
	static constexpr int32 kMaxPending = 64;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	// ------------------------------------------------------------------------
	// THE PUBLIC WAY TO MAKE A RIPPLE
	// ------------------------------------------------------------------------
	//
	// WorldPos is in UE units; only XY is used, because the field is a 2D sheet
	// and the caller's Z is exactly the thing the water surface already knows
	// better than the caller does. RadiusM is the ring's initial radius in
	// metres, StrengthM its initial height in metres.
	//
	// SAFE TO CALL FROM ANYWHERE ON THE GAME THREAD AT ANY TIME, including
	// before the first frame, with the subsystem disabled, with no render
	// targets and with no water anywhere near: the disturbance is either queued
	// or DROPPED AND COUNTED, and never anything else. A caller must never have
	// to ask whether ripples are on.
	//
	// "AND COUNTED" IS LOAD-BEARING AND WAS UNTRUE UNTIL 2026-08-12. The two
	// earliest exits -- the subsystem never armed, and a strength of zero -- used
	// to return without touching a counter, so a hook site firing correctly into
	// a disabled subsystem was indistinguishable from a hook site that was never
	// called. That is the exact failure docs/water-architecture.md §4 forbids
	// ("every stage writes a ran-flag distinguishable from 'found nothing'"), and
	// it is the one that matters most here, because the four ways a disturbance
	// can come to nothing are four different diagnoses:
	//
	//   DroppedUnarmed  the feature is off or its assets are missing -- run
	//                   Tools/create_ripple_field_materials.py and read the
	//                   startup log. Counted even though nothing else runs, so
	//                   voxel.Water.Ripple.Stat can still tell "the hooks fire and
	//                   the field is off" from "nothing ever calls in".
	//   DroppedInert    the CALLER asked for nothing: strength 0, or a strength or
	//                   radius that is not finite. A tuning mistake upstream (a
	//                   strength CVar at 0, an impact fraction that came out 0),
	//                   never a fault of this subsystem.
	//   DroppedOutside  the position is not inside the 51.2 m window at all.
	//   DroppedFull     the queue of kMaxPending overflowed, which is a bug.
	//
	// TYPICAL NUMBERS. A player dropping into a lake is about 0.5 m radius and
	// 6-12 cm of strength depending on how fast they were falling; a thrown
	// voxel volume is its own half-width and rather less strength than people
	// expect -- a 9 cm ring is already a big splash on water whose ambient wave
	// is 8.6 cm.
	UFUNCTION(BlueprintCallable, Category = "Voxel|Water")
	void AddDisturbance(const FVector& WorldPos, float RadiusM, float StrengthM);

	// The same thing for a caller that has a UWorld and does not want to know
	// this subsystem exists. Every hook site in docs/water-interactive-ripples.md
	// is one call to this and nothing else -- which is the point: a hook into a
	// file this feature does not own should be one line that cannot fail.
	static void AddDisturbanceAt(const UWorld* World, const FVector& WorldPos,
	                             float RadiusM, float StrengthM);

	// Run N simulation steps RIGHT NOW, outside the frame's own stepping. Exists
	// for one reason: a screenshot at a pinned pose has to photograph the same
	// water every time it is taken, and a ripple that has been settling for
	// "however many frames the capture harness happened to take" is not
	// reproducible. Inject, run a fixed number of steps, freeze, capture.
	void RunSteps(int32 NumSteps);

	// Both state targets back to flat water. Also called on arm and disarm -- a
	// field left over from a previous world would otherwise be the first thing
	// the next one shows.
	void ClearState();

	// --- diagnostics ---------------------------------------------------------
	//
	// TotalSteps stuck at 0 with water on screen is the whole diagnosis, in the
	// same way BathyField's PublishedWindows is. The four Dropped counters
	// separate the four ways a disturbance can come to nothing; see the block
	// above AddDisturbance for which one means what. Injected + the four of them
	// is the number of calls AddDisturbance has ever received, exactly -- if that
	// identity ever fails, a path out of that function has stopped counting.
	uint64 TotalSteps() const { return TotalSteps_; }
	uint64 DisturbancesInjected() const { return Injected_; }
	uint64 DisturbancesDroppedOutside() const { return DroppedOutside_; }
	uint64 DisturbancesDroppedFull() const { return DroppedFull_; }
	uint64 DisturbancesDroppedUnarmed() const { return DroppedUnarmed_; }
	uint64 DisturbancesDroppedInert() const { return DroppedInert_; }
	bool IsArmed() const { return bArmed_; }
	bool HasPublished() const { return bPublished_; }
	FVector2D WindowOriginUU() const;

private:
	// A disturbance waiting for a slot in a step. World XY only; see
	// AddDisturbance on why Z is discarded at the door rather than carried.
	struct FPendingDisturbance
	{
		double WorldX = 0.0;
		double WorldY = 0.0;
		double RadiusM = 0.0;
		double StrengthM = 0.0;
	};

	// One actor the auto-watcher is tracking across the water line. Held by weak
	// pointer because debris and thrown explosives are destroyed on their own
	// schedule and this map must never be the reason one stays alive.
	struct FWatchedActor
	{
		// False until this actor has been seen ONCE. Without it, an actor that is
		// spawned already underwater -- debris promoted from a lake bed, a thrown
		// charge that starts submerged -- reads as a fresh entry on the frame it
		// appears and splashes for no reason.
		bool bSeen = false;
		bool bWasSubmerged = false;
		FVector LastPos = FVector::ZeroVector;
	};

	// True once every asset was found AND passed the size/format guard. False
	// disables everything, permanently, for this world.
	bool bArmed_ = false;
	// True once RippleFieldGain has been published above zero, i.e. once there is
	// a simulated field for a material to read. Keeps IsTickable alive for one
	// more tick after a runtime disable so the gain can be published back to 0.
	bool bPublished_ = false;
	// Said once, not per frame: the first step ran, a radius was widened, the
	// wave speed was clamped.
	bool bLoggedFirstStep_ = false;
	bool bLoggedRadiusClamp_ = false;
	bool bLoggedCourantClamp_ = false;
	// Whether MPC_VoxelSky actually carries the three ripple parameters. Checked
	// ONCE, in Initialize, and never again: UKismetMaterialLibrary's setters log a
	// warning and do nothing for a name that is not on the collection, and doing
	// that three times a frame would bury every other line in the log.
	bool bMpcHasParams_ = false;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> StateA_;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> StateB_;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> Field_;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> StepMid_;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DeriveMid_;

	// Cached because PublishWindow runs EVERY frame, unlike the bathymetry
	// field's, which runs every 120 m of travel and can afford to re-resolve the
	// path each time (VoxelBathyField.cpp:337).
	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> SkyCollection_;

	// Which state target holds the newest heights. The step reads the front and
	// writes the back, then they swap.
	bool bFrontIsA_ = true;

	// Window origin, in TEXELS, of the currently simulated window's minimum
	// corner. Texel space rather than UU for VoxelBathyField.h:190-194's reason:
	// the snap and the scroll are integer arithmetic and only the publish step is
	// metric, so there is exactly one conversion and it cannot drift.
	int64 OriginPx_ = 0;
	int64 OriginPy_ = 0;
	bool bHaveOrigin_ = false;

	// Unspent frame time, in seconds. See "STABILITY" above.
	double Accumulator_ = 0.0;

	TArray<FPendingDisturbance> Pending_;
	TMap<TWeakObjectPtr<AActor>, FWatchedActor> Watched_;
	bool bPawnWasSubmerged_ = false;
	FVector LastPawnPos_ = FVector::ZeroVector;
	bool bHaveLastPawnPos_ = false;

	uint64 TotalSteps_ = 0;
	uint64 Injected_ = 0;
	uint64 DroppedOutside_ = 0;
	uint64 DroppedFull_ = 0;
	uint64 DroppedUnarmed_ = 0;
	uint64 DroppedInert_ = 0;

	// Camera XY in UU, or false when there is no view to follow. The SAME anchor
	// VoxelBathyField::GetCameraXY uses, and for the same reason: two
	// camera-following windows centred on different things would disagree about
	// where the world is, and the step samples the bathymetry window.
	bool GetCameraXY(double& OutX, double& OutY) const;

	// One simulation step: scroll by ShiftUv (zero on every substep after the
	// first of a frame), inject up to kSplatSlots queued disturbances, draw.
	void StepOnce(double ShiftUvX, double ShiftUvY);
	// State -> the field the water material samples. Once per frame, after the
	// last step, never per step.
	void RunDerive();
	// Origin, inverse size and gain into MPC_VoxelSky. Same tick as the draws.
	void PublishWindow(float Gain);
	// Fill Splat0..Splat7 from the queue for one step; empties them first, so a
	// step with nothing queued cannot re-fire the previous step's disturbances.
	void FillSplatSlots();
	// The zero-edit half of the hook-up: watch the pawn and the two actor classes
	// that fall into water, and inject on the crossing. See the class comment in
	// the .cpp for what this can and cannot see.
	void AutoWatch(float DeltaTime);

	UTextureRenderTarget2D* Front() const;
	UTextureRenderTarget2D* Back() const;
};
