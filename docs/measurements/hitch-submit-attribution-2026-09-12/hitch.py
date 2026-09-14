import csv,sys,statistics as st
path=sys.argv[1]; thresh=float(sys.argv[2]) if len(sys.argv)>2 else 100.0
rows=[]
with open(path,newline='',encoding='utf-8-sig') as f:
    r=csv.DictReader(f)
    for d in r: rows.append(d)
def num(d,k):
    v=d.get(k,'')
    try: return float(v)
    except: return None
ft=[num(d,'FrameTime') for d in rows]
ft=[x for x in ft if x is not None]
ft_s=sorted(ft)
def pct(p): return ft_s[min(len(ft_s)-1,int(len(ft_s)*p))]
print(f"frames={len(ft)} p50={pct(.5):.2f} p95={pct(.95):.2f} p99={pct(.99):.2f} max={ft_s[-1]:.2f}")
for t in (33.3,50,100,200,400):
    print(f"  frames > {t:>5} ms: {sum(1 for x in ft if x>t):5d}  ({100*sum(1 for x in ft if x>t)/len(ft):.2f}%)")
hit=[d for d in rows if (num(d,'FrameTime') or 0)>thresh]
quiet=[d for d in rows if 0<(num(d,'FrameTime') or 0)<=25]
print(f"\nhitch frames (>{thresh} ms): {len(hit)}   quiet frames (<=25 ms): {len(quiet)}")
if not hit: sys.exit()
cols=[c for c in rows[0].keys() if c and c not in ('EVENTS',)]
out=[]
for c in cols:
    hv=[num(d,c) for d in hit]; hv=[x for x in hv if x is not None]
    qv=[num(d,c) for d in quiet]; qv=[x for x in qv if x is not None]
    if len(hv)<max(2,len(hit)//4) or not qv: continue
    hm=st.median(hv); qm=st.median(qv)
    out.append((hm-qm,hm,qm,max(hv),c))
out.sort(reverse=True)
print(f"\n{'delta':>10} {'hitch med':>10} {'quiet med':>10} {'hitch max':>11}  column")
for delta,hm,qm,mx,c in out[:28]:
    print(f"{delta:10.2f} {hm:10.2f} {qm:10.2f} {mx:11.2f}  {c}")
