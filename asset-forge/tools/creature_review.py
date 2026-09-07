"""Build a searchable offline review gallery and contact sheets from exports."""
import html
import json
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out' / 'creature-refresh'


def main():
    review = OUT / 'review'
    review.mkdir(parents=True, exist_ok=True)
    rows = []
    for path in sorted((ROOT / 'library').glob('*/*/meta.json')):
        meta = json.loads(path.read_text())
        if meta.get('category') != 'creature':
            continue
        body = json.loads((path.parent / 'spec.json').read_text())
        rows.append(dict(name=meta['species'], kind='goblin' if body.get('goblin', {}).get('role', 'none') != 'none' else meta['kind'],
                         seed=meta['seed'], pitch=float(body['resolution_cm'])*10,
                         directory=path.parent, voxels=meta['stats']['voxels']))
    cards = []
    for row in rows:
        link = '../../' + row['directory'].relative_to(ROOT).as_posix()
        title = html.escape(row['name'].replace('-', ' ').title())
        cards.append(f'''<article data-kind="{row['kind']}" data-name="{html.escape(row['name'])}" data-pitch="{row['pitch']}">
<a href="{link}/thumb.png"><img loading="lazy" src="{link}/thumb.png" alt="{title}"></a>
<h2>{title}</h2><p>{row['kind'].title()} · {row['pitch']:g} mm · seed {row['seed']}</p>
<footer><a href="{link}/tree.vox" download>VOX model</a><a href="{link}/tree.vxa" download>Game asset</a></footer></article>''')
    document = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Creature model review</title><style>
*{box-sizing:border-box}body{margin:0;background:#15191e;color:#eef1f4;font:16px system-ui,sans-serif;padding:32px}
header{max-width:1000px;margin-bottom:28px}h1{font-size:36px;margin:0 0 12px}header p{color:#bbc5ce;line-height:1.6}
nav{display:flex;flex-wrap:wrap;gap:12px;margin:24px 0}input,select{font:inherit;padding:12px;border:1px solid #62707e;border-radius:7px;background:#252c34;color:inherit}
#models{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:18px}article{border:1px solid #3e4853;border-radius:10px;overflow:hidden;background:#252c34}
article img{width:100%;height:220px;object-fit:contain;background:#6c747c}h2{font-size:18px;margin:16px 16px 6px}article p{margin:0 16px 16px;color:#bac6d1;font-size:14px}
footer{display:flex;gap:20px;padding:0 16px 20px}a{color:#8dd4ef}article[hidden]{display:none}#count{color:#a7b9c7}
</style><header><h1>Creature model review</h1><p>Refreshed wildlife and five evil goblin mobs. Models use 12.5 mm voxels where their size allows; the largest animals retain a coarser tier. Select a model image for a closer look.</p>
<nav><input id="search" type="search" placeholder="Find a creature" aria-label="Find a creature">
<select id="kind" aria-label="Creature family"><option value="">All families</option><option>goblin</option><option>quadruped</option><option>bird</option><option>fish</option><option>cetacean</option></select>
<select id="pitch" aria-label="Voxel size"><option value="">All voxel sizes</option><option value="12.5">12.5 mm</option><option value="25">25 mm</option><option value="50">50 mm</option><option value="100">100 mm</option></select></nav><p id="count"></p></header><main id="models">'''
    document += '\n'.join(cards) + '''</main><script>
const search=document.querySelector('#search'),kind=document.querySelector('#kind'),pitch=document.querySelector('#pitch');
function filter(){let count=0;for(const card of document.querySelectorAll('article')){card.hidden=!(card.dataset.name.includes(search.value.toLowerCase().trim().replaceAll(' ','-'))&&(!kind.value||card.dataset.kind===kind.value)&&(!pitch.value||Number(card.dataset.pitch)===Number(pitch.value)));if(!card.hidden)count++;}document.querySelector('#count').textContent=`${count} models`;}
for(const input of [search,kind,pitch])input.addEventListener('input',filter);filter();</script></html>'''
    (OUT / 'index.html').write_text(document, encoding='utf-8')
    for kind in ('bird', 'quadruped', 'fish', 'cetacean', 'goblin'):
        group = [r for r in rows if r['kind'] == kind]
        for page, start in enumerate(range(0, len(group), 30), 1):
            batch = group[start:start+30]
            canvas = Image.new('RGB', (1300, 230*((len(batch)+4)//5)), (32, 35, 40))
            draw = ImageDraw.Draw(canvas)
            for i, row in enumerate(batch):
                thumb = Image.open(row['directory']/'thumb.png')
                thumb.thumbnail((244, 186))
                x, y = i % 5 * 260, i // 5 * 230
                canvas.paste(thumb, (x+(260-thumb.width)//2, y+28))
                draw.text((x+8, y+5), row['name'], fill='white')
                draw.text((x+8, y+213), f"{row['pitch']:g} mm", fill='#aab7c0')
            canvas.save(review/f'{kind}-{page}.jpg')
    print(f'{len(rows)} models: {OUT / "index.html"}')


if __name__ == '__main__':
    main()
