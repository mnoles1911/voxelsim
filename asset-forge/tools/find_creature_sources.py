"""Cache public source metadata and license-filtered review sheets.

Discovery is not anatomical approval. An individual model's license is recorded
separately from the archive license. Downloads are bounded and cached.
"""
import argparse
import json
import urllib.request
import urllib.error
import time
from pathlib import Path
from PIL import Image, ImageDraw
import io
import objaverse

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out/creature-reconstruction/research'


def fetch(uid):
    path=OUT/(uid+'.json')
    if path.exists():
        return json.loads(path.read_text(encoding='utf8'))
    request=urllib.request.Request('https://api.sketchfab.com/v3/models/'+uid,
                                  headers={'User-Agent':'AssetForge-reference-research/1.0'})
    try:
        with urllib.request.urlopen(request,timeout=25) as response:
            data=json.load(response)
        path.write_text(json.dumps(data,indent=2),encoding='utf8')
        return data
    except urllib.error.HTTPError as error:
        return dict(uid=uid,error=str(error),status=error.code,
                    retry_after=error.headers.get('Retry-After'))
    except Exception as error:
        return dict(uid=uid,error=str(error))


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('category',choices=['tiger','deer','crow','eagle','shark'])
    args=parser.parse_args()
    OUT.mkdir(parents=True,exist_ok=True)
    uids=objaverse.load_lvis_annotations()[args.category]
    data=[]
    for index,uid in enumerate(uids):
        cached=(OUT/(uid+'.json')).exists()
        item=fetch(uid)
        data.append(item)
        if item.get('status') == 429:
            # Stop this run instead of hammering the source or reporting an
            # empty result as evidence that no licensed sources exist.
            data.extend(dict(uid=u,error='deferred after HTTP 429') for u in uids[index+1:])
            break
        if not cached:
            time.sleep(.5)
    (OUT/(args.category+'-candidates.json')).write_text(json.dumps(data,indent=2),encoding='utf8')
    permitted=[d for d in data if d.get('license',{}).get('slug') in ('by','cc0')]
    permitted.sort(key=lambda d:d.get('likeCount',0),reverse=True)
    summary=[]
    for page in range((len(permitted)+19)//20):
        canvas=Image.new('RGB',(1500,5*230),(225,228,230));draw=ImageDraw.Draw(canvas)
        for i,d in enumerate(permitted[page*20:page*20+20]):
            x,y=(i%4)*375,(i//4)*230
            try:
                thumb=sorted(d['thumbnails']['images'],key=lambda t:abs(t['width']-640))[0]
                image=Image.open(io.BytesIO(urllib.request.urlopen(thumb['url'],timeout=20).read())).convert('RGB')
                image.thumbnail((370,175));canvas.paste(image,(x,y))
            except Exception:
                pass
            label=(d.get('name','')[:45]).encode('ascii','replace').decode()
            draw.text((x+4,y+178),label,fill='black')
            draw.text((x+4,y+194),d['uid'],fill='black')
            draw.text((x+4,y+209),d['license']['slug'],fill='black')
        canvas.save(OUT/f'{args.category}-sources-{page}.jpg')
    for d in permitted:
        summary.append(dict(uid=d['uid'],name=d['name'],author=d.get('user',{}).get('displayName'),
                            license=d['license'],url=d.get('viewerUrl'),faces=d.get('faceCount'),
                            description=d.get('description',''),visual_approved=False))
    (OUT/(args.category+'-shortlist.json')).write_text(json.dumps(summary,indent=2),encoding='utf8')
    errors=sum('error' in d for d in data)
    print(json.dumps(dict(category=args.category,total=len(data),errors=errors,
                          discovery_complete=errors==0,license_candidates=len(permitted),
                          pages=(len(permitted)+19)//20)))


if __name__=='__main__':main()
