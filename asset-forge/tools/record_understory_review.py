"""Bind an explicitly written manual review to the exact inspected artifacts."""
import argparse,json,time
from pathlib import Path
import _path
from forge import inventory
ROOT=Path(__file__).resolve().parents[1]
def record(name):
 path=ROOT/'out/understory-review/manual-findings.json';reviews=json.loads(path.read_text());review=reviews.get(name,{})
 if review.get('status')!='ready_for_acceptance':raise ValueError('Write the actual manual comparison and explicit review decision first')
 if review.get('reviewed_seeds')!=[1,4,7] or review.get('batch_seeds')!=list(range(1,37)) or not review.get('viewed_photos'):raise ValueError('Complete manual photo, three-size and batch review required')
 root=ROOT/'out/understory-collection';item=json.loads((root/f'{name}.json').read_text());code=inventory.generator_digest(item['spec'])
 if item['generator_digest']!=code:raise ValueError('Restage current source before binding review')
 review.update(status='accepted',generator_digest=code,sheet_hash=inventory.digest(root/f'{name}-36.png'),reviewed_at=time.time(),pilot_hashes={str(seed):inventory.digest(root/'variants'/name/f'{name}-{seed:04d}'/'tree.vxa') for seed in (1,4,7)})
 inventory.write_json(path,reviews)
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('names',nargs='+');args=ap.parse_args()
 for name in args.names:record(name)
