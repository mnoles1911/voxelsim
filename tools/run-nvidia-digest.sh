#!/usr/bin/env bash
# One-shot M0 cross-vendor (NVIDIA vs AMD) determinism gate runner, for a
# fresh Ubuntu box with an NVIDIA GPU (ADR-0001). Installs build deps,
# proves the Vulkan device really is the NVIDIA GPU, builds vxc_gpu against
# the SPIR-V kernels already committed under voxel-core/shaders/prebuilt/
# (compiled once on a Windows box via the DXC flow — SPIR-V is portable
# bytecode, so no DXC/HLSL toolchain is needed here at all), runs the
# harness, and prints a single clear PASS/FAIL line.
#
# Usage (from the repo root, on the rented/CI Ubuntu+NVIDIA box):
#   tools/run-nvidia-digest.sh
#
# What "M0 close" means: this harness byte-compares GPU output against the
# CPU reference (vxc::Amplifier / vxc::meshBrick) on THIS NVIDIA GPU, the
# same check voxel-core/bench/gpu_harness.cpp already runs and PASSes on the
# AMD leg (see voxel-core/shaders/prebuilt/README.md for the AMD-leg
# digests). Both legs independently agreeing with the CPU reference closes
# the NVIDIA-vs-AMD gate transitively — this script does not diff digests
# against AMD directly, it just needs to print PASS with a digest, which the
# operator then eyeballs against prebuilt/README.md's AMD-leg digest table
# (identical committed .spv + identical seed => the digests SHOULD match
# too, as a bonus signal, but the authoritative check is each leg's own
# GPU-vs-CPU byte-compare).
#
# Idempotent: safe to re-run; apt-get/cmake/build steps are all no-ops (or
# fast) on a box that already has everything from a prior run.
set -euo pipefail

log() { printf '\n=== %s ===\n' "$1"; }

# --- 0. locate the repo root (this script lives in tools/) -----------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." &>/dev/null && pwd)"
cd "${REPO_ROOT}"

SPV_DIR="voxel-core/shaders/prebuilt"
if [[ ! -f "${SPV_DIR}/worldgen.ColumnMain.spv" ]]; then
  echo "FATAL: ${SPV_DIR}/worldgen.ColumnMain.spv missing — this script expects the" >&2
  echo "committed prebuilt SPIR-V kernels to already be present at that path." >&2
  exit 1
fi

# --- 1. install deps --------------------------------------------------------
log "Installing build + Vulkan dependencies"
sudo apt-get update
sudo apt-get install -y build-essential cmake git libvulkan-dev vulkan-tools

# --- 2. prove the Vulkan device really is the NVIDIA GPU --------------------
# The operator SEES this in the log before any gate result is trusted: if
# the NVIDIA driver isn't installed (or vulkaninfo picks a software/llvmpipe
# fallback), the harness will still happily dispatch on whatever device it
# finds first — this line is the guard against silently gating on the wrong
# GPU.
log "Vulkan device (must show the NVIDIA GPU)"
vulkaninfo | grep -i deviceName || {
  echo "FATAL: 'vulkaninfo | grep -i deviceName' found nothing — no Vulkan-capable" >&2
  echo "device visible. Check the NVIDIA driver + Vulkan ICD are installed" >&2
  echo "(e.g. nvidia-driver-XXX, and /usr/share/vulkan/icd.d/nvidia_icd.json exists)." >&2
  exit 1
}

# --- 3. configure + build vxc_gpu -------------------------------------------
log "Configuring (VXC_BUILD_GPU_HARNESS=ON)"
cmake -S voxel-core -B build-gpu \
  -DVXC_BUILD_GPU_HARNESS=ON \
  -DCMAKE_BUILD_TYPE=Release

log "Building vxc_gpu"
cmake --build build-gpu --config Release --target vxc_gpu -j"$(nproc)"

VXC_GPU_BIN="build-gpu/bench/vxc_gpu"
if [[ ! -x "${VXC_GPU_BIN}" ]]; then
  echo "FATAL: ${VXC_GPU_BIN} not found after build" >&2
  exit 1
fi

# --- 4. run the determinism harness -----------------------------------------
# Default (column-only regions) mode first: fast, always run. --radius 64
# mirrors the AMD leg's recorded gate check (see prebuilt/README.md) and
# adds the full columns+cells+quads comparison over a real tiled area.
log "Running vxc_gpu (default regions mode)"
set +e
"${VXC_GPU_BIN}"
DEFAULT_RC=$?
set -e

log "Running vxc_gpu --radius 64"
set +e
"${VXC_GPU_BIN}" --radius 64
RADIUS_RC=$?
set -e

DEVICE_NAME="$(vulkaninfo | grep -i deviceName | head -1 | sed -E 's/^[[:space:]]*deviceName[[:space:]]*=[[:space:]]*//')"

if [[ ${DEFAULT_RC} -eq 0 && ${RADIUS_RC} -eq 0 ]]; then
  echo
  echo "=== M0 NVIDIA DETERMINISM: PASS (gpu=${DEVICE_NAME}) ==="
  exit 0
else
  echo
  echo "=== M0 NVIDIA DETERMINISM: FAIL (gpu=${DEVICE_NAME}) ===" >&2
  echo "(default-regions exit=${DEFAULT_RC}, --radius 64 exit=${RADIUS_RC})" >&2
  exit 1
fi
