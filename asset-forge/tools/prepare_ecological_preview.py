"""Stage an isolated runtime test whitelist, without endorsing library assets.

The output MUST be a fresh directory under out/ecological-placement/previews.
Its appearance/published.json is an engine-format fixture, not the real game
publication. PREVIEW_ONLY.json records every source's unchanged review status.
"""
import argparse
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import shutil
import struct

import _path
from forge import manifest, spec as specs
from ecological_inventory import ROOT, digest, geometry, stable_id
from compile_ecological_placement import compile_rules


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def seed_plan(names, seeds, overrides=None):
    overrides={} if overrides is None else overrides
    if not isinstance(overrides,dict) or set(overrides)-set(names) or len(set(names))!=len(names):
        raise ValueError('Seed overrides require unique known species')
    result={}
    for name in names:
        selected=overrides.get(name,seeds)
        if not isinstance(selected,list) or not selected or len(selected)>65535:
            raise ValueError(f'Expected nonempty unique seed list: {name}')
        if any(type(seed) is not int or not 0<=seed<=0xffffffff for seed in selected):
            raise ValueError(f'Invalid asset seed: {name}')
        if len(set(selected))!=len(selected):
            raise ValueError(f'Duplicate asset seed: {name}')
        result[name]=list(selected)
    return result


def apply_low_cover_pilot(placement):
    """Private relative-weight experiment; trees and water-dependent roles stay authored.

    Favor grass profiles whose entire supplied inventory is at most 1m tall.
    Reduce other dry understory weights instead of overflowing 1000-per-mille.
    This does not guarantee density ratios after habitat/occupancy gates.
    """
    favored=[]
    for p in placement['profiles']:
        if p['kind']=='tree' or p.get('cover_role') in ('wetland','inert'):continue
        low=p['kind']=='grass' and p['variants'] and max(v['height_mm'] for v in p['variants'])<=1000
        if low:favored.append(p['species'])
        else:p['community_weights_per_mille']=[max(1,w//4) if w else 0 for w in p['community_weights_per_mille']]
    return sorted(favored)


def stage(output, names, seeds, reuse_packets_from=None, seed_overrides=None, tall_canopy_pilot=False, low_cover_pilot=False):
    selected_seeds=seed_plan(names,seeds,seed_overrides)
    output = output.resolve()
    allowed = (ROOT/'out/ecological-placement/previews').resolve()
    output.relative_to(allowed)
    if output == allowed or output.exists():
        raise ValueError('Use a fresh named preview directory')
    entries, bodies, protected = [], {}, {}
    for name in names:
        record = ROOT/'library'/name/'species.json'
        body = json.loads(record.read_text(encoding='utf-8'))['baseline_spec']
        bodies[name] = body
        for seed in selected_seeds[name]:
            model_id = f'{name}-{seed:04d}'
            choices = [ROOT/'library'/name/model_id, ROOT/'out/forge-candidates'/name/model_id]
            source = next((p for p in choices if (p/'tree.vxa').is_file()), None)
            if source is None:
                raise ValueError(f'Missing preview source: {model_id}')
            meta_bytes = (source/'meta.json').read_bytes()
            meta = json.loads(meta_bytes)
            if meta.get('review_status') == 'rejected' or meta.get('imported'):
                raise ValueError(f'Rejected/imported preview source: {model_id}')
            if (meta['id'], meta['species'], meta['seed']) != (model_id, name, seed):
                raise ValueError(f'Preview identity mismatch: {source}')
            blob = (source/'tree.vxa').read_bytes()
            measured = geometry(blob)
            protected[str(source/'meta.json')] = digest(meta_bytes)
            protected[str(source/'tree.vxa')] = digest(blob)
            entries.append((name, model_id, seed, source, meta, blob, measured))
    output.mkdir(parents=True)
    appearances, inventory_rows = [], []
    cached = {}
    if reuse_packets_from:
        cache_root = reuse_packets_from.resolve()
        cache_root.relative_to(allowed)
        cache_publication = json.loads((cache_root/'appearance/published.json').read_text())
        if cache_publication.get('preview_only') is not True:
            raise ValueError('Packet reuse requires an isolated preview')
        cached = {r['id']: r for r in cache_publication['models']}
    tree_manifest = {r['id']: r for r in json.loads((ROOT/'out/tree-appearance-collection-v2/manifest.json').read_text())['models']}
    for name, model_id, seed, source, meta, blob, measured in entries:
        md5 = hashlib.md5(blob).hexdigest()
        bank = output/'banks'/name/(model_id+'.vxa')
        bank.parent.mkdir(parents=True, exist_ok=True)
        bank.write_bytes(blob)
        appearance_dir = output/'appearance'
        appearance_dir.mkdir(exist_ok=True)
        if model_id in cached:
            row = dict(cached[model_id])
            packet_path = cache_root/'appearance'/(md5+'.vac')
            if row['geometry_sha256'] != digest(blob) or row['sha256'] != digest(packet_path.read_bytes()):
                raise ValueError(f'Stale cached preview packet: {model_id}')
            shutil.copyfile(packet_path, appearance_dir/(md5+'.vac'))
        elif meta['kind'] == 'tree':
            from export_tree_runtime_appearance import export
            row = export(tree_manifest[model_id], appearance_dir)
        else:
            packet_path = source/'tree-appearance-runtime.vac'
            row = json.loads((source/'tree-appearance-runtime.json').read_text())
            if row['geometry_sha256'] != digest(blob) or row['sha256'] != digest(packet_path.read_bytes()):
                raise ValueError(f'Stale appearance packet: {model_id}')
            shutil.copyfile(packet_path, appearance_dir/(md5+'.vac'))
        row.update(id=model_id, species=name, seed=seed, file=md5+'.vac', preview_only=True)
        appearances.append(row)
        stats = meta.get('stats', {})
        inventory_rows.append({'species': name, 'species_stable_id': stable_id('species:'+name),
            'id': model_id, 'variant_stable_id': stable_id('variant:'+model_id), 'asset_seed': seed,
            'kind': meta['kind'], 'bank_file': bank.relative_to(output).as_posix(),
            'geometry_sha256': digest(blob), 'geometry_md5': md5, 'geometry': measured,
            'size_class': stats.get('size_class'), 'growth_form': stats.get('growth_form')})
        print(f'Preview staged {model_id}', flush=True)
    publication = {'preview_only': True, 'models': appearances, 'count': len(appearances)}
    write(output/'appearance/published.json', publication)
    inventory = {'preview_only': True, 'variants': inventory_rows, 'refused': [],
                 'publication_sha256': digest((output/'appearance/published.json').read_bytes())}
    placement = compile_rules(ROOT, preview_inventory=inventory)
    favored_cover=apply_low_cover_pilot(placement) if low_cover_pilot else []
    write(output/'placement.json', placement)
    # File measured bounds into the private fixture's layer table. Original
    # assignment is kept fixed while expanding caps; no source spec is changed.
    original_layers = manifest.LAYERS
    working_layers = list(original_layers)
    if low_cover_pilot:
        working_layers[3] = replace(working_layers[3],density_per_mille=600)
    if tall_canopy_pilot:
        # Ordinary tall canopy must not inherit the legacy hero-only candidate
        # rate. Hero rarity remains in species/stand policy. Private pilot only:
        # the expanded tall-tree population needs runtime cost/visual review.
        working_layers[0] = replace(working_layers[0],cell_mm=8000,density_per_mille=550)
    original_assign = manifest.assign_layer
    original_terrain = manifest.is_terrain_lattice
    assignments = {}
    required = [[l.max_height_mm,l.max_depth_mm,l.max_radius_mm] for l in working_layers]
    for name, body in bodies.items():
        report = manifest.ExportReport()
        index = (manifest.assign_layer(body['kind'],manifest.nominal_height_m(body,body['kind']),report,name,float(specs.get(body,'placement.spacing_m')))
                 if body['kind'] in ('tree','rock') else 3)
        # The reviewed large specimens can exceed legacy layer caps. File on
        # the coarse terrain layer and expand its bounds from actual grids.
        if index < 0 and body['kind'] in ('tree','rock'):
            index = 0
        if index<0 or index>=len(required):raise ValueError(f'Unplaceable preview species: {name}')
        assignments[name] = index
        for row in inventory_rows:
            if row['species']!=name:continue
            g=row['geometry']
            required[index]=[max(required[index][0],g['above_anchor_mm'],g['height_mm']),
                             max(required[index][1],g['below_anchor_mm']),max(required[index][2],g['bounds_radius_mm'])]
        body['height_m']=max(row['geometry']['height_mm'] for row in inventory_rows if row['species']==name)/1000
        body['resolution_cm']=str(next(row['geometry']['voxel_pitch_um'] for row in inventory_rows if row['species']==name)/10000)
    try:
        manifest.LAYERS=tuple(replace(layer,max_height_mm=required[i][0],max_depth_mm=required[i][1],max_radius_mm=required[i][2]) for i,layer in enumerate(working_layers))
        manifest.assign_layer = lambda kind, height, report, name, spacing: assignments[name]
        manifest.is_terrain_lattice = lambda kind, height, name: kind in ('tree','rock')
        blob = manifest.encode(list(bodies.items()), {name:len(selected_seeds[name]) for name in names})
        (output/'species.vxm').write_bytes(blob)
    finally:
        manifest.LAYERS=original_layers
        manifest.assign_layer=original_assign
        manifest.is_terrain_lattice=original_terrain
    for path, expected in protected.items():
        if digest(Path(path).read_bytes()) != expected:
            raise ValueError(f'Source changed during preview staging: {path}')
    report={'preview_only':True, 'species':names, 'seeds':seeds, 'seeds_by_species':selected_seeds, 'variants':len(entries),
            'lattice_profile':'temperate-tall-canopy-pilot-v1' if tall_canopy_pilot else 'legacy-lattice',
            'understory_profile':'low-cover-pilot-v1' if low_cover_pilot else 'authored-understory',
            'favored_low_cover_species':favored_cover,
            'source_files_unchanged':True, 'source_decisions':{mid:meta.get('review_status') for _,mid,_,_,meta,_,_ in entries},
            'purpose':'Private runtime placement test whitelist; no library endorsement or real publication change.',
            'appearance_bytes':sum(p.stat().st_size for p in (output/'appearance').glob('*.vac'))}
    write(output/'PREVIEW_ONLY.json', report)
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--species', nargs='+', default=['temperate-oak','birch','european-beech','common-bluebell','male-fern','bramble-thicket','meadow-grass','water-reed'])
    parser.add_argument('--seeds', nargs='+', type=int, default=[7,12])
    parser.add_argument('--reuse-packets-from', type=Path)
    parser.add_argument('--seed-overrides', type=Path, help='JSON species-to-seed-list overrides; all source review gates still apply')
    parser.add_argument('--tall-canopy-pilot',action='store_true',help='Private density experiment for the tall-tree layer; requires runtime review')
    parser.add_argument('--low-cover-pilot',action='store_true',help='Private denser low-grass experiment; requires coverage and runtime review')
    args=parser.parse_args()
    overrides=json.loads(args.seed_overrides.read_text()) if args.seed_overrides else None
    print(json.dumps(stage(args.out,args.species,args.seeds,args.reuse_packets_from,overrides,args.tall_canopy_pilot,args.low_cover_pilot)))
