"""Build complete, geometry-bound presentation colors for editable tree assets.

VAC1 stores all occupied source voxels, including buried wood and leaves. The
128-byte header binds dimensions, origin, pitch, geometry MD5/SHA256 and payload
integrity SHA256. Sorted 10-byte records carry xyz uint16, physical material byte, RGB8.
This is appearance only: no source VXA, decision or world catalog is modified.
"""
import argparse
import hashlib
import json
import struct
import tempfile
from pathlib import Path
import _path
import numpy as np
from forge import inventory,vxa
from tree_appearance_pilot import build

ROOT=inventory.ROOT
OUT=ROOT/'out/tree-runtime-appearance-v1'
RECORD=np.dtype([('xyz','<u2',(3,)),('material','u1'),('rgb','u1',(3,))])

def export(row,output):
    choices=[ROOT/'library'/row['species']/row['id'],inventory.CANDIDATES/row['species']/row['id']]
    source=next(d for d in choices if (d/'meta.json').exists())
    original=(source/'meta.json').read_bytes();geometry=(source/'tree.vxa').read_bytes()
    sha=hashlib.sha256(geometry).hexdigest();md5=hashlib.md5(geometry).hexdigest()
    assert sha==row['geometry_sha256']
    palette=json.loads((ROOT/'rules/tree-appearance-spring-v2.json').read_text())['profiles'][row['species']]
    grid=vxa.read(source/'tree.vxa')
    assert max(grid.shape)<=65535
    with tempfile.TemporaryDirectory(prefix='runtime-colors-',dir=ROOT/'out') as temp:
        build(row['species'],row['seed'],source=source,output=temp,palette=tuple(palette[k] for k in ('bark','foliage','fresh_growth','bark_pattern')),include_interior=True)
        raw=(Path(temp)/'voxels.bin').read_bytes();count=struct.unpack_from('<I',raw,12)[0]
        coords=np.frombuffer(raw,dtype='<i2',count=count*3,offset=16).reshape(-1,3)
        assert np.all(coords>=0)
        records=np.empty(count,dtype=RECORD)
        records['xyz']=coords;records['material']=np.frombuffer(raw,dtype='u1',count=count,offset=16+count*6)
        records['rgb']=np.frombuffer((Path(temp)/'species.rgb').read_bytes(),dtype='u1').reshape(-1,3)
        assert count==np.count_nonzero(grid.data),'Missing occupied source cells'
        # Match every already-approved preview color exactly before export.
        preview=(source/'tree-appearance.bin').read_bytes();pc=struct.unpack_from('<I',preview,12)[0]
        pxyz=np.frombuffer(preview,dtype='<i2',count=pc*3,offset=16).reshape(-1,3)
        keys=np.ravel_multi_index(coords.T,grid.shape)
        selected=np.searchsorted(keys,np.ravel_multi_index(pxyz.T,grid.shape))
        assert preview[16+pc*7:20+pc*7]==b'RGB1'
        expected=np.frombuffer(preview,dtype='u1',offset=20+pc*7).reshape(-1,3)
        assert np.array_equal(records['rgb'][selected],expected),'Preview color drift'
        payload=records.tobytes()
    header=struct.pack('<4sI3I3i4I',b'VAC1',1,*grid.shape,*map(int,grid.origin),round(grid.voxel_m*1000),count,int(row['needle']),0)
    header+=bytes.fromhex(md5)+bytes.fromhex(sha)
    header+=hashlib.sha256(header+payload).digest()
    assert len(header)==128
    output.mkdir(parents=True,exist_ok=True)
    path=output/(md5+'.vac');tmp=path.with_suffix('.tmp');tmp.write_bytes(header+payload);tmp.replace(path)
    assert (source/'meta.json').read_bytes()==original
    assert inventory.digest(source/'tree.vxa')==sha
    return dict(id=row['id'],species=row['species'],seed=row['seed'],geometry_md5=md5,geometry_sha256=sha,
                file=path.name,sha256=inventory.digest(path),voxels=count,preview_voxels=pc,
                bytes=path.stat().st_size,source=str(source.relative_to(ROOT)))

def publish_endorsed(source, output):
    """Publish only a reviewed appearance attached to an endorsed library VXA.

    Called before bank publication, so a failed appearance export cannot leave
    a newly published tree with an absent or mismatched appearance packet.
    """
    source=Path(source).resolve();output=Path(output)
    source.relative_to((ROOT/'library').resolve())
    meta=json.loads((source/'meta.json').read_text())
    if meta.get('inventory_candidate') or not meta.get('visual_approved') or meta.get('review_status')!='endorsed':
        raise ValueError('Appearance publication requires an explicitly endorsed variant')
    palette_path=ROOT/'rules/tree-appearance-spring-v2.json'
    palettes=json.loads(palette_path.read_text())
    if meta['species'] not in palettes['profiles']:return None
    info=json.loads((source/'tree-appearance.json').read_text())
    active=json.loads((ROOT/'rules/tree-appearance-active.json').read_text())
    geometry_sha=inventory.digest(source/'tree.vxa');preview_sha=inventory.digest(source/'tree-appearance.bin')
    palette_sha=inventory.digest(palette_path)
    if info.get('geometry_sha256')!=geometry_sha or info.get('preview_sha256')!=preview_sha or info.get('palette_sha256')!=palette_sha or active.get('palette_sha256')!=palette_sha:
        raise ValueError('Endorsed appearance is stale or corrupt; rebuild before publishing')
    fingerprint=dict(export_revision='vac1-complete-v1',geometry_sha256=geometry_sha,preview_sha256=preview_sha,palette_sha256=palette_sha)
    record_path=output/(meta['id']+'.json')
    if record_path.exists():
        old=json.loads(record_path.read_text())
        packet=output/(hashlib.md5((source/'tree.vxa').read_bytes()).hexdigest()+'.vac')
        if all(old.get(k)==v for k,v in fingerprint.items()) and packet.exists() and inventory.digest(packet)==old.get('sha256'):
            return old
    row=dict(id=meta['id'],species=meta['species'],seed=meta['seed'],geometry_sha256=geometry_sha,needle=bool(info['needle']))
    result=export(row,output);result.update(fingerprint)
    inventory.write_json(record_path,result)
    return result

def refresh_published_inventory(output, banks):
    """Commit the intersection of current endorsements and exact bank geometry.

    Old sidecars remain usable by saved objects, but are not placement authority.
    Called after bank export, including partial batches, so new endorsements and
    withdrawals become visible together in one atomic inventory replacement.
    """
    output=Path(output);banks=Path(banks);results=[];unavailable=[]
    profiles=json.loads((ROOT/'rules/tree-appearance-spring-v2.json').read_text())['profiles']
    for path in sorted((ROOT/'library').glob('*/*/meta.json')):
        meta=json.loads(path.read_text())
        if meta.get('kind')!='tree' or meta.get('species') not in profiles:continue
        if meta.get('review_status')!='endorsed' or not meta.get('visual_approved') or meta.get('inventory_candidate'):continue
        bank=banks/meta['species']/(meta['id']+'.vxa')
        if not bank.is_file() or inventory.digest(bank)!=inventory.digest(path.parent/'tree.vxa'):
            unavailable.append(meta['id']);continue
        result=publish_endorsed(path.parent,output)
        if result:results.append(result)
    report=dict(models=results,count=len(results),unavailable_banks=unavailable)
    inventory.write_json(output/'published.json',report)
    return report


def main():
    parser=argparse.ArgumentParser();mode=parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--id');mode.add_argument('--publish-endorsed',action='store_true')
    parser.add_argument('--banks',type=Path,default=ROOT/'out/engine/banks',
                        help='Exact exported banks required for placement publication')
    parser.add_argument('--output',type=Path,default=OUT);args=parser.parse_args()
    if args.publish_endorsed:
        report=refresh_published_inventory(args.output,args.banks)
        print(json.dumps(report,indent=2));return
    manifest=json.loads((ROOT/'out/tree-appearance-collection-v2/manifest.json').read_text())
    row=next(r for r in manifest['models'] if r['id']==args.id)
    result=export(row,args.output)
    inventory.write_json(args.output/(args.id+'.json'),result)
    print(json.dumps(result))

if __name__=='__main__':main()
