"""Reference-informed coat study on unchanged wolf voxel geometry.

Colors and region boundaries are authored hypotheses inspired by the credited
NPS photograph, not photogrammetry or measured biological parameters.
"""
import json
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree
from PIL import Image, ImageDraw
from voxelize_reference_mesh import write_views

ROOT = Path(__file__).resolve().parents[1]


def smoothstep(a, b, value):
    t = np.clip((value-a)/(b-a), 0, 1)
    return t*t*(3-2*t)


def main():
    source = ROOT/'out/creature-reconstruction/grey-wolf-source-study'
    output = ROOT/'out/creature-reconstruction/grey-wolf-coat-study'
    output.mkdir(parents=True, exist_ok=True)
    data = np.load(source/'surface-appearance.npz')
    cells, surface = data['occupied_cells'], data['cells']
    original = data['rgb'].astype(float)
    pitch = float(data['voxel_m'])
    xyz = surface*pitch
    x, y, z = xyz.T
    # Neighborhood averages suppress isolated texture pixels. Broad patterns
    # below remain explicit hypotheses, not purported reconstructed markings.
    tree = cKDTree(xyz)
    distance, neighbors = tree.query(xyz, k=24)
    weights = np.exp(-(distance/.025)**2)
    luminance = original @ np.array([.2126, .7152, .0722])
    low = (luminance[neighbors]*weights).sum(1)/weights.sum(1)
    detail = np.clip((luminance-low)*.45, -18, 18)
    broad = np.clip((low-np.median(low))*.40, -18, 18)
    lateral = np.abs(y-(cells[:, 1].max()+cells[:, 1].min())*pitch/2)
    # Body saddle tapers across the flanks, rather than a hard painted stripe.
    back = smoothstep(.66, .94, z)
    back *= smoothstep(.27, .52, x)*(1-smoothstep(1.30, 1.64, x))
    back *= 1-.32*smoothstep(.075, .18, lateral)
    base = np.tile([173., 163., 143.], (len(x), 1))
    light = 1-smoothstep(.38, .67, z)
    base = base*(1-light[:, None])+np.array([196,190,172])*light[:, None]
    base = base*(1-back[:, None])+np.array([65,67,63])*back[:, None]
    # Paler cheek/muzzle, grizzled crown, darker rear half of the tail.
    head = 1-smoothstep(.25, .45, x)
    crown = smoothstep(.86, 1.06, z)
    head_color = np.array([191,188,172])[None,:]*(1-crown[:,None])+np.array([104,107,98])[None,:]*crown[:,None]
    base = base*(1-head[:,None])+head_color*head[:,None]
    tail = smoothstep(1.42, 1.68, x)
    base = base*(1-tail[:,None])+np.array([91,91,81])*tail[:,None]
    rgb = np.clip(base+(detail+broad)[:,None],0,255)
    # Preserve source nose, mouth and dark eye pixels; do not invent eyes by
    # painting arbitrary ellipses. Replace conspicuous cyan eye pigment only.
    face = (x<.24)&(z>.61)
    dark = face&(luminance<65)
    rgb[dark] = original[dark]
    mouth = face&(original[:,0]>original[:,1]*1.25)&(z<.82)
    rgb[mouth] = original[mouth]
    eye = face&(z>.83)&(original[:,2]>original[:,0]*1.15)&(original[:,1]>original[:,0]*1.10)
    rgb[eye] = [112,88,44]
    # Source white claw pixels are subdued, while paw geometry stays untouched.
    claws = (z<.045)&(luminance>210)
    rgb[claws] = [78,76,67]
    rgb = np.rint(rgb).astype(np.uint8)
    np.savez_compressed(output/'surface-appearance.npz', cells=surface,
        occupied_cells=cells, rgb=rgb, voxel_m=pitch)
    write_views(output,cells,surface,rgb,pitch)
    comparison = Image.new('RGB',(1400,750),(223,226,229))
    draw = ImageDraw.Draw(comparison)
    for i,(folder,title) in enumerate([(source,'Previous source texture'),(output,'Reference-informed coat study')]):
        comparison.paste(Image.open(folder/'view-0.png'),(700*i,40))
        draw.text((700*i+20,15),title,fill='black')
    comparison.save(output/'before-after.png')
    shoulder_region = cells[(cells[:,0]*pitch>=.60)&(cells[:,0]*pitch<=.75)]
    shoulder_envelope = float((shoulder_region[:,2].max()-cells[:,2].min()+1)*pitch)
    report = dict(status='coat_candidate_requires_visual_review',visual_approved=False,
        runtime_ready=False,geometry_unchanged=True,pitch_m=pitch,
        occupied_voxels=len(cells),surface_voxels=len(surface),
        reference='refs/reconstruction/grey-wolf/nps-canyon-alpha.jpg',
        assumptions='Authored coat regions and colors, not calibrated photo measurements',
        scale_audit=dict(shoulder_region_envelope_m=shoulder_envelope,
            landmark_note='Manual x=0.60..0.75 m window, includes coat; not a measured skeletal landmark',
            nps_male_average_shoulder_m=.81,nps_shoulder_range_m=[.6604,.9144],
            source='https://www.nps.gov/yell/learn/nature/wolf.htm',
            conclusion='Revisit source proportions and scale; total length alone is insufficient'),
        remaining=['Head and paw anatomical review','Measured scale calibration',
                   'Runtime RGB appearance support','Rig and animation transfer'])
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
