#include "VoxelTileCodec.h"

#include "VoxelEarth.h"

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

	void LogFineTileCodecStatus()
	{
		if (HasFineTileDecompressor())
		{
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
		       TEXT("vxtl v2 CODEC_ZSTD: NO decompressor registered. CODEC_RAW tiles are "
		            "unaffected; a CODEC_ZSTD tile will be REFUSED WHOLE by "
		            "vxc::FineTile::parse with FineError::kNoDecompressor (never decoded "
		            "as zeros). Build with a zstd module visible to VoxelEarth.Build.cs, "
		            "or call VoxelEarth::SetFineTileDecompressor before loading tiles."));
	}
} // namespace VoxelEarth
