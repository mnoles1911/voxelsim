"""The Claude lane: natural language -> a PATCH DICT, never a spec.

Owner ruling (2026-09-05): natural-language generation instructions run on
the owner's Claude subscription. The backend is the Claude Code CLI in print
mode (`claude -p`) as a subprocess -- it authenticates against the owner's
existing login, so there is NO API key, no SDK and no new dependency in this
repo. One call per user action.

THE CONTRACT, and why it cannot break the library:

  * The model is asked for `{"changes": {<schema path>: value}, "note": str}`
    and NOTHING it says is trusted: the CLI cannot enforce an output schema,
    so the response is post-validated here. A reply that is not JSON, or
    whose `changes` is not a flat dict, is rejected with the reason -- the
    caller falls back to the local grammar (`language.interpret`), which is
    always available and always offline.
  * Every accepted path must exist in `spec.PARAMS` (unknown paths are
    dropped BY NAME into the report), and values land exclusively through
    `spec.patch`, so they are clamped to the slider ranges and an
    out-of-menu choice trips the substitution alarm (`spec.py`'s "BUILT AND
    DRAWN AS" warning) exactly as a hand edit would. The model can never add
    a key `validate` does not own, so the hash discipline is untouched.
  * Provenance rides in the spec's `notes` field (hash-excluded): what was
    asked, which model, when. The verdict on the result stays where it
    always was -- with the human looking at the render.

The UI labels this lane as leaving the machine; the local lanes keep their
"nothing leaves this machine" label. That promise became per-panel by the
same ruling.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from datetime import date

from . import language, spec as specmod

# The one config constant. A translation task, not a reasoning task, so the
# sonnet-class alias is the default; the CLI resolves it to the current
# model of that class.
MODEL = "sonnet"

TIMEOUT_S = 120

# Tests override this to a stub executable so no probe run ever spends a
# real subscription call; see tools/llmprobe.py.
CMD_ENV = "ASSET_FORGE_CLAUDE_CMD"


def _claude_path() -> str | None:
    override = os.environ.get(CMD_ENV)
    if override:
        return override
    # shutil.which resolves claude.cmd/.exe on Windows, which a bare
    # subprocess argv[0] would not.
    return shutil.which("claude")


def _prompt(spec: dict, text: str) -> str:
    kind = specmod.get(spec, "kind")
    rows = []
    for p in specmod.ui_schema(kind):
        cur = specmod.get(spec, p["path"])
        if p["kind"] == "choice":
            rows.append(f"  {p['path']} = {cur!r}  (choice of {p['choices']})")
        else:
            rows.append(f"  {p['path']} = {cur}  ({p['kind']} in "
                        f"[{p['lo']}, {p['hi']}], {p['label']})")
    return (
        "You translate a plain-English request about a procedural voxel "
        f"asset (a {kind}) into parameter changes.\n"
        "Current parameters, with their legal ranges:\n"
        + "\n".join(rows) + "\n\n"
        f"Request: {text}\n\n"
        "Reply with ONLY a JSON object, no prose, no code fences:\n"
        '{"changes": {"<parameter path>": <value>, ...}, '
        '"note": "<one short sentence on what you changed and why>"}\n'
        "Use only parameter paths from the list above. Change only what the "
        "request asks for. If the request asks for nothing that these "
        'parameters express, reply {"changes": {}, "note": "<why not>"}.'
    )


def _extract_json(text: str) -> dict | None:
    """The reply's JSON object, tolerating fences and stray prose."""
    t = text.strip()
    m = re.search(r"```(?:json)?\s*(.*?)```", t, re.DOTALL)
    if m:
        t = m.group(1).strip()
    if not t.startswith("{"):
        i = t.find("{")
        if i < 0:
            return None
        t = t[i:]
    try:
        out = json.loads(t)
    except json.JSONDecodeError:
        # Trailing prose after the object: balance braces and retry.
        depth = 0
        for i, ch in enumerate(t):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    try:
                        out = json.loads(t[: i + 1])
                        break
                    except json.JSONDecodeError:
                        return None
        else:
            return None
    return out if isinstance(out, dict) else None


def _fail(reason: str, spec: dict, text: str) -> dict:
    """The failure shape: the reason, plus the LOCAL lane's answer so the
    user is never left with nothing. The caller labels the fallback."""
    local = language.interpret(spec, text)
    local["source"] = "local-fallback"
    local["error"] = reason
    return local


def interpret_llm(spec: dict, text: str) -> dict:
    """One `claude -p` call -> validated patch via `spec.patch`.

    Returns the same shape as `language.interpret` plus
    `source` ("llm" | "local-fallback"), `model`, and on failure `error`.
    Never raises.
    """
    exe = _claude_path()
    if exe is None:
        return _fail(
            "the 'claude' command was not found on this machine. Install "
            "Claude Code (https://claude.com/claude-code) and run "
            "`claude login` once; the forge uses your existing subscription, "
            "no API key.", spec, text)

    try:
        proc = subprocess.run(
            [exe, "-p", "--model", MODEL, "--output-format", "json"],
            input=_prompt(spec, text), capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return _fail(f"Claude did not answer within {TIMEOUT_S} s", spec, text)
    except OSError as e:
        return _fail(f"could not run {exe!r}: {e}", spec, text)

    if proc.returncode != 0:
        err = (proc.stderr or proc.stdout or "").strip()
        hint = (" -- run `claude login` first"
                if re.search(r"log ?in|auth|credential", err, re.I) else "")
        return _fail(f"claude exited {proc.returncode}: "
                     f"{err[:300] or 'no error text'}{hint}", spec, text)

    # --output-format json wraps the reply in an envelope; the model's own
    # text is in `result`. A bare-text reply is tolerated.
    reply = proc.stdout
    envelope = _extract_json(proc.stdout)
    if envelope and isinstance(envelope.get("result"), str):
        reply = envelope["result"]

    parsed = _extract_json(reply)
    if parsed is None or not isinstance(parsed.get("changes"), dict):
        return _fail("Claude's reply was not the required "
                     '{"changes": {...}} JSON object', spec, text)

    # Post-validation: unknown paths are dropped BY NAME; values go through
    # spec.patch so clamps and the out-of-menu substitution alarm apply.
    changes: dict = {}
    dropped: list[str] = []
    for path, value in parsed["changes"].items():
        if path in specmod.BY_PATH:
            changes[str(path)] = value
        else:
            dropped.append(str(path))

    patched, report = specmod.patch(spec, changes)

    edits = []
    for path in changes:
        old, new = specmod.get(spec, path), specmod.get(patched, path)
        if old != new:
            edits.append({"path": path,
                          "label": specmod.BY_PATH[path].label,
                          "from": old, "to": new})

    # Provenance into `notes` (hash-excluded): what was asked, which model,
    # when. The next person reading the spec file sees where it came from.
    stamp = f"[claude {MODEL} {date.today().isoformat()}] {text.strip()}"
    prior = str(patched.get("notes") or "").strip()
    patched["notes"] = f"{prior}\n{stamp}".strip()

    note = str(parsed.get("note") or "").strip()
    understood = ([note] if note else []) + (
        [f"ignored unknown parameter paths: {', '.join(sorted(dropped))}"]
        if dropped else [])
    return {
        "spec": patched,
        "edits": edits,
        "understood": understood,
        "ignored": sorted(dropped),
        "warnings": report.warnings,
        "source": "llm",
        "model": MODEL,
    }
