#!/usr/bin/env python3
"""Internal-linkage name-collision lint for the unity-built Unreal modules.

WHY THIS EXISTS
---------------
`VoxelSweBreachFixture.cpp:371` and `VoxelOceanCaptureFixture.cpp:88` each
declared

    using FRunRef = TSharedRef<FSweBreachRun, ESPMode::ThreadSafe>;
    using FRunRef = TSharedRef<FOceanRun,     ESPMode::ThreadSafe>;

inside an ANONYMOUS namespace. As separate translation units that is correct
and idiomatic C++: two anonymous namespaces never meet. But UnrealBuildTool
concatenates a module's .cpp files into unity blobs, and once both files land
in the same blob the two anonymous namespaces ARE the same scope -- `FRunRef`
is then declared twice, with different types, and the module does not compile.

(PR #210's commit message names VoxelSkyLadderFixture.cpp as the other half.
That is a misattribution: the sky ladder has used a NAMED namespace,
`VoxelSkyLadderDetail`, since 2026-07-29 (2284c5e), so its `FRunRef` is
`VoxelSkyLadderDetail::FRunRef` and cannot collide with anything. The SWE
breach fixture is the file the sky ladder borrowed the vocabulary FROM, and
it is still anonymous today -- which is fine now only because the ocean
fixture's alias was renamed away from it.)

The default build is unity. `-DisableUnity` is not. The fixture was written
and built with `-DisableUnity`, which is exactly why the collision shipped
(PR #210 fixed it, days later, only because an agent happened to build without
that flag). The whole class is invisible to review, invisible to a
`-DisableUnity` build, and invisible to every existing CI job -- none of which
compiles the Unreal module at all.

This lint catches the class WITHOUT AN ENGINE. It needs nothing but Python and
the source tree, so it runs on a stock GitHub-hosted runner in about a second,
where a real UE build cannot run at all (see .github/workflows/ue-build.yml
for why: the engine is a 30 GB install and hosted runners have a 14 GB disk).

It is a substitute for a compile, not a replacement. A compile catches
everything; this catches one class. It happens to be the class that actually
bit us.

WHAT COUNTS AS A COLLISION
--------------------------
A name has INTERNAL LINKAGE, and therefore becomes file-local-in-name-only
under unity, when it is declared:

  * at the top level of an anonymous namespace, or
  * at file scope with the `static` keyword, or
  * by DEFINE_LOG_CATEGORY_STATIC (which expands to a file-scope static).

Two files in the SAME MODULE declaring the same such name is a finding when
the declarations are not interchangeable:

  ALIAS / VARIABLE  finding when the declaration text differs. Two IDENTICAL
                    `using` aliases are a legal redeclaration, so they are not
                    reported; `= TSharedRef<FSweBreachRun,...>` versus
                    `= TSharedRef<FOceanRun,...>` is the shipped bug.
  RECORD / ENUM     finding on ANY duplicate name. A class, struct, union or
                    enum cannot be defined twice in one translation unit even
                    if the two definitions are character-identical. Forward
                    declarations (no body) are not recorded.
  FUNCTION          finding only when the normalized parameter list matches
                    too. Different parameters are an overload, which is legal
                    and common; the same signature twice is a redefinition.
  LOG CATEGORY      finding on any duplicate name.

Module boundaries are respected: unity blobs never span modules, so the same
name in VoxelEarth and in VoxelEarthShaders is not a finding.

The lint deliberately does NOT ban anonymous namespaces. They are correct C++
and the module uses them in ~30 files. It bans the collision, which is the
part that is actually wrong.

WHAT IT DOES NOT CATCH
----------------------
It is a linter, not a C++ front end. It sees declarations, not macro
expansions (beyond the one UE macro above), not templates instantiated into
existence, and not `#if`-disabled code -- every branch is scanned as if live,
which is the conservative direction. It cannot see collisions between a
module .cpp and a header that .cpp includes. And it cannot tell you which
files UBT actually grouped into a blob, so it treats every pair of .cpp files
in a module as potentially co-resident. That over-approximates, which is the
right way for this to be wrong: UBT's grouping changes when a file is added.

Comments and string/character literals are blanked (newline-preserving)
before any matching, in the style of the existing `float-ban` job and
tools/lint-shader-ub.py, so prose can never trip a rule.

SUPPRESSION
-----------
A finding is silenced by an inline annotation on the declaration's line or
the line immediately above it, carrying a mandatory reason:

    // lint-unity: allow - <reason, >= 10 characters>

A bare `allow` is itself an error, and an annotation that silences nothing is
an error, so a stale justification fails the build instead of quietly
covering future code. Same two properties as tools/lint-shader-ub.py.

USAGE
-----
    python tools/lint-unity-collisions.py                 # default modules
    python tools/lint-unity-collisions.py ue-project/Source/VoxelEarth
Exit status 0 = clean, 1 = findings.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from collections import defaultdict

DEFAULT_ROOTS = [
    os.path.join("ue-project", "Source", "VoxelEarth"),
    os.path.join("ue-project", "Source", "VoxelEarthShaders"),
    # The front end. UI code is unusually dense in file-local constants --
    # kGold, kBorderPx, kMenuZOrder -- which is precisely the collision class
    # this lint models, so the module is covered from the commit that created
    # it rather than from the first time it bites.
    os.path.join("ue-project", "Source", "VoxelEarthUI"),
]

ANNOTATION_RE = re.compile(r"//\s*lint-unity:\s*allow\b(.*)$")
MIN_REASON = 10


# ---------------------------------------------------------------------------
# Lexical preprocessing
# ---------------------------------------------------------------------------

def blank_comments_and_strings(src: str) -> str:
    """Replace comment and literal bodies with spaces, preserving newlines.

    Byte offsets and line numbers are unchanged, so findings still point at
    the real source location.
    """
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            quote = c
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == quote or src[j] == "\n":
                    break
                j += 1
            j = min(j + 1, n)
            for k in range(i + 1, j - 1):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def line_of(src: str, pos: int) -> int:
    return src.count("\n", 0, pos) + 1


def match_brace(src: str, open_pos: int) -> int:
    """Index just past the '}' matching the '{' at open_pos (or len(src))."""
    depth = 0
    i, n = open_pos, len(src)
    while i < n:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


# ---------------------------------------------------------------------------
# Declaration extraction
# ---------------------------------------------------------------------------

NAMESPACE_RE = re.compile(r"\bnamespace\b(?P<name>[\w\s:]*?)\{")
LOG_CATEGORY_RE = re.compile(r"\bDEFINE_LOG_CATEGORY_STATIC\s*\(\s*(\w+)")
USING_NS_RE = re.compile(r"\busing\s+namespace\s+([\w:]+)\s*;")

# The bucket of names reachable by UNQUALIFIED lookup at file scope. Anything
# that lands here from two different .cpp files in one unity blob is a
# redefinition or an ambiguity. See FILE-VISIBLE SCOPE in the module docstring.
FILE_VISIBLE = "(file-visible)"

ALIAS_RE = re.compile(r"^using\s+(\w+)\s*=\s*(.+)$", re.S)
TYPEDEF_RE = re.compile(r"^typedef\s+(.*?)\b(\w+)\s*$", re.S)
RECORD_RE = re.compile(r"\b(?:struct|class|union)\s+(?:\w+_API\s+)?(\w+)\b")
ENUM_RE = re.compile(r"\benum\s+(?:class\s+|struct\s+)?(\w+)\b")
FUNC_RE = re.compile(r"(?:^|[\s*&>])(\w+)\s*\(([^()]*)\)\s*[\w\s:]*$", re.S)
VAR_RE = re.compile(r"(\w+)\s*(?:\[[^\]]*\])?\s*$")

# Names that mean "this is not a declaration we can reason about".
CONTROL_KEYWORDS = {
    "if", "for", "while", "switch", "return", "else", "do", "case",
    "static_assert", "template", "extern", "friend",
}


def normalize(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def collides(a: "Decl", b: "Decl") -> bool:
    """Would these two declarations conflict inside one translation unit?

    The rule is NOT the same in both directions, which is the easy mistake:

      ALIAS / VARIABLE  conflict when the definitions DIFFER. Two identical
                        `using` aliases are a legal redeclaration.
      FUNCTION          conflict when the signatures MATCH. Two functions of
                        the same name and different parameters are an
                        overload set, which is legal and common here --
                        VoxelSkyLadderFixture and VoxelSweBreachFixture share
                        a whole helper vocabulary (Finish, StageBegin,
                        WorldOf) that overloads cleanly once their run types
                        are distinct.
      RECORD / ENUM     always conflict; a type cannot be defined twice in one
      LOG_CATEGORY      translation unit even character-identically.

    Function signatures are compared as WRITTEN, so two params both spelled
    `FRunRef` look identical even when the alias means different types in the
    two files. That is deliberate: the alias itself is then reported, which is
    the root cause, instead of every helper that takes one.
    """
    if a.kind != b.kind:
        return False
    if a.sig is None:
        return True
    if a.kind == "FUNCTION":
        return a.sig == b.sig
    return a.sig != b.sig


class Decl:
    __slots__ = ("kind", "name", "sig", "line", "path", "scope", "is_static")

    def __init__(self, kind, name, sig, line, path):
        self.scope = ()
        self.is_static = False
        self.kind = kind      # ALIAS | RECORD | ENUM | FUNCTION | VARIABLE | LOG_CATEGORY
        self.name = name
        # sig is None  -> any duplicate name is a finding
        # sig is a str -> a finding only when two files disagree on it
        self.sig = sig
        self.line = line
        self.path = path


def classify(raw_head: str, has_body: bool, start_line: int, path: str):
    """Turn one top-level declaration's text into a Decl, or None.

    `start_line` is where the statement began, which is just after the
    previous `;` and so usually sits on a preceding comment line. Findings
    are reported at the line holding the NAME instead: that is where a reader
    looks, and the `lint-unity: allow` annotation is documented to sit on the
    declaration's own line or the one above it.
    """
    def at(name):
        idx = raw_head.find(name)
        return start_line + (raw_head.count("\n", 0, idx) if idx >= 0 else 0)

    head = normalize(raw_head)
    if not head:
        return None

    first = head.split(" ", 1)[0].rstrip("(:")
    if first in CONTROL_KEYWORDS:
        return None
    # Macro invocations that are not declarations we model.
    if head.endswith(")") and "=" not in head and not has_body and head.isupper():
        return None

    m = ALIAS_RE.match(head)
    if m:
        return Decl("ALIAS", m.group(1), normalize(m.group(2)), at(m.group(1)), path)

    m = TYPEDEF_RE.match(head)
    if m:
        return Decl("ALIAS", m.group(2), normalize(m.group(1)), at(m.group(2)), path)

    if has_body:
        m = ENUM_RE.search(head)
        if m:
            return Decl("ENUM", m.group(1), None, at(m.group(1)), path)
        m = RECORD_RE.search(head)
        if m:
            return Decl("RECORD", m.group(1), None, at(m.group(1)), path)

        m = FUNC_RE.search(head)
        if m and m.group(1) not in CONTROL_KEYWORDS:
            params = [normalize(p) for p in m.group(2).split(",") if normalize(p)]
            # Strip parameter NAMES: the signature is the types. `int Count`
            # and `int N` are the same overload and must collide.
            types = []
            for p in params:
                p = re.sub(r"\s*=\s*.*$", "", p)              # default argument
                p = re.sub(r"\b\w+\s*(\[[^\]]*\])?$", "", p)  # parameter name
                types.append(normalize(p) or "?")
            return Decl("FUNCTION", m.group(1), "(" + ",".join(types) + ")",
                        at(m.group(1)), path)
        return None

    # No body, ends in ';'. A variable definition if it names something and is
    # not a bare forward declaration or a function prototype.
    if head.startswith(("struct", "class", "union", "enum")):
        return None            # forward declaration: legal to repeat
    if "(" in head:
        return None            # prototype or macro: not modelled

    lhs = head.split("=", 1)[0]
    m = VAR_RE.search(normalize(lhs))
    if m and m.group(1) not in CONTROL_KEYWORDS and " " in normalize(lhs):
        return Decl("VARIABLE", m.group(1), head, at(m.group(1)), path)
    return None


NAMESPACE_HEAD_RE = re.compile(r"\bnamespace\b(?P<name>[\w\s:]*)$")


def top_level_decls(src: str, a: int, b: int, path: str):
    """Extract top-level declarations from src[a:b], plus nested namespaces.

    Returns (decls, nested) where nested is a list of (name, inner_a, inner_b)
    for each namespace opened directly at this level. `name` is "" for an
    anonymous one. The caller recurses, carrying the enclosing scope path --
    which is the whole point: `VoxelSky::<anon>::kDegToRad` and
    `<anon>::kDegToRad` are DIFFERENT names and do not collide, and a lint
    that conflated them would fire on VoxelEphemeris.cpp / VoxelSkyTests.cpp,
    where the duplicate is deliberate and correct.
    """
    decls = []
    nested = []
    i = a
    start = i
    while i < b:
        c = src[i]
        if c == "{":
            head = src[start:i]
            end = match_brace(src, i)
            m = NAMESPACE_HEAD_RE.search(normalize(head))
            if m:
                nested.append((m.group("name").strip(), i + 1, end - 1))
                i = start = end
                continue
            j = end
            while j < b and src[j] in " \t\r\n":
                j += 1
            if j < b and src[j] == ";":
                end = j + 1
            d = classify(head, True, line_of(src, start), path)
            if d:
                d.is_static = bool(re.search(r"\bstatic\b", head))
                decls.append(d)
            i = start = end
        elif c == ";":
            head = src[start:i]
            d = classify(head, False, line_of(src, start), path)
            if d:
                d.is_static = bool(re.search(r"\bstatic\b", head))
                decls.append(d)
            i += 1
            start = i
        elif c in "([":
            # Skip balanced parens/brackets so `foo(a, b);` is one statement
            # and a comma inside them never looks like a separator.
            close = {"(": ")", "[": "]"}[c]
            depth = 0
            while i < b:
                if src[i] == c:
                    depth += 1
                elif src[i] == close:
                    depth -= 1
                    if depth == 0:
                        break
                elif src[i] == "{":       # brace-init inside parens
                    i = match_brace(src, i) - 1
                i += 1
            i += 1
        else:
            i += 1
    return decls, nested


def walk_scope(src, a, b, path, scope, decls):
    """Collect every top-level declaration in src[a:b] and everything under it.

    `scope` is the enclosing namespace path, e.g. () at file scope,
    ("VoxelSky",) inside `namespace VoxelSky`, ("VoxelSky", "(anon)") inside an
    anonymous namespace nested in it. Nothing is filtered here: scan_file
    decides afterwards which of these are reachable by unqualified lookup at
    file scope, because that depends on the file's `using namespace`
    directives, which may appear after the declaration.
    """
    found, nested = top_level_decls(src, a, b, path)
    for d in found:
        d.scope = scope
        decls.append(d)
    for name, na, nb in nested:
        walk_scope(src, na, nb, path, scope + (name or "(anon)",), decls)


def scan_file(path: str):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        raw = fh.read()
    src = blank_comments_and_strings(raw)

    decls = []
    walk_scope(src, 0, len(src), path, (), decls)

    for m in LOG_CATEGORY_RE.finditer(src):
        d = Decl("LOG_CATEGORY", m.group(1), None, line_of(src, m.start()), path)
        d.scope = ()
        decls.append(d)

    # A file-scope `using namespace N;` for a namespace DEFINED IN THIS FILE
    # hoists N's names into unqualified lookup for the REST OF THE UNITY BLOB.
    # This is not a footnote -- it is how PR #210's defect actually fired, and
    # a lint that modelled only anonymous namespaces would miss it. A namespace
    # defined in a HEADER is deliberately not treated this way: two .cpp files
    # pulling in the same header namespace refer to one set of entities, which
    # is not a collision (VoxelCharacterMovement.cpp and VoxelWalkTestSubsystem
    # .cpp both do this with VoxelMovementTuning, correctly).
    defined_here = {d.scope[0] for d in decls if d.scope}
    injected = set()
    for m in USING_NS_RE.finditer(src):
        if src.count("{", 0, m.start()) == src.count("}", 0, m.start()):
            name = m.group(1)
            if name in defined_here:
                injected.add(name)

    kept = []
    for d in decls:
        if not d.scope:
            # True file scope. Only `static` has internal linkage; a
            # non-static definition out here is a link-level clash that the
            # linker already reports, unity or not.
            if not d.is_static:
                continue
            d.scope = (FILE_VISIBLE,)
        elif d.scope[0] == "(anon)" or d.scope[0] in injected:
            d.scope = (FILE_VISIBLE,) + d.scope[1:]
        elif not d.is_static:
            # A named namespace this file does not hoist. Qualified-only, so
            # it cannot be reached unqualified from elsewhere in the blob --
            # this is exactly the fix PR #210 applied, and the reason
            # VoxelEphemeris.cpp's `VoxelSky::(anon)::kDegToRad` does not
            # collide with VoxelSkyTests.cpp's own `kDegToRad`.
            continue
        kept.append(d)
    decls = kept

    # Annotations, keyed by the line they cover (own line + the one below).
    annotations = {}
    ann_errors = []
    for idx, line in enumerate(raw.splitlines(), 1):
        m = ANNOTATION_RE.search(line)
        if not m:
            continue
        reason = m.group(1).lstrip()
        reason = reason[1:].strip() if reason.startswith("-") else reason.strip()
        if len(reason) < MIN_REASON:
            ann_errors.append(
                f"{path}:{idx}: `lint-unity: allow` needs a reason of at least "
                f"{MIN_REASON} characters explaining why the duplicate is safe")
            continue
        annotations[idx] = [False]
        annotations[idx + 1] = annotations[idx]

    return decls, annotations, ann_errors


def module_name_for(root: str) -> str:
    for entry in sorted(os.listdir(root)):
        if entry.endswith(".Build.cs"):
            return entry[: -len(".Build.cs")]
    return os.path.basename(os.path.normpath(root))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("roots", nargs="*", default=None,
                    help="Unreal module directories (each holding a .Build.cs)")
    args = ap.parse_args()
    roots = args.roots or DEFAULT_ROOTS

    findings = []
    errors = []
    scanned = 0
    all_annotations = {}

    for root in roots:
        if not os.path.isdir(root):
            errors.append(f"{root}: not a directory")
            continue
        module = module_name_for(root)
        by_name = defaultdict(list)
        for dirpath, _dirs, files in os.walk(root):
            for fn in sorted(files):
                if not fn.endswith(".cpp"):
                    continue
                path = os.path.join(dirpath, fn).replace(os.sep, "/")
                scanned += 1
                decls, annotations, ann_errors = scan_file(path)
                errors.extend(ann_errors)
                all_annotations[path] = annotations
                for d in decls:
                    by_name[d.scope + (d.name,)].append(d)

        for qualified, group in sorted(by_name.items()):
            name = "::".join(qualified)
            per_file = defaultdict(list)
            for d in group:
                per_file[d.path].append(d)
            if len(per_file) < 2:
                continue
            # One representative per file: repeats within a file are the
            # compiler's problem today, not a unity regression.
            reps = [ds[0] for ds in per_file.values()]
            for i in range(len(reps)):
                for j in range(i + 1, len(reps)):
                    a, b = reps[i], reps[j]
                    if not collides(a, b):
                        continue
                    ann_a = all_annotations[a.path].get(a.line)
                    ann_b = all_annotations[b.path].get(b.line)
                    if ann_a or ann_b:
                        if ann_a:
                            ann_a[0] = True
                        if ann_b:
                            ann_b[0] = True
                        continue
                    findings.append(
                        f"{a.path}:{a.line}: {a.kind} `{name}` has internal "
                        f"linkage here and at {b.path}:{b.line}, in module "
                        f"{module}. Under a unity build both land in one "
                        f"translation unit and this is a redefinition."
                        + (f"\n    {a.path}:{a.line}: {a.sig}"
                           f"\n    {b.path}:{b.line}: {b.sig}"
                           if a.sig is not None else "")
                        + "\n    Fix: move them into per-file NAMED namespaces "
                          "(see VoxelSkyLadderFixture.cpp's "
                          "`namespace VoxelSkyLadderDetail`), or rename one.")

    for path, annotations in all_annotations.items():
        seen = set()
        for line, holder in sorted(annotations.items()):
            if id(holder) in seen:
                continue
            seen.add(id(holder))
            if not holder[0]:
                errors.append(
                    f"{path}:{line}: unused `lint-unity: allow` annotation. "
                    f"The collision it justified is gone -- delete the "
                    f"annotation so it cannot silently cover future code.")

    if findings or errors:
        if findings:
            print(f"unity internal-linkage collisions ({len(findings)}):\n")
            for f in findings:
                print(f + "\n")
        for e in errors:
            print(e)
        return 1

    print(f"lint-unity-collisions: clean ({scanned} .cpp files in "
          f"{len(roots)} module(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
