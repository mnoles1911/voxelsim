"""Read-only placement inventory compiled from explicitly endorsed library assets.

This does not endorse, publish, regenerate or copy assets. Runtime must bind the
named bank file and its digest, never reinterpret the authored seed as a slot.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stable_id(name: str) -> str:
    # String avoids JSON/JavaScript's 53-bit integer precision limit.
    return hashlib.sha256(name.encode("utf-8")).digest()[:8].hex()


def geometry(blob: bytes) -> dict:
    if len(blob) < 48 or blob[:4] != b"VXA1":
        raise ValueError("invalid VXA header")
    version, ox, oy, oz, nx, ny, nz, pitch, runs, parts, joints = struct.unpack_from("<IiiiIIIIIII", blob, 4)
    if version not in (3, 4) or not min(nx, ny, nz, pitch, runs):
        raise ValueError("invalid VXA geometry dimensions/version")
    if parts or joints:
        raise ValueError("rigged assets are outside environment placement")
    if len(blob) != 48 + runs * 5:
        raise ValueError("VXA payload size mismatch")
    if nx * ny * nz > 512 * 1024 * 1024:
        raise ValueError("VXA extent exceeds inventory limit")
    count = 0
    for offset in range(48, len(blob), 5):
        material, length = struct.unpack_from("<BI", blob, offset)
        if not length or material >= 47:
            raise ValueError("invalid material run")
        count += length
    if count != nx * ny * nz:
        raise ValueError("VXA run coverage mismatch")
    pitch_um = pitch * 1000 if version == 3 else pitch
    if pitch_um not in (25000, 50000, 100000):
        raise ValueError("environment voxel pitch must be 25, 50 or 100 mm")
    def mm(cells: int) -> int:
        return (cells * pitch_um + 999) // 1000
    return {"voxel_pitch_um": pitch_um, "origin_vox": [ox, oy, oz],
            "extent_vox": [nx, ny, nz], "height_mm": mm(nz),
            "above_anchor_mm": mm(max(0, oz + nz)),
            "below_anchor_mm": mm(max(0, -oz)),
            "bounds_radius_mm": mm(max(abs(ox), abs(oy), abs(ox + nx), abs(oy + ny)))}


def compile_inventory(root: Path) -> dict:
    root = root.resolve()
    library = root / "library"
    engine = root / "out" / "engine"
    published_path = engine / "appearance" / "published.json"
    publication_bytes = published_path.read_bytes()
    published = json.loads(publication_bytes)
    rows, refused = [], []
    seen = set()
    for model in published["models"]:
        model_id, species = model["id"], model["species"]
        try:
            if model_id in seen:
                raise ValueError("duplicate published model identity")
            seen.add(model_id)
            if any(Path(s).name != s or "/" in s or "\\" in s or s in (".", "..")
                   for s in (model_id, species)):
                raise ValueError("invalid library identity")
            source = library / species / model_id
            meta = json.loads((source / "meta.json").read_text(encoding="utf-8"))
            if meta.get("kind") not in ("tree", "rock", "bush", "grass", "reed", "flower"):
                continue
            if (meta.get("inventory_candidate") or meta.get("review_status") != "endorsed"
                    or not meta.get("visual_approved")):
                raise ValueError("asset is not explicitly endorsed")
            if (meta.get("id"), meta.get("species"), meta.get("seed")) != (model_id, species, model["seed"]):
                raise ValueError("library/publication identity mismatch")
            blob = (source / "tree.vxa").read_bytes()
            geometry_hash = digest(blob)
            if geometry_hash != model["geometry_sha256"]:
                raise ValueError("library geometry differs from publication")
            bank = engine / "banks" / species / (model_id + ".vxa")
            if bank.read_bytes() != blob:
                raise ValueError("bank geometry differs from endorsed library source")
            measured = geometry(blob)
            if meta["kind"] in ("tree", "rock") and measured["voxel_pitch_um"] != 100000:
                raise ValueError("terrain asset is not on the 100 mm lattice")
            source_record = library / species / "species.json"
            if not source_record.is_file():
                raise ValueError("authoritative species record missing")
            stats = meta.get("stats", {})
            rows.append({"species": species, "species_stable_id": stable_id("species:" + species),
                         "id": model_id, "variant_stable_id": stable_id("variant:" + model_id),
                         "asset_seed": model["seed"], "kind": meta["kind"],
                         "bank_file": bank.relative_to(engine).as_posix(),
                         "geometry_sha256": geometry_hash,
                         "geometry_md5": hashlib.md5(blob).hexdigest(),
                         "species_record_sha256": digest(source_record.read_bytes()),
                         "size_class": stats.get("size_class"), "growth_form": stats.get("growth_form"),
                         "geometry": measured})
        except (ValueError, KeyError, OSError, struct.error) as exc:
            refused.append({"id": model_id, "reason": str(exc)})
    rows.sort(key=lambda r: (r["species"], r["id"]))
    available = {r["species"] for r in rows}
    unrepresented = []
    for record in sorted(library.glob("*/species.json")):
        body = json.loads(record.read_text(encoding="utf-8"))
        spec = body.get("baseline_spec", {})
        if spec.get("kind") not in ("tree", "rock", "bush", "grass", "reed", "flower"):
            continue
        if record.parent.name not in available:
            unrepresented.append(record.parent.name)
    return {"schema_version": 1, "status": "inventory_only_not_runtime_configuration",
            "publication_sha256": digest(publication_bytes), "variants": rows,
            "variant_count": len(rows), "species_count": len(available), "refused": refused,
            "source_species_without_published_variant": unrepresented,
            "notes": ["Bounds are measured; crown opacity and root spacing require ecological traits.",
                      "asset_seed is not a bank slot; bind bank_file and geometry_sha256 at runtime.",
                      "No endorsement, publication or source files were modified."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "ecological-placement" / "inventory.json")
    args = parser.parse_args()
    result = compile_inventory(args.root)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("variant_count", "species_count", "refused")}))
    raise SystemExit(1 if result["refused"] else 0)
