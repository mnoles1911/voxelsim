#!/usr/bin/env python3
"""Pick the PyTorch wheel index that matches the pod's actual NVIDIA driver.

Why this exists: the Vast.ai PyTorch image we rented shipped
``torch 2.13.0+cu130`` on a host whose driver only supported CUDA 12.8. The
result is NOT a crash -- ``torch.cuda.is_available()`` just returns ``False``
and inference silently runs on CPU, which at 4090 rental prices means hours
per tile and a bill for nothing. So the wheel must be chosen from the DRIVER,
not assumed from the image.

``nvidia-smi`` reports the highest CUDA runtime the installed driver can run:

    | NVIDIA-SMI 570.86.15  Driver Version: 570.86.15  CUDA Version: 12.8   |

A cuXYZ wheel runs on any driver whose reported CUDA version is >= XYZ (CUDA
minor-version compatibility), so the rule is "highest published index that
does not exceed the driver". We emit an ordered LADDER rather than a single
answer: the top choice is tried first, and the caller falls back down the
ladder if ``torch.cuda.is_available()`` still comes back False. That makes the
bootstrap self-healing on driver/wheel combinations nobody has tested yet,
which is the normal case on a fresh rental.

Usage:
    python tools/cuda_index.py                 # ladder from live nvidia-smi
    python tools/cuda_index.py --driver 12.8   # ladder for a stated version

Prints one wheel index URL per line, best first. Exits 3 if no driver was
found (i.e. this is not a GPU box).
"""

import argparse
import re
import shutil
import subprocess
import sys

# Wheel indices published by download.pytorch.org, oldest first. Kept
# deliberately short: these are the ones with current stable torch builds.
KNOWN_INDICES: tuple[tuple[tuple[int, int], str], ...] = (
    ((11, 8), "cu118"),
    ((12, 1), "cu121"),
    ((12, 4), "cu124"),
    ((12, 6), "cu126"),
    ((12, 8), "cu128"),
)

INDEX_URL = "https://download.pytorch.org/whl/{tag}"

# Verified working on 2026-07-19 against a 12.8 driver on Vast.ai. When the
# driver allows it, this is tried FIRST even if a newer index would also fit:
# a known-good combination beats a theoretically-better untested one, and the
# ladder falls through to the newer ones anyway if it does not work out.
KNOWN_GOOD = "cu124"


def parse_driver_cuda(nvidia_smi_output: str) -> tuple[int, int] | None:
    """Extract the ``CUDA Version: X.Y`` the driver advertises.

    Returns ``(major, minor)`` or ``None`` if the banner is absent (which
    happens when nvidia-smi errors out, or on a CPU-only box).
    """
    m = re.search(r"CUDA Version:\s*(\d+)\.(\d+)", nvidia_smi_output)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2))


def ladder(driver: tuple[int, int]) -> list[str]:
    """Ordered list of wheel index URLs to try, best first.

    Every returned index is <= the driver version, so all of them are
    *plausible*; the ordering encodes preference, not validity.
    """
    fitting = [tag for ver, tag in KNOWN_INDICES if ver <= driver]
    if not fitting:
        # Driver older than every index we know about. Offer the oldest
        # anyway -- failing with a concrete pip error beats failing with
        # "no candidates", and cu118 covers a very long tail of drivers.
        fitting = [KNOWN_INDICES[0][1]]

    # Preference: known-good first, then newest-to-oldest of the remainder.
    ordered: list[str] = []
    if KNOWN_GOOD in fitting:
        ordered.append(KNOWN_GOOD)
    ordered += [t for t in reversed(fitting) if t not in ordered]
    return [INDEX_URL.format(tag=t) for t in ordered]


def detect() -> tuple[int, int] | None:
    if shutil.which("nvidia-smi") is None:
        return None
    try:
        out = subprocess.run(
            ["nvidia-smi"], capture_output=True, text=True, timeout=60
        ).stdout
    except Exception:  # noqa: BLE001 - any failure means "no usable driver"
        return None
    return parse_driver_cuda(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", help="override, e.g. 12.8 (skips nvidia-smi)")
    args = ap.parse_args()

    if args.driver:
        try:
            major, minor = (int(v) for v in args.driver.split(".")[:2])
        except ValueError:
            print("ERROR: --driver must look like 12.8", file=sys.stderr)
            return 2
        driver = (major, minor)
    else:
        driver = detect()

    if driver is None:
        print("ERROR: no NVIDIA driver detected (nvidia-smi missing or failed).",
              file=sys.stderr)
        print("This script only makes sense on a GPU host.", file=sys.stderr)
        return 3

    print(f"# driver CUDA {driver[0]}.{driver[1]}", file=sys.stderr)
    for url in ladder(driver):
        print(url)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
