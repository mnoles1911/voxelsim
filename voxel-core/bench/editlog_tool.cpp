// vxc_editlog: offline edit-log tool (plan §3.2 "compacted offline"; M3
// persistence groundwork). Wires voxelcore/editcompact.h's compactLog() to
// real file I/O so it has an actual caller ahead of a future server
// save/load cycle (see docs/status.md).
//
//   vxc_editlog stats   <file>              parse + print log summary
//   vxc_editlog compact <in> <out>          compact, write atomically, report
//   vxc_editlog verify  <original> <compacted>
//                                            replay both against World<8>,
//                                            compare editedDigest()
//   vxc_editlog selftest                    end-to-end round trip via a
//                                            temp file, through the same
//                                            code paths as the commands
//                                            above (wired into CTest)
//
// No floats: reduction percentages are computed and printed as integer
// per-mille split into whole/tenths, matching the rest of voxel-core's
// float-free contract (bench_main.cpp's timing report is the one place
// floats are allowed, for wall-clock ms; this tool has none of that).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "voxelcore/editcompact.h"
#include "voxelcore/editlog.h"
#include "voxelcore/tiles.h"
#include "voxelcore/tilestore.h" // readFileBytes
#include "voxelcore/world.h"

using namespace vxc;
namespace fs = std::filesystem;

namespace {

void printUsage() {
    std::fprintf(stderr,
        "usage: vxc_editlog <command> [args]\n"
        "  vxc_editlog stats   <file>\n"
        "  vxc_editlog compact <in> <out>\n"
        "  vxc_editlog verify  <original> <compacted>\n"
        "  vxc_editlog selftest\n");
}

// One decimal place of a b/a percentage, computed entirely in integers:
// permille = floor(1000 * (before-after) / before), printed as ddd.d%.
void printReductionPercent(uint64_t before, uint64_t after) {
    if (before == 0) {
        std::printf("n/a (before was 0)");
        return;
    }
    const uint64_t shrink = before >= after ? before - after : 0;
    const uint64_t permille = shrink * 1000 / before;
    std::printf("%llu.%llu%%", static_cast<unsigned long long>(permille / 10),
                static_cast<unsigned long long>(permille % 10));
}

std::optional<EditLog> loadLogFile(const fs::path& path, std::string& err) {
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    if (!bytes) {
        err = "cannot open file: " + path.string();
        return std::nullopt;
    }
    std::optional<EditLog> log = EditLog::parse(bytes->data(), bytes->size());
    if (!log) {
        err = "corrupt or unrecognized edit-log file: " + path.string();
        return std::nullopt;
    }
    return log;
}

// Write bytes to `out` atomically: write to `out.tmp` then rename over the
// destination. Avoids ever leaving `out` truncated/partial if the process
// dies mid-write.
bool writeFileAtomic(const fs::path& out, const std::vector<uint8_t>& bytes, std::string& err) {
    fs::path tmp = out;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open temp file for write: " + tmp.string();
            return false;
        }
        if (!bytes.empty())
            f.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        if (!f) {
            err = "write failed: " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) {
        // Some platforms refuse rename-over-existing; fall back to
        // remove-then-rename rather than failing outright.
        std::error_code removeEc;
        fs::remove(out, removeEc);
        fs::rename(tmp, out, ec);
        if (ec) {
            err = "rename failed (" + tmp.string() + " -> " + out.string() + "): " + ec.message();
            fs::remove(tmp, removeEc);
            return false;
        }
    }
    return true;
}

size_t uniqueBrickCount(const EditLog& log) {
    std::unordered_set<BrickKey, BrickKeyHash> keys;
    for (const EditEntry& e : log.entries()) keys.insert(e.key);
    return keys.size();
}

uint64_t cellsTouched(const EditLog& log) {
    uint64_t total = 0;
    for (const EditEntry& e : log.entries()) total += e.cells.size();
    return total;
}

void printStatsBlock(const char* label, const EditLog& log, size_t serializedBytes) {
    std::printf("%s:\n", label);
    std::printf("  seed:            %llu\n", static_cast<unsigned long long>(log.seed()));
    std::printf("  brickEdge:       %d\n", int(log.brickEdge()));
    std::printf("  entries:         %zu\n", log.size());
    std::printf("  uniqueBricks:    %zu\n", uniqueBrickCount(log));
    std::printf("  serializedBytes: %zu\n", serializedBytes);
    std::printf("  cellsTouched:    %llu\n", static_cast<unsigned long long>(cellsTouched(log)));
}

// --- commands ----------------------------------------------------------

int cmdStats(const fs::path& path) {
    std::string err;
    std::optional<EditLog> log = loadLogFile(path, err);
    if (!log) {
        std::fprintf(stderr, "vxc_editlog stats: %s\n", err.c_str());
        return 1;
    }
    std::vector<uint8_t> bytes;
    log->serialize(bytes);
    printStatsBlock(path.string().c_str(), *log, bytes.size());
    return 0;
}

// Shared by `compact` and `selftest`. On success, writes `out` and reports
// before/after via stdout (unless `quiet`); returns true/false for caller
// to turn into an exit code.
bool runCompact(const fs::path& in, const fs::path& out, bool quiet) {
    std::string err;
    std::optional<EditLog> log = loadLogFile(in, err);
    if (!log) {
        std::fprintf(stderr, "vxc_editlog compact: %s\n", err.c_str());
        return false;
    }

    EditLog compacted = compactLog(*log);

    std::vector<uint8_t> origBytes, compactBytes;
    log->serialize(origBytes);
    compacted.serialize(compactBytes);

    if (!writeFileAtomic(out, compactBytes, err)) {
        std::fprintf(stderr, "vxc_editlog compact: %s\n", err.c_str());
        return false;
    }

    if (!quiet) {
        std::printf("entries: %zu -> %zu (", log->size(), compacted.size());
        printReductionPercent(log->size(), compacted.size());
        std::printf(" reduction)\n");
        std::printf("bytes:   %zu -> %zu (", origBytes.size(), compactBytes.size());
        printReductionPercent(origBytes.size(), compactBytes.size());
        std::printf(" reduction)\n");
        std::printf("wrote %s\n", out.string().c_str());
    }
    return true;
}

int cmdCompact(const fs::path& in, const fs::path& out) { return runCompact(in, out, false) ? 0 : 1; }

// Shared by `verify` and `selftest`. Replays both logs against a fresh
// World<8>/SyntheticTileSampler pair seeded from each log's own header and
// compares editedDigest(). Returns true on PASS.
bool runVerify(const fs::path& origPath, const fs::path& compactPath, bool quiet) {
    std::string err;
    std::optional<EditLog> origLog = loadLogFile(origPath, err);
    if (!origLog) {
        std::fprintf(stderr, "vxc_editlog verify: %s\n", err.c_str());
        return false;
    }
    std::optional<EditLog> compactLogParsed = loadLogFile(compactPath, err);
    if (!compactLogParsed) {
        std::fprintf(stderr, "vxc_editlog verify: %s\n", err.c_str());
        return false;
    }

    for (const EditLog* log : {&*origLog, &*compactLogParsed}) {
        if (log->brickEdge() == 16) {
            std::fprintf(stderr,
                "vxc_editlog verify: brickEdge 16 logs are not supported by this tool yet "
                "(World<8> only) -- got a brickEdge-16 log\n");
            return false;
        }
        if (log->brickEdge() != 8) {
            std::fprintf(stderr,
                "vxc_editlog verify: unsupported brickEdge %d (only 8 is supported)\n",
                int(log->brickEdge()));
            return false;
        }
    }

    if (origLog->seed() != compactLogParsed->seed() ||
        origLog->brickEdge() != compactLogParsed->brickEdge()) {
        std::fprintf(stderr,
            "vxc_editlog verify: seed/brickEdge mismatch between the two logs -- they cannot "
            "be replayed against the same world\n");
        return false;
    }

    SyntheticTileSampler tilesOrig(origLog->seed()), tilesCompact(compactLogParsed->seed());
    World<8> worldOrig(origLog->seed(), tilesOrig);
    World<8> worldCompact(compactLogParsed->seed(), tilesCompact);

    if (!worldOrig.replay(*origLog)) {
        std::fprintf(stderr, "vxc_editlog verify: replay of %s failed\n", origPath.string().c_str());
        return false;
    }
    if (!worldCompact.replay(*compactLogParsed)) {
        std::fprintf(stderr, "vxc_editlog verify: replay of %s failed\n",
                     compactPath.string().c_str());
        return false;
    }

    const uint64_t digestOrig = worldOrig.editedDigest();
    const uint64_t digestCompact = worldCompact.editedDigest();
    const bool pass = digestOrig == digestCompact;

    if (!quiet) {
        std::printf("original digest:  %016llx\n", static_cast<unsigned long long>(digestOrig));
        std::printf("compacted digest: %016llx\n", static_cast<unsigned long long>(digestCompact));
        std::printf("%s\n", pass ? "PASS" : "FAIL");
    }
    return pass;
}

int cmdVerify(const fs::path& origPath, const fs::path& compactPath) {
    return runVerify(origPath, compactPath, false) ? 0 : 1;
}

// Builds a deliberately messy in-memory log (same pattern as
// tests/test_editcompact.cpp's buildMessyLog): a multi-brick crater dig, a
// pillar build with an overwrite-then-revert, a dig-then-refill net no-op,
// and a flip-flopped single voxel across several entries -- enough real
// work that compaction actually collapses something.
EditLog buildMessyLog(uint64_t seed) {
    SyntheticTileSampler tiles(seed);
    World<8> w(seed, tiles);
    const ColumnSample col = w.amplifier().column(0, 0);
    const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);

    for (int64_t x = -5; x <= 5; ++x)
        for (int64_t y = -5; y <= 5; ++y)
            for (int64_t z = topVz - 3; z <= topVz; ++z)
                if (x * x + y * y + (z - topVz) * (z - topVz) < 20) w.setVoxel(x, y, z, MAT_AIR);

    for (int64_t z = topVz; z < topVz + 12; ++z) w.setVoxel(20, 20, z, MAT_ROCK);
    w.setVoxel(20, 20, topVz + 5, MAT_SAND);
    w.setVoxel(20, 20, topVz + 5, MAT_ROCK);
    w.setVoxel(20, 20, topVz + 2, MAT_AIR);
    w.setVoxel(20, 20, topVz + 2, MAT_ROCK);

    for (int i = 0; i < 5; ++i)
        w.setVoxel(0, 0, topVz - 1, (i % 2 == 0) ? MAT_AIR : MAT_SAND);

    return w.log();
}

int cmdSelftest() {
    constexpr uint64_t kSeed = 20260720;

    const fs::path tmpDir = fs::temp_directory_path();
    const fs::path origPath = tmpDir / "vxc_editlog_selftest_orig.vxel";
    const fs::path compactPath = tmpDir / "vxc_editlog_selftest_compact.vxel";

    // Exercise the real file-write path: serialize the messy log and write
    // it out atomically, same as a real caller would.
    EditLog log = buildMessyLog(kSeed);
    std::vector<uint8_t> logBytes;
    log.serialize(logBytes);
    std::string err;
    if (!writeFileAtomic(origPath, logBytes, err)) {
        std::fprintf(stderr, "vxc_editlog selftest: %s\n", err.c_str());
        return 1;
    }

    std::printf("selftest: wrote messy log (%zu entries, %zu bytes) to %s\n", log.size(),
                logBytes.size(), origPath.string().c_str());

    bool ok = runCompact(origPath, compactPath, /*quiet=*/false);
    if (ok) ok = runVerify(origPath, compactPath, /*quiet=*/false);

    std::error_code ec;
    if (!std::getenv("VXC_EDITLOG_SELFTEST_KEEP_TEMP")) {
        fs::remove(origPath, ec);
        fs::remove(compactPath, ec);
    }

    std::printf("selftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    const std::string cmd = argv[1];

    if (cmd == "stats") {
        if (argc != 3) {
            printUsage();
            return 2;
        }
        return cmdStats(argv[2]);
    }
    if (cmd == "compact") {
        if (argc != 4) {
            printUsage();
            return 2;
        }
        return cmdCompact(argv[2], argv[3]);
    }
    if (cmd == "verify") {
        if (argc != 4) {
            printUsage();
            return 2;
        }
        return cmdVerify(argv[2], argv[3]);
    }
    if (cmd == "selftest") {
        if (argc != 2) {
            printUsage();
            return 2;
        }
        return cmdSelftest();
    }

    std::fprintf(stderr, "vxc_editlog: unknown command '%s'\n", cmd.c_str());
    printUsage();
    return 2;
}
