#pragma once
// "Voxelmark Map.html": the parchment sheet, the compass rose, the you-are-here
// pin, the player's named marks and the saved-places drawer.
//
// 2026-09-08: THE MOCK'S PAN, ZOOM AND MARKS ARE NOW PORTED. Owner directive,
// verbatim: *"on the map, create live overlay/marker for where the player is on
// the map. support zooming in and out on the map. in addition, right clicking
// and dragging should move the map around. and there should be functionality
// for player to right click on map, have a drop down to make a mark, name it,
// and save the place for future reference on the map. default zoom on the map
// should be centered on player location and a 10km x 10km square - not the
// entire game world map."*
//
// Everything below the next rule is what changed and why; the material facts
// about the raster that were already recorded here are kept, corrected where
// this pass found them wrong.
//
// --- THE TERRAIN LAYER IS THE OFFLINE HILLSHADE ------------------------------
//
//   * The mock leaves #topo deliberately empty with the note "the sheet will be
//     drawn from the procedural heightmap". There is no such thing at runtime:
//     UVoxelWorldSubsystem exposes a per-column height query and no raster, and
//     FVoxelRasterAtlasCpu is a GPU generation cache with no CPU-readable
//     image. Rasterising a 4200 px sheet by calling GetSurfaceHeightUU eleven
//     million times is not a UI change.
//   * What DOES exist is world-maps/<world>/01-heightmap-hillshade.png, an
//     offline render of exactly this terrain, produced by
//     terrain-service/tools/worldmaps/heightmap.py.
//   * The file is NOT COPIED INTO Content/. It is generated output that already
//     lives in the repository once, it is per-world, and a packaged build
//     wanting maps needs a per-world pipeline rather than one checked-in PNG.
//     So this reads it from the repo path and treats absence as ordinary.
//
// THE SEED IS NOT THE WORLD, AND THE OLD PATH RULE WAS WRONG. RasterPathForSeed
// used to return world-maps/seed<N>/01-heightmap-hillshade.png and nothing
// else. There are TWO directories under world-maps/ for seed 20260719 --
// `seed20260719` (provider terrain-diffusion-unlabeled-71e2b362e3241e71, origin
// tile (-3,-6), generated 2026-08-02) and `80b9ca451a23eae4-seed20260719`
// (provider ...-80b9ca451a23eae4, origin tile (-8,-8), generated 2026-08-03) --
// and DefaultGame.ini has pointed the engine at the SECOND since 2026-08-01.
// Its own README says so in a heading: "THIS IS A DIFFERENT PLANET FROM
// 71e2b362e3241e71. Same seed, same code, different world." So the screen has
// been drawing another planet's relief, with a different tile origin, under a
// comment explaining that drawing another world's hillshade is exactly the
// thing not to do. Nobody could see it, because nothing was drawn ON the sheet
// to disagree with it -- which is precisely what a live player marker now is.
//
// ResolveRaster below therefore picks the raster by PROVIDER, from the same
// coarse tile directory the world subsystem itself resolves (-VoxelTileDir=,
// else DefaultTileDir under [/Script/VoxelEarth.VoxelWorldSubsystem]), and
// there is no fall-back to the bare seed directory: a world whose provider has
// no map gets no map. See SVoxelMapScreen.cpp for the resolution and for how
// the extent is MEASURED from that same tile directory rather than assumed.
//
// MARKS SURVIVE THE SESSION. They are written through to
// Saved/VoxelWorlds/<seed>.vxmarks.json on every add, rename and remove -- see
// VoxelMapMarks.h for why that file rather than the checkpoint system. The
// subsystem still owns the live array and the Codex's Places list reads the
// same one, so the two screens cannot disagree.

#include "CoreMinimal.h"
#include "VoxelScreenData.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SEditableText;
class SWidget;

// The map screen hands the WHOLE new list up rather than a diff. There are a
// handful of marks, the subsystem has to write the whole file anyway, and a
// "one changed, here is which" protocol is three delegates and a bug surface
// for no gain.
DECLARE_DELEGATE_OneParam(FOnVoxelMapMarksChanged, const TArray<FVoxelMapMark>&);

// ---------------------------------------------------------------------------
// The sheet: the pannable, zoomable map surface.
//
// A WIDGET OF ITS OWN, AND A COMPOUND ONE. Everything the map draws that moves
// with the view -- the hillshade, the marks, their labels, the player -- is
// Slate GEOMETRY in OnPaint, because their positions change every frame of a
// pan and rebuilding a widget tree per frame is not a thing to do. Everything
// that does NOT move with the view -- the compass, the scale readout, the
// right-click menu and its name field -- is a real widget, because those take
// focus, take clicks and hold text. A compound widget is the one shape that
// gives both: OnPaint draws the moving half underneath and then calls the base
// to draw the still half on top.
class VOXELEARTHUI_API SVoxelMapSheet : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelMapSheet) {}
		SLATE_ARGUMENT(FVoxelMapScreenData, Data)
		// The player's position and yaw, re-read EVERY FRAME. The rest of
		// FVoxelMapScreenData is gathered once when the tab opens, because it
		// queries the sky subsystem, the terrain height and the filesystem.
		SLATE_ATTRIBUTE(FVoxelMapPose, LivePose)
		SLATE_EVENT(FOnVoxelMapMarksChanged, OnMarksChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// How many METRES the view spans across the frame's shorter side. The
	// screen prints it under the compass; it is also the number the zoom bounds
	// are expressed in.
	double GetVisibleMetresAcross() const;

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	                      FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                      bool bParentEnabled) const override;
	virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;
	// The +/- keys need somewhere to arrive. See OnMouseButtonDown for how the
	// sheet comes to hold focus and why it is not taken proactively.
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FCursorReply OnCursorQuery(const FGeometry& Geometry, const FPointerEvent& CursorEvent) const override;

private:
	// --- The one authoritative transform ------------------------------------
	// World UU <-> sheet-local pixels. LocalSize is passed rather than cached
	// so that every caller uses the geometry it was actually handed --
	// OnPaint's, a mouse event's, or Tick's -- and no path can read a frame
	// size from a different frame.
	FVector2D WorldToLocal(const FVector2D& WorldUU, const FVector2D& LocalSize) const;
	FVector2D LocalToWorld(const FVector2D& LocalPx, const FVector2D& LocalSize) const;

	// The default the owner asked for: centred on the player, 10 km across the
	// frame's SHORTER side, clamped to the raster's extent.
	void ResetViewToPlayer(const FVector2D& LocalSize);
	// Keeps the view inside the extent, and centres it when the view is larger
	// than the extent on an axis.
	void ClampView(const FVector2D& LocalSize);
	// Zoom bounds for a given frame, in UU per local pixel.
	double MinUUPerPixel(const FVector2D& LocalSize) const;
	double MaxUUPerPixel(const FVector2D& LocalSize) const;
	// One notch, about a fixed point in local space (the cursor, or the frame
	// centre for the keyboard).
	void ZoomAbout(int32 Notches, const FVector2D& AnchorLocal, const FVector2D& LocalSize);

	// --- Marks --------------------------------------------------------------
	// Index of the mark whose glyph is within MapMarkHitRadius of a local
	// point, nearest first, or INDEX_NONE.
	int32 MarkAtLocal(const FVector2D& LocalPx, const FVector2D& LocalSize) const;
	void CommitMarks();

	// --- The popup ----------------------------------------------------------
	enum class EPopup : uint8
	{
		None,
		// Right-clicked empty sheet: "Mark this place".
		Place,
		// Right-clicked an existing mark: "Rename" / "Remove".
		Mark,
		// The inline name field, for a new mark or a rename.
		Name,
	};
	void OpenPopup(EPopup Mode, const FVector2D& LocalPx, const FVector2D& LocalSize);
	void ClosePopup();
	void RebuildOverlay();
	TSharedRef<SWidget> BuildPopup();
	TSharedRef<SWidget> BuildCompass();
	TSharedRef<SWidget> BuildPopupRow(const FText& Label, bool bDanger, TFunction<void()> OnChosen);
	void BeginName(bool bRename);
	void ConfirmName();

	FVoxelMapScreenData Data;
	TAttribute<FVoxelMapPose> LivePose;
	FOnVoxelMapMarksChanged OnMarksChanged;

	// Cached at Tick from the attribute, read at OnPaint, which is const. Same
	// arrangement SVoxelHourglass uses and for the same reason.
	FVoxelMapPose Pose;

	// --- View state ---------------------------------------------------------
	FVector2D ViewCentreUU = FVector2D::ZeroVector;
	// THE ZOOM SCALAR. Unreal units per sheet-local pixel; larger is further
	// out. Everything else about the view is derived from this and the centre.
	double UUPerPixel = 1.0;
	// False until the first Tick that has a real frame size. The default view
	// cannot be computed before then -- "10 km across the shorter side" needs
	// to know which side is shorter and how many pixels it is.
	bool bViewInitialised = false;
	// The raster extent in UU, resolved once when the screen opens. Always
	// valid: with no raster it falls back to the documented 17x17 world so the
	// sheet still pans and zooms over blank parchment.
	FVector2D ExtentMinUU = FVector2D::ZeroVector;
	FVector2D ExtentMaxUU = FVector2D::ZeroVector;
	// The last frame size Tick saw, for the handful of places that have to act
	// without a geometry in hand (a popup rebuild, a commit).
	FVector2D LastLocalSize = FVector2D(1.0, 1.0);

	// --- Right-button drag --------------------------------------------------
	bool bRightDown = false;
	// Promoted from "a click that has not finished" to "a pan" the first time
	// the pointer moves MapDragThresholdPx from where it went down. Once true
	// it stays true until the button comes up, so a drag that returns to its
	// origin still does not open a menu.
	bool bPanning = false;
	FVector2D PressLocal = FVector2D::ZeroVector;
	FVector2D PressCentreUU = FVector2D::ZeroVector;

	// --- Popup state --------------------------------------------------------
	EPopup Popup = EPopup::None;
	FVector2D PopupLocal = FVector2D::ZeroVector;
	// The world point the popup is about. For Place this is where the player
	// right-clicked; for Mark and a rename it is the mark's own position, so
	// that a rename cannot move a mark.
	FVector2D PopupWorldUU = FVector2D::ZeroVector;
	int32 PopupMarkIndex = INDEX_NONE;
	bool bNamingRename = false;
	FText PendingName;
	TSharedPtr<SEditableText> NameField;
};

// ---------------------------------------------------------------------------
class VOXELEARTHUI_API SVoxelMapScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelMapScreen) {}
		SLATE_ARGUMENT(FVoxelMapScreenData, Data)
		SLATE_ATTRIBUTE(FVoxelMapPose, LivePose)
		SLATE_EVENT(FOnVoxelMapMarksChanged, OnMarksChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelMapScreen() override;

	static TArray<struct FVoxelScreenAction> Actions(bool bHasRaster);

	// Where the hillshade for the world this run is playing would be. Public
	// because the subsystem asks the asset library for it before constructing
	// the screen -- the decode is asynchronous, so the brush has to be
	// requested a frame or more before it is wanted.
	//
	// THE SEED ALONE DOES NOT NAME A WORLD; see the header note. This resolves
	// through the run's coarse tile directory and returns an empty string when
	// no raster can be attributed to it with certainty.
	static FString RasterPathForSeed(uint64 Seed);

	// The hillshade's world extent, in Unreal units, MEASURED from the coarse
	// tile directory the picture was rendered from. Returns false and leaves
	// the outputs at the documented 17x17 default when the directory cannot be
	// read.
	static bool ResolveExtentUU(FVector2D& OutMinUU, FVector2D& OutMaxUU);

private:
	TSharedRef<class SWidget> BuildReadout();
	TSharedRef<class SWidget> BuildPlacesDrawer();
	// THE DRAWER IS THE ONLY PART OF THE SCREEN A MARK CHANGES, and it is
	// refilled rather than the tab rebuilt. Rebuilding the tab would work --
	// the shell already rebuilds itself on a tab press -- but it would also
	// reconstruct the sheet, and the sheet's pan and zoom are Construct-time
	// state. A player who names a place should not be thrown back to the
	// default view for doing it.
	void RefreshDrawer();
	// The sheet's write-back, before it goes on to the subsystem: the screen
	// keeps its own copy so the drawer and the sheet agree.
	void HandleSheetMarks(const TArray<FVoxelMapMark>& NewMarks);

	FVoxelMapScreenData Data;
	TSharedPtr<SVoxelMapSheet> Sheet;
	TSharedPtr<class SBox> DrawerHost;
	FOnVoxelMapMarksChanged OnMarksChanged;
};
