"""Stage the complete spring appearance collection without touching decisions.

Defaults to authoritative seed 7 per profile for a bounded visual review.
--all-seeds stages all 36 seeds. Source voxel hashes are checked by build().
"""
import argparse
import json
import time
import _path
import tree_appearance_pilot as pilot
from forge import inventory
from forge.forest_profiles import PROFILES

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--all-seeds',action='store_true')
    args=parser.parse_args()
    palette=pilot.ROOT/'rules/tree-appearance-spring-v2.json'
    catalog=json.loads(palette.read_text())
    assert set(catalog['profiles'])==set(PROFILES), 'Incomplete species appearance mapping'
    pilot.PROFILES={name:(p['bark'],p['foliage'],p['fresh_growth'],p['bark_pattern']) for name,p in catalog['profiles'].items()}
    pilot.OUT=pilot.ROOT/'out/tree-appearance-collection-v2'
    pilot.OUT.mkdir(parents=True,exist_ok=True)
    started=time.time();rows=[];errors=[]
    for name in pilot.PROFILES:
        for seed in (range(1,37) if args.all_seeds else (7,)):
            try:
                row=pilot.build(name,seed)
                row['needle']=PROFILES[name].architecture in ('conifer','hemlock','cedar','spruce','pine','cedar-wide','column','giant')
                rows.append(row)
            except Exception as exc:errors.append(dict(species=name,seed=seed,error=str(exc)))
        print('STAGED',name,'models',len(rows),'errors',len(errors),flush=True)
        inventory.write_json(pilot.OUT/'manifest.json',dict(status='staged; not installed',palette_sha256=inventory.digest(palette),models=rows,errors=errors,elapsed_seconds=round(time.time()-started,2)))
    if errors:raise RuntimeError(f'{len(errors)} appearance stages failed; inspect manifest')

if __name__=='__main__':main()
