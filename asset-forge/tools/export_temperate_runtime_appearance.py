"""VAC1 version2 complete source colors; local export is NOT world publication."""
import hashlib,json,struct
from pathlib import Path
if not __package__:import _path
import numpy as np
from forge import inventory,materials,vxa
RECORD=np.dtype([('xyz','<u2',(3,)),('material','u1'),('rgb','u1',(3,))])

def export(source,output=None):
    source=Path(source).resolve();meta=json.loads((source/'tree-appearance.json').read_text())
    if meta.get('revision')!='temperate-spring-variation-v1' or meta.get('foliage_mask') is not False:raise ValueError('Generic opaque reviewed variation required')
    geometry=(source/'tree.vxa').read_bytes();sha=hashlib.sha256(geometry).hexdigest();md5=hashlib.md5(geometry).hexdigest()
    if sha!=meta['geometry_sha256'] or inventory.digest(source/'tree-appearance.bin')!=meta['preview_sha256']:raise ValueError('stale generic appearance')
    grid=vxa.read(source/'tree.vxa');coords=np.argwhere(grid.data!=0)
    if not len(coords) or max(grid.shape)>65535:raise ValueError('invalid source dimensions/count')
    mats=grid.data[tuple(coords.T)].astype(np.uint8);records=np.empty(len(coords),dtype=RECORD);records['xyz']=coords;records['material']=mats
    records['rgb']=np.asarray([materials.color(int(m)) for m in mats],dtype=np.uint8)
    # Exact saved surface colors take precedence over material defaults. The
    # untouched interior retains its canonical physical material base color.
    preview=(source/'tree-appearance.bin').read_bytes();count=struct.unpack_from('<I',preview,12)[0]
    surface=np.frombuffer(preview,dtype='<i2',count=count*3,offset=16).reshape(-1,3)
    keys=np.ravel_multi_index(coords.T,grid.shape);selected=np.searchsorted(keys,np.ravel_multi_index(surface.T,grid.shape))
    if preview[16+count*7:20+count*7]!=b'RGB1':raise ValueError('missing base RGB')
    records['rgb'][selected]=np.frombuffer(preview,dtype='u1',offset=20+count*7).reshape(-1,3)
    payload=records.tobytes();header=struct.pack('<4sI3I3i4I',b'VAC1',2,*grid.shape,*map(int,grid.origin),round(grid.voxel_m*1000000),len(coords),0,0)
    header+=bytes.fromhex(md5)+bytes.fromhex(sha);header+=hashlib.sha256(header+payload).digest();assert len(header)==128
    dest=Path(output)/(md5+'.vac') if output else source/'tree-appearance-runtime.vac';dest.parent.mkdir(parents=True,exist_ok=True)
    temp=dest.with_suffix('.tmp');temp.write_bytes(header+payload);temp.replace(dest)
    if inventory.digest(source/'tree.vxa')!=sha:raise ValueError('geometry changed during appearance export')
    row=dict(file=dest.name,version=2,pitch_encoding='integer-micrometres',geometry_md5=md5,geometry_sha256=sha,sha256=inventory.digest(dest),voxels=len(coords),voxel_mm=grid.voxel_m*1000,voxel_um=round(grid.voxel_m*1000000),preview_sha256=meta['preview_sha256'],foliage_mask=False)
    inventory.write_json(source/'tree-appearance-runtime.json',row)
    if meta['voxel_mm']!=grid.voxel_m*1000:
        meta['voxel_mm']=grid.voxel_m*1000;inventory.write_json(source/'tree-appearance.json',meta)
    return row


def ensure(source):
    source=Path(source);record=source/'tree-appearance-runtime.json';packet=source/'tree-appearance-runtime.vac'
    if record.is_file() and packet.is_file():
        row=json.loads(record.read_text());meta=json.loads((source/'tree-appearance.json').read_text())
        if row.get('pitch_encoding')=='integer-micrometres' and row.get('geometry_sha256')==meta['geometry_sha256'] and row.get('preview_sha256')==meta['preview_sha256'] and inventory.digest(packet)==row.get('sha256'):return row
    return export(source)
