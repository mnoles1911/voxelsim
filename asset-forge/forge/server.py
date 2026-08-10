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

from . import (biomes as biomelib, contact, kinds as kindlib, materials, pipeline,
               render, spec as specmod, vox, vxa)

ROOT = Path(__file__).resolve().parent.parent
WEB = Path(__file__).resolve().parent / "web"
SPECS = ROOT / "specs"
LIBRARY = ROOT / "library"

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
        scale = render.scale_for(
            [render.predicted_extent(spec, preview_cm / 100.0)], TILE_PX
        )
        job = Job(id=uuid.uuid4().hex[:12], spec=spec, seeds=seeds, scale=scale,
                  preview_cm=preview_cm)
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
            key = (digest, seed, job.scale, job.preview_cm)
            tile = self._cache_get(key)
            if tile is None:
                tile = Tile(seed=seed)
                try:
                    tree = pipeline.build(job.spec, seed, resolution_cm=job.preview_cm)
                    img = render.render(tree.grid, scale=job.scale)
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
    vxa.write(tree.grid, out / "tree.vxa")
    render.render(tree.grid, target_px=DETAIL_PX).save(out / "thumb.png")
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
                                "size_m": (specmod.get(s, "rock.size_m") if kind == "rock"
                                           else specmod.get(s, "height_m")),
                                "model": (kind if kind == "rock"
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
                        "size_m": (specmod.get(s, "rock.size_m") if kind == "rock"
                                   else specmod.get(s, "height_m")),
                        "height_m": specmod.get(s, "height_m"),
                        "resolution_cm": specmod.get(s, "resolution_cm"),
                        "shape": (specmod.get(s, "materials.rock") if kind == "rock"
                                  else specmod.get(s, "crown.shape")),
                        "notes": specmod.get(s, "notes"),
                        "biomes": biomelib.summary(s),
                    }
                )
            return self._json(out)

        if path == "/api/spec":
            p = SPECS / f"{Path(q.get('name', '')).name}.json"
            if not p.exists():
                return self._json({"error": "no such spec"}, 404)
            s, rep = specmod.load(p)
            return self._json({"spec": s, "warnings": rep.warnings,
                               "hash": specmod.spec_hash(s)})

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
            render.render(tree.grid, target_px=DETAIL_PX).save(buf, "PNG")
            return self._send(200, buf.getvalue(), "image/png", cache=True)

        if path == "/api/voxels":
            # Binary surface voxels for the 3D viewer. Regenerated from
            # (spec, seed) like the detail render -- deterministic build means
            # this is exactly the tree the thumbnail showed.
            job = FORGE.get(q.get("job", ""))
            if job:
                spec, seed = job.spec, int(q["seed"])
            else:
                d = library_dir(Path(q.get("id", "")).name)
                if not d:
                    return self._json({"error": "unknown tree"}, 404)
                spec, _ = specmod.load(d / "spec.json")
                seed = json.loads((d / "meta.json").read_text(encoding="utf-8"))["seed"]
            try:
                cap = int(q["max"]) if q.get("max") else None
            except ValueError:
                cap = None
            cm = viewer_resolution(spec, cap)
            tree = pipeline.build(spec, seed, connectivity=False, resolution_cm=cm)
            body = encode_voxels(tree.grid)
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
        target = (WEB / rel).resolve()
        if WEB.resolve() not in target.parents or not target.is_file():
            return self._json({"error": "not found"}, 404)
        ctype = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        self._send(200, target.read_bytes(), ctype)


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
