# The float-ban CI job is red, and has been for three days

**Status: diagnosis + patch text. NOTHING HAS BEEN APPLIED.** A screenshot
capture is in flight and its staleness guard aborts if anything under
`voxel-core/src` or `voxel-core/include` is newer than `voxelcore.lib`, so the
header edits below are text for a human to apply after the capture. Nothing was
built and no editor was launched while this was written.

## The answer

Yes, CI is red. The `float-ban` job fails with **exactly 16 findings, all in
`voxel-core/include/voxelcore/fluidoccupancy.h`**. The second-hand report was
accurate to the line count.

**All 16 are false positives.** Every one of them is in code the project has
already decided, in writing and with owner sign-off, is outside the determinism
boundary — PBF particle collision, which is *presentation*, not world
derivation. The bug is in the check, not the file. The check has no way to
express an exemption, while both of its sibling lints
(`tools/lint-shader-ub.py`, `tools/lint-unity-collisions.py`) do.

**It has been red since `66f0619`, 2026-08-09 13:15 — 3 days and 32 commits on
`main`.** Nobody noticed, which is the second finding: this check cannot be run
locally. It is ~20 lines of Python embedded in a `shell: python` step in
`.github/workflows/ci.yml`, so there is no command a developer can type before
pushing. Its two siblings live in `tools/` and are named in plans and
checklists; this one is invisible until GitHub goes red, and then it stays red.

The fix is three parts: extract the check to `tools/lint-float-ban.py` with a
region-scoped exemption mechanism copied from the sibling lints, bracket the
three float clusters in `fluidoccupancy.h` with reasons, and add one paragraph
to `docs/determinism.md` recording that the exemption exists. Patch text is at
the bottom, and it was verified against a scratch copy of the tree.

---

## 1. What the check is, and why the ban exists

The job is `float-ban` in `.github/workflows/ci.yml`, lines 75-104. It walks
`voxel-core/include` and `voxel-core/src`, strips `//` and `/* */` comments,
and fails on any line matching `\b(float|double)\b`. 57 files are in scope
today.

The rule it enforces is doctrine §2.3, written down in `docs/determinism.md:27`:

> No floating point anywhere in world derivation. `float`/`double` are banned
> in `voxel-core/src` and `voxel-core/include` (CI greps for them; rendering
> and bench timing live outside that boundary).

The "why" is not theoretical. From `docs/adr/0004-swe-fixed-point-coupling.md`:

> voxel-core is integer-only by doctrine §2.3, because worldgen and simulation
> must be bit-identical across machines and GPU vendors — a rule paid for in
> blood when the M0 cross-vendor gate passed on AMD and failed on NVIDIA
> against identical committed SPIR-V (ADR-0001).

Saved edit logs, golden digests and multiplayer all rest on that bit-identity.
A float that reaches world derivation desyncs clients and invalidates every
saved world. This is a check worth keeping sharp.

**The important detail: the two directory names are a PROXY for the boundary,
not the boundary.** The job's own header comment says so — "Bench/timing and
UE-side rendering are outside the boundary" — it just has no way to say that
about anything living inside those two directories.

## 2. Running it: the claim is true

Run verbatim from `.github/workflows/ci.yml` against the current working tree:

```
float/double type found in voxel-core world-derivation code:
voxel-core/include/voxelcore/fluidoccupancy.h:109: inline constexpr float kFluidVoxelUU = static_cast<float>(kVoxelSizeMm) / 10.0f;
voxel-core/include/voxelcore/fluidoccupancy.h:688: inline constexpr float fluidRebaseDeltaUU(int32_t deltaVoxels) {
voxel-core/include/voxelcore/fluidoccupancy.h:689:     return static_cast<float>(deltaVoxels) * kFluidVoxelUU;
voxel-core/include/voxelcore/fluidoccupancy.h:759: inline constexpr float kFluidCollisionSkinUU = 0.01f;
voxel-core/include/voxelcore/fluidoccupancy.h:764: inline constexpr float kFluidNoCrossingT = 1e30f;
voxel-core/include/voxelcore/fluidoccupancy.h:768: inline float fluidMaxTraversalSpeedUU(float dtSeconds) {
voxel-core/include/voxelcore/fluidoccupancy.h:769:     return static_cast<float>(kFluidMaxCollisionSteps) * kFluidVoxelUU /
voxel-core/include/voxelcore/fluidoccupancy.h:783:     float tEnter = 0.0f;
voxel-core/include/voxelcore/fluidoccupancy.h:799:                                 const float prevPosUU[3], const float posUU[3]) {
voxel-core/include/voxelcore/fluidoccupancy.h:818:     float d[3], tMax[3], tDelta[3];
voxel-core/include/voxelcore/fluidoccupancy.h:824:         const float localV = static_cast<float>(v[a] - originVoxel[a]);
voxel-core/include/voxelcore/fluidoccupancy.h:840:     float tLast = 0.0f;
voxel-core/include/voxelcore/fluidoccupancy.h:874:     float posUU[3] = {0.0f, 0.0f, 0.0f};
voxel-core/include/voxelcore/fluidoccupancy.h:877:     float normal[3] = {0.0f, 0.0f, 0.0f};
voxel-core/include/voxelcore/fluidoccupancy.h:905:                                            const float posUU[3], const float prevPosUU[3]) {
voxel-core/include/voxelcore/fluidoccupancy.h:953:         const float localV = static_cast<float>(h.faceAxis == 0   ? h.vx - originVoxel[0]
exit status 1
```

16 findings, one file. No other file in the 57 fails. The two untracked headers
another agent has just added under `voxel-core/include` (`weather.h`,
`materialpalette.h`) are float-clean, so they are not part of this and will not
make it worse when committed.

The `float-ban` job is independent of the build and test jobs, so the rest of CI
is presumably green; this one job is the whole failure.

## 3. Real or false positive: all 16 are false positives

None of the 16 is a float in a comment or a string — the existing check already
handles those correctly, and it was patched once before for exactly that
(`f610aa2`, "false-positive on 'double-visit' in a comment"). These are real
`float` declarations in real code. The question is whether that code is inside
the determinism boundary. It is not, and the project has said so three times
independently.

**The file's own header block** (`fluidoccupancy.h:19-27`) states the policy
before any of the code:

> FLOAT POLICY. The packing half is integer-only, per the library's rules: bit
> indices, word indices and the brick→volume transpose never touch a float. The
> collision half IS float, deliberately. Particles are PRESENTATION under the
> plan's authority split (authoritative water is the integer basin ledgers and
> the routing graph), so no gameplay consequence and nothing on the wire depends
> on a particle position being bit-reproducible. What still has to be exactly
> right is which FACE was entered, and that is decided by an integer DDA over
> integer voxel coordinates.

**The plan that authorised it**, `docs/water-rearchitecture-plan-2026-08-09.md`
§5, is an owner decision ("Owner's decision, reaffirmed after review"):

> **Determinism / float ban.** The project's integer-determinism rules apply to
> *authoritative* state. The split: **authority = scalar hydrology** — per-basin
> volume ledgers, faucet rates, graph segment storage — all integers, all tiny,
> all persisted. **PBF = presentation of flow**, driven by those scalars. No
> particle position is ever authoritative … Nothing particle-shaped crosses the
> wire.

**The structural evidence.** Nothing in `voxel-core/src` includes this header —
it cannot reach world derivation even by accident. Its only consumers are
`voxel-core/tests/test_fluidoccupancy.cpp` and two UE translation units
(`VoxelFluidSubsystem.cpp`, `VoxelEarthShaders/Public/VoxelFluidOccupancy.h`),
i.e. the test harness and the render side, both explicitly outside the boundary.
It contributes nothing to `vxc::WorldDigest`, so the cross-compiler determinism
job is unaffected by it — and indeed that job is green while this one is red,
which is itself evidence the floats are not reaching world derivation.

Grouped, the 16 lines are three clusters:

| Lines | What | Verdict |
|---|---|---|
| 109 | `kFluidVoxelUU` — one voxel in Unreal units | False positive. UU is the presentation frame; the constant feeds only the collision maths and the UE mirrors. |
| 688-689 | `fluidRebaseDeltaUU` — shifts particle positions when the window recentres | False positive. Operates on particle positions, which are presentation. Its exactness is bounded by `kFluidRebaseExactMaxVoxels` and pinned by test (lines 678-700). |
| 759-968 | The collision half: skin, sentinel, speed bound, the float Amanatides & Woo walk and the face projection | False positive. The declared-float half. The one thing that must be exact — which face was entered — is decided on integer voxel coordinates and pinned against `raycast.h`'s exact integer walk by `tests/test_fluidoccupancy.cpp`. |

The judgement call, stated plainly: I am treating "presentation code physically
located in `voxel-core`" as legitimate. The trade is that the boundary stops
being "a directory" and becomes "a directory minus annotated regions", which is
weaker to audit. What would change my mind is any of the three claims above
being false — if something in `voxel-core/src` started including this header, or
if a particle position ever became authoritative or replicated, these 16 lines
would become real violations and the exemptions would have to come out. The
annotation text below says so, so a future reader can check.

## 4. How long it has been red, and why nobody saw it

`git blame` on the 16 lines:

- 14 of 16 arrived in **`66f0619`, 2026-08-09 13:15** — "fluid: Phase 0 spikes
  (a) and (c) — the PBF solver core and the collision volume", the commit that
  created the file.
- 2 of 16 (688-689, the rebase helper) arrived in **`bd81fca`, 2026-08-09
  21:00**.

So the job went red the moment the file landed and has never been green since:
**3 days, 32 commits on `main`**, spanning the whole fluid Phase 3-5 push, the
sky work, the bathymetry fixes and the water rendering rebuild.

Three days is short in absolute terms, but the mechanism is the concerning part
and it is not new to this repo. Two things kept it invisible:

1. **The check cannot be run locally.** It is inline YAML. `lint-shader-ub.py`
   and `lint-unity-collisions.py` are files in `tools/` that plans and
   checklists name by command (`python tools/lint-shader-ub.py …` appears in
   `docs/gpu-waves-plan.md`, `docs/cavern-design.md`). There is no equivalent
   line anyone can run for `float-ban`, so "float-ban clean" claims in
   `docs/status.md` and in ADRs 0004/0007 were asserted, not executed.
2. **It has no exemption mechanism**, so the first legitimate exception in the
   project's history had nowhere to go and the author had to either fight the
   check or leave it red. Leaving it red is what happened, and a check that is
   permanently red is a check nobody reads — the same failure mode as the
   staleness guard noted in `tools/voxel-capture.ps1`.

## 5. Two latent bugs in the current check, fixed in passing

Neither fires today; both are one commit away from firing.

1. **Reported line numbers are wrong after any multi-line block comment.**
   `re.sub(r"/\*.*?\*/", " ", src, flags=re.S)` collapses a comment spanning *n*
   lines into a single space, shifting every subsequent line number by *n-1*.
   There is no multi-line block comment in `voxel-core` today, which is the only
   reason the 16 numbers above are correct. Verified: with a 4-line block
   comment inserted into `core.h`, the old form misreports by 3; the replacement
   reports the true line.
2. **String literals are not stripped, though the comment says they are.** The
   step's own comment reads "Strip `//` and `/* */` comments and string literals
   before searching", and the step name says "(comments/strings stripped)", but
   no string handling exists. Today no string literal in `voxel-core` contains
   the word float or double (checked, zero hits), so it is latent. A
   `static_assert(..., "no float here")` message would trip it.

## 6. Alternatives considered and rejected

- **Add a file-level skip for `fluidoccupancy.h`.** Rejected: the *packing* half
  of that same file must stay integer-only — it computes bit and word indices
  that the GPU mirrors, and the header says so at line 19. A per-file skip
  removes the guard from the half that needs it most.
- **Move the file out of `voxel-core`.** Rejected: the header's stated reason to
  exist is that it is the engine-free definition testable by `vxc_tests`
  ("neither can be tested where it lives … This header is the engine-free
  definition of both halves"). Moving it to `ue-project` would take it out of
  reach of `voxel-core/tests/test_fluidoccupancy.cpp` and out of reach of the
  raycast.h parity test that keeps the two walks from drifting.
- **Per-line annotations instead of regions**, matching `lint-shader-ub`
  exactly. Rejected on balance: 16 near-identical one-line justifications in one
  header is copy-paste that defeats the point of a mandatory reason, and it
  would make the file materially worse to read. Regions give one real reason per
  cluster. The trade is that a region is coarser — new float code added inside
  the collision region is silently exempt. Three things limit that: regions must
  be closed explicitly (so nothing appended to the file is covered), an empty
  region is an error, and the collision region is a section that is entirely
  float by design already. If you would rather have the finer instrument, the
  per-line form is a small change to `parse_regions` and I would not argue.
- **Leave the check inline in YAML and just add the annotation logic.**
  Workable — about 15 extra lines — and it is the smaller diff if you want the
  minimum. I recommend against it: the inability to run this locally is a direct
  cause of the three-day redness, and the annotation logic is past the size
  where YAML-embedded Python is reviewable.

## 7. Does this need owner sign-off?

Possibly. ADRs 0004 and 0007 both make a point of saying "no waiver is
requested", which implies a §2.3 waiver is an owner-signed thing. My reading is
that the sign-off already exists — the water re-architecture plan's §5 authority
split is recorded as the owner's decision, reaffirmed after review, and this
header is that decision's collision half. So the patch below records the
exemption rather than requesting a new one. If you want it ratified as an ADR
instead, the material is the FLOAT POLICY block already in the header plus plan
§5; say the word and it is a short document.

---

# THE PATCH (not applied)

Four files. Only the third touches `voxel-core/include`; apply that one after
the capture finishes.

## Patch 1 of 4 — NEW FILE `tools/lint-float-ban.py`

```python
#!/usr/bin/env python3
"""Doctrine §2.3 float ban for voxel-core (docs/determinism.md).

WHAT IT FORBIDS AND WHY. Everything below the 30 m diffusion tiles must derive
bit-identical worlds on every compiler, OS, CPU and GPU vendor. Floating point
does not give that: the M0 gate passed on AMD and failed on NVIDIA running the
identical committed SPIR-V (ADR-0001), and IEEE contraction/reassociation
differs between compilers on the CPU side. So `float` and `double` are banned as
TYPES in voxel-core/include and voxel-core/src. Bench timing, UE-side rendering
and terrain-service live outside that boundary and are not scanned.

The directory is a PROXY for the boundary, not the boundary itself, and the
proxy is not exact: voxel-core also carries engine-free reference code for
things that are explicitly presentation. Those are silenced one region at a
time by an annotation carrying a written reason:

    // float-ban: begin-allow - <reason, >= 10 chars>
    ...
    // float-ban: end-allow

Four properties keep the allowlist honest, matching lint-shader-ub.py and
lint-unity-collisions.py:

  * A reason is mandatory. A bare `begin-allow` is itself an error.
  * A region that silences nothing is an error, so a stale exemption cannot sit
    there quietly covering future code.
  * An unterminated region is an error. Without this, one marker near the top of
    a file would exempt everything appended to it forever.
  * Regions do not nest, and `end-allow` without `begin-allow` is an error.

Annotations are read from the RAW text; the ban itself is matched after comments
and string literals are blanked, so prose can never trip the guard and a comment
can still carry the annotation.

Blanking preserves line numbers (a multi-line block comment becomes the same
number of newlines, not one space), which the earlier inline-YAML version of
this check did not do. Known limitation: raw string literals (R"(...)") are not
modelled; there are none in voxel-core today.

Usage:  python tools/lint-float-ban.py [--root .]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

SCAN_DIRS = ("voxel-core/include", "voxel-core/src")
SUFFIXES = (".h", ".hpp", ".cpp", ".cc")

BANNED = re.compile(r"\b(float|double)\b")
BEGIN_RE = re.compile(r"//\s*float-ban:\s*begin-allow\b(.*)$")
END_RE = re.compile(r"//\s*float-ban:\s*end-allow\b")
MIN_REASON = 10


def blank_comments_and_strings(src: str) -> str:
    """Replace comment and string-literal content with spaces, keeping every
    newline so reported line numbers match the file on disk."""
    out: list[str] = []
    i, n = 0, len(src)
    state = "code"
    quote = ""
    while i < n:
        c = src[i]
        if state == "code":
            if c == "/" and i + 1 < n and src[i + 1] == "/":
                state, i = "line", i + 2
                out.append("  ")
                continue
            if c == "/" and i + 1 < n and src[i + 1] == "*":
                state, i = "block", i + 2
                out.append("  ")
                continue
            # A ' between two word characters is a C++14 digit separator
            # (kSurfaceClampMinMm = -8'000'000), not a char literal.
            if c == "'" and i > 0 and src[i - 1].isalnum():
                out.append(c)
                i += 1
                continue
            if c in "\"'":
                state, quote, i = "str", c, i + 1
                out.append(" ")
                continue
            out.append(c)
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if c == "*" and i + 1 < n and src[i + 1] == "/":
                state, i = "code", i + 2
                out.append("  ")
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        # state == "str"
        if c == "\\" and i + 1 < n:
            out.append("  ")
            i += 2
            continue
        if c == quote:
            state = "code"
            out.append(" ")
            i += 1
            continue
        out.append("\n" if c == "\n" else " ")
        i += 1
    return "".join(out)


def parse_regions(raw_lines: list[str], path: pathlib.Path) -> tuple[list[dict], list[str]]:
    """Returns (regions, errors). A region is {'start', 'end', 'reason', 'used'}."""
    regions: list[dict] = []
    errors: list[str] = []
    open_region: dict | None = None
    for idx, line in enumerate(raw_lines, 1):
        begin = BEGIN_RE.search(line)
        end = END_RE.search(line)
        if begin:
            if open_region is not None:
                errors.append(
                    f"{path}:{idx}: nested `float-ban: begin-allow` (the one opened at line "
                    f"{open_region['start']} is still open). Regions do not nest."
                )
                continue
            reason = begin.group(1).lstrip()
            reason = reason[1:].strip() if reason.startswith("-") else reason.strip()
            if len(reason) < MIN_REASON:
                errors.append(
                    f"{path}:{idx}: `float-ban: begin-allow` needs a written reason of at "
                    f"least {MIN_REASON} characters — fail-closed by design."
                )
                continue
            open_region = {"start": idx, "end": None, "reason": reason, "used": False}
            continue
        if end:
            if open_region is None:
                errors.append(
                    f"{path}:{idx}: `float-ban: end-allow` with no open `begin-allow`."
                )
                continue
            open_region["end"] = idx
            regions.append(open_region)
            open_region = None
    if open_region is not None:
        errors.append(
            f"{path}:{open_region['start']}: unterminated `float-ban: begin-allow` — add "
            f"`// float-ban: end-allow` so the exemption cannot cover code appended later."
        )
    return regions, errors


def lint_file(path: pathlib.Path, display: pathlib.Path) -> list[str]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    raw_lines = raw.splitlines()
    regions, problems = parse_regions(raw_lines, display)
    code_lines = blank_comments_and_strings(raw).splitlines()

    for i, line in enumerate(code_lines, 1):
        if not BANNED.search(line):
            continue
        covering = next(
            (r for r in regions if r["start"] < i < (r["end"] or 0)), None
        )
        if covering is not None:
            covering["used"] = True
            continue
        problems.append(f"{display}:{i}: {raw_lines[i - 1].strip()}")

    for r in regions:
        if not r["used"]:
            problems.append(
                f"{display}:{r['start']}: unused `float-ban: begin-allow` region "
                f"(lines {r['start']}-{r['end']}) — it silences nothing. Delete it so it "
                f"cannot silently cover future code."
            )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=".", help="repository root (default: cwd)")
    args = ap.parse_args()
    root = pathlib.Path(args.root)

    problems: list[str] = []
    scanned = 0
    for base in SCAN_DIRS:
        d = root / base
        if not d.is_dir():
            print(f"lint-float-ban: {d} not found — run from the repo root, or pass --root",
                  file=sys.stderr)
            return 1
        for f in sorted(d.rglob("*")):
            if f.suffix not in SUFFIXES:
                continue
            scanned += 1
            problems.extend(lint_file(f, f.relative_to(root)))

    if problems:
        print("lint-float-ban: doctrine §2.3 violations\n")
        for p in problems:
            print(p)
        print(
            f"\n{len(problems)} finding(s). float/double are banned as types in "
            f"voxel-core/include and voxel-core/src because world derivation must be "
            f"bit-identical across compilers and GPU vendors (docs/determinism.md, "
            f"ADR-0001). Use integer or fixed-point arithmetic, or — only for code that "
            f"is provably outside the determinism boundary — bracket it with\n"
            f"    // float-ban: begin-allow - <why this is not world derivation>\n"
            f"    // float-ban: end-allow"
        )
        return 1

    print(f"lint-float-ban: clean ({scanned} file(s) checked, fail-closed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

## Patch 2 of 4 — `.github/workflows/ci.yml`

Replace lines 75-104 in their entirety.

**BEFORE:**

```yaml
  # Doctrine §2.3: no floating point in world-derivation code. Bench/timing
  # and UE-side rendering are outside the boundary.
  float-ban:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Grep for float/double in voxel-core src+include (comments/strings stripped)
        shell: python
        run: |
          import re, sys, pathlib
          # Doctrine bans float/double as TYPES in world-derivation code, not
          # the words "double" / "float" appearing in comments or strings
          # (e.g. "double-visit"). Strip // and /* */ comments and string
          # literals before searching so prose never trips the guard.
          bad = []
          for base in ("voxel-core/include", "voxel-core/src"):
              for f in pathlib.Path(base).rglob("*"):
                  if f.suffix not in (".h", ".hpp", ".cpp", ".cc"):
                      continue
                  src = f.read_text(encoding="utf-8", errors="replace")
                  src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)   # block comments
                  src = re.sub(r"//[^\n]*", " ", src)                # line comments
                  for i, line in enumerate(src.splitlines(), 1):
                      if re.search(r"\b(float|double)\b", line):
                          bad.append(f"{f}:{i}: {line.strip()}")
          if bad:
              print("float/double type found in voxel-core world-derivation code:")
              print("\n".join(bad))
              sys.exit(1)
          print("float-ban clean")
```

**AFTER:**

```yaml
  # Doctrine §2.3: no floating point in world-derivation code. Bench/timing
  # and UE-side rendering are outside the boundary.
  #
  # This was ~20 lines of Python inlined here until it stayed red for three days
  # and 32 commits without anyone noticing (fluidoccupancy.h, 2026-08-09). Two
  # causes, both fixed by moving it to tools/. There was no command a developer
  # could run before pushing, so every "float-ban clean" claim in status.md and
  # in ADRs 0004/0007 was asserted rather than executed. And it had no way to
  # express an exception, while both sibling lints do -- so the project's first
  # legitimate exemption had nowhere to go and the job just stayed red.
  #
  # The directories are a PROXY for the determinism boundary. voxel-core also
  # holds engine-free reference code for things that are explicitly presentation
  # (PBF particle collision: no particle position is authoritative and none
  # crosses the wire -- water-rearchitecture-plan-2026-08-09.md §5). Those are
  # exempted one region at a time by an annotation carrying a written reason;
  # an empty or unterminated region is itself a failure, as in shader-ub-lint.
  float-ban:
    name: Doctrine §2.3 float ban
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: "3.11"
      - name: Lint voxel-core src+include for float/double types
        run: python tools/lint-float-ban.py
```

## Patch 3 of 4 — `voxel-core/include/voxelcore/fluidoccupancy.h`

**DO NOT APPLY UNTIL THE CAPTURE IS DONE.** Four edits, all comment-only: no
code changes, so the compiled behaviour is byte-identical and no rebuild is
logically required (only the staleness guard's mtime check cares).

### 3a — around line 109

**BEFORE:**

```cpp
inline constexpr float kFluidVoxelUU = static_cast<float>(kVoxelSizeMm) / 10.0f;
static_assert(kVoxelSizeMm == 100, "kFluidVoxelUU's conversion assumes 10 cm voxels");
```

**AFTER:**

```cpp
// float-ban: begin-allow - UU is the presentation frame; consumed only by the
// collision half below and by the UE mirrors, never by world derivation
inline constexpr float kFluidVoxelUU = static_cast<float>(kVoxelSizeMm) / 10.0f;
// float-ban: end-allow
static_assert(kVoxelSizeMm == 100, "kFluidVoxelUU's conversion assumes 10 cm voxels");
```

### 3b — around lines 688-690

**BEFORE:**

```cpp
inline constexpr float fluidRebaseDeltaUU(int32_t deltaVoxels) {
    return static_cast<float>(deltaVoxels) * kFluidVoxelUU;
}
```

**AFTER:**

```cpp
// float-ban: begin-allow - rebases PARTICLE positions, which are presentation
// under the plan's authority split; exactness bounded by
// kFluidRebaseExactMaxVoxels below and pinned by test
inline constexpr float fluidRebaseDeltaUU(int32_t deltaVoxels) {
    return static_cast<float>(deltaVoxels) * kFluidVoxelUU;
}
// float-ban: end-allow
```

### 3c — around lines 702-705, the collision section banner

**BEFORE:**

```cpp
// ---------------------------------------------------------------------------
// Collision: the voxel-line walk and the face projection
// ---------------------------------------------------------------------------
```

**AFTER:**

```cpp
// ---------------------------------------------------------------------------
// Collision: the voxel-line walk and the face projection
// ---------------------------------------------------------------------------
//
// float-ban: begin-allow - the collision half is float by design (see FLOAT
// POLICY at the top): it moves PBF particles, which are presentation, not
// authority. The one thing that must be exact -- which FACE was entered -- is
// decided by integer voxel coordinates, and test_fluidoccupancy.cpp pins that
// against raycast.h's exact integer walk. The region ends before the end of the
// file so code appended later is still guarded.
```

### 3d — the end of the file, lines 967-970

**BEFORE:**

```cpp
    return out;
}

} // namespace vxc
```

**AFTER:**

```cpp
    return out;
}
// float-ban: end-allow

} // namespace vxc
```

## Patch 4 of 4 — `docs/determinism.md`, line 27

The doctrine text currently states the ban with no exception mechanism, which is
what left the first legitimate exception with nowhere to go.

**BEFORE:**

```markdown
- No floating point anywhere in world derivation. `float`/`double` are banned
  in `voxel-core/src` and `voxel-core/include` (CI greps for them; rendering
  and bench timing live outside that boundary).
```

**AFTER:**

```markdown
- No floating point anywhere in world derivation. `float`/`double` are banned
  in `voxel-core/src` and `voxel-core/include` (`tools/lint-float-ban.py`, CI
  job `float-ban`; rendering and bench timing live outside that boundary).
  Those two directories are a *proxy* for the boundary and the proxy is not
  exact — voxel-core also holds engine-free reference code for things that are
  explicitly presentation. Such code is exempted one region at a time, with a
  written reason, by bracketing it:

  ```cpp
  // float-ban: begin-allow - <why this is not world derivation>
  // float-ban: end-allow
  ```

  A region that silences nothing, one left unterminated, and a bare
  `begin-allow` with no reason are all failures, so the mechanism cannot be
  used to bulk-silence the rule. There is exactly one exemption today:
  `fluidoccupancy.h`'s PBF particle-collision half, under the authority split
  in `docs/water-rearchitecture-plan-2026-08-09.md` §5 (no particle position is
  authoritative and none crosses the wire). If a particle position ever becomes
  authoritative, that exemption comes out.
```

---

# Verification performed (no repo files written except this one)

The replacement lint was run against a scratch copy of `voxel-core`, not the
working tree.

1. **Reproduces the old result exactly.** Against the unmodified tree:
   16 findings, the same 16 lines, exit 1.
2. **Goes clean with Patch 3 applied.** Against a scratch copy with the four
   header edits: `lint-float-ban: clean (57 file(s) checked, fail-closed)`,
   exit 0.
3. **A new float outside the regions still fails.** `inline double kSneaky`
   appended after `end-allow` → reported, exit 1.
4. **An empty region fails.** A `begin-allow`/`end-allow` pair silencing nothing
   → "unused … it silences nothing", exit 1.
5. **An unterminated region fails**, and the float it was hiding is still
   reported.
6. **A bare `begin-allow` fails** with "needs a written reason of at least 10
   characters", and the float is still reported.
7. **Prose and strings still never trip it.** `// a double-visit of the float
   queue` plus `static const char* kMsg = "float double";` → clean.
8. **Line numbers survive a multi-line block comment.** A 4-line block comment
   inserted into `core.h`: the replacement reports line 415, which is where the
   float actually is; the current inline check would report 412.

Nothing under `voxel-core/` was modified, no build was run, and no editor was
launched.
