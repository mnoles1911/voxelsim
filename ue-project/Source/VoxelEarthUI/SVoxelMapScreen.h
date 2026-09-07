#pragma once
// "Voxelmark Map.html": the parchment sheet, the compass rose, the you-are-here
// pin, the player's named marks and the saved-places drawer.
//
// WHAT IS REAL. Everything in the readout. UVoxelSkySubsystem already computes
// the player's latitude and longitude every frame (FVoxelSkyState carries them,
// derived through VoxelSky::GeoFromWorldUU), VoxelCoords converts a world
// position into voxel and chunk coordinates, and
// UVoxelWorldSubsystem::GetSurfaceHeightUU answers for the terrain under the
// player. So the position block is live, correct and to four decimal places.
//
// THE TERRAIN LAYER IS THE OFFLINE HILLSHADE, and it is drawn only when the
// world's seed is the one it was rendered for.
//
//   * The mock leaves #topo deliberately empty with the note "the sheet will be
//     drawn from the procedural heightmap". There is no such thing at runtime:
//     UVoxelWorldSubsystem exposes a per-column height query and no raster, and
//     FVoxelRasterAtlasCpu is a GPU generation cache with no CPU-readable
//     image. Rasterising a 4200 px sheet by calling GetSurfaceHeightUU eleven
//     million times is not a UI change.
//   * What DOES exist is world-maps/seed<N>/01-heightmap-hillshade.png, a
//     4096 px offline render of exactly this terrain. It is drawn here when
//     UVoxelWorldSubsystem::GetSeed() matches the directory it came from --
//     which by default it does, because DefaultSeed is 20260719 and the
//     directory is seed20260719.
//   * A SEED MISMATCH DRAWS NO RASTER, deliberately. A hillshade of a different
//     world is not a rough map, it is a wrong one, and a map that is confidently
//     wrong about where the hills are is worse than a map with no hills.
//   * The file is NOT COPIED INTO Content/. It is 5.7 MB of generated output
//     that already lives in the repository once, it is per-seed, and a packaged
//     build wanting maps needs a per-seed pipeline rather than one checked-in
//     PNG. So this reads it from the repo path and treats absence as ordinary.
//
// THE MOCK'S PAN, ZOOM AND ROTATE ARE NOT PORTED, and the action bar says so.
// The sheet in the mock is 4200x2800 and drags under a frame with Q/E rotating
// it about the centre. Slate has no render transform on a layout panel that
// would survive hit-testing the marks, and the whole interaction exists to move
// around a raster this game cannot generate. What is drawn instead is the
// hillshade letterboxed into the frame with the player pinned on it -- an
// overview, not a pannable map -- and MapNoRaster()/the action hints say which
// of the two the player is looking at.
//
// MARKS ARE SESSION-ONLY. The mock persists them to localStorage; this project's
// save format has no room reserved for them (VoxelSave::FSaveInfo carries a
// timestamp, a seed, a position and an edit count), so adding them is a save
// schema change rather than a UI one. They live in the subsystem for the
// session and the Codex's Places list reads the same array, so the two screens
// cannot disagree.

#include "CoreMinimal.h"
#include "VoxelScreenData.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class VOXELEARTHUI_API SVoxelMapScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelMapScreen) {}
		SLATE_ARGUMENT(FVoxelMapScreenData, Data)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelMapScreen() override;

	static TArray<struct FVoxelScreenAction> Actions(bool bHasRaster);

	// Where the hillshade for a given seed would be. Public because the
	// subsystem asks the asset library for it before constructing the screen --
	// the decode is asynchronous, so the brush has to be requested a frame or
	// more before it is wanted.
	static FString RasterPathForSeed(uint64 Seed);

private:
	TSharedRef<class SWidget> BuildSheet();
	TSharedRef<class SWidget> BuildReadout();
	TSharedRef<class SWidget> BuildCompass();
	TSharedRef<class SWidget> BuildPlacesDrawer();

	FVoxelMapScreenData Data;
};
