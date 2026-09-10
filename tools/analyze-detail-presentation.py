"""Analyze a pinned offline detail manifest; proposal only, never edits assets.
Stdlib only. Package bytes are serialized file sizes, not GPU allocation sizes.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import statistics


def distribution(values):
    values = sorted(values)
    return {"min": values[0], "median": statistics.median(values),
            "p90": values[math.ceil(.9 * len(values)) - 1], "max": values[-1]}


def analyze(manifest, maximum=256., minimum=32., metres_per_size=128.):
    if not (0 < maximum <= 512 and minimum > 0) or metres_per_size <= 0:
        raise ValueError("Require 0 < configured maximum <= 512 and positive floor/distance scale")
    data = json.loads(manifest.read_text(encoding="utf-8-sig"))
    rows = []
    for model in data["models"]:
        facts = model["mesh_facts"]
        match = re.search(r"BoxExtent=\(X=([\d.e+-]+) Y=([\d.e+-]+) Z=([\d.e+-]+)\)", facts["bounds"])
        if not match:
            raise ValueError(f"Unrecognized bounds: {model['id']}")
        width, depth, height = [float(v) * .02 for v in match.groups()]
        if any(not math.isfinite(v) or v <= 0 for v in (width, depth, height)):
            raise ValueError("Non-positive/nonfinite bounds")
        size = max(height, .5 * max(width, depth))
        end = min(maximum, max(min(minimum, maximum), 16 * math.ceil(size * metres_per_size / 16)))
        lods = facts["lods"]
        # Conservative 32-bit indices; assumed packed high-precision tangent/UV
        # layout. Actual engine buffers, alignment and CPU copies are not known.
        estimated = sum(l["vertices"] * (12 + 16 + 4 + 8*l["uv_channels"]) +
                        4 * sum(l.get(k, 0) for k in ("main_indices", "depth_indices", "reversed_indices", "reversed_depth_indices", "wireframe_indices")) for l in lods)
        package = Path(model["package_file"])
        rows.append({"id": model["id"], "species": model["species"],
                     "voxel_pitch_um": model["voxel_pitch_um"],
                     "bounds_width_m": width, "bounds_depth_m": depth, "bounds_height_m": height,
                     "presentation_size_m": size, "proposed_end_distance_m": end,
                     "uniform_area_fraction_vs_max": (end/maximum)**2,
                     "lods": [{k:l[k] for k in ("vertices", "triangles", "uv_channels", "screen_size")} for l in lods],
                     "lod0_triangles": lods[0]["triangles"], "last_lod_triangles": lods[-1]["triangles"],
                     "last_to_first_triangle_ratio": lods[-1]["triangles"]/lods[0]["triangles"],
                     "serialized_mesh_package_bytes": package.stat().st_size if package.exists() else None,
                     "assumed_packed_buffer_bytes_all_lods": estimated})
    if not rows:
        raise ValueError("No mesh rows")
    return {"schema": 1, "scope": "Private fixture presentation-distance proposal, not applied or accepted",
            "manifest": str(manifest.resolve()), "manifest_sha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
            "preview_only": data.get("preview_only"), "model_count": len(rows), "species_count": len({r['species'] for r in rows}),
            "policy": {"max_distance_m": maximum, "min_distance_m": minimum, "metres_per_size_m": metres_per_size,
                       "formula": "min(maximum,max(min(minimum,maximum),ceil(metres_per_size*max(height,.5*max(width,depth))/16)*16))",
                       "notes": "Wind-expanded mesh bounds; leaves source pitch, ecology query/residency radius and all source banks unchanged"},
            "lod_count_distribution": dict(Counter(len(r['lods']) for r in rows)),
            "end_distance_distribution": dict(sorted(Counter(r['proposed_end_distance_m'] for r in rows).items())),
            "height_m": distribution([r['bounds_height_m'] for r in rows]),
            "lod0_triangles": distribution([r['lod0_triangles'] for r in rows]),
            "last_lod_triangles": distribution([r['last_lod_triangles'] for r in rows]),
            "sum_serialized_mesh_package_bytes": sum(r['serialized_mesh_package_bytes'] or 0 for r in rows),
            "missing_package_sizes": sum(r['serialized_mesh_package_bytes'] is None for r in rows),
            "sum_assumed_packed_buffer_bytes_all_lods": sum(r['assumed_packed_buffer_bytes_all_lods'] for r in rows),
            "equal_variant_uniform_area_reduction_fraction": 1-statistics.mean(r['uniform_area_fraction_vs_max'] for r in rows),
            "area_warning": "Geometric circle-area proxy with equal variant weights and uniform density; NOT measured instance/GPU/frame-time/memory reduction. Actual regional abundance, occlusion and LOD matter.",
            "resource_warning": "Serialized package bytes and assumed packed buffers are NOT measured CPU/RHI/VRAM usage. CPU-access duplicate arrays, materials, HISM data, engine metadata and allocation alignment excluded.",
            "largest_triangle_models": sorted(rows,key=lambda r:r['lod0_triangles'],reverse=True)[:12],
            "single_lod_models": [r['id'] for r in rows if len(r['lods'])==1], "models": rows}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--maximum',type=float,default=256);p.add_argument('--minimum',type=float,default=32)
    p.add_argument('--metres-per-size',type=float,default=128)
    a=p.parse_args();result=analyze(a.manifest,a.maximum,a.minimum,a.metres_per_size)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('models','largest_triangle_models')},indent=2))

if __name__=='__main__':main()
