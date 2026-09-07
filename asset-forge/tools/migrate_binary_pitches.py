"""Migrate legacy authoring pitches with exact source backups and an audit log."""
import json
import shutil
from pathlib import Path
import _path
from forge import resolution, spec, server

ROOT=Path(__file__).resolve().parents[1]
BACKUP=ROOT/'.backup/binary-pitches-2026-09-06'

def main():
    changes=[]
    for p in sorted((ROOT/'specs').glob('*.json')):
        raw=json.loads(p.read_text(encoding='utf-8'))
        old=raw.get('resolution_cm','5');new=resolution.normalize(raw,old)
        if str(old)==new: continue
        dest=BACKUP/'specs'/p.name;dest.parent.mkdir(parents=True,exist_ok=True)
        if not dest.exists(): shutil.copy2(p,dest)
        raw['resolution_cm']=new
        p.write_text(json.dumps(raw,indent=2,sort_keys=True)+'\n',encoding='utf-8')
        row=dict(name=p.stem,old_cm=old,new_cm=new,rebuilt_keeps=[])
        directory=ROOT/'library'/p.stem
        if directory.exists():
            for entry in sorted(directory.iterdir()):
                if not (entry/'tree.vxa').is_file(): continue
                target=BACKUP/'library'/p.stem/entry.name
                if not target.exists(): shutil.copytree(entry,target)
                seed=int(entry.name.rsplit('-',1)[1])
                body,_=spec.load(p)
                result=server.keep(body,seed)
                row['rebuilt_keeps'].append(dict(id=result['id'],problems=result['problems']))
        changes.append(row)
    if changes:
        (BACKUP/'migration.json').write_text(json.dumps(changes,indent=2)+'\n',encoding='utf-8')
    print(f'Migrated {len(changes)} specs; rebuilt {sum(len(c["rebuilt_keeps"]) for c in changes)} kept assets. Originals: {BACKUP}')

if __name__=='__main__':main()
