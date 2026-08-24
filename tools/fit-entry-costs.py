#!/usr/bin/env python3
"""Fit what an entry scan costs, from logs already on disk.

    python tools/fit-entry-costs.py Saved/ahead-on.log Saved/gp-ctl2.log ...

Regresses entryMs[level, window] on two regressors that every leg since
2026-08-23 already logs -- the per-level SCAN COUNT (`Voxel recompute (max
since last log)`, the `scans R0=..` field) and the per-level REJECTION COUNT
(`Voxel admission detail`) -- with one per-scan cost per ring and one shared
per-candidate cost.

WHY IT PRINTS A MODEL COMPARISON AND NOT JUST COEFFICIENTS. On the three
census legs the candidate term changes R2 by 0.0000 and moves every per-scan
cost by under 0.5%: it is NOT IDENTIFIABLE, and its fitted 2 ns/candidate is an
artefact. Correlation with the scan count is only 0.55-0.81, so this is not
plain collinearity -- entryMs genuinely does not track how many candidates a
scan rejects. Printing both models side by side is what makes that visible
instead of letting a plausible coefficient be quoted.

READ THE DELTA FIRST. If `scans + candidates` does not beat `scans only` by a
real margin, the candidate share below is not a measurement and must not be
reported as one.
"""

import re, io, sys, math


SUM=re.compile(r'Voxel recompute \(sum since last log\): totalMs=([\d.]+) .*?\| entryMs (.*)')
MAX=re.compile(r'Voxel recompute \(max since last log\):.*?\| scans (R0=\d+.*?) \| tracked')
ADM=re.compile(r'Voxel admission detail \(5s window\): (.*)')
E=re.compile(r'R(\d)=([\d.]+)')
S=re.compile(r'R(\d)=(\d+)')
A=re.compile(r'R(\d)\[cut=(-?[\d.]+) rejB=(\d+) rejC=(\d+) rejF=(\d+) rejN=(\d+)')
def load(path):
    ent=[];scn=[];rej=[]
    for line in io.open(path,errors='ignore'):
        m=SUM.search(line)
        if m:
            v=[0.0]*7
            for l,x in E.findall(m.group(2)): v[int(l)]=float(x)
            ent.append((float(m.group(1)),v)); continue
        m=MAX.search(line)
        if m:
            v=[0]*7
            for l,x in S.findall(m.group(1)): v[int(l)]=int(x)
            scn.append(v); continue
        m=ADM.search(line)
        if m:
            v=[0]*7
            for l,c,b,cc,f,n in A.findall(m.group(1)): v[int(l)]=int(b)+int(cc)+int(f)+int(n)
            rej.append(v)
    return ent,scn,rej
rows=[]
for p in sys.argv[1:]:
    e,s,r=load(p)
    for w in range(min(len(e),len(s),len(r))):
        tot,ent=e[w]
        if tot<=0: continue
        for L in range(7):
            if ent[L]<=0.0 or s[w][L]<=0: continue
            rows.append((L,s[w][L],r[w][L],ent[L]))
def corr(xs,ys):
    n=len(xs)
    if n<3: return float('nan')
    mx=sum(xs)/n; my=sum(ys)/n
    sx=math.sqrt(sum((x-mx)**2 for x in xs)); sy=math.sqrt(sum((y-my)**2 for y in ys))
    if sx==0 or sy==0: return float('nan')
    return sum((x-mx)*(y-my) for x,y in zip(xs,ys))/(sx*sy)
print("COLLINEARITY: corr(scans, candidates) within each level")
for L in sorted(set(x[0] for x in rows)):
    s=[x[1] for x in rows if x[0]==L]; c=[x[2] for x in rows if x[0]==L]
    cps=[x[2]/x[1] for x in rows if x[0]==L]
    m=sum(cps)/len(cps); sd=math.sqrt(sum((v-m)**2 for v in cps)/len(cps))
    print("   R%d  n=%4d  r=%+.3f   candidates/scan mean=%9.0f  sd/mean=%.2f"%(L,len(s),corr(s,c),m,sd/m if m else 0))
print()
print("MODEL COMPARISON (does the candidate term earn its place?)")
def fit(use_cand):
    lv=sorted(set(x[0] for x in rows)); k=len(lv)+(1 if use_cand else 0); idx={L:i for i,L in enumerate(lv)}
    A=[[0.0]*k for _ in range(k)]; b=[0.0]*k
    for L,sc,cd,ms in rows:
        x=[0.0]*k; x[idx[L]]=float(sc)
        if use_cand: x[k-1]=float(cd)
        for i in range(k):
            b[i]+=x[i]*ms
            for j in range(k): A[i][j]+=x[i]*x[j]
    n=k; M=[A[i][:]+[b[i]] for i in range(n)]
    for c in range(n):
        pv=max(range(c,n),key=lambda r:abs(M[r][c]))
        M[c],M[pv]=M[pv],M[c]
        for r in range(n):
            if r==c: continue
            f=M[r][c]/M[c][c]
            for kk in range(c,n+1): M[r][kk]-=f*M[c][kk]
    co=[M[i][n]/M[i][i] for i in range(n)]
    sse=0.0
    for L,sc,cd,ms in rows:
        pm=co[idx[L]]*sc+(co[k-1]*cd if use_cand else 0.0); sse+=(ms-pm)**2
    mean=sum(x[3] for x in rows)/len(rows); sst=sum((x[3]-mean)**2 for x in rows)
    return co,idx,1-sse/sst
c1,i1,r1=fit(False); c2,i2,r2=fit(True)
print("   scans only          R2=%.4f"%r1)
print("   scans + candidates  R2=%.4f   (delta %+.4f)"%(r2,r2-r1))
print()
print("   per-scan cost, ms, both models:")
for L in sorted(i1):
    print("     R%d   scans-only=%7.3f   with-cand=%7.3f   diff=%+.1f%%"%(L,c1[i1[L]],c2[i2[L]],100*(c2[i2[L]]-c1[i1[L]])/c1[i1[L]]))
