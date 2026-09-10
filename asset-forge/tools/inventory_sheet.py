"""Review every saved oak candidate, plus nine representatives at player scale."""
import json
from pathlib import Path
from PIL import Image,ImageDraw
import oak_pilot
from forge import vxa

root=oak_pilot.ROOT
out=root/'out/oak-inventory';out.mkdir(exist_ok=True)
entries=sorted((root/'out/forge-candidates/temperate-oak').glob('*/meta.json'))
canvas=Image.new('RGB',(6*280,6*300),(25,29,29))
for i,p in enumerate(entries):
    meta=json.loads(p.read_text());s=meta['stats']
    image=Image.open(p.parent/'thumb.png').convert('RGB');image.thumbnail((266,240))
    x,y=(i%6)*280,(i//6)*300
    canvas.paste(image,(x+(280-image.width)//2,y+12+(240-image.height)//2))
    d=ImageDraw.Draw(canvas)
    d.text((x+10,y+255),f"Seed {meta['seed']:02d} | {s['size_class']} / {s['growth_form']}",fill=(219,210,177))
    d.text((x+10,y+274),f"{s['height_m']:.1f} m | {s['voxels']:,} voxels",fill=(161,176,163))
canvas.save(out/'36-oaks.png')
scale=Image.new('RGB',(3*480,3*460),(224,230,224))
for i,p in enumerate(entries[:9]):
    meta=json.loads(p.read_text());s=meta['stats']
    tile=oak_pilot.tile(vxa.read(p.parent/'tree.vxa'),pixels_per_m=40)
    tile=tile.resize((480,460),Image.Resampling.LANCZOS)
    x,y=(i%3)*480,(i//3)*460
    scale.paste(tile,(x,y));d=ImageDraw.Draw(scale)
    d.rectangle((x,y,x+480,y+30),fill=(224,230,224))
    d.text((x+12,y+10),f"{s['size_class']} / {s['growth_form']} | {s['height_m']:.1f} m | seed {meta['seed']}",fill=(25,40,27))
scale.save(out/'size-and-form.png')
print(out)
