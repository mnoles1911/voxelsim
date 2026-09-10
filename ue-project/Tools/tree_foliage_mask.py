"""Approved spring tree leaf shapes, shared by pilot and editable materials."""
SHAPED_MASK_BODY = r'''
float scale=lerp(.19,.13,Needle);
float2 p=UV/scale;
float2 base=floor(p);
float d=10.0;
for(int x=-1;x<=1;x++)for(int y=-1;y<=1;y++){
    float2 cell=base+float2(x,y);
    float h0=frac(sin(dot(cell,float2(127.1,311.7)))*43758.5453);
    float h1=frac(sin(dot(cell+19.,float2(127.1,311.7)))*43758.5453);
    float h2=frac(sin(dot(cell+7.,float2(127.1,311.7)))*43758.5453);
    float2 q=p-cell-.5-float2(h0-.5,h1-.5)*.48;
    float angle=(h2-.5)*2.6;
    for(int i=-2;i<=2;i++){
        if(Needle<.5 && i!=0)continue;
        float2 v=q-(Needle>.5?float2(i*.08,abs(i)*.10):float2(0,0));
        float a=angle+(Needle>.5?i*.28:0.0);
        // Matches GLSL's column-major mat2 rotation in the web pilot.
        v=float2(cos(a)*v.x+sin(a)*v.y,-sin(a)*v.x+cos(a)*v.y);
        float t=v.y/.62;
        float width=Needle>.5?lerp(.11,.055,Opening):lerp(.40,.12,Opening);
        d=min(d,max(abs(t),abs(v.x)/(width*max(.06,1-t*t))));
    }
}
float footprint=max(length(ddx(p)),length(ddy(p)));
float detail=1-smoothstep(.65,1.8,footprint);
float FoliageCoverage=lerp(1,1-step(1,d),detail);
'''
