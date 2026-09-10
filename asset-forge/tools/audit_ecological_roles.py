"""Describe inventory coverage, not habitat acceptance or final stand density.

Only positive community weights contribute. Size labels are relative to each
species; a large sapling is not proof that a community has a tall canopy.
"""
import argparse
import hashlib
import json
from pathlib import Path


def audit(config):
    communities=[]
    for index,community in enumerate(config['communities']):
        profiles=[p for p in config['profiles'] if p['community_weights_per_mille'][index]>0 and p['variants']]
        trees=[p for p in profiles if p['kind']=='tree']
        sizes={size:sorted(v['id'] for p in trees for v in p['variants'] if v.get('size_class')==size)
               for size in ('small','medium','large')}
        forms={form:sorted(v['id'] for p in trees for v in p['variants'] if v.get('growth_form')==form)
               for form in ('open','woodland','edge','compact','spreading','leaning')}
        cover={role:sorted(p['species'] for p in profiles if p['kind']!='tree' and p.get('cover_role')==role)
               for role in ('sun','shade','spring-woodland','shrub','wetland','shade-shrub','inert')}
        communities.append(dict(id=community['id'],tree_species=sorted(p['species'] for p in trees),
            tree_variants_by_size=sizes,tree_variants_by_form=forms,cover_species_by_role=cover,
            missing_tree_sizes=[k for k,v in sizes.items() if not v],
            missing_tree_forms=[k for k,v in forms.items() if not v],
            absent_cover_roles=[k for k,v in cover.items() if not v],
            tallest_available_tree_mm=max((v['height_mm'] for p in trees for v in p['variants']),default=None)))
    return dict(scope=__doc__,preview_only=config.get('preview_only',False),communities=communities,
        limitations=['Missing categories are coverage diagnostics, not a requirement that every habitat contain every role.',
                     'Positive community weights do not prove slope, water, stand or spacing acceptance.',
                     'This audit does not endorse assets or change runtime configuration.'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('configuration',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    blob=args.configuration.read_bytes()
    report=audit(json.loads(blob))
    report['configuration_sha256']=hashlib.sha256(blob).hexdigest()
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps([dict(id=c['id'],tree_species=len(c['tree_species']),missing_sizes=c['missing_tree_sizes'],
        absent_cover=c['absent_cover_roles']) for c in report['communities']]))
