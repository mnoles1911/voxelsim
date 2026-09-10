"""Bind isolated runtime LOD cases to existing private VXA/VAC bytes. No publication."""
import argparse,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--library',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
if a.output.exists():raise SystemExit('Use a fresh output directory')
cases=[]
for species in ['meadow-grass','water-reed','meadow-daisy','bramble-thicket']:
 source=sorted((a.library/'banks'/species).glob('*.vxa'))[0]
 data=source.read_bytes();md5=hashlib.md5(data).hexdigest();appearance=a.library/'appearance'/(md5+'.vac')
 if not appearance.is_file():raise SystemExit('Missing bound appearance '+str(appearance))
 cases.append(dict(id=source.stem,vxa=str(source.resolve()),vac=str(appearance.resolve()),vxa_md5=md5,vxa_sha256=hashlib.sha256(data).hexdigest(),vac_sha256=hashlib.sha256(appearance.read_bytes()).hexdigest()))
a.output.mkdir(parents=True)
(a.output/'cases.json').write_text(json.dumps(dict(scope='Private diagnostic assets, no endorsement; actual runtime mesh LOD capture',cases=cases),indent=2))
print(a.output/'cases.json')
