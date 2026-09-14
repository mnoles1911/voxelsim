import csv,sys,statistics as st
path=sys.argv[1]; N=int(sys.argv[3]) if len(sys.argv)>3 else 6
rows=list(csv.DictReader(open(path,newline='',encoding='utf-8-sig')))
def num(d,k):
    try: return float(d.get(k,''))
    except: return None
idx=sorted(range(len(rows)),key=lambda i:-(num(rows[i],'FrameTime') or 0))[:N]
keys=[k for k in rows[0].keys() if k and k!='EVENTS']
quiet=[d for d in rows if 0<(num(d,'FrameTime') or 0)<=25]
qmed={k:(st.median([x for x in (num(d,k) for d in quiet) if x is not None]) if any(num(d,k) is not None for d in quiet) else None) for k in keys}
MS=[k for k in keys if k.endswith('Ms') or k in ('FrameTime','GameThreadTime','RenderThreadTime','GPUTime','RHIThreadTime') or k.startswith(('Exclusive/','GPU/','Slate/','FileIO/','VoxelWorklist/'))]
for i in idx:
    d=rows[i]
    print(f"\n=== frame #{i}  FrameTime={num(d,'FrameTime'):.2f}  GT={num(d,'GameThreadTime')}  RT={num(d,'RenderThreadTime')}  GPU={num(d,'GPUTime')}  RHIT={num(d,'RHIThreadTime')}")
    items=[]
    for k in MS:
        v=num(d,k)
        if v is None: continue
        q=qmed.get(k) or 0
        if v-q>1.0: items.append((v-q,v,q,k))
    items.sort(reverse=True)
    for delta,v,q,k in items[:12]:
        print(f"   +{delta:9.2f}  now {v:9.2f}  quiet {q:7.2f}  {k}")
