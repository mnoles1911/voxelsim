import csv,sys
rows=list(csv.DictReader(open(sys.argv[1],newline='',encoding='utf-8-sig')))
def num(d,k):
    try: return float(d.get(k,''))
    except: return None
waitcols=[k for k in rows[0].keys() if k and 'EventWait' in k]
print("wait columns:",waitcols)
idx=sorted(range(len(rows)),key=lambda i:-(num(rows[i],'FrameTime') or 0))[:5]
for i in idx:
    for off in (-1,0):
        d=rows[i+off]
        print(f"frame {i+off:5d} FT={num(d,'FrameTime'):8.1f} GT={num(d,'GameThreadTime'):7.1f} RT={num(d,'RenderThreadTime'):7.1f} | " +
              " ".join(f"{k}={num(d,k):.1f}" for k in waitcols if (num(d,k) or 0)>0.5))
    print()
# how common is a big wait overall
big=[(i,num(rows[i],k),k) for i in range(len(rows)) for k in waitcols if (num(rows[i],k) or 0)>100]
print("frames with any EventWait > 100 ms:",len(big))
from collections import Counter
print(Counter(k for _,_,k in big))
