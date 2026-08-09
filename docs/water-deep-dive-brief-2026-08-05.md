# SUPERSEDED — see `water-system-architecture.md`

> **STATUS 2026-08-09: [CURRENT].** This redirect is still accurate. Note that
> its target, `water-system-architecture.md`, is now itself
> [PARTIALLY CURRENT] under the re-architecture — see
> `docs/water-architecture.md` for the current single entry point.

This was the input brief for a deep-dive session: what the water system was,
what was settled, what had been falsified, and where the cracks were. The
deep dive happened on 2026-08-05 and its results are folded into
`docs/water-system-architecture.md`, which is now the single durable water
document.

**Where each part went:**

| was here | now |
|---|---|
| §1 what the system is | §1–§6, in more detail |
| §2 Settled — do not re-derive | §12a, verbatim in substance |
| §3 Falsified — do not re-run | §13, plus four this brief did not have |
| §4 the open cracks | §11a — and **two of them were measured to different conclusions**, see below |
| §5 where the code is | §9, the file map |
| §6 test on wet country | §10 |
| §7 traps that cost hours | §14 |
| §8 open branches | §11b, updated |
| §9 what to come back with | answered; this file is the answer |

**Two of this brief's own conclusions did not survive measurement, and that is
the main reason it is superseded rather than merely merged:**

* **§4.1 "the painted river does not obey its own law"** was right that drawn
  width does not track discharge, and right to rank it first. But it framed the
  defect as rivers being *too narrow*, from a centreline measurement at km 20.
  Measured across the whole wet set on three tiles, the median drawn river is
  **10–17× wider** than the law. Both are true: the drawn width is roughly 20 m
  everywhere, too wide at headwaters and too narrow at trunks. The defect is the
  missing dependence on discharge, not the sign of the error.

* **§4.6 "burial away from the centreline"** attributed the widened edge's 56.7%
  burial to the widening policy drawing water where there is no bed. The bed is
  in fact there — the terrain offers p50 24–37 m of submerged room against a law
  asking 1.5 m, and the extent rule already uses 75–82% of it.

The brief also did not contain what turned out to be the binding defect:
`HYDROLOGY_RESIDUALS` #7, which delivered the pyramid's discharge onto a tile's
apron instead of into the tile, at a cost of **276×** on the measured tile. That
is §11a, and it is fixed.

Nothing is preserved here. Use git history for the original text.
