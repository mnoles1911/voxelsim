from pathlib import Path
import json,html,shutil
import _path
from forge.understory_profiles import PROFILES
from forge import inventory,species_registry
ROOT=Path(__file__).resolve().parents[1]
rows=json.loads((ROOT/'out/understory-review/references.json').read_text())
root=Path('web/public/understory-review');root.mkdir(parents=True,exist_ok=True)
approved={n for n in PROFILES if (ROOT/'library'/n/'species.json').exists() and species_registry.read(n).get('generator_approved')}
for r in rows:
 n=r['species'];display=PROFILES[n].get('display_name',n);images=''.join('<figure><img src="'+html.escape(p['url'],quote=True)+'"><figcaption>'+html.escape(' '.join(p['caption'].split()[:7])+' - '+p['credit'])+'</figcaption></figure>' for p in r['photos'][:3])
 accepted=n in approved
 status='Accepted source - user variant endorsement remains separate' if accepted else 'Unaccepted research pilot'
 pilots=''
 for seed in (1,4,7):
  staged=Path('out/understory-collection/variants')/n/f'{n}-{seed:04d}'/'thumb.png'
  candidates=Path('out/forge-candidates')/n/f'{n}-{seed:04d}'/'thumb.png'
  endorsed=Path('library')/n/f'{n}-{seed:04d}'/'thumb.png'
  p=next((q for q in ((endorsed,candidates,staged) if accepted else (staged,candidates,endorsed)) if q.exists()),Path('out/understory-review')/n/f'pilot-{seed}.png')
  if p.exists():shutil.copyfile(p,root/f'{n}-{seed}.png');pilots+=f'<figure><img src="{n}-{seed}.png"><figcaption>Seed {seed} - 25 mm</figcaption></figure>'
 batch=Path('out/understory-collection')/f'{n}-36.png'
 if batch.exists():shutil.copy2(batch,root/batch.name)
 batch_link=f'<p><a href="{n}-36.png">Review 36-variant contact sheet</a> - <a href="index.html">All profiles</a></p>' if batch.exists() else ''
 body=f'<!doctype html><meta charset="utf-8"><title>{n} understory review</title><style>body{{background:#181b20;color:#ddd;font:16px system-ui}}section{{display:flex}}figure{{flex:1;margin:10px}}img{{width:100%;height:270px;object-fit:contain}}a{{color:#acb}}h1{{font-size:24px}}</style><h1>{html.escape(display)} - {r["taxon"]} - {r["architecture"]}</h1><p>{status} - spring, 25 mm - <a href="{r["source"]}">Original source and photo credits</a></p><section>{images}</section><section>{pilots}</section>{batch_link}'
 (root/f'{n}.html').write_text(body,encoding='utf8')
links=''.join(f'<tr><td><a href="{r["species"]}.html">{r["species"]}</a></td><td>{r["taxon"] or "Unresolved"}</td><td>{r["architecture"]}</td><td>{len(r["photos"])} reference photos</td><td>{"Accepted source; seeds pending endorsement" if r["species"] in approved else "Unaccepted research"}</td></tr>' for r in rows)
(root/'index.html').write_text('<!doctype html><meta charset="utf-8"><title>Temperate understory research</title><style>body{background:#181b20;color:#ddd;font:16px system-ui;padding:25px}a{color:#acd}td{padding:8px 16px;border-bottom:1px solid #444}</style><h1>Temperate understory research</h1><p>113 scoped profiles. Rows distinguish accepted sources from offline research pilots. Source acceptance does not endorse game variants. Spring - 25 mm cubic voxels. Remote photographs remain at their original hosts.</p><table>'+links+'</table>',encoding='utf8')
# The running server serves web/dist/static paths; source copies persist builds.
dest=Path('web/dist/understory-review');shutil.copytree(root,dest,dirs_exist_ok=True)
