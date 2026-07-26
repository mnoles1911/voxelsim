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

using System.IO;
using UnrealBuildTool;

public class VoxelEarthShaders : ModuleRules
{
	public VoxelEarthShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Matches VoxelEarth. voxel-core is C++20 and its headers are included
		// by the verification path, so both modules must agree.
		CppStandard = CppStandardVersion.Cpp20;

		// voxel-core HEADERS ONLY. VoxelGpuWorldGen.cpp's
		// ModifyCompilationEnvironment passes vxc::kWorldGenVersion into
		// worldgen.ush, which #errors if the two disagree — the version half of
		// the mirror contract described at the top of that shader. This module
		// deliberately links nothing from voxel-core: VoxelEarth owns
		// voxelcore.lib and its staleness guard, and adding a second linker
		// dependency here would give that guard a second place to be wrong.
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "..", "..",
		                                    "voxel-core", "include"));

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"RenderCore",   // AddShaderSourceDirectoryMapping, FGlobalShader, FRDGBuilder
			"RHI",          // FRHIGPUBufferReadback, buffer descriptors
			// The vertex factory needs both: FMaterialShaderParameters (for
			// ShouldCompilePermutation) and FMeshBatch live in Engine,
			// FMeshMaterialShader and FMeshDrawSingleShaderBindings in Renderer.
			"Engine",
			"Renderer"
		});

		// Depending on Engine/Renderer from a PostConfigInit module is fine and
		// is what shipping engine plugins do -- OpenColorIO is PostConfigInit
		// and depends on both, and LidarPointCloudRuntime (the vertex factory
		// this one is modelled on) is PostConfigInit too. Module dependency is
		// DLL load order, which is a separate thing from engine initialisation.

		// Projects supplies IPluginManager/plugin paths. Not needed today (the
		// shader directories are resolved from FPaths::ProjectDir), but the
		// mapping code lives here, so keep the dependency list honest by NOT
		// adding it until something actually calls into it.
	}
}
