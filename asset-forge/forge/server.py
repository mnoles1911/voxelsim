"""The local web app: browse variants, turn sliders, keep the good ones.

Stdlib `http.server` and a hand-written frontend, deliberately. The tool needs
to open on this machine with no install step and no network, and a framework
would buy nothing here -- there is one page, and the expensive work all happens
in numpy on the server side.

The design leans on one property from `pipeline.build`: generation is
deterministic from `(spec, seed)`. So the server never holds voxel grids in
memory. It keeps thumbnails and statistics, and when you click a tree for a
closer look or export it, the tree is regenerated from its spec and seed and
comes back byte-identical. A hundred-tile gallery costs a few megabytes of PNG
instead of a gigabyte of voxels.
"""

from __future__ import annotations

import io
import json
import mimetypes
import struct
import threading
import traceback
import uuid
import webbrowser
from dataclasses import dataclass, field
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from . import parts as partslib
from . import (biomes as biomelib, contact, kinds as kindlib, materials, pipeline,
               render, spec as specmod, vox, vxa)

ROOT = Path(__file__).resolve().parent.parent
# The React frontend (web/dist, built by `npm run build` in web/) took over
# from the hand-written page in forge/web on 2026-08-18. The old page is kept
# as a fallback ONLY for a checkout that has never built the app, so the
# server still opens on something rather than a 404.
DIST = ROOT / "web" / "dist"
LEGACY_WEB = Path(__file__).resolve().parent / "web"
SPECS = ROOT / "specs"
LIBRARY = ROOT / "library"
RULES_DIR = ROOT / "rules"
PLACEMENT_RULES = RULES_DIR / "placement-rules.json"
BIOME_DENSITY = RULES_DIR / "biome-density.json"

TILE_PX = 260
DETAIL_PX = 820
MAX_JOBS = 6

# Gallery tiles generate at whatever resolution is AFFORDABLE, not at a fixed
# one. The skeleton is resolution-independent, so a preview and an export are
# the same asset sampled differently, and a twelve-tile gallery of 2 cm trees
# would be a minute of compute for detail no 260-pixel thumbnail can show.
#
# But a fixed 10 cm floor was wrong in the other direction and worse: a 1 m
# bush at 10 cm is ten voxels tall, so every shrub in the gallery previewed as
# an unreadable smudge no matter how it was authored. Cost is what actually
# matters, and cost is cells, so budget cells. A big tree lands on 10 cm and a
# small shrub lands on 2 cm from the same rule.
PREVIEW_CELLS = int(os.environ.get("ASSET_FORGE_PREVIEW_CELLS", "6000000"))
PREVIEW_CM = float(os.environ.get("ASSET_FORGE_PREVIEW_CM", "10"))   # coarsest tier

# The tiers a preview may land on, finest first. Authored resolutions coarser
# than a tier are never refined up to it.
PREVIEW_TIERS = (1.0, 2.0, 2.5, 5.0, 10.0)

# Ceiling on instances sent to the browser's 3D viewer. Above this the voxels
# are thinned by a fixed stride -- a 2 cm emergent has tens of millions of
# surface voxels, which is a 200 MB download and a stalled tab.
MAX_VIEWER_VOXELS = int(os.environ.get("ASSET_FORGE_MAX_VIEWER_VOXELS", "2500000"))


# --- generation jobs --------------------------------------------------------


@dataclass
class Tile:
    seed: int
    png: bytes | None = None
    stats: dict | None = None
    problems: list[str] = field(default_factory=list)
    error: str | None = None


@dataclass
class Job:
    id: str
    spec: dict
    seeds: list[int]
    scale: int
    preview_cm: float = 10.0
    # Which of the three review cameras this kind wants; see
    # `forge.render.camera_for`, which is the ONE place that decides.
    camera: str = "iso"
    tiles: dict[int, Tile] = field(default_factory=dict)
    done: int = 0
    cancelled: bool = False

    def progress(self) -> dict:
        return {
            "job": self.id,
            "done": self.done,
            "total": len(self.seeds),
            "seeds": self.seeds,
            "tiles": {
                str(s): {
                    "ready": t.png is not None or t.error is not None,
                    "error": t.error,
                    "stats": t.stats,
                    "problems": t.problems,
                }
                for s, t in self.tiles.items()
            },
        }


class Forge:
    """Owns the job cache. One instance per server.

    Also memoises finished tiles by `(spec hash, seed, scale)`. Because
    generation is deterministic, a tile computed once is valid forever -- so
    nudging a slider back to where it was, switching species and returning, or
    just reloading the page all come back instantly instead of regrowing
    identical trees.
    """

    TILE_CACHE_MAX = 600

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.jobs: dict[str, Job] = {}
        self.order: list[str] = []
        self.tile_cache: dict[tuple, Tile] = {}
        self.tile_order: list[tuple] = []

    def _cache_get(self, key) -> Tile | None:
        with self.lock:
            return self.tile_cache.get(key)

    def _cache_put(self, key, tile: Tile) -> None:
        with self.lock:
            if key in self.tile_cache:
                return
            self.tile_cache[key] = tile
            self.tile_order.append(key)
            while len(self.tile_order) > self.TILE_CACHE_MAX:
                self.tile_cache.pop(self.tile_order.pop(0), None)

    def start(self, spec: dict, seeds: list[int]) -> Job:
        # One scale for the whole batch so tiles are comparable by size, the
        # same reason contact sheets do it.
        preview_cm = preview_resolution(spec)
        # Each kind is previewed through its own camera, and WHICH ONE is
        # decided in exactly one place -- `forge.render.camera_for`.
        #
        # The isometric looks down onto the asset, which is exactly where a
        # rock keeps the thing that defines it. It has now hidden three
        # separate features and produced three wrong verdicts: a tor stack read
        # as "overlapping plates" when it is four tiers of boulders, a balanced
        # rock read as a domed lump when its neck is measurably half the width
        # of its cap, and BOTH arch heroes read as solid stone when they have a
        # 40%-plus opening. The last one was reported by the owner as "no hole
        # in the rock", and the hole was there the whole time.
        #
        # A low camera sees over a lip if what is behind stands 0.18 m higher
        # per metre of gap; the isometric needs 0.71 m. That ratio is the whole
        # difference, and it is why this is a camera fix and not an asset fix.
        # A fish needed a third camera again, for a different reason: it is
        # flat and it has a front.
        camera = render.camera_for(spec)
        scale = render.scale_for_camera(
            [render.predicted_extent(spec, preview_cm / 100.0)], camera, TILE_PX)
        job = Job(id=uuid.uuid4().hex[:12], spec=spec, seeds=seeds, scale=scale,
                  preview_cm=preview_cm, camera=camera)
        with self.lock:
            self.jobs[job.id] = job
            self.order.append(job.id)
            while len(self.order) > MAX_JOBS:
                old = self.order.pop(0)
                self.jobs.pop(old, None)
        threading.Thread(target=self._run, args=(job,), daemon=True).start()
        return job

    def _run(self, job: Job) -> None:
        digest = specmod.spec_hash(job.spec)
        for seed in job.seeds:
            if job.cancelled:
                return
            key = (digest, seed, job.scale, job.preview_cm, job.camera)
            tile = self._cache_get(key)
            if tile is None:
                tile = Tile(seed=seed)
                try:
                    tree = pipeline.build(job.spec, seed, resolution_cm=job.preview_cm)
                    g = tree.grid
                    if job.camera == "side":
                        # Which quarter turn to show is MEASURED, not fixed:
                        # `best_turn` scores daylight through the silhouette
                        # first, then overhang. A fin with a hole bored across
                        # its short axis is open from one direction and solid
                        # from ninety degrees round, so a fixed angle shows the
                        # wrong side of it half the time. Rocks only -- a fish
                        # is BUILT facing its camera and a measured turn would
                        # sometimes show the animal end-on.
                        g = render.turned(g, render.best_turn(g.data))
                    img = render.view(g, job.camera, scale=job.scale)
                    buf = io.BytesIO()
                    img.save(buf, "PNG")
                    tile.png = buf.getvalue()
                    tile.stats = tree.stats
                    tile.problems = pipeline.health(tree)
                except Exception:
                    tile.error = traceback.format_exc(limit=3)
                if tile.error is None:
                    self._cache_put(key, tile)
            with self.lock:
                job.tiles[seed] = tile
                job.done += 1

    def get(self, job_id: str) -> Job | None:
        with self.lock:
            return self.jobs.get(job_id)


FORGE = Forge()


# --- library ----------------------------------------------------------------


def encode_voxels(grid) -> bytes:
    """Surface voxels as a compact binary blob for the 3D viewer.

        uint32 nx, ny, nz          grid dimensions
        uint32 count               number of surface voxels
        int16  x, y, z  x count    voxel coordinates
        uint8  material x count    material id per voxel

    Two typed-array sections rather than interleaved structs, because that is
    what the browser can map straight onto `Int16Array` / `Uint8Array` with no
    per-voxel JavaScript.
    """
    import numpy as np

    mask = grid.surface_mask()
    xs, ys, zs = np.nonzero(mask)
    mats = grid.data[xs, ys, zs].astype(np.uint8)
    pos = np.empty((xs.size, 3), dtype=np.int16)
    pos[:, 0], pos[:, 1], pos[:, 2] = xs, ys, zs
    header = struct.pack("<IIII", *(int(v) for v in grid.shape), int(xs.size))
    return header + pos.tobytes() + mats.tobytes()


def _finest_within(spec: dict, budget: float, weight: float) -> float:
    """Finest tier at or coarser than the authored size that fits `budget`.

    `weight` converts a bounding box into the thing being budgeted -- 1.0 to
    budget grid cells, or the measured surface fraction to budget the instances
    the browser has to draw.
    """
    authored = float(specmod.get(spec, "resolution_cm"))
    for cm in PREVIEW_TIERS:
        if cm < authored:
            continue
        nx, ny, nz = render.predicted_extent(spec, cm / 100.0)
        if nx * ny * nz * weight <= budget:
            return cm
    return max(PREVIEW_CM, authored)


def preview_resolution(spec: dict) -> float:
    """Voxel size for gallery tiles."""
    return _finest_within(spec, PREVIEW_CELLS, 1.0)


def viewer_resolution(spec: dict, budget: int | None = None) -> float:
    """Finest voxel size the browser can be asked to draw for this species.

    Never finer than the species is authored at, and never so fine that the
    instance count blows past the viewer budget. A 28 m emergent at 2 cm has
    tens of millions of surface voxels; showing it at 5 cm is a far better
    answer than shipping 200 MB and stalling the tab. The export is unaffected
    -- this only governs what gets drawn.

    `budget` lets the client ask for less. A phone on wifi with a mobile GPU is
    not a desktop, and the honest answer is to send it a coarser lattice rather
    than the same one more slowly.
    """
    budget = MAX_VIEWER_VOXELS if budget is None else max(
        50_000, min(MAX_VIEWER_VOXELS, int(budget)))
    # Measured: an asset's surface voxels come to about 2.3% of its bounding box
    # (an oak at 5 cm is 436k surface out of 18.7M cells). Good enough to pick a
    # tier; the tier only has to be roughly right.
    return _finest_within(spec, budget, 0.023)


def thumb_grid(grid, spec: dict):
    """The grid oriented the way its picture should be taken.

    The CAMERA is `render.camera_for`, in one place, read by everybody. The
    TURN is a second decision and it was being made in two places and skipped
    in a third -- the gallery tile turned a rock by measurement, the library
    thumbnail did not, so `hero-arch-colossal` was baked with a portrait of its
    own wall. An arch is a hole from one direction and a solid from ninety
    degrees round; on that asset the difference is 65% open sky against 0.1%.

    Rocks only, and for the reason `elevation.turn_for` gives: the measure
    scores daylight and then overhang, which are questions about stone. A tree
    has neither, so it would pick between four equivalent views on noise, and
    an animal is BUILT facing its camera -- a measured turn would sometimes
    show it end-on.
    """
    if render.camera_for(spec) == "side":
        return render.turned(grid, render.best_turn(grid.data))
    return grid


def keep(spec: dict, seed: int) -> dict:
    """Save a tree the designer approved.

    Writes the spec and seed (which is the asset), plus the exports and a
    thumbnail so the library can be browsed without regenerating anything.
    """
    name = specmod.get(spec, "name")
    entry_id = f"{name}-{seed:04d}"
    out = LIBRARY / name / entry_id
    out.mkdir(parents=True, exist_ok=True)

    tree = pipeline.build(spec, seed)
    specmod.save(spec, out / "spec.json")
    specmod.save(tree.realized, out / "realized.json")
    models = vox.write(tree.grid, out / "tree.vox", name=entry_id)
    vxa.write(tree.grid, out / "tree.vxa", tree.parts,
              partslib.joints(tree.parts))
    render.view(thumb_grid(tree.grid, spec), render.camera_for(spec),
                target_px=DETAIL_PX).save(out / "thumb.png")
    meta = {
        "id": entry_id,
        "species": name,
        "kind": specmod.get(spec, "kind"),
        "seed": seed,
        "spec_hash": specmod.spec_hash(spec),
        "stats": tree.stats,
        "problems": pipeline.health(tree),
        "vox_models": models,
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    return meta


def _size_m(spec: dict, kind: str) -> float:
    """The number that means "how big is it" for this kind.

    Reading `height_m` off a rock or a fish gives the untouched default -- a
    12 m fish -- and both the species list and the biome coverage tab print it
    as though it meant something.
    """
    if kind == "rock":
        return specmod.get(spec, "rock.size_m")
    if kind in ("fish", "cetacean"):
        return specmod.get(spec, "fish.length_m")
    if kind == "bird":
        return specmod.get(spec, "bird.length_m")
    if kind == "quadruped":
        # HEAD-BODY LENGTH, which is what the parameter means and what every
        # size in `docs/biomes/*.md` is quoted as. Not the shoulder height, even
        # though that is the number a field guide leads with for a large mammal:
        # this column sits beside a tree's height and a fish's length, and a
        # bison reading "1.8" next to a trout reading "0.3" would be comparing
        # two different measurements in one column.
        return specmod.get(spec, "quad.length_m")
    return specmod.get(spec, "height_m")


def _shape_word(spec: dict, kind: str) -> str:
    """One word for what shape this species is, per kind."""
    if kind == "rock":
        return specmod.get(spec, "materials.rock")
    if kind in ("fish", "cetacean"):
        return specmod.get(spec, "fish.caudal_shape")
    if kind == "bird":
        # The POSE, not the tail shape. A bird's tail has seven outlines and a
        # one-word summary of a species is better spent saying whether it is
        # perched or in the air, which is the thing that decides its camera,
        # its grid size and half its parameter set.
        return specmod.get(spec, "bird.pose")
    if kind == "quadruped":
        # THE STANCE, not the headgear. Only a handful of species carry horns at
        # all, so a headgear column would read "none" for most of the list;
        # standing / sprawling / bipedal splits it into three groups that mean
        # something, and it is the row that decides the limb geometry.
        return specmod.get(spec, "quad.stance")
    return specmod.get(spec, "crown.shape")


def library_list() -> list[dict]:
    if not LIBRARY.exists():
        return []
    entries = []
    for meta_path in sorted(LIBRARY.glob("*/*/meta.json")):
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        # Entries kept before kinds existed have no `kind`; read it back off the
        # saved spec rather than assuming, so an old library scopes correctly
        # instead of piling everything into the tree tab.
        if "kind" not in meta:
            try:
                s, _ = specmod.load(meta_path.parent / "spec.json")
                meta["kind"] = specmod.get(s, "kind")
            except (OSError, json.JSONDecodeError, KeyError):
                meta["kind"] = "tree"
        entries.append(meta)
    entries.sort(key=lambda m: (m.get("species", ""), m.get("seed", 0)))
    return entries


def library_dir(entry_id: str) -> Path | None:
    """Resolve an entry id to its directory, refusing anything that escapes."""
    for d in LIBRARY.glob(f"*/{entry_id}"):
        resolved = d.resolve()
        if resolved.is_dir() and LIBRARY.resolve() in resolved.parents:
            return resolved
    return None


# --- placement: the named-rule library + per-spec blocks --------------------
#
# The UI writes placement the same way it writes curation: into the RAW file,
# touching only the edited block, so the diff IS the edit. The schema itself
# (field law, composition, allowlist resolution) is owned by forge/spec.py and
# forge/biomes.py -- everything here just enforces it at the route.

# Clamp ranges for rule fields, mirrored in web/src/lib/schema.ts (the client
# validates before writing; the server refuses regardless).
RULE_CLAMPS = {
    "elev_min_m": (-500.0, 9000.0), "elev_max_m": (-500.0, 9000.0),
    "slope_min_pct": (0.0, 70.0), "slope_max_pct": (0.0, 70.0),
    "water_max_m": (0.1, 2000.0), "spacing_m": (0.0, 500.0),
    "abundance": (0.0, 1.0), "cluster": (0.0, 1.0),
}
RULE_WATER_KINDS = ("any", "ocean", "river", "lake", "shallow", "reef")

# Species names are file names; same charset the seeders use.
import re as _re
SPECIES_NAME_RE = _re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")


def _read_json(path: Path, fallback: dict) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return dict(fallback)


def rules_doc() -> dict:
    """rules/placement-rules.json, raw (the `_readme` rides along untouched)."""
    return _read_json(PLACEMENT_RULES, {"rules": {}})


def density_table() -> dict:
    return _read_json(BIOME_DENSITY, {}).get("density", {})


def rule_references() -> dict:
    """Rule name -> the species whose `biome_rules` cite it. Read from the RAW
    spec files: this is what makes delete refuse instead of silently orphaning
    a reference the export would then refuse by name."""
    refs: dict[str, list[str]] = {}
    for p in sorted(SPECS.glob("*.json")):
        raw = _read_json(p, {})
        br = raw.get("biome_rules")
        if not isinstance(br, dict):
            continue
        cited = set()
        for names in br.values():
            if isinstance(names, list):
                cited.update(n for n in names if isinstance(n, str))
        for n in sorted(cited):
            refs.setdefault(n, []).append(raw.get("name", p.stem))
    return refs


def clean_rule_body(raw) -> tuple[dict | None, str | None]:
    """One rule body, checked against the field law. Returns (cleaned, error)."""
    if not isinstance(raw, dict):
        return None, "a rule must be a mapping of restriction fields"
    out: dict = {}
    for k, v in raw.items():
        if k == "_note":
            if str(v).strip():
                out["_note"] = str(v)
            continue
        if k not in specmod.RULE_FIELDS:
            return None, f"{k!r} is not a rule field (menu: {list(specmod.RULE_FIELDS)})"
        if k == "water":
            if v not in RULE_WATER_KINDS:
                return None, f"water must be one of {RULE_WATER_KINDS}"
            out[k] = v
            continue
        try:
            fv = float(v)
        except (TypeError, ValueError):
            return None, f"{k} must be a number"
        lo, hi = RULE_CLAMPS[k]
        if not (lo <= fv <= hi):
            return None, f"{k} must be between {lo:g} and {hi:g}"
        out[k] = int(fv) if fv == int(fv) else fv
    if not any(k in out for k in specmod.RULE_FIELDS):
        return None, "a rule must set at least one field, or it restricts nothing"
    if "elev_min_m" in out and "elev_max_m" in out and out["elev_min_m"] >= out["elev_max_m"]:
        return None, "the elevation floor must sit below the ceiling"
    if ("slope_min_pct" in out and "slope_max_pct" in out
            and out["slope_min_pct"] >= out["slope_max_pct"]):
        return None, "the minimum slope must sit below the maximum"
    if "water" in out and "water_max_m" not in out:
        return None, "a water kind needs a distance (set water_max_m)"
    return out, None


def import_asset(name: str, kind: str, grid, source_format: str) -> dict:
    """Save an asset from an OUTSIDE source into the library.

    The point of the exercise: an imported asset gets a spec file exactly like
    a generated species, so curation and placement (biomes, allowlist, rule
    overrides) work identically and nothing downstream can tell the two paths
    apart. The one honest difference is recorded in `meta.imported` -- there
    is no (spec, seed) that regenerates it, so routes that rebuild from the
    spec serve the stored files instead.
    """
    spec, _ = specmod.validate({
        "name": name, "kind": kind,
        "notes": f"imported from an outside 3D source ({source_format})",
    })
    entry_id = f"{name}-0001"
    out = LIBRARY / name / entry_id
    out.mkdir(parents=True, exist_ok=True)

    specmod.save(spec, out / "spec.json")
    models = vox.write(grid, out / "tree.vox", name=entry_id)
    vxa.write(grid, out / "tree.vxa")
    render.view(grid, "iso", target_px=DETAIL_PX).save(out / "thumb.png")
    meta = {
        "id": entry_id,
        "species": name,
        "kind": kind,
        "seed": 1,
        "imported": True,
        "source_format": source_format,
        "spec_hash": specmod.spec_hash(spec),
        "stats": {
            "height_m": round(grid.shape[2] * grid.voxel_m, 3),
            "voxels": int((grid.data > 0).sum()),
        },
        "problems": [],
        "vox_models": models,
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")

    # The species entry in specs/, never clobbering one that exists. Curation
    # starts at DRAFT: the grandfather clause is for the pre-gate library, and
    # a brand-new import has by definition never been looked at.
    sp = SPECS / f"{name}.json"
    if not sp.exists():
        specmod.save(spec, sp)
        raw = json.loads(sp.read_text(encoding="utf-8"))
        raw["curation"] = {"status": "draft", "seeds": [1],
                          "notes": "imported asset; approve to publish"}
        sp.write_text(json.dumps(raw, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    return meta


def grid_from_vox(blob: bytes, voxel_mm: int):
    """A MagicaVoxel .vox (first model) as a VoxelGrid.

    Colours snap to the nearest forge material colour -- imports keep their
    shape exactly; their palette is quantised onto the engine's materials,
    which is the only palette anything downstream can draw.
    """
    import numpy as np

    from .grid import VoxelGrid, dense_bytes

    if len(blob) < 20 or blob[:4] != b"VOX ":
        raise ValueError("not a MagicaVoxel .vox file (missing 'VOX ' magic)")
    size = xyzi = rgba = None
    i = 8
    while i + 12 <= len(blob):
        cid = blob[i:i + 4]
        n, _children = struct.unpack_from("<II", blob, i + 4)
        content = blob[i + 12:i + 12 + n]
        if cid == b"SIZE" and size is None:
            size = struct.unpack_from("<III", content, 0)
        elif cid == b"XYZI" and xyzi is None:
            cnt = struct.unpack_from("<I", content, 0)[0]
            xyzi = np.frombuffer(content, dtype=np.uint8,
                                 count=cnt * 4, offset=4).reshape(cnt, 4)
        elif cid == b"RGBA":
            rgba = np.frombuffer(content, dtype=np.uint8,
                                 count=256 * 4).reshape(256, 4)
        # A chunk's children are themselves chunks, so skipping only the
        # content walks straight into them (MAIN's content is empty).
        i += 12 + n
    if size is None or xyzi is None or len(xyzi) == 0:
        raise ValueError("no voxels found (missing SIZE/XYZI chunk)")
    if rgba is None:
        raise ValueError(
            "no RGBA palette chunk; save the model from MagicaVoxel 0.99+ "
            "so the file carries its palette")
    sx, sy, sz = (int(v) for v in size)
    if not (0 < sx and 0 < sy and 0 < sz) or dense_bytes((sx, sy, sz)) > 512 << 20:
        raise ValueError(f"model size {sx}x{sy}x{sz} is out of range")

    grid = VoxelGrid((sx, sy, sz), voxel_m=voxel_mm / 1000.0)
    mats = sorted((m, c) for m, c in materials.COLORS.items() if m != materials.MAT_AIR)
    mat_ids = np.array([m for m, _ in mats], dtype=np.uint8)
    mat_rgb = np.array([c for _, c in mats], dtype=np.int64)
    lut = np.zeros(256, dtype=np.uint8)
    for pi in np.unique(xyzi[:, 3]):
        c = rgba[(int(pi) - 1) % 256, :3].astype(np.int64)
        lut[pi] = mat_ids[int(np.argmin(((mat_rgb - c) ** 2).sum(axis=1)))]
    xs = xyzi[:, 0].astype(np.int64)
    ys = xyzi[:, 1].astype(np.int64)
    zs = xyzi[:, 2].astype(np.int64)
    keep = (xs < sx) & (ys < sy) & (zs < sz)
    grid.data[xs[keep], ys[keep], zs[keep]] = lut[xyzi[keep, 3]]
    if not grid.data.any():
        raise ValueError("the model decoded to zero voxels")
    return grid


# --- http -------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    server_version = "assetforge"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quieter console
        pass

    # -- helpers --

    def _send(self, code: int, body: bytes, ctype: str, cache: bool = False) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if cache:
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
        else:
            self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, obj, code: int = 200) -> None:
        self._send(code, json.dumps(obj).encode("utf-8"), "application/json")

    def _body(self) -> dict:
        n = int(self.headers.get("Content-Length", 0))
        if not n:
            return {}
        return json.loads(self.rfile.read(n).decode("utf-8"))

    # -- routes --

    def do_GET(self) -> None:
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        try:
            self._route_get(url.path, q)
        except BrokenPipeError:
            pass
        except Exception:
            self._json({"error": traceback.format_exc(limit=4)}, 500)

    def _route_get(self, path: str, q: dict) -> None:
        if path == "/":
            return self._static("index.html")
        if path.startswith("/static/"):
            return self._static(path[len("/static/") :])
        if path.startswith("/assets/"):
            # Vite's content-hashed bundles from web/dist.
            return self._static(path[1:])

        if path == "/api/schema":
            # Scoped to a kind when asked. The sliders a rock has nothing to do
            # with are not disabled or greyed, they are absent -- a designer
            # authoring boulders should never scroll past a foliage section.
            kind = q.get("kind") or None
            return self._json({
                "kind": kind,
                "params": specmod.ui_schema(kind),
                "groups": [g for g in specmod.GROUPS
                           if not kind or g in kindlib.groups_for(kind)],
            })

        if path == "/api/biomes":
            # The biome menu, straight off forge/biomes.py -- the frontend
            # never hardcodes a biome key or a hosting decision.
            return self._json([
                {"id": b.id, "key": b.key, "label": b.label,
                 "surface": b.surface, "climate": b.climate,
                 "plantable": b.plantable, "hosts": list(b.hosts)}
                for b in biomelib.BIOMES
            ])

        if path == "/api/rules":
            # The named-rule library + the kind x biome density table (the
            # latter read-only context in the UI: it scales everything).
            doc = rules_doc()
            return self._json({
                "rules": doc.get("rules", {}),
                "density": density_table(),
                "fields": list(specmod.RULE_FIELDS),
                "referenced": rule_references(),
            })

        if path == "/api/kinds":
            return self._json([
                {"key": k.key, "label": k.label, "blurb": k.blurb, "ready": k.ready,
                 "species": sum(1 for _, s in self._all_specs()
                                if specmod.get(s, "kind") == k.key)}
                for k in kindlib.KINDS
            ])

        if path == "/api/vocabulary":
            from . import language

            return self._json(language.vocabulary())

        if path == "/api/palette":
            return self._json(
                {str(m): list(materials.color(m)) for m in sorted(materials.COLORS)}
            )

        if path == "/api/coverage":
            # One row per biome: which species claim it and how many approved
            # assets exist. This is the view that answers "what am I missing?",
            # which nothing else in the app does. Scoped by kind, because
            # "tundra has nothing" is a different job from "tundra has no
            # boulders" and mixing them hides both.
            want = q.get("kind") or None
            kept: dict[str, int] = {}
            for entry in library_list():
                kept[entry.get("species", "")] = kept.get(entry.get("species", ""), 0) + 1

            loaded = [(specmod.get(s, "name"), specmod.get(s, "kind"), s)
                      for _, s in self._all_specs()
                      if not want or specmod.get(s, "kind") == want]

            rows = []
            for b in biomelib.BIOMES:
                members = []
                hosts = bool(b.hosts) and (not want or want in b.hosts)
                if hosts:
                    for name, kind, s in loaded:
                        w = float(specmod.get(s, f"biomes.{b.key}") or 0.0)
                        if w > 0:
                            members.append({
                                "name": name, "weight": w, "kind": kind,
                                "kept": kept.get(name, 0),
                                "size_m": _size_m(s, kind),
                                "model": (kind if kind in ("rock", "fish", "cetacean", "bird",
                                                   "quadruped")
                                          else specmod.get(s, "growth.model")),
                            })
                    members.sort(key=lambda m: -m["weight"])
                rows.append({
                    "id": b.id, "key": b.key, "label": b.label,
                    "surface": b.surface, "climate": b.climate,
                    "plantable": b.plantable, "hosts": hosts,
                    "species": members,
                    "kept": sum(m["kept"] for m in members),
                })
            unassigned = [n for n, _, s in loaded if not biomelib.weights(s)]
            return self._json({"kind": want, "biomes": rows, "unassigned": unassigned})

        if path == "/api/specs":
            want = q.get("kind") or None
            out = []
            for p, s in self._all_specs():
                kind = specmod.get(s, "kind")
                if want and kind != want:
                    continue
                out.append(
                    {
                        "name": specmod.get(s, "name"),
                        "file": p.name,
                        "kind": kind,
                        "hash": specmod.spec_hash(s),
                        "size_m": _size_m(s, kind),
                        "height_m": specmod.get(s, "height_m"),
                        "resolution_cm": specmod.get(s, "resolution_cm"),
                        "shape": _shape_word(s, kind),
                        "notes": specmod.get(s, "notes"),
                        "biomes": biomelib.summary(s),
                        "curation": specmod.curation(s),
                        # The placement blocks, for the library's placement
                        # panel. `biome_allow` absent -> null (derived from
                        # the weights); `validate` has already cleaned both.
                        "weights": biomelib.weights(s),
                        "biome_allow": (s.get("biome_allow")
                                        if isinstance(s.get("biome_allow"), list)
                                        else None),
                        "biome_rules": ({k: v for k, v in s["biome_rules"].items()
                                         if k != "__illegible__"}
                                        if isinstance(s.get("biome_rules"), dict)
                                        else {}),
                    }
                )
            return self._json(out)

        if path == "/api/spec":
            p = SPECS / f"{Path(q.get('name', '')).name}.json"
            if not p.exists():
                return self._json({"error": "no such spec"}, 404)
            s, rep = specmod.load(p)
            return self._json({"spec": s, "warnings": rep.warnings,
                               "hash": specmod.spec_hash(s),
                               "curation": specmod.curation(s)})

        if path == "/api/job":
            job = FORGE.get(q.get("job", ""))
            if not job:
                return self._json({"error": "unknown job"}, 404)
            return self._json(job.progress())

        if path == "/api/tile":
            job = FORGE.get(q.get("job", ""))
            if not job:
                return self._json({"error": "unknown job"}, 404)
            tile = job.tiles.get(int(q.get("seed", -1)))
            if tile is None:
                return self._json({"pending": True}, 202)
            if tile.error:
                return self._json({"error": tile.error}, 500)
            return self._send(200, tile.png, "image/png", cache=True)

        if path == "/api/detail":
            # Regenerated rather than cached -- deterministic build means this
            # is exactly the tree the thumbnail showed.
            job = FORGE.get(q.get("job", ""))
            spec = job.spec if job else self._spec_from_query(q)
            # Same tier as the 3D viewer: inspecting shows the finest voxel
            # size that is practical, exporting always uses the authored one.
            tree = pipeline.build(spec, int(q["seed"]),
                                  resolution_cm=viewer_resolution(spec))
            buf = io.BytesIO()
            # The SAME camera the gallery tile used. These were different
            # -- tiles went through `camera_for` and the detail overlay was
            # hardcoded to the isometric -- so clicking a rock changed the
            # angle it was being judged from without saying so.
            render.view(tree.grid, render.camera_for(spec),
                        target_px=DETAIL_PX).save(buf, "PNG")
            return self._send(200, buf.getvalue(), "image/png", cache=True)

        if path == "/api/voxels":
            # Binary surface voxels for the 3D viewer. Regenerated from
            # (spec, seed) like the detail render -- deterministic build means
            # this is exactly the tree the thumbnail showed.
            job = FORGE.get(q.get("job", ""))
            grid = None
            if job:
                spec, seed = job.spec, int(q["seed"])
            else:
                d = library_dir(Path(q.get("id", "")).name)
                if not d:
                    return self._json({"error": "unknown tree"}, 404)
                spec, _ = specmod.load(d / "spec.json")
                meta = json.loads((d / "meta.json").read_text(encoding="utf-8"))
                seed = meta["seed"]
                if meta.get("imported"):
                    # No (spec, seed) regenerates an import -- the stored
                    # voxels ARE the asset, so serve those.
                    grid = vxa.read(d / "tree.vxa")
            try:
                cap = int(q["max"]) if q.get("max") else None
            except ValueError:
                cap = None
            if grid is None:
                cm = viewer_resolution(spec, cap)
                grid = pipeline.build(spec, seed, connectivity=False,
                                      resolution_cm=cm).grid
            else:
                cm = grid.voxel_m * 100.0
            body = encode_voxels(grid)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("X-Voxel-Cm", f"{cm:g}")
            self.send_header("X-Authored-Cm", f"{float(specmod.get(spec, 'resolution_cm')):g}")
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
            self.end_headers()
            return self.wfile.write(body)

        if path == "/api/library":
            return self._json(library_list())

        if path == "/api/library/spec":
            d = library_dir(Path(q.get("id", "")).name)
            if not d:
                return self._json({"error": "not found"}, 404)
            spec, _ = specmod.load(d / "spec.json")
            meta = json.loads((d / "meta.json").read_text(encoding="utf-8"))
            return self._json({"spec": spec, "seed": meta.get("seed", 1),
                               "hash": specmod.spec_hash(spec)})

        if path == "/api/library/thumb":
            d = library_dir(Path(q.get("id", "")).name)
            if not d or not (d / "thumb.png").exists():
                return self._json({"error": "not found"}, 404)
            return self._send(200, (d / "thumb.png").read_bytes(), "image/png", cache=True)

        if path == "/api/download":
            d = library_dir(Path(q.get("id", "")).name)
            fmt = q.get("fmt", "vox")
            fname = {"vox": "tree.vox", "vxa": "tree.vxa", "spec": "spec.json"}.get(fmt)
            if not d or not fname or not (d / fname).exists():
                return self._json({"error": "not found"}, 404)
            body = (d / fname).read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.send_header(
                "Content-Disposition", f'attachment; filename="{d.name}.{fmt}"'
            )
            self.end_headers()
            return self.wfile.write(body)

        if not path.startswith("/api/"):
            # SPA fallback: any non-API path is a client-side route of the
            # React app; serve the shell and let it route.
            return self._static("index.html")
        return self._json({"error": "no such route"}, 404)

    def do_POST(self) -> None:
        url = urlparse(self.path)
        try:
            self._route_post(url.path, self._body())
        except BrokenPipeError:
            pass
        except Exception:
            self._json({"error": traceback.format_exc(limit=4)}, 500)

    def _route_post(self, path: str, body: dict) -> None:
        if path == "/api/generate":
            spec, rep = specmod.validate(body.get("spec") or {})
            start = int(body.get("seed_start", 1))
            count = max(1, min(int(body.get("count", 12)), 200))
            job = FORGE.start(spec, list(range(start, start + count)))
            return self._json(
                {"job": job.id, "seeds": job.seeds, "warnings": rep.warnings,
                 "hash": specmod.spec_hash(spec)}
            )

        if path == "/api/validate":
            spec, rep = specmod.validate(body.get("spec") or {})
            return self._json({"spec": spec, "warnings": rep.warnings,
                               "hash": specmod.spec_hash(spec)})

        if path == "/api/interpret":
            from . import language

            spec, _ = specmod.validate(body.get("spec") or {})
            request = str(body.get("request", "")).strip()
            if not request:
                return self._json({"error": "say what you want changed"}, 400)
            return self._json(language.interpret(spec, request))

        if path == "/api/save-spec":
            spec, rep = specmod.validate(body.get("spec") or {})
            name = Path(str(specmod.get(spec, "name") or "unnamed")).name
            if not name or name in (".", ".."):
                return self._json({"error": "bad species name"}, 400)
            specmod.save(spec, SPECS / f"{name}.json")
            return self._json({"saved": f"{name}.json", "warnings": rep.warnings})

        if path == "/api/curation":
            # The publish verdict, written into the spec FILE and nowhere
            # else. The exporters read the gate from specs/, so a verdict
            # held in server memory or a sidecar would be one more derived
            # copy waiting to detach from its source.
            name = Path(str(body.get("name", ""))).name
            p = SPECS / f"{name}.json"
            if not name or name in (".", "..") or not p.is_file():
                return self._json({"error": "no such spec"}, 404)
            status = str(body.get("status", ""))
            if status not in specmod.CURATION_STATUSES:
                return self._json(
                    {"error": f"status must be one of {specmod.CURATION_STATUSES}"}, 400)
            try:
                seeds = sorted({int(s) for s in (body.get("seeds") or [])})
            except (TypeError, ValueError):
                return self._json({"error": "seeds must be whole numbers"}, 400)
            if not seeds or not all(1 <= s <= 9999 for s in seeds):
                # An approved species with no seeds would be published with an
                # empty bank; refuse the write instead of letting the resolver
                # quietly substitute the default later.
                return self._json({"error": "at least one seed, each 1-9999"}, 400)
            # The RAW file, not the validated body: validate re-clamps every
            # parameter it touches, and this route's whole contract is that
            # nothing but the curation block moves. The dump matches
            # `spec.save`'s byte format, so the diff IS the block.
            raw = json.loads(p.read_text(encoding="utf-8"))
            raw["curation"] = {"status": status, "seeds": seeds,
                               "notes": str(body.get("notes", ""))}
            p.write_text(json.dumps(raw, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
            return self._json({"saved": f"{name}.json",
                               "curation": specmod.curation(raw)})

        if path == "/api/keep":
            spec, _ = specmod.validate(body.get("spec") or {})
            meta = keep(spec, int(body["seed"]))
            return self._json(meta)

        if path == "/api/library/delete":
            d = library_dir(Path(str(body.get("id", ""))).name)
            if not d:
                return self._json({"error": "not found"}, 404)
            for f in sorted(d.iterdir()):
                if f.is_file():
                    f.unlink()
            d.rmdir()
            # Drop the species folder too once its last entry is gone, so the
            # library does not accumulate empty directories.
            parent = d.parent
            if parent != LIBRARY and not any(parent.iterdir()):
                parent.rmdir()
            return self._json({"deleted": d.name})

        if path == "/api/sheet":
            # Contact sheet of the current job, written to out/.
            job = FORGE.get(body.get("job", ""))
            if not job:
                return self._json({"error": "unknown job"}, 404)
            cells = []
            from PIL import Image

            for seed in job.seeds:
                t = job.tiles.get(seed)
                if not t or not t.png:
                    continue
                st = t.stats or {}
                cells.append(
                    (
                        Image.open(io.BytesIO(t.png)).convert("RGBA"),
                        f"seed {seed}   {st.get('height_m', 0):.1f} m",
                        t.problems,
                    )
                )
            if not cells:
                return self._json({"error": "nothing generated yet"}, 400)
            name = specmod.get(job.spec, "name")
            img = contact.sheet(
                cells,
                title=f"{name}  x{len(cells)}",
                subtitle=f"spec {specmod.spec_hash(job.spec)}",
                columns=min(8, len(cells)),
            )
            p = contact.save(img, ROOT / "out" / name / f"{name}-sheet.png")
            return self._json({"path": str(p)})

        if path == "/api/placement":
            # Placement for one species: biome weights, the allowlist, and the
            # per-biome rule attachments. Same contract as /api/curation: the
            # RAW spec file, and only the blocks present in the request move.
            #   weights      {biome: 0..1}  DELTAS -- only the changed keys;
            #                0 removes the key from the biomes block
            #   biome_allow  list | null    whole block; null removes it
            #                (allowlist back to derived-from-weights)
            #   biome_rules  {biome: [rule names]} | null   whole block
            name = Path(str(body.get("name", ""))).name
            p = SPECS / f"{name}.json"
            if not name or name in (".", "..") or not p.is_file():
                return self._json({"error": "no such spec"}, 404)
            s, _ = specmod.load(p)
            kind = specmod.get(s, "kind")
            hosts = tuple(b.key for b in biomelib.BIOMES if kind in b.hosts)
            raw = json.loads(p.read_text(encoding="utf-8"))

            if "weights" in body:
                deltas = body["weights"]
                if not isinstance(deltas, dict):
                    return self._json({"error": "weights must map biome -> 0..1"}, 400)
                block = raw.get("biomes")
                if not isinstance(block, dict):
                    block = {}
                for k, v in deltas.items():
                    if k not in hosts:
                        return self._json(
                            {"error": f"{k!r} does not host kind {kind!r} "
                                      f"(menu: {list(hosts)})"}, 400)
                    try:
                        w = float(v)
                    except (TypeError, ValueError):
                        return self._json({"error": f"weight for {k} must be a number"}, 400)
                    if not (0.0 <= w <= 1.0):
                        return self._json({"error": f"weight for {k} must be 0..1"}, 400)
                    if w <= 0:
                        block.pop(k, None)
                    else:
                        block[k] = round(w, 4)
                if block:
                    raw["biomes"] = block
                else:
                    raw.pop("biomes", None)

            if "biome_allow" in body:
                allow = body["biome_allow"]
                if allow is None:
                    raw.pop("biome_allow", None)
                elif isinstance(allow, list):
                    bad = [k for k in allow if k not in hosts]
                    if bad:
                        return self._json(
                            {"error": f"biome_allow: {bad} do not host kind "
                                      f"{kind!r} (menu: {list(hosts)})"}, 400)
                    raw["biome_allow"] = sorted(set(allow))
                else:
                    return self._json(
                        {"error": "biome_allow must be a list of biome keys, "
                                  "or null to derive from the weights"}, 400)

            if "biome_rules" in body:
                br = body["biome_rules"]
                if br is None:
                    raw.pop("biome_rules", None)
                elif isinstance(br, dict):
                    known = rules_doc().get("rules", {})
                    cleaned: dict = {}
                    for bkey, names in br.items():
                        if bkey not in biomelib.BY_KEY:
                            return self._json({"error": f"{bkey!r} is not a biome key"}, 400)
                        if not isinstance(names, list):
                            return self._json(
                                {"error": f"biome_rules.{bkey} must be a list of rule names"}, 400)
                        # Referential check AT AUTHORING TIME: the export
                        # refuses dangling names, so the UI must never be able
                        # to write one.
                        missing = [n for n in names if n not in known]
                        if missing:
                            return self._json(
                                {"error": f"unknown rule(s) {missing}; author them "
                                          f"in the rule library first"}, 400)
                        if names:
                            cleaned[bkey] = sorted(set(names))
                    if cleaned:
                        raw["biome_rules"] = cleaned
                    else:
                        raw.pop("biome_rules", None)
                else:
                    return self._json(
                        {"error": "biome_rules must map biome -> [rule names], "
                                  "or null to clear"}, 400)

            p.write_text(json.dumps(raw, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
            s2, rep = specmod.load(p)
            return self._json({
                "saved": f"{name}.json",
                "weights": biomelib.weights(s2),
                "biome_allow": (s2.get("biome_allow")
                                if isinstance(s2.get("biome_allow"), list) else None),
                "biome_rules": ({k: v for k, v in s2["biome_rules"].items()
                                 if k != "__illegible__"}
                                if isinstance(s2.get("biome_rules"), dict) else {}),
                "allowed": list(biomelib.allowed(s2)),
                "warnings": rep.warnings,
            })

        if path == "/api/rules/save":
            # Create or update ONE named rule in rules/placement-rules.json.
            # The raw document is edited in place, so the diff is the rule.
            name = str(body.get("name", ""))
            if not specmod.RULE_NAME_RE.match(name):
                return self._json(
                    {"error": "rule name must be 1-31 chars of [A-Za-z0-9_-] "
                              "(it is wire data)"}, 400)
            cleaned, err = clean_rule_body(body.get("rule"))
            if err:
                return self._json({"error": err}, 400)
            doc = rules_doc()
            doc.setdefault("rules", {})[name] = cleaned
            RULES_DIR.mkdir(parents=True, exist_ok=True)
            PLACEMENT_RULES.write_text(json.dumps(doc, indent=2) + "\n",
                                       encoding="utf-8")
            return self._json({"saved": name, "rules": doc["rules"]})

        if path == "/api/rules/delete":
            # Deleting a rule any spec still cites would leave dangling names
            # the export then refuses -- so the delete refuses FIRST, by name.
            name = str(body.get("name", ""))
            doc = rules_doc()
            if name not in doc.get("rules", {}):
                return self._json({"error": "no such rule"}, 404)
            refs = rule_references().get(name, [])
            if refs:
                return self._json(
                    {"error": f"rule {name!r} is attached to {len(refs)} species; "
                              f"detach it everywhere first",
                     "referenced": refs}, 409)
            del doc["rules"][name]
            PLACEMENT_RULES.write_text(json.dumps(doc, indent=2) + "\n",
                                       encoding="utf-8")
            return self._json({"deleted": name, "rules": doc["rules"]})

        if path == "/api/import":
            # An asset from an outside 3D source, into the library, with a
            # spec file so it is curated and placement-specced identically to
            # a generated species.
            import base64

            name = str(body.get("name", ""))
            if not SPECIES_NAME_RE.match(name):
                return self._json(
                    {"error": "species name must be lowercase letters, digits "
                              "and dashes (up to 40)"}, 400)
            kind = str(body.get("kind", ""))
            if kind not in {k.key for k in kindlib.KINDS}:
                return self._json(
                    {"error": f"kind must be one of "
                              f"{[k.key for k in kindlib.KINDS]}"}, 400)
            fmt = str(body.get("format", ""))
            try:
                blob = base64.b64decode(str(body.get("data_b64", "")), validate=True)
            except (ValueError, TypeError):
                return self._json({"error": "data_b64 is not valid base64"}, 400)
            if not blob:
                return self._json({"error": "the file is empty"}, 400)
            if len(blob) > 64 << 20:
                return self._json({"error": "file too large (64 MB cap)"}, 400)
            try:
                if fmt == "vxa":
                    import tempfile

                    tmp = None
                    try:
                        with tempfile.NamedTemporaryFile(
                                suffix=".vxa", delete=False) as f:
                            f.write(blob)
                            tmp = f.name
                        grid = vxa.read(tmp)
                    finally:
                        if tmp:
                            os.unlink(tmp)
                elif fmt == "vox":
                    voxel_mm = int(body.get("voxel_mm", 100))
                    if not (10 <= voxel_mm <= 1000):
                        return self._json({"error": "voxel_mm must be 10-1000"}, 400)
                    grid = grid_from_vox(blob, voxel_mm)
                else:
                    return self._json({"error": "format must be 'vox' or 'vxa'"}, 400)
            except ValueError as e:
                return self._json({"error": str(e)}, 400)
            return self._json(import_asset(name, kind, grid, fmt))

        return self._json({"error": "no such route"}, 404)

    def _all_specs(self) -> list[tuple[Path, dict]]:
        out = []
        for p in sorted(SPECS.glob("*.json")):
            try:
                s, _ = specmod.load(p)
            except (OSError, json.JSONDecodeError):
                continue
            out.append((p, s))
        return out

    def _spec_from_query(self, q: dict) -> dict:
        p = SPECS / f"{Path(q.get('name', '')).name}.json"
        s, _ = specmod.load(p)
        return s

    def _static(self, rel: str) -> None:
        # web/dist (the built React app) when it exists; the legacy page in
        # forge/web only as a fallback for a checkout that never ran a build.
        root = DIST if (DIST / "index.html").is_file() else LEGACY_WEB
        target = (root / rel).resolve()
        if root.resolve() not in target.parents or not target.is_file():
            return self._json({"error": "not found"}, 404)
        ctype = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        # Vite bundles are content-hashed, so they can cache forever.
        self._send(200, target.read_bytes(), ctype, cache=rel.startswith("assets/"))


class Server(ThreadingHTTPServer):
    """Quiet about clients that hang up.

    A browser that navigates away mid-request, and a phone that sleeps its wifi,
    both drop the connection, and stdlib's default is a full traceback per drop.
    On a phone those come in bursts, and a console full of ConnectionResetError
    buries whatever real error you were watching for.
    """

    daemon_threads = True

    def handle_error(self, request, client_address):
        import sys

        if isinstance(sys.exc_info()[1], (ConnectionResetError, ConnectionAbortedError,
                                          BrokenPipeError)):
            return
        super().handle_error(request, client_address)


def lan_address() -> str | None:
    """This machine's address on the local network, for reaching it by phone.

    Uses a UDP socket to a routable address to find which interface the OS would
    route out of. Nothing is sent -- connect() on UDP only sets the peer -- but
    it picks the right interface on a box with several, which reading the
    hostname does not.
    """
    import socket

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 1))     # TEST-NET-1: reserved, never routed
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def serve(host: str = "127.0.0.1", port: int = 8731, open_browser: bool = True) -> None:
    LIBRARY.mkdir(parents=True, exist_ok=True)
    httpd = Server((host, port), Handler)
    url = f"http://{'127.0.0.1' if host in ('', '0.0.0.0') else host}:{port}/"
    print(f"asset-forge  {url}")
    if host in ("", "0.0.0.0"):
        lan = lan_address()
        print(f"  phone   http://{lan}:{port}/" if lan else
              "  phone   (no LAN address found)")
    else:
        # Loopback-only is the default on purpose -- the app writes files and
        # runs unauthenticated -- so say how to reach it from a phone rather
        # than leaving it looking broken.
        print("  phone   not reachable; restart with --host 0.0.0.0 to allow "
              "devices on your network")
    print(f"  specs   {SPECS}")
    print(f"  library {LIBRARY}")
    print("  ctrl-c to stop")
    if open_browser:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        httpd.server_close()
