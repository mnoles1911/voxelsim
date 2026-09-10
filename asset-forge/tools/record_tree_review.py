"""Record an already performed photo, three-size and 36-variant visual review.

This does not judge images. Call only for explicitly inspected/accepted species;
the reviewer supplies concrete findings in visual-decisions.json first.
"""
import argparse
import json
from pathlib import Path
import _path
from forge import inventory
from temperate_collection import ROOT, STAGE, install


def record(names):
    decisions=json.loads((ROOT/'out/temperate-review/visual-decisions.json').read_text())
    catalog={r['species']:r for r in json.loads((ROOT/'out/temperate-review/reference-catalog.json').read_text())}
    for name in names:
        finding=decisions[name]
        report=json.loads((STAGE/f'{name}.json').read_text())['report']
        if report['status']!='ready_for_review' or report['generator_digest']!=inventory.generator_digest():
            raise ValueError('Incomplete or stale batch: '+name)
        artifacts={}
        for seed in (1,4,7):
            folder=ROOT/'out/temperate-review'/name
            pilot=json.loads((folder/f'pilot-{seed}.json').read_text())
            if pilot['problems']:raise ValueError('Pilot health failure: '+name)
            sha=inventory.digest(folder/f'pilot-{seed}.vxa')
            if sha!=inventory.digest(STAGE/'variants'/name/f'{name}-{seed:04d}'/'tree.vxa'):
                raise ValueError('Reviewed and staged geometry differ: '+name)
            artifacts[str(seed)]=sha
        path=ROOT/'library'/name/'reference-review.json'
        review=json.loads(path.read_text());entry=catalog[name]
        if review.get('findings'):review['previous_findings']=review['findings']
        review.update(status='passed pilot review',reviewed_seeds=[1,4,7],
            generator_digest=inventory.generator_digest(),review_date='2026-09-07',
            review_scope='Stylized spring vertical slice; source acceptance is separate from world variant endorsement.',
            findings=[finding],pilot_artifacts=artifacts,
            reviewed_images=[dict(image_id=p['image_id'],source=entry['photo_page'],
                purpose='Crown and branching; other-season photos are structure-only') for p in entry['photos'][:2]],
            batch_review=dict(status='accepted',seeds=list(range(1,37)),
                sheet_hash=inventory.digest(STAGE/f'{name}-36.png')))
        inventory.write_json(path,review)
        print('RECORDED',name,flush=True)


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('names',nargs='+');ap.add_argument('--install',action='store_true')
    args=ap.parse_args();record(args.names)
    if args.install:install(args.names)
