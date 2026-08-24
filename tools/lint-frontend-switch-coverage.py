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


EXEMPTIONS_RE = re.compile(
    r"kRule5Exemptions\[\]\s*=\s*\{(.*?)\};", re.DOTALL)


def parse_exemptions() -> tuple[dict[str, str], list[str]]:
    """Returns ({exempt_name: reason}, errors) from kRule5Exemptions[].

    WHY THIS IS CHECKED AT ALL. Recording a switch as ACCIDENTAL in the
    classification file changes NOTHING about what the game does -- no C++ reads
    that file; it feeds this lint and nothing else. So a decision recorded there
    and not mirrored into kRule5Exemptions leaves this lint green while the menu
    still disappears for that switch, which is the precise failure this project
    keeps finding: an indicator that is not in the path it claims to watch.
    """
    errors: list[str] = []
    source = read(POLICY_CPP)
    match = EXEMPTIONS_RE.search(source)
    if not match:
        errors.append(f"{POLICY_CPP}: could not find kRule5Exemptions[]")
        return {}, errors
    out: dict[str, str] = {}
    for name, reason in EXTRA_ENTRY_RE.findall(match.group(1)):
        if len(reason.strip()) < MIN_REASON:
            errors.append(
                f"{POLICY_CPP}: kRule5Exemptions entry '{name}' has no meaningful reason")
        out[name] = reason
    return out, errors


# The acknowledgement section marker. Everything after it is
# "Name = reason" rather than a bare name.
ACK_MARKER = "## SUBSTRING-MATCHED -- ACKNOWLEDGED"


def parse_classification() -> tuple[set[str], dict[str, str]]:
    """Returns (tuning names, {substring-matched name: reason}).

    THE SECOND MAP IS THE 2026-08-23 ADDITION, and the defect it exists for is
    worth stating. Rule 5 suppresses the main menu for any switch CONTAINING
    Test/Shot/After/Check/Verify/Probe/... -- and the lint above then SKIPS
    that switch entirely, on the reasoning that "the rule handles it". So a
    switch whose name matches by accident is both mis-behaving (it suppresses
    the menu on an interactive launch) AND invisible to the one lint built to
    catch exactly this. `-VoxelFineLockProbe` was caught by a human, by luck.

    The suffix advice this lint prints does not fix the class either:
    `-VoxelGpuMeshInFlight` ENDS in "Flight" and is a job-count cap, not a
    fixture. Intent cannot be read off a name, so the decision has to be
    recorded instead of inferred -- which is what this section is.
    """
    names: set[str] = set()
    acknowledged: dict[str, str] = {}
    in_ack = False
    for line in read(CLASSIFICATION).splitlines():
        stripped = line.strip()
        if stripped.startswith(ACK_MARKER):
            in_ack = True
            continue
        if not stripped or stripped.startswith("#"):
            continue
        if in_ack:
            name, _, reason = stripped.partition("=")
            acknowledged[name.strip()] = reason.strip()
        else:
            names.add(stripped)
    return names, acknowledged


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
    classified, acknowledged = parse_classification()
    sites = find_switches()

    unclassified: list[tuple[str, str]] = []
    unacknowledged: list[tuple[str, str, str]] = []
    for name in sorted(sites):
        matched = [needle for needle in substrings if needle in name]
        if matched:
            # The rule DECIDES this one -- which is exactly why it needs a
            # recorded decision rather than a silent skip.
            if name not in acknowledged or len(acknowledged[name]) < MIN_REASON:
                unacknowledged.append((name, sites[name][0], ",".join(matched)))
            continue
        if name in extras:
            continue          # explicitly named as self-driving
        if name in classified:
            continue          # a human confirmed it is tuning
        unclassified.append((name, sites[name][0]))

    # A name listed as tuning that no longer appears anywhere is not an error --
    # a deleted switch should not fail the build -- but it IS worth saying, so
    # the file does not silently accumulate ghosts.
    stale = sorted(classified - set(sites))
    stale_ack = sorted(set(acknowledged) - set(sites))

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

    if unacknowledged:
        print("lint-frontend-switch-coverage: substring-matched switch(es) with no "
              "recorded decision:", file=sys.stderr)
        for name, site, matched in unacknowledged:
            print(f"  -{name}   matches '{matched}'   first seen at {site}", file=sys.stderr)
        print("", file=sys.stderr)
        print("  Rule 5 already treats each of these as SELF-DRIVING and suppresses the main", file=sys.stderr)
        print("  menu whenever it is on the command line -- so if any is ordinary tuning or a", file=sys.stderr)
        print("  diagnostic arm, it is changing front-end behaviour by accident AND is invisible", file=sys.stderr)
        print("  to the rest of this lint, which skips anything the rule matches.", file=sys.stderr)
        print(f"  Record the decision under '{ACK_MARKER}' in {CLASSIFICATION} as", file=sys.stderr)
        print("  'Name = reason', or rename the switch. Note that renaming to a SUFFIX does not", file=sys.stderr)
        print("  help: -VoxelGpuMeshInFlight already ends in 'Flight' and is a job cap.", file=sys.stderr)

    if stale:
        print(f"lint-frontend-switch-coverage: {len(stale)} name(s) in {CLASSIFICATION} "
              f"no longer appear in the source (not an error): {', '.join(stale)}")
    if stale_ack:
        print(f"lint-frontend-switch-coverage: {len(stale_ack)} acknowledged name(s) "
              f"no longer appear in the source (not an error): {', '.join(stale_ack)}")

    # --- the two halves of an ACCIDENTAL decision must agree -------------
    #
    # The classification file says which matches are accidents; kRule5Exemptions
    # is what actually spares them at runtime. Either one alone is a half-fix,
    # and the half that is easy to write is the one that does nothing.
    exemptions, exemption_errors = parse_exemptions()
    errors.extend(exemption_errors)

    accidental = {n for n, r in acknowledged.items()
                  if r.lstrip().upper().startswith("ACCIDENTAL")}
    missing_exemption = sorted(n for n in accidental if n not in exemptions)
    orphan_exemption = sorted(n for n in exemptions if n not in accidental)

    if missing_exemption:
        print()
        print(f"lint-frontend-switch-coverage: {len(missing_exemption)} switch(es) are "
              f"recorded ACCIDENTAL but are NOT in kRule5Exemptions[], so rule 5 "
              f"still suppresses the main menu for them on interactive runs:")
        print()
        for name in missing_exemption:
            print(f"  {name}")
        print()
        print(f"  Recording the decision in {CLASSIFICATION} does not change what")
        print(f"  the game does -- no C++ reads that file. Add each name to")
        print(f"  kRule5Exemptions[] in {POLICY_CPP} with a reason.")
        print()

    if orphan_exemption:
        print()
        print(f"lint-frontend-switch-coverage: {len(orphan_exemption)} name(s) in "
              f"kRule5Exemptions[] are not recorded ACCIDENTAL in {CLASSIFICATION}:")
        print()
        for name in orphan_exemption:
            print(f"  {name}")
        print()
        print("  An exemption nobody wrote down is indistinguishable from a mistake.")
        print("  Either record the decision or drop the exemption.")
        print()

    if (errors or unclassified or unacknowledged
            or missing_exemption or orphan_exemption):
        return 1
    print(f"lint-frontend-switch-coverage: clean ({len(sites)} switch(es), "
          f"{len(substrings)} rule(s), {len(extras)} named exception(s), "
          f"{len(classified)} classified as tuning, "
          f"{len(acknowledged)} substring-matched decisions recorded, "
          f"{len(exemptions)} of them exempted at runtime)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
