"""Prove the Claude lane's guardrails fire, WITHOUT spending a call.

`forge/llm.py` shells out to the Claude Code CLI on the owner's
subscription. Every arm here runs against a STUB standing in for that CLI
(`ASSET_FORGE_CLAUDE_CMD`), because a probe that burned a real subscription
call per CI run would train people to stop running it -- and because the
arms are about THIS side of the seam: what happens when the model answers
badly, which no live call can be asked to do on demand.

    python tools/llmprobe.py            # all stubbed arms
    python tools/llmprobe.py --live     # plus ONE real `claude -p` smoke
                                        # call, only if claude is on PATH

The arms:
  * good reply        -> patch applies through spec.patch, edits reported,
                         provenance stamped into notes
  * out-of-menu reply -> the substitution alarm ("BUILT AND DRAWN AS")
                         fires through the lane, exactly as a hand edit
  * unknown path      -> dropped BY NAME, never applied
  * prose reply       -> rejected with the reason; the LOCAL grammar answers
  * clamped value     -> a number outside the slider range lands clamped
  * missing CLI       -> plain-English failure naming `claude login`
  * timeout           -> plain-English failure, local fallback
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import llm, spec as sm

FIXTURE_ENV = "ASSET_FORGE_LLMPROBE_FIXTURE"

STUB_PY = r'''
import json, os, sys, time
fixture = os.environ.get("ASSET_FORGE_LLMPROBE_FIXTURE", "good")
sys.stdin.read()
replies = {
    "good": json.dumps({"changes": {"height_m": 30.0},
                        "note": "made it much taller"}),
    "menu": json.dumps({"changes": {"materials.bark": "definitely-not-a-material"},
                        "note": "repainted the bark"}),
    "unknown": json.dumps({"changes": {"nonsense.path": 1, "height_m": 12.0},
                           "note": "mixed reply"}),
    "prose": "I think you should simply make it taller!",
    "clamp": json.dumps({"changes": {"height_m": 99999.0}, "note": "to the moon"}),
    "slow": "",
}
if fixture == "slow":
    time.sleep(10)
# the CLI's --output-format json envelope
print(json.dumps({"type": "result", "subtype": "success",
                  "result": replies[fixture]}))
'''


def make_stub(tmp: Path) -> str:
    """A stand-in `claude` the subprocess can exec on Windows: a .cmd shim
    that runs the python stub."""
    (tmp / "stub.py").write_text(STUB_PY, encoding="utf-8")
    cmd = tmp / "claude-stub.cmd"
    cmd.write_text(f'@"{sys.executable}" "{tmp / "stub.py"}" %*\n',
                   encoding="utf-8")
    return str(cmd)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--live", action="store_true",
                    help="also make ONE real claude -p call if it is on PATH")
    args = ap.parse_args()

    body, _ = sm.load(Path(__file__).resolve().parents[1] / "specs" / "temperate-oak.json")
    ok = True

    def arm(fired, label: str, detail: str = "") -> None:
        nonlocal ok
        fired = bool(fired)
        print(f"  {'fires' if fired else 'SILENT'}: {label}"
              + (f"  ({detail})" if detail and not fired else ""))
        ok &= fired

    with tempfile.TemporaryDirectory() as td:
        os.environ[llm.CMD_ENV] = make_stub(Path(td))
        try:
            os.environ[FIXTURE_ENV] = "good"
            r = llm.interpret_llm(body, "make it much taller")
            arm(r.get("source") == "llm"
                and any(e["path"] == "height_m" and e["to"] == 30.0 for e in r["edits"])
                and not r["warnings"]
                and "[claude" in str(r["spec"].get("notes", "")),
                "a good reply applies through spec.patch, zero warnings, "
                "provenance stamped", json.dumps(r, default=str)[:300])

            os.environ[FIXTURE_ENV] = "menu"
            r = llm.interpret_llm(body, "repaint the bark")
            arm(any("BUILT AND DRAWN AS" in w for w in r["warnings"]),
                "an out-of-menu choice trips the substitution alarm",
                str(r["warnings"]))

            os.environ[FIXTURE_ENV] = "unknown"
            r = llm.interpret_llm(body, "mixed")
            arm("nonsense.path" in r["ignored"]
                and all(e["path"] != "nonsense.path" for e in r["edits"])
                and any(e["path"] == "height_m" for e in r["edits"]),
                "an unknown path is dropped BY NAME; known ones still land",
                json.dumps({"ignored": r["ignored"]}))

            os.environ[FIXTURE_ENV] = "prose"
            r = llm.interpret_llm(body, "make it taller")
            arm(r.get("source") == "local-fallback" and "error" in r
                and r["edits"],  # the LOCAL grammar answered "taller"
                "a prose reply is rejected and the local grammar answers",
                str(r.get("error")))

            os.environ[FIXTURE_ENV] = "clamp"
            r = llm.interpret_llm(body, "to the moon")
            hi = sm.BY_PATH["height_m"].hi
            arm(any(e["path"] == "height_m" and e["to"] == hi for e in r["edits"])
                and any("clamped" in w for w in r["warnings"]),
                f"an out-of-range value is clamped to the slider hi ({hi})",
                str(r["warnings"]))

            os.environ[FIXTURE_ENV] = "slow"
            was = llm.TIMEOUT_S
            llm.TIMEOUT_S = 2
            try:
                r = llm.interpret_llm(body, "anything")
                arm(r.get("source") == "local-fallback"
                    and "did not answer" in str(r.get("error")),
                    "a hang times out into a plain-English local fallback")
            finally:
                llm.TIMEOUT_S = was

            os.environ[FIXTURE_ENV] = "good"
            # THE CREATION-FLOW ARMS (owner directive 2026-09-05): the human
            # fixed kind/sub-category/name; the model must not move them.
            r = llm.create_llm("fish", "reed-eel", "long and slender",
                               subcategory="Eels ", wants_new_generator=True)
            cur = sm.curation(r["spec"])
            arm(sm.get(r["spec"], "kind") == "fish"
                and sm.get(r["spec"], "name") == "reed-eel"
                and r["spec"].get("subcategory") == "eels"
                and cur["status"] == "draft"
                and "TODO(owner)" in str(r["spec"].get("notes", ""))
                and any(e["path"] == "height_m" for e in r["edits"]),
                "create_llm: human choices forced, label cleaned, DRAFT, "
                "new-generator wish recorded as a TODO -- and the patch landed",
                json.dumps({"kind": sm.get(r["spec"], "kind"),
                            "sub": r["spec"].get("subcategory"),
                            "cur": cur, "notes": r["spec"].get("notes")}))
            r = llm.create_llm("not-a-kind", "x", "anything")
            arm(r.get("spec") is None and "unknown kind" in str(r.get("error")),
                "create_llm refuses an unknown kind by name")

            os.environ[llm.CMD_ENV] = str(Path(td) / "does-not-exist.cmd")
            r = llm.interpret_llm(body, "anything")
            arm(r.get("source") == "local-fallback"
                and "could not run" in str(r.get("error")),
                "a missing stub path fails in plain English")
            # Creation still works offline: the LOCAL grammar answers,
            # labelled, and the draft spec is still built.
            r = llm.create_llm("fish", "reed-eel", "much longer")
            arm(r.get("source") == "local-fallback" and r.get("spec") is not None
                and sm.get(r["spec"], "name") == "reed-eel"
                and sm.curation(r["spec"])["status"] == "draft",
                "create_llm with no claude falls back to the local grammar "
                "and still creates the draft")
        finally:
            os.environ.pop(llm.CMD_ENV, None)
            os.environ.pop(FIXTURE_ENV, None)

    # no override, empty PATH: the not-installed message must name the fix
    old_path = os.environ.get("PATH", "")
    os.environ["PATH"] = ""
    try:
        r = llm.interpret_llm(body, "anything")
        arm(r.get("source") == "local-fallback"
            and "claude login" in str(r.get("error")),
            "no claude on PATH says install + `claude login`, no traceback")
    finally:
        os.environ["PATH"] = old_path

    if args.live:
        exe = shutil.which("claude")
        if not exe:
            print("  live: SKIPPED (claude not on PATH)")
        else:
            r = llm.interpret_llm(body, "make it a little taller")
            fired = r.get("source") == "llm" and r["edits"]
            print(f"  live: {'PASS' if fired else 'FAIL'} "
                  f"(source={r.get('source')}, edits={[(e['path'], e['to']) for e in r['edits']]}, "
                  f"error={r.get('error')})")
            ok &= bool(fired)

    print("llmprobe:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
