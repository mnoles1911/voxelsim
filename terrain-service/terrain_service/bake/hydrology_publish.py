"""Stage corrected water-distance tiles in a fresh, explicitly separate output.

Requires complete hash-bound bake masks for every target's 3x3 neighborhood.
Does not activate a cache, modify original tiles, or declare habitat acceptance.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
from .. import tile_codec as tc
from .hydrology_distance import distance_from_final_masks
from .hydrology_sources import read_bake_water_source


def stage(tile_dir, mask_dir, output, targets):
    tile_dir=Path(tile_dir).resolve();mask_dir=Path(mask_dir).resolve();output=Path(output).resolve()
    if output.exists() or output==tile_dir or tile_dir in output.parents or output==mask_dir or mask_dir in output.parents:
        raise ValueError('Use a fresh output outside both source directories')
    targets=sorted(set(tuple(p) for p in targets))
    if not targets:raise ValueError('At least one target required')
    needed=sorted({(x+dx,y+dy) for x,y in targets for dx in (-1,0,1) for dy in (-1,0,1)})
    records=[];common=None
    # Validate every dependency before creating any output. Read encoded tile
    # snapshots sequentially; do not hold nine full decoded terrain tiles.
    for x,y in needed:
        encoded=(tile_dir/f'{x}_{y}.vxtl').read_bytes()
        source=(mask_dir/f'{x}_{y}.water-source.npz').read_bytes()
        wet,info=read_bake_water_source(io.BytesIO(source),encoded)
        if info['tile']!=[x,y]:raise ValueError('File coordinates do not match tile')
        identity=(info['provider_id'],info['seed'],info['edge'],info['cell_m'])
        if common is not None and identity!=common:raise ValueError('Mixed water source identities')
        common=identity
        records.append(dict(tile=[x,y],tile_sha256=info['source_sha256'],
            mask_sha256=hashlib.sha256(source).hexdigest()))
        del wet,encoded,source
    provenance=dict(algorithm='neighbor-final-water-distance-v1',source_provider=common[0],
        seed=common[1],edge=common[2],cell_m=common[3],targets=targets,sources=records)
    digest=hashlib.sha256(json.dumps(provenance,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    output.mkdir(parents=True)
    result=dict(**provenance,publication_id='hydrology-'+digest,outputs=[],activated=False)
    for x,y in targets:
        # Memory stays bounded to one target and its halo as a world grows.
        masks={}
        for dx in (-1,0,1):
            for dy in (-1,0,1):
                nx,ny=x+dx,y+dy
                record=next(r for r in records if r['tile']==[nx,ny])
                neighbor=(tile_dir/f'{nx}_{ny}.vxtl').read_bytes()
                source=(mask_dir/f'{nx}_{ny}.water-source.npz').read_bytes()
                if (hashlib.sha256(neighbor).hexdigest()!=record['tile_sha256'] or
                    hashlib.sha256(source).hexdigest()!=record['mask_sha256']):
                    raise ValueError('Neighbor source changed during staging')
                masks[(nx,ny)],_=read_bake_water_source(io.BytesIO(source),neighbor)
                del neighbor,source
        encoded=(tile_dir/f'{x}_{y}.vxtl').read_bytes()
        expected=next(r['tile_sha256'] for r in records if r['tile']==[x,y])
        if hashlib.sha256(encoded).hexdigest()!=expected:raise ValueError('Target tile changed during staging')
        tile=tc.decode_v2(encoded)
        if tile.place_dist_water is None:raise ValueError('Target lacks placement channels')
        tile.place_dist_water=distance_from_final_masks(masks,(x,y),cell_m=common[3])
        updated=tc.encode_v2(tile)
        name=f'{x}_{y}.vxtl';(output/name).write_bytes(updated)
        result['outputs'].append(dict(file=name,sha256=hashlib.sha256(updated).hexdigest()))
    # Manifest is the completion marker; partial failures cannot look published.
    (output/'hydrology-publication.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tiles',required=True,type=Path);p.add_argument('--masks',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--target',required=True,nargs=2,type=int,action='append',metavar=('X','Y'))
    a=p.parse_args();print(json.dumps(stage(a.tiles,a.masks,a.output,a.target),indent=2))
