"""Render actual runtime terrain/context exports, never synthetic terrain.

The stand panel is selection intent after terrain response; final anchor dots
are separate. Neither is a walkability or visible canopy measurement.
"""
import argparse
import csv
import json
from collections import Counter
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

STANDS = ['#709d65','#b0cf65','#efdc85','#234d39','#956881']


def slope_exposure(rows, anchors):
    """Diagnostic sampled area rates, not causal slope response or walkability.

    Slope is the native L1 rise/run, not an angle. Exact anchor facts supply
    counts; the 8m terrain grid estimates area and can miss narrow features.
    """
    def band(value):
        return ('invalid' if value < 0 else 'below_100' if value < 100 else
                '100_to_299' if value < 300 else '300_to_599' if value < 600 else '600_plus')
    xs=sorted({r['x_mm'] for r in rows});ys=sorted({r['y_mm'] for r in rows})
    if (len(rows)!=1024 or len(xs)!=32 or len(ys)!=32 or
        len({(r['x_mm'],r['y_mm']) for r in rows})!=1024 or
        any(b-a!=8000 for axis in (xs,ys) for a,b in zip(axis,axis[1:]))):
        raise ValueError('Expected complete native 8m terrain grid')
    area=Counter(band(r['slope_mm_per_m']) for r in rows if r['active']==1)
    counts={'trees':Counter(),'understory':Counter()}
    unknown=outside=0
    required={'x_mm','y_mm','facts_known','active','slope_mm_per_m','terrain_lattice'}
    for raw in anchors:
        if not required<=raw.keys():
            return {'available':False,'reason':'Capture lacks exact anchor slope facts'}
        r={k:int(raw[k]) for k in required}
        if not (xs[0]-4000<=r['x_mm']<xs[-1]+4000 and ys[0]-4000<=r['y_mm']<ys[-1]+4000):continue
        if not r['facts_known']:
            unknown+=1
            continue
        if r['active']!=1:
            outside+=1
            continue
        counts['trees' if r['terrain_lattice']==1 else 'understory'][band(r['slope_mm_per_m'])]+=1
    bands=[]
    for label in ('below_100','100_to_299','300_to_599','600_plus','invalid'):
        square_m=area[label]*64
        bands.append(dict(band=label,estimated_area_m2=square_m,
            **{kind:dict(anchors=values[label],estimated_per_hectare=(values[label]*10000/square_m
                if square_m and label!='invalid' else None)) for kind,values in counts.items()}))
    return dict(available=True,scope=slope_exposure.__doc__,bands=bands,
                unknown_anchor_facts=unknown,outside_ecology_anchors=outside)


def plot_relief_summary(low,high):
    low=np.asarray(low,dtype=np.int64);high=np.asarray(high,dtype=np.int64)
    valid=(low!=np.iinfo(np.int64).min)&(high!=np.iinfo(np.int64).max)&(high>=low)
    # Subtract only admitted bounds; declined sentinel subtraction overflows.
    relief=high[valid]-low[valid]
    return {'scope':'Conservative terrain-only 5m footprint relief; not tree/water/support clearance or build approval',
        'bounded_fraction':float(valid.mean()),'within_500mm_relief_bound_fraction':float((relief<=500).sum()/low.size),
        'median_relief_bound_mm':float(np.median(relief)) if relief.size else None}


def render(root):
    with (root/'terrain-samples.csv').open(encoding='utf-8-sig',newline='') as stream:
        rows=[{k:int(v) for k,v in row.items()} for row in csv.DictReader(stream)]
    xs=sorted({r['x_mm'] for r in rows}); ys=sorted({r['y_mm'] for r in rows},reverse=True)
    if len(xs)!=32 or len(ys)!=32 or len(rows)!=1024:
        raise ValueError('Expected complete native 32 by 32 survey')
    if any(b-a!=8000 for a,b in zip(xs,xs[1:])) or any(a-b!=8000 for a,b in zip(ys,ys[1:])):
        raise ValueError('Unexpected terrain survey spacing')
    keyed={(r['x_mm'],r['y_mm']):r for r in rows}
    if len(keyed)!=len(rows):raise ValueError('Duplicate terrain sample')
    grids={k:np.array([[keyed[x,y][k] for x in xs] for y in ys]) for k in rows[0]}
    elev=grids['surface_mm']/1000
    low,high=float(elev.min()),float(elev.max())
    height=(elev-low)/max(1.,high-low)
    elevation=np.stack([100+height*120,110+height*115,90+height*120],axis=-1).astype('uint8')
    slope=np.clip(grids['slope_mm_per_m']/1000,0,1)
    slope_rgb=np.stack([100+slope*155,190-slope*125,115-slope*70],axis=-1).astype('uint8')
    water=grids['distance_water_mm']; unknown=water==2147483647
    proximity=1-np.clip(water/80000,0,1)
    hydro=np.stack([220-proximity*180,215-proximity*70,185+proximity*60],axis=-1).astype('uint8')
    hydro[unknown]=[140,140,140];hydro[grids['water_mm']>0]=[30,95,190]
    intent=np.full((32,32,3),190,dtype='uint8')
    for index,color in enumerate(STANDS):
        intent[(grids['active']==1)&(grids['stand']==index)]=tuple(bytes.fromhex(color[1:]))
    font=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',16)
    small=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',13)
    canvas=Image.new('RGB',(1120,690),'#f7f5ed');draw=ImageDraw.Draw(canvas)
    draw.text((20,12),'Actual terrain and ecological placement — 256 m square, north up',font=font,fill='#25332c')
    panels=[('Elevation',elevation),('Slope (L1 rise/run)',slope_rgb),('Hydrography',hydro),('Terrain-conditioned stand intent',intent)]
    for i,(name,pixels) in enumerate(panels):
        left=20+i*275
        draw.text((left,43),name,font=small,fill='#25332c')
        panel=Image.fromarray(pixels).resize((256,256),Image.Resampling.NEAREST)
        canvas.paste(panel,(left,65))
    draw.text((20,331),f'{low:.1f}–{high:.1f} m elevation',font=small,fill='#25332c')
    draw.text((295,331),'Green: flat; red: >=1000 mm/m',font=small,fill='#25332c')
    draw.text((570,331),'Blue: water/near water; grey: unknown',font=small,fill='#25332c')
    draw.text((845,331),'Mixed / young / open / ancient / thicket',font=small,fill='#25332c')
    canvas.paste(Image.fromarray(elevation).resize((256,256),Image.Resampling.NEAREST),(20,383))
    with (root/'terrain-placement.csv').open(encoding='utf-8-sig',newline='') as stream:
        anchors=list(csv.DictReader(stream))
    tree_count=detail_count=0
    # Exact exported anchors and runtime classification, not guessed from a
    # layer number. This fixture contains trees and understory (no rocks).
    for row in anchors:
        px=(int(row['x_mm'])-(xs[0]-4000))/1000
        py=((ys[0]+4000)-int(row['y_mm']))/1000
        if not(0<=px<256 and 0<=py<256):continue
        tree=int(row['terrain_lattice'])==1
        tree_count+=tree;detail_count+=not tree
        if tree:draw.ellipse((19+px,382+py,21+px,384+py),fill='#173f28')
    draw.text((20,358),'Final tree anchors over elevation',font=small,fill='#25332c')
    draw.text((295,383),f'{tree_count} tree anchors; {detail_count} understory anchors inside survey.',font=font,fill='#25332c')
    draw.text((295,414),'Dots are anchor positions, not crown sizes or walkability.',font=font,fill='#25332c')
    draw.text((295,445),'Hydrology uses baked water distance; grey explicitly means unavailable.',font=font,fill='#25332c')
    draw.text((295,476),'Compare terrain, stand intent, and final placement separately.',font=font,fill='#25332c')
    active_fraction=float((grids['active']==1).mean())
    draw.text((295,507),f'Ecology active in {active_fraction:.1%} of samples; grey stand cells are outside scope.',font=font,fill='#25332c')
    result={'scope':'Actual runtime export; not visibility/clearance measurement','elevation_range_m':[low,high],
        'slope_range_mm_per_m':[int(grids['slope_mm_per_m'].min()),int(grids['slope_mm_per_m'].max())],
        'water_distance_unknown_fraction':float(unknown.mean()),'standing_water_fraction':float((grids['water_mm']>0).mean()),
        'ecology_active_fraction':active_fraction,'tree_anchors':tree_count,'understory_anchors':detail_count}
    if 'plot_lower_mm' in grids and 'plot_upper_mm' in grids:
        result['plot_relief']=plot_relief_summary(grids['plot_lower_mm'],grids['plot_upper_mm'])
    result['slope_exposure']=slope_exposure(rows,anchors)
    canvas.save(root/'terrain-placement-map.png')
    (root/'terrain-map-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path)
    print(json.dumps(render(parser.parse_args().directory),indent=2))
