from pathlib import Path
import json
root=Path('D:/voxelsim')
guide=root/'docs/crafting-system-design-guide.md'
old=root/'docs/crafting-progression-2026-09-06.md'
guide.write_text(old.read_text(encoding='utf-8').replace('# Craftable tools and compounding technology','# Crafting system design guide',1),encoding='utf-8')
old.write_text('# Crafting progression proposal\n\nThe maintained design guide is [Crafting system design guide](crafting-system-design-guide.md).\n\nOriginal proposal: 6 September 2026.\n',encoding='utf-8')
p=root/'asset-forge/docs/craftable-modeling-roadmap.md'
p.write_text(p.read_text(encoding='utf-8').replace('crafting-progression-2026-09-06.md','crafting-system-design-guide.md'),encoding='utf-8')
p=root/'asset-forge/specs/bushcraft-wooden-haft.json'
d=json.loads(p.read_text());d['artifact'].update(length_m=.725,beam_m=.30,depth_m=.125)
p.write_text(json.dumps(d,indent=2,sort_keys=True)+'\n',encoding='utf-8')
print('Saved canonical crafting guide and aligned the component haft with the assembled axe.')
