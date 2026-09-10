"""Compare matched prewarming OFF/ON captures without asserting a frame win."""
import argparse
import json
from pathlib import Path
import re
from analyze_ecological_walk import analyze
from analyze_asset_resolve_windows import analyze as resolve_windows


def compare(baseline, current):
    manifests = [json.loads((p / "run-manifest.json").read_text(encoding="utf-8-sig")) for p in (baseline, current)]
    old, new = manifests
    flag = "-VoxelPredictiveAssetResolve"
    if flag in old["arguments"] or flag not in new["arguments"]:
        raise ValueError("Expected prewarming OFF baseline and ON current capture")
    def args(m):
        return sorted(a for a in m["arguments"] if a != flag and not a.startswith(("-UserDir=", "-abslog=")))
    if args(old) != args(new):
        raise ValueError("Capture arguments differ beyond prewarming and output paths")
    for field in ("configurationSha256", "speciesManifestSha256", "detailCacheManifestSha256"):
        if old[field].lower() != new[field].lower():
            raise ValueError(f"Changed input: {field}")
    old_modules = old.get("runtimeModuleHashes")
    binary_scope = "Both captures record start module hashes; wrappers also check end hashes."
    if old_modules is None:
        old_modules = json.loads((baseline / "post-run-module-hashes.json").read_text(encoding="utf-8-sig"))["hashes"]
        binary_scope = "Baseline post-run module hashes match current start hashes; baseline lacks retroactive start pins."
    if old_modules != new.get("runtimeModuleHashes"):
        raise ValueError("Runtime modules differ")
    reports = [analyze(p) for p in (baseline, current)]
    if any(r["passed_checks"] != 8 or r["failed_checks"] for r in reports):
        raise ValueError("Movement checks failed")
    if reports[0]["internal_resolutions"] != reports[1]["internal_resolutions"]:
        raise ValueError("Render resolution differs")
    logs = [(p / "game.log").read_text(errors="replace") for p in (baseline, current)]
    contexts = [re.search(r"ECOLOGY_MEASURE_BEGIN biome=(\d+) liveInstances=(\d+)", log).groups() for log in logs]
    if contexts[0] != contexts[1]:
        raise ValueError("Initial measured biome/instance count differs")
    identities = [re.search(r"VoxelMarchDispatchIdentity scheduled=1 type=FVoxelMarchCS permutation=(\d+) outputHash=(\w+) sourceHash=(\w+)", log) for log in logs]
    if any(i is None for i in identities) or identities[0].groups() != identities[1].groups():
        raise ValueError("Initial March dispatch shader identity differs or is missing")
    counters = [resolve_windows(log) for log in logs]
    if counters[0]["predictiveWindows"] or not counters[1]["predictiveWindows"]:
        raise ValueError("Expected predictive counters only in ON arm")
    predictive = counters[1]["predictiveWindows"]
    if not sum(w["launched"] for w in predictive):
        raise ValueError("Prewarming did not exercise any task launch")
    phases = {}
    for phase in ("WalkForward", "SprintGate"):
        before, after = [r["phases"][phase]["metrics_ms"]["FrameTime"] for r in reports]
        phases[phase] = {"baseline_frame_ms": before, "prewarm_frame_ms": after,
                         "change_percent": {k: 100 * (after[k] / before[k] - 1) for k in before}}
    return {"baseline": str(baseline), "current": str(current), "arguments_and_input_pins_matched": True,
            "binary_evidence": binary_scope, "initial_measured_instances": int(contexts[0][1]),
            "first_dispatch_identity": identities[0].groups(), "phases": phases,
            "predictive_reported_launches": sum(w["launched"] for w in predictive),
            "predictive_reported_max_tick_ms": max(w["maxTickMs"] for w in predictive),
            "predictive_reported_epoch_rejections": sum(w["epochRejected"] for w in predictive),
            "scope": "One matched editor pair, short scripted movement and48m detail ring. Predictor counters include startup; no overall, shipping, fullradius, teleport or teardown acceptance."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("current", type=Path)
    args = parser.parse_args()
    result = compare(args.baseline, args.current)
    (args.current / "predictive-comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
