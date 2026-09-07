"""Remove the inspected museum perch by location and source texture color.

This is specific to MP 040, not a generic bird segmentation rule. Review the
result before installation: toes touch the support and need a separate check.
"""
import json
from pathlib import Path
import numpy as np
import trimesh

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/creature-reconstruction/common-raven-master'
scene = trimesh.load(OUT / 'with-perch.glb', force='scene')
removed = 0
for mesh in scene.geometry.values():
    centers = mesh.triangles_center
    uv = mesh.visual.uv[mesh.faces].mean(1)
    rgb = mesh.visual.material.to_color(uv)[:, :3].astype(int)
    brown = (rgb[:, 0]-rgb[:, 1] > 6) & (rgb[:, 0]-rgb[:, 2] > 14)
    perch = (centers[:, 1] < .14) | ((centers[:, 1] < .36) & brown)
    removed += int(perch.sum())
    mesh.update_faces(~perch)
    mesh.remove_unreferenced_vertices()
scene.export(OUT / 'perch-removal-study.glb')
(OUT / 'perch-removal.json').write_text(json.dumps(dict(
    removed_faces=removed, visual_approved=False,
    method='MP 040 only: lower support cutoff and warm brown texture below 0.36 source metres',
    remaining_checks=['all toe silhouettes', 'remaining support fragments', 'source shell continuity']), indent=2)+'\n')
print(removed)
