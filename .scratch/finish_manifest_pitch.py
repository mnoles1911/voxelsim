from pathlib import Path
r=Path('D:/voxelsim')
def edit(p,fn):
 p=r/p;p.write_text(fn(p.read_text(encoding='utf-8')),encoding='utf-8')
def manifest(s):
 s=s.replace('report: ExportReport) -> bytes | None:', 'report: ExportReport, pitch_um: bool = False) -> bytes | None:',1)
 s=s.replace('    if not float(res_cm * 10.0).is_integer():\n        raise ValueError("World-scatter manifests require whole-mm pitch; fractional-pitch craftables use the entity library")\n    voxel_mm = int(round(res_cm * 10.0))','    if not pitch_um and not float(res_cm * 10.0).is_integer():\n        raise ValueError("Fractional species pitch requires a VXM v3 micrometre record")\n    voxel_mm = int(round(res_cm * (10000.0 if pitch_um else 10.0)))')
 needle='    records = []\n    attachments: list[tuple[int, int, int]]'
 s=s.replace(needle,'    pitch_um = any(not float(float(sm.get(s,"resolution_cm"))*10).is_integer() for _,s in specs)\n    version = 3 if pitch_um else 2\n'+needle)
 s=s.replace('species_record(spec, name, int(seeds_baked.get(name, 0)), report)','species_record(spec, name, int(seeds_baked.get(name, 0)), report, pitch_um=pitch_um)')
 s=s.replace('"<IIIIIII", MANIFEST_VERSION,','"<IIIIIII", version,')
 s=s.replace('if version != MANIFEST_VERSION:', 'if version not in (2,3):')
 s=s.replace('"voxel_mm": rec[5],','"voxel_mm": rec[5] / 1000.0 if version == 3 else rec[5],')
 return s
edit('asset-forge/forge/manifest.py',manifest)
edit('voxel-core/include/voxelcore/assetmanifest.h',lambda s:s.replace('uint32_t voxelSizeMm = 0;', 'double voxelSizeMm = 0;'))
edit('voxel-core/include/voxelcore/assetpolicy.h',lambda s:s.replace('uint32_t voxelSizeMm = uint32_t(kVoxelSizeMm);','double voxelSizeMm = kVoxelSizeMm;'))
edit('voxel-core/src/assetmanifest.cpp',lambda s:s.replace('if (readU32(blob + 4) != kVxmVersion)', 'const uint32_t version = readU32(blob + 4);\n    if (version != 2u && version != 3u)').replace('s.voxelSizeMm = readU32(p + 72);','s.voxelSizeMm = double(readU32(p + 72)) / (version == 3 ? 1000.0 : 1.0);'))
edit('voxel-core/src/assetbank.cpp',lambda s:s.replace('const int64_t vs = int64_t(g.voxelSizeMm());','const int64_t vs = g.voxelSizeUm(); // exact micrometres, including 12.5 mm').replace('int64_t(layer.maxHeightMm))','int64_t(layer.maxHeightMm)*1000)').replace('int64_t(layer.maxDepthMm))','int64_t(layer.maxDepthMm)*1000)').replace('int64_t(layer.maxRadiusMm))','int64_t(layer.maxRadiusMm)*1000)'))
edit('voxel-core/tests/test_craftpersist.cpp',lambda s:s.replace('CHECK(stamped == EditLog::kFormatVersion);','CHECK(stamped == 3u); // legacy whole-mm craft logs retain v3 bytes').replace('CHECK(full.format == EditLog::kFormatVersion);','CHECK(full.format == 3u);'))
edit('asset-forge/forge/server.py',lambda s:s.replace('if str(cm).rstrip("0").rstrip(".") not in resolutionlib.allowed(spec) and cm not in map(float,resolutionlib.allowed(spec)):', 'if cm not in map(float,resolutionlib.allowed(spec)):'))
print('VXM v3 stores exact pitch; legacy whole-mm manifests and logs retain their versions.')
