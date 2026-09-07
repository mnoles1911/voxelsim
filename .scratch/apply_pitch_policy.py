from pathlib import Path
r=Path('D:/voxelsim/asset-forge')
def edit(p,fn):
 p=r/p;p.write_text(fn(p.read_text(encoding='utf-8')),encoding='utf-8')
def spec(s):
 s=s.replace('from . import (biomes as biomelib, categories as catlib,','from . import resolution as resolutionlib\nfrom . import (biomes as biomelib, categories as catlib,')
 a=s.index('    P("resolution_cm",');b=s.index('    P("trunk.radius_base_m"',a)
 s=s[:a]+'''    P("resolution_cm", "Voxel size", "5", kind="choice", group="general",
      choices=resolutionlib.TIERS_CM,
      help="Supported pitches: 100, 50, 25 and 12.5 mm. Only creatures and "
           "craftables may use 12.5 mm. Terrain-stamped trees and rocks remain "
           "at 100 mm; other environment assets may use 100, 50 or 25 mm. "
           "Previews may select a coarser supported tier to fit their budget."),

'''+s[b:]
 s=s.replace('        if raw is _MISSING:\n            continue\n        try:', '        if raw is _MISSING:\n            continue\n        if p.path == "resolution_cm":\n            set_(out,p.path,raw)\n            continue\n        try:',1)
 marker='    # Cross-checks with the consequence named:'
 pos=s.index(marker)
 s=s[:pos]+'''    prior_pitch = get(out,"resolution_cm")
    pitch = resolutionlib.normalize(out,prior_pitch)
    if str(prior_pitch) != pitch and "resolution_cm" in spec:
        rep.warnings.append(f"resolution_cm: {prior_pitch!r} changed to {pitch} cm under the supported pitch policy")
    set_(out,"resolution_cm",pitch)
'''+s[pos:]
 s=s.replace('"choices": list(p.choices),','''"choices": list(resolutionlib.allowed({"kind":kind})) if p.path=="resolution_cm" and kind else list(p.choices),
            **({"default_category":catlib.BY_KIND.get(kind or "tree"),
                "choices_by_category":{c:list(resolutionlib.allowed({"kind":kind or "tree","category":c})) for c in ("environment","creature","craftable")}}
               if p.path=="resolution_cm" else {}),''')
 return s
edit('forge/spec.py',spec)
edit('forge/pipeline.py',lambda s:s.replace('from . import parts as partslib','from . import resolution as resolutionlib\nfrom . import parts as partslib').replace('cm = float(override) if override else float(get(spec, "resolution_cm"))','cm = resolutionlib.require(spec, override if override is not None else get(spec, "resolution_cm"))'))
def server(s):
 s=s.replace('PREVIEW_TIERS = (1.0, 2.0, 2.5, 5.0, 10.0)','PREVIEW_TIERS = (1.25, 2.5, 5.0, 10.0)')
 s=s.replace('    for cm in PREVIEW_TIERS:\n        if cm < authored:', '    from . import resolution as resolutionlib\n    for cm in PREVIEW_TIERS:\n        if str(cm).rstrip("0").rstrip(".") not in resolutionlib.allowed(spec) and cm not in map(float,resolutionlib.allowed(spec)):\n            continue\n        if cm < authored:')
 s=s.replace('    return max(PREVIEW_CM, authored)','    return 10.0')
 s=s.replace('"category": catlib.BY_KIND.get(k.key),','"category": catlib.BY_KIND.get(k.key),\n                 "voxel_pitches_mm": [float(c)*10 for c in specmod.resolutionlib.allowed({"kind":k.key})],')
 needle='    spec, _ = specmod.validate({\n        "name": name, "kind": kind,'
 s=s.replace(needle,'    specmod.resolutionlib.require({"kind":kind},grid.voxel_m*100)\n'+needle+'\n        "resolution_cm": f"{grid.voxel_m*100:g}",')
 s=s.replace('if not (10 <= voxel_mm <= 1000):','if voxel_mm not in [float(c)*10 for c in specmod.resolutionlib.allowed({"kind":kind})]:').replace('"voxel_mm must be 10-1000"','"voxel_mm must be one of the supported pitches for this kind"')
 s=s.replace('            return self._json(import_asset(name, kind, grid, fmt))','            try:\n                return self._json(import_asset(name, kind, grid, fmt))\n            except ValueError as e:\n                return self._json({"error":str(e)},400)')
 return s
edit('forge/server.py',server)
edit('forge/language.py',lambda s:s.replace('"2" if length >= 0.5 else "1"','"2.5" if length >= 0.5 else "1.25"'))
edit('forge/cli.py',lambda s:s.replace('if k is None or k.lattice != "terrain":','if k is None or k.lattice != "terrain" or specmod.resolutionlib.categories.of(s) != "environment":').replace('e.g. --res 2','e.g. --res 1.25'))
edit('web/src/lib/schema.ts',lambda s:s.replace('export interface UiParam {','export interface UiParam {\n  choices_by_category?: Record<string, string[]>;\n  default_category?: string;').replace('  ready: boolean;','  ready: boolean;\n  voxel_pitches_mm: number[];'))
edit('web/src/components/ForgeView.tsx',lambda s:s.replace('                p={p}','                p={p.choices_by_category ? {...p, choices:p.choices_by_category[String(spec.category ?? p.default_category)] ?? p.choices} : p}').replace('<SelectItem key={c} value={c}>{c}</SelectItem>','<SelectItem key={c} value={c}>{p.path === "resolution_cm" ? `${Number(c)*10} mm` : c}</SelectItem>'))
def imports(s):
 s=s.replace('  const problems: string[] = [];','  const pitches = world.kinds.find(k => k.key === kind)?.voxel_pitches_mm ?? [100];\n  const problems: string[] = [];')
 s=s.replace('if (format === "vox" && (voxelMm < 10 || voxelMm > 1000)) problems.push("Voxel size must be 10-1000 mm.");','if (format === "vox" && !pitches.includes(voxelMm)) problems.push("Choose a supported voxel size for this asset kind.");')
 s=s.replace('<Input type="number" min={10} max={1000} step={10} value={voxelMm} onChange={(e) => setVoxelMm(Number(e.target.value))} />','<Select value={String(voxelMm)} onValueChange={v => setVoxelMm(Number(v))}><SelectTrigger><SelectValue /></SelectTrigger><SelectContent>{pitches.map(mm => <SelectItem key={mm} value={String(mm)}>{mm} mm</SelectItem>)}</SelectContent></Select>')
 return s
edit('web/src/components/ImportDialog.tsx',imports)
print('Central pitch policy is wired into validation, generation, previews, schema, imports and UI.')
