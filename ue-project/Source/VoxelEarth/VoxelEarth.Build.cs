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

		// -------------------------------------------------------------------
		// `.vxtl` v2 CODEC_ZSTD: which zstd this binary decodes fine tiles with
		// (docs/vxtl-v2-format.md §3, Source/VoxelEarth/VoxelTileCodec.h).
		//
		// voxel-core deliberately contains NO zstd -- it has zero third-party
		// dependencies, and vendoring one into a static library linked into
		// this binary would risk a second copy of zstd's symbols alongside
		// anyone else's. That is an ODR/symbol-collision hazard whose failure
		// mode is wrong terrain, not a link error. So voxel-core takes an
		// injected decompressor and the choice is made HERE, once.
		//
		// MEASURED, not assumed (2026-07-29, UE 5.8.0-55116800 at D:\UE_5.8):
		// the binary/launcher engine distribution does NOT ship a C/C++ zstd.
		// No zstd.h, no zstd*.lib, no zstd module, and no .Build.cs under
		// Engine/Source or Engine/Plugins referencing one -- it ships Oodle
		// and LZ4 in Runtime/Core, plus a C# ZstdSharp.dll used by UBT and a
		// few zstd .tps/LICENSE records belonging to other prebuilt libraries.
		// A from-source engine build, or a plugin, may provide one. Nothing
		// here may assume it, so this probes and reports rather than requiring.
		//
		// The collision half of the argument did check out, though: scanning
		// every engine binary over 200 KB for the symbol ZSTD_decompress finds
		// it statically linked inside ThirdParty/Blosc's libblosc.lib, which
		// bundles zstd. A zstd vendored into voxel-core would therefore be
		// sharing a binary with another zstd's C symbols at a version nobody
		// chose. See Source/VoxelEarth/VoxelTileCodec.h.
		//
		// With no zstd found, the module compiles and links exactly as before
		// and CODEC_RAW tiles are unaffected; a CODEC_ZSTD tile is then refused
		// whole by vxc::FineTile::parse with FineError::kNoDecompressor, and
		// FVoxelEarthModule::StartupModule says so in the log. It is never
		// decoded as zeros.
		string ZstdModule = null;
		string[] ZstdCandidates = new string[]
		{
			// Project-local first: if this project ever vendors zstd for the
			// client, that is the copy this module should use, not a different
			// one the engine might acquire later.
			Path.Combine(ModuleDirectory, "..", "ThirdParty", "zstd"),
			Path.Combine(EngineDirectory, "Source", "ThirdParty", "zstd"),
		};
		foreach (string Candidate in ZstdCandidates)
		{
			if (!Directory.Exists(Candidate)) { continue; }
			string[] Rules = Directory.GetFiles(Candidate, "*.Build.cs", SearchOption.TopDirectoryOnly);
			if (Rules.Length == 0) { continue; }
			// The module name is the .Build.cs basename, not the folder name --
			// they are conventionally equal but UBT keys on the former, and
			// guessing "zstd" would fail at link time rather than here.
			ZstdModule = Path.GetFileName(Rules[0]);
			ZstdModule = ZstdModule.Substring(0, ZstdModule.Length - ".Build.cs".Length);
			break;
		}

		if (ZstdModule != null)
		{
			PrivateDependencyModuleNames.Add(ZstdModule);
			PrivateDefinitions.Add("VOXELEARTH_WITH_ZSTD=1");
		}
		else
		{
			PrivateDefinitions.Add("VOXELEARTH_WITH_ZSTD=0");
			Target.Logger.LogInformation(
				"VoxelEarth: no zstd module found (looked for a *.Build.cs in {Candidates}). " +
				"Building without CODEC_ZSTD support: .vxtl v2 CODEC_RAW tiles are unaffected, " +
				"and a CODEC_ZSTD tile will be refused with FineError::kNoDecompressor rather " +
				"than mis-decoded. See Source/VoxelEarth/VoxelTileCodec.h.",
				string.Join(", ", ZstdCandidates));
		}

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
