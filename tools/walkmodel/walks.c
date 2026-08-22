// float32 mirror of the flat walk and the v2 skip walk, to find the residual
// diagonal disagreement by construction. Structure follows the HLSL line for
// line; floats are float, exactly as on the GPU.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#define VOX      10.0f
#define CELLV    8
#define CELL     (CELLV*VOX)
#define NOCROSS  1e30f
#define INSET    0.05f
#define BACKOFF  (0.5f*VOX)
#define DIM      64
#define CDIM     (DIM/CELLV)

static unsigned char g_solid[DIM*DIM*DIM];
static unsigned char g_mip[CDIM*CDIM*CDIM];
static float g_extent = DIM*VOX;

static int vox_solid(int x,int y,int z){
    if(x<0||y<0||z<0||x>=DIM||y>=DIM||z>=DIM) return 1;
    return g_solid[(z*DIM+y)*DIM+x];
}
static int cell_solid(int x,int y,int z){
    if(x<0||y<0||z<0||x>=CDIM||y>=CDIM||z>=CDIM) return 1;
    return g_mip[(z*CDIM+y)*CDIM+x];
}

typedef struct { int hit,inside,exhausted; int v[3]; float t; } Hit;

/* mirror of VoxelFluidWalkVoxelLine */
static Hit flat_walk(const float p0[3], const float p1[3], int budget){
    Hit o; memset(&o,0,sizeof(o));
    int ls[3], v[3];
    for(int a=0;a<3;a++){ ls[a]=(int)floorf(p0[a]/VOX); v[a]=ls[a]; }
    memcpy(o.v,v,sizeof(v));
    if(vox_solid(v[0],v[1],v[2])){ o.hit=1;o.inside=1;o.t=0.f; return o; }
    float d[3],tMax[3],tDelta[3]; int step[3];
    for(int a=0;a<3;a++) d[a]=p1[a]-p0[a];
    for(int a=0;a<3;a++){
        if(d[a]>0.f){ step[a]=1;  tMax[a]=(((float)ls[a]+1.f)*VOX-p0[a])/d[a]; tDelta[a]=VOX/d[a]; }
        else if(d[a]<0.f){ step[a]=-1; tMax[a]=((float)ls[a]*VOX-p0[a])/d[a];  tDelta[a]=-VOX/d[a]; }
        else { step[a]=0; tMax[a]=NOCROSS; tDelta[a]=NOCROSS; }
    }
    float tLast=0.f;
    for(int i=0;i<budget;i++){
        int axis = (tMax[0]<=tMax[1]&&tMax[0]<=tMax[2])?0:((tMax[1]<=tMax[2])?1:2);
        if(tMax[axis]>1.f){ memcpy(o.v,v,sizeof(v)); o.t=tLast; return o; }
        tLast=tMax[axis]; v[axis]+=step[axis]; tMax[axis]+=tDelta[axis];
        if(vox_solid(v[0],v[1],v[2])){ o.hit=1; memcpy(o.v,v,sizeof(v)); o.t=tLast; return o; }
    }
    o.exhausted=1; memcpy(o.v,v,sizeof(v)); o.t=tLast; return o;
}

static float clampf(float x,float lo,float hi){ return x<lo?lo:(x>hi?hi:x); }

/* mirror of VoxelMarchSpikeWalkL1Span, TFrom=0 TTo=1 */
static Hit skip_walk(const float entry[3], const float exitp[3], int budget){
    Hit miss; memset(&miss,0,sizeof(miss));
    float seg[3]; for(int a=0;a<3;a++) seg[a]=exitp[a]-entry[a];
    float sl = sqrtf(seg[0]*seg[0]+seg[1]*seg[1]+seg[2]*seg[2]);
    if(sl<1e-4f) sl=1e-4f;
    const float backT = BACKOFF/sl;
    const float hi = g_extent-INSET;
    float start[3]; for(int a=0;a<3;a++) start[a]=clampf(entry[a],INSET,hi);

    int cell[3]; float tMax[3],tDelta[3]; int step[3]; float tcur=0.f;
    for(int a=0;a<3;a++) cell[a]=(int)floorf(start[a]/CELL);
    for(int a=0;a<3;a++){
        if(seg[a]>0.f){ step[a]=1;  tMax[a]=0.f+(((float)cell[a]+1.f)*CELL-start[a])/seg[a]; tDelta[a]=CELL/seg[a]; }
        else if(seg[a]<0.f){ step[a]=-1; tMax[a]=0.f+((float)cell[a]*CELL-start[a])/seg[a];  tDelta[a]=-CELL/seg[a]; }
        else { step[a]=0; tMax[a]=NOCROSS; tDelta[a]=NOCROSS; }
    }
    int steps=0;
    for(int it=0; it<224; it++){
        if(steps>=budget){ miss.exhausted=1; miss.t=tcur; return miss; }
        int axis = (tMax[0]<=tMax[1]&&tMax[0]<=tMax[2])?0:((tMax[1]<=tMax[2])?1:2);
        float tCellExit = tMax[axis]; if(tCellExit>1.f) tCellExit=1.f;
        steps++;
        if(cell_solid(cell[0],cell[1],cell[2])){
            float tsub = tcur-backT; if(tsub<0.f) tsub=0.f;
            float se[3], sx[3];
            for(int a=0;a<3;a++){ se[a]=clampf(entry[a]+seg[a]*tsub,INSET,hi);
                                  sx[a]=clampf(entry[a]+seg[a]*tCellExit,INSET,hi); }
            Hit h = flat_walk(se,sx,budget);
            steps++;
            if(h.hit||h.exhausted){ h.t = tsub+(tCellExit-tsub)*h.t; return h; }
        }
        if(tMax[axis]>=1.f) return miss;
        tcur=tMax[axis]; cell[axis]+=step[axis]; tMax[axis]+=tDelta[axis];
    }
    miss.exhausted=1; miss.t=tcur; return miss;
}

static uint32_t rs=0x12345678u;
static uint32_t xr(){ rs^=rs<<13; rs^=rs>>17; rs^=rs<<5; return rs; }
static float frand(){ return (float)(xr()>>8)/(float)(1u<<24); }
static float grand(){ float s=0; for(int i=0;i<6;i++) s+=frand(); return s-3.f; }

int main(int argc,char**argv){
    long long N = (argc>1)? atoll(argv[1]) : 2000000LL;
    int mode = (argc>2)? atoi(argv[2]) : 1;   /* 1 = edge-aimed, 0 = random */
    rs = (argc>3)? (uint32_t)atoi(argv[3]) : 0x12345678u;

    for(int i=0;i<CDIM*CDIM*CDIM;i++){
        if(frand()<0.35f){
            int cx=i%CDIM, cy=(i/CDIM)%CDIM, cz=i/(CDIM*CDIM);
            for(int dz=0;dz<CELLV;dz++)for(int dy=0;dy<CELLV;dy++)for(int dx=0;dx<CELLV;dx++)
                if(frand()<0.30f) g_solid[((cz*CELLV+dz)*DIM+(cy*CELLV+dy))*DIM+(cx*CELLV+dx)]=1;
        }
    }
    for(int z=0;z<DIM;z++)for(int y=0;y<DIM;y++)for(int x=0;x<DIM;x++)
        if(g_solid[(z*DIM+y)*DIM+x]) g_mip[((z/CELLV)*CDIM+(y/CELLV))*CDIM+(x/CELLV)]=1;

    long long rays=0, mm=0, hist[8]={0};
    for(long long it=0; it<N; it++){
        float o[3],d[3],thr[3];
        if(mode){
            int cx=1+(int)(frand()*(CDIM-1)), cy=1+(int)(frand()*(CDIM-1));
            float eps[6]={0.f,1e-7f,-1e-7f,1e-5f,-1e-5f,1e-3f};
            thr[0]=cx*CELL+eps[xr()%6]; thr[1]=cy*CELL+eps[xr()%6];
            thr[2]=CELL+frand()*(g_extent-2*CELL);
            d[0]=-(fabsf(grand())+0.05f); d[1]=fabsf(grand())+0.05f; d[2]=grand()*0.4f;
        } else {
            thr[0]=frand()*g_extent; thr[1]=frand()*g_extent; thr[2]=frand()*g_extent;
            d[0]=grand(); d[1]=grand(); d[2]=grand();
        }
        float L=sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); if(L<1e-6f) continue;
        for(int a=0;a<3;a++) d[a]/=L;
        float back = 80.f+frand()*320.f;
        for(int a=0;a<3;a++) o[a]=thr[a]-d[a]*back;
        float te=0.f, tx=1e30f; int ok=1;
        for(int a=0;a<3;a++){
            if(fabsf(d[a])<1e-9f){ if(o[a]<0.f||o[a]>g_extent){ok=0;break;} continue; }
            float t0=(0.f-o[a])/d[a], t1=(g_extent-o[a])/d[a];
            if(t0>t1){float s=t0;t0=t1;t1=s;}
            if(t0>te) te=t0; if(t1<tx) tx=t1;
        }
        if(!ok||tx<=te) continue;
        float hi=g_extent-INSET, entry[3],exitp[3];
        for(int a=0;a<3;a++){ entry[a]=clampf(o[a]+d[a]*te,INSET,hi);
                              exitp[a]=clampf(o[a]+d[a]*tx,INSET,hi); }
        Hit f=flat_walk(entry,exitp,886);
        Hit s=skip_walk(entry,exitp,886);
        rays++;
        int diff = (f.hit!=s.hit);
        int l1=0;
        if(!diff && f.hit){ for(int a=0;a<3;a++) l1+=abs(f.v[a]-s.v[a]); diff = (l1!=0); }
        if(diff){
            mm++;
            int b = (f.hit!=s.hit)?7:(l1<7?l1:6);
            hist[b]++;
            if(mm<=6) printf("  flat=(%d,%d,%d)%s skip=(%d,%d,%d)%s L1=%d d=(%d,%d,%d)\n",
                f.v[0],f.v[1],f.v[2], f.inside?"[in]":"", s.v[0],s.v[1],s.v[2], s.inside?"[in]":"",
                l1, f.v[0]-s.v[0], f.v[1]-s.v[1], f.v[2]-s.v[2]);
        }
    }
    printf("mode=%d rays=%lld mismatches=%lld  (L1: 1=%lld 2=%lld 3=%lld 4+=%lld hitness=%lld)\n",
           mode, rays, mm, hist[1],hist[2],hist[3],hist[4]+hist[5]+hist[6],hist[7]);
    return 0;
}
