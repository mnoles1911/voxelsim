"""Continuous-mesh proportion study, retaining the reviewed wolf coat.

The deformation is an explicit modeling hypothesis, not a fitted specimen.
Head/limb shapes are scaled uniformly; additional length is confined to torso.
"""
import json
from pathlib import Path
import numpy as np
import trimesh
from scipy import ndimage
from scipy.spatial import cKDTree
from PIL import Image, ImageDraw
from voxelize_reference_mesh import write_views
from refine_wolf_pilot import smoothstep
from creature_review_views import ortho

ROOT = Path(__file__).resolve().parents[1]
PITCH = .0125
SCALE = .84
TORSO_EXTENSION = .14


def deform(points):
    result = points*SCALE
    result[:,0] += TORSO_EXTENSION*smoothstep(.65,1.22,points[:,0])
    return result


def main():
    base = ROOT/'out/creature-reconstruction'
    output = base/'grey-wolf-proportion-study'
    output.mkdir(parents=True,exist_ok=True)
    path = base/'source-meshes/56de4df672654ed599777d2980bf0f53.glb'
    scene = trimesh.load(path,force='scene')
    meshes = []
    for node in scene.graph.nodes_geometry:
        transform,name = scene.graph[node]
        mesh = scene.geometry[name].copy();mesh.apply_transform(transform)
        meshes.append(mesh)
    vertices = np.vstack([m.vertices for m in meshes])
    lower,upper = vertices.min(0),vertices.max(0)
    source_scale = 1.81/(upper[0]-lower[0])
    sampled=[]
    master=trimesh.Scene()
    for i,mesh in enumerate(meshes):
        canonical = (mesh.vertices-lower)[:,[0,2,1]]*source_scale
        mesh.vertices = deform(canonical)
        sampled.append(np.rint(mesh.voxelized(PITCH).fill().points/PITCH).astype(np.int32))
        # The axis swap above reverses handedness. Repair winding explicitly.
        mesh.faces = mesh.faces[:,::-1]
        exported=mesh.copy()
        exported.apply_transform(np.array([[1,0,0,0],[0,0,1,0],[0,-1,0,0],[0,0,0,1]]))
        master.add_geometry(exported,node_name=f'source-part-{i}')
    master.export(output/'master.glb')
    cells=np.unique(np.vstack(sampled),axis=0)
    lo=cells.min(0)-1;shape=cells.max(0)-lo+2
    occupied=np.zeros(shape,bool);occupied[tuple((cells-lo).T)]=True
    components,n=ndimage.label(occupied)
    sizes=sorted(np.bincount(components.ravel())[1:].tolist(),reverse=True)
    surface=np.argwhere(occupied & ~ndimage.binary_erosion(occupied))+lo
    coat=np.load(base/'grey-wolf-coat-study/surface-appearance.npz')
    coat_positions=deform(coat['cells']*PITCH)
    distance,nearest=cKDTree(coat_positions).query(surface*PITCH,k=4)
    weights=np.exp(-(distance/.008)**2)
    rgb=np.rint((coat['rgb'][nearest]*weights[:,:,None]).sum(1)/weights.sum(1)[:,None]).astype(np.uint8)
    # Preserve small high-contrast facial features instead of blurring them.
    original=coat['rgb'][nearest[:,0]]
    face=(surface[:,0]*PITCH<.24*SCALE)&(original.mean(1)<80)
    rgb[face]=original[face]
    np.savez_compressed(output/'surface-appearance.npz',cells=surface,
        occupied_cells=cells,rgb=rgb,voxel_m=PITCH)
    write_views(output,cells,surface,rgb,PITCH)
    comparison=Image.new('RGB',(1400,750),(223,226,229));draw=ImageDraw.Draw(comparison)
    common_lo=np.minimum(cells.min(0),coat['occupied_cells'].min(0))-1
    common_hi=np.maximum(cells.max(0),coat['occupied_cells'].max(0))+2
    for i,(item,label) in enumerate([(coat,'Previous proportions (same physical scale)'),
                                      (dict(occupied_cells=cells,cells=surface,rgb=rgb),'Torso and scale study (same physical scale)')]):
        occ=np.zeros(common_hi-common_lo,bool)
        occ[tuple((item['occupied_cells']-common_lo).T)]=True
        color=np.zeros((*occ.shape,3),np.uint8)
        color[tuple((item['cells']-common_lo).T)]=item['rgb']
        comparison.paste(ortho(color,occ,1),(i*700,40))
        draw.text((20+i*700,15),label,fill='black')
    comparison.save(output/'before-after.png')
    window=deform(np.array([[.60,0,0],[.75,0,0]]))[:,0]
    region=cells[(cells[:,0]*PITCH>=window[0])&(cells[:,0]*PITCH<=window[1])]
    report=dict(status='proportion_candidate_requires_visual_review',visual_approved=False,
        runtime_ready=False,pitch_m=PITCH,occupied_voxels=len(cells),components=sizes,
        dimensions_m=((cells.max(0)-cells.min(0)+1)*PITCH).tolist(),
        shoulder_region_envelope_m=float((region[:,2].max()-cells[:,2].min()+1)*PITCH),
        deformation=dict(uniform_scale=SCALE,torso_extension_m=TORSO_EXTENSION,
            source_torso_interval_m=[.65,1.22]),
        appearance_transfer_max_nearest_distance_m=float(distance[:,0].max()),
        rationale='Reduce excessive shoulder envelope while restoring torso length without flattening head or paws',
        limitations=['Authored deformation, not specimen fit','Shoulder region includes fur and manual landmark uncertainty',
                     'Paw/head local anatomy and animation remain unverified','RGB research output only'])
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__=='__main__':main()
