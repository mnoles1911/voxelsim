"""Contract checks for inventory caching, candidates and recipe isolation."""
import _path
import json
import shutil
import subprocess
import sys
import tempfile
import types
from pathlib import Path
import numpy as np
from forge import inventory,manifest,pipeline,server,spec,vxa


def main():
    root=inventory.ROOT
    name='temperate-oak'
    body,_=spec.load(root/'specs'/f'{name}.json')
    entries=sorted(p for base in (root/'out/forge-candidates'/name,root/'library'/name) for p in base.glob('*/meta.json') if 1 <= json.loads(p.read_text())['seed'] <= 36)
    assert len(entries)==36
    kept=set(manifest.kept_seeds(root/'library',name))
    candidates={json.loads(p.read_text())['seed'] for p in entries if json.loads(p.read_text()).get('inventory_candidate')}
    assert not kept.intersection(candidates), 'Candidates must not publish'
    assert len({json.loads(p.read_text())['artifact_hashes']['tree.vxa'] for p in entries})==36
    baseline=vxa.read(entries[0].parent/'tree.vxa')
    seed=json.loads(entries[0].read_text())['seed']
    rebuilt=pipeline.build(body,seed)
    assert np.array_equal(baseline.data,rebuilt.grid.data)
    assert np.array_equal(baseline.origin,rebuilt.grid.origin)
    # Compare previous committed spec implementation against current for ALL
    # existing specs, so an opt-in field cannot reseed unrelated species.
    source=subprocess.check_output(['git','show','HEAD:asset-forge/forge/spec.py'],cwd=root.parent,text=True)
    previous=types.ModuleType('forge._previous_spec');previous.__package__='forge'
    sys.modules[previous.__name__]=previous
    exec(compile(source,'previous-spec.py','exec'),previous.__dict__)
    unchanged=0
    for p in (root/'specs').glob('*.json'):
        raw=json.loads(p.read_text())
        if 'plant_recipe' in raw:continue
        old,_=previous.validate(raw);new,_=spec.validate(raw)
        assert previous.spec_hash(old)==spec.spec_hash(new),p.name
        assert previous.seed_hash(old)==spec.seed_hash(new),p.name
        unchanged+=1
    try:spec.validate(dict(body,plant_recipe={'generator':'typo'}))
    except ValueError:pass
    else:raise AssertionError('Invalid recipes must not silently fall back')
    with tempfile.TemporaryDirectory(prefix='inventory-probe-',dir=root/'out') as tmp:
        temp=Path(tmp);lib=temp/'library';specs=temp/'specs';specs.mkdir()
        src=entries[0].parent;dest=lib/name/src.name
        shutil.copy2(root/'specs'/f'{name}.json',specs/f'{name}.json')
        task=(name,body,seed,inventory.generator_digest(),str(lib),'probe')
        assert inventory.build_one(task)['status']=='built'
        assert inventory.build_one(task)['status']=='cached'
        old_lib,old_specs=server.LIBRARY,server.SPECS
        server.LIBRARY,server.SPECS=lib,specs
        try:
            assert not manifest.kept_seeds(lib,name)
            inventory.promote(src.name)
            assert manifest.kept_seeds(lib,name)==[seed]
            assert json.loads((specs/f'{name}.json').read_text())['curation']['status']=='approved'
            # Byte changes are detected, never overwritten or auto-approved.
            with (dest/'tree.vxa').open('ab') as f:f.write(b'corruption')
            try:inventory.build_one(task)
            except ValueError:pass
            else:raise AssertionError('Corruption was accepted as cache')
        finally:server.LIBRARY,server.SPECS=old_lib,old_specs
    result=dict(distinct_oaks=36,deterministic_rebuild=True,unchanged_existing_spec_hashes=unchanged,
        candidate_publication_excluded=True,explicit_keep_fixture=True,corrupt_cache_refused=True)
    inventory.write_json(root/'out/inventory-validation.json',result)
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
