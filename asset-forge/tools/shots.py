"""Screenshot the app at several widths via headless Chrome.

Checks the responsive layout by looking at it, rather than by trusting that the
media queries say what they mean.

Phone widths go through `/static/_probe.html`, which puts the app in a
fixed-width iframe. Chrome on Windows will not open a window narrower than about
500 px, so `--window-size=390` renders a wider viewport and the resulting
picture is not a phone at all -- which is exactly the false alarm that sent me
chasing an overflow bug that did not exist. The probe also prints any element
that runs past the edge, so the check is a measurement and not an impression.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
OUT = Path(__file__).resolve().parents[1] / "out" / "shots"
BASE = "http://127.0.0.1:8747"

# name, url, window w, window h
SHOTS = [
    ("phone-gallery", "/static/_probe.html?w=390&h=820", 460, 940),
    ("phone-small", "/static/_probe.html?w=360&h=780", 430, 900),
    ("phone-panel", "/static/_probe.html?w=390&h=820&open=panel", 460, 940),
    ("phone-rocks", "/static/_probe.html?w=390&h=820&route=%23kind%3Drock", 460, 940),
    ("phone-biomes", "/static/_probe.html?w=390&h=820&route=%23tab%3Dbiomes", 460, 940),
    ("tablet", "/", 820, 1180),
    ("desktop", "/", 1500, 950),
]


def shot(name, url, w, h):
    OUT.mkdir(parents=True, exist_ok=True)
    target = OUT / f"{name}.png"
    subprocess.run(
        [CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
         "--no-first-run", "--no-default-browser-check",
         f"--window-size={w},{h}", "--virtual-time-budget=14000",
         f"--screenshot={target}", f"--user-data-dir={tempfile.mkdtemp()}",
         BASE + url],
        capture_output=True, timeout=120)
    print(f"{name:<16} {target.stat().st_size:>8} bytes")


if __name__ == "__main__":
    wanted = sys.argv[1:]
    for s in SHOTS:
        if not wanted or s[0] in wanted:
            shot(*s)
