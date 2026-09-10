"""Additional conservation references. Metadata only; source photos stay remote."""
from html.parser import HTMLParser
from urllib.parse import urljoin
from pathlib import Path
import urllib.request,json,concurrent.futures
SOURCES={
'bilberry-mat':('https://www.wildlifetrusts.org/wildlife-explorer/trees-and-shrubs/bilberry','Ben Osbourne'),
'bramble-thicket':('https://www.wildlifetrusts.org/wildlife-explorer/wildflowers/bramble','Philip Precey; see source'),
'common-dog-violet':('https://www.wildlifetrusts.org/wildlife-explorer/wildflowers/common-dog-violet','Philip Precey'),
'dogs-mercury':('https://www.wildlifetrusts.org/wildlife-explorer/wildflowers/dogs-mercury','Philip Precey'),
'ramsons':('https://www.wildlifetrusts.org/wildlife-explorer/wildflowers/wild-garlic','Ross Hoddinott/2020VISION'),
'harts-tongue-fern':('https://www.wildlifetrusts.org/wildlife-explorer/ferns-and-horsetails/harts-tongue-fern','Paul Lane; see source'),
'white-water-lily':('https://www.wildlifetrusts.org/wildlife-explorer/wildflowers/white-water-lily','Philip Precey'),
'yellow-water-lily':('https://www.wildlifetrusts.org/wildlife-explorer/wildflowers/yellow-water-lily','The Wildlife Trusts; see original credit'),
'moss-cushion':('https://www.britishbryologicalsociety.org.uk/learning/species-finder/leucobryum-glaucum/','British Bryological Society; see original image credit'),
'hair-cap-moss':('https://www.britishbryologicalsociety.org.uk/learning/species-finder/polytrichum-commune/','British Bryological Society; see original image credit'),
}
class Parser(HTMLParser):
 def __init__(self):super().__init__();self.og=[];self.images=[]
 def handle_starttag(self,t,attrs):
  a=dict(attrs)
  if t=='meta' and a.get('property')=='og:image':self.og.append(a['content'])
  if t=='img' and a.get('src') and '/uploads/' in a['src']:self.images.append((a['src'],a.get('alt','')))
def collect(r):
 if r['species'] not in SOURCES:return r
 url,credit=SOURCES[r['species']]
 try:
  parser=Parser();parser.feed(urllib.request.urlopen(url,timeout=25).read().decode())
  urls=[(u,r['taxon']) for u in parser.og] if parser.og else [(u,c) for u,c in parser.images if r['taxon'].split()[0].lower() in (u+' '+c).lower()]
  if urls:
   r.update(source=url,photos=[dict(url=urljoin(url,u),caption=c,credit=credit,license='See original source',image_id=u) for u,c in urls[:3]]);r.pop('error',None)
 except Exception as e:r['additional_source_error']=str(e)
 return r
if __name__=='__main__':
 p=Path('out/understory-review/references.json');rows=json.loads(p.read_text())
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:rows=list(pool.map(collect,rows))
 p.write_text(json.dumps(rows,indent=2));print('Photo references',sum(bool(r['photos']) for r in rows));print('Missing',[r['species'] for r in rows if not r['photos']])
