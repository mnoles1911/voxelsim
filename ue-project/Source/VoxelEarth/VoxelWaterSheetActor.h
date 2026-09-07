#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
// For FLakeSheetLod, which is a nested struct and so cannot be forward
// declared. The .cpp already included this; the ladder moved it up here.
#include "VoxelWaterSubsystem.h"

#include "VoxelWaterSheetActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;

// LAKE SHEETS AT RANGE -- docs/watershed-system-plan.md work item 5, §5.2's
// "clipmap bands" row.
//
// THE DEFECT THIS EXISTS FOR, measured rather than assumed. UVoxelWaterSubsystem's
// RefreshImplicitWater meshes implicit water only inside a 65-brick disc, 52 m
// across (kImplicitRadiusBricks). Basin 1 of tile (-12,-5) -- the exemplar lake
// -- is 2.0 x 2.4 km. So 99.9% of that lake is not merely coarse at range, it is
// NOT DRAWN. A vista over the basin shows a dry bowl; a shoreline capture cannot
// contain both the shallow band and the deep centre, because the disc is smaller
// than the shallow band is wide. Work item 4 shipped a lake you can stand in;
// this is the one you can see.
//
// WHAT IT DRAWS. One flat translucent rectangle set per baked basin, at that
// basin's own datum, from the SAME extent masks the near field's ImplicitFn
// consumes (UVoxelWaterSubsystem::BuildLakeSheetRects -> vxc::lakeSheetRects).
// No collision, no tick on the water CA, no replication, nothing persisted --
// §5.2's rule that water at range is a SURFACE, and §7's "content is free at
// runtime" unchanged: an untouched lake still costs 0 bricks and 0 bytes.
//
// WHY IT IS NOT PART OF AVoxelClipmapActor, which is the other flat-mesh-at-
// range actor and was the obvious home. The clipmap's geometry is a
// camera-centred lattice at a fixed vertex count, rebuilt whole when the camera
// crosses a cell; a sheet's geometry is per-BASIN, cached across camera motion,
// and rebuilt only when a lake enters or leaves range or the near-field hole
// moves. Sharing an actor would mean sharing neither the rebuild trigger nor the
// budget, i.e. sharing the file and nothing else.
//
// WHERE IT STOPS, AND WHY THAT EDGE IS A SUBTRACTION. Inside the implicit disc
// the near field already draws real water voxels at the same datum. Two coplanar
// translucent surfaces z-fight AND blend twice, so the sheet cuts the disc's
// exact footprint out of itself (vxc::subtractRect) instead of fading or
// offsetting. The cut is applied ONLY when that disc is actually meshing this
// basin's surface -- the disc is bounded in z as well as xy, so a camera 30 m
// above the water has no near-field water to hand over to and must not have a
// hole cut for it. GetImplicitWaterDiscUU reports both spans for exactly that
// test.
//
// THE MATERIAL IS M_WaterVoxel, THE SAME ONE THE VOXELS USE, and that is a
// correctness choice rather than a convenience: the owner-tuned constants
// (shallow/deep opacity, Beer-Lambert D, the two tints, the W6 Fresnel sky term)
// then apply to the vista and the near field by construction, and the two cannot
// diverge the way the clipmap's biome colours would have if it had authored its
// own. The vertex-colour convention is that file's, reproduced here:
//   R = fill fraction, 1.0 -- a sheet is a full surface, so the fill-drop WPO is
//       zero and the surface sits AT the datum.
//   G = AO, 1.0 -- no greedy-mesher occlusion applies to a free-standing sheet.
//   B = 1 -- this vertex IS on its cell's +Z boundary; it is what un-masks the
//       ripple normal and keeps the far water shimmering like the near water.
//   A = foam activity, 0 -- baked water is settled, exactly as every implicit
//       brick already passes.
// The depth cue needs nothing added: it is a scene-DEPTH read (thickness along
// the view ray), so a flat sheet over a real bowl gets pale at the shore and
// deep in the middle for free, from the geometry rather than from a second
// authored gradient.
UCLASS()
class VOXELEARTH_API AVoxelWaterSheetActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelWaterSheetActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	// Diagnostics for the capture writeup and the perf run: what is on screen,
	// and what it cost to put there. A sheet that is absent and a sheet that is
	// present but behind terrain look the same in a screenshot; these do not.
	int32 GetSheetCount() const { return Sheets.Num(); }
	int32 GetSheetRectCount() const { return TotalRects; }
	int32 GetUnresolvedBasinCount() const { return UnresolvedBasins; }
	// Ticks this actor did nothing on because no pawn had been possessed yet, so
	// there was no real position to gather around. Zero on an ordinary -game
	// run; non-zero and STILL COUNTING means the session has no pawn (front-end
	// menu, or a spawn that never happened) and there are no sheets for a reason
	// that is not about water. See Tick() and GetCameraLocationUU().
	int32 GetDeferredGatherTicks() const { return DeferredGatherTicks; }

	// B1 WAVE TESSELLATION, as a number a leg can read. The greedy rectangles a
	// sheet is made of span hundreds of metres, so a material's World Position
	// Offset moves FOUR vertices and the surface stays flat -- the wave is
	// authored, computed, and invisible. Tessellation inside the camera disc is
	// what gives WPO somewhere to push. That is a claim about the IMAGE, so it
	// gets a counter that can fail: zero here with the disc over a lake means
	// the waves are not being drawn no matter what the material graph says.
	int32 GetTessellatedVertCount() const { return TotalTessVerts; }

	// B3 TIDE x SHEETS. TideNudgedSheets counts component transform updates
	// (the cheap path -- the mesh does not change, it moves); TideRebuilds
	// counts basins whose datum drifted past kTideRebuildDriftUU and had to be
	// re-meshed at the new surface. Both are cumulative. A tide leg with
	// TideNudgedSheets stuck at 0 while the subsystem reports datum steps is
	// the sheet half of the feature silently not engaging.
	int32 GetTideNudgedSheets() const { return TideNudgedSheets; }
	int32 GetTideRebuilds() const { return TideRebuilds; }

private:
	// One basin's mesh and the state that says whether it is still current.
	struct FSheet
	{
		int32 TileX = 0, TileY = 0, BasinId = 0;
		// ---- ONE COMPONENT PER BASIN: Section -> Comp, 2026-08-28 -------------
		//
		// Every basin used to be a mesh SECTION on the actor's single
		// UProceduralMeshComponent. The engine recreates the WHOLE scene proxy on
		// any CreateMeshSection/ClearMeshSection (ProceduralMeshComponent.cpp:635
		// -- "New section requires recreating scene proxy"), and the proxy ctor
		// converts every vertex of every section and calls BeginInitResource five
		// times per section. At the measured 495 resident basins that is ~2,475
		// BeginInitResource calls to rebuild ONE basin: 3.79 ms of game thread
		// (Exclusive/GameThread/EndOfFrameUpdates) plus 5.81 ms of render thread
		// (InitRenderResource) on every rebuild tick -- ~9 ms to change one lake,
		// on 3.9% of moving frames. The CSV A/B is docs-grade: -VoxelLakeSheets=0
		// collapses both columns to zero (spikes 352 -> 0 of 9,000 frames).
		//
		// With a component per basin the recreate touches ONE basin's vertices.
		// The component is created lazily on first non-empty build (an empty PMC
		// has no proxy, so unbuilt basins cost nothing), registered to this actor
		// (GC-safe: RegisterComponent puts it in OwnedComponents), and destroyed
		// on re-gather. UpdateMeshSection is NOT the alternative here -- a hole
		// re-cut changes topology, which Update cannot express (see the comment at
		// the build site).
		UProceduralMeshComponent* Comp = nullptr;
		int32 StepPx = 1;
		int32 RectCount = 0;
		// The camera cell this basin's LOD bands were last centred on, and
		// whether they depend on the camera at all. A basin further out than the
		// outermost BOUNDED band decimates uniformly, so its geometry does not
		// change when the camera moves and it must leave the rebuild rotation --
		// with ~289 basins resident, rebuilding all of them because the camera
		// crossed a cell is the cost this flag exists to refuse.
		FIntPoint LodKey = FIntPoint(MIN_int32, MIN_int32);
		bool bUniformCoarse = true;
		// Built, as opposed to "has rectangles". A basin whose extent decimates
		// to zero cells at this range is BUILT and must leave the rebuild
		// rotation; keying the rotation on RectCount instead would re-mesh every
		// such basin on every tick forever.
		bool bBuilt = false;
		// Set once the basin's tile refused to decode. Retried never, counted
		// always -- see the .cpp.
		bool bUnresolved = false;
		// THE GATHER'S DATUM. Written once by GatherLakeSheetBasinsInTile and
		// NEVER mutated afterwards, because the re-gather's adoption test
		// compares it field-for-field against a freshly gathered basin (see
		// AdoptableSheets). A tide that moved this would fail that compare on
		// every basin and turn a 98%-adopting gather into a full rebuild drain
		// -- the 22.7 ms first-build hitch, back, once per tide step.
		double SurfaceZUU = 0.0;
		// THE DATUM THE MESH WAS ACTUALLY BUILT AT, which is SurfaceZUU plus
		// whatever the tide was at build time. The per-tick nudge is measured
		// from THIS, so a basin that has been re-meshed at high water does not
		// then get the whole tide offset applied a second time as a transform.
		double BuiltSurfaceZUU = 0.0;
		// Vertices this basin contributed to the tessellated (fine-cell) total.
		// Held per basin so the actor-wide count survives one basin rebuilding:
		// the same delta bookkeeping RectCount uses.
		int32 TessVerts = 0;
		// THE TESS RADIUS THIS MESH WAS ACTUALLY BUILT AT, and it exists because
		// the radius stopped being a launch constant. Every other staleness term
		// in the rebuild trigger is a function of the camera CELL, so a parked
		// camera correctly rebuilds nothing -- which would have made "Water Wave
		// Detail" do nothing at all until the player walked 30 m. Comparing this
		// against the live radius is what makes the toggle a toggle.
		//
		// -1 IS "NEVER BUILT", distinct from 0 ("built with the disc off"): a
		// basin whose mesh really was built at radius 0 must not be counted
		// stale forever.
		double TessRadiusM = -1.0;
		double MinXUU = 0.0, MinYUU = 0.0, MaxXUU = 0.0, MaxYUU = 0.0;
		// The near-field hole this mesh was cut with, so a moving camera only
		// rebuilds the one basin whose water it is standing in.
		bool bHadHole = false;
		FBox2D HoleUU = FBox2D(ForceInit);
	};

	// Rebuilds one basin's mesh section. Returns false if the basin would not
	// resolve (its tile or a block failed to decode) -- which is counted, not
	// swallowed, because it is indistinguishable from a dry basin on screen.
	bool RebuildSheet(FSheet& Sheet, const FVector& CamUU);

	// Lazily creates Sheet.Comp -- see FSheet::Comp for why each basin owns a
	// component. Mirrors the root Mesh's flags exactly (movable, no collision,
	// no shadow) so a basin cannot behave differently for having been split out.
	UProceduralMeshComponent* GetOrCreateSheetComp(FSheet& Sheet);

	// ---- RE-GATHER ADOPTION, 2026-08-28 ------------------------------------
	//
	// The re-gather used to wipe every sheet and rebuild all of them from
	// scratch -- measured on the LSHEET legs as a 485-tick drain in which EVERY
	// spike frame of the whole flight lived (the 352-spike cluster ends exactly
	// on the DRAINED tick, both arms). After the per-basin component split the
	// spikes moved into TickActors instead of vanishing: a ~5.7 ms/frame
	// plateau across the drain, a 17.7 ms all-at-once destroy hitch, and a
	// 22.7 ms first-build tick -- the worst single hitch got WORSE while the
	// spike metric read zero. And the wipe deletes every lake for ~5 s each
	// kilometre flown, which is a visual, not a counter.
	//
	// So the re-gather now ADOPTS: old sheets park here keyed by
	// (TileX, TileY, BasinId), and a re-gathered basin whose geometry is
	// unchanged takes its old component, mesh and LOD state back instead of
	// rebuilding. On the measured legs the tile set was identical across all
	// three gathers (476/482/495 basins), so ~98% adopt and the drain shrinks
	// from 482 builds to the handful that actually changed. Leftovers -- basins
	// no longer in range -- are destroyed a few per tick via PendingDestroy
	// rather than all in one tick, which is what the 17.7 ms hitch was.
	TMap<FIntVector, FSheet> AdoptableSheets;
	TArray<UProceduralMeshComponent*> PendingDestroy;

	// Serial for component names. NewObject with a DETERMINISTIC name collides
	// with the same-named component destroyed in the same gather (destroyed
	// objects keep their names until GC), forcing StaticAllocateObject's
	// displace-existing-object path on every build -- the leading suspect for
	// the TickActors plateau. A per-actor serial makes every name fresh.
	uint32 CompNameSerial = 0;

	// Decimation for a basin, in fine pixels per emitted cell. See the .cpp.
	int32 StepForBasin(double SpanUU) const;

	// The steps and radii for one basin -- fine underfoot, coarsening with range,
	// coarsest step still the basin-span one StepForBasin picks. No camera: this
	// is the ladder's SHAPE, which depends only on the basin.
	void BuildLadder(const FSheet& Sheet, UVoxelWaterSubsystem::FLakeSheetLod& OutLod) const;

	// BuildLadder plus the band centre, and whether this basin is far enough out
	// that every block lands in the last unbounded band (so it can leave the
	// rebuild rotation).
	void BuildLodForBasin(const FSheet& Sheet, const FVector& CamUU,
	                      UVoxelWaterSubsystem::FLakeSheetLod& OutLod, bool& bOutUniform) const;

	// Does this basin reach the outermost BOUNDED band at this camera? Evaluated
	// against the snapped cell, so it cannot flap.
	bool IsBandedAtCamera(const FSheet& Sheet, const FVector& CamUU) const;

	// Which hysteresis cell the camera is in. The bands only re-centre when this
	// changes, so a metre of walking rebuilds nothing.
	FIntPoint LodKeyForCamera(const FVector& CamUU) const;

	// The centre of that cell. The bands are centred here rather than on the
	// camera, which is what makes a basin's mesh a pure function of its LodKey.
	FVector2D SnappedCamXY(const FVector& CamUU) const;

	// The near-field cut-out, or false when the implicit disc is not meshing
	// water at this datum (too far above or below it) and no hole is owed.
	bool HoleForDatum(double SurfaceZUU, FBox2D& OutHoleUU) const;

	// ---- B1: THE WAVE TESSELLATION DISC ------------------------------------
	//
	// The square, in world UU, inside which the finest band emits fine cells
	// instead of greedy rectangles; false when tessellation is off for this
	// camera (radius 0, or no camera).
	//
	// A SQUARE, AND THE PLAN CALLS IT A DISC. Every range test in this actor is
	// already L-infinity -- IsBandedAtCamera takes FMath::Max(DX, DY) against
	// the band radius, and the LOD bands themselves are square annuli, because
	// the rectangles being decimated are axis-aligned and a circular boundary
	// through them cannot be expressed as rectangles. A round disc would have
	// to be approximated per cell, and a per-cell radius test that rejects a
	// cell leaves a HOLE in the sheet -- the one defect class this actor exists
	// to remove. So the disc is the SQUARE of the same radius: it over-covers
	// the circle and can never under-cover it.
	//
	// CENTRED ON THE SNAPPED CAMERA CELL, not the camera (SnappedCamXY), so the
	// tessellated geometry is a pure function of LodKey exactly like the bands
	// are. Centre it on the raw camera and every basin in the disc re-meshes on
	// every frame the camera moves -- 1-per-tick round robin or not, that is a
	// permanent rebuild treadmill, and the cell phase would flap under a
	// stationary-but-jittering camera.
	bool TessBoxForCamera(const FVector& CamUU, FBox2D& OutBoxUU) const;

	// THE TESSELLATION RADIUS, IN METRES, AS OF RIGHT NOW -- read from
	// voxel.Water.WaveTessRadiusM every time it is asked rather than latched
	// into a member at BeginPlay. It is a function and not a field because the
	// setting behind it ("Water Wave Detail") is a runtime toggle: latching
	// would make a mid-session flip do nothing, which is the silent kind of
	// nothing this file has been bitten by repeatedly.
	//
	// IT ALSO OWNS THE CLAMP. Past FineBandRadiusM the sheet is no longer
	// meshing at one fine pixel, so cells emitted out there would not be the
	// finest band's; clamping here rather than at the parse site means the
	// invariant holds for a value set from the console or the settings panel
	// too, not only for one set from the command line.
	double WaveTessRadiusMNow() const;

	// ---- B3/C3: WHERE THIS BASIN'S SURFACE IS *NOW* ------------------------
	//
	// Phase C DID replace the body, exactly as the Phase-B note here promised:
	// this is now a forwarder to UVoxelWaterSubsystem::GetBasinDatumNowZUU --
	// the per-basin query through the ONE datum seam (basin ledger wrapped by
	// vxc::TidalDatumSource) -- so an oracle-qualified rock pool rides the
	// tide while connected and holds at its sill when cut off, and an inland
	// lake answers its ledger datum untouched. Phase B's "standing at the sea
	// datum +-5 UU" placeholder is gone. Everything here that consumes a
	// surface height goes through this one function, which is what makes the
	// sheet structurally unable to disagree with the near field about where a
	// pool stands.
	double CurrentSurfaceZUUForBasin(const FSheet& Sheet, const UVoxelWaterSubsystem* Water) const;

	// Applies the tide to every resident sheet as a component transform, and
	// flags the ones whose drift has outgrown a transform for a real rebuild.
	// Does NOTHING -- not even a walk of Sheets -- on a tick where the datum
	// has not moved, which is every tick outside a tide quantum crossing.
	void ApplyTideNudge(const UVoxelWaterSubsystem* Water);

	bool GetCameraLocationUU(FVector& Out) const;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> WaterMaterial;

	// -VoxelWaterDepthAuthority=<float>: overrides M_WaterVoxel's
	// BathyDepthAuthority scalar for SHEETS ONLY, via one shared instance.
	//
	// WHAT IT IS FOR, and it is a defect and not a tuning knob. The material
	// blends two absorption depths (create_water_voxel_material.py, the
	// BathyDepthAuthority comment): the BAKED vertical depth at weight
	// a * validity, and the ENGINE's own BehindWaterSceneDepth -
	// WaterSurfaceSceneDepth -- an ALONG-VIEW-RAY distance -- at weight
	// 1 - a * validity. The engine half is UNBOUNDED and the material has no
	// input that can override it.
	//
	// The sheet's extent mask over-covers the basin by up to one mask cell
	// (1.875 m at the finest band, coarser at range) because drawing must
	// over-cover or it leaves a gap at the shoreline. On those over-covered
	// cells the bake answers "dry": validity 1, depth 0. So the baked half
	// contributes NOTHING and the engine half still carries 1 - a = 0.15 of a
	// ray that, at a grazing view over ground falling away past the shore,
	// runs for hundreds of metres. That saturates the absorption and paints a
	// near-black band tracing the mask boundary, leaving only the Fresnel sky
	// term -- measured at RGB (26, 30, 36) against snow at (205, 201, 197).
	//
	// At 1.0 the engine half is zero wherever the bake answered, so an
	// over-covered dry cell contributes zero optical depth and the sheet is
	// simply clear there. Where the bake did NOT answer (validity 0, outside
	// the 960 m BathyField window or a hole) the engine half returns at full
	// weight and behaviour is unchanged -- so this cannot make anything worse
	// where there is no data.
	//
	// THE COST, stated because the generator's own comment states it: at 1.0
	// the absorption comes entirely from the baked field, which does not know
	// about dynamic geometry, so a boat or a player standing in the shallows
	// no longer displaces the water colour exactly. That is why the material's
	// own default is 0.85 and not 1.0.
	//
	// SINCE THE B4 SHORE CLIP LANDED, THIS INSTANCE ALWAYS EXISTS: BeginPlay
	// creates the one shared sheet MID unconditionally to arm
	// WaterShoreClipEnabled=1 (the asset ships it 0 -- bathy validity is not a
	// wet test, and the sheet is the clip's sole legitimate consumer; see the
	// BeginPlay block). The diagnostic switches write onto that same instance,
	// so their control arms are VALUE-identical to the material defaults
	// rather than instance-free -- judge an arm by its logged parameter
	// values, never by whether a MID exists. (-VoxelWaterShoreClip=0 is the
	// clip's own control arm; captures from before 2026-09-04 predate the MID
	// and stay comparable because clip 0 equals the bare asset.)
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> SheetMaterialOverride;

	TArray<FSheet> Sheets;
	int32 TotalRects = 0;
	int32 UnresolvedBasins = 0;
	// Sum of FSheet::TessVerts over the resident set. See
	// GetTessellatedVertCount for why this is an instrument and not a stat.
	int32 TotalTessVerts = 0;
	int32 TideNudgedSheets = 0;
	int32 TideRebuilds = 0;

	// The tide offset (SeaSurfaceZNowUU - SeaLevelZUU) the sheets are currently
	// standing at. Held so ApplyTideNudge can return in three instructions on
	// the overwhelming majority of ticks: the subsystem's DATUM is quantised
	// (25 mm) and rate-limited (2 s), so it is unchanged on essentially every
	// frame, and walking ~500 basins to write an unchanged SetRelativeLocation
	// would dirty ~500 render transforms per tick for no visual change at all.
	double AppliedTideOffsetUU = 0.0;
	bool bTideOffsetApplied = false;

	// Basin scan radius in UU. Defaults to the clipmap's own outer half-extent so
	// the sheet covers exactly the ground the far terrain draws -- water stops
	// where the world it sits in stops, and neither number is written down twice.
	// -VoxelLakeSheetRangeM overrides it for the range A/B.
	double ScanRadiusUU = 0.0;

	// Camera position the basin set was last gathered at, and how far it may move
	// before that set is re-gathered. A gather loads fine tiles, so it is not a
	// per-tick operation; a kilometre of travel cannot bring a basin into a
	// multi-kilometre radius that was not already in it.
	FVector2D LastGatherXY = FVector2D::ZeroVector;
	// The centre the CURRENT scan is clipping against, held apart from
	// LastGatherXY so a scan that spans several ticks keeps one origin and does
	// not admit a different set of basins on its last tile than on its first.
	FVector2D GatherCenterXY = FVector2D::ZeroVector;
	bool bGathered = false;
	double RegatherDistanceUU = 100000.0; // 1 km

	// HOW LONG THE FIRST GATHER WAITED FOR A REAL POSITION, and the proof that
	// waiting is what happened rather than nothing happening. The gather used to
	// run on tick 1 at the camera manager's default (0,0,0) -- 61 km from the
	// spawn, into four unbaked origin tiles, 10,814 sea-level elevation reads,
	// fatal on the first one under -unattended. It now waits for a possessed
	// pawn, and a wait that logged nothing would be indistinguishable from a
	// feature that quietly stopped working. See Tick().
	int32 DeferredGatherTicks = 0;
	double FirstDeferSeconds = 0.0;
	bool bWarnedLongDefer = false;

	// Fine tiles this scan still has to read, one per tick. See Tick().
	TArray<FIntPoint> PendingTiles;

	// -VoxelLakeSheets=0 removes the whole feature on the same binary. This is
	// THE CONTROL for every sheet capture, and it is a switch rather than a cvar
	// for the reason -VoxelNoClipmap is: -ExecCmds lands after BeginPlay.
	bool bEnabled = true;

	// Fine pixels per emitted sheet cell is derived from a target cell COUNT per
	// basin side, not from a fixed metre size: a 30 m pond and a 2.4 km lake
	// otherwise get wildly different triangle budgets for the same screen area.
	// -VoxelLakeSheetCells overrides it.
	int32 TargetCellsPerSide = 128;

	// THE FINEST BAND'S HALF-EXTENT, in metres. Inside it the sheet meshes at ONE
	// FINE PIXEL (1.875 m), the finest the baked extent mask can express; each
	// band out doubles the radius and coarsens the step until the basin-span step
	// takes over.
	//
	// 96 m rather than something tidier because the number it has to beat is the
	// retired near disc's 25.6 m half-extent, and the hysteresis grid below can
	// take up to half a cell off the effective radius. 96 m with a 30 m grid
	// never drops the fine band below ~81 m, i.e. always more than three times
	// the coverage the voxel path had. -VoxelLakeSheetFineM moves it.
	double FineBandRadiusM = 96.0;

	// ---- B1: HOW FAR THE WAVES REACH, IN METRES -----------------------------
	//
	// The half-extent of the tessellation square (TessBoxForCamera). Inside it
	// the finest band emits ~1.875 m cells so the material's WPO has vertices
	// to move; outside it the greedy rectangles are unchanged and the surface
	// is flat, which is correct because the material fades WPO to ZERO before
	// this edge.
	//
	// 80 m, AND THE NUMBER IS NOT FREE. It is bounded above by two things that
	// are not ours: the material's WPO distance fade (B2, 55 -> 72 m -- past 72
	// m the displacement is identically zero, so tessellation past it buys
	// vertices that cannot move), and FineBandRadiusM (96 m -- past that the
	// sheet is not meshing at one fine pixel any more and the cells would not
	// be the finest band's). 80 m sits between them: comfortably outside the
	// fade so the fade never lands on the tessellation boundary, comfortably
	// inside the fine band so the finest-band invariant holds by construction.
	// WaveTessRadiusMNow() CLAMPS to FineBandRadiusM and BeginPlay says so when
	// the command line asked for more -- a silently clamped switch is
	// indistinguishable from a mis-spelled one.
	//
	// THERE IS NO MEMBER HERE ANY MORE (2026-09-05). The radius lives in
	// voxel.Water.WaveTessRadiusM, default 80, because the player-facing "Water
	// Wave Detail" row has to be able to move it mid-session; a member latched
	// at BeginPlay could only ever be moved by relaunching. -VoxelWaveTessM
	// still works and still means the same thing -- it now SETS that cvar at
	// BeginPlay instead of being read into a field -- and 0 is still the honest
	// OFF control (no tessellated vertex is emitted at all, and the sheet is
	// byte-identical to every capture taken before this feature).

	// Rate limit for the tessellation census line: one line per camera cell,
	// not one per rebuild. See the log site.
	FIntPoint LastTessLogKey = FIntPoint(MIN_int32, MIN_int32);

	// Camera hysteresis for the band centre, in fine pixels. The bands re-centre
	// when the camera crosses a cell of this size, not when it moves -- the same
	// recentring policy the fluid window and the clipmap use, and the reason a
	// basin is not remeshed because the player took a step. 16 fine pixels = 30 m.
	int32 LodSnapPx = 16;

	// THE LADDER'S OFF SWITCH, and the reason it is a rung COUNT rather than a
	// radius: -VoxelLakeSheetFineM 0 does NOT turn the ladder off. Radii round UP
	// to the band alignment (lcm of the rungs, 28 fine px on the exemplar basin),
	// so asking for a zero-metre fine band still yields a 52.5 m one. 1 here is
	// the honest control -- one band at the basin-span step, i.e. exactly the
	// decomposition the sheet shipped with before distance-aware LOD.
	int32 MaxBands = 4;

	// One basin rebuilt per tick at most, so a first frame in range never lands
	// as a hitch -- the same budget discipline RefreshImplicitWater's
	// kMaxImplicitMeshesPerTick and the clipmap's round-robin already use.
	int32 RoundRobinCursor = 0;

	bool bLoggedFirstBuild = false;
};
