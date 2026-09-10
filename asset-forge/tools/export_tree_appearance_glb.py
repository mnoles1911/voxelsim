"""Prepare isolated vertex-colored pilot meshes for Unreal comparison imports.

No game catalog changes. glTF uses metres, Y-up and linear vertex colors.
UV0 retains the exact asset-local planar metres used by the review leaf mask.
"""
import json
import struct
import os
from pathlib import Path
import _path
import numpy as np
from forge import vxa,inventory
from tree_appearance_pilot import ROOT,OUT

DEST=Path(os.environ.get('TREE_APPEARANCE_EXPORT_DIR',str(ROOT/'out/tree-appearance-pilot/game')))

def face_colors(srgb, coords, axis, positive, foliage):
    """Match the pilot's uint32 vertex shader; one constant RGB per quad."""
    c=coords.astype(np.uint32)
    def h(x):
        x=x.copy();x^=x>>16;x*=np.uint32(2246822519);x^=x>>13
        x*=np.uint32(3266489917);return x^(x>>16)
    def unit(x):return (h(x)&65535).astype(float)/65535*2-1
    key=c[:,0]*np.uint32(73856093)^c[:,1]*np.uint32(19349663)^c[:,2]*np.uint32(83492791)
    voxel=unit(key);side=unit(key^np.uint32(((axis*2+int(positive)+1)*2654435761)&0xffffffff))
    warmth=unit(key^np.uint32(1597334677))
    gain=1+.085*voxel+.045*side
    tint=np.array([.025,0,-.025])
    result=np.clip(srgb*gain[:,None]*(1+warmth[:,None]*tint),0,1)
    return np.where(result<=.04045,result/12.92,((result+.055)/1.055)**2.4).astype('<f4')

def export(row):
    grid=vxa.read(ROOT/row['source']/'tree.vxa')
    raw=(OUT/row['id']/'voxels.bin').read_bytes();count=struct.unpack_from('<I',raw,12)[0]
    coords=np.frombuffer(raw,dtype='<i2',count=count*3,offset=16).reshape(-1,3).astype(np.int32)
    mats=grid.data[tuple(coords.T)]
    srgb=np.frombuffer((OUT/row['id']/'species.rgb').read_bytes(),dtype=np.uint8).reshape(-1,3)/255.
    colors=np.where(srgb<=.04045,srgb/12.92,((srgb+.055)/1.055)**2.4).astype(np.float32)
    wood=np.isin(mats,[16,17,18,23]);leaf=np.isin(mats,[19,20,21,22,24,25])
    arrays=[dict(position=[],normal=[],uv=[],color=[]) for _ in range(2)]
    # U,V choices yield a positive-axis normal; reverse indices for negative faces.
    for axis in range(3):
        u,v=(axis+1)%3,(axis+2)%3
        for positive in (False,True):
            neighbor=coords.copy();neighbor[:,axis]+=1 if positive else -1
            inside=np.all((neighbor>=0)&(neighbor<grid.shape),axis=1)
            nm=np.zeros(count,dtype=np.uint8);nm[inside]=grid.data[tuple(neighbor[inside].T)]
            exposed=(nm==0)|(wood&np.isin(nm,[19,20,21,22,24,25]))
            for material in (0,1):
                keep=np.flatnonzero(exposed&(leaf if material else ~leaf))
                if not len(keep):continue
                corners=np.repeat(coords[keep,None,:].astype(float),4,axis=1)
                corners[:,:,axis]+=int(positive)
                corners[:,:,u]+=np.array([0,1,1,0]);corners[:,:,v]+=np.array([0,0,1,1])
                # Pattern axes match the browser, independent of face winding.
                pu,pv=(0,1) if axis==2 else (1,2) if axis==0 else (0,2)
                uv=(corners[:,:,[pu,pv]]*grid.voxel_m).astype('<f4')
                xyz=(corners+grid.origin)*grid.voxel_m
                xyz=xyz[:,:,[0,2,1]];xyz[:,:,2]*=-1
                n=np.zeros(3);n[axis]=1 if positive else -1;n=n[[0,2,1]];n[2]*=-1
                a=arrays[material];a['position'].append(xyz.astype('<f4').reshape(-1,3))
                a['normal'].append(np.tile(n,(len(keep)*4,1)).astype('<f4'))
                a['uv'].append(uv.reshape(-1,2))
                a['color'].append(np.repeat(face_colors(srgb[keep],coords[keep],axis,positive,bool(material)),4,axis=0))
                if 'winding' not in a:a['winding']=[]
                a['winding'].append(np.tile([0,1,2,0,2,3] if positive else [0,2,1,0,3,2],(len(keep),1)))
    doc={'asset':{'version':'2.0','generator':'Asset Forge isolated appearance pilot'},'scene':0,
        'scenes':[{'nodes':[0]}],'nodes':[{'mesh':0,'name':row['id']}],
        'materials':[{'name':n,'pbrMetallicRoughness':{'baseColorFactor':[1,1,1,1],'metallicFactor':0,'roughnessFactor':.9},'doubleSided':bool(i)} for i,n in enumerate(('SolidWood','Foliage'))],
        'meshes':[{'primitives':[]}],'bufferViews':[],'accessors':[],'buffers':[]}
    data=bytearray()
    def accessor(a,typ,component=5126):
        while len(data)%4:data.append(0)
        view=len(doc['bufferViews']);start=len(data);data.extend(a.tobytes())
        doc['bufferViews'].append({'buffer':0,'byteOffset':start,'byteLength':a.nbytes})
        item={'bufferView':view,'componentType':component,'count':len(a),'type':typ}
        if typ=='VEC3' and component==5126:item.update(min=a.min(axis=0).tolist(),max=a.max(axis=0).tolist())
        doc['accessors'].append(item);return len(doc['accessors'])-1
    faces=0
    for i,a in enumerate(arrays):
        if not a['position']:continue
        merged={k:np.concatenate(a[k]) for k in ('position','normal','uv','color')}
        winding=np.concatenate(a['winding']);indices=(winding+np.arange(len(winding))[:,None]*4).astype('<u4').reshape(-1)
        faces+=len(winding)
        attrs={name:accessor(merged[key],typ) for name,key,typ in [('POSITION','position','VEC3'),('NORMAL','normal','VEC3'),('TEXCOORD_0','uv','VEC2'),('COLOR_0','color','VEC3')]}
        doc['meshes'][0]['primitives'].append({'attributes':attrs,'indices':accessor(indices,'SCALAR',5125),'material':i})
    while len(data)%4:data.append(0)
    doc['buffers']=[{'byteLength':len(data)}]
    encoded=json.dumps(doc,separators=(',',':')).encode();encoded+=b' '*((-len(encoded))%4)
    content=struct.pack('<III',0x46546C67,2,12+8+len(encoded)+8+len(data))+struct.pack('<II',len(encoded),0x4E4F534A)+encoded+struct.pack('<II',len(data),0x004E4942)+data
    path=DEST/(row['id']+'.glb');path.write_bytes(content)
    return dict(id=row['id'],faces=faces,bytes=len(content),sha256=inventory.digest(path),needle=row['needle'])

if __name__=='__main__':
    DEST.mkdir(parents=True,exist_ok=True)
    models=json.loads((OUT/'manifest.json').read_text())['models'];rows=[]
    for m in models:rows.append(export(m));print('GLB',rows[-1]['id'],rows[-1]['faces'],flush=True)
    inventory.write_json(DEST/'manifest.json',dict(status='prepared, not imported or game-verified',models=rows))
