"""CPU appearance thumbnails. Separate sidecars leave canonical artifacts intact.

Uses the inspector's metric leaf shapes and deterministic face colors, rasterized
isometrically and downsampled. Lighting/camera differ from the interactive view.
"""
import argparse
import json
import struct
from pathlib import Path
if not __package__:
    import _path
import numpy as np
from PIL import Image
from forge import inventory
from forge.render import _elevation_sprite

REVISION = 'spring-v2-thumb-3'

def face_color(rgb, coords, axis):
    c=coords.astype(np.uint32)
    def unit(a):
        a=a.copy();a^=a>>16;a*=np.uint32(2246822519);a^=a>>13
        a*=np.uint32(3266489917);a^=a>>16
        return (a&65535).astype(float)/65535*2-1
    key=c[:,0]*np.uint32(73856093)^c[:,1]*np.uint32(19349663)^c[:,2]*np.uint32(83492791)
    gain=1+.085*unit(key)+.045*unit(key^np.uint32(((axis*2+2)*2654435761)&0xffffffff))
    tint=1+unit(key^np.uint32(1597334677))[:,None]*np.array([.025,0,-.025])
    light=np.array([.45,.35,.82]);light/=np.linalg.norm(light)
    return np.clip(rgb*gain[:,None]*tint,0,255)*(.42+.62*light[axis])

def mask(uv, needle):
    p=uv.astype(np.float32)/np.float32(.13 if needle else .19);base=np.floor(p)
    def h(c):
        value=np.sin(c[:,0]*np.float32(127.1)+c[:,1]*np.float32(311.7))*np.float32(43758.5453)
        return value-np.floor(value)
    visible=np.zeros(len(p),dtype=bool)
    for x in (-1,0,1):
        for y in (-1,0,1):
            cell=base+np.array([x,y],dtype=np.float32)
            centre=cell+.5+np.column_stack((h(cell)-.5,h(cell+19)-.5))*.48
            angle=(h(cell+7)-.5)*2.6;q=p-centre
            for i in range(-2,3) if needle else (0,):
                a=angle+i*.28
                v=q-np.array([i*.08,abs(i)*.10],dtype=np.float32) if needle else q
                px=np.cos(a)*v[:,0]+np.sin(a)*v[:,1]
                py=-np.sin(a)*v[:,0]+np.cos(a)*v[:,1];t=py/.62
                width=.08525 if needle else .274
                visible|=(np.abs(t)<1)&(np.abs(px)<width*np.maximum(.06,1-t*t))
    return visible

def render(directory, size=384):
    directory=Path(directory);meta_path=directory/'tree-appearance.json'
    meta=json.loads(meta_path.read_text(encoding='utf8'))
    output=directory/'tree-appearance-thumb.png'
    record=directory/'tree-appearance-thumb.json'
    if output.exists() and record.exists():
        old=json.loads(record.read_text())
        if old.get('revision')==REVISION and old.get('preview_sha256')==meta['preview_sha256'] and old.get('sha256')==inventory.digest(output):return False
    original=(directory/'meta.json').read_bytes()
    assert inventory.digest(directory/'tree.vxa')==meta['geometry_sha256']
    raw=(directory/'tree-appearance.bin').read_bytes()
    assert inventory.digest(directory/'tree-appearance.bin')==meta['preview_sha256']
    count=struct.unpack_from('<I',raw,12)[0]
    coords=np.frombuffer(raw,dtype='<i2',count=count*3,offset=16).reshape(-1,3).astype(np.int64)
    mats=np.frombuffer(raw,dtype=np.uint8,count=count,offset=16+count*6)
    assert raw[16+count*7:20+count*7]==b'RGB1'
    rgb=np.frombuffer(raw,dtype=np.uint8,count=count*3,offset=20+count*7).reshape(-1,3)
    leaf=np.isin(mats,[19,20,21,22,24,25]) if meta.get('foliage_mask',True) else np.zeros(len(mats),dtype=bool);indices=np.flatnonzero(leaf)
    a,b,h=4,1,5
    dx,dy,shade=_elevation_sprite(a,b,h);ns=len(dx)
    sx=(coords[:,0]-coords[:,1])*a;sy=(coords[:,0]+coords[:,1])*b-coords[:,2]*h
    sx-=sx.min()-a;sy-=sy.min()
    width=int(sx.max()+a+1);height=int(sy.max()+2*b+h)
    depth=(coords[:,0]+coords[:,1])*h+coords[:,2]*2*b;depth-=depth.min()
    keys=(depth*count+np.arange(count))*ns+1
    zbuffer=np.zeros(width*height,dtype=np.int64)
    axes=[]
    # A canopy contains many voxels sharing the same face-plane coordinates.
    # Evaluate each mask on that 2D lattice once instead of once per 3D voxel.
    planes={}
    for axis,uv_axes in ((2,(0,1)),(0,(1,2)),(1,(0,2))):
        pairs=coords[indices][:,uv_axes]
        if not len(pairs):continue
        lo=pairs.min(axis=0);shape=pairs.max(axis=0)-lo+1
        if np.prod(shape)<len(pairs):
            lattice=np.indices(tuple(shape)).reshape(2,-1).T+lo
            planes[axis]=(lattice,tuple((pairs-lo).T),tuple(shape))
    for j,(x,y,s) in enumerate(zip(dx,dy,shade)):
        if s>.9:
            axis=2;local=np.array([(y/b+x/a)/2,(y/b-x/a)/2,1.])
        elif s>.7:
            axis=0;v=1-x/a;local=np.array([1.,v,1-(y-b*(1+v))/h])
        else:
            axis=1;u=1+x/a;local=np.array([u,1.,1-(y-b*(1+u))/h])
        axes.append(axis)
        keep=np.ones(count,dtype=bool)
        uv_axes=(0,1) if axis==2 else (1,2) if axis==0 else (0,2)
        if axis in planes:
            lattice,lookup,shape=planes[axis]
            uv=(lattice+local[list(uv_axes)])*meta['voxel_mm']/1000
            keep[indices]=mask(uv,meta['needle']).reshape(shape)[lookup]
        else:
            uv=(coords[indices]+local)[:,uv_axes]*meta['voxel_mm']/1000
            keep[indices]=mask(uv,meta['needle'])
        pixel=(sy+y)*width+sx+x
        np.maximum.at(zbuffer,pixel[keep],keys[keep]+j)
    occupied=zbuffer>0;winner=zbuffer[occupied]-1
    vox=(winner//ns)%count;faces=np.asarray(axes)[winner%ns]
    canvas=np.empty((height*width,3),dtype=np.uint8);canvas[:]=[24,26,30]
    colors=np.empty((len(vox),3),dtype=np.uint8)
    for axis in range(3):
        select=faces==axis;colors[select]=face_color(rgb[vox[select]],coords[vox[select]],axis).astype(np.uint8)
    canvas[occupied]=colors
    image=Image.fromarray(canvas.reshape(height,width,3));image.thumbnail((size-24,size-24),Image.Resampling.LANCZOS)
    square=Image.new('RGB',(size,size),(24,26,30));square.paste(image,((size-image.width)//2,(size-image.height)//2))
    pending=output.with_suffix('.tmp');square.save(pending,format='PNG');pending.replace(output)
    inventory.write_json(record,dict(revision=REVISION,preview_sha256=meta['preview_sha256'],sha256=inventory.digest(output)))
    assert (directory/'meta.json').read_bytes()==original
    return True

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--id');args=parser.parse_args()
    manifest=json.loads((inventory.ROOT/'out/tree-appearance-collection-v2/manifest.json').read_text())
    done=0
    for row in manifest['models']:
        if args.id and row['id']!=args.id:continue
        choices=[inventory.ROOT/'library'/row['species']/row['id'],inventory.CANDIDATES/row['species']/row['id']]
        target=next(d for d in choices if (d/'meta.json').exists())
        if render(target):done+=1
        print('THUMB',row['id'],flush=True)
    print('UPDATED',done,flush=True)

if __name__=='__main__':main()
