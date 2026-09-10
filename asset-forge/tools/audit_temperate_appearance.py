"""Repair pre-release VAC2 pitch units and audit actual palette/geometry parity."""
import hashlib,json,struct,time,collections
from pathlib import Path
if not __package__:import _path
import numpy as np
from forge import inventory,vxa
from tools.temperate_appearance import protected,in_scope
from tools.export_temperate_runtime_appearance import ensure,RECORD
ROOT=inventory.ROOT

def main():
    start=time.time();ledger=ROOT/'out/temperate-appearance-rollout';before=json.loads((ledger/'before.json').read_text());counts=collections.Counter();pitches=collections.Counter();rows=[]
    for name,original in before['artifacts'].items():
        d=ROOT/name;meta=json.loads((d/'tree-appearance.json').read_text())
        assert protected(d)==original,name
        if meta['revision']!='temperate-spring-variation-v1':continue
        row=ensure(d);meta=json.loads((d/'tree-appearance.json').read_text());blob=(d/'tree-appearance-runtime.vac').read_bytes();grid=vxa.read(d/'tree.vxa')
        assert blob[:4]==b'VAC1' and struct.unpack_from('<I',blob,4)[0]==2
        assert blob[96:128]==hashlib.sha256(blob[:96]+blob[128:]).digest()
        assert blob[48:64]==hashlib.md5((d/'tree.vxa').read_bytes()).digest()
        assert blob[64:96].hex()==original['tree.vxa']
        assert struct.unpack_from('<III',blob,8)==grid.shape
        assert struct.unpack_from('<iii',blob,20)==tuple(grid.origin)
        pitch,count,needle,flags=struct.unpack_from('<4I',blob,32)
        assert pitch==round(grid.voxel_m*1000000) and meta['voxel_mm']==grid.voxel_m*1000
        assert needle==0 and flags==0 and meta['foliage_mask'] is False
        records=np.frombuffer(blob,dtype=RECORD,offset=128);coords=np.argwhere(grid.data!=0)
        assert count==len(coords)==len(records) and np.array_equal(coords,records['xyz'])
        assert np.array_equal(records['material'],grid.data[tuple(coords.T)])
        preview=(d/'tree-appearance.bin').read_bytes();n=struct.unpack_from('<I',preview,12)[0];surface=np.frombuffer(preview,dtype='<i2',count=n*3,offset=16).reshape(-1,3)
        selected=np.searchsorted(np.ravel_multi_index(coords.T,grid.shape),np.ravel_multi_index(surface.T,grid.shape))
        assert np.array_equal(records['rgb'][selected],np.frombuffer(preview,dtype='u1',offset=20+n*7).reshape(-1,3))
        assert protected(d)==original
        body=json.loads((d/'spec.json').read_text());counts[body['kind']]+=1;pitches[str(meta['voxel_mm'])]+=1
        rows.append(dict(source=name,kind=body['kind'],**row))
        if len(rows)%100==0:print('VAC2 AUDIT',len(rows),flush=True)
    assert all(inventory.digest(ROOT/n)==h for n,h in before['species'].items())
    scope=[]
    for p in sorted((ROOT/'specs').glob('*.json')):
        body=json.loads(p.read_text())
        if in_scope(body):scope.append(dict(species=p.stem,kind=body['kind']))
    inventory.write_json(ledger/'scope.json',dict(profiles=scope,count=len(scope),rule='positive temperate_forest weight, respecting biome_allow'))
    report=dict(count=len(rows),kinds=dict(counts),pitches_mm=dict(pitches),models=rows,geometry_and_decisions_preserved=True,all_packet_records_match_original=True,all_surface_rgb_preserved=True,elapsed_seconds=round(time.time()-start,2))
    inventory.write_json(ledger/'runtime-audit.json',report);print(json.dumps({k:v for k,v in report.items() if k!='models'}),flush=True)
if __name__=='__main__':main()
