"""Build every spec at a couple of seeds and print what the health checks say."""
import sys
import time
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def main():
    seeds = [int(a) for a in sys.argv[1:]] or [1, 2]
    worst = 0
    for fp in sorted(SPECS.glob("*.json")):
        s, _ = sm.load(fp)
        kind = sm.get(s, "kind")
        for seed in seeds:
            t = time.perf_counter()
            a = pipeline.build(s, seed)
            ms = (time.perf_counter() - t) * 1000
            st = a.stats
            found = pipeline.health(a)
            probs = "; ".join(found)
            worst = max(worst, len(found))
            print(f"{fp.stem:<20} {kind:<5} s{seed} "
                  f"{st['voxels']:>8} vox  {st['height_m']:>5.1f} m  {ms:>6.0f} ms"
                  + (f"  ! {probs}" if probs else ""))
    print("clean" if worst == 0 else "problems above")


if __name__ == "__main__":
    main()
