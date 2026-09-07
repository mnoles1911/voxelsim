#!/usr/bin/env python3
"""Ban floating types in deterministic voxel derivation; pin reviewed API boundaries.

Each exemption is an exact code line, not a filename/region wildcard. A new
floating declaration, a modified exempt line, or a stale exemption fails CI.
The core implementations behind these interfaces still use integer voxel
coordinates/materials and integer pitch on disk. These exceptions do not grant
permission to add floating world generation math.
"""
from __future__ import annotations
import pathlib
import re
import sys

# Exact reviewed boundary declarations, with their reason adjacent to the list.
APPROVED = {
    # Manifest/schema metadata converted from integer micrometres; binary pitches
    # 100/50/25/12.5 mm remain exactly representable in the compatibility API.
    'voxel-core/include/voxelcore/assetmanifest.h': {
        'double voxelSizeMm = 0;',
    },
    'voxel-core/src/assetmanifest.cpp': {
        's.voxelSizeMm = double(readU32(p + 72)) / (version == 3 ? 1000.0 : 1.0);',
    },
    # Admission only compares canonical pitch against 100 mm, never derives cells.
    'voxel-core/include/voxelcore/assetplacement.h': {
        'inline bool assetLayerAdmitsVoxelSize(const AssetLayer& layer, double voxelSizeMm) {',
    },
    'voxel-core/include/voxelcore/assetpolicy.h': {
        'double voxelSizeMm = kVoxelSizeMm;',
    },
    # Display/engine adapter constant; craft coordinates use kCraftPitchUm.
    'voxel-core/include/voxelcore/craftlattice.h': {
        'static constexpr double kCraftPitchMm = double(kCraftPitchUm) / 1000.0;',
    },
    # Read-only millimetre display/API adapter; authoritative pitch is integer micrometres.
    'voxel-core/include/voxelcore/assetgrid.h': {
        'double voxelSizeMm() const { return double(voxelSizeUm_) / 1000.0; }',
    },
    # Validated millimetre API and legacy wire conversion; stored authoritative pitch is integer micrometres.
    'voxel-core/include/voxelcore/editlog.h': {
        'EditLog log(seed, edge, std::move(providerId), fmt == 4 ? double(pitchMm)/1000.0 : double(pitchMm));',
        'const double um=mm*1000.0;',
        'double latticePitchMm = 0;',
        'double latticePitchMm = kVoxelSizeMm)',
        'double latticePitchMm() const { return double(latticePitchUm_) / 1000.0; }',
        'h.latticePitchMm = h.format == 4 ? double(wirePitch)/1000.0 : double(wirePitch);',
        'if (!std::isfinite(um) || um<1 || um>double(UINT32_MAX) || std::abs(um-std::round(um))>1e-6)',
        'static uint32_t encodePitchUm(double mm) {',
    },
    # Server actor seating against sampled terrain heights; does not derive terrain cells or voxel materials.
    'voxel-core/include/voxelcore/assetslopefit.h': {
        'double bottomMm = 0;',
        'double embedMm = 0) {',
        'double groundHighMm = 0;',
        'double groundLowMm = 0;',
        'double maximumBurialMm = 0;',
        'double maximumSinkMm, double maximumBurialMm,',
        'double originZMm = 0;',
        'double referenceZMm, double latticeMm,',
        'double sinkMm = 0;',
        'double target = referenceZMm;',
    },
    # Particle presentation collision/rebase API; authoritative occupancy packing and indices remain integer.
    'voxel-core/include/voxelcore/fluidoccupancy.h': {
        'const float localV = static_cast<float>(h.faceAxis == 0   ? h.vx - originVoxel[0]',
        'const float localV = static_cast<float>(v[a] - originVoxel[a]);',
        'const float posUU[3], const float prevPosUU[3]) {',
        'const float prevPosUU[3], const float posUU[3]) {',
        'float d[3], tMax[3], tDelta[3];',
        'float normal[3] = {0.0f, 0.0f, 0.0f};',
        'float posUU[3] = {0.0f, 0.0f, 0.0f};',
        'float tEnter = 0.0f;',
        'float tLast = 0.0f;',
        'inline constexpr float fluidRebaseDeltaUU(int32_t deltaVoxels) {',
        'inline constexpr float kFluidCollisionSkinUU = 0.01f;',
        'inline constexpr float kFluidNoCrossingT = 1e30f;',
        'inline constexpr float kFluidVoxelUU = static_cast<float>(kVoxelSizeMm) / 10.0f;',
        'inline float fluidMaxTraversalSpeedUU(float dtSeconds) {',
        'return static_cast<float>(deltaVoxels) * kFluidVoxelUU;',
        'return static_cast<float>(kFluidMaxCollisionSteps) * kFluidVoxelUU /',
    },
}


def code_lines(source: str) -> list[str]:
    # Preserve line numbers; ignore prose, string and character literals.
    pattern = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda m: "\n" * m.group().count("\n"), source, flags=re.S).splitlines()


def check(root: pathlib.Path) -> list[str]:
    errors = []
    seen = {path: set() for path in APPROVED}
    for base in ("voxel-core/include", "voxel-core/src"):
        for path in sorted((root / base).rglob("*")):
            if path.suffix not in (".h", ".hpp", ".cpp", ".cc"):
                continue
            relative = path.relative_to(root).as_posix()
            for number, line in enumerate(code_lines(path.read_text(encoding="utf-8", errors="replace")), 1):
                line = line.strip()
                if not re.search(r"\b(float|double)\b", line):
                    continue
                if line in APPROVED.get(relative, set()):
                    seen[relative].add(line)
                else:
                    errors.append(f"{relative}:{number}: unreviewed floating type: {line}")
    for path, approved in APPROVED.items():
        for stale in sorted(approved - seen[path]):
            errors.append(f"{path}: remove stale float boundary exemption: {stale}")
    return errors


if __name__ == "__main__":
    failures = check(pathlib.Path(__file__).resolve().parents[1])
    if failures:
        print("\n".join(failures))
        sys.exit(1)
    print("float-ban clean (exact reviewed API boundaries checked)")
