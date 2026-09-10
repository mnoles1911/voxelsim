(function(){const t=document.createElement("link").relList;if(t&&t.supports&&t.supports("modulepreload"))return;for(const r of document.querySelectorAll('link[rel="modulepreload"]'))c(r);new MutationObserver(r=>{for(const a of r)if(a.type==="childList")for(const s of a.addedNodes)s.tagName==="LINK"&&s.rel==="modulepreload"&&c(s)}).observe(document,{childList:!0,subtree:!0});function i(r){const a={};return r.integrity&&(a.integrity=r.integrity),r.referrerPolicy&&(a.referrerPolicy=r.referrerPolicy),r.crossOrigin==="use-credentials"?a.credentials="include":r.crossOrigin==="anonymous"?a.credentials="omit":a.credentials="same-origin",a}function c(r){if(r.ep)return;r.ep=!0;const a=i(r);fetch(r.href,a)}})();const q=`#version 300 es
in vec3 aPos;
in vec3 aNormal;
in vec3 aOffset;
in vec3 aColor;
in float aFoliage;
uniform float uVariation;
out float vFoliage;
out vec3 vLocal;
uniform mat4 uMVP;
uniform bool uPawn;
uniform vec3 uPartSize;
uniform vec3 uPartOffset;
uniform vec3 uPartColor;
out vec3 vColor;
out vec3 vNormal;
uint colorHash(uint h) { h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; return h ^ (h >> 16); }
void main() {
  gl_Position = uMVP * vec4(uPawn ? aPos * uPartSize + uPartOffset : aPos + aOffset, 1.0);
  vColor = uPawn ? uPartColor : aColor;
  if (!uPawn && uVariation > 0.0) {
    uvec3 c=uvec3(ivec3(round(aOffset)));
    uint face=abs(aNormal.x)>.5?0u:abs(aNormal.y)>.5?2u:4u;
    face += (aNormal.x+aNormal.y+aNormal.z)>0.0?1u:0u;
    uint key=c.x*73856093u ^ c.y*19349663u ^ c.z*83492791u;
    float voxel=float(colorHash(key)&65535u)/65535.0*2.0-1.0;
    float side=float(colorHash(key ^ ((face+1u)*2654435761u))&65535u)/65535.0*2.0-1.0;
    float warmth=float(colorHash(key ^ 1597334677u)&65535u)/65535.0*2.0-1.0;
    float gain=1.0+uVariation*(.085*voxel+.045*side);
    vColor=clamp(aColor*gain*(vec3(1.0)+uVariation*warmth*vec3(.025,0,-.025)),0.0,1.0);
  }
  vNormal = aNormal;
  vLocal = aPos + aOffset;
  vFoliage = uPawn ? 0.0 : aFoliage;
}`,X=`#version 300 es
precision highp float;
in vec3 vColor;
in vec3 vNormal;
uniform vec3 uLight;
in float vFoliage;
in vec3 vLocal;
uniform float uMask;
uniform float uNeedle;
uniform float uPitch;
uniform float uOpening;
float h(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float blade(vec2 p,float a,float width){
  p=mat2(cos(a),-sin(a),sin(a),cos(a))*p;
  float t=p.y/.62;
  return max(abs(t),abs(p.x)/(width*max(.06,1.0-t*t)));
}
float foliageMask(vec2 uv){
  float scale=mix(.19,.13,uNeedle);
  vec2 p=uv/scale;
  vec2 base=floor(p);
  float d=10.0;
  for(int x=-1;x<=1;x++)for(int y=-1;y<=1;y++){
    vec2 cell=base+vec2(x,y);
    vec2 centre=cell+.5+vec2(h(cell)-.5,h(cell+19.)-.5)*.48;
    float angle=(h(cell+7.)-.5)*2.6;
    vec2 q=p-centre;
    if(uNeedle<.5){
      d=min(d,blade(q,angle,mix(.40,.12,uOpening)));
    }else{
      for(int i=-2;i<=2;i++)d=min(d,blade(q-vec2(float(i)*.08,abs(float(i))*.10),angle+float(i)*.28,mix(.11,.055,uOpening)));
    }
  }
  float footprint=max(length(dFdx(p)),length(dFdy(p)));
  // Pilot fallback only: distant masks close gradually. Game LOD validation pending.
  float detail=1.0-smoothstep(.65,1.8,footprint);
  return mix(1.0,1.0-step(1.0,d),detail);
}
out vec4 frag;
void main() {
  vec3 n = normalize(vNormal);
  if(uMask>.5 && vFoliage>.5){
    vec3 p=vLocal*uPitch;
    vec2 uv=abs(n.z)>.5?p.xy:abs(n.x)>.5?p.yz:p.xz;
    if(foliageMask(uv)<.5)discard;
  }
  float key = max(dot(n, uLight), 0.0);
  float fill = max(dot(n, vec3(-0.3, -0.4, -0.2)), 0.0);
  frag = vec4(vColor * (0.42 + 0.62 * key + 0.12 * fill), 1.0);
}`,K=[{min:[-.2,-.2,.8],size:[.4,.4,.7],color:[.2,.35,.55]},{min:[-.16,-.15,1.52],size:[.32,.3,.28],color:[.85,.7,.55]},{min:[-.14,-.3,.75],size:[.28,.14,.7],color:[.55,.3,.15]},{min:[-.14,.16,.75],size:[.28,.14,.7],color:[.55,.3,.15]},{min:[-.14,-.21,0],size:[.28,.2,.8],color:[.55,.3,.15]},{min:[-.14,.01,0],size:[.28,.2,.8],color:[.55,.3,.15]}];function W(){const e=[[[0,0,1],[[0,0,1],[1,0,1],[1,1,1],[0,1,1]]],[[0,0,-1],[[0,1,0],[1,1,0],[1,0,0],[0,0,0]]],[[1,0,0],[[1,0,0],[1,1,0],[1,1,1],[1,0,1]]],[[-1,0,0],[[0,0,1],[0,1,1],[0,1,0],[0,0,0]]],[[0,1,0],[[0,1,1],[1,1,1],[1,1,0],[0,1,0]]],[[0,-1,0],[[0,0,0],[1,0,0],[1,0,1],[0,0,1]]]],t=[],i=[];for(const[c,r]of e)for(const a of[0,1,2,0,2,3])t.push(...r[a]),i.push(...c);return{pos:new Float32Array(t),nrm:new Float32Array(i)}}function j(e,t,i,c){const r=1/Math.tan(e/2),a=1/(i-c);return new Float32Array([r/t,0,0,0,0,r,0,0,0,0,(c+i)*a,-1,0,0,2*c*i*a,0])}const J=(e,t)=>[e[0]-t[0],e[1]-t[1],e[2]-t[2]],S=(e,t)=>e[0]*t[0]+e[1]*t[1]+e[2]*t[2],B=(e,t)=>[e[1]*t[2]-e[2]*t[1],e[2]*t[0]-e[0]*t[2],e[0]*t[1]-e[1]*t[0]];function T(e){const t=Math.hypot(e[0],e[1],e[2])||1;return[e[0]/t,e[1]/t,e[2]/t]}function Q(e,t,i){const c=T(J(e,t)),r=T(B(i,c)),a=B(c,r);return new Float32Array([r[0],a[0],c[0],0,r[1],a[1],c[1],0,r[2],a[2],c[2],0,-S(r,e),-S(a,e),-S(c,e),1])}function Z(e,t){const i=new Float32Array(16);for(let c=0;c<4;c++)for(let r=0;r<4;r++){let a=0;for(let s=0;s<4;s++)a+=e[s*4+r]*t[c*4+s];i[c*4+r]=a}return i}function $(e,t,i){const c=e.createProgram(),r=[[e.VERTEX_SHADER,t],[e.FRAGMENT_SHADER,i]];for(const[a,s]of r){const d=e.createShader(a);if(e.shaderSource(d,s.trim()),e.compileShader(d),!e.getShaderParameter(d,e.COMPILE_STATUS))throw new Error(e.getShaderInfoLog(d)??"shader");e.attachShader(c,d)}if(e.linkProgram(c),!e.getProgramParameter(c,e.LINK_STATUS))throw new Error(e.getProgramInfoLog(c)??"link");return c}function _(e,t,i,c,r){const a=e.createBuffer();e.bindBuffer(e.ARRAY_BUFFER,a),e.bufferData(e.ARRAY_BUFFER,c,e.STATIC_DRAW);const s=e.getAttribLocation(t,i);e.enableVertexAttribArray(s),e.vertexAttribPointer(s,r,e.FLOAT,!1,0,0)}function z(e,t,i,c,r,a,s,d){e.bindBuffer(e.ARRAY_BUFFER,c),e.bufferData(e.ARRAY_BUFFER,r,e.STATIC_DRAW);const m=e.getAttribLocation(t,i);e.enableVertexAttribArray(m),e.vertexAttribPointer(m,a,s,d,0,0),e.vertexAttribDivisor(m,1)}function tt(e){const t=e.getContext("webgl2",{antialias:!0});if(!t)return null;const i=$(t,q,X),c=W(),r=t.createVertexArray();t.bindVertexArray(r),_(t,i,"aPos",c.pos,3),_(t,i,"aNormal",c.nrm,3);const a=t.createBuffer(),s=t.createBuffer(),d=t.createBuffer();let m=!1,g=!1,M=.45,x=1;t.enable(t.DEPTH_TEST),t.enable(t.CULL_FACE),t.cullFace(t.BACK);const f=t.getUniformLocation(i,"uMVP"),b=t.getUniformLocation(i,"uLight"),w=t.getUniformLocation(i,"uPawn"),k=t.getUniformLocation(i,"uPartSize"),D=t.getUniformLocation(i,"uPartOffset"),Y=t.getUniformLocation(i,"uPartColor");let C=!0,N=[0,0,0],h=0;const o={count:0,center:[0,0,0],radius:100,home:100,azimuth:-.9,elevation:.45,dirty:!0};function H(n,u,l,p,A){t.bindVertexArray(r),z(t,i,"aOffset",a,n,3,t.FLOAT,!1),z(t,i,"aColor",s,u,3,t.UNSIGNED_BYTE,!0),z(t,i,"aFoliage",d,A||new Float32Array(n.length/3),1,t.FLOAT,!1);const U=o.count>0?o.radius/o.home:1;o.count=n.length/3,h=p&&Number.isFinite(p)&&p>0?100/p:0;let L=1/0,P=-1/0;for(let F=0;F<n.length;F+=3)P=Math.max(P,n[F]+1),L=Math.min(L,n[F+2]);N=[P+.8*h,l[1]/2,L],o.center=[l[0]/2,l[1]/2,l[2]/2];const v=h&&o.count?[Math.max(l[0],N[0]+.3*h),l[1],Math.max(l[2],L+1.8*h)]:l;o.center=[v[0]/2,v[1]/2,v[2]/2],o.home=Math.max(...v)*1.5,o.radius=o.home*U,o.dirty=!0}function G(){const n=Math.min(window.devicePixelRatio||1,2),u=Math.max(1,Math.floor(e.clientWidth*n)),l=Math.max(1,Math.floor(e.clientHeight*n));if((e.width!==u||e.height!==l)&&(e.width=u,e.height=l,o.dirty=!0),!o.dirty||!o.count)return;o.dirty=!1,t.viewport(0,0,u,l),t.clearColor(.54,.66,.73,1),t.clear(t.COLOR_BUFFER_BIT|t.DEPTH_BUFFER_BIT);const p=Math.cos(o.elevation),A=[o.center[0]+o.radius*p*Math.cos(o.azimuth),o.center[1]+o.radius*p*Math.sin(o.azimuth),o.center[2]+o.radius*Math.sin(o.elevation)],U=Q(A,o.center,[0,0,1]),L=j(.7,u/l,.5,o.radius*8+500);if(t.useProgram(i),t.uniform1f(t.getUniformLocation(i,"uMask"),m?1:0),t.uniform1f(t.getUniformLocation(i,"uVariation"),x),t.uniform1f(t.getUniformLocation(i,"uNeedle"),g?1:0),t.uniform1f(t.getUniformLocation(i,"uOpening"),M),t.uniform1f(t.getUniformLocation(i,"uPitch"),1/h),t.uniformMatrix4fv(f,!1,Z(L,U)),t.uniform3fv(b,T([.45,.35,.82])),t.bindVertexArray(r),t.uniform1i(w,0),t.drawArraysInstanced(t.TRIANGLES,0,36,o.count),C&&h){t.uniform1i(w,1);for(const P of K)t.uniform3fv(k,P.size.map(v=>v*h)),t.uniform3fv(D,P.min.map((v,F)=>N[F]+v*h)),t.uniform3fv(Y,P.color),t.drawArraysInstanced(t.TRIANGLES,0,36,1)}}const y=new Map;let E=0;const R=()=>{const[n,u]=[...y.values()];return Math.hypot(n.x-u.x,n.y-u.y)},O=n=>Math.max(o.home*.15,Math.min(o.home*6,n));e.addEventListener("pointerdown",n=>{y.set(n.pointerId,{x:n.clientX,y:n.clientY});try{e.setPointerCapture(n.pointerId)}catch{}y.size===2&&(E=R())});const I=n=>{y.delete(n.pointerId);try{e.releasePointerCapture(n.pointerId)}catch{}y.size===2&&(E=R())};e.addEventListener("pointerup",I),e.addEventListener("pointercancel",I),e.addEventListener("pointermove",n=>{const u=y.get(n.pointerId);if(!u)return;const l=n.clientX-u.x,p=n.clientY-u.y;if(u.x=n.clientX,u.y=n.clientY,y.size===1)o.azimuth-=l*.008,o.elevation=Math.max(-1.5,Math.min(1.5,o.elevation+p*.006)),o.dirty=!0;else if(y.size===2){const A=R();E>0&&A>0&&(o.radius=O(o.radius*(E/A)),o.dirty=!0),E=A}}),e.addEventListener("wheel",n=>{n.preventDefault(),o.radius=O(o.radius*Math.exp(n.deltaY*.0012)),o.dirty=!0},{passive:!1}),e.addEventListener("dblclick",()=>{o.azimuth=-.9,o.elevation=.45,o.radius=o.home,o.dirty=!0});let V=requestAnimationFrame(function n(){G(),V=requestAnimationFrame(n)});return{setInstances:H,setColors(n){t.bindVertexArray(r),z(t,i,"aColor",s,n,3,t.UNSIGNED_BYTE,!0),o.dirty=!0},frame(n,u){o.center=n,o.radius=u,o.dirty=!0},setMask(n,u,l){m=n,g=u,M=l,o.dirty=!0},setVariation(n){x=n,o.dirty=!0},setPawnVisible(n){C=n,o.dirty=!0},reset(){o.azimuth=-.9,o.elevation=.45,o.radius=o.home,o.dirty=!0},zoom(n){o.radius=O(o.radius*n),o.dirty=!0},invalidate(){o.dirty=!0},dispose(){cancelAnimationFrame(V)}}}function et(e,t){const i=new Uint32Array(e,0,4),c=[i[0],i[1],i[2]],r=i[3],a=new Int16Array(e,16,r*3),s=new Uint8Array(e,16+r*6,r),d=new Float32Array(r*3),m=new Uint8Array(r*3),g=16+r*7,M=e.byteLength===g+4+r*3&&new DataView(e).getUint32(g,!1)===1380401713?new Uint8Array(e,g+4,r*3):null,x={};for(let f=0;f<r;f++){d[f*3]=a[f*3],d[f*3+1]=a[f*3+1],d[f*3+2]=a[f*3+2];const b=s[f];x[b]=(x[b]??0)+1;const w=M?M.subarray(f*3,f*3+3):t[String(b)]||[255,0,255];m[f*3]=w[0],m[f*3+1]=w[1],m[f*3+2]=w[2]}return{offsets:d,colors:m,dims:c,count:r,materialCounts:x}}export{tt as c,et as d};
