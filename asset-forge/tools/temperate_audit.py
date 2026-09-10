"""Audit installed source authority, all seeded bytes and retirement evidence."""
import json
from collections import Counter
import _path
from forge import inventory, spec
from forge.forest_profiles import PROFILES
from temperate_collection import ROOT, STAGE

def main():
    code=inventory.generator_digest();rows=[];retired=0
    for name in PROFILES:
        record=json.loads((ROOT/'library'/name/'species.json').read_text())
        review=json.loads((ROOT/'library'/name/'reference-review.json').read_text())
        body,_=spec.load(ROOT/'specs'/f'{name}.json')
        assert record['generator_approved'] and record['approved_generator_digest']==code
        assert record['baseline_spec_hash']==spec.spec_hash(body)==spec.spec_hash(record['baseline_spec'])
        assert record['reference_seed']==7 and record['reference_variant_id']==f'{name}-0007'
        assert review['generator_digest']==code and review['status']=='passed pilot review'
        assert review['batch_review']['sheet_hash']==inventory.digest(STAGE/f'{name}-36.png')
        assert review['reviewed_images'] and review['reviewed_seeds']==[1,4,7]
        assert [r['seed'] for r in record['variants']]==list(range(1,37))
        hashes=set();sizes=Counter();forms=Counter();pitches=Counter();pending=0;heights=[]
        for row in record['variants']:
            path=ROOT/'out/forge-candidates'/name/row['id']
            if not path.exists():path=ROOT/'library'/name/row['id']
            meta=json.loads((path/'meta.json').read_text())
            assert meta['generator_digest']==code and not meta['problems']
            assert meta['spec_hash']==record['baseline_spec_hash']
            for file,sha in meta['artifact_hashes'].items():assert inventory.digest(path/file)==sha,(name,file)
            assert row['artifact_hash']==meta['artifact_hashes']['tree.vxa']
            hashes.add(row['artifact_hash']);s=meta['stats']
            sizes[s['size_class']]+=1;forms[s['growth_form']]+=1;pitches[s['voxel_cm']*10]+=1
            heights.append(s['height_m']);pending+=bool(meta['inventory_candidate'])
        assert len(hashes)==36 and sorted(sizes.values())==[12,12,12]
        assert sorted(forms.values())==[12,12,12] and pitches=={100:36}
        retired+=len(json.loads((STAGE/f'{name}-retirement.json').read_text())['replaced_ids'])
        report=json.loads((ROOT/'out/inventory-runs'/f'temperate-{name}'/'report.json').read_text())
        assert report['status']=='complete' and not report['failures']
        rows.append(dict(species=name,variants=36,pending=pending,sizes=dict(sizes),growth_forms=dict(forms),
            height_range_m=[min(heights),max(heights)],generation_seconds=report['elapsed_seconds']))
    aliases=json.loads((STAGE/'oak-alias-retirement.json').read_text())
    for alias in aliases['aliases']:
        name=alias['species'];retired+=len(alias['retired_ids'])
        for path in (ROOT/'specs'/f'{name}.json',ROOT/'library'/name,ROOT/'out/forge-candidates'/name,ROOT/'out/engine/banks'/name):assert not path.exists()
    result=dict(status='passed',generator_digest=code,profiles=len(rows),variants=sum(r['variants'] for r in rows),
        pending=sum(r['pending'] for r in rows),voxel_mm={'100':1764,'50':0,'25':0},
        retired_assets=retired,total_generation_seconds=round(sum(r['generation_seconds'] for r in rows),2),species=rows)
    inventory.write_json(STAGE/'completion-audit.json',result)
    print(json.dumps({k:v for k,v in result.items() if k!='species'},indent=2))

if __name__=='__main__':main()
