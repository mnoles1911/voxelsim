"""Experimental GLB to 12.5 mm voxel comparison; never publishes assets.

Requires explicit physical length. Input uses glTF Y-up coordinates. Textures
are sampled into a research RGB sidecar, not claimed to be engine materials.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import trimesh
from PIL import Image, ImageDraw
from scipy import ndimage

from reconstruct_pilot import ortho


def write_views(output, cells, surface_cells, rgb, pitch):
    """Export matched sRGB orthographic images and linear-color cubic GLB."""
    lower = cells.min(0) - 1
    shape = cells.max(0) - lower + 2
    occupied = np.zeros(shape, bool)
    occupied[tuple((cells-lower).T)] = True
    indices = surface_cells - lower
    full = np.zeros((*shape, 3), np.uint8)
    full[tuple(indices.T)] = rgb
    # Exposed cube faces preserve the actual cubic surface in oblique renders.
    vertices, faces, colors = [], [], []
    for axis in range(3):
        u, v = [i for i in range(3) if i != axis]
        for sign in (-1, 1):
            adjacent = indices.copy(); adjacent[:, axis] += sign
            exposed = ~occupied[tuple(adjacent.T)]
            selected = indices[exposed]
            corners = np.zeros((4, 3))
            corners[:, axis] = sign*.5
            corners[:, u] = [-.5, .5, .5, -.5]
            corners[:, v] = [-.5, -.5, .5, .5]
            quad = ((selected+lower)[:, None, :] + corners)*pitch
            base = len(vertices)
            vertices.extend(quad.reshape(-1, 3))
            # Make winding consistent with the exposed face normal.
            pattern = np.array([[0, 1, 2], [0, 2, 3]])
            if (1 if axis != 1 else -1) != sign:
                pattern = pattern[:, ::-1]
            faces.extend((np.arange(len(selected))[:, None, None]*4 + base + pattern).reshape(-1, 3))
            colors.extend(np.repeat(full[tuple(selected.T)], 4, axis=0))
    # glTF vertex COLOR_0 is linear, while the image/RGB sidecar is sRGB.
    srgb = np.asarray(colors, dtype=float)/255
    linear = np.where(srgb <= .04045, srgb/12.92, ((srgb+.055)/1.055)**2.4)
    voxel_mesh = trimesh.Trimesh(vertices=vertices, faces=faces,
        vertex_colors=np.c_[np.rint(linear*255).astype(np.uint8), np.full(len(colors), 255)], process=False)
    voxel_mesh.apply_transform(np.array([[1,0,0,0],[0,0,1,0],[0,-1,0,0],[0,0,0,1]]))
    voxel_mesh.export(output/'colored-voxels.glb')
    sheet = Image.new('RGB', (2100, 1460), (223, 226, 229))
    draw = ImageDraw.Draw(sheet)
    for i, (name, axis, reverse) in enumerate([
        ('side A', 1, False), ('side B', 1, True), ('end A', 0, True),
        ('end B', 0, False), ('top', 2, True), ('bottom', 2, False)]):
        view = ortho(full, occupied, axis, reverse)
        view.save(output/f'view-{i}.png')
        x, y = i % 3 * 700, i // 3 * 730
        sheet.paste(view, (x, y + 30))
        draw.text((x + 20, y + 8), name, fill='black')
    sheet.save(output/'six-views.png')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--length-m', type=float, required=True)
    parser.add_argument('--long-axis', type=int, choices=(0, 2), required=True)
    parser.add_argument('--fill-union', action='store_true',
                        help='Fill the combined shell of scans split across mesh chunks')
    args = parser.parse_args()
    if args.length_m <= 0:
        parser.error('length must be positive')
    pitch = .0125
    scene = trimesh.load(args.source, force='scene')
    meshes = []
    # Preserve each instantiated geometry and its world transform.
    for node in scene.graph.nodes_geometry:
        transform, name = scene.graph[node]
        mesh = scene.geometry[name].copy()
        mesh.apply_transform(transform)
        mesh.update_faces(mesh.nondegenerate_faces())
        mesh.remove_unreferenced_vertices()
        if not len(mesh.faces):
            continue
        meshes.append(mesh)
    vertices = np.vstack([m.vertices for m in meshes])
    lo, hi = vertices.min(0), vertices.max(0)
    scale = args.length_m / (hi[args.long_axis] - lo[args.long_axis])
    order = [args.long_axis, 2 if args.long_axis == 0 else 0, 1]
    for mesh in meshes:
        mesh.vertices = (mesh.vertices - lo)[:, order] * scale
    cells = np.unique(np.vstack([
        np.rint(m.voxelized(pitch).fill().points / pitch).astype(np.int32)
        for m in meshes]), axis=0)
    lower = cells.min(0) - 1
    shape = cells.max(0) - lower + 2
    if np.prod(shape) > 32_000_000:
        raise ValueError('Research allocation exceeds 32 million cells')
    occupied = np.zeros(shape, bool)
    occupied[tuple((cells-lower).T)] = True
    if args.fill_union:
        occupied = ndimage.binary_fill_holes(occupied)
        cells = np.argwhere(occupied).astype(np.int32) + lower
    labels, _ = ndimage.label(occupied)
    components = sorted(np.bincount(labels.ravel())[1:].tolist(), reverse=True)
    surface = occupied & ~ndimage.binary_erosion(occupied)
    indices = np.argwhere(surface)
    points = (indices + lower) * pitch
    rgb = np.full((len(points), 3), 127, np.uint8)
    best = np.full(len(points), np.inf)
    textured = []
    for mesh in meshes:
        visual = mesh.visual
        textured.append(visual.kind == 'texture')
        for begin in range(0, len(points), 2048):
            end = min(begin + 2048, len(points))
            closest, distance, triangles = trimesh.proximity.closest_point(mesh, points[begin:end])
            replace = distance < best[begin:end]
            if not np.any(replace):
                continue
            if visual.kind == 'texture' and visual.uv is not None:
                # Integrate a small footprint instead of aliasing detailed fur
                # textures to a single point per 12.5 mm cube. Clamp samples
                # within the selected triangle so UV seams never interpolate.
                samples = []
                offsets = np.vstack([np.zeros(3), np.eye(3)*pitch*.35, -np.eye(3)*pitch*.35])
                for offset in offsets:
                    bary = trimesh.triangles.points_to_barycentric(mesh.triangles[triangles], closest+offset)
                    bary = np.clip(bary, 0, 1)
                    bary /= np.maximum(bary.sum(1, keepdims=True), 1e-12)
                    uv = (visual.uv[mesh.faces[triangles]] * bary[:, :, None]).sum(1)
                    color = visual.material.to_color(uv)[:, :3].astype(float)/255
                    samples.append(np.where(color <= .04045, color/12.92, ((color+.055)/1.055)**2.4))
                linear = np.mean(samples, axis=0)
                sampled = np.clip(np.where(linear <= .0031308, linear*12.92,
                    1.055*linear**(1/2.4)-.055)*255, 0, 255).astype(np.uint8)
            else:
                sampled = visual.to_color().face_colors[triangles, :3] if visual.kind == 'texture' else visual.face_colors[triangles, :3]
            rgb[begin:end][replace] = sampled[replace]
            best[begin:end][replace] = distance[replace]
    args.output.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.output/'surface-appearance.npz', cells=indices+lower,
                        rgb=rgb, occupied_cells=cells, voxel_m=pitch)
    write_views(args.output, cells, indices+lower, rgb, pitch)
    report = dict(source=args.source.name, fill_union=args.fill_union,
        source_sha256=hashlib.sha256(args.source.read_bytes()).hexdigest(),
        scale_assumption='Total source bounding length, including tail; not a measured specimen',
        length_m=args.length_m, pitch_m=pitch, occupied_voxels=len(cells),
        surface_voxels=len(indices), components=components, textured_meshes=textured,
        color_sampling='Seven-point linear-light texture footprint approximation',
        visual_approved=False, runtime_ready=False,
        limitations=['Static bind-pose geometry; no animation or skinning transfer',
                    'All source objects included; inspect for context objects and alpha cards',
                    'RGB research preview only; no engine color compatibility claim'])
    (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
