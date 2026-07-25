// VoxelEarthShaders — the GPU worldgen/mesher kernels, compiled by Unreal.
//
// WHY THIS IS A SEPARATE MODULE FROM VoxelEarth, AND WHY IT LOADS EARLIER.
//
// Global shaders are compiled once, during engine startup, before any module
// with LoadingPhase "Default" has loaded. A module can only tell Unreal where
// to find its shader source by calling AddShaderSourceDirectoryMapping, and a
// mapping registered after the global shader map is built is simply too late —
// the shaders are already missing from it.
//
// So the shader directory mapping has to happen in a module whose LoadingPhase
// is PostConfigInit. VoxelEarth is the primary game module and loads at
// "Default" (it depends on Engine), so it cannot be that module. This one can:
// it depends only on rendering plumbing, never on Engine gameplay types.
//
// Same pattern the engine's own shader-owning plugins use — see
// Engine/Plugins/Experimental/DynamicWind, whose .uplugin marks its runtime
// module PostConfigInit for exactly this reason.

using UnrealBuildTool;

public class VoxelEarthShaders : ModuleRules
{
	public VoxelEarthShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Matches VoxelEarth. voxel-core is C++20 and its headers are included
		// by the verification path, so both modules must agree.
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"RenderCore",   // AddShaderSourceDirectoryMapping, FGlobalShader, FRDGBuilder
			"RHI"           // FRHIGPUBufferReadback, buffer descriptors
		});

		// Projects supplies IPluginManager/plugin paths. Not needed today (the
		// shader directories are resolved from FPaths::ProjectDir), but the
		// mapping code lives here, so keep the dependency list honest by NOT
		// adding it until something actually calls into it.
	}
}
