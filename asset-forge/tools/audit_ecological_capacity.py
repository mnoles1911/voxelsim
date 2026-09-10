"""Report placement lattice capacity separately from accepted stem density.

Expected candidate counts precede species selection, habitat/stand gates and
tree spacing. They are not predicted final density or botanical targets.
"""
import argparse
import hashlib
import json
from pathlib import Path
import _path
from forge import manifest


def audit(path):
    blob=path.read_bytes();decoded=manifest.decode(blob)
    layers=[]
    for index,values in enumerate(decoded['layers']):
        cell,height,depth,radius,density,seeds,terrain=values
        trees=[s['name'] for s in decoded['species'] if s['layer']==index and s['kind']=='tree']
        if not trees:continue
        sites=10000/(cell/1000)**2
        layers.append(dict(layer=index,tree_species=trees,cell_m=cell/1000,
            maximum_model_height_m=height/1000,lattice_sites_per_hectare=sites,
            layer_candidate_probability=density/1000,
            expected_candidates_per_hectare_before_habitat=sites*density/1000))
    return dict(scope=__doc__,manifest_sha256=hashlib.sha256(blob).hexdigest(),layers=layers)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('manifest',type=Path);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();report=audit(a.manifest)
    a.output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
