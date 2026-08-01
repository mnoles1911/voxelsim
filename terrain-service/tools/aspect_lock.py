#!/usr/bin/env python3
"""Detect DIRECTION QUANTISATION in a baked fine tile -- specifically the 45-degree
signature of D8 flow routing.

WHY THIS EXISTS. The owner's description of the artifact ruled out the contour
explanation: the parallel lines run DOWNHILL at 45 degrees, sometimes one
diagonal and sometimes the other, evenly spaced and sharing direction. Contours
are iso-elevation by construction, so whatever draws these is not a level set.
45 degrees is the signature of an 8-neighbour lock, and flow.py:24-25 already
records the mechanism in its own words:

    "single steepest neighbour -- can only route along 8 compass directions, and
    at this resolution that shows up in a hillshade as dead-straight 45-degree
    channels tens of pixels long"

That was mitigated for the ACCUMULATION field (MFD, mfd_p=1.1) and deliberately
kept for channel centrelines. Two bake_ver-4 changes then promoted it:
``incision_mode = "profile"`` carves along the D8 receiver tree, and
``channel_init_area_m2`` fell 1e4 -> 156, which puts 68% of a steep tile above
the initiation threshold. So D8's 8-direction lock now shapes most of a
mountainside rather than a few trunk lines.

TWO INDEPENDENT STATISTICS, because either alone can mislead:

  ASPECT LOCK. Histogram the gradient direction over 360 bins and read its
  Fourier harmonics. Real landforms are strongly anisotropic at LOW harmonics --
  a hillside faces one way (h1), a valley two ways (h2) -- so a raw histogram
  peak proves nothing. An 8-direction lock is specifically **harmonic 8**
  (period 45 deg); a pure grid-axis bias is harmonic 4 (period 90 deg). The
  harmonics separate the artifact from the terrain.

  ORIENTATION LOCK. Bin the 2-D power spectrum by direction over [0, 180) and
  read the same harmonics. This measures the orientation of LINEAR STRUCTURE
  directly rather than inferring it from per-cell slope, and it is restricted to
  an annulus of wavelengths so landform power cannot swamp it.

A statistic is only meaningful against its own noise floor, which is reported.
For N independent samples the floor on |c_k| is ~1/sqrt(N); neighbouring cells
are correlated, so the true floor is higher and the printed one is optimistic --
treat a result within ~3x of it as nothing.

Usage:
  python tools/aspect_lock.py D:\\ue-cache\\bake-g35\\-5_2.vxtl
  python tools/aspect_lock.py <a.vxtl> <b.vxtl> ...      # ablation series
  python tools/aspect_lock.py --raw D:\\ue-cache\\band-g35-v19.bin --cell-m 0.1
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

FINE_CELL_M = 1.875


# ---------------------------------------------------------------------------
# Loading.
# ---------------------------------------------------------------------------


def load_vxtl(path: Path) -> tuple[np.ndarray, float]:
    """Decoded elevation in metres from a .vxtl v2, plus its cell size.

    Returns the CONTROL POINTS scaled to metres, not resampled surface values.
    The B-spline prefilter is separable and direction-symmetric, so it can
    neither create nor destroy the 8-fold anisotropy this tool looks for; using
    the control lattice keeps the test on exactly the bytes that ship.
    """
    from terrain_service import tile_codec

    data = path.read_bytes()
    try:
        tile = tile_codec.decode_v2(data)
    except Exception:
        import zstandard as zstd

        dctx = zstd.ZstdDecompressor()
        tile = tile_codec.decode_v2(
            data, decompressor=lambda b, n: dctx.decompress(b, max_output_size=n)
        )
    mm = tile_codec.control_points_to_mm(tile.elevation_cp, tile.base_offset_mm, tile.quant)
    return np.asarray(mm, dtype=np.float64) / 1000.0, FINE_CELL_M


def load_raw(path: Path, n: int) -> np.ndarray:
    """The client's VXC_PROBE_DUMP raster: n*n int32 mm, row-major."""
    a = np.fromfile(path, dtype="<i4", count=n * n)
    return a.reshape(n, n).astype(np.float64) / 1000.0


def centre_window(z: np.ndarray, n: int) -> np.ndarray:
    if z.shape[0] <= n:
        return z
    o = (z.shape[0] - n) // 2
    return z[o : o + n, o : o + n]


# ---------------------------------------------------------------------------
# Statistic 1: aspect lock.
# ---------------------------------------------------------------------------


def harmonics(counts: np.ndarray, orders: tuple[int, ...]) -> dict[int, float]:
    """Normalised Fourier amplitude of a circular histogram at each order.

    counts sum to 1 first, so the result is the amplitude of the k-th angular
    harmonic as a fraction of the mean -- comparable across sample sizes and
    across tiles.
    """
    p = counts / max(counts.sum(), 1e-30)
    nb = len(p)
    ang = 2.0 * np.pi * np.arange(nb) / nb
    out = {}
    for k in orders:
        c = (p * np.exp(1j * k * ang)).sum()
        out[k] = 2.0 * abs(c)  # x2: a cos harmonic splits over +/-k
    return out


def aspect_lock(z: np.ndarray, cell_m: float, min_grade: float = 0.05) -> dict:
    """Harmonic content of the gradient-direction distribution.

    Weighted by gradient magnitude: a lock that only bites on steep ground is
    still the artifact, and unweighted counts let vast gentle areas outvote it.
    """
    gy, gx = np.gradient(z, cell_m)
    mag = np.hypot(gx, gy)
    ok = mag > min_grade
    if ok.sum() < 1000:
        return {"n": int(ok.sum())}
    th = np.arctan2(gy[ok], gx[ok])
    w = mag[ok]
    nb = 360
    idx = ((th + np.pi) / (2 * np.pi) * nb).astype(int) % nb
    counts = np.bincount(idx, weights=w, minlength=nb).astype(np.float64)
    h = harmonics(counts, (1, 2, 4, 8, 16))
    return {"n": int(ok.sum()), "h": h, "floor": 2.0 / np.sqrt(ok.sum())}


# ---------------------------------------------------------------------------
# Statistic 2: orientation lock (2-D spectrum).
# ---------------------------------------------------------------------------


def orientation_lock(z: np.ndarray, cell_m: float, lam_lo_m: float, lam_hi_m: float) -> dict:
    """Angular distribution of spectral power in a wavelength annulus.

    Restricted to an annulus so the landform ramp -- which lives at the largest
    wavelengths and is enormously more powerful than anything else -- cannot set
    the answer. Orientation is mod 180 deg because a power spectrum is
    centrosymmetric, so a 45-deg family repeats 4 times over the domain and
    reads as harmonic 4 here.
    """
    n = z.shape[0]
    win = np.outer(np.hanning(n), np.hanning(n))
    f = np.fft.fftshift(np.abs(np.fft.fft2((z - z.mean()) * win)) ** 2)
    fy = np.fft.fftshift(np.fft.fftfreq(n, d=cell_m))
    ky, kx = np.meshgrid(fy, fy, indexing="ij")
    kr = np.hypot(kx, ky)
    with np.errstate(divide="ignore"):
        lam = np.where(kr > 0, 1.0 / np.maximum(kr, 1e-12), np.inf)
    band = (lam >= lam_lo_m) & (lam <= lam_hi_m)
    if band.sum() < 1000:
        return {"n": int(band.sum())}
    # Direction of the WAVE VECTOR; a stripe runs perpendicular to it.
    phi = np.degrees(np.arctan2(ky[band], kx[band])) % 180.0
    nb = 180
    idx = phi.astype(int) % nb
    counts = np.bincount(idx, weights=f[band], minlength=nb).astype(np.float64)
    # Over a 180-deg domain: harmonic 1 = one dominant direction (period 180),
    # 2 = axis bias (period 90), 4 = the 45-deg family (period 45).
    h = harmonics(counts, (1, 2, 4, 8))
    peak = int(np.argmax(counts))
    return {
        "n": int(band.sum()),
        "h": h,
        "floor": 2.0 / np.sqrt(band.sum()),
        "peak_wavevector_deg": peak,
        "peak_stripe_deg": (peak + 90) % 180,
    }


# ---------------------------------------------------------------------------


def report(label: str, z: np.ndarray, cell_m: float, lam_lo: float, lam_hi: float) -> None:
    print(f"\n=== {label} ===")
    print(
        f"  {z.shape[0]}x{z.shape[0]} at {cell_m:.4g} m/px "
        f"({z.shape[0] * cell_m / 1000:.2f} km), relief {z.max() - z.min():.1f} m"
    )
    a = aspect_lock(z, cell_m)
    if "h" not in a:
        print(f"  aspect: too few sloped cells ({a['n']})")
    else:
        h = a["h"]
        print(f"  ASPECT LOCK   (n={a['n']}, noise floor {a['floor']:.4f})")
        print(
            f"    h1 {h[1]:.4f}  h2 {h[2]:.4f}  h4 {h[4]:.4f}   "
            f"h8 {h[8]:.4f} <== D8   h16 {h[16]:.4f}"
        )
        print(f"    h8/floor = {h[8] / a['floor']:6.1f}x     h4/floor = {h[4] / a['floor']:6.1f}x")
    o = orientation_lock(z, cell_m, lam_lo, lam_hi)
    if "h" not in o:
        print(f"  orientation: annulus too small ({o['n']})")
    else:
        h = o["h"]
        print(f"  ORIENTATION LOCK  {lam_lo:.1f}-{lam_hi:.1f} m band "
              f"(n={o['n']}, noise floor {o['floor']:.4f})")
        print(
            f"    h1 {h[1]:.4f}  h2 {h[2]:.4f}   h4 {h[4]:.4f} <== 45-deg family   h8 {h[8]:.4f}"
        )
        print(f"    h4/floor = {h[4] / o['floor']:6.1f}x")
        print(f"    dominant stripe orientation {o['peak_stripe_deg']} deg")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--raw", action="store_true", help="inputs are VXC_PROBE_DUMP int32 mm rasters")
    ap.add_argument("--cell-m", type=float, default=None)
    ap.add_argument("--n", type=int, default=2048, help="centre window, px")
    ap.add_argument("--lam-lo", type=float, default=None)
    ap.add_argument("--lam-hi", type=float, default=None)
    a = ap.parse_args()

    for p in a.paths:
        path = Path(p)
        if a.raw:
            side = int(round((path.stat().st_size / 4) ** 0.5))
            z = load_raw(path, side)
            cell = a.cell_m if a.cell_m else 0.1
        else:
            z, cell = load_vxtl(path)
            if a.cell_m:
                cell = a.cell_m
        z = centre_window(z, a.n)
        lo = a.lam_lo if a.lam_lo else 4.0 * cell
        hi = a.lam_hi if a.lam_hi else 32.0 * cell
        report(path.parent.name + "/" + path.name, z, cell, lo, hi)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
