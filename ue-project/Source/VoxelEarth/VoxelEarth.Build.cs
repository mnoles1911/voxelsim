using Microsoft.Extensions.Logging;
using System;
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
			"ProceduralMeshComponent",
			// ADR-0006 G2a: the GPU worldgen/mesher kernels. They live in their
			// own module because that module has to load at PostConfigInit to
			// register its shader directory before the global shader map is
			// built -- this one loads at Default, far too late. Used by
			// VoxelGpuVerify.cpp's voxel.GPU.VerifyRegion command.
			"VoxelEarthShaders"
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

		// STALENESS GUARD.
		//
		// This links a PREBUILT static library. Nothing in UBT's dependency graph
		// knows about voxel-core's sources, so editing amplifier.cpp and
		// rebuilding the editor silently links the OLD generator -- and the
		// failure is not a link error, it is WRONG TERRAIN. That is the worst
		// shape a build problem can have here, and it has already cost real time
		// twice: once as a GPU/CPU material mismatch chased into the shaders, and
		// once as a cross-toolchain determinism gate failure investigated as a
		// worldgen bug. In both cases the library was simply four hours older
		// than the sources it was built from.
		//
		// A warning, not an error: the lib may legitimately be newer than a
		// comment-only edit, and failing the build on a timestamp would be worse
		// than the problem. The point is that the next person sees it in the
		// build log instead of in the terrain.
		try
		{
			DateTime LibTime = File.GetLastWriteTimeUtc(VoxelCoreLib);
			string NewestName = null;
			DateTime NewestTime = DateTime.MinValue;
			foreach (string Dir in new string[] { Path.Combine(VoxelCoreRoot, "src"),
			                                      Path.Combine(VoxelCoreRoot, "include") })
			{
				if (!Directory.Exists(Dir)) { continue; }
				foreach (string Src in Directory.GetFiles(Dir, "*.*", SearchOption.AllDirectories))
				{
					string Ext = Path.GetExtension(Src).ToLowerInvariant();
					if (Ext != ".cpp" && Ext != ".h" && Ext != ".hpp" && Ext != ".inl") { continue; }
					DateTime T = File.GetLastWriteTimeUtc(Src);
					if (T > NewestTime) { NewestTime = T; NewestName = Src; }
				}
			}

			if (NewestName != null && NewestTime > LibTime)
			{
				Target.Logger.LogWarning(
					"VoxelEarth: voxelcore.lib is STALE (lib {LibTime}, newest source {SrcTime} -- {SrcName}). " +
					"This build will link an out-of-date generator and may produce wrong terrain or " +
					"fail the cross-toolchain determinism gate. " +
					"Rebuild it: cmake --build build/voxel-core-msvc --config Release",
					LibTime, NewestTime, NewestName);
			}
		}
		catch (Exception)
		{
			// A diagnostic must never be the reason a build fails.
		}

		PublicAdditionalLibraries.Add(VoxelCoreLib);
	}
}
