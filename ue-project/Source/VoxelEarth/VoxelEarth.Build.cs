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

		PrivateDependencyModuleNames.AddRange(new string[] { });

		// voxel-core: engine-agnostic, UE-header-free C++20 static library.
		string VoxelCoreRoot = Path.Combine(ModuleDirectory, "..", "..", "..", "voxel-core");
		string VoxelCoreInclude = Path.Combine(VoxelCoreRoot, "include");
		PublicIncludePaths.Add(VoxelCoreInclude);

		string VoxelCoreLib = Path.Combine(ModuleDirectory, "..", "..", "..", "build", "voxel-core-msvc", "voxelcore.lib");
		PublicAdditionalLibraries.Add(VoxelCoreLib);
	}
}
