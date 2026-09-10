"""Regression: Windows sharing errors must not leave an immortal starting job."""
import _path
import json
import tempfile
import time
from pathlib import Path
from unittest.mock import patch
from forge import inventory


def main():
    with tempfile.TemporaryDirectory() as temp:
        path=Path(temp)/'report.json'
        path.write_text('{}')
        replace=inventory.os.replace
        calls=[]
        def transient(src,dest):
            calls.append(1)
            if len(calls)<3:raise PermissionError('sharing violation')
            return replace(src,dest)
        with patch.object(inventory.os,'replace',transient):
            inventory.write_json(path,{'ok':True})
        assert json.loads(path.read_text())=={'ok':True} and len(calls)==3
        real_write=inventory.write_json
        def blocked(path,body):
            if body['status']=='starting':return real_write(path,body)
            raise PermissionError('progress storage denied')
        def failed_run(*args,**kwargs):
            raise PermissionError('progress storage denied')
        with patch.object(inventory,'ROOT',Path(temp)),patch.object(inventory,'write_json',blocked),patch.object(inventory,'run',failed_run):
            ident=inventory.start({'species':['tundra-pine'],'count':36})['id']
            deadline=time.time()+3
            while inventory._active and time.time()<deadline:time.sleep(.01)
            report=next(r for r in inventory.reports() if r['id']==ident)
            assert report['status']=='failed' and 'storage denied' in report['error']
            assert inventory._active is None
        inventory._live_reports.clear()
    print('PASS: transient replacement retries; permanent errors visible to UI; batch lock released')


if __name__=='__main__':main()
