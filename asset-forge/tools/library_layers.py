"""Size published query bounds from endorsed authoritative library geometry.

The same deterministic layer table is used by banks, manifest and enginecheck.
This changes publication bounds, never generator geometry or saved variants.
"""
from dataclasses import replace
import json
from pathlib import Path
from types import FunctionType
from forge import manifest, spec, vxa

BASE_LAYERS=manifest.LAYERS
# Preserve the existing filing policy while expanding query bounds. Otherwise
# a taller published cap could silently admit unrelated legacy species.
BASE_ASSIGN=FunctionType(manifest.assign_layer.__code__,
    dict(manifest.assign_layer.__globals__,LAYERS=BASE_LAYERS),
    argdefs=manifest.assign_layer.__defaults__)


def configure(root):
    root=Path(root);manifest.LAYERS=BASE_LAYERS;manifest.assign_layer=BASE_ASSIGN
    oversized=set()
    required=[[l.max_height_mm,l.max_depth_mm,l.max_radius_mm] for l in BASE_LAYERS]
    # Filing uses the original table throughout measurement, so the result is
    # independent of species iteration order and previous configure() calls.
    for record in sorted((root/'library').glob('*/species.json')):
        name=record.parent.name
        path=root/'specs'/f'{name}.json'
        if not path.exists():continue
        body,_=spec.load(path)
        if spec.curation(body)['status']!='approved':continue
        height=manifest.nominal_height_m(body,body['kind'])
        index=manifest.assign_layer(body['kind'],height,manifest.ExportReport(),name,float(spec.get(body,'placement.spacing_m')))
        if index==manifest.LAYER_NOT_SCATTERED:continue
        if index<0:index=0
        endorsed=False
        for meta_path in sorted(record.parent.glob('*/meta.json')):
            meta=json.loads(meta_path.read_text())
            if meta.get('inventory_candidate') or meta.get('imported'):continue
            endorsed=True
            grid=vxa.read(meta_path.parent/'tree.vxa')
            pitch=grid.voxel_m*1000
            ox,oy,oz=map(int,grid.origin);nx,ny,nz=grid.shape
            dims=[max(round((oz+nz)*pitch),round(height*1000*manifest.FILE_HEADROOM_NUM/manifest.FILE_HEADROOM_DEN)),
                  max(0,round(-oz*pitch)),round(max(abs(ox),abs(oy),abs(ox+nx),abs(oy+ny))*pitch)]
            required[index]=[max(a,b) for a,b in zip(required[index],dims)]
        if endorsed and height*1000*manifest.FILE_HEADROOM_NUM/manifest.FILE_HEADROOM_DEN>BASE_LAYERS[0].max_height_mm:
            oversized.add(name)
    manifest.LAYERS=tuple(replace(layer,max_height_mm=req[0],max_depth_mm=req[1],max_radius_mm=req[2])
                          for layer,req in zip(BASE_LAYERS,required))
    def assign(kind,height_m,report,name,spacing_m):
        if name in oversized:return 0
        return BASE_ASSIGN(kind,height_m,report,name,spacing_m)
    manifest.assign_layer=assign
    return manifest.LAYERS
