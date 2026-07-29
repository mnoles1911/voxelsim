"""PROTOTYPE of the Phase 2 geomorphic bake, for MEASUREMENT not for shipping.

docs/terrain-amplification-plan.md open risk #1: the "~1.5 s/tile" bake cost is
an estimate, and the whole on-demand latency argument rests on it. This runs the
real algorithms at the real resolution on a real tile and times each stage.

What it is NOT: production code. No aprons, no cross-tile inflow boundary
conditions, no hydrology pyramid, no codec. It runs one tile in isolation, which
is exactly the thing the plan says produces seams — the point here is the COST
and the SHAPE of the output, not correctness at the edges.

Stages, matching the plan's B0-B3:
  B0  cubic B-spline upsample 30 m -> 3.75 m          (the carrier the client uses)
  B1  slope/curvature-conditioned fBm roughness       (substrate for erosion)
  B2  priority-flood fill -> D8 receivers -> flow accumulation -> stream-power
      incision  depth = K * A^m * S^n
  B3  slope-limited thermal relaxation                (talus, footslopes)

Run:  <terrain-diffusion venv python> bake_prototype.py <tile.vxtl> [--iters N]

Needs numba; the CPU here is not the GPU pod the plan assumes, so treat the
per-stage numbers as an upper bound on the parallelisable stages (B1/B3/carve)
and as roughly representative for the inherently sequential one (priority-flood
and the accumulation sweep).
"""
import argparse, struct, time
import numpy as np
from numba import njit, prange

TILE, SCALE = 512, 8
FINE = TILE * SCALE
PIXEL_M = 30.0 / SCALE


# --------------------------------------------------------------------------- io
def decode_vxtl(path):
    b = open(path, "rb").read()
    magic, _ver, _seed, _x, _y, _scale, size = struct.unpack_from("<4sHQiiBH", b, 0)
    assert magic == b"VXTL"
    off = struct.calcsize("<4sHQiiBH")
    elev = np.frombuffer(b, dtype="<i2", count=size * size, offset=off).reshape(size, size)
    return elev.astype(np.float32)


# ------------------------------------------------------------------- B0 carrier
def _weights(scale):
    T = 1024
    tq = (np.arange(scale) * T) // scale
    u = T - tq
    W = np.stack([u ** 3,
                  3 * tq ** 3 - 6 * tq ** 2 * T + 4 * T ** 3,
                  -3 * tq ** 3 + 3 * tq ** 2 * T + 3 * tq * T ** 2 + T ** 3,
                  tq ** 3], axis=1).astype(np.float64)
    return (W / (6.0 * T ** 3)).astype(np.float32)


def _up_axis(a, scale, W):
    rows, n = a.shape
    pad = np.pad(a, ((0, 0), (1, 2)), mode="edge")
    out = np.zeros((rows, n, scale), dtype=np.float32)
    for k in range(4):
        out += pad[:, k:k + n][:, :, None] * W[None, None, :, k]
    return out.reshape(rows, n * scale)


def bspline_upsample(a, scale):
    W = _weights(scale)
    b = _up_axis(a, scale, W)
    c = _up_axis(np.ascontiguousarray(b.T), scale, W)
    return np.ascontiguousarray(c.T)


# ----------------------------------------------------------------- B1 roughness
def conditioned_fbm(shape, slope, seed=20260719):
    """fBm whose amplitude follows local slope. Correlated by construction --
    the size measurement showed uncorrelated noise costs ~2x the bytes.

    EACH OCTAVE IS BICUBICALLY INTERPOLATED. The first version box-upsampled
    each octave with np.kron, which is nearest-neighbour, and the hillshade came
    out as a lattice of hard rectangles at every octave scale -- precisely the
    grid artifact this whole project exists to remove, reintroduced by the pass
    that is supposed to supply natural roughness. Worth recording because it is
    invisible in any statistic that averages over the field and obvious the
    instant it is shaded."""
    from scipy.ndimage import zoom
    rng = np.random.default_rng(seed)
    out = np.zeros(shape, dtype=np.float32)
    amp, size = 1.0, 16
    while size <= shape[0]:
        g = rng.standard_normal((size + 4, size + 4)).astype(np.float32)
        up = zoom(g, shape[0] / size, order=3, mode="nearest")   # bicubic
        out += amp * up[:shape[0], :shape[1]]
        amp *= 0.55
        size *= 2
    out /= out.std()
    gain = np.clip(slope / 0.3, 0.25, 2.0).astype(np.float32)   # steeper = rougher
    return out * gain


# ------------------------------------------------- B2 priority flood (heap, njit)
@njit(cache=True)
def priority_flood(z):
    """Barnes et al. priority-flood: raise every cell to the lowest elevation
    reachable from the boundary. Removes depressions so flow routing terminates."""
    h, w = z.shape
    out = z.copy()
    done = np.zeros((h, w), np.uint8)
    n = h * w
    hz = np.empty(n, np.float32)      # binary heap of elevations
    hi = np.empty(n, np.int64)        # ... and packed indices
    cnt = 0

    def_push_i = 0  # placeholder to keep numba happy about closures
    # seed with the border
    for y in range(h):
        for x in range(w):
            if y == 0 or x == 0 or y == h - 1 or x == w - 1:
                # push
                hz[cnt] = out[y, x]; hi[cnt] = y * w + x; cnt += 1
                c = cnt - 1
                while c > 0:
                    p = (c - 1) >> 1
                    if hz[p] <= hz[c]:
                        break
                    hz[p], hz[c] = hz[c], hz[p]
                    hi[p], hi[c] = hi[c], hi[p]
                    c = p
                done[y, x] = 1

    dy = np.array([-1, 1, 0, 0, -1, -1, 1, 1], np.int64)
    dx = np.array([0, 0, -1, 1, -1, 1, -1, 1], np.int64)

    while cnt > 0:
        ez = hz[0]; ei = hi[0]
        cnt -= 1
        hz[0] = hz[cnt]; hi[0] = hi[cnt]
        c = 0
        while True:                                   # sift down
            l = 2 * c + 1; r = l + 1; m = c
            if l < cnt and hz[l] < hz[m]: m = l
            if r < cnt and hz[r] < hz[m]: m = r
            if m == c: break
            hz[m], hz[c] = hz[c], hz[m]
            hi[m], hi[c] = hi[c], hi[m]
            c = m

        cy = ei // w; cx = ei % w
        for k in range(8):
            ny = cy + dy[k]; nx = cx + dx[k]
            if ny < 0 or nx < 0 or ny >= h or nx >= w or done[ny, nx]:
                continue
            if out[ny, nx] < ez:
                out[ny, nx] = ez                       # fill
            done[ny, nx] = 1
            hz[cnt] = out[ny, nx]; hi[cnt] = ny * w + nx; cnt += 1
            c = cnt - 1
            while c > 0:
                p = (c - 1) >> 1
                if hz[p] <= hz[c]: break
                hz[p], hz[c] = hz[c], hz[p]
                hi[p], hi[c] = hi[c], hi[p]
                c = p
    return out


# --------------------------------------------------------- D8 + flow accumulation
@njit(cache=True, parallel=True)
def d8_receiver(z, cell_m):
    h, w = z.shape
    rec = np.empty((h, w), np.int64)
    slope = np.zeros((h, w), np.float32)
    dy = np.array([-1, 1, 0, 0, -1, -1, 1, 1], np.int64)
    dx = np.array([0, 0, -1, 1, -1, 1, -1, 1], np.int64)
    dist = np.array([1.0, 1.0, 1.0, 1.0, 1.4142135, 1.4142135, 1.4142135, 1.4142135], np.float32)
    for y in prange(h):
        for x in range(w):
            best = 0.0; bi = -1
            for k in range(8):
                ny = y + dy[k]; nx = x + dx[k]
                if ny < 0 or nx < 0 or ny >= h or nx >= w:
                    continue
                s = (z[y, x] - z[ny, nx]) / (dist[k] * cell_m)
                if s > best:
                    best = s; bi = ny * w + nx
            rec[y, x] = bi
            slope[y, x] = best
    return rec, slope


@njit(cache=True)
def accumulate_mfd(z, order, cell_area, cell_m, p):
    """MULTIPLE-flow-direction accumulation, in one descending-elevation sweep.

    D8 -- send everything to the single steepest neighbour -- is what this did
    first, and the hillshade showed exactly the failure the plan predicted for
    it: channels can only run along 8 compass directions, so at 3.75 m/px the
    drainage came out as dead-straight 45-degree diagonals tens of pixels long.
    Faceting like that is not subtle and no amount of downstream smoothing hides
    it.

    MFD splits each cell's flow across ALL lower neighbours in proportion to
    slope^p, so the accumulation field is smooth and channels follow the terrain
    instead of the lattice. D8 is still the right thing for tracing a channel
    CENTRELINE; it is the wrong thing for the area field."""
    h, w = z.shape
    n = order.size
    acc = np.full(n, cell_area, np.float32)
    dy = np.array([-1, 1, 0, 0, -1, -1, 1, 1], np.int64)
    dx = np.array([0, 0, -1, 1, -1, 1, -1, 1], np.int64)
    dist = np.array([1.0, 1.0, 1.0, 1.0, 1.4142135, 1.4142135, 1.4142135, 1.4142135], np.float32)
    wgt = np.empty(8, np.float32)
    for i in range(n):
        c = order[i]
        cy = c // w
        cx = c % w
        zc = z[cy, cx]
        tot = np.float32(0.0)
        for k in range(8):
            ny = cy + dy[k]
            nx = cx + dx[k]
            wgt[k] = 0.0
            if ny < 0 or nx < 0 or ny >= h or nx >= w:
                continue
            s = (zc - z[ny, nx]) / (dist[k] * cell_m)
            if s > 0.0:
                wk = s ** p
                wgt[k] = wk
                tot += wk
        if tot <= 0.0:
            continue
        a = acc[c]
        for k in range(8):
            if wgt[k] > 0.0:
                acc[(cy + dy[k]) * w + (cx + dx[k])] += a * (wgt[k] / tot)
    return acc


# ------------------------------------------------------------ B3 thermal (njit)
@njit(cache=True, parallel=True)
def thermal_step(z, max_drop, rate):
    """Slope-limited creep, MASS-CONSERVING.

    The obvious formulation -- subtract the over-repose excess from each high
    cell -- is what this had first, and it does not move material, it DELETES
    it. Measured on a real tile that removed 128 m from cliff tops in 48
    iterations, because a 30 m drop across one 3.75 m post is ~10x the repose
    limit and nothing put the debris anywhere. Real thermal erosion moves the
    excess to the neighbours it fell from, which is also what builds the talus
    aprons and concave footslopes the whole pass exists for.

    Two passes so it stays parallel-safe: compute each cell's total excess, then
    have every cell GATHER its share from higher neighbours in proportion to
    that neighbour's own excess.

    STABILITY. The amount a cell sheds per step is scaled by its STEEPEST
    over-repose pair, not by the sum over all eight neighbours. Scaling by the
    sum is what a first attempt did, and with rate 0.35 a spike with eight
    20 m-lower neighbours shed 48 m in one step, overshot far below them, and
    the whole field diverged to ~1e23 in 48 iterations. Capping by the steepest
    pair means that pair's height difference can at most be driven TO repose,
    never through it, for any rate <= 0.5."""
    h, w = z.shape
    exc = np.zeros((h, w), np.float32)     # sum of over-repose drops (shares)
    mx = np.zeros((h, w), np.float32)      # steepest over-repose drop (magnitude)
    for y in prange(1, h - 1):
        for x in range(1, w - 1):
            zc = z[y, x]
            s = np.float32(0.0)
            m = np.float32(0.0)
            for dy in range(-1, 2):
                for dx in range(-1, 2):
                    if dy == 0 and dx == 0:
                        continue
                    d = zc - z[y + dy, x + dx]
                    if d > max_drop:
                        e = d - max_drop
                        s += e
                        if e > m:
                            m = e
            exc[y, x] = s
            mx[y, x] = m

    out = z.copy()
    for y in prange(1, h - 1):
        for x in range(1, w - 1):
            zc = z[y, x]
            gain = np.float32(0.0)
            for dy in range(-1, 2):
                for dx in range(-1, 2):
                    if dy == 0 and dx == 0:
                        continue
                    zn = z[y + dy, x + dx]
                    en = exc[y + dy, x + dx]
                    if en <= 0.0:
                        continue
                    d = zn - zc                      # that neighbour is higher
                    if d > max_drop:
                        # my share of what that neighbour sheds this step
                        gain += (d - max_drop) / en * (mx[y + dy, x + dx] * rate)
            out[y, x] = zc - mx[y, x] * rate + gain
    return out


# --------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tile")
    ap.add_argument("--iters", type=int, default=48, help="thermal iterations")
    ap.add_argument("--png", help="write a before/after hillshade here")
    ap.add_argument("--crop", type=int, default=1024)
    ap.add_argument("--cx", type=int, default=None)
    ap.add_argument("--cy", type=int, default=None)
    a = ap.parse_args()

    t = {}
    def tick(name, t0):
        t[name] = time.perf_counter() - t0
        print(f"  {name:<34} {t[name]:7.2f} s")

    print(f"tile {a.tile}   -> {FINE}x{FINE} @ {PIXEL_M} m/px")
    coarse = decode_vxtl(a.tile)

    t0 = time.perf_counter(); fine = bspline_upsample(coarse, SCALE); tick("B0 B-spline upsample", t0)

    gy, gx = np.gradient(fine, PIXEL_M)
    slope0 = np.hypot(gx, gy)
    t0 = time.perf_counter()
    fine = fine + conditioned_fbm(fine.shape, slope0) * 1.5
    tick("B1 conditioned fBm roughness", t0)

    t0 = time.perf_counter(); filled = priority_flood(fine); tick("B2a priority-flood fill", t0)
    t0 = time.perf_counter(); rec, slope = d8_receiver(filled, PIXEL_M); tick("B2b D8 receivers", t0)
    t0 = time.perf_counter(); order = np.argsort(filled.ravel())[::-1].astype(np.int64); tick("B2c elevation sort", t0)
    t0 = time.perf_counter()
    acc = accumulate_mfd(filled, order, np.float32(PIXEL_M * PIXEL_M),
                         np.float32(PIXEL_M), np.float32(1.1)).reshape(fine.shape)
    tick("B2d MFD flow accumulation", t0)

    t0 = time.perf_counter()
    # K calibrated so the largest channels on this tile carve metres, not
    # millimetres: at A = 10 km2 and a 10% slope, A^0.45 * S^0.8 ~ 220, so K of
    # order 1e-2 gives ~2 m. The first pass used 4e-4 and produced a p99 of
    # 0.01 m -- a drainage network that existed in the flow field and nowhere in
    # the terrain.
    K, m, n = 1.2e-2, 0.45, 0.8
    incision = K * np.power(acc, m, dtype=np.float32) * np.power(slope + 1e-6, n, dtype=np.float32)
    incision = np.minimum(incision, 8.0)                    # plan's sub-threshold cap
    eroded = filled - incision
    tick("B2e stream-power incision", t0)

    t0 = time.perf_counter()
    max_drop = np.float32(np.tan(np.radians(36.0)) * PIXEL_M)
    z = eroded
    for _ in range(a.iters):
        z = thermal_step(z, max_drop, np.float32(0.35))
    tick(f"B3 thermal relaxation x{a.iters}", t0)

    total = sum(t.values())
    print(f"  {'TOTAL':<34} {total:7.2f} s  (one tile, {FINE**2/1e6:.1f} Mcell)")

    resid = z - fine
    print(f"\n  drainage: max accumulation {acc.max()/1e6:.1f} km2, "
          f"cells with A > 1 km2: {(acc > 1e6).sum()}")
    print(f"  incision: mean {incision.mean():.2f} m, p99 {np.percentile(incision,99):.2f} m, "
          f"max {incision.max():.2f} m")
    print(f"  net change vs B1 surface: mean {resid.mean():+.2f} m, "
          f"sd {resid.std():.2f} m, min {resid.min():+.1f} m, max {resid.max():+.1f} m")

    np.save("bake_out.npy", z.astype(np.float32))
    print("  wrote bake_out.npy")

    if a.png:
        hillshade_png(a.png, fine, z, acc, crop=a.crop, x0=a.cx, y0=a.cy)
        print(f"  wrote {a.png}")


def _hillshade(z, cell_m, az=315.0, alt=45.0):
    gy, gx = np.gradient(z.astype(np.float64), cell_m)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gx, gy)
    az_r, alt_r = np.radians(az), np.radians(alt)
    sh = (np.sin(alt_r) * np.cos(slope) +
          np.cos(alt_r) * np.sin(slope) * np.cos(az_r - aspect))
    return np.clip(sh, 0, 1)


def hillshade_png(path, before, after, acc, crop=1024, x0=None, y0=None):
    """Side-by-side hillshade: the roughened carrier vs the baked surface.

    This is the check the spectra cannot make. The plan calls it the hillshade
    A/B harness, and it answers the only question that matters for B2: does the
    flow field show up as STRUCTURE in the ground, or only in the flow array?"""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    if x0 is None:
        # Pick the crop by DISSECTED UPLAND, not by peak accumulation. Peak
        # accumulation is the outlet, which sits at the tile edge in flat ground
        # or ocean -- the first auto-crop went straight there and produced two
        # featureless grey squares. Score instead on "how many cells here carry
        # real flow AND stand above sea level", which is where a drainage
        # network is actually legible.
        k = 128
        H = acc.shape[0] // k * k
        chan = ((acc[:H, :H] > 2e4) & (after[:H, :H] > 50.0)).astype(np.float32)
        score = chan.reshape(H // k, k, H // k, k).mean(axis=(1, 3))
        iy, ix = np.unravel_index(np.argmax(score), score.shape)
        y0, x0 = iy * k + k // 2 - crop // 2, ix * k + k // 2 - crop // 2
        print(f"  crop scored on dissected upland: block ({ix},{iy}) "
              f"channel fraction {score.max():.2f}")
    y0 = int(np.clip(y0, 0, before.shape[0] - crop))
    x0 = int(np.clip(x0, 0, before.shape[1] - crop))
    sl = (slice(y0, y0 + crop), slice(x0, x0 + crop))

    fig, axes = plt.subplots(1, 3, figsize=(21, 7.4))
    for ax, img, title in (
            (axes[0], before[sl], "B1: carrier + conditioned roughness"),
            (axes[1], after[sl], "B3: after incision + thermal relaxation")):
        ax.imshow(_hillshade(img, PIXEL_M), cmap="gray", vmin=0, vmax=1,
                  interpolation="nearest")
        ax.set_title(title, fontsize=11)
        ax.set_xticks([]); ax.set_yticks([])
    # The flow field itself, so "the network exists" and "the network got carved
    # into the ground" can be told apart.
    axes[2].imshow(np.log10(acc[sl] + 1.0), cmap="magma", interpolation="nearest")
    axes[2].set_title("MFD flow accumulation (log10 m²)", fontsize=11)
    axes[2].set_xticks([]); axes[2].set_yticks([])
    fig.suptitle(f"Phase 2 bake prototype - {crop}x{crop} px = "
                 f"{crop*PIXEL_M/1000:.1f} km at {PIXEL_M} m/px "
                 f"(tile offset {x0},{y0})", fontsize=12)
    fig.tight_layout()
    fig.savefig(path, dpi=110)


if __name__ == "__main__":
    main()
