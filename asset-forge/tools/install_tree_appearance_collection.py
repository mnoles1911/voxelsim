"""Install staged appearance sidecars, preserving geometry and review decisions.

Explicit --apply is required; default only verifies and writes a dry-run report.
Only currently existing library/candidate records are eligible. No asset is
endorsed, recreated, or moved. Each record's appearance manifest is written last.
"""
import argparse
import json
import _path
from forge import inventory

ROOT=inventory.ROOT
STAGE=ROOT/'out/tree-appearance-collection-v2'

def main():
    parser=argparse.ArgumentParser()
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--apply',action='store_true')
    mode.add_argument('--verify-installed',action='store_true')
    args=parser.parse_args()
    manifest=json.loads((STAGE/'manifest.json').read_text())
    assert not manifest['errors'] and len(manifest['models'])==49*36,'Incomplete staged collection'
    assert inventory.digest(ROOT/'rules/tree-appearance-spring-v2.json')==manifest['palette_sha256'],'Palette changed since staging'
    prepared=[]
    for row in manifest['models']:
        choices=[ROOT/'library'/row['species']/row['id'],inventory.CANDIDATES/row['species']/row['id']]
        dirs=[d for d in choices if (d/'meta.json').is_file()]
        assert len(dirs)==1,'Missing or ambiguous current asset '+row['id']
        target=dirs[0];source=STAGE/row['id']
        assert inventory.digest(target/'tree.vxa')==row['geometry_sha256'],'Geometry changed '+row['id']
        for name,sha in row['files'].items():assert inventory.digest(source/name)==sha,'Staged payload changed '+row['id']
        if args.verify_installed:
            actual=json.loads((target/'tree-appearance.json').read_text())
            assert actual['geometry_sha256']==row['geometry_sha256'],'Appearance geometry mismatch '+row['id']
            assert actual['palette_sha256']==manifest['palette_sha256'],'Appearance palette mismatch '+row['id']
            assert actual['preview_sha256']==inventory.digest(target/'tree-appearance.bin'),'Installed payload corrupt '+row['id']
            expected=(source/'voxels.bin').read_bytes()+b'RGB1'+(source/'species.rgb').read_bytes()
            assert (target/'tree-appearance.bin').read_bytes()==expected,'Installed appearance differs from staged result '+row['id']
        original=(target/'meta.json').read_bytes()
        prepared.append((row,target,original,source))
    for row,target,original,source in prepared:
        if not args.apply:continue
        # Never edit meta.json: its endorsement/rejection state and geometry key
        # remain exactly as they were. Appearance has its own revision manifest.
        assert (target/'meta.json').read_bytes()==original,'Concurrent review changed; rerun safely'
        data=(source/'voxels.bin').read_bytes()+b'RGB1'+(source/'species.rgb').read_bytes()
        tmp=target/'tree-appearance.bin.tmp';tmp.write_bytes(data);tmp.replace(target/'tree-appearance.bin')
        inventory.write_json(target/'tree-appearance.json',dict(revision='spring-v2',geometry_sha256=row['geometry_sha256'],preview_sha256=inventory.digest(target/'tree-appearance.bin'),palette_sha256=manifest['palette_sha256'],needle=row['needle'],voxel_mm=row['voxel_mm']))
        assert (target/'meta.json').read_bytes()==original
    status='installed verified' if args.verify_installed else 'installed' if args.apply else 'dry-run verified'
    report='verification-report.json' if args.verify_installed else 'install-report.json'
    inventory.write_json(STAGE/report,dict(status=status,models=len(prepared),decisions_modified=False,geometry_modified=False))
    if args.apply:inventory.write_json(ROOT/'rules/tree-appearance-active.json',dict(revision='spring-v2',palette_sha256=manifest['palette_sha256']))
    print(('INSTALLED' if args.apply else 'VERIFIED'),len(prepared),'appearance records')

if __name__=='__main__':main()
