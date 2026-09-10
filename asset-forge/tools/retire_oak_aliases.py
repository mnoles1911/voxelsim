"""Retire superseded oak pilot/family collections after canonical replacement."""
import json
import shutil
import time
import _path
from temperate_collection import ROOT, STAGE, contained
from forge import inventory

def main():
    record=json.loads((ROOT/'library/temperate-oak/species.json').read_text())
    assert record['generator_approved'] and record['generator_digest']==inventory.generator_digest()
    assert len(record['variants'])==36
    for row in record['variants']:
        path=ROOT/'out/forge-candidates/temperate-oak'/row['id']/'tree.vxa'
        if not path.exists():path=ROOT/'library/temperate-oak'/row['id']/'tree.vxa'
        assert inventory.digest(path)==row['artifact_hash']
    ledger_path=STAGE/'oak-alias-retirement.json'
    if ledger_path.exists():
        print('Oak alias retirement already recorded');return
    ledger={'replacement':'temperate-oak','timestamp':time.time(),'aliases':[]}
    targets=[]
    for name in ('temperate-oak-family','temperate-oak-pilot'):
        specpath=ROOT/'specs'/f'{name}.json'
        assets=[]
        for parent in (ROOT/'library',ROOT/'out/forge-candidates'):
            assets.extend(json.loads(p.read_text())['id'] for p in (parent/name).glob('*/meta.json'))
        ledger['aliases'].append({'species':name,'retired_ids':assets,
            'old_spec':json.loads(specpath.read_text()) if specpath.exists() else None})
        targets.append(contained(specpath,ROOT/'specs'))
        for parent in (ROOT/'library',ROOT/'out/forge-candidates',ROOT/'out/engine/banks'):
            targets.append(contained(parent/name,parent))
    inventory.write_json(ledger_path,ledger)
    for target in targets:
        if target.is_dir():shutil.rmtree(target)
        elif target.exists():target.unlink()
    print('Retired oak aliases:',sum(len(x['retired_ids']) for x in ledger['aliases']),'assets')

if __name__=='__main__':main()
