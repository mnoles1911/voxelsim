"""Exercise appearance serving with a copied asset, never the live library."""
import json
import shutil
import tempfile
import threading
import urllib.request
import urllib.error
from pathlib import Path
import _path
from forge import server,inventory

root=inventory.ROOT
name='temperate-oak';ident=name+'-0007'
source=root/'library'/name/ident
if not source.exists():source=inventory.CANDIDATES/name/ident
stage=root/'out/tree-appearance-collection-v2'/ident
with tempfile.TemporaryDirectory(prefix='appearance-probe-',dir=root/'out') as tmp:
    fixture=Path(tmp).resolve();assert fixture.parent==(root/'out').resolve()
    target=fixture/'library'/name/ident;shutil.copytree(source,target)
    server.ROOT=fixture;server.LIBRARY=fixture/'library';server.SPECS=fixture/'specs'
    (fixture/'out').mkdir()
    (fixture/'rules').mkdir()
    for name in ('tree-appearance-active.json','tree-appearance-spring-v2.json'):
        shutil.copy2(root/'rules'/name,fixture/'rules'/name)
    before=(target/'meta.json').read_bytes()
    body=(stage/'voxels.bin').read_bytes()+b'RGB1'+(stage/'species.rgb').read_bytes()
    # Simulate a newly saved seed without appearance sidecars. First inspection
    # must reproduce the approved batch result, without altering saved decisions.
    for name in ('tree-appearance.bin','tree-appearance.json','tree-appearance-thumb.png','tree-appearance-thumb.json'):
        (target/name).unlink(missing_ok=True)
    geometry_before=inventory.digest(target/'tree.vxa')
    http=server.Server(('127.0.0.1',0),server.Handler)
    worker=threading.Thread(target=http.serve_forever,daemon=True);worker.start()
    try:
        url=f'http://127.0.0.1:{http.server_port}/api/voxels?id={ident}'
        with urllib.request.urlopen(url) as response:
            assert response.read()==body
            assert response.headers['X-Tree-Appearance']=='spring-v2'
            assert response.headers['X-Tree-Foliage']=='broadleaf'
            assert response.headers['Cache-Control']=='no-cache'
        thumb_url=f'http://127.0.0.1:{http.server_port}/api/library/thumb?id={ident}'
        with urllib.request.urlopen(thumb_url) as response:
            assert response.read()==(target/'tree-appearance-thumb.png').read_bytes()
            assert response.headers['Cache-Control']=='no-store'
        thumb_meta=json.loads((target/'tree-appearance-thumb.json').read_text())
        thumb_meta['preview_sha256']='obsolete'
        inventory.write_json(target/'tree-appearance-thumb.json',thumb_meta)
        with urllib.request.urlopen(thumb_url) as response:
            assert response.read()==(target/'thumb.png').read_bytes()
        (target/'tree-appearance.bin').write_bytes(body+b'corrupt')
        try:urllib.request.urlopen(url)
        except urllib.error.HTTPError as exc:assert exc.code==409
        else:raise AssertionError('Corrupt appearance was served')
        assert (target/'meta.json').read_bytes()==before
        assert inventory.digest(target/'tree.vxa')==geometry_before
    finally:http.shutdown();http.server_close();worker.join()
print('PASS: first-inspection appearance equals batch payload; correct headers; corruption refused; geometry and decisions unchanged')
