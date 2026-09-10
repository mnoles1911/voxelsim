"""Fill missing catalog sources from Native Plant Trust without downloading media."""
from html.parser import HTMLParser
import concurrent.futures,json,urllib.request
from pathlib import Path
class Photos(HTMLParser):
 def __init__(self):super().__init__();self.photos=[]
 def handle_starttag(self,tag,attrs):
  a=dict(attrs)
  if tag=='a' and '/taxon-images-1000s1000/' in a.get('href',''):
   title=a.get('title','');pieces=title.split('~');self.photos.append(dict(url=a['href'],caption=pieces[0].strip(),credit=' · '.join(x.strip() for x in pieces[1:3]),license='See source credit; remotely hosted',image_id=a['href']))
def fill(row):
 if row['photos'] or not row['taxon']:return row
 url='https://gobotany.nativeplanttrust.org/species/'+row['taxon'].lower().replace(' ','/')+'/'
 try:
  parser=Photos();parser.feed(urllib.request.urlopen(url,timeout=20).read().decode())
  if parser.photos:
   row.update(source=url,photos=sorted(parser.photos,key=lambda p:'Plant form' not in p['caption'])[:6]);row.pop('error',None)
 except Exception as e:row['alternate_error']=str(e)
 return row
if __name__=='__main__':
 p=Path('out/understory-review/references.json');rows=json.loads(p.read_text())
 with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:rows=list(pool.map(fill,rows))
 p.write_text(json.dumps(rows,indent=2));print('Photo catalogs',sum(bool(r['photos']) for r in rows));print('Missing',[r['species'] for r in rows if not r['photos']])
