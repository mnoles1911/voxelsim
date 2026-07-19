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
			"Engine"
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
