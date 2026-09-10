"""Generate local environment inventory, shared with the Forge workbench.

python tools/environment_inventory.py temperate-oak --count 36 --workers 3
python tools/environment_inventory.py birch meadow-grass water-reed --count 8
Rerun the same request to verify and reuse cached exports.
"""
import argparse
import json
import _path
from forge import inventory

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('species',nargs='+');p.add_argument('--count',type=int,default=36)
    p.add_argument('--seed-start',type=int,default=1);p.add_argument('--workers',type=int,default=2)
    args=p.parse_args()
    result=inventory.run(args.species,args.seed_start,args.count,args.workers)
    print(json.dumps({k:v for k,v in result.items() if k!='rows'},indent=2))
    if result['failures']:raise SystemExit(1)

if __name__=='__main__':main()
