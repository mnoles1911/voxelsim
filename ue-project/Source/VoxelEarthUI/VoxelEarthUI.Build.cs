// VoxelEarthUI — the front end: main menu, loading screen, and the Slate
// widgets they are made of.
//
// WHY THIS IS A SEPARATE MODULE FROM VoxelEarth.
//
// 1. UNITY BLOB HYGIENE. VoxelEarth is one 16,793-line .cpp among 59, built as
//    unity blobs by default. UI code is unusually dense in file-local
//    constants -- kGold, kPanelPad, kBarWidth, kRowHeight -- which is exactly
//    the internal-linkage collision class tools/lint-unity-collisions.py was
//    written for after PR #210 shipped one. Keeping the front end in its own
//    module keeps those names out of the gameplay module's blobs entirely.
//
// 2. SLATE STAYS OUT OF THE SERVER. Type is ClientOnly and
//    VoxelEarthServer.Target.cs deliberately does not list this module, so a
//    dedicated server links no Slate at all.
//
// 3. THE DEPENDENCY RUNS ONE WAY: VoxelEarthUI -> VoxelEarth, never back.
//    There is no circularity because the front end owns itself --
//    UVoxelFrontEndSubsystem is a UTickableWorldSubsystem declared here and
//    auto-instantiated per UWorld, so AVoxelEarthGameMode never learns it
//    exists. The one thing the gameplay module does need to know -- whether
//    the front end runs this session -- lives in VoxelEarth's own
//    VoxelFrontEndPolicy.h for precisely that reason.
//
// VoxelEarth.Build.cs gains nothing from this module's existence, and that is
// a property worth keeping: the gameplay module has no UI dependency to
// accidentally grow one through.
//
// See docs/adr/0009-slate-front-end-and-committed-ui-art.md for the
// Slate-over-UMG decision this module embodies.

using UnrealBuildTool;

public class VoxelEarthUI : ModuleRules
{
	public VoxelEarthUI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Matches VoxelEarth, whose headers this module includes.
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",       // UWorld, UGameViewportClient, UTickableWorldSubsystem
			"Slate",        // SCompoundWidget, SButton, SOverlay, ...
			"SlateCore",    // FSlateBrush, FSlateDrawElement, FSlateFontInfo
			"InputCore",    // FKey, for the menu's keyboard handling
			"VoxelEarth"    // VoxelFrontEndPolicy, UVoxelWorldSubsystem, AVoxelEarthGameMode
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// FSlateApplication lives in Slate, but the input-mode and cursor
			// plumbing the front end drives reaches ApplicationCore types.
			"ApplicationCore",
			// FVoxelUIAssetLibrary decodes the menu background JPEGs itself,
			// on a worker, rather than going through the Slate resource
			// manager -- see that file for why. FImageUtils loads this module
			// dynamically; naming it here keeps the dependency list honest
			// about what the module actually uses.
			"ImageWrapper"
		});
	}
}
