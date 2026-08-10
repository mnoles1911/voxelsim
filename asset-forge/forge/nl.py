"""Plain-language edits to a species spec.

The model edits *parameters*, never voxels. That is the whole design: a
sentence like "shorter and more gnarled, sparser canopy" comes back as a small
list of `(path, value)` changes, which go through exactly the same
`spec.patch()` path a slider drag does -- so language edits are validated,
clamped, undoable with Revert, and visible as moved sliders rather than as an
opaque black box.

The parameter table is sent as a cached system block. It is stable across every
request, so it is written once per five minutes rather than on every edit.

Needs `anthropic` installed and a credential (`ANTHROPIC_API_KEY`, or an
`ant auth login` profile). Without one the app runs normally and the language
box reports that it is unconfigured.
"""

from __future__ import annotations

import json
import os
from typing import Any

from . import spec as specmod

MODEL = os.environ.get("ASSET_FORGE_MODEL", "claude-opus-5")

EDIT_SCHEMA = {
    "type": "object",
    "properties": {
        "edits": {
            "type": "array",
            "description": "Only the parameters that need to change.",
            "items": {
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Exact parameter path."},
                    "value": {
                        "type": "string",
                        "description": "New value as a string; numbers plain, booleans "
                                       "'true'/'false', choices exactly as listed.",
                    },
                    "why": {"type": "string", "description": "Short reason, one clause."},
                },
                "required": ["path", "value", "why"],
                "additionalProperties": False,
            },
        },
        "explanation": {
            "type": "string",
            "description": "One or two sentences for the designer describing the change.",
        },
    },
    "required": ["edits", "explanation"],
    "additionalProperties": False,
}


def available() -> tuple[bool, str]:
    """Is a plain-language edit actually possible right now?

    Constructing `Anthropic()` is NOT a credential check — it succeeds with no
    key and only fails at request time with "Could not resolve authentication
    method". Inspect the resolved credential instead, so the UI can say the box
    is unconfigured rather than letting the designer type a request and hit an
    error.
    """
    try:
        import anthropic
    except ImportError:
        return False, "the `anthropic` package is not installed (pip install anthropic)"
    try:
        client = anthropic.Anthropic()
    except Exception as exc:
        return False, f"client setup failed ({exc.__class__.__name__})"
    if not (getattr(client, "api_key", None) or getattr(client, "auth_token", None)):
        return False, "set ANTHROPIC_API_KEY (or run `ant auth login`) and restart"
    return True, ""


def _parameter_table() -> str:
    """The stable half of the system prompt: what the designer can steer."""
    lines = []
    for p in specmod.PARAMS:
        if p.kind == "choice":
            rng = "one of: " + ", ".join(p.choices)
        elif p.kind == "bool":
            rng = "true or false"
        elif p.kind == "text":
            rng = "free text"
        else:
            rng = f"{p.lo} to {p.hi} ({p.kind})"
        row = f"{p.path} [{p.group}] — {p.label}; {rng}"
        if p.help:
            row += f". {p.help}"
        lines.append(row)
    return "\n".join(lines)


SYSTEM_GUIDE = """\
You turn a designer's plain-language request into parameter edits for a
procedural voxel tree generator. The designer is not expected to know which
sliders do what — that is your job.

How the generator works, so your edits land where you intend:

- The crown is a VOLUME that growth targets are scattered into, and branches
  grow toward them. So a tree's silhouette comes from `crown.shape`,
  `crown.radius_m`, `crown.height_frac` and `crown.center_frac` — not from any
  branch parameter.
- `crown.shell` pushes targets toward the crown's outer surface. Raise it for
  layered, airy, or umbrella crowns; lower it for dense round ones.
- Gnarled, twisted, weather-beaten looks come from `trunk.wander` and
  `growth.jitter`, plus `trunk.lean_deg`.
- Droop and weeping come from `growth.gravity` (negative droops) and
  `foliage.droop_m`.
- Fine twiggy branching comes from a SMALL `growth.kill_m` and `growth.step_m`
  with MORE `crown.points`. Coarse, stubby branching is the opposite.
- `variation.*` controls how much individuals of the species differ from each
  other, not what the species looks like. Only touch it when the request is
  about variety or consistency across trees.
- `height_m` is the whole tree. Trunk thickness is `trunk.radius_base_m`.

Rules:

- Emit ONLY parameters that need to change. A request about the canopy should
  not move trunk parameters.
- Stay inside each parameter's stated range. Values outside it get clamped and
  the designer sees a warning.
- Prefer a few decisive edits over many timid ones — a request for "much
  taller" should be a real change, not a 5% nudge.
- Interpret relative language against the CURRENT VALUES you are given.
- If the request names a real species or a look you recognise, use what you
  know about how that tree is actually shaped.
- If a request cannot be expressed in these parameters, say so in the
  explanation and return the edits that get closest.

Parameters you may change:

"""


def edit(spec: dict, request: str) -> dict:
    """Apply a plain-language request. Returns the patched spec and the reasoning."""
    import anthropic

    client = anthropic.Anthropic()

    current = "\n".join(
        f"{p.path} = {specmod.get(spec, p.path)!r}"
        for p in specmod.PARAMS
        if p.kind != "text"
    )

    response = client.messages.create(
        model=MODEL,
        max_tokens=4000,
        output_config={"effort": "medium", "format": {"type": "json_schema",
                                                      "schema": EDIT_SCHEMA}},
        system=[
            {
                "type": "text",
                "text": SYSTEM_GUIDE + _parameter_table(),
                # Stable across every request in the session, so it is written
                # to the cache once rather than re-sent at full price per edit.
                "cache_control": {"type": "ephemeral"},
            },
            {"type": "text", "text": "CURRENT VALUES\n\n" + current},
        ],
        messages=[{"role": "user", "content": request}],
    )

    if response.stop_reason == "refusal":
        return {"error": "the request was declined", "edits": [], "explanation": ""}

    text = next((b.text for b in response.content if b.type == "text"), "")
    if not text:
        return {"error": "empty response", "edits": [], "explanation": ""}
    result = json.loads(text)

    changes: dict[str, Any] = {}
    rejected: list[str] = []
    for e in result.get("edits", []):
        path = e.get("path", "")
        param = specmod.BY_PATH.get(path)
        if param is None:
            rejected.append(f"unknown parameter {path!r}")
            continue
        try:
            changes[path] = _coerce(param, e.get("value"))
        except (TypeError, ValueError):
            rejected.append(f"{path}: {e.get('value')!r} is not a {param.kind}")

    patched, report = specmod.patch(spec, changes)
    return {
        "spec": patched,
        "edits": [
            {"path": k, "value": v, "why": _why(result, k), "label": specmod.BY_PATH[k].label}
            for k, v in changes.items()
        ],
        "explanation": result.get("explanation", ""),
        "warnings": report.warnings + rejected,
        "usage": {
            "input": response.usage.input_tokens,
            "output": response.usage.output_tokens,
            "cache_read": getattr(response.usage, "cache_read_input_tokens", 0),
        },
    }


def _why(result: dict, path: str) -> str:
    for e in result.get("edits", []):
        if e.get("path") == path:
            return e.get("why", "")
    return ""


def _coerce(param, raw):
    """String from the model -> the parameter's real type."""
    if raw is None:
        raise TypeError("missing value")
    text = str(raw).strip()
    if param.kind == "bool":
        return text.lower() in ("1", "true", "yes", "on")
    if param.kind in ("choice", "text"):
        return text
    value = float(text)
    return int(round(value)) if param.kind == "int" else value
