# The parked floor, named

2026-08-28. One leg, `Flight=static`, shipping default, 2560x1440, `-csvGpuStats`
with a deferred `CsvProfile FRAMES=9000`. Closes the open half of backlog 0.1b.

## Arm checks, before any number

    48 GPU/ columns present          (r.GPUCsvStatsEnabled defaults 0; absent = silent)
    GPU-CLOCK samples=27,587 ARMED
    mode=3 pop=SETTLED-PARKED, 27,027 frames -- p50 8.40  p95 8.92  p99 9.23
    RED ARM: GPU/VoxelMarch p50=4.180 ms on a scope known live
    Moving=0.000  SpeedMps=0.000  ChunksApplied=0.000  TickMs=0.055   <- genuinely parked

**And the decomposition checks itself:** the GPU columns sum to **6.949 ms**
against `VoxelStream/GpuFrameMs` **7.033 ms**, the engine's own clock, on the same
rows. 1.2% apart.

## The 6.95 ms parked GPU frame

    GPU/VoxelMarch                     4.186 ms   60.2%
    GPU/TemporalSuperResolution        1.385      19.9%
    GPU/Unaccounted                    0.168       2.4%
    GPU/Postprocessing                 0.127
    GPU/VoxelMarchEmit                 0.119
    GPU/SingleLayerWaterDepthPrepass   0.116
    GPU/VolumetricFog                  0.100
    GPU/SingleLayerWater               0.094
    GPU/Translucency                   0.086
    GPU/SkyAtmosphere                  0.065
    GPU/ShadowDepths                   0.058
    GPU/ReflectionEnvironment          0.055
    GPU/Fog / SkyAtmosphereLUTs /
      Lights / Basepass / everything    <= 0.050 each
    ------------------------------------------------
    SUM                                6.949

**The marcher is 60% of the parked GPU frame. TSR is 20%. Everything else in the
engine -- sky, water, fog, shadows, lights, reflections, translucency, post,
base pass -- is 20% TOGETHER, and no single item exceeds 0.13 ms.**

**No streaming term appears at all**, which is the expected reading for a parked
leg and is what makes this the floor rather than the tail.

## What this settles, and what it costs

Backlog 0.1b asked what the ~9.5 ms render block is. The answer in two parts:

1. **It is not the tail.** Render-thread BUSY moves +0.78 ms fast-to-tail while
   `renderWait` rises +2.91 and `rhi` +4.32 -- the render thread waits more, it
   does not work more (see `docs/recompute-split-2026-08-28.md`).
2. **It is the floor, and the floor is the marcher.** 4.186 of 6.949 ms.

**THE STRATEGIC CONSEQUENCE, and it is uncomfortable.** Goal 3a is p95 < 10 ms.
Parked is already 8.40 p50 / 8.92 p95 with a near-idle game thread and zero
streaming, so **the floor leaves ~1 ms of headroom** for everything streaming
does. Reaching >100 fps while moving therefore requires the FLOOR to fall, and
the floor is 60% one thing.

**And the marcher's only known lever is rejected.** Its cost is ray-count linear
within 2% and per-ray cost is immovable
([[voxelsim-marcher-cost-is-ray-count]]); the ray-count reduction that was built
-- half-res -- was judged four times by the owner and REJECTED FINAL on image
quality. Empty-space skipping has been run and is largely exhausted (`anySolid`
shipped for -0.13 ms, the height pyramid retired, ZCut refuted for the horizon).

So there is no fat left in the parked frame outside the marcher, and no unspent
marcher lever. **>100 fps steady is not reachable by tuning from here.** It needs
either a marcher algorithm change (not a knob) or an explicit decision to accept
the 1% low target that IS being met.

## The one number NOT settled by this leg

The parked *render thread* reads 8.12 ms against a 6.95 ms GPU frame. That ~1.2 ms
is render-thread CPU or waiting, and the GPU columns cannot see it.
`renderBusyMs` is not clean CPU time -- `TaskGraph.cpp:739-745` books every
dependent wait on the render thread's local queue as busy -- so naming that
residual needs a different instrument, not this one.
