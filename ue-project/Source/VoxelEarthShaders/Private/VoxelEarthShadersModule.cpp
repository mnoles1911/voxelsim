// VoxelEarthShaders module — registers where Unreal should look for the voxel
// GPU kernels, and nothing else.
//
// This module exists purely for its LOADING PHASE. See VoxelEarthShaders.Build.cs
// for the full explanation; the short version is that Unreal builds its global
// shader map during engine startup, so a shader directory mapping registered by
// a normal game module (LoadingPhase "Default") arrives after the shaders were
// already needed. This module is marked PostConfigInit in VoxelEarth.uproject,
// which runs early enough.

#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

class FVoxelEarthShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Two mappings, because the shader source is deliberately split across
		// two trees:
		//
		//   /VoxelEarth -> ue-project/Shaders
		//       Unreal-specific glue. Today just VoxelWorldGen.usf, the entry
		//       file every kernel is compiled from.
		//
		//   /VoxelCore  -> voxel-core/shaders
		//       The kernels themselves (worldgen.ush). This tree is engine-free
		//       and is shared, unmodified, with the standalone Vulkan bench that
		//       owns the bit-exactness gate. Mapping it in rather than copying
		//       it is what keeps the gate meaningful — see VoxelWorldGen.usf.
		//
		// FPaths::ProjectDir() is ue-project/, so voxel-core is one level up.
		// CollapseRelativeDirectories turns that into a clean absolute path;
		// AddShaderSourceDirectoryMapping wants a real directory, and a path
		// still containing ".." reads badly in any error it later logs.
		const FString ProjectShaderDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/VoxelEarth"), ProjectShaderDir);

		FString VoxelCoreShaderDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("../voxel-core/shaders"));
		FPaths::CollapseRelativeDirectories(VoxelCoreShaderDir);
		AddShaderSourceDirectoryMapping(TEXT("/VoxelCore"), VoxelCoreShaderDir);
	}

	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FVoxelEarthShadersModule, VoxelEarthShaders);
