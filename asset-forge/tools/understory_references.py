"""Collect attributed remote reference URLs. This never marks a review passed."""
import argparse,concurrent.futures,json,urllib.request,html
from pathlib import Path
import _path
from tree_references import Photos
from forge.understory_profiles import PROFILES
ROOT=Path(__file__).resolve().parents[1]
def collect(item):
 name,p=item;slug={'fireweed':'epilobium-angustifolium'}.get(name,p['taxon'].lower().replace(' ','-'));url='https://plants.ces.ncsu.edu/plants/'+slug+'/'
 result=dict(species=name,**p,source=url,photos=[],review_status='not reviewed')
 if not p['taxon']:return dict(result,error='Habitat and botanical identity unresolved')
 try:
  response=urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'AssetForge botanical reference catalog'}),timeout=25)
  if response.url.rstrip('/')!=url.rstrip('/'):raise ValueError('Species URL redirected to another taxon: '+response.url)
  parser=Photos();parser.feed(response.read().decode())
  def score(p):
   s=p['caption'].lower();return 5*any(x in s for x in ('habit','form','whole','plant','foliage'))-10*any(x in s for x in ('cultivar','fruit','seed','autumn','fall','winter',"'"))
  result['photos']=sorted({p['image_id']:p for p in parser.photos}.values(),key=score,reverse=True)[:6]
  if not result['photos']:result['error']='No source photographs extracted'
 except Exception as e:result['error']=str(e)
 return result
if __name__=='__main__':
 out=ROOT/'out/understory-review';out.mkdir(exist_ok=True,parents=True)
 ap=argparse.ArgumentParser();ap.add_argument('names',nargs='*');args=ap.parse_args()
 path=out/'references.json';existing={r['species']:r for r in json.loads(path.read_text())} if path.exists() else {}
 names=args.names or [n for n in PROFILES if not existing.get(n,{}).get('photos') or existing.get(n,{}).get('source','').startswith('https://plants.ces.ncsu.edu/')]
 with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:results=list(pool.map(collect,[(n,PROFILES[n]) for n in names]))
 for result in results:
  old=existing.get(result['species'],{})
  if result['photos']:
   old.update({k:v for k,v in result.items() if k!='review_status'});existing[result['species']]=old
  elif not old.get('photos'):existing[result['species']]=result
  else:old['refresh_error']=result.get('error','No photographs returned')
 rows=[existing[n] for n in PROFILES if n in existing]
 (out/'references.json').write_text(json.dumps(rows,indent=2))
 print('References',sum(bool(r['photos']) for r in rows),'of',len(rows));print('Missing',[r['species'] for r in rows if not r['photos']])
