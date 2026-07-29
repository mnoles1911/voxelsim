"""Assemble ridge_out/metrics/*.json into one comparison table per tile."""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MET = HERE / "ridge_out" / "metrics"

# Committed reference numbers (docs/terrain-validation-2026-07.md).
REF = {
    "plains": {
        "g10_ridge_peak": "0.0434 IL / 0.0484 LL",
        "g10_valley_pit": "0.038 IL / 0.034 LL",
        "ladder": [2.134, 1.943, 1.728, 1.523, 1.301],  # Illinois 3DEP
        "theta": 0.089, "hack_h": 0.546,
    },
    "alpine": {
        "g10_ridge_peak": "0.0332 Teton",
        "g10_valley_pit": "0.023 Teton",
        "ladder": None,
        "theta": 0.177, "hack_h": 0.891,
    },
    "rolling": {
        "g10_ridge_peak": "(0.188 Iowa @300m only)",
        "g10_valley_pit": "0.050 Iowa @300m",
        "ladder": None,
        "theta": 0.318, "hack_h": 0.508,
    },
}

ROWS = [
    ("g10_ridge_peak", "{:.4f}"),
    ("g10_valley_pit", "{:.4f}"),
    ("g10_slope", "{:.3f}"),
    ("g10_hollow_footslope", "{:.3f}"),
    ("g10_spur", "{:.3f}"),
    ("mean_deg_1.875m", "{:.3f}"),
    ("mean_deg_3.75m", "{:.3f}"),
    ("mean_deg_7.5m", "{:.3f}"),
    ("mean_deg_15m", "{:.3f}"),
    ("mean_deg_30m", "{:.3f}"),
    ("frac_flat", "{:.3f}"),
    ("frac_ridge_peak", "{:.3f}"),
    ("frac_valley_pit", "{:.3f}"),
    ("frac_flat_coarse_thresh", "{:.3f}"),
    ("theta", "{:.3f}"),
    ("slope_area_r2", "{:.2f}"),
    ("hack_h", "{:.3f}"),
    ("hack_r2", "{:.2f}"),
    ("curvature_asymmetry", "{:.4f}"),
    ("tail_asymmetry", "{:.3f}"),
]
B30 = [("b30_theta", "{:.3f}"), ("b30_hack_h", "{:.3f}"), ("b30_mean_deg", "{:.2f}"),
       ("b30_frac_ridge_peak", "{:.3f}"), ("b30_frac_valley_pit", "{:.3f}"),
       ("b30_frac_flat", "{:.3f}")]


def stage_of(rec):
    return rec.get("S1") or rec.get("S1a")


def main():
    tiles = sys.argv[1:] or ["plains", "alpine", "rolling"]
    for tile in tiles:
        recs = {}
        for p in sorted(MET.glob(f"*_{tile}.json")):
            rec = json.loads(p.read_text())
            recs[rec["config"] + ("" if rec["mode"] == "bake" else "*")] = rec
        if not recs:
            continue
        names = list(recs)
        print(f"\n=== {tile}  (* = S1a only, no erosion)  "
              f"REF g10 r+p {REF[tile]['g10_ridge_peak']}, "
              f"v+p {REF[tile]['g10_valley_pit']} ===")
        hdr = f"{'metric':26s}" + "".join(f"{n:>18s}" for n in names)
        print(hdr)
        for key, fmt in ROWS + B30:
            vals = []
            for n in names:
                m = stage_of(recs[n])
                v = m.get(key) if key not in ("b30_theta", "b30_hack_h", "b30_mean_deg",
                                              "b30_frac_ridge_peak", "b30_frac_valley_pit",
                                              "b30_frac_flat") \
                    else recs[n].get("S1_30m", {}).get(key)
                vals.append(fmt.format(v) if isinstance(v, (int, float)) else "-")
            print(f"{key:26s}" + "".join(f"{v:>18s}" for v in vals))
        lad = REF[tile]["ladder"]
        if lad:
            print(f"{'REF ladder':26s}" + f"  {lad}")
            for n in names:
                m = stage_of(recs[n])
                ks = ["mean_deg_1.875m", "mean_deg_3.75m", "mean_deg_7.5m",
                      "mean_deg_15m", "mean_deg_30m"]
                if all(k in m for k in ks):
                    dev = max(abs(m[k] / r - 1) for k, r in zip(ks, lad))
                    print(f"  {n}: worst ladder dev {dev * 100:.1f}%")


if __name__ == "__main__":
    main()
