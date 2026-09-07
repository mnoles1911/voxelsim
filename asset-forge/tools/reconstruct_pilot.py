"""Anatomical master-surface pilot; outputs stay outside the approved library.

Requires trimesh/rtree in addition to the normal Forge dependencies.
The vendored spline handles carry their own pinned source/license manifest.
Dimensions and joint landmarks below are explicit reconstruction hypotheses,
not measurements attributed to the source templates or to photographs.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import numpy as np
from scipy.interpolate import BSpline, CubicSpline
from scipy.spatial import cKDTree
from scipy import ndimage
import trimesh
from PIL import Image, ImageDraw
import _path  # noqa: F401
from forge import materials, parts, vxa, vox, render
from forge.grid import VoxelGrid

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'refs/reconstruction/infinigen'
PITCH = .0125


def surface(rings, samples=72, around=40):
    """Evaluate a clamped longitudinal spline and periodic section curves."""
    rings = np.asarray(rings, dtype=float)
    n, m, _ = rings.shape
    degree = min(3, n-1)
    knots = np.r_[np.zeros(degree+1),
                  np.linspace(0, 1, n-degree+1)[1:-1], np.ones(degree+1)]
    along = BSpline(knots, rings, degree)(np.linspace(0, 1, samples))
    closed = np.concatenate([along, along[:, :1]], axis=1)
    verts = CubicSpline(np.arange(m+1), closed, axis=1, bc_type='periodic')(
        np.linspace(0, m, around, endpoint=False))
    faces = []
    for i in range(samples-1):
        for j in range(around):
            a=i*around+j; b=i*around+(j+1)%around
            faces.extend(((a,b,b+around),(a,b+around,a+around)))
    faces.extend((0,j+1,j) for j in range(1,around-1))
    q=(samples-1)*around
    faces.extend((q,q+j,q+j+1) for j in range(1,around-1))
    mesh=trimesh.Trimesh(verts.reshape(-1,3),faces,process=True)
    mesh.fix_normals()
    return mesh


def template(name, scale, offset):
    rings=np.load(DATA/(name+'.npy'))[...,:3]
    return surface(rings*np.asarray(scale)+np.asarray(offset))


def tube(points, widths, depths=None):
    points=np.asarray(points,float); widths=np.asarray(widths,float)
    depths=widths if depths is None else np.asarray(depths,float)
    tangents=np.gradient(points,axis=0)
    tangents/=np.linalg.norm(tangents,axis=1)[:,None]
    sideways=np.cross(tangents,np.array([0,1,0]))
    sideways/=np.maximum(np.linalg.norm(sideways,axis=1)[:,None],1e-8)
    other=np.cross(tangents,sideways)
    angles=np.linspace(0,2*np.pi,12,endpoint=False)
    rings=(points[:,None,:]+np.cos(angles)[None,:,None]*sideways[:,None,:]*depths[:,None,None]
           +np.sin(angles)[None,:,None]*other[:,None,:]*widths[:,None,None])
    return surface(rings,samples=48,around=24)


def ellipsoid(center, radii):
    mesh=trimesh.creation.icosphere(subdivisions=3)
    mesh.vertices=mesh.vertices*np.asarray(radii)+np.asarray(center)
    return mesh


def wolf():
    # Metres; +X faces forward, +Z is up. Full-grown neutral standing pilot.
    meshes=[('body',1,template('body_feline_wolf',[.50,.49,.48],[0,0,.655])),
            ('head',2,template('head_carnivore_wolf',[.55,.48,.52],[1.01,0,.778]))]
    for side in (-1,1):
        stride=64 if side>0 else 0
        y=side*.103
        meshes.append(('fore-leg',11+stride,tube(
            [[.755,y,.655],[.713,y,.49],[.719,y,.36],[.759,y,.15],[.775,y,.055]],
            [.057,.044,.032,.022,.029],[.072,.05,.033,.024,.035])))
        meshes.append(('hind-leg',43+stride,tube(
            [[.195,y,.642],[.267,y,.51],[.305,y,.39],[.156,y,.22],[.155,y,.08],[.19,y,.047]],
            [.072,.061,.042,.026,.020,.030],[.085,.075,.044,.03,.024,.034])))
        for x,pid in ((.795,11+stride),(.21,43+stride)):
            meshes.append(('paw',pid,ellipsoid([x,y,.037],[.063,.038,.035])))
        # Leaf-shaped ear, joined into the posterior skull. No spherical eye props.
        meshes.append(('ear',12+stride,tube(
            [[1.017,side*.067,.86],[1.014,side*.07,.90],[1.021,side*.074,.966],[1.029,side*.075,.982]],
            [.026,.026,.012,.001],[.028,.025,.01,.001])))
    meshes.append(('tail',5,tube([[.045,0,.64],[-.105,0,.585],[-.255,0,.45],[-.367,0,.32],[-.39,0,.29]],
                                 [.046,.052,.048,.032,.001])))
    meshes.append(('nose',2,ellipsoid([1.249,0,.786],[.022,.034,.025])))
    return meshes


def wolf_color(points, labels):
    x,y,z=points.T
    # Restrained grizzled gray/buff coat, pale lower limbs/cheeks; no baked light.
    dorsal=np.clip((z-.46)/.27,0,1)
    flank=np.clip(np.abs(y)/.17,0,1)
    dark=dorsal*(1-.42*flank)*np.clip((1.04-x)*5,0,1)
    low=np.array([181,178,159]); high=np.array([70,72,70])
    rgb=low[None,:]*(1-dark[:,None])+high[None,:]*dark[:,None]
    fur=(np.sin(x*157+y*57+np.sin(z*79))*np.sin(z*103-y*91)
         +.5*np.sin(x*311-y*173+z*227))*7
    rgb+=fur[:,None]
    head=labels==2
    rgb[head]=np.array([160,158,143])+fur[head,None]
    cheek=head&(z<.80)&(x<1.22)
    rgb[cheek]=np.array([202,198,180])+fur[cheek,None]
    nose=head&(x>1.233)&(z>.76)&(z<.82)
    rgb[nose]=[32,31,29]
    # Eyes are surface pigment at true skull positions, not enlarged balls.
    eye=head&(((x-1.112)/.014)**2+((z-.829)/.010)**2<1)&(np.abs(y)>.055)
    rgb[eye]=[53,42,24]
    pupil=eye&(np.abs(x-1.113)<.005)
    rgb[pupil]=[20,20,19]
    ears=np.isin(labels,[12,76]);inner=ears&(z>.895)&(x>1.01)
    rgb[ears]=[98,96,82];rgb[inner]=[158,147,132]
    tail=labels==5;rgb[tail]=np.array([100,100,91])+fur[tail,None]
    return np.clip(rgb,0,255).astype(np.uint8)


def fin(outline,thickness=.013,axis=1):
    """A closed, thin anatomical blade; outline is authored in world metres."""
    outline=np.asarray(outline,float)
    # A convex blade is sufficient for each individually authored fin lobe.
    offset=np.zeros(3);offset[axis]=thickness/2
    return trimesh.convex.convex_hull(np.r_[outline-offset,outline+offset])


def tiger():
    out=[('body',1,template('body_feline_tiger',[.77,.86,.64],[0,0,.89])),
         ('head',2,template('head_carnivore_tiger',[.87,.85,.76],[1.49,0,1.002]))]
    for side in (-1,1):
        stride=64 if side>0 else 0;y=side*.195
        out.append(('fore-leg',11+stride,tube([[1.10,y,.91],[1.12,y,.70],[1.11,y,.51],[1.18,y,.18],[1.20,y,.06]],
                                              [.11,.083,.06,.045,.058],[.14,.10,.066,.048,.06])))
        out.append(('hind-leg',43+stride,tube([[.30,y,.87],[.35,y,.65],[.48,y,.43],[.28,y,.24],[.29,y,.08]],
                                              [.13,.115,.075,.052,.052],[.16,.13,.083,.058,.05])))
        for x,pid in ((1.23,11+stride),(.34,43+stride)):
            out.append(('paw',pid,ellipsoid([x,y,.055],[.10,.075,.052])))
        out.append(('ear',12+stride,ellipsoid([1.48,side*.116,1.186],[.035,.063,.069])))
    out.append(('tail',5,tube([[.04,0,.86],[-.28,0,.74],[-.60,0,.44],[-.80,0,.26],[-.85,0,.35]],
                                [.045,.048,.043,.036,.025])))
    out.append(('nose',2,ellipsoid([1.825,0,1.025],[.022,.055,.031])))
    return out


def deer():
    # Distinct cervid torso/neck/joints. Goat handles are a starting surface only.
    out=[('body',1,template('body_herbivore_goat',[.64,.61,.49],[0,0,1.10])),
         ('neck',3,tube([[1.00,0,1.05],[1.15,0,1.23],[1.32,0,1.52],[1.42,0,1.60]],
                       [.18,.17,.12,.09],[.23,.23,.15,.10])),
         ('head',2,template('head_herbivore_goat',[.62,.48,.40],[1.42,0,1.58]))]
    for side in (-1,1):
        stride=64 if side>0 else 0;y=side*.115
        out.append(('fore-leg',11+stride,tube([[.93,y,1.05],[.90,y,.80],[.96,y,.52],[.95,y,.16],[.97,y,.045]],
                                               [.07,.048,.028,.023,.034])))
        out.append(('hind-leg',43+stride,tube([[.21,y,1.02],[.34,y,.76],[.20,y,.44],[.15,y,.20],[.21,y,.045]],
                                               [.095,.063,.033,.024,.034])))
        for x,pid in ((.99,11+stride),(.23,43+stride)):
            out.append(('hoof',pid,ellipsoid([x,y,.034],[.052,.034,.034])))
        out.append(('ear',12+stride,tube([[1.44,side*.05,1.66],[1.41,side*.14,1.72],[1.38,side*.22,1.74]],
                                         [.015,.04,.001],[.017,.052,.001])))
        pid=13+stride
        beam=[[1.435,side*.065,1.73],[1.32,side*.18,1.97],[1.25,side*.30,2.19],[1.40,side*.38,2.30]]
        out.append(('antler',pid,tube(beam,[.027,.024,.017,.001])))
        for root,end in ((beam[0],[1.63,side*.14,1.93]),(beam[1],[1.55,side*.24,2.10]),(beam[2],[1.40,side*.46,2.40])):
            middle=(np.asarray(root)+end)/2
            out.append(('tine',pid,tube([root,middle,end],[.016,.012,.001])))
    out.append(('tail',5,tube([[.025,0,1.05],[-.07,0,.99],[-.11,0,.92]],[.042,.04,.003])))
    return out


def bird_master(eagle=False):
    out=[('body',1,template('body_bird_robin',[.55,.50,.40],[0,0,.24])),
         ('head',2,ellipsoid([.335,0,.425],[.070,.056,.064])),
         ('bill',4,tube([[.378,0,.438],[.404,0,.445],[.452,0,.436],[.489,0,.416]],
                        [.034,.029,.019,.001],[.025,.025,.015,.001]))]
    for side in (-1,1):
        stride=64 if side>0 else 0;y=side*.042
        out.append(('leg',11+stride,tube([[.17,y,.20],[.175,y,.12],[.135,y,.077],[.16,y,.025]],
                                         [.017,.012,.009,.009])))
        for j in (-1,0,1):
            out.append(('toe',11+stride,tube([[.16,y,.025],[.20,y+j*.013,.018],[.225,y+j*.018,.013]],
                                              [.007,.006,.002])))
        out.append(('rear-toe',11+stride,tube([[.16,y,.025],[.13,y,.015],[.11,y,.012]],[.007,.006,.002])))
        out.append(('wing-coverts',10+stride,tube([[.265,side*.061,.324],[.20,side*.080,.28],[.06,side*.063,.23],[-.035,side*.035,.207]],
                                                   [.025,.030,.019,.001],[.040,.054,.025,.001])))
        for j in range(5):
            out.append(('folded-primary',10+stride,tube([[.16-j*.014,side*(.088-j*.004),.264-j*.006],
                    [.035-j*.009,side*(.069-j*.004),.210-j*.004],[-.078-j*.006,side*(.024+j*.004),.17+j*.003]],
                    [.010,.012,.001],[.006,.006,.001])))
    for j in range(-3,4):
        length=.22-abs(j)*.015
        out.append(('tail-feather',5,tube([[.055,j*.009,.235],[-.07,j*.012,.20],[-length,j*.011,.164]],
                                           [.010,.012,.001],[.006,.006,.001])))
    if eagle:
        for name,pid,mesh in out:
            mesh.vertices*=np.array([1.3,1.40,1.32])
            if name=='bill':
                mesh.vertices[:,0]=.495+(mesh.vertices[:,0]-.495)*.63
                mesh.vertices[:,2]-=np.clip((mesh.vertices[:,0]-.535)*.7,0,.035)
    return out


def aquatic(species):
    trout=species=='rainbow-trout';orca=species=='orca'
    length=.46 if trout else 5.8 if orca else 3.45
    depth=.053 if trout else .63 if orca else .43
    width=.027 if trout else .59 if orca else .36
    z=.16 if trout else .85
    xx=np.array([0,.04,.12,.28,.48,.67,.83,.94,1])*length
    profiles=np.array([.07,.16,.42,.80,1,.98,.85,.59,.12 if orca else .015])
    points=np.c_[xx,np.zeros(9),z+depth*np.array([0,0,0,.025,.03,.025,0,-.04,-.08])]
    out=[('body',1,tube(points,width*profiles,depth*profiles))]
    # Each fin lobe is a closed blade, with its insertion overlapping the body.
    if orca:
        out.append(('dorsal',7,fin([[length*.45,0,z+depth*.80],[length*.55,0,z+depth*.93],
                    [length*.51,0,z+depth+ .73],[length*.465,0,z+depth+.64]],.04)))
        for side in (-1,1):
            out.append(('pectoral',9+(64 if side>0 else 0),fin([[length*.80,side*width*.45,z-.2],
                [length*.72,side*1.08,z-.62],[length*.60,side*1.15,z-.59],[length*.65,side*width*.60,z-.24]],.06,2)))
            out.append(('fluke',8,fin([[.20,side*.035,z],[.02,side*.89,z+.02],[-.20,side*.86,z-.01],[-.07,side*.025,z-.02]],.05,2)))
    else:
        dorsal=.07 if trout else .56
        out.append(('dorsal',7,fin([[length*.40,0,z+depth*.8],[length*.65,0,z+depth*.90],
                    [length*.54,0,z+depth+dorsal],[length*.43,0,z+depth+dorsal*.65]],.0125 if trout else .035)))
        out.append(('caudal-upper',8,fin([[.035*length,0,z],[0,0,z+depth*.3],[-.17*length,0,z+depth*1.7],[-.12*length,0,z+depth*.6]],.013 if trout else .025)))
        out.append(('caudal-lower',8,fin([[.035*length,0,z],[0,0,z-depth*.3],[-.16*length,0,z-depth*1.45],[-.12*length,0,z-depth*.55]],.013 if trout else .025)))
        for side in (-1,1):
            pid=9+(64 if side>0 else 0)
            for at,span in ((.73,.10 if trout else .77),(.31,.06 if trout else .32)):
                out.append(('paired-fin',pid,fin([[length*at,side*width*.6,z-depth*.40],
                    [length*(at-.11),side*(width+span),z-depth*.95],[length*(at-.16),side*width*.8,z-depth*.73]],.0125 if trout else .028,2)))
        out.append(('anal-fin',7,fin([[length*.16,0,z-depth*.5],[length*.30,0,z-depth*.80],
                    [length*.17,0,z-depth*(1.65 if trout else 1.20)]],.0125 if trout else .025)))
        if trout:
            out.append(('adipose',7,fin([[length*.19,0,z+depth*.50],[length*.26,0,z+depth*.65],
                                       [length*.225,0,z+depth*1.12]],.0125)))
        else:
            out.append(('second-dorsal',7,fin([[length*.16,0,z+depth*.3],[length*.24,0,z+depth*.6],
                        [length*.20,0,z+depth*.88]],.025)))
    return out


BUILDERS={'grey-wolf':wolf,'bengal-tiger':tiger,'red-deer-stag':deer,
          'common-raven':lambda:bird_master(False),'golden-eagle':lambda:bird_master(True),
          'rainbow-trout':lambda:aquatic('rainbow-trout'),
          'great-white-shark':lambda:aquatic('great-white-shark'),'orca':lambda:aquatic('orca')}


def animal_color(species,points,labels):
    if species=='grey-wolf':return wolf_color(points,labels)
    x,y,z=points.T
    noise=np.sin(x*173+y*71+np.sin(z*131))*np.sin(z*217-y*103)
    if species=='bengal-tiger':
        rgb=np.tile([188.,111.,49.],(len(x),1));rgb+=noise[:,None]*5
        belly=(z<.70)&(np.abs(y)<.24);cheek=(x>1.52)&(z<1.0)
        rgb[belly|cheek]=[219,211,187]
        phase=x*20+1.0*np.sin(z*8+x*1.5)+.6*np.sin(z*19+x*4)+np.sign(y)*.67
        band=(np.abs(np.sin(phase))<(.17+.12*np.sin(z*12+x*9)**2))&(z>.58)
        # Break and branch individual strokes; pattern coordinates follow the flank.
        branch=(np.abs(np.sin(phase+.65))<.08)&(np.sin(x*15-z*8)>.4)&(z>.75)
        rgb[band|branch]=[37,31,25]
        limbs=np.isin(labels,[11,75,43,107]);rgb[limbs&(np.sin(z*38+x*5)>.90)]=[43,36,27]
        tail=labels==5;rgb[tail&(np.sin(x*31+z*14)>.55)]=[34,29,24]
        ears=np.isin(labels,[12,76]);rgb[ears]=[33,28,23]
        rgb[ears&(z>1.19)&(x<1.48)]=[206,199,177]
        eyes=(labels==2)&(((x-1.655)/.021)**2+((z-1.105)/.014)**2<1)&(np.abs(y)>.08)
        rgb[eyes]=[55,55,25];rgb[(labels==2)&(x>1.813)]=[100,66,54]
    elif species=='red-deer-stag':
        rgb=np.tile([127.,84.,53.],(len(x),1))+noise[:,None]*5
        rgb[(z<.84)&(labels==1)]=[159,125,85]
        rgb[labels==3]=[87,64,46]
        rgb[(x<.23)&(z>.83)&(z<1.16)]=[177,154,111]
        rgb[np.isin(labels,[13,77])]=[154,134,105]
        rgb[z<.075]=[42,36,29]
        rgb[(labels==2)&(x>1.68)]=[49,41,31]
        eyes=(labels==2)&(((x-1.525)/.016)**2+((z-1.65)/.014)**2<1)&(np.abs(y)>.047)
        rgb[eyes]=[23,21,18]
    elif species in ('common-raven','golden-eagle'):
        eagle=species=='golden-eagle';scale=1.3 if eagle else 1
        rgb=np.tile([64.,48.,35.] if eagle else [28.,30.,31.],(len(x),1))+noise[:,None]*2
        wings=np.isin(labels,[10,74]);feather=np.sin(x*120/scale+z*40/scale+y*65)
        rgb[wings]+=feather[wings,None]*(5 if eagle else 2)
        if eagle:
            rgb[(labels==2)&(x<.445)&(z>.54)]=[135,106,61]
            rgb[(labels==4)]=[69,63,47]
            rgb[(labels==4)&(x<.537)]=[173,132,48]
            rgb[np.isin(labels,[11,75])&(z<.10)]=[156,117,43]
        else:rgb[labels==4]=[25,26,27]
        eyes=(labels==2)&(((x/scale-.365)/.0065)**2+((z/scale-.440)/.0065)**2<1)&(np.abs(y)>.045*scale)
        rgb[eyes]=[13,13,12]
    elif species=='rainbow-trout':
        top=np.clip((z-.115)/.09,0,1)
        rgb=np.array([205,208,186])[None,:]*(1-top[:,None])+np.array([78,94,62])[None,:]*top[:,None]
        stripe=np.exp(-((z-.164)/.013)**2)*np.clip(np.abs(y)/.023,0,1)
        rgb=rgb*(1-stripe[:,None]*.65)+np.array([180,105,114])[None,:]*stripe[:,None]*.65
        spots=(np.sin(x*173+z*59)*np.cos(z*227+y*107)>.88)&(z>.145)
        rgb[spots]=[38,43,33]
        rgb[np.isin(labels,[7,8,9,73])]*=.87
        eyes=(((x-.397)/.010)**2+((z-.170)/.009)**2<1)&(np.abs(y)>.01)
        rgb[eyes]=[27,29,21]
    elif species=='great-white-shark':
        transition=np.clip((z-.74)/.11,0,1)
        rgb=np.array([214,211,196])[None,:]*(1-transition[:,None])+np.array([81,90,94])[None,:]*transition[:,None]
        gills=(x>2.50)&(x<2.94)&(z>.68)&(z<1.02)&(np.abs(y)>.24)
        rgb[gills&(np.sin((x-2.50)*np.pi/.073)>.87)]=[47,53,53]
        eyes=(((x-3.12)/.035)**2+((z-.905)/.028)**2<1)&(np.abs(y)>.12)
        rgb[eyes]=[19,23,24]
    else:
        rgb=np.tile([26.,30.,32.],(len(x),1))
        belly=z<(.46+.13*np.cos((x-2.8)*.7));rgb[belly]=[229,229,218]
        patch=(((x-4.90)/.39)**2+((z-1.06)/.14)**2<1)&(np.abs(y)>.30)
        rgb[patch]=[234,234,222]
        saddle=(x>2.25)&(x<2.97)&(z>1.10)&(np.abs(y)>.22)
        rgb[saddle]=[112,118,118]
        flank=(x>1.45)&(x<2.5)&(z<.78)&(np.abs(y)>.28)
        rgb[flank]=[226,226,216]
    return np.clip(rgb,0,255).astype(np.uint8)


def voxelize(meshes):
    samples=[]
    for name,pid,mesh in meshes:
        # Surface subdivision + fill is independent of reference image lighting.
        points=mesh.voxelized(PITCH).fill().points
        cells=np.rint(points/PITCH).astype(np.int32)
        samples.append((pid,cells))
    lower=np.vstack([a.min(0) for _,a in samples]).min(0)-2
    upper=np.vstack([a.max(0) for _,a in samples]).max(0)+3
    tags=np.zeros(upper-lower,np.uint8)
    for pid,cells in samples:
        tags[tuple((cells-lower).T)]=pid
    labels,count=ndimage.label(tags!=0,structure=ndimage.generate_binary_structure(3,1))
    sizes=np.bincount(labels.ravel());sizes[0]=0
    # Report detached pieces; never silently drop anatomical errors.
    components=sorted(sizes[sizes>0].tolist(),reverse=True)
    return tags,lower,components


def ortho(rgb,occ,axis,reverse=False,size=700):
    colors=np.moveaxis(rgb,axis,0); solid=np.moveaxis(occ,axis,0)
    if reverse:colors=colors[::-1];solid=solid[::-1]
    idx=solid.argmax(0);u,v=np.indices(idx.shape)
    pixels=colors[idx,u,v].copy();mask=solid.any(0)
    pixels[~mask]=[223,226,229]
    if axis!=2:pixels=np.flip(pixels.transpose(1,0,2),axis=0)
    else:pixels=pixels.transpose(1,0,2)
    if reverse and axis!=2:pixels=pixels[:,::-1]
    img=Image.fromarray(pixels)
    img.thumbnail((size,size),Image.Resampling.NEAREST) if max(img.size)>size else None
    scale=max(1,size//max(img.size));img=img.resize((img.width*scale,img.height*scale),Image.Resampling.NEAREST)
    canvas=Image.new('RGB',(size,size),(223,226,229));canvas.paste(img,((size-img.width)//2,(size-img.height)//2))
    return canvas


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--species',choices=list(BUILDERS),default='grey-wolf')
    args=ap.parse_args();out=ROOT/'out/creature-reconstruction'/args.species;out.mkdir(parents=True,exist_ok=True)
    meshes=BUILDERS[args.species]()
    master=trimesh.Scene()
    for i,(name,pid,mesh) in enumerate(meshes):
        mesh.visual.vertex_colors=np.c_[animal_color(args.species,mesh.vertices,np.full(len(mesh.vertices),pid)),np.full(len(mesh.vertices),255)]
        master.add_geometry(mesh,node_name=f'{name}-{i}')
    master.export(out/'master.glb')
    tags,lower,components=voxelize(meshes)
    ijk=np.argwhere(tags!=0);xyz=(ijk+lower)*PITCH
    rgb=animal_color(args.species,xyz,tags[tuple(ijk.T)])
    full=np.zeros((*tags.shape,3),np.uint8);full[tuple(ijk.T)]=rgb
    ids=np.array([materials.resolve(n) for n in materials.CREATURE_NAMES])
    colors=np.array([materials.color(int(i)) for i in ids])
    nearest=cKDTree(colors).query(rgb)[1]
    grid=VoxelGrid(tags.shape,origin=lower,voxel_m=PITCH);grid.data[tuple(ijk.T)]=ids[nearest]
    # Preview RGB is recorded separately: it is not falsely claimed to be VXA color.
    np.savez_compressed(out/'appearance.npz',rgb=rgb,cells=ijk+lower,part_ids=tags[tuple(ijk.T)],voxel_m=PITCH)
    joints=parts.joints(tags)
    chunks=[]
    if tags.size<=8*1024*1024:
        vxa.write(grid,out/'model.vxa',tags,joints)
    else:
        # Current VXA body decoder has an 8M-cell ceiling. Export explicit tiles
        # rather than silently changing the requested pitch or writing unloadable data.
        for bx in range(0,tags.shape[0],128):
            for by in range(0,tags.shape[1],128):
                for bz in range(0,tags.shape[2],128):
                    sl=(slice(bx,bx+128),slice(by,by+128),slice(bz,bz+128))
                    if not np.any(tags[sl]):continue
                    tile=VoxelGrid(tags[sl].shape,origin=lower+[bx,by,bz],voxel_m=PITCH)
                    tile.data=grid.data[sl].copy();path=f'chunk-{bx}-{by}-{bz}.vxa'
                    vxa.write(tile,out/path,tags[sl],[])
                    chunks.append(dict(file=path,origin_cells=tile.origin.tolist(),voxels=tile.count()))
        (out/'chunks.json').write_text(json.dumps(dict(voxel_m=PITCH,chunks=chunks,
            joints=joints,joint_origin_cells=lower.tolist(),runtime_ready=False,
            note='Research tiled export; runtime creature assembly is not yet implemented.'),indent=2)+'\n')
    vox.write(grid,out/'engine-palette.vox',args.species)
    preview=render.view(grid,'broad',target_px=900,background=(223,226,229,255),ao=.15)
    from PIL import ImageOps
    ImageOps.expand(preview,border=24,fill=(223,226,229,255)).save(out/'engine-preview.png')
    sheet=Image.new('RGB',(2100,1460),(223,226,229));draw=ImageDraw.Draw(sheet)
    for i,(label,axis,reverse) in enumerate([('left',1,False),('right',1,True),('front',0,True),('back',0,False),('top',2,True),('bottom',2,False)]):
        picture=ortho(full,tags!=0,axis,reverse);picture.save(out/(label+'.png'))
        x=i%3*700;y=i//3*730;sheet.paste(picture,(x,y+30));draw.text((x+20,y+8),label,fill='black')
    sheet.save(out/'six-views.png')
    report=dict(species=args.species,voxel_m=PITCH,voxels=len(ijk),components=components,
                appearance='RGB research sidecar; VXA uses existing engine palette',
                status='candidate_requires_visual_review',master='master.glb',
                chunks=len(chunks),runtime_ready=False,
                single_vxa_body_compatible=not bool(chunks),
                body_dimensions_m=((ijk.max(0)-ijk.min(0)+1)*PITCH).tolist())
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report),flush=True)


if __name__=='__main__':main()
