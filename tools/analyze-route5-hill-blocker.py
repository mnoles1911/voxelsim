"""Reproduce hill route5 tree-body collision evidence (not terrain collision)."""
import csv, hashlib, json, math
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'asset-forge'/'tools'))
from analyze_ecological_plots import captured_bank_names, runtime_variant, rotated_grid
from forge import vxa
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
CAP=ROOT/'asset-forge/out/ecological-placement/world-capture-16-hill'
RUN=ROOT/'asset-forge/out/ecological-placement/route-capture-5-hill'
FIX=ROOT/'asset-forge/out/ecological-placement/previews/full-forest-low-cover-1'
OUT=ROOT/'asset-forge/out/ecological-placement/hill-blocker-analysis-1'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
run=json.loads((CAP/'run-manifest.json').read_text(encoding='utf-8-sig'))
assert sha(FIX/'placement.json').upper()==run['configurationSha256'].upper()
profiles={p['species']:p for p in json.loads((FIX/'placement.json').read_text())['profiles']}
names=captured_bank_names(FIX,run);grids=[];pins={}
for row in csv.DictReader((CAP/'terrain-placement.csv').open()):
 r={k:int(v) for k,v in row.items()}
 if not r['terrain_lattice']:continue
 variant=runtime_variant(sorted(profiles[names[r['bank_id']]]['variants'],key=lambda v:v['bank_file']),r['seed_slot'])
 # All grids capable of reaching the bounded8m neighborhood of target.
 if math.hypot(r['x_mm']/1000+154052,r['y_mm']/1000+81116)>variant['bounds_radius_mm']/1000+8:continue
 p=FIX/variant['bank_file'];assert sha(p)==variant['geometry_sha256'];pins[variant['id']]=sha(p)
 grid=vxa.decode(p.read_bytes())[0];assert round(grid.voxel_m*1000)==100
 a,o=rotated_grid(grid.data,grid.origin,r['yaw']);o=o+np.array([r['x_mm']//100,r['y_mm']//100,r['z_mm']//100])
 grids.append((variant['id'],r,a,o))
def hits(x,y,z):
 lo=np.floor((np.array([x,y,z])-[.3,.3,.9])*10).astype(np.int64)
 hi=np.floor((np.array([x,y,z])+[.3,.3,.9]-1e-6)*10).astype(np.int64)+1
 result=[]
 for name,r,a,o in grids:
  l=np.maximum(0,lo-o);h=np.minimum(a.shape,hi-o)
  if np.any(h<=l):continue
  sub=a[tuple(slice(int(i),int(j)) for i,j in zip(l,h))];loc=np.argwhere(sub!=0)
  if len(loc):result.append({'id':name,'anchor':r,'count':len(loc),'materials':np.unique(sub[sub!=0]).tolist(),'voxels':(loc+o+l).tolist()})
 return result
x,y,z=-154051.499,-81114.899,99.301
probes={name:{'center_m':[a,b,c],'tree_hits':hits(a,b,c)} for name,a,b,c in [('current',x,y,z),('west_2cm',x-.02,y,z),('south_2cm',x,y-.02,z),('raised_west_2cm',x-.02,y,z+.3),('raised_south_2cm',x,y-.02,z+.3),('original_target_center',-154052,-81116,z)]}
# Original target center is in foliage; its unchanged0.75m arrival disk can be
# approached from the west. Screen through a point0.70m west of it, not through
# the obstructed center. Runtime decides actual arrival, terrain and next leg.
points=[(-154048,-81112),(-154053,-81114),(-154053,-81116),(-154052.7,-81116)]
segments=[]
for a,b in zip(points,points[1:]):
 count=math.ceil(math.dist(a,b)/.025);blocked=[]
 for i in range(count+1):
  t=i/count;px=a[0]+(b[0]-a[0])*t;py=a[1]+(b[1]-a[1])*t
  for pz in (z,z+.3):
   h=hits(px,py,pz)
   if h:blocked.append({'center':[px,py,pz],'hits':h})
 segments.append({'from':a,'to':b,'spacing_max_m':.025,'pose_count':2*(count+1),'blocked':blocked})
report={'scope':'Exact captured rotated tree-grid intersections at recorded or assumed pawn height. Does not sample live terrain, caves, edits or prove connected traversal.','pins':{str(p.relative_to(ROOT)):sha(p) for p in [CAP/'run-manifest.json',CAP/'terrain-placement.csv',RUN/'run-manifest.json',RUN/'results/route-samples.csv',FIX/'placement.json',FIX/'species.vxm']},'geometry_pins':pins,'body_m':[.6,.6,1.8],'step_raise_m':.3,'probes':probes,'detour_sampled_segments':segments,'terrain_unknown':True,'conclusion':'Hawthorn broadleaf cells block negativeY and raised retry. West-only frozen motion requires actual terrain/material probe; no sole-cause claim.'}
OUT.mkdir(parents=True,exist_ok=True);(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print('probes',[(k,sum(h['count'] for h in p['tree_hits'])) for k,p in probes.items()]);print('detour blocked',[len(s['blocked']) for s in segments]);print('report',OUT/'report.json')
