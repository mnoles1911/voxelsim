"""Isolated fixtures prove endorsement/revocation gates; never publishes real banks."""
import json,shutil,tempfile
from pathlib import Path
if not __package__:import _path
from forge import inventory
from tools import publish_temperate_appearance as publish
ROOT=inventory.ROOT

def main():
    source=ROOT/'out/forge-candidates/arrowhead/arrowhead-0007'
    with tempfile.TemporaryDirectory(prefix='appearance-gate-test-',dir=ROOT/'out') as temp:
        root=Path(temp);inventory.ROOT=root;target=root/'library/arrowhead/arrowhead-0007';target.mkdir(parents=True)
        for name in ('tree.vxa','spec.json','meta.json','tree-appearance.bin','tree-appearance.json','tree-appearance-runtime.vac','tree-appearance-runtime.json','tree-appearance-thumb.png','tree-appearance-thumb.json'):
            if (source/name).is_file():shutil.copy2(source/name,target/name)
        meta=json.loads((target/'meta.json').read_text());meta.update(id=target.name,species='arrowhead',seed=7,review_status='pending',visual_approved=False,inventory_candidate=True)
        inventory.write_json(target/'meta.json',meta);output=root/'appearance';banks=root/'banks'
        try:publish.publish_endorsed(target,output);raise AssertionError('pending admitted')
        except ValueError:pass
        meta.update(review_status='endorsed',visual_approved=True,inventory_candidate=False);inventory.write_json(target/'meta.json',meta)
        row=publish.publish_endorsed(target,output);assert row['version']==2 and row['pitch_encoding']=='integer-micrometres'
        assert publish.refresh_published_inventory(output,banks)['count']==0
        bank=banks/'arrowhead'/f'{target.name}.vxa';bank.parent.mkdir(parents=True);shutil.copy2(target/'tree.vxa',bank)
        assert publish.refresh_published_inventory(output,banks)['count']==1
        bank.write_bytes(b'wrong');assert publish.refresh_published_inventory(output,banks)['count']==0
        shutil.copy2(target/'tree.vxa',bank);meta['review_status']='rejected';meta['visual_approved']=False;inventory.write_json(target/'meta.json',meta)
        assert publish.refresh_published_inventory(output,banks)['count']==0
        assert (output/row['file']).is_file(),'stale packet retained but no longer authorizes placement'
        print('PASS: pending refusal, VAC2 export, missing/mismatched bank refusal, exact bank admission, rejection revocation')
    inventory.ROOT=ROOT
if __name__=='__main__':main()
