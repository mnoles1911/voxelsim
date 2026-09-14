import csv,sys,statistics as st
rows=list(csv.DictReader(open(sys.argv[1],newline='',encoding='utf-8-sig')))
N=int(sys.argv[2]) if len(sys.argv)>2 else 6
def num(d,k):
    try: return float(d.get(k,''))
    except: return None
keys=[k for k in rows[0].keys() if k and k!='EVENTS']
quiet=[d for d in rows if 0<(num(d,'GameThreadTime') or 0)<=25]
qmed={}
for k in keys:
    vs=[x for x in (num(d,k) for d in quiet) if x is not None]
    qmed[k]=st.median(vs) if vs else None
MS=[k for k in keys if k.endswith('Ms') or k in ('GameThreadTime','RenderThreadTime','GPUTime','RHIThreadTime') or k.startswith(('Exclusive/GameThread','Slate/GameThread','FileIO/','VoxelWorklist/','VoxelStream/'))]
idx=sorted(range(len(rows)),key=lambda i:-(num(rows[i],'GameThreadTime') or 0))[:N]
for i in idx:
    d=rows[i]
    print(f"\n=== row #{i}  GameThreadTime={num(d,'GameThreadTime'):.1f}  (next row FrameTime={num(rows[i+1],'FrameTime') if i+1<len(rows) else float('nan')})")
    items=[]
    for k in MS:
        v=num(d,k); q=qmed.get(k) or 0
        if v is None: continue
        if v-q>0.8: items.append((v-q,v,q,k))
    items.sort(reverse=True)
    for delta,v,q,k in items[:14]:
        print(f"   +{delta:9.2f}  now {v:9.2f}  quiet {q:7.2f}  {k}")
