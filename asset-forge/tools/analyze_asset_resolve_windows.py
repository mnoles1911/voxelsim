"""Read caller timing windows without mistaking their totals for frame costs."""
import argparse
from datetime import datetime
import json
import math
from pathlib import Path
import re

PREFIX = re.compile(r"\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\].*Voxel asset resolve caller \(window\): (.*)")
COUNTS = ("calls", "hits", "coldMisses", "forcedInline", "level0", "coarse")
TIMES = ("totalMs", "inlineMs", "rawResolveMs", "maxCallMs", "maxRawResolveMs")


def predictive_windows(log):
    pattern = re.compile(r"\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\].*Voxel predictive asset resolve \(window\): (.*)")
    rows = []
    launched = completed = 0
    for match in pattern.finditer(log):
        fields = dict(re.findall(r"(\w+)=([^\s]+)", match[2]))
        row = {k: float(v) if k.endswith("Ms") else int(v) for k, v in fields.items()}
        if any(not math.isfinite(v) or v < 0 for v in row.values()):
            raise ValueError("Invalid predictive metric")
        if row["pending"] > row["inFlightCap"] or row["queueRemaining"] > row["queueCap"]:
            raise ValueError("Predictive queue exceeds declared bound")
        launched += row["launched"]
        # Source reports residency-at-landing and changed-epoch rejection as
        # separate terminal outcomes, not a total plus subset.
        completed += row["landed"] + row["raced"] + row["rejected"] + row["epochRejected"]
        if launched != completed + row["pending"]:
            raise ValueError("Predictive launched/completed/pending accounting mismatch")
        for smaller, larger in (("queueBuildMs", "tickMs"), ("maxQueueBuildMs", "maxTickMs"), ("maxTickMs", "tickMs")):
            if row[smaller] > row[larger] + .002:
                raise ValueError("Invalid predictive nested timing")
        row["windowReportedUtc"] = datetime.strptime(match[1], "%Y.%m.%d-%H.%M.%S:%f").isoformat()
        rows.append(row)
    return rows


def analyze(log):
    rows = []
    for match in PREFIX.finditer(log):
        fields = dict(re.findall(r"(\w+)=([^\s]+)", match[2]))
        caller = fields.get("caller")
        if caller not in ("Admission", "GpuSubmit", "EditedPage"):
            raise ValueError("Unknown resolver caller")
        row = {"windowReportedUtc": datetime.strptime(match[1], "%Y.%m.%d-%H.%M.%S:%f").isoformat(), "caller": caller}
        row.update((key, int(fields[key])) for key in COUNTS)
        row.update((key, float(fields[key])) for key in TIMES)
        if any(not math.isfinite(row[k]) or row[k] < 0 for k in COUNTS + TIMES):
            raise ValueError("Negative/nonfinite resolver metric")
        if row["calls"] != row["hits"] + row["coldMisses"] + row["forcedInline"]:
            raise ValueError("Resolver hit/miss count mismatch")
        if row["calls"] != row["level0"] + row["coarse"]:
            raise ValueError("Resolver level count mismatch")
        # Independently rounded millisecond fields can differ by 0.001 ms.
        for smaller, larger in (("inlineMs", "totalMs"), ("rawResolveMs", "inlineMs"),
                                ("maxCallMs", "totalMs"), ("maxRawResolveMs", "rawResolveMs")):
            if row[smaller] > row[larger] + .002:
                raise ValueError(f"Invalid nested timing: {smaller} > {larger}")
        rows.append(row)
    totals = {}
    for caller in sorted({r["caller"] for r in rows}):
        selected = [r for r in rows if r["caller"] == caller]
        totals[caller] = {k: sum(r[k] for r in selected) for k in COUNTS + TIMES[:3]}
        totals[caller].update((k, max(r[k] for r in selected)) for k in TIMES[3:])
    return {"scope": "Reported windows only; includes startup and partial phase overlaps. Nested timings are not additive to frame/submission totals. Final unreported window is absent.",
            "windows": rows, "reportedTotalsByCaller": totals, "predictiveWindows": predictive_windows(log)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    result = analyze(args.log.read_text(errors="replace"))
    target = args.log.parent / "asset-resolve-windows.json"
    target.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"windowRows": len(result["windows"]), "reportedTotalsByCaller": result["reportedTotalsByCaller"]}, indent=2))
