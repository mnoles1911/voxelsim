#!/usr/bin/env python3
"""Every -Voxel* switch must be classified against the front-end policy.

WHY THIS EXISTS
---------------
VoxelFrontEndPolicy decides, per run, whether the main menu appears. Its rule 5
suppresses the menu whenever a "self-driving" switch is present -- one whose run
spawns, poses, waits and captures with no human in the room. Getting that wrong
in the safe direction costs a developer one extra flag. Getting it wrong in the
other direction means a capture run stops at a menu nobody will ever click, and
sits there until -VoxelMenuWatchdog kills it several minutes later.

The classification is a naming-convention RULE rather than a list of ~190
names, because a list is wrong the first week somebody adds -VoxelFooTest and
forgets to update it. But the rule cannot catch a switch that follows no
convention, and that failure is silent -- which is exactly the shape of defect
this repository's other two lints exist for (see lint-unity-collisions.py's
header for the same argument about a defect no CI job could see).

So: this lint reads the RULE out of VoxelFrontEndPolicy.cpp, walks every
FParse::Param/Value call in both Unreal modules, and fails on any switch the
rule does not match that is also absent from
tools/frontend-switch-classification.txt. Adding a switch that follows no
convention therefore forces a one-line decision instead of a five-minute hang
three weeks later.

It runs on a stock GitHub runner in about a second, which matters because CI
cannot compile the Unreal module at all (30 GB engine, 14 GB runner disk --
see .github/workflows/ue-build.yml).

USAGE
-----
    python tools/lint-frontend-switch-coverage.py
Exit status 0 = clean, 1 = findings.
"""

from __future__ import annotations

import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POLICY_CPP = os.path.join("ue-project", "Source", "VoxelEarth", "VoxelFrontEndPolicy.cpp")
CLASSIFICATION = os.path.join("tools", "frontend-switch-classification.txt")
SOURCE_ROOTS = [
    os.path.join("ue-project", "Source", "VoxelEarth"),
    os.path.join("ue-project", "Source", "VoxelEarthShaders"),
    os.path.join("ue-project", "Source", "VoxelEarthUI"),
]

# The two tables inside VoxelFrontEndPolicy.cpp. Parsed rather than duplicated:
# a copy here would be one more thing to keep in sync, and the whole point of
# the lint is that things do not stay in sync on their own.
SUBSTRINGS_RE = re.compile(
    r"kSelfDrivingSubstrings\[\]\s*=\s*\{(.*?)\};", re.DOTALL)
EXTRAS_RE = re.compile(r"kSelfDrivingExtras\[\]\s*=\s*\{(.*?)\};", re.DOTALL)
TEXT_LITERAL_RE = re.compile(r'TEXT\("([^"]+)"\)')
EXTRA_ENTRY_RE = re.compile(r'\{\s*TEXT\("([^"]+)"\)\s*,\s*TEXT\("([^"]*)"\)\s*\}')

# Matches both forms every switch in this codebase is read through.
#
# THE FIRST ARGUMENT IS NOT ALWAYS THE LITERAL FCommandLine::Get(). An earlier
# version of this pattern required it and therefore silently skipped every
# switch in VoxelFrontEndSwitches.cpp, which caches the command line in a local
# first -- a lint that reports "clean" because it looked at nothing is worse
# than no lint, so this accepts any single-token first argument.
SWITCH_RE = re.compile(
    r'FParse::(?:Param|Value)\(\s*[A-Za-z_][A-Za-z0-9_:]*(?:\(\))?\s*,\s*TEXT\("(Voxel[A-Za-z0-9]*)=?"\)')

MIN_REASON = 10


def read(path: str) -> str:
    with open(os.path.join(REPO_ROOT, path), encoding="utf-8", errors="ignore") as handle:
        return handle.read()


def parse_policy() -> tuple[list[str], dict[str, str], list[str]]:
    """Returns (substrings, {extra_name: reason}, errors)."""
    errors: list[str] = []
    source = read(POLICY_CPP)

    substrings_match = SUBSTRINGS_RE.search(source)
    if not substrings_match:
        errors.append(f"{POLICY_CPP}: could not find kSelfDrivingSubstrings[]")
        substrings = []
    else:
        substrings = TEXT_LITERAL_RE.findall(substrings_match.group(1))

    extras: dict[str, str] = {}
    extras_match = EXTRAS_RE.search(source)
    if not extras_match:
        errors.append(f"{POLICY_CPP}: could not find kSelfDrivingExtras[]")
    else:
        for name, reason in EXTRA_ENTRY_RE.findall(extras_match.group(1)):
            # An unexplained entry is how the table starts rotting: the next
            # person cannot tell whether it is still true.
            if len(reason.strip()) < MIN_REASON:
                errors.append(
                    f"{POLICY_CPP}: kSelfDrivingExtras entry '{name}' has no meaningful reason")
            extras[name] = reason
    return substrings, extras, errors


def parse_classification() -> set[str]:
    names: set[str] = set()
    for line in read(CLASSIFICATION).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        names.add(line)
    return names


def find_switches() -> dict[str, list[str]]:
    """switch name -> sorted list of 'path:line' sites."""
    sites: dict[str, list[str]] = {}
    for root in SOURCE_ROOTS:
        abs_root = os.path.join(REPO_ROOT, root)
        if not os.path.isdir(abs_root):
            continue
        for dirpath, _dirnames, filenames in os.walk(abs_root):
            for filename in filenames:
                if not filename.endswith((".cpp", ".h")):
                    continue
                path = os.path.join(dirpath, filename)
                rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
                with open(path, encoding="utf-8", errors="ignore") as handle:
                    for number, line in enumerate(handle, start=1):
                        for name in SWITCH_RE.findall(line):
                            sites.setdefault(name, []).append(f"{rel}:{number}")
    return sites


def main() -> int:
    substrings, extras, errors = parse_policy()
    classified = parse_classification()
    sites = find_switches()

    unclassified: list[tuple[str, str]] = []
    for name in sorted(sites):
        if any(needle in name for needle in substrings):
            continue          # the rule handles it
        if name in extras:
            continue          # explicitly named as self-driving
        if name in classified:
            continue          # a human confirmed it is tuning
        unclassified.append((name, sites[name][0]))

    # A name listed as tuning that no longer appears anywhere is not an error --
    # a deleted switch should not fail the build -- but it IS worth saying, so
    # the file does not silently accumulate ghosts.
    stale = sorted(classified - set(sites))

    for message in errors:
        print(f"lint-frontend-switch-coverage: {message}", file=sys.stderr)

    if unclassified:
        print("lint-frontend-switch-coverage: unclassified switch(es):", file=sys.stderr)
        for name, site in unclassified:
            print(f"  -{name}   first seen at {site}", file=sys.stderr)
        print("", file=sys.stderr)
        print("  Each one needs a decision. If the run drives itself and quits, rename it", file=sys.stderr)
        print("  to end in Test/Shot/After (the rule then handles it) or add it to", file=sys.stderr)
        print(f"  kSelfDrivingExtras in {POLICY_CPP} with a reason.", file=sys.stderr)
        print(f"  If it is ordinary tuning or configuration, add it to {CLASSIFICATION}.", file=sys.stderr)

    if stale:
        print(f"lint-frontend-switch-coverage: {len(stale)} name(s) in {CLASSIFICATION} "
              f"no longer appear in the source (not an error): {', '.join(stale)}")

    if errors or unclassified:
        return 1
    print(f"lint-frontend-switch-coverage: clean ({len(sites)} switch(es), "
          f"{len(substrings)} rule(s), {len(extras)} named exception(s), {len(classified)} classified as tuning)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
