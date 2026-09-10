"""Read published freshwater samples without changing a terrain tile.

Lake and river masks use their published depth rasters. Sea is NOT included:
elevation control points require B-spline reconstruction with neighbor support
before testing sea level. These are raster samples, not subpixel shorelines.
"""
from pathlib import Path
import hashlib
import mmap
import os
import tempfile
import numpy as np
from .. import tile_codec as tc


def write_bake_water_source(path, packed, encoded, *, provider_id, cell_m):
    """Save a complete bake mask bound to the exact encoded tile bytes."""
    path=Path(path)
    h=tc._HEADER.unpack_from(encoded)
    a=np.asarray(packed)
    if a.dtype!=np.uint8 or a.shape!=(h[-1],h[-1]//8) or h[-1]%8:
        raise ValueError('Invalid packed water mask')
    if not np.isfinite(cell_m) or cell_m<=0:raise ValueError('Invalid cell size')
    metadata=dict(schema_version=1,seed=h[2],tile=[h[3],h[4]],edge=h[-1],
        source_sha256=hashlib.sha256(encoded).hexdigest(),provider_id=provider_id,
        cell_m=float(cell_m),includes_sea=True,bitorder='little')
    import json
    with tempfile.NamedTemporaryFile(dir=path.parent,suffix='.npz',delete=False) as out:
        temporary=Path(out.name)
        try:np.savez(out,packed=a,metadata=json.dumps(metadata,sort_keys=True))
        except BaseException:
            out.close();temporary.unlink();raise
    try:os.replace(temporary,path)
    finally:temporary.unlink(missing_ok=True)


def read_bake_water_source(path, encoded):
    """Refuse a source mask if its published tile binding does not match."""
    import json
    h=tc._HEADER.unpack_from(encoded)
    with np.load(path,allow_pickle=False) as data:
        info=json.loads(str(data['metadata']))
        packed=data['packed']
    if (info.get('schema_version')!=1 or info.get('includes_sea') is not True
        or info.get('bitorder')!='little' or info.get('seed')!=h[2]
        or info.get('tile')!=[h[3],h[4]] or info.get('edge')!=h[-1]
        or info.get('source_sha256')!=hashlib.sha256(encoded).hexdigest()):
        raise ValueError('Water source does not match published tile')
    if packed.dtype!=np.uint8 or packed.shape!=(h[-1],h[-1]//8):
        raise ValueError('Invalid packed water source')
    return np.unpackbits(packed,axis=1,bitorder='little').astype(bool),info


def read_published_freshwater_mask(path):
    path=Path(path)
    with path.open('rb') as stream, mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ) as data:
        sha=hashlib.sha256(data).hexdigest()
        h=tc._HEADER.unpack_from(data)
        e=tc._V2_EXT.unpack_from(data,tc._HEADER.size)
        if h[0]!=tc.MAGIC or h[1]!=tc.VERSION_V2 or h[5]!=tc.FINE_SCALE:
            raise ValueError('Expected fine V2 terrain tile')
        block,predictor,quant,codec,bake,flags,base,parent,reserved,count=e
        if predictor!=tc.PRED_MED or quant not in tc.QUANT_MM or codec not in (tc.CODEC_RAW,tc.CODEC_ZSTD):
            raise ValueError('Unsupported tile encoding')
        if parent!=0 or reserved!=tc._ZERO_RESERVED3 or not 1<=block<=13 or not h[-1] or h[-1]%(1<<block):
            raise ValueError('Invalid tile geometry/header')
        required_flags=tc.FLAG_WATER_PRESENT|tc.FLAG_BATHY_PRESENT
        if flags&required_flags!=required_flags:
            raise ValueError('Published river and lake depth planes required')
        offset=tc._HEADER.size+tc._V2_EXT.size
        end=offset+count*tc._SECTION_ENTRY.size
        if end>len(data):raise ValueError('Truncated section table')
        sections={}
        for i in range(count):
            sid,start,length=tc._SECTION_ENTRY.unpack_from(data,offset+i*tc._SECTION_ENTRY.size)
            if sid in sections or start<end or start+length>len(data):raise ValueError('Invalid section extent')
            sections[sid]=(start,length)
        previous=end
        for start,length in sorted(sections.values()):
            if start<previous:raise ValueError('Overlapping sections')
            previous=start+length
        def plane(index,payload):
            blocks=[]
            for sid in (index,payload):
                if sid not in sections:raise ValueError('Required depth/elevation section missing')
                start,length=sections[sid];blocks.append(data[start:start+length])
            return tc._decode_plane(*blocks,size=h[-1],block_log2=block,codec=codec,
                elem_dtype='<i2',out_dtype=np.int16,decompressor=None)
        wet=np.zeros((h[-1],h[-1]),dtype=bool)
        for ix,dx in [(tc.SECTION_WATER_INDEX,tc.SECTION_WATER_DATA),
                      (tc.SECTION_BATHY_DEPTH_INDEX,tc.SECTION_BATHY_DEPTH_DATA)]:
            depth=plane(ix,dx);wet|=depth>=0;del depth
        if hashlib.sha256(data).hexdigest()!=sha:raise ValueError('Source tile changed during read')
    return wet,dict(path=str(path.resolve()),sha256=sha,seed=h[2],tile=[h[3],h[4]],
        edge=h[-1],bake_version=bake,includes_sea=False,scope=__doc__)
