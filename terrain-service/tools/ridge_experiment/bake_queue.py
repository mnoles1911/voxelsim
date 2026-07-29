"""Run a queue of full bakes sequentially (NEVER two at once: 5.5 GiB peak each).

Usage: python bake_queue.py QUEUE.json
QUEUE.json: [{"config": ..., "tile": ..., "set": {...}, "noise": {...}}, ...]
Skips entries whose metrics file already exists.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

PY = sys.executable
HERE = Path(__file__).resolve().parent

queue = json.loads(Path(sys.argv[1]).read_text())
for job in queue:
    out = HERE / "ridge_out" / "metrics" / f"{job['config']}_{job['tile']}.json"
    if out.exists():
        print(f"skip {job['config']}/{job['tile']} (exists)")
        continue
    args = [PY, str(HERE / "ridge_harness.py"), "bake",
            "--config", job["config"], "--tile", job["tile"]]
    for k, v in job.get("set", {}).items():
        args += ["--set", f"{k}={v}"]
    if job.get("noise"):
        args += ["--noise", json.dumps(job["noise"])]
    t0 = time.time()
    r = subprocess.run(args)
    print(f"== {job['config']}/{job['tile']} rc={r.returncode} "
          f"{time.time()-t0:.0f}s ==", flush=True)
    if r.returncode != 0:
        sys.exit(f"job failed: {job}")
