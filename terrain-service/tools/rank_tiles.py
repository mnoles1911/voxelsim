"""Rank cached `.vxtl` tiles by how much they can tell us about a bake.

Calibrating stream_K needs terrain with real dissected relief. An ocean tile or a
coastal flat cannot falsify a K value -- there is nothing for the incision term to
act on -- so picking the tile matters as much as picking the metric.

Ranks on relief and on the fraction of the tile that is land AND sloping, and
prints the extremes so a bad pick is obvious rather than silent.

  rank_tiles.py <dir-of-vxtl> [--top 10]
"""
import argparse, glob, os, struct
import numpy as np

HDR = "<4sHQiiBH"


def decode(path):
    b = open(path, "rb").read()
    magic, ver, _seed, tx, ty, _sc, size = struct.unpack_from(HDR, b, 0)
    if magic != b"VXTL":
        return None
    off = struct.calcsize(HDR)
    need = size * size * 2
    if len(b) - off < need:
        return None  # truncated; validate, never trust
    e = np.frombuffer(b, dtype="<i2", count=size * size, offset=off).reshape(size, size)
    return ver, tx, ty, e.astype(np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--top", type=int, default=10)
    a = ap.parse_args()

    rows = []
    for p in sorted(glob.glob(os.path.join(a.dir, "**", "*.vxtl"), recursive=True)):
        d = decode(p)
        if d is None:
            print(f"  SKIP unreadable/truncated: {p}")
            continue
        ver, tx, ty, e = d
        land = e > 0
        # Grade in percent at the 30 m pixel: the quantity the client's slope
        # gating is expressed in, so it is the one that decides whether a tile
        # exercises the interesting branches.
        gy, gx = np.gradient(e.astype(np.float64), 30.0)
        grade = np.hypot(gx, gy) * 100.0
        lf = float(land.mean())
        steep = float((land & (grade > 10.0)).mean())
        rows.append((steep, lf, float(e.min()), float(e.max()),
                     float(np.percentile(grade[land], 95)) if land.any() else 0.0,
                     tx, ty, os.path.basename(p)))

    if not rows:
        print("no readable tiles")
        return
    rows.sort(reverse=True)
    print(f"{len(rows)} tiles\n")
    print(f"  {'steep%':>7} {'land%':>6} {'minZ':>7} {'maxZ':>7} {'p95 grade':>10}  tile")
    for r in rows[:a.top]:
        print(f"  {r[0]*100:6.1f}% {r[1]*100:5.1f}% {r[2]:7.0f} {r[3]:7.0f} "
              f"{r[4]:9.1f}%  {r[7]} ({r[5]},{r[6]})")
    print("\n  worst 3, for contrast:")
    for r in rows[-3:]:
        print(f"  {r[0]*100:6.1f}% {r[1]*100:5.1f}% {r[2]:7.0f} {r[3]:7.0f} "
              f"{r[4]:9.1f}%  {r[7]} ({r[5]},{r[6]})")


if __name__ == "__main__":
    main()
