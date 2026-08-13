"""Write a seeded spec without destroying a tuned one.

Every `seed_*.py` script authors species by patching a default spec and saving
it over `specs/<name>.json`. That is right the first time and destructive every
time after: the values in those files are the DRAFT a species started from, and
what is on disk is the result of tuning it, so re-running one silently reverts
the work. It has already happened. `tools/seed_heroes.py` reverted a finished
`hero-natural-arch` from its hand-tuned values back to draft, and the only
reason it was recoverable is that a backup happened to be seconds old. Sixteen
legacy rock specs are one command away from the same thing today.

So: a seed script does not overwrite a spec that already exists. It says what it
skipped and moves on. `--force` restores the old behaviour for the case where
reverting IS the intent, and prints loudly enough that nobody does it by
accident.

This is deliberately not clever. Merging draft values into a tuned spec sounds
better and is worse -- it would half-revert a species, which is harder to notice
than a full revert and impossible to undo by hand.
"""
from pathlib import Path

from forge import spec as sm


def parse_force(argv) -> bool:
    """`--force` anywhere on the command line."""
    return "--force" in argv


def write(spec: dict, path: Path, warnings=(), *, force: bool = False,
          label: str | None = None, width: int = 24) -> bool:
    """Save `spec` to `path` unless it exists. Returns True if written."""
    name = label or path.stem
    if path.exists() and not force:
        print(f"  {name:<{width}} SKIPPED, already authored "
              f"(--force to overwrite with draft values)")
        return False
    sm.save(spec, path)
    bad = [w for w in warnings if "tip_radius" not in w]
    print(f"  {name:<{width}} " + ("! " + "; ".join(bad) if bad else
                                   ("OVERWRITTEN with draft values" if force
                                    else "written")))
    return True


def announce(force: bool, what: str = "specs") -> None:
    if force:
        print(f"--force: existing {what} will be OVERWRITTEN with draft "
              f"values, discarding any tuning on disk.")
