#include "VoxelTileCodec.h"

#include "VoxelEarth.h"

#include "HAL/PlatformProcess.h" // runtime zstd bind (GetDllHandle/GetDllExport)
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#ifndef VOXELEARTH_WITH_ZSTD
#define VOXELEARTH_WITH_ZSTD 0
#endif

#if VOXELEARTH_WITH_ZSTD
// Supplied by whichever zstd module VoxelEarth.Build.cs found. voxel-core
// never sees this header, and this is the ONLY translation unit in the project
// that does -- which is the whole point of putting the boundary here.
#include "zstd.h"
#endif

namespace VoxelEarth
{
	namespace
	{
#if VOXELEARTH_WITH_ZSTD
		/**
		 * vxc::FineDecompressFn over the host zstd. One §4 block frame in, the
		 * caller's exactly-sized buffer out.
		 *
		 * Every line of this is a clause of tilestore.h's contract:
		 *
		 *  * ZSTD_decompress returns the number of bytes produced, or an error
		 *    code. `Produced != DstLen` therefore covers BOTH the error case
		 *    and a frame that expands to the wrong size -- truncated, padded,
		 *    or simply not the frame the block index claims. Under CODEC_ZSTD
		 *    comp_len is the COMPRESSED length and constrains nothing, so this
		 *    equality is the only length check that exists. Getting it wrong
		 *    does not crash; it produces plausible, wrong terrain.
		 *
		 *  * ZSTD_isError() is checked explicitly as well, because an error
		 *    code is a size_t near SIZE_MAX and reading it as a length would
		 *    be an unbounded write if the equality above ever loosened.
		 *
		 *  * No state is kept between calls -- no context, no dictionary, no
		 *    reuse. Blocks are independent (§4) and that is exactly what buys
		 *    per-block random access; a shared ZSTD_DCtx here would also make
		 *    this non-thread-safe, and FineTileSampler::prewarm exists so
		 *    meshing workers can decode from several threads.
		 *
		 *  * It cannot throw. voxel-core is exception-free on the query path.
		 *
		 * Decode stays a pure integer function of the bytes (§7) because zstd
		 * frame decode is bit-exact by format definition -- any compliant
		 * decoder, any version, same bytes out. Nothing here is allowed to
		 * introduce host-dependent behaviour, which is why there is no
		 * platform branch, no size heuristic and no fallback path.
		 */
		bool ZstdDecompressBlock(void* /*User*/, const uint8_t* Src, size_t SrcLen,
		                         uint8_t* Dst, size_t DstLen)
		{
			if (Src == nullptr || Dst == nullptr || SrcLen == 0 || DstLen == 0)
			{
				return false;
			}
			const size_t Produced = ZSTD_decompress(Dst, DstLen, Src, SrcLen);
			if (ZSTD_isError(Produced))
			{
				return false;
			}
			return Produced == DstLen;
		}
#endif // VOXELEARTH_WITH_ZSTD

		// --- runtime-bound zstd ---------------------------------------------
		//
		// Same contract as ZstdDecompressBlock above, against pointers resolved
		// by the dynamic loader instead of by the linker. See the header for
		// why this route exists at all: binding at runtime introduces no
		// link-time zstd symbols, so it cannot collide with the zstd already
		// statically linked inside ThirdParty/Blosc's libblosc.lib -- the
		// hazard VoxelEarth.Build.cs measured and that keeps zstd out of
		// voxel-core.
		//
		// Signatures are zstd's public C ABI and are restated rather than
		// included, because including zstd.h is precisely what this avoids.
		// They have been stable since zstd 1.0 and are covered by its
		// API-stability promise; a library that does not match is rejected by
		// the both-or-neither export check in TryRegisterRuntimeZstd.
		using FZstdDecompressFn = size_t (*)(void* Dst, size_t DstCap, const void* Src,
		                                     size_t SrcSize);
		using FZstdIsErrorFn = unsigned (*)(size_t Code);

		void* GRuntimeZstdHandle = nullptr;
		FZstdDecompressFn GRuntimeZstdDecompress = nullptr;
		FZstdIsErrorFn GRuntimeZstdIsError = nullptr;
		FString GRuntimeZstdPath;

		bool RuntimeZstdDecompressBlock(void* /*User*/, const uint8_t* Src, size_t SrcLen,
		                                uint8_t* Dst, size_t DstLen)
		{
			if (Src == nullptr || Dst == nullptr || SrcLen == 0 || DstLen == 0)
			{
				return false;
			}
			if (GRuntimeZstdDecompress == nullptr || GRuntimeZstdIsError == nullptr)
			{
				return false;
			}
			const size_t Produced = GRuntimeZstdDecompress(Dst, DstLen, Src, SrcLen);
			if (GRuntimeZstdIsError(Produced))
			{
				return false;
			}
			// The exact-length clause, and the only length check that means
			// anything under CODEC_ZSTD -- see ZstdDecompressBlock's comment.
			return Produced == DstLen;
		}

		vxc::FineDecompressor& MutableDecompressor()
		{
			static vxc::FineDecompressor Instance = []
			{
				vxc::FineDecompressor D;
#if VOXELEARTH_WITH_ZSTD
				D.fn = &ZstdDecompressBlock;
				D.user = nullptr;
#endif
				return D;
			}();
			return Instance;
		}
	} // namespace

	vxc::FineDecompressor GetFineTileDecompressor()
	{
		return MutableDecompressor();
	}

	bool HasFineTileDecompressor()
	{
		return MutableDecompressor().valid();
	}

	void SetFineTileDecompressor(const vxc::FineDecompressor& Decompressor)
	{
		MutableDecompressor() = Decompressor;
	}

	bool TryRegisterRuntimeZstd()
	{
		// Already have one (compile-time zstd, or an explicit
		// SetFineTileDecompressor from a plugin or a test): never override it.
		// Whoever registered deliberately outranks a library found by search.
		if (HasFineTileDecompressor())
		{
			return true;
		}
		if (GRuntimeZstdHandle != nullptr)
		{
			return GRuntimeZstdDecompress != nullptr; // already tried
		}

		TArray<FString> Candidates;
		FString Override;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelZstdDll="), Override) && !Override.IsEmpty())
		{
			Candidates.Add(Override);
		}
		// Shipped with the game, if anyone puts one there.
		const FString Shipped = FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries"),
		                                        TEXT("ThirdParty"), TEXT("zstd"),
		                                        FString(FPlatformProcess::GetBinariesSubdirectory()));
		// Bare names last, so the platform's own search path is the fallback
		// rather than the first thing tried -- a game that ships its own zstd
		// must not be silently overridden by whatever is on PATH.
#if PLATFORM_WINDOWS
		const TCHAR* Names[] = {TEXT("libzstd.dll"), TEXT("zstd.dll")};
#elif PLATFORM_MAC
		const TCHAR* Names[] = {TEXT("libzstd.1.dylib"), TEXT("libzstd.dylib")};
#else
		const TCHAR* Names[] = {TEXT("libzstd.so.1"), TEXT("libzstd.so")};
#endif
		for (const TCHAR* Name : Names)
		{
			Candidates.Add(FPaths::Combine(Shipped, Name));
		}
		for (const TCHAR* Name : Names)
		{
			Candidates.Add(FString(Name));
		}

		for (const FString& Candidate : Candidates)
		{
			void* Handle = FPlatformProcess::GetDllHandle(*Candidate);
			if (Handle == nullptr)
			{
				continue;
			}
			// BIND BOTH OR NEITHER. A library that exports ZSTD_decompress but
			// not ZSTD_isError is not a zstd this code understands, and
			// registering a half-bound decompressor would turn a deployment
			// mistake into wrong terrain -- exactly what the whole
			// kNoDecompressor path exists to prevent.
			auto* Decompress = reinterpret_cast<FZstdDecompressFn>(
			    FPlatformProcess::GetDllExport(Handle, TEXT("ZSTD_decompress")));
			auto* IsError = reinterpret_cast<FZstdIsErrorFn>(
			    FPlatformProcess::GetDllExport(Handle, TEXT("ZSTD_isError")));
			if (Decompress == nullptr || IsError == nullptr)
			{
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("vxtl v2 CODEC_ZSTD: loaded '%s' but it does not export both "
				            "ZSTD_decompress and ZSTD_isError -- ignoring it rather than "
				            "registering a half-bound decompressor."),
				       *Candidate);
				FPlatformProcess::FreeDllHandle(Handle);
				continue;
			}
			GRuntimeZstdHandle = Handle;
			GRuntimeZstdDecompress = Decompress;
			GRuntimeZstdIsError = IsError;
			GRuntimeZstdPath = Candidate;

			vxc::FineDecompressor D;
			D.fn = &RuntimeZstdDecompressBlock;
			D.user = nullptr;
			SetFineTileDecompressor(D);
			return true;
		}
		return false;
	}

	void LogFineTileCodecStatus()
	{
		if (HasFineTileDecompressor())
		{
			if (!GRuntimeZstdPath.IsEmpty())
			{
				UE_LOG(LogVoxelEarth, Log,
				       TEXT("vxtl v2 CODEC_ZSTD: decompressor bound at RUNTIME from '%s'. No "
				            "zstd symbols are linked into this binary, so it cannot collide "
				            "with the zstd already inside ThirdParty/Blosc."),
				       *GRuntimeZstdPath);
				return;
			}
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("vxtl v2 CODEC_ZSTD: decompressor registered at the host boundary "
			            "(voxel-core links none of its own)."));
			return;
		}

		// Warning, not Error: a build that only ever sees CODEC_RAW tiles is
		// perfectly valid, and the fine tier is not wired into the runtime yet.
		// What must never happen is discovering this from wrong terrain, so it
		// is stated once, up front, with the exact reason code the parse will
		// give.
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("vxtl v2 CODEC_ZSTD: NO decompressor registered, and no zstd was found "
		            "at runtime. CODEC_RAW tiles are unaffected; a CODEC_ZSTD tile will be "
		            "REFUSED WHOLE by vxc::FineTile::parse with FineError::kNoDecompressor "
		            "(never decoded as zeros). Fixes, cheapest first: drop a zstd shared "
		            "library in <Project>/Binaries/ThirdParty/zstd/%s/, pass "
		            "-VoxelZstdDll=<path>, build with a zstd module visible to "
		            "VoxelEarth.Build.cs, or call VoxelEarth::SetFineTileDecompressor "
		            "before loading tiles."),
		       FPlatformProcess::GetBinariesSubdirectory());
	}
} // namespace VoxelEarth
