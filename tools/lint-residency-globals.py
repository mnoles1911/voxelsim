#!/usr/bin/env python3
"""Guard the VoxelResidencyScan.usf loose-global trap.

WHY THIS EXISTS
---------------
VoxelResidencyScan.usf declares its parameters as LOOSE GLOBALS at file scope,
shared by all six kernels, while the C++ side declares one FParameters struct
PER KERNEL. Nothing in the engine ties the two together: if a kernel reads a
global its own FParameters does not declare, the binding is simply absent and
the kernel reads garbage -- silently, with every log line still healthy.

That is not hypothetical. `MaxRingLevel` was missing from four of the five
parameter structs and shipped that way; the kernels that needed it read
whatever the constant buffer happened to hold. It was found by eye on
2026-08-23, not by any test.

This script closes that hole mechanically. It reads the .usf, works out which
globals each kernel's call graph actually touches, and holds that against the
FParameters struct the .cpp declares for it.

  python tools/lint-residency-globals.py

Exit 0 = clean. Exit 1 = at least one MISSING binding (a kernel reads a global
its struct does not declare). Unread globals and struct-only extras are
reported as warnings, not failures: an extra SHADER_PARAMETER is harmless, but
a global no kernel reads is one edit away from becoming the MaxRingLevel trap
again and is worth deleting.

The check is textual on purpose -- it must not need a shader compiler, a build,
or an editor, because the whole point is to run in the lane that has none of
those.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
USF = os.path.join(ROOT, 'ue-project', 'Shaders', 'VoxelResidencyScan.usf')
CPP = os.path.join(ROOT, 'ue-project', 'Source', 'VoxelEarthShaders', 'Private',
                   'VoxelResidencyGpu.cpp')

# Globals whose absence cannot be detected this way and which the engine binds
# itself. Empty today; kept so a future exemption is a deliberate, named line
# rather than a silent loosening of the check.
EXEMPT = set()

# A global declaration at file scope: an optional RW/StructuredBuffer<...> or a
# scalar/vector/matrix type, then a name, then ';'. Anything indented is a
# local, and anything with '(' is a function.
DECL = re.compile(
    r'^(?:RW)?(?:StructuredBuffer|Buffer|Texture2D|Texture3D|ByteAddressBuffer)\s*'
    r'(?:<[^>]*>)?\s+(\w+)\s*;'
    r'|^(?:uint|int|float|bool)[1-4]?(?:x[1-4])?\s+(\w+)\s*(?:\[\s*\d+\s*\])?\s*;',
    re.MULTILINE)

ENTRY = re.compile(r'^\[numthreads\([^)]*\)\]\s*\n^\w[\w\s]*?\b(\w+CS)\s*\(', re.MULTILINE)
FUNC = re.compile(r'^(?:\w[\w<>, ]*?)\s+(\w+)\s*\([^;]*?\)\s*\n\{', re.MULTILINE)


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def body_of(text, start):
    """Return the brace-balanced body that begins at or after `start`."""
    i = text.index('{', start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return text[i:]


def main():
    raw_usf = open(USF, encoding='utf-8').read()
    usf = strip_comments(raw_usf)
    cpp = strip_comments(open(CPP, encoding='utf-8').read())

    globals_ = []
    for m in DECL.finditer(usf):
        name = m.group(1) or m.group(2)
        if name and name not in globals_:
            globals_.append(name)
    if not globals_:
        print('FAIL: parsed no globals out of %s -- the lint itself is broken' % USF)
        return 1

    # Every non-entry function, so a global touched only through a helper
    # (AppendProposal, BinIndex, CellIndex, ...) still counts against the
    # kernel that calls it.
    entries = {m.group(1): m.start() for m in ENTRY.finditer(usf)}
    helpers = {}
    for m in FUNC.finditer(usf):
        name = m.group(1)
        if name in entries or name in ('main',):
            continue
        helpers[name] = body_of(usf, m.start())

    def touched(body, seen=None):
        """Globals read or written by `body`, following helper calls."""
        seen = seen if seen is not None else set()
        out = set()
        words = set(re.findall(r'\b(\w+)\b', body))
        for g in globals_:
            if g in words:
                out.add(g)
        for h, hbody in helpers.items():
            if h in words and h not in seen:
                seen.add(h)
                out |= touched(hbody, seen)
        return out

    # class FVoxelResidencyFooCS ... BEGIN_SHADER_PARAMETER_STRUCT ... END
    structs = {}
    for m in re.finditer(r'class\s+FVoxelResidency(\w+)CS\s*:', cpp):
        cls = m.group(1)
        blk = cpp[m.start():]
        b = blk.find('BEGIN_SHADER_PARAMETER_STRUCT')
        e = blk.find('END_SHADER_PARAMETER_STRUCT')
        if b < 0 or e < 0:
            continue
        decls = set(re.findall(r'SHADER_PARAMETER\w*\([^,]+,\s*(\w+)', blk[b:e]))
        structs[cls + 'CS'] = decls

    # Entry-point name in IMPLEMENT_GLOBAL_SHADER is the authority for pairing.
    pairs = {}
    for m in re.finditer(r'IMPLEMENT_GLOBAL_SHADER\(\s*(FVoxelResidency(\w+)CS)\s*,'
                         r'[^,]+,\s*"(\w+)"', cpp):
        pairs[m.group(3)] = m.group(2) + 'CS'

    fails = 0
    print('globals in %s: %d' % (os.path.basename(USF), len(globals_)))
    used_anywhere = set()

    for kernel in sorted(entries):
        body = body_of(usf, entries[kernel])
        need = touched(body) - EXEMPT
        used_anywhere |= need
        cls = pairs.get(kernel)
        if cls is None:
            print('FAIL %-18s no IMPLEMENT_GLOBAL_SHADER names this entry point' % kernel)
            fails += 1
            continue
        have = structs.get(cls)
        if have is None:
            print('FAIL %-18s no FParameters struct found for %s' % (kernel, cls))
            fails += 1
            continue
        missing = sorted(need - have)
        extra = sorted(have - need)
        if missing:
            print('FAIL %-18s reads but does NOT declare: %s' % (kernel, ', '.join(missing)))
            fails += 1
        else:
            print('ok   %-18s %2d globals, all declared' % (kernel, len(need)))
        if extra:
            print('     warn: declared but unread (harmless): %s' % ', '.join(extra))

    orphans = [g for g in globals_ if g not in used_anywhere]
    if orphans:
        print('     warn: declared in the .usf and read by NO kernel: %s' % ', '.join(orphans))
        print('           delete them -- an unread loose global is the trap in waiting.')

    print('FAIL: %d kernel(s) with missing bindings' % fails if fails else 'PASS')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
