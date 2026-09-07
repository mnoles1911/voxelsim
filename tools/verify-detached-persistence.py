"""Independently validate detached snapshot formats 1 through 4."""
from pathlib import Path
from checkpoint_store import resolve_checkpoint
import hashlib, json, math, struct, zlib
root = Path(__file__).resolve().parents[1]
LIMIT = 512 * 1024**2

class Reader:
    def __init__(self, data): self.data, self.offset = data, 0
    def take(self, size):
        assert 0 <= size <= len(self.data)-self.offset, 'Truncated record'
        start=self.offset; self.offset+=size; return self.data[start:self.offset]
    def unpack(self, fmt): return struct.unpack('<'+fmt, self.take(struct.calcsize('<'+fmt)))
    def bytes(self, maximum=LIMIT):
        size,=self.unpack('i'); assert 0 <= size <= maximum; return self.take(size)
    def done(self): assert self.offset==len(self.data), 'Trailing record bytes'

def plant_record(record):
    r=Reader(record); name,=r.unpack('i'); transform=r.unpack('10d')
    size=r.unpack('3i'); origin=r.unpack('3i'); mm,max_z,collision,severed=r.unpack('diII')
    assert name in range(4) and all(map(math.isfinite,transform))
    assert all(0<n<=4096 for n in size) and math.prod(size)<=64*1024**2
    assert all(abs(n)<=1000000 for n in origin) and mm in (25,50,100)
    assert 0<=max_z<=size[2] or max_z==2147483647
    assert collision in (0,1) and severed in (0,1)
    grid=r.bytes(64*1024**2); assert len(grid)==math.prod(size); r.done()
    return name,hashlib.sha256(record).hexdigest(),hashlib.sha256(grid).hexdigest()

def snapshot(terrain_path, expected_kinds=(1,2,2,3,3,3,3)):
    terrain_path = resolve_checkpoint(terrain_path)
    terrain_path=Path(terrain_path)
    sidecar=Path(str(terrain_path)+'.detached-'+hashlib.md5(terrain_path.read_bytes()).hexdigest()+'.bin')
    blob=sidecar.read_bytes(); f=Reader(blob); magic,version,crc=f.unpack('III')
    assert magic==0x56444252 and version in (1,2,3,4)
    if version==1: payload=f.bytes()
    else:
        raw_size,=f.unpack('i'); assert 4<=raw_size<=LIMIT
        decoder=zlib.decompressobj(); payload=decoder.decompress(f.bytes(),raw_size+1)
        assert decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail
        assert len(payload)==raw_size
    f.done(); assert zlib.crc32(payload)==crc, 'Snapshot CRC mismatch'
    r=Reader(payload); marker,=r.unpack('i')
    if version==4: assert marker==-4; count,=r.unpack('i')
    else: count=marker
    assert 0<=count<=4096
    kinds=[]; plants={}; grids={}; objects=[]; ids=set(); remaining=None
    for _ in range(count):
        if version<4:
            kind,=r.unpack('B'); record=r.bytes(); assert kind in (1,2,3) and record
            kinds.append(kind)
            if kind==1:
                assert record[-9]==1, 'Resource lost harvestable category'
                remaining,=struct.unpack_from('<d',record,len(record)-8)
            if kind==3:
                name,digest,grid_digest=plant_record(record); assert name not in plants
                plants[name],grids[name]=digest,grid_digest
            continue
        identity=r.take(16).hex(); revision,geometry_revision,kind,state=r.unpack('QQBB')
        assert identity!='00'*16 and identity not in ids and revision>0; ids.add(identity)
        assert kind in (1,2,3) and state in range(5)
        transform=r.unpack('10d'); velocity=r.unpack('3d'); angular=r.unpack('3d'); bounds=r.unpack('3d')
        assert all(map(math.isfinite,transform+velocity+angular+bounds)) and all(n>=0 for n in bounds)
        life,timer=r.unpack('Bd'); owner=r.take(16).hex(); retained,active,geometry_format=r.unpack('III')
        assert life in range(4) and math.isfinite(timer) and timer>=0
        assert retained in (0,1) and active in (0,1) and geometry_format in (0,1)
        dynamic,geometry=r.bytes(1024**2),r.bytes(); tombstone=state==4
        assert (not geometry and not dynamic) if tombstone else bool(geometry)
        objects.append(dict(id=identity,revision=revision,geometry_revision=geometry_revision,kind=kind,state=state,
                            owner=owner,retained=bool(retained),lifetime_kind=life,remaining_seconds=timer,
                            geometry_bytes=len(geometry),dynamic_bytes=len(dynamic),geometry_sha256=hashlib.sha256(geometry).hexdigest(),
                            dynamic_sha256=hashlib.sha256(dynamic).hexdigest()))
        if tombstone: continue
        if geometry_format == 1:
            assert dynamic, 'Missing dynamic actor state'
            if kind in (2, 3):
                assert len(dynamic) >= 4 and len(geometry) >= 4
                assert struct.unpack_from('<I', dynamic)[0] == struct.unpack_from('<I', geometry)[0] == 1
            else:
                cells, = struct.unpack_from('<i', geometry)
                assert 0 < cells <= 1024**2 and len(geometry) == 4 + 24*cells
                assert len(dynamic) == 153
                dynamic_life, dynamic_timer, simulating, ticking = struct.unpack_from('<BdII', dynamic, len(dynamic)-17)
                assert dynamic_life == life and dynamic_timer + .001 >= timer
                assert simulating in (0, 1) and ticking in (0, 1)
        kinds.append(kind)
        if kind==1:
            assert life==1, 'Resource lost harvestable category'
            remaining=timer
        if kind==3:
            if geometry_format==0: record=geometry
            else:
                assert len(dynamic)>=4 and len(geometry)>=4
                assert struct.unpack_from('<I',dynamic)[0]==struct.unpack_from('<I',geometry)[0]==1
                record=dynamic[4:]+geometry[4:]
            name,digest,grid_digest=plant_record(record); assert name not in plants
            plants[name],grids[name]=digest,grid_digest
    r.done()
    if expected_kinds is not None: assert sorted(kinds)==list(expected_kinds), f'Unexpected live actors: {kinds}'
    assert remaining is None or math.isfinite(remaining) and remaining>=0
    return dict(file=str(sidecar),version=version,compressed_bytes=len(blob),raw_bytes=len(payload),record_count=count,
                tombstone_count=sum(o['state']==4 for o in objects),plants=plants,plant_grids=grids,objects=objects,
                resource_remaining_seconds=remaining)

def main():
    roundtrip=(root/'Saved/detached-roundtrip-game.log').read_text(errors='replace')
    restart=(root/'Saved/detached-restart-game.log').read_text(errors='replace')
    assert 'DetachedSave PROBE PASS' in roundtrip and 'timerMatched=1' in roundtrip
    assert 'DetachedSave CORRUPTION PASS actorsUnchanged=1' in roundtrip
    assert 'DetachedSave RESTART PASS' in restart
    assert 'DetachedSave wrote actors=7' in restart, 'Shutdown did not retain seven actors'
    original=snapshot(root/'ue-project/Saved/Tests/detached-roundtrip.vxlog')
    shutdown=snapshot(root/'ue-project/Saved/SaveGames/detached-restore-probe/world.vxlog')
    assert abs(original['resource_remaining_seconds']-123)<.01
    assert 60<shutdown['resource_remaining_seconds']<123, 'Shutdown reset/lost timer'
    assert shutdown['plants']==original['plants'] and shutdown['plant_grids']==original['plant_grids']
    result=dict(roundtrip=True,corruption_refused=True,fresh_process_restore=True,shutdown_preserved_actors=True,original=original,shutdown=shutdown)
    (root/'Saved/detached-persistence-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__': main()
