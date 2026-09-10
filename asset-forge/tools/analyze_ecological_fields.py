"""Map native stand fields and measure area and transect frequencies.

This survey intentionally does not claim terrain habitat or walking clearance:
it measures authored fields before biome, slope, water and asset selection gates.
"""
import argparse
import csv
import json
from collections import Counter, deque
from pathlib import Path
from statistics import median

STANDS=('mixed','young','open','ancient','thicket')
COLORS=('#719f69','#afd374','#ecd994','#254f3d','#8c6073')


def analyze(root, render_png=False):
    samples={}
    with (root/'field-samples.csv').open(encoding='utf-8-sig',newline='') as stream:
        for row in csv.DictReader(stream):
            r={k:int(v) for k,v in row.items()}
            cells=samples.setdefault(r['world_seed'],{})
            key=(r['x_mm'],r['y_mm'])
            if key in cells:raise ValueError('Duplicate field sample')
            cells[key]=r
    report={'scope':'Native pre-habitat stand fields, 8m sampling; not actual tree density or traversability','seeds':[]}
    panels=[]
    if render_png:
        from PIL import Image,ImageDraw,ImageFont,ImageColor
        canvas=Image.new('RGB',(max(900,290*len(samples)+20),410),'#f7f5ed')
        draw=ImageDraw.Draw(canvas)
        font=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',14)
        draw.text((20,8),'Stand fields — each panel is 2.048 km square; north is up',font=font,fill='#25332c')
    for seed,cells in sorted(samples.items()):
        xs=sorted({x for x,_ in cells});ys=sorted({y for _,y in cells})
        assert len(cells)==len(xs)*len(ys) and len(xs)>1 and len(ys)>1
        assert all(b-a==8000 for a,b in zip(xs,xs[1:])) and all(b-a==8000 for a,b in zip(ys,ys[1:]))
        count=Counter(r['stand'] for r in cells.values())
        assert all(0<=s<len(STANDS) for s in count)
        features={}
        for stand in (3,4):
            remaining={key for key,r in cells.items() if r['stand']==stand};areas=[];complete=0
            while remaining:
                start=remaining.pop();queue=deque([start]);area=0;edge=False
                while queue:
                    x,y=queue.popleft();area+=64
                    edge|=x in (xs[0],xs[-1]) or y in (ys[0],ys[-1])
                    for nxt in ((x-8000,y),(x+8000,y),(x,y-8000),(x,y+8000)):
                        if nxt in remaining:remaining.remove(nxt);queue.append(nxt)
                areas.append(area)
                complete+=not edge
            entries=0;distance_km=0
            paths=[[(x,y) for x in xs] for y in ys[::16]]+[[ (x,y) for y in ys] for x in xs[::16]]
            for path in paths:
                previous=cells[path[0]]['stand']==stand
                for key in path[1:]:
                    current=cells[key]['stand']==stand
                    entries+=current and not previous;previous=current
                distance_km+=(len(path)-1)*.008
            features[STANDS[stand]]={'patches_intersecting_survey':len(areas),'complete_patches':complete,
                'median_sampled_patch_area_m2':median(areas) if areas else None,
                'transect_entries':entries,'transect_km':distance_km,'entries_per_km':entries/distance_km}
        report['seeds'].append({'world_seed':seed,'survey_area_km2':len(cells)*.000064,
            'stand_area_fractions':{name:count[i]/len(cells) for i,name in enumerate(STANDS)},
            'community_area_fractions':{str(i):n/len(cells) for i,n in sorted(Counter(r['community'] for r in cells.values()).items())},
            'features':features})
        pixels=[]
        for yi,y in enumerate(reversed(ys)):
            # Merge horizontal runs for a compact, exact categorical map.
            start=0;old=cells[(xs[0],y)]['stand']
            for xi in range(1,len(xs)+1):
                new=cells[(xs[xi],y)]['stand'] if xi<len(xs) else -1
                if new!=old:
                    pixels.append(f'<rect x="{start}" y="{yi}" width="{xi-start}" height="1" fill="{COLORS[old]}"/>')
                    start=xi;old=new
        panels.append(f'<svg x="{len(panels)*290+20}" y="65" width="256" height="256" viewBox="0 0 {len(xs)} {len(ys)}">'+''.join(pixels)+'</svg>'+f'<text x="{len(panels)*290+20}" y="48">World seed {seed}</text>')
        if render_png:
            panel=Image.new('RGB',(len(xs),len(ys)))
            panel.putdata([ImageColor.getrgb(COLORS[cells[(x,y)]['stand']]) for y in reversed(ys) for x in xs])
            left=(len(panels)-1)*290+20
            canvas.paste(panel.resize((256,256),Image.Resampling.NEAREST),(left,65))
            draw.text((left,34),f'World seed {seed}',font=font,fill='#25332c')
    width=max(900,290*len(panels)+20)
    legend=''.join(f'<rect x="{20+i*170}" y="345" width="14" height="14" fill="{color}"/><text x="{40+i*170}" y="357">{name}</text>' for i,(name,color) in enumerate(zip(STANDS,COLORS)))
    svg=f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="410"><rect width="100%" height="100%" fill="#f7f5ed"/><g font-family="sans-serif" font-size="14" fill="#25332c"><text x="20" y="22">Stand fields — each panel is 2.048 km square; north is up</text>'+''.join(panels)+legend+'<text x="20" y="390">Before terrain habitat gates. Colors describe stand intent, not guaranteed vegetation or walkability.</text></g></svg>'
    (root/'stand-fields.svg').write_text(svg)
    if render_png:
        for i,(name,color) in enumerate(zip(STANDS,COLORS)):
            draw.rectangle((20+i*170,345,34+i*170,359),fill=color)
            draw.text((40+i*170,343),name,font=font,fill='#25332c')
        draw.text((20,377),'Before terrain habitat gates. Colors describe stand intent, not guaranteed vegetation or walkability.',font=font,fill='#25332c')
        canvas.save(root/'stand-fields.png')
    (root/'field-analysis.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path);parser.add_argument('--png',action='store_true')
    args=parser.parse_args()
    print(json.dumps(analyze(args.directory,args.png),indent=2))
