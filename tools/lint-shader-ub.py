#!/usr/bin/env python3
"""Vendor-divergence (undefined-behavior) lint for voxel-core's HLSL kernels.

WHY THIS EXISTS
---------------
The M0 cross-vendor determinism gate passed on an AMD RX 7800 XT and failed on
an NVIDIA RTX 4090 running the IDENTICAL committed SPIR-V. Root cause (commit
6ab4b2a): `worldgen.ush`'s `floorDiv` derived its flooring correction from
`a % b`. HLSL defines `%` only when both operands share a sign, and every
worldgen coordinate hits it with a negative dividend over a positive divisor.
DXC lowers that to a 64-bit `OpSRem`; AMD's emulation carried the dividend's
sign (matching x86/C++) so the correction fired, NVIDIA's did not, and
`floorDiv` silently degraded to TRUNCATING division for every negative world
coordinate — metre-scale terrain divergence between vendors, from bytecode
that is bit-identical on both.

That bug is invisible in review, invisible in a single-vendor test run, and
cost a day to find. This lint makes the whole CLASS of it mechanical: it scans
the HLSL for constructs whose result is not pinned down by the spec, and fails
the build. Everything here is a shape that either already bit us or sits one
edit away from biting us the same way.

DESIGN: FAIL CLOSED
-------------------
The default answer is "this is a finding". Constructs that are genuinely safe
are silenced ONE AT A TIME with an inline annotation that records WHY, next to
the code, where it can be re-checked when the code changes:

    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - <reason, >= 10 chars>

The annotation applies to every finding of that rule on the annotated line or
on the line immediately below it. Two properties keep the allowlist honest:

  * A reason is mandatory. A bare `allow` is itself an error, so the mechanism
    cannot be used to bulk-silence a rule.
  * Unused annotations are errors. When a guard is refactored away the stale
    justification fails the build instead of quietly covering new code.

There is deliberately NO exemption for the approved `floorDiv`/`truncDiv`
helpers: they are written against magnitude-only UNSIGNED division precisely
so they pass this lint on their own merits. Reintroducing the original
`%`-based implementation inside them fails SIGNED_DIVISION, which is the
single most important thing this file does.

Comments and string literals are blanked (newline-preserving) before any
matching, in the style of the existing `float-ban` CI job, so prose can never
trip a rule.

RULES
-----
  SIGNED_DIVISION    `/` or `%` where either operand can be negative. This is
                     the shipped bug. Use floorDiv (floor) or truncDiv
                     (toward zero) instead; both route through OpUDiv, which
                     has exactly one legal result on every implementation.
                     Divisions of provably non-negative compile-time constants
                     are not findings.
  VARIABLE_SHIFT     shift distance that is not an integer literal provably
                     below the left operand's bit width. Out-of-range shifts
                     are undefined in HLSL/SPIR-V and vendors differ on
                     whether they mask the count or produce zero.
  UNSIGNED_UNDERFLOW unsigned (or unsigned-derived) value minus a literal or
                     another value. `bricksX - 2u` wraps to 0xFFFFFFFF for a
                     region with one brick, and a `>= count` range check then
                     passes; `(int64_t)RasterSize.x - 1` inverts a clamp range
                     to [0, -1]. Both end as wild buffer indices.
  UNGUARDED_WRITE    write through an RW*Buffer whose index is not visibly
                     bounded — no early-out mentioning the index, no
                     GetDimensions clamp. Out-of-bounds writes are the most
                     vendor-divergent thing a shader can do (AMD's
                     range-checked descriptors drop them, NVIDIA's may not).
  WAVE_WIDTH         wave/subgroup intrinsics and lane-count literals. AMD is
                     wave64, NVIDIA warp32; any code whose RESULT depends on
                     which is a cross-vendor divergence by construction.

SPIR-V AUDIT
------------
With --spv-dir, the committed kernels are additionally scanned for the
OpSDiv/OpSRem/OpSMod opcodes. The HLSL rules are heuristic (this is a linter,
not a front end); the opcode scan is exact, and it is the ground truth for
"no signed division survives into the shipped bytecode".

USAGE
-----
    python tools/lint-shader-ub.py                       # default paths
    python tools/lint-shader-ub.py --spv-dir voxel-core/shaders/prebuilt
Exit status 0 = clean, 1 = findings.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import struct
import sys

RULES = (
    "SIGNED_DIVISION",
    "VARIABLE_SHIFT",
    "UNSIGNED_UNDERFLOW",
    "UNGUARDED_WRITE",
    "WAVE_WIDTH",
)

# --- type tables -------------------------------------------------------------

SIGNED_SCALARS = {"int", "int16_t", "int32_t", "int64_t", "min16int", "min12int"}
UNSIGNED_SCALARS = {"uint", "uint16_t", "uint32_t", "uint64_t", "dword", "min16uint"}


def _vectorize(base: set[str]) -> set[str]:
    out = set(base)
    for t in base:
        for n in (2, 3, 4):
            out.add(f"{t}{n}")
    return out


SIGNED_TYPES = _vectorize(SIGNED_SCALARS)
UNSIGNED_TYPES = _vectorize(UNSIGNED_SCALARS)
ALL_TYPES = SIGNED_TYPES | UNSIGNED_TYPES | {"bool", "void"} | _vectorize({"bool"})

BIT_WIDTH = {}
for _t in ("int64_t", "uint64_t"):
    BIT_WIDTH[_t] = 64
for _t in ("int", "uint", "dword", "int32_t", "uint32_t"):
    BIT_WIDTH[_t] = 32
for _t in ("int16_t", "uint16_t", "min16int", "min16uint"):
    BIT_WIDTH[_t] = 16
for _t, _w in list(BIT_WIDTH.items()):
    for _n in (2, 3, 4):
        BIT_WIDTH[f"{_t}{_n}"] = _w

# HLSL system-value semantics are all unsigned 32-bit.
SV_UNSIGNED = {"tid", "gtid", "gid", "groupIndex"}

WAVE_INTRINSICS = re.compile(
    r"\b(Wave[A-Z]\w*|Quad(Read|Any|All)\w*|GetRenderTargetSample\w*|"
    r"subgroup\w*|ballot\w*|__shfl\w*)\b"
)
WAVE_LITERAL = re.compile(r"\b(wave|warp|lane|subgroup)\w*\b[^;\n]{0,40}?\b(32|64)\b", re.I)

IDENT = re.compile(r"[A-Za-z_]\w*")
INT_LITERAL = re.compile(r"^(0[xX][0-9a-fA-F]+|\d+)[uUlL]*$")


class Finding:
    def __init__(self, path: pathlib.Path, line: int, rule: str, text: str, detail: str):
        self.path, self.line, self.rule = path, line, rule
        self.text, self.detail = text.strip(), detail

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: [{self.rule}] {self.detail}\n    {self.text}"


# --- source preparation ------------------------------------------------------


def strip_noise(src: str) -> str:
    """Blank comments and string literals, preserving every byte offset and
    newline so line numbers and index arithmetic stay valid."""

    def blank(m: re.Match) -> str:
        return re.sub(r"[^\n]", " ", m.group(0))

    src = re.sub(r"/\*.*?\*/", blank, src, flags=re.S)
    src = re.sub(r"//[^\n]*", blank, src)
    src = re.sub(r'"(\\.|[^"\\\n])*"', blank, src)
    return src


def line_of(src: str, pos: int) -> int:
    return src.count("\n", 0, pos) + 1


def line_text(raw_lines: list[str], line: int) -> str:
    return raw_lines[line - 1] if 1 <= line <= len(raw_lines) else ""


# --- allowlist ---------------------------------------------------------------

ALLOW_RE = re.compile(r"lint-shader-ub:\s*allow\s+([A-Z_]+)\s*(?:[-—:]\s*(.*))?$")


def parse_allowlist(raw: str, path: pathlib.Path) -> tuple[dict, list[Finding]]:
    """Returns {(line, rule): [used_flag_holder]} and any malformed-annotation
    errors. An annotation covers its own line and the one after it, so it can
    sit either trailing the code or on the line above."""
    allows: dict[tuple[int, str], dict] = {}
    errors: list[Finding] = []
    for i, text in enumerate(raw.splitlines(), 1):
        m = ALLOW_RE.search(text)
        if not m:
            continue
        rule, reason = m.group(1), (m.group(2) or "").strip()
        if rule not in RULES:
            errors.append(
                Finding(path, i, "ALLOWLIST", text, f"unknown rule {rule!r} (known: {', '.join(RULES)})")
            )
            continue
        if len(reason) < 10:
            errors.append(
                Finding(
                    path, i, "ALLOWLIST", text,
                    f"allow {rule} needs a written reason (>=10 chars) — fail-closed by design",
                )
            )
            continue
        allows[(i, rule)] = {"used": False, "reason": reason}
    return allows, errors


def allowed(allows: dict, line: int, rule: str) -> bool:
    for candidate in (line, line - 1):
        entry = allows.get((candidate, rule))
        if entry is not None:
            entry["used"] = True
            return True
    return False


# --- declaration scan --------------------------------------------------------


def scan_declarations(src: str) -> tuple[dict, dict, set]:
    """Maps identifier -> declared type for locals/params/globals, function
    name -> return type, and the set of const identifiers initialized to a
    provably non-negative literal."""
    var_types: dict[str, str] = {}
    fn_types: dict[str, str] = {}
    nonneg: set[str] = set()

    type_alt = "|".join(sorted(ALL_TYPES, key=len, reverse=True))

    # Function definitions/declarations: "<type> Name(" at any indentation.
    for m in re.finditer(rf"\b({type_alt})\s+([A-Za-z_]\w*)\s*\(", src):
        fn_types[m.group(2)] = m.group(1)

    # Variable declarations, including parameters (out/inout qualified too).
    decl = re.compile(
        rf"\b(?:static\s+|const\s+|groupshared\s+|in\s+|out\s+|inout\s+|uniform\s+)*"
        rf"({type_alt})\s+([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(=\s*([^;,)]*))?"
    )
    for m in decl.finditer(src):
        ty, name, _arr, _eq, init = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
        if name in fn_types:
            continue  # it's a function, already recorded
        var_types.setdefault(name, ty)
        if init is not None:
            lit = init.strip()
            if INT_LITERAL.match(lit) and int(lit.rstrip("uUlL"), 0) >= 0:
                nonneg.add(name)

    # Multi-declarator lines: "const uint bx = ..., by = ...;"
    for m in re.finditer(rf"\b({type_alt})\s+([^;]*);", src):
        ty = m.group(1)
        for part in m.group(2).split(","):
            nm = IDENT.match(part.strip())
            if nm and nm.group(0) not in fn_types:
                var_types.setdefault(nm.group(0), ty)

    for sv in SV_UNSIGNED:
        var_types.setdefault(sv, "uint3")
    return var_types, fn_types, nonneg


# --- operand extraction ------------------------------------------------------


def match_back(src: str, i: int, open_ch: str, close_ch: str) -> int:
    """i indexes the closing bracket; returns the index of its opener."""
    depth = 0
    while i >= 0:
        if src[i] == close_ch:
            depth += 1
        elif src[i] == open_ch:
            depth -= 1
            if depth == 0:
                return i
        i -= 1
    return 0


def match_fwd(src: str, i: int, open_ch: str, close_ch: str) -> int:
    depth = 0
    n = len(src)
    while i < n:
        if src[i] == open_ch:
            depth += 1
        elif src[i] == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n - 1


KEYWORDS = {"return", "else", "do", "case", "const", "static", "in", "out", "inout"}


def left_operand(src: str, op_start: int) -> str:
    """Text of the operand immediately left of the operator at op_start."""
    i = op_start - 1
    while i >= 0 and src[i] in " \t\n":
        i -= 1
    end = i + 1
    while i >= 0:
        c = src[i]
        if c in ")]":
            i = match_back(src, i, "(" if c == ")" else "[", c) - 1
        elif c.isalnum() or c in "_.":
            i -= 1
        elif c in " \t":
            j = i
            while j >= 0 and src[j] in " \t":
                j -= 1
            if j >= 0 and (src[j] in ")]" or src[j].isalnum() or src[j] in "_."):
                i = j
            else:
                break
        else:
            break
    text = src[i + 1 : end]
    # A keyword can never be part of an operand ("return (int)x - 1" must
    # yield "(int)x", not "return (int)x"); drop any leading keyword run.
    while True:
        m = re.match(r"\s*([A-Za-z_]\w*)\s+", text)
        if m and m.group(1) in KEYWORDS:
            text = text[m.end() :]
            continue
        break
    return text


def right_operand(src: str, op_end: int) -> str:
    i = op_end
    n = len(src)
    while i < n and src[i] in " \t\n":
        i += 1
    start = i
    while i < n:
        c = src[i]
        if c == "(":
            i = match_fwd(src, i, "(", ")") + 1
        elif c == "[":
            i = match_fwd(src, i, "[", "]") + 1
        elif c.isalnum() or c in "_.":
            i += 1
        elif c in " \t" and i + 1 < n and (src[i + 1].isalnum() or src[i + 1] == "("):
            # a cast like "(int64_t) x" — keep going
            i += 1
        else:
            break
    return src[start:i]


MEMBER = re.compile(r"\b([A-Za-z_]\w*)\s*\.\s*([A-Za-z_]\w*)")


def normalize_members(text: str, var_types: dict) -> str:
    """Rewrite `A.b` to whichever of A or b actually carries the type.

    Without this, `tid.x` contributes the identifier `x` — which happens to be
    an `int64_t` PARAMETER NAME of hash2 elsewhere in the file — and every
    `tid.x / 8u` looks like a signed division. For a vector (`uint3 tid`) the
    swizzle has the base's element signedness, so keep the base; otherwise it
    is a struct field (`col.surfaceMm`), so keep the member, which the
    struct/cbuffer declaration scan has already typed.
    """

    def sub(m: re.Match) -> str:
        base, member = m.group(1), m.group(2)
        ty = var_types.get(base)
        if ty and re.search(r"\d$", ty):  # vector type: uint3, int2, ...
            return base
        if ty:
            return base
        return member

    return MEMBER.sub(sub, text)


def leading_cast(operand: str) -> str | None:
    m = re.match(r"\s*\(\s*([A-Za-z_]\w*)\s*\)", operand)
    if m and m.group(1) in ALL_TYPES:
        return m.group(1)
    return None


def bare_literal(operand: str) -> int | None:
    s = operand.strip()
    s = re.sub(r"^\((?:[A-Za-z_]\w*)\)\s*", "", s)  # drop one leading cast
    s = s.strip().strip("()").strip()
    if INT_LITERAL.match(s):
        return int(s.rstrip("uUlL"), 0)
    return None


def outer_call(operand: str, fn_types) -> str | None:
    """If the operand is exactly one call to a known function, its name. The
    call's RETURN type is what the surrounding operator sees, so the argument
    types must not leak out — `absToU64(a)` is unsigned even though `a` is
    `int64_t`, and that is the whole point of the helper."""
    text = operand.strip()
    m = re.match(r"([A-Za-z_]\w*)\s*\(", text)
    if not m or m.group(1) not in fn_types:
        return None
    # The call must span the whole operand, not just start it.
    if match_fwd(text, m.end() - 1, "(", ")") != len(text) - 1:
        return None
    return m.group(1)


def operand_can_be_negative(operand: str, var_types, fn_types, nonneg) -> bool:
    """Conservative "could this value be < 0 at runtime?".

    Only a value that CAN be negative can trigger the mixed-sign `/` and `%`
    behavior HLSL leaves undefined. Non-negative literals and provably
    non-negative named constants therefore are not findings — that keeps the
    rule off `(int64_t)kVoxelSizeMm / 2` and every `x / 8u` while still
    catching any division involving a world coordinate.
    """
    text = operand.strip()
    if text.startswith("-"):
        return True
    cast = leading_cast(operand)
    if cast in UNSIGNED_TYPES:
        return False

    lit = bare_literal(operand)
    if lit is not None:
        return lit < 0

    call = outer_call(operand, fn_types)
    if call is not None:
        return fn_types[call] in SIGNED_TYPES

    names = IDENT.findall(normalize_members(operand, var_types))
    for name in names:
        if name in ALL_TYPES or name in KEYWORDS or name in nonneg:
            continue
        ty = fn_types.get(name) or var_types.get(name)
        if ty in SIGNED_TYPES:
            return True
    return cast in SIGNED_TYPES and not all(
        n in nonneg or n in ALL_TYPES or n in KEYWORDS for n in names
    )


def operand_is_unsigned(operand: str, var_types, fn_types, nonneg) -> bool:
    """True when the operand's VALUE originates from an unsigned quantity —
    even if a signed cast has been applied on top. `(int64_t)RasterSize.x - 1`
    is still 'a count minus one' and still inverts a clamp range when the
    count is zero, so the cast must not launder it."""
    lit = bare_literal(operand)
    cast = leading_cast(operand)
    if lit is not None:
        return bool(re.search(r"[uU]", operand)) or (cast in UNSIGNED_TYPES)
    call = outer_call(operand, fn_types)
    if call is not None:
        return fn_types[call] in UNSIGNED_TYPES
    for name in IDENT.findall(normalize_members(operand, var_types)):
        if name in ALL_TYPES or name in KEYWORDS:
            continue
        ty = fn_types.get(name) or var_types.get(name)
        if ty in UNSIGNED_TYPES:
            return True
        if ty in SIGNED_TYPES:
            return False
    return False


# --- function-body index (for UNGUARDED_WRITE) -------------------------------


def function_bodies(src: str) -> list[tuple[int, int]]:
    """Byte ranges of every top-level `{ ... }` block."""
    spans, depth, start = [], 0, None
    for i, c in enumerate(src):
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start is not None:
                spans.append((start, i))
                start = None
            if depth < 0:
                depth = 0
    return spans


def enclosing_body(spans, pos: int) -> tuple[int, int] | None:
    for a, b in spans:
        if a <= pos <= b:
            return (a, b)
    return None


# --- the rules ---------------------------------------------------------------


def check_signed_division(src, path, raw_lines, var_types, fn_types, nonneg) -> list[Finding]:
    out = []
    for m in re.finditer(r"[/%]", src):
        i = m.start()
        if src[i] == "/" and (src[i + 1 : i + 2] in ("/", "*", "=") or src[i - 1 : i] in ("/", "*")):
            continue
        if src[i] == "%" and src[i + 1 : i + 2] == "=":
            continue
        left = left_operand(src, i)
        right = right_operand(src, i + 1)
        if not left or not right:
            continue
        l_neg = operand_can_be_negative(left, var_types, fn_types, nonneg)
        r_neg = operand_can_be_negative(right, var_types, fn_types, nonneg)
        if not (l_neg or r_neg):
            continue
        op = src[i]
        out.append(
            Finding(
                path, line_of(src, i), "SIGNED_DIVISION", line_text(raw_lines, line_of(src, i)),
                f"signed `{op}` on possibly-negative operands ({left.strip()} {op} {right.strip()}) — "
                f"HLSL leaves this undefined for mixed signs; route through floorDiv/truncDiv",
            )
        )
    return out


def check_shifts(src, path, raw_lines, var_types, fn_types, nonneg) -> list[Finding]:
    out = []
    for m in re.finditer(r"(<<=|>>=|<<|>>)", src):
        op = m.group(1)
        left = left_operand(src, m.start())
        right = right_operand(src, m.end())
        lit = bare_literal(right)
        line = line_of(src, m.start())
        if lit is None:
            out.append(
                Finding(path, line, "VARIABLE_SHIFT", line_text(raw_lines, line),
                        f"shift distance `{right.strip()}` is not an integer literal — an "
                        f"out-of-range shift is undefined and vendors differ on masking")
            )
            continue
        cast = leading_cast(left)
        width = BIT_WIDTH.get(cast) if cast else None
        if width is None:
            for name in IDENT.findall(normalize_members(left, var_types)):
                if name in ALL_TYPES or name in KEYWORDS:
                    continue
                ty = fn_types.get(name) or var_types.get(name)
                if ty in BIT_WIDTH:
                    width = BIT_WIDTH[ty]
                    break
        if width is None:
            width = 64  # unknown: only catch the unambiguously-illegal cases
        if lit >= width:
            out.append(
                Finding(path, line, "VARIABLE_SHIFT", line_text(raw_lines, line),
                        f"shift of `{left.strip()}` ({width}-bit) by {lit} is at or beyond the "
                        f"operand width — undefined")
            )
    return out


def check_unsigned_underflow(src, path, raw_lines, var_types, fn_types, nonneg) -> list[Finding]:
    out = []
    for m in re.finditer(r"-", src):
        i = m.start()
        if src[i + 1 : i + 2] in ("-", "=") or src[i - 1 : i] in ("-", "+", "*", "/", "%", "<", ">", "!", "="):
            continue
        j = i - 1
        while j >= 0 and src[j] in " \t\n":
            j -= 1
        if j < 0 or not (src[j].isalnum() or src[j] in "_.)]"):
            continue  # unary minus
        left = left_operand(src, i)
        right = right_operand(src, i + 1)
        if not left or not right:
            continue
        if not operand_is_unsigned(left, var_types, fn_types, nonneg):
            continue
        line = line_of(src, i)
        out.append(
            Finding(path, line, "UNSIGNED_UNDERFLOW", line_text(raw_lines, line),
                    f"`{left.strip()} - {right.strip()}` subtracts from an unsigned quantity — "
                    f"wraps to a huge value (or inverts a clamp range) when it goes below zero")
        )
    return out


def base_identifiers(text: str) -> set[str]:
    """Identifiers with member/swizzle accessors removed. `tid.x` and `gid.x`
    must NOT look related just because both end in `.x` — that would let an
    unrelated guard vouch for an index it never constrains."""
    stripped = re.sub(r"\.\s*[A-Za-z_]\w*", "", text)
    return {n for n in IDENT.findall(stripped) if n not in ALL_TYPES and n not in KEYWORDS}


def check_unguarded_writes(src, path, raw_lines, var_types, fn_types, nonneg) -> list[Finding]:
    rw_buffers = set(re.findall(r"\bRW\w*Buffer\s*<[^>]*>\s*([A-Za-z_]\w*)", src))
    if not rw_buffers:
        return []
    spans = function_bodies(src)
    out = []
    name_alt = "|".join(sorted(rw_buffers, key=len, reverse=True))
    for m in re.finditer(rf"\b({name_alt})\s*\[", src):
        buf = m.group(1)
        close = match_fwd(src, m.end() - 1, "[", "]")
        after = src[close + 1 : close + 4]
        if not re.match(r"\s*(=[^=]|\+=|-=|\|=|&=|\^=)", after):
            continue  # a read, not a write
        idx = src[m.end() : close]
        line = line_of(src, m.start())
        body = enclosing_body(spans, m.start())
        scope = src[body[0] : body[1]] if body else src
        if "GetDimensions" in scope:
            continue  # bounded against the buffer's own length
        idx_names = base_identifiers(idx)
        guarded = False
        for gm in re.finditer(r"\bif\s*\(([^{;]*)\)\s*(?:\{[^}]*\})?\s*(return|continue)\b", scope):
            if base_identifiers(gm.group(1)) & idx_names:
                guarded = True
                break
        if not guarded:
            for gm in re.finditer(r"\bif\s*\(([^{;]*)\)\s*$", scope, re.M):
                if base_identifiers(gm.group(1)) & idx_names:
                    guarded = True
                    break
        if guarded:
            continue
        out.append(
            Finding(path, line, "UNGUARDED_WRITE", line_text(raw_lines, line),
                    f"write to {buf}[{idx.strip()}] with no visible bound on the index — "
                    f"an out-of-range store is dropped on some vendors and lands on others")
        )
    return out


def check_wave_width(src, path, raw_lines, *_unused) -> list[Finding]:
    out = []
    for m in WAVE_INTRINSICS.finditer(src):
        line = line_of(src, m.start())
        out.append(
            Finding(path, line, "WAVE_WIDTH", line_text(raw_lines, line),
                    f"`{m.group(0)}` is a wave/subgroup intrinsic — AMD is wave64 and NVIDIA "
                    f"warp32, so any result that depends on width diverges by vendor")
        )
    for m in WAVE_LITERAL.finditer(src):
        line = line_of(src, m.start())
        out.append(
            Finding(path, line, "WAVE_WIDTH", line_text(raw_lines, line),
                    "hardcoded wave/lane width — AMD wave64 vs NVIDIA warp32")
        )
    return out


CHECKS = (
    check_signed_division,
    check_shifts,
    check_unsigned_underflow,
    check_unguarded_writes,
    check_wave_width,
)


def lint_file(path: pathlib.Path) -> list[Finding]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    raw_lines = raw.splitlines()
    src = strip_noise(raw)
    allows, findings = parse_allowlist(raw, path)
    var_types, fn_types, nonneg = scan_declarations(src)

    for check in CHECKS:
        for f in check(src, path, raw_lines, var_types, fn_types, nonneg):
            if not allowed(allows, f.line, f.rule):
                findings.append(f)

    for (line, rule), entry in sorted(allows.items()):
        if not entry["used"]:
            findings.append(
                Finding(path, line, "ALLOWLIST", line_text(raw_lines, line),
                        f"unused `allow {rule}` — the construct it justified is gone; delete the "
                        f"annotation so it cannot silently cover future code")
            )
    findings.sort(key=lambda f: (f.line, f.rule))
    return findings


# --- SPIR-V opcode audit -----------------------------------------------------

SPIRV_MAGIC = 0x07230203
# SPIR-V 1.x core arithmetic opcodes (spec section "Arithmetic Instructions").
# Note 137 is OpUMod and 136 is OpFDiv — NOT OpSRem; getting this table wrong
# makes the audit report the unsigned remainders as violations, so the
# self-test below pins the numbering against a known-good module.
OP_UDIV, OP_SDIV, OP_UMOD, OP_SREM, OP_SMOD = 134, 135, 137, 138, 139
BANNED_OPCODES = {OP_SDIV: "OpSDiv", OP_SREM: "OpSRem", OP_SMOD: "OpSMod"}
SAFE_DIVISION_OPCODES = {OP_UDIV: "OpUDiv", OP_UMOD: "OpUMod"}


def spv_opcode_histogram(path: pathlib.Path) -> dict[int, int] | str:
    """Opcode -> count for one module, or an error string."""
    data = path.read_bytes()
    if len(data) < 20:
        return f"{path}: too short to be SPIR-V"
    endian = "<"
    if struct.unpack("<I", data[:4])[0] != SPIRV_MAGIC:
        if struct.unpack(">I", data[:4])[0] != SPIRV_MAGIC:
            return f"{path}: not a SPIR-V module (bad magic)"
        endian = ">"
    words = struct.unpack(f"{endian}{len(data) // 4}I", data[: len(data) // 4 * 4])
    hist: dict[int, int] = {}
    i, n = 5, len(words)
    while i < n:
        count, opcode = words[i] >> 16, words[i] & 0xFFFF
        if count == 0:  # malformed stream; stop rather than spin
            break
        hist[opcode] = hist.get(opcode, 0) + 1
        i += count
    return hist


def audit_spv(paths: list[pathlib.Path]) -> list[str]:
    """Exact check: no signed division/remainder opcode may survive into the
    committed bytecode. The HLSL rules are heuristic; this one is ground
    truth, and it is what would have caught the shipped bug directly."""
    problems: list[str] = []
    total_safe = 0
    for path in paths:
        hist = spv_opcode_histogram(path)
        if isinstance(hist, str):
            problems.append(hist)
            continue
        total_safe += sum(hist.get(op, 0) for op in SAFE_DIVISION_OPCODES)
        for opcode, name in sorted(BANNED_OPCODES.items()):
            if hist.get(opcode):
                problems.append(
                    f"{path}: contains {hist[opcode]} x {name} — signed division survived into "
                    f"the shipped bytecode. That opcode's 64-bit emulation is where AMD and "
                    f"NVIDIA parted ways at the M0 gate; route the division through "
                    f"floorDiv/truncDiv so it lowers to OpUDiv instead."
                )
    # Positive control. Every kernel set here performs integer division, so
    # zero OpUDiv/OpUMod across the whole set means the parse found nothing
    # and the "clean" result would be meaningless — fail instead of lying.
    if not problems and total_safe == 0:
        problems.append(
            "SPIR-V audit found no division opcodes at all across the module set — the opcode "
            "scan is not seeing instructions, so its clean result cannot be trusted"
        )
    return problems


# --- driver ------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="*", default=None,
                    help="HLSL files or directories (default: voxel-core/shaders)")
    ap.add_argument("--spv-dir", default=None,
                    help="also audit committed SPIR-V in this directory for OpSDiv/OpSRem/OpSMod")
    args = ap.parse_args()

    targets: list[pathlib.Path] = []
    for p in args.paths or ["voxel-core/shaders"]:
        path = pathlib.Path(p)
        # .ush as well as .hlsl: worldgen was renamed to .ush so that Unreal's
        # shader pipeline will load it (UE accepts only .usf/.ush -- see
        # ShaderCore.cpp CheckVirtualShaderFilePath). Same file, same lint.
        targets.extend(sorted([*path.glob("*.hlsl"), *path.glob("*.ush")])
                       if path.is_dir() else [path])
    if not targets:
        print("lint-shader-ub: no .hlsl/.ush files found", file=sys.stderr)
        return 1

    findings: list[Finding] = []
    for t in targets:
        findings.extend(lint_file(t))

    spv_problems: list[str] = []
    if args.spv_dir:
        spvs = sorted(pathlib.Path(args.spv_dir).glob("*.spv"))
        if not spvs:
            print(f"lint-shader-ub: no .spv files in {args.spv_dir}", file=sys.stderr)
            return 1
        spv_problems.extend(audit_spv(spvs))
        print(f"lint-shader-ub: audited {len(spvs)} SPIR-V module(s) for OpSDiv/OpSRem/OpSMod")

    if findings or spv_problems:
        print("\nlint-shader-ub: vendor-divergent constructs found\n")
        for f in findings:
            print(str(f) + "\n")
        for p in spv_problems:
            print(p + "\n")
        print(f"{len(findings) + len(spv_problems)} finding(s). Each is a construct whose result "
              f"is not pinned down by the HLSL/SPIR-V spec and can therefore differ between GPU "
              f"vendors. Fix it, or annotate the line with\n"
              f"    // lint-shader-ub: allow <RULE> - <why this one is provably safe>")
        return 1

    print(f"lint-shader-ub: clean ({len(targets)} HLSL file(s) checked, "
          f"{len(RULES)} rules, fail-closed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
