"""Map screened foundation candidates; not support/cave/build approval."""
import argparse
import csv
import json
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont


def render(root):
    report=json.loads((root/'plot-analysis.json').read_text())
    with (root/'terrain-samples.csv').open(encoding='utf-8-sig') as f:terrain=list(csv.DictReader(f))
    x0=min(int(r['x_mm']) for r in terrain)-4000;y0=min(int(r['y_mm']) for r in terrain)-4000
    points=report['tree_clear_candidates']
    if not all(p.get('footprint_water_checked') is True for p in points):
        raise ValueError('Full footprint water checks required for this map')
    canvas=Image.new('RGB',(700,700),'#f7f5ed');draw=ImageDraw.Draw(canvas)
    font=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',17)
    small=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',14)
    draw.text((28,16),'Actual terrain: screened 5 x 5 m openings',font=font,fill='#25332c')
    draw.rectangle((28,58,540,570),fill='#dfd8be')
    def xy(x,y):return 28+(x-x0)/500,570-(y-y0)/500
    with (root/'terrain-placement.csv').open(encoding='utf-8-sig') as f:
        for r in csv.DictReader(f):
            if not int(r['terrain_lattice']):continue
            x,y=xy(int(r['x_mm']),int(r['y_mm']))
            if 28<=x<540 and 58<y<=570:draw.ellipse((x-1,y-1,x+1,y+1),fill='#394b35')
    for p in points:
        x,y=xy(p['x_mm'],p['y_mm'])
        draw.rectangle((x-5,y-5,x+5,y+5),outline='#328561',width=1)
    draw.text((557,62),'N ↑',font=font,fill='#25332c')
    draw.text((28,585),f'{len(points)} candidates across 256 x 256 m; dark dots are tree anchors.',font=small,fill='#25332c')
    draw.text((28,608),'Green: dry, <=0.5 m relief, clear of tree voxels in a 5 x 5 x 3 m volume.',font=small,fill='#25332c')
    draw.text((28,631),'Does not establish foundation support, cave safety or player access.',font=small,fill='#25332c')
    draw.text((28,654),'Squares are checks on an 8 m survey grid, not generated building pads.',font=small,fill='#25332c')
    target=root/'plot-candidates.png';canvas.save(target)
    return target


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path)
    print(render(p.parse_args().capture))
