"""Build a linked botanical-photo review catalog; never download source images.

Photo URLs and attribution are extracted from the source page. The browser loads
the original images. Re-run to refresh expiring source URLs. A catalog entry is
not a completed review: assessments live independently in the species library.
"""
import concurrent.futures
import html
from html.parser import HTMLParser
import json
import re
from pathlib import Path
import shutil
import urllib.request
from urllib.parse import urljoin
import _path
from forge.forest_profiles import PROFILES,source,ART_BASELINE
from forge import inventory

ROOT=Path(__file__).resolve().parents[1]
SELECTED = {
    'european-beech': ['/site/assets/files/5501/fagus-sylvatica-11.jpg','/site/assets/files/5501/fagus-sylvatica-14.jpg'],
    'crab-apple': ['/site/assets/files/6302/malus-sylvestris-5.jpg','/site/assets/files/6302/malus-sylvestris-3.jpg'],
    'holm-oak': ['/site/assets/files/7071/quercus-ilex-11.jpg','/site/assets/files/7071/quercus-ilex-07.jpg'],
    'hawthorn-scrub': ['/site/assets/files/5094/crataegus-monogyna-6.jpg','/site/assets/files/5094/crataegus-monogyna-4.jpg'],
    'flowering-dogwood': ['18690','10079'],
    'quaking-aspen': ['31379','31381'],
}
class Photos(HTMLParser):
    def __init__(self):super().__init__();self.photos=[]
    def handle_starttag(self,tag,attrs):
        a=dict(attrs)
        if tag=='img' and 'modal_img' in a.get('class',''):
            self.photos.append(dict(url=a['src'],caption=a.get('data-caption',a.get('alt','')),
                credit=a.get('data-attrib',''),license=a.get('data-license',''),image_id=a.get('data-image-id')))
        if tag=='a' and a.get('data-caption') and '/site/assets/files/' in a.get('href',''):
            self.photos.append(dict(url=urljoin('https://www.treesandshrubsonline.org',a['href']),caption=re.sub('<[^>]+>','',a['data-caption']),credit=('Trees and Shrubs Online · '+re.sub('<[^>]+>','',a['data-caption']).rsplit('Image ',1)[-1]) if 'Image ' in a['data-caption'] else 'Trees and Shrubs Online; see original photo credit',license='',image_id=a['href']))

def collect(item):
    name,p=item
    # NCSU has consistently attributed species galleries, including taxa whose
    # morphology notes use a different botanical source.
    taxon='cupressus-macrocarpa' if p.taxon=='hesperocyparis-macrocarpa' else p.taxon
    url=f'https://plants.ces.ncsu.edu/plants/{taxon}/'
    if name in ('european-beech','japanese-maple','hornbeam','common-ash','hawthorn-scrub','holm-oak','tree-fern','crab-apple','wild-pear'):
        url=f'https://www.treesandshrubsonline.org/articles/{p.taxon.split("-")[0]}/{p.taxon}/'
    out=dict(species=name,taxon=p.taxon,source=source(p),photo_page=url,photos=[],cue=p.cue,habitat=p.habitat)
    try:
        req=urllib.request.Request(url,headers={'User-Agent':'AssetForgeReferenceReview/1.0'})
        parser=Photos();parser.feed(urllib.request.urlopen(req,timeout=25).read().decode())
        def score(photo):
            cap=photo['caption'].lower()
            return (5 if any(x in cap for x in ('form','habit','whole tree','mature tree','winter silhouette','crown','specimen')) else 0)+(2 if any(x in cap for x in ('branch','trunk','bark')) else 0)-(10 if any(x in cap for x in ('illustration','flower','fruit','cone','seed','bonsai','cultivar','purple fountain','fastigiata','pendula','monumentale')) or "'" in cap or '‘' in cap else 0)
        unique={p['image_id']:p for p in parser.photos}
        out['photos']=sorted(unique.values(),key=score,reverse=True)
        chosen=SELECTED.get(name,[])
        out['photos'].sort(key=lambda p: chosen.index(str(p['image_id'])) if str(p['image_id']) in chosen else len(chosen))
        out['photo_status']='available' if out['photos'] else 'needs alternate photo source'
    except Exception as exc:out['photo_status']='needs alternate photo source';out['error']=str(exc)
    if name=='wild-pear':
        out.update(photo_status='available',photo_page='https://bdc.univie.ac.at/fileadmin/user_upload/i_bdc/dashboard/Pyruspyraster.html',source='https://www.rhs.org.uk/plants/69543/pyrus-pyraster/details',photos=[dict(url='https://bdc.univie.ac.at/fileadmin/user_upload/i_bdc/dashboard/Pyrus_pyraster.jpg',caption='Wild pear tree; species profile',credit='University of Vienna Biodiversity Center',license='',image_id='vienna-pyrus-pyraster'),dict(url='https://files.ibot.cas.cz/cevs/images/taxa/large/Pyrus_pyraster11.jpg',caption='Wild pear in flower',credit='Pladias database of Czech flora and vegetation; see original source for photographer',license='',image_id='pladias-pyraster11')])
    if name=='white-poplar':
        out.update(photo_status='available',photo_page='https://landscapeplants.oregonstate.edu/plants/populus-alba',photos=[dict(url='https://landscapeplants.oregonstate.edu/sites/plantid7/files/plantimage/'+file,caption=caption,credit='Oregon State University Landscape Plants',license='',image_id=file) for file,caption in [('poalba910.jpg','Whole tree habit'),('poalba994.jpg','Whole tree habit'),('poalba72.jpg','Leaves'),('poalba73.jpg','Silvery leaf undersides')]])
    return out

def main():
    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:catalog=list(pool.map(collect,PROFILES.items()))
    static=ROOT/'web/public/tree-reference-pilots';static.mkdir(parents=True,exist_ok=True)
    for entry in catalog:
        name=entry['species'];path=ROOT/'library'/name/'reference-review.json'
        record=json.loads(path.read_text()) if path.exists() else dict(species=name,status='awaiting photo comparison',reviewed_seeds=[],findings=[],reviewed_images=[])
        record.update(taxon=entry['taxon'],sources=list(dict.fromkeys([entry['source'],entry['photo_page']])),habitat=entry['habitat'],art_baseline=ART_BASELINE)
        inventory.write_json(path,record)
        entry['review']=record
        for file in (f'{view}-{seed}.png' for seed in (1,4,7) for view in ('pilot','wood','shaded')):
            pilot=ROOT/'out/temperate-review'/name/file
            if pilot.exists():shutil.copyfile(pilot,static/f'{name}-{file}')
    inventory.write_json(ROOT/'out/temperate-review/reference-catalog.json',catalog)
    payload=json.dumps(catalog).replace('</','<\\/')
    page=r'''<!doctype html><html><head><meta charset="utf-8"><title>Tree reference review · Asset Forge</title><style>
    *{box-sizing:border-box}body{margin:0;background:#1b211f;color:#e1e6dc;font:15px system-ui}header{padding:14px 22px;background:#28332d;display:flex;align-items:center;gap:18px}a{color:#c2dca4}button,select{background:#34483b;color:#f3ebd2;border:1px solid #687c60;padding:9px;font:inherit}main{padding:18px}h1{font-size:24px;margin:0}p{line-height:1.5}.grid{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:12px}.card{background:#28332d;padding:10px;border-radius:6px}.card img{width:100%;height:390px;object-fit:contain;background:#e2e8e2}.caption{font-size:12px;min-height:45px;line-height:1.4}.status{color:#e6c085}#findings{color:#edc8a0}small{color:#b9c2b2}</style></head><body>
    <header><a href="/?tab=forge">← Forge</a><b>Tree reference review</b><select id="species" aria-label="Review species"></select><button id="prev">Previous species</button><button id="next">Next species</button><span id="position"></span><select id="pilotseed" aria-label="Pilot size"><option value="1">Small · seed 1</option><option value="4">Medium · seed 4</option><option value="7" selected>Large · seed 7</option></select></header>
    <main><p>Art baseline: spring foliage, color and fresh growth. Other-season photographs inform structure only. Seasonal changes and recoloring are out of scope.</p><h1 id="title"></h1><p id="cue"></p><p class="status" id="status"></p><div class="grid" id="images"></div><p id="findings"></p><p id="sources"></p><small>Photographs remain hosted by their original publishers. A source link or completed voxel check does not constitute visual approval. Compare crown outline, major forks, twig direction, foliage attachment, negative space and player scale. Review small, medium and large seeds before accepting the generator.</small></main><script>
    const catalog=PAYLOAD;let index=Math.max(0,catalog.findIndex(s=>s.species===new URLSearchParams(location.search).get("species")));const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const select=document.getElementById('species');select.innerHTML=catalog.map((s,i)=>`<option value="${i}">${esc(s.species.replaceAll('-',' '))}</option>`).join('');
    function show(){const s=catalog[index];const seed=document.getElementById('pilotseed').value;select.value=index;document.getElementById('position').textContent=`${index+1} / ${catalog.length}`;document.getElementById('title').textContent=s.species.replaceAll('-',' ')+' · '+s.taxon.replaceAll('-',' ');document.getElementById('cue').textContent=s.cue+' Habitat role: '+s.habitat;document.getElementById('status').textContent=s.review.status+' · Photo source: '+s.photo_status;
    const photos=s.photos.filter(p=>!/illustration/i.test(p.caption)).slice(0,2);document.getElementById('images').innerHTML=photos.map(p=>`<div class="card"><img src="${esc(p.url)}" alt="${esc(s.species)} botanical reference photograph"><div class="caption">${esc(p.caption.split(/\s+/).slice(0,10).join(' '))}…<br>${esc(p.credit)} · <a href="${esc(s.photo_page)}" target="_blank" rel="noreferrer">Photo credit and license</a></div></div>`).join('')+[`pilot-${seed}.png`,`wood-${seed}.png`,`shaded-${seed}.png`].map((f,i)=>`<div class="card"><img src="/static/tree-reference-pilots/${s.species}-${f}" alt="${i?'Branch structure':'Voxel pilot with pawn'}"><div class="caption">Seed ${seed} · ${['foliage silhouette','branch structure','shaded detail'][i]} · 100 mm voxels<br>${i===2?'Shaded geometry':'Player pawn: 1.80 m'}</div></div>`).join('');document.getElementById('findings').textContent=s.review.findings.join(' ');document.getElementById('sources').innerHTML=`<a target="_blank" rel="noreferrer" href="${esc(s.photo_page)}">All source photographs</a> · <a target="_blank" rel="noreferrer" href="${esc(s.source)}">Botanical description</a>`;}
    document.getElementById('pilotseed').onchange=show;select.onchange=()=>{index=Number(select.value);show()};document.getElementById('next').onclick=()=>{index=(index+1)%catalog.length;show()};document.getElementById('prev').onclick=()=>{index=(index+catalog.length-1)%catalog.length;show()};show();</script></body></html>'''.replace('PAYLOAD',payload)
    (ROOT/'web/public/tree-reference-review.html').write_text(page,encoding='utf8')
    print(json.dumps({'species':len(catalog),'photo_galleries':sum(bool(s['photos']) for s in catalog),'missing':[s['species'] for s in catalog if not s['photos']]}))

if __name__=='__main__':main()
