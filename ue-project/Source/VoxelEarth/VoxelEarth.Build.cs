using System.IO;
using UnrealBuildTool;

public class VoxelEarth : ModuleRules
{
	public VoxelEarth(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// UE 5.7 defaults to C++20 (BuildSettingsVersion.Latest), but set it
		// explicitly since this module links a C++20 static library.
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// FVoxelChunkSceneProxy (custom FPrimitiveSceneProxy, doctrine
			// SS3.3 Band 1 -- NOT ProceduralMeshComponent): FLocalVertexFactory,
			// FStaticMeshVertexBuffers, FDynamicMeshIndexBuffer32.
			"RenderCore",
			"RHI",
			// Stage 2: legacy raw-key input bindings (EKeys::W, EKeys::LeftMouseButton, ...)
			// on AVoxelEarthFlyPawn / AVoxelEarthPlayerController -- no Enhanced Input assets.
			"InputCore",
			// m1-plan.md "Explosives v1" row: UVoxelBlastCameraShake configures
			// a UPerlinNoiseCameraShakePattern (VoxelExplosive.h/.cpp) for the
			// brief detonation camera kick. Enabled-by-default engine plugin.
			"EngineCameras"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// M2 Band 3 first slice (docs/m2-plan.md, AVoxelClipmapActor):
			// PRAGMATIC EXCEPTION to the "no PMC" doctrine (that rule targets
			// the voxel rendering path, not a conventional heightmap clipmap
			// -- see VoxelClipmapActor.h's class comment).
			"ProceduralMeshComponent"
		});

		// voxel-core: engine-agnostic, UE-header-free C++20 static library.
		string VoxelCoreRoot = Path.Combine(ModuleDirectory, "..", "..", "..", "voxel-core");
		string VoxelCoreInclude = Path.Combine(VoxelCoreRoot, "include");
		PublicIncludePaths.Add(VoxelCoreInclude);

		// Single-config generators (Ninja, Makefiles) place voxelcore.lib
		// directly in this directory; multi-config generators (Visual Studio)
		// place it under a per-configuration subdirectory instead. Search
		// both, preferring an optimized build over Debug, and preferring the
		// subdirectory that matches the UE target configuration when there's
		// a choice, so a stray Debug lib doesn't win over a Release build in
		// an optimized UE build (and vice versa).
		string VoxelCoreBuildDir = Path.Combine(ModuleDirectory, "..", "..", "..", "build", "voxel-core-msvc");
		bool bDebugTarget = Target.Configuration == UnrealTargetConfiguration.Debug
			|| Target.Configuration == UnrealTargetConfiguration.DebugGame;
		string[] ConfigSearchOrder = bDebugTarget
			? new string[] { "Debug", "RelWithDebInfo", "Release", "" }
			: new string[] { "Release", "RelWithDebInfo", "Debug", "" };

		string VoxelCoreLib = null;
		foreach (string Config in ConfigSearchOrder)
		{
			string Candidate = string.IsNullOrEmpty(Config)
				? Path.Combine(VoxelCoreBuildDir, "voxelcore.lib")
				: Path.Combine(VoxelCoreBuildDir, Config, "voxelcore.lib");
			if (File.Exists(Candidate))
			{
				VoxelCoreLib = Candidate;
				break;
			}
		}

		if (VoxelCoreLib == null)
		{
			throw new BuildException(
				"VoxelEarth: could not find voxelcore.lib. Searched:\n" +
				"  " + Path.Combine(VoxelCoreBuildDir, "voxelcore.lib") + "\n" +
				"  " + Path.Combine(VoxelCoreBuildDir, "Release", "voxelcore.lib") + "\n" +
				"  " + Path.Combine(VoxelCoreBuildDir, "RelWithDebInfo", "voxelcore.lib") + "\n" +
				"  " + Path.Combine(VoxelCoreBuildDir, "Debug", "voxelcore.lib") + "\n" +
				"Build voxel-core first, e.g.:\n" +
				"  cmake -S voxel-core -B build/voxel-core-msvc\n" +
				"  cmake --build build/voxel-core-msvc --config Release"
			);
		}

		PublicAdditionalLibraries.Add(VoxelCoreLib);
	}
}
