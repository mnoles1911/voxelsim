# ADR-0001: Vendor-neutral GPU compute; diffusion worker on rented NVIDIA cloud

- **Status:** accepted
- **Date:** 2026-07-19
- **Doctrine sections affected:** none violated — this *implements* §2.3
  (cross-vendor bit-determinism) and §3.4 (infra cost reality); it constrains
  the M0 "GPU compute port" implementation choice the plan left open.
- **Human sign-off:** Matt Noles, 2026-07-19 (approved the revised form below:
  Vulkan harness rather than the initially drafted D3D12 harness; diffusion
  cloud spend deferred to ~M2)

## Context

The primary dev machine (Matt's desktop) has an **AMD Radeon RX 7800 XT**.
There is no local NVIDIA GPU and therefore no CUDA anywhere in the loop.

Two M0 deliverables assumed "a GPU":

1. **terrain-diffusion worker** — upstream is PyTorch/CUDA. ROCm has no
   practical Windows story and torch-directml is slow and drifts numerically.
   Running the diffusion model locally is not viable on this hardware.
2. **GPU compute ports of amplifier + mesher** — the plan says "GPU compute"
   without fixing an API. CUDA is now off the table for local dev regardless.

Meanwhile the M0 determinism gate is *defined* as NVIDIA-vs-AMD bit-equality —
and we permanently have one AMD machine in hand.

## Decision

1. **GPU compute ports are written in HLSL compute shaders**, integer-only per
   docs/determinism.md, hosted two ways from one shader source:
   - inside UE (RDG compute passes, DXC → DXIL) for the shipping game, and
   - a thin standalone **Vulkan** harness in `voxel-core/bench` (same HLSL,
     DXC → SPIR-V) for headless determinism/perf CI — no UE dependency; runs
     on the local Windows/AMD leg AND cheap Linux/NVIDIA cloud runners, and
     is the future path for Linux dedicated servers doing generation.
   No CUDA, no vendor intrinsics, no wave-size assumptions; explicit
   `[numthreads]` and integer math only. Note: worldgen hash v1 needs 64-bit
   integer ops (`shaderInt64` / `Int64ShaderOps`) — supported on both target
   GPU families; if 64-bit multiply profiles badly, the remedy is a
   deliberate 32-bit hash under a kWorldGenVersion bump, never a silent one.
2. **The terrain-diffusion worker never runs on dev hardware.** It runs on a
   rented NVIDIA GPU (serverless scale-to-zero, plan §3.4: generation cost
   scales with newly explored area, not players). Dev and CI use the synthetic
   provider; canonical tiles are generated in the cloud, cached, and
   distributed as data — which doctrine §2.3 already requires, since diffusion
   output was never cross-GPU deterministic in the first place. **Cloud spend
   is deferred**: no rental until real terrain matters (~M2 vistas); synthetic
   tiles carry all development until then.
3. **This desktop is the canonical AMD leg of the M0 determinism gate.** The
   NVIDIA leg runs on a rented/CI Linux NVIDIA machine. Gate = identical
   `vxc_bench --digest` output from the Vulkan harness on both.

## Consequences

- Easier: local GPU work starts now on AMD; the cross-vendor gate becomes
  concretely testable instead of aspirational; one HLSL source serves UE and
  headless CI.
- Harder: a small Vulkan harness must be written and maintained (plus the DXC
  → SPIR-V pipeline); determinism discipline in HLSL (no fast-math, no
  undefined wave ops) needs its own lint/tests. The CPU reference remains the
  source of truth that GPU output must match bit-exactly.
- Option kept open: a D3D12 path for the same HLSL is trivial to add later if
  ever needed (the source language already targets DXIL for UE).
- terrain-diffusion bring-up moves from "local desktop task" to "cloud GPU
  task" (Runpod/Modal-class rental, ~$0.35–0.7/hr, scale-to-zero).
