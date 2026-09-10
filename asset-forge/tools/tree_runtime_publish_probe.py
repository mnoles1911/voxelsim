"""Exercise endorsed publication, cache reuse and refusal in a private library."""
import hashlib
import json
import shutil
import tempfile
from unittest.mock import patch
from pathlib import Path
import _path
import export_tree_runtime_appearance as runtime

real=runtime.ROOT
with tempfile.TemporaryDirectory(prefix='runtime-publish-probe-',dir=real/'out') as temp:
    fixture=Path(temp);(fixture/'out').mkdir();(fixture/'rules').mkdir()
    for name in ('tree-appearance-active.json','tree-appearance-spring-v2.json'):
        shutil.copy2(real/'rules'/name,fixture/'rules'/name)
    source=fixture/'library/temperate-oak/temperate-oak-0007'
    shutil.copytree(real/'library/temperate-oak/temperate-oak-0007',source)
    runtime.ROOT=fixture
    out=fixture/'engine/appearance';original=(source/'meta.json').read_bytes()
    before=hashlib.sha256((source/'tree.vxa').read_bytes()).hexdigest()
    result=runtime.publish_endorsed(source,out);packet=out/result['file']
    stamp=packet.stat().st_mtime_ns
    assert runtime.publish_endorsed(source,out)==result and packet.stat().st_mtime_ns==stamp
    assert (source/'meta.json').read_bytes()==original
    assert hashlib.sha256((source/'tree.vxa').read_bytes()).hexdigest()==before
    # A corrupt cache is rebuilt, never reused based on file existence.
    packet.write_bytes(b'corrupt');runtime.publish_endorsed(source,out)
    assert hashlib.sha256(packet.read_bytes()).hexdigest()==result['sha256']
    banks=fixture/'engine/banks';bank=banks/'temperate-oak/temperate-oak-0007.vxa'
    bank.parent.mkdir(parents=True);shutil.copy2(source/'tree.vxa',bank)
    published=runtime.refresh_published_inventory(out,banks)
    assert published['count']==1 and published['models'][0]['id']=='temperate-oak-0007'
    bank.write_bytes(b'stale bank')
    assert runtime.refresh_published_inventory(out,banks)['count']==0
    # The CLI must enforce the same bank intersection, rather than authorizing
    # an endorsed source whose game bank still contains different geometry.
    with patch('sys.argv', ['export_tree_runtime_appearance.py', '--publish-endorsed',
                            '--output', str(out), '--banks', str(banks)]):
        runtime.main()
    assert json.loads((out/'published.json').read_text())['count']==0
    shutil.copy2(source/'tree.vxa',bank)
    meta=json.loads(original);meta.update(inventory_candidate=True,visual_approved=False,review_status='inventory_candidate')
    (source/'meta.json').write_text(json.dumps(meta))
    try:runtime.publish_endorsed(source,out)
    except ValueError:pass
    else:raise AssertionError('Pending variant published')
    assert runtime.refresh_published_inventory(out,banks)['count']==0
    assert packet.exists() and bank.exists(),'Withdrawal must not delete saved-object geometry'
    (source/'meta.json').write_bytes(original)
    (source/'tree-appearance.bin').write_bytes(b'corrupt')
    try:runtime.publish_endorsed(source,out)
    except ValueError:pass
    else:raise AssertionError('Corrupt approved preview published')
    print('PASS: endorsed publication, byte preservation, verified cache/rebuild, inventory withdrawals, stale-bank exclusion, pending and corrupt refusals')
