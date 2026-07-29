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


def bspline_upsample(a, scale, prefilter=True):
    """Cubic B-spline upsample.

    PREFILTER MATTERS AND THE PLAN SAYS SO. A B-spline APPROXIMATES its control
    points rather than interpolating them, so feeding it raw samples low-passes
    the source. Measured: without the prefilter the detrended H of the upsampled
    surface is 0.89 over 120-240 m (i.e. the real 30 m data) but degrades to 1.47
    by 30-60 m -- the carrier is smoother than the raster it came from, right at
    the band the bake is supposed to be extending.

    The standard fix is to solve for control points whose spline INTERPOLATES the
    samples, which is a recursive IIR pass (pole sqrt(3)-2). That is a float
    operation, which is exactly why the plan puts it in the bake and ships
    control points rather than samples: illegal in voxel-core, trivial here."""
    if prefilter:
        from scipy.ndimage import spline_filter
        a = spline_filter(a.astype(np.float64), order=3, mode="nearest").astype(np.float32)
    W = _weights(scale)
    b = _up_axis(a, scale, W)
    c = _up_axis(np.ascontiguousarray(b.T), scale, W)
    return np.ascontiguousarray(c.T)


# ----------------------------------------------------------------- B1 roughness
def measure_S2(z, cell_m, lags_cells):
    rows = z[::7]
    out = []
    for d in lags_cells:
        a, b, c = rows[:, 2 * d:], rows[:, d:-d], rows[:, :-2 * d]
        out.append((d * cell_m, float(np.mean(np.abs(a - 2 * b + c)))))
    return out


def spectrum_fitted_fbm(carrier, cell_m, slope, seed=20260719, src_nyquist_m=30.0):
    """B1 built to a TARGET SPECTRUM rather than a target RMS.

    The RMS formulation does not work and the probe says why: nearly all of an
    fBm's energy sits in its coarse octaves, so quadrupling the total moved
    S2(7.5 m) by only 1.5x while H stayed ~1.65 -- 'smoother than linear', the
    worldgen-v1 signature.

    So fit the CARRIER's own self-affine trend over the band where it still
    carries real 30 m data (120-240 m), extrapolate it down, and give each
    octave the amplitude that continues it:  A(L) = C * L^H.

    Only octaves BELOW the source Nyquist are synthesised. Above it the carrier
    already has the real data and adding noise there would fight the diffusion
    model rather than extend it -- the same 'replace, do not layer' rule the
    plan applies to the client's landform octaves."""
    from scipy.ndimage import zoom
    pts = measure_S2(carrier, cell_m, [32, 64])          # 120 m and 240 m
    (d0, s0), (d1, s1) = pts
    H = float(np.log(s1 / s0) / np.log(d1 / d0))
    C = float(s1 / d1 ** H)
    print(f"  B1 target spectrum fitted to the carrier: H={H:.3f} C={C:.4f}  "
          f"(S2({d1:.0f} m)={s1:.2f} m)")

    rng = np.random.default_rng(seed)
    out = np.zeros(carrier.shape, dtype=np.float32)
    n = carrier.shape[0]
    size = 16
    while size <= n:
        L = n * cell_m / size                            # octave wavelength, m
        size *= 2
        if L > src_nyquist_m:
            continue                                     # the carrier owns this band
        amp = C * L ** H
        # Grid points across the tile for an octave of wavelength L: one control
        # point per L, i.e. n*cell_m/L of them. (Inverting this puts LOW
        # frequency content in with a fine octave's amplitude, which measurably
        # makes the fine end smoother -- H went 1.65 -> 1.91 before it was fixed.)
        gsz = max(4, int(round(n * cell_m / L)))
        g = rng.standard_normal((gsz + 4, gsz + 4)).astype(np.float32)
        up = zoom(g, n / gsz, order=3, mode="nearest")
        band = up[:n, :n]
        band /= max(band.std(), 1e-6)
        out += amp * band
        print(f"    octave L={L:7.2f} m  amplitude {amp*1000:8.0f} mm")
    gain = np.clip(slope / 0.3, 0.25, 2.0).astype(np.float32)
    return out * gain


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


# ---------------------------------------------------------------------- the bake
def run_bake(coarse, iters=48, verbose=True, seed=20260719, noise=None,
             K=1.2e-2, cap=8.0, rough=1.5):
    """B0-B3 over whatever coarse array is handed in, including any apron.

    Takes the coarse array rather than a path so the seam test can bake a
    PADDED domain (centre tile + apron drawn from its neighbours) and crop the
    interior, which is the whole apron argument in the plan."""
    # Wall-clock AND process CPU time. Wall-clock is what a user waits, but it
    # is badly distorted by anything else on the machine -- these numbers were
    # first taken while another session held the box, which made them upper
    # bounds of unknown looseness. process_time() sums kernel+user CPU across
    # this process's threads, so a competing process steals wall-clock from it
    # but not CPU-seconds. For the parallel (numba prange) stages CPU-s exceeds
    # wall-s by roughly the thread count; that is the point, it is the WORK.
    t, tc = {}, {}

    def tick(name, t0, c0):
        t[name] = time.perf_counter() - t0
        tc[name] = time.process_time() - c0
        if verbose:
            print(f"  {name:<34} {t[name]:7.2f} s wall  {tc[name]:8.2f} s cpu")

    t0, c0 = time.perf_counter(), time.process_time(); fine = bspline_upsample(coarse, SCALE); tick("B0 B-spline upsample", t0, c0)

    gy, gx = np.gradient(fine, PIXEL_M)
    slope0 = np.hypot(gx, gy)
    t0, c0 = time.perf_counter(), time.process_time()
    # The fBm here is generated in ARRAY coordinates, so two overlapping domains
    # would NOT agree in their overlap. Production B1 must hash WORLD
    # coordinates (the plan says so, and it is what makes the apron argument
    # work at all). `noise` lets a caller pass a pre-computed field sliced from
    # a shared parent, which is an exact stand-in for world-anchoring and is how
    # the seam test isolates apron adequacy from this prototype's own limitation.
    if noise is not None:
        fine = fine + noise * np.clip(slope0 / 0.3, 0.25, 2.0).astype(np.float32) * rough
    elif rough < 0:                    # negative rough = "fit the spectrum instead"
        fine = fine + spectrum_fitted_fbm(fine, PIXEL_M, slope0, seed=seed)
    else:
        fine = fine + conditioned_fbm(fine.shape, slope0, seed=seed) * rough
    tick("B1 conditioned fBm roughness", t0, c0)

    t0, c0 = time.perf_counter(), time.process_time(); filled = priority_flood(fine); tick("B2a priority-flood fill", t0, c0)
    t0, c0 = time.perf_counter(), time.process_time(); rec, slope = d8_receiver(filled, PIXEL_M); tick("B2b D8 receivers", t0, c0)
    t0, c0 = time.perf_counter(), time.process_time(); order = np.argsort(filled.ravel())[::-1].astype(np.int64); tick("B2c elevation sort", t0, c0)
    t0, c0 = time.perf_counter(), time.process_time()
    acc = accumulate_mfd(filled, order, np.float32(PIXEL_M * PIXEL_M),
                         np.float32(PIXEL_M), np.float32(1.1)).reshape(fine.shape)
    tick("B2d MFD flow accumulation", t0, c0)

    t0, c0 = time.perf_counter(), time.process_time()
    # K calibrated so the largest channels carve metres, not millimetres: at
    # A = 10 km2 and a 10% slope, A^0.45 * S^0.8 ~ 220, so K of order 1e-2 gives
    # ~2 m. The first pass used 4e-4 and produced a p99 of 0.01 m -- a drainage
    # network that existed in the flow field and nowhere in the terrain.
    m, n = 0.45, 0.8
    incision = K * np.power(acc, m, dtype=np.float32) * np.power(slope + 1e-6, n, dtype=np.float32)
    incision = np.minimum(incision, cap)
    eroded = filled - incision
    tick("B2e stream-power incision", t0, c0)

    t0, c0 = time.perf_counter(), time.process_time()
    max_drop = np.float32(np.tan(np.radians(36.0)) * PIXEL_M)
    z = eroded
    for _ in range(iters):
        z = thermal_step(z, max_drop, np.float32(0.35))
    tick(f"B3 thermal relaxation x{iters}", t0, c0)

    if verbose:
        print(f"  {'TOTAL':<34} {sum(t.values()):7.2f} s wall  {sum(tc.values()):8.2f} s cpu  "
              f"({coarse.shape[0]*SCALE}^2 = {(coarse.shape[0]*SCALE)**2/1e6:.1f} Mcell)")
    return {"fine": fine, "z": z, "acc": acc, "incision": incision,
            "filled": filled, "eroded": eroded, "times": t}


def report_spectrum(z, cell_m, label="baked fine tier", brief=False):
    """Detrended roughness S2(d) of the baked surface, and the client octave
    amplitudes that CONTINUE it below the fine tier's Nyquist.

    This is the measurement docs/terrain-amplification-plan.md Phase 3 asks for:
    'amplitudes set by probe measurement, not taste', placed so S2(d) continues
    at the fine tier's own exponent through 7.5 m. Same S2 definition as
    voxel-core/bench/terrainprobe.cpp, so the two are directly comparable.
    """
    rows = z[::7]
    print(f"\n  === {label}: detrended roughness S2(d) ===")
    if not brief:
        print(f"  {'lag':>10} {'S2(d)':>12} {'local H':>9}")
    lags = [1, 2, 4, 8, 16, 32, 64]
    prev = None
    pts = []
    for d in lags:
        a = rows[:, 2 * d:]
        b = rows[:, d:-d]
        c = rows[:, :-2 * d]
        s2 = float(np.mean(np.abs(a - 2 * b + c)))
        dm = d * cell_m
        H = ""
        if prev is not None and s2 > 0:
            h = np.log(s2 / prev[1]) / np.log(dm / prev[0])
            H = f"{h:9.3f}"
        pts.append((dm, s2))
        prev = (dm, s2)
        if not brief:
            print(f"  {dm:8.1f} m {s2:11.4f} m {H}")

    # Fit H over the well-resolved band (4-64 cells) and extrapolate down.
    xs = np.log([p[0] for p in pts[2:]])
    ys = np.log([max(p[1], 1e-9) for p in pts[2:]])
    H = float(np.polyfit(xs, ys, 1)[0])
    s2_at = lambda dm: float(np.exp(np.interp(np.log(dm), xs, ys))) if dm >= pts[2][0] \
        else pts[2][1] * (dm / pts[2][0]) ** H
    ref_m = 2 * cell_m                       # fine-tier Nyquist-ish reference
    fine_H = np.log(pts[1][1] / pts[0][1]) / np.log(pts[1][0] / pts[0][0])
    print(f"  H over 15-240 m: {H:.3f}   H over {pts[0][0]:.1f}-{pts[1][0]:.1f} m: "
          f"{fine_H:.3f}   S2({ref_m:.2f} m) = {s2_at(ref_m):.4f} m")
    if brief:
        return
    if fine_H > 1.05:
        print(f"  !! H > 1 at the fine end means SMOOTHER THAN LINEAR -- the same")
        print(f"     signature amplifier.cpp:242 records for worldgen v1, i.e. the")
        print(f"     surface has run out of octaves. Real terrain is H 0.6-0.9.")
        print(f"     Extrapolating client amplitudes from this H would make the")
        print(f"     client SMOOTHER, which is backwards; fix the bake first.")
        return
    print("  client octave amplitudes that CONTINUE this trend "
          "(voxel-core kDetailOctaves, mm):")
    for lattice_mm in (3200, 1600, 400, 200):
        dm = lattice_mm / 1000.0
        amp = s2_at(ref_m) * (dm / ref_m) ** H
        print(f"    lattice {lattice_mm:>5} mm  ->  amplitude {amp*1000:7.0f} mm")
    print("  (v9 ships 1600/500, 400/190, 200/60 mm; the 3200 mm octave is new "
          "in Phase 3)")


# --------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tile")
    ap.add_argument("--iters", type=int, default=48, help="thermal iterations")
    ap.add_argument("--png", help="write a before/after hillshade here")
    ap.add_argument("--crop", type=int, default=1024)
    ap.add_argument("--cx", type=int, default=None)
    ap.add_argument("--cy", type=int, default=None)
    ap.add_argument("--K", type=float, default=1.2e-2, help="stream-power erodibility")
    ap.add_argument("--cap", type=float, default=8.0, help="max incision (m)")
    ap.add_argument("--rough", type=float, default=1.5, help="B1 roughness scale (m)")
    ap.add_argument("--npy", help="also dump the baked surface here (67 MB; off by default)")
    a = ap.parse_args()

    print(f"tile {a.tile}   -> {FINE}x{FINE} @ {PIXEL_M} m/px")
    coarse = decode_vxtl(a.tile)
    r = run_bake(coarse, iters=a.iters, K=a.K, cap=a.cap, rough=a.rough)
    fine, z, acc, incision = r["fine"], r["z"], r["acc"], r["incision"]

    # Per STAGE, because a single end-of-pipeline number cannot say which pass
    # is responsible for the fine end.
    for label, arr in (("B1 (carrier+roughness)", fine),
                       ("B2a filled", r["filled"]),
                       ("B2e after incision", r["eroded"]),
                       ("B3 after thermal", z)):
        report_spectrum(arr, PIXEL_M, label=label, brief=(label != "B3 after thermal"))

    resid = z - fine
    print(f"\n  drainage: max accumulation {acc.max()/1e6:.1f} km2, "
          f"cells with A > 1 km2: {(acc > 1e6).sum()}")
    print(f"  incision: mean {incision.mean():.2f} m, p99 {np.percentile(incision,99):.2f} m, "
          f"max {incision.max():.2f} m")
    print(f"  net change vs B1 surface: mean {resid.mean():+.2f} m, "
          f"sd {resid.std():.2f} m, min {resid.min():+.1f} m, max {resid.max():+.1f} m")

    # Only on request. This used to write bake_out.npy into the CWD
    # unconditionally, and a `git add -A` from the tools directory duly
    # committed 67 MB of float32 into the repo.
    if a.npy:
        np.save(a.npy, z.astype(np.float32))
        print(f"  wrote {a.npy}")

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
