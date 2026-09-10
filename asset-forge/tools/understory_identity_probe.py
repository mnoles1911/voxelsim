"""The selected recipe's identity tracks shared code, not unrelated morphology."""
import _path
from forge import inventory
from forge.understory_profiles import PROFILES
from pathlib import Path
import json
raw=Path('forge/understory.py').read_bytes();changed=raw.replace(b"p.get('frond_open_min',.32)",b"p.get('frond_open_min',.38)");assert changed!=raw
rose={'plant_recipe':{'profile':'primrose','generator':'temperate-understory-v1'}};fern={'plant_recipe':{'profile':'male-fern','generator':'temperate-understory-v1'}}
f=inventory._understory_dependency_bytes
assert f('understory.py',raw,rose)==f('understory.py',changed,rose)
assert f('understory.py',raw,fern)!=f('understory.py',changed,fern)
shared=raw.replace(b'def unit(v):',b'def unit(v, sentinel=None):');assert f('understory.py',raw,rose)!=f('understory.py',shared,rose)
before=f('understory_profiles.py',b'',rose);old=PROFILES['male-fern']['taxon'];PROFILES['male-fern']['taxon']='fixture taxon'
try:assert f('understory_profiles.py',b'',rose)==before
finally:PROFILES['male-fern']['taxon']=old
assert inventory.generator_digest()==json.load(open('out/understory-review/legacy-generator.json'))['digest']
print('PASS: unrelated fern branch/profile edits preserve primrose identity; relevant/shared edits invalidate; legacy tree identity unchanged')
