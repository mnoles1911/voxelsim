import csv,sys
rows=list(csv.DictReader(open(sys.argv[1],newline='',encoding='utf-8-sig')))
def num(d,k):
    try: return float(d.get(k,''))
    except: return None
keys=rows[0].keys()
want=[k for k in keys if k and any(s in k for s in ('AsyncLoading','PSO','RasterAtlas','ChunksApplied','Unaccounted','MaxFrameTime','EventWait','Hitch','QueueDepth','Streamable','RenderAssetStreaming','ShaderCompil','Pipeline'))]
print("columns watched:", len(want))
idx=sorted(range(len(rows)),key=lambda i:-(num(rows[i],'FrameTime') or 0))[:5]
ev=[ (i,rows[i].get('EVENTS','')) for i in range(len(rows)) if rows[i].get('EVENTS','')]
print("EVENTS rows:",len(ev))
for i,e in ev[:15]: print("   frame",i,"->",e[:120])
for i in idx:
    print(f"\n=== frame #{i} FrameTime={num(rows[i],'FrameTime'):.1f}")
    for off in (-1,0,1):
        j=i+off
        if j<0 or j>=len(rows): continue
        d=rows[j]
        vals=[(k,num(d,k)) for k in want]
        vals=[(k,v) for k,v in vals if v not in (None,0.0)]
        print(f"  [{off:+d}] FT={num(d,'FrameTime'):8.1f} EVENTS={d.get('EVENTS','')[:40]!r} " + " ".join(f"{k.split('/')[-1]}={v:g}" for k,v in vals[:10]))
