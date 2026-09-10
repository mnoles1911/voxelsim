import { createViewer, decodeVoxels } from './lib/appearance-viewer';

document.head.insertAdjacentHTML('beforeend',`<style>
body{margin:0;background:#18211d;color:#e9eee5;font:15px system-ui}main{max-width:1500px;margin:auto;padding:24px}h1{margin:14px 0 8px}a{color:#b8d696}header{display:flex;gap:14px;align-items:center;flex-wrap:wrap}button,select{font:inherit;color:inherit;background:#334738;border:1px solid #667c65;padding:10px;border-radius:5px}button:disabled{opacity:.4}canvas{width:100%;height:66vh;touch-action:none;display:block;background:#899faa}.frame{border:1px solid #526451;border-radius:8px;overflow:hidden;margin:16px 0}.bar{padding:12px;background:#263a2e;display:flex;justify-content:space-between;gap:16px;flex-wrap:wrap}.muted{color:#b6c5b6}.warning{background:#3b382a;padding:12px;border-left:3px solid #cabb7a}input{vertical-align:middle}#status{min-height:24px}
</style>`);
document.querySelector('#app')!.innerHTML=`<main><a href="/?tab=forge">← Forge</a><h1>Tree appearance pilot</h1><p id="collection-summary">24 saved models · 100 mm voxels · existing geometry and decisions preserved</p><header><label>Species <select id="species"></select></label><label>Size <select id="seed"><option value="1">Small · seed 1</option><option value="4">Medium · seed 4</option><option value="7" selected>Large · seed 7</option></select></label><label>Comparison <select id="mode"><option value="baseline">Current colors · opaque</option><option value="color">Species colors · opaque</option><option value="mask" selected>Species colors · shaped foliage</option></select></label><button id="next">Next model</button></header><div class="frame"><div class="bar"><span id="status" role="status">Loading…</span><span><button id="bark">Bark close-up</button> <button id="whole">Whole tree</button> <button id="zoom">Closer</button> <button id="out">Farther</button> <button id="reset">Reset view</button></span></div><canvas id="view" aria-label="Interactive appearance pilot"></canvas><div class="bar"><label>Color variation <input id="variation" type="range" min="0" max="150" value="100"></label><label>Foliage openness <input id="opening" type="range" min="0" max="100" value="45"></label><label><input id="pawn" type="checkbox" checked> Player pawn · 1.8 m</label><span>Drag to orbit · scroll to zoom</span></div></div><p class="warning">Review prototype: shaped leaf outlines replace random square holes. Masks still act on voxel surfaces, not individual free-standing leaves. Game rendering, shadows and performance have not yet been validated for this pilot.</p><p class="muted">Bark pattern squares are whole solid-color voxel faces. Only foliage has sub-voxel cutouts. Color patches and branch-junction assignments are provisional; this viewer does not change the saved asset library.</p></main>`;
type Model={id:string;species:string;seed:number;needle:boolean;voxels:number};
const select=(id:string)=>document.getElementById(id) as HTMLSelectElement;
const species=select('species'),seed=select('seed'),mode=select('mode');
const status=document.getElementById('status')!;
const opening=document.getElementById('opening') as HTMLInputElement;
const viewer=createViewer(document.getElementById('view') as HTMLCanvasElement);
let models:Model[]=[],token=0;
let current:Model|undefined;
let shownId='';
let decoded:ReturnType<typeof decodeVoxels>|undefined,foliage:Float32Array|undefined;
let baseline:Uint8Array|undefined,colored:Uint8Array|undefined;
const collection=new URLSearchParams(location.search).get('collection')==='1';
const base=collection?'/static/tree-appearance-collection-v2/':'/static/tree-appearance-pilot/';
async function bytes(url:string){const r=await fetch(url);if(!r.ok)throw Error(`Could not load ${url}: ${r.status}`);return r.arrayBuffer();}
function display(){
 if(!viewer||!decoded||!current||!baseline||!colored)return;
 const colors=mode.value==='baseline'?baseline:colored;
 if(shownId!==current.id){viewer.setInstances(decoded.offsets,colors,decoded.dims,10,foliage);shownId=current.id;}
 else viewer.setColors(colors);
 viewer.setVariation(mode.value==='baseline'?0:Number((document.getElementById('variation') as HTMLInputElement).value)/100);
 viewer.setMask(mode.value==='mask',current.needle,Number(opening.value)/100);
}
async function load(){
 const request=++token;const model=models.find(m=>m.species===species.value&&m.seed===Number(seed.value));
 if(!model)return;
 status.textContent='Loading saved model…';
 try{
  const [vox,a,b]=await Promise.all(['voxels.bin','baseline.rgb','species.rgb'].map(f=>bytes(base+model.id+'/'+f)));
  if(request!==token)return;
  decoded=decodeVoxels(vox,{});current=model;baseline=new Uint8Array(a);colored=new Uint8Array(b);
  const n=decoded.count;const mats=new Uint8Array(vox,16+n*6,n);
  foliage=Float32Array.from(mats,m=>[19,20,21,22,24,25].includes(m)?1:0);
  display();status.textContent=`${model.species.replaceAll('-',' ')} · seed ${model.seed} · ${n.toLocaleString()} visible cells`;
 }catch(e){status.textContent=String(e);}
}
document.getElementById('variation')!.oninput=display;
function selectSpecies(){const values=models.filter(m=>m.species===species.value).map(m=>m.seed);const old=Number(seed.value);seed.replaceChildren(...values.map(n=>new Option((n===1?'Small':n===4?'Medium':n===7?'Large':'Variant')+' · seed '+n,String(n))));seed.value=String(values.includes(old)?old:values[0]);void load();}
species.onchange=selectSpecies;seed.onchange=()=>void load();mode.onchange=display;opening.oninput=display;
document.getElementById('bark')!.onclick=()=>{
 if(!decoded||!foliage)return;
 const points=decoded.offsets;let x=0,y=0,count=0,low=Infinity;
 for(let i=0;i<foliage.length;i++)if(!foliage[i])low=Math.min(low,points[i*3+2]);
 for(let i=0;i<foliage.length;i++)if(!foliage[i]&&points[i*3+2]<low+20){x+=points[i*3];y+=points[i*3+1];count++;}
 if(count)viewer?.frame([x/count,y/count,low+15],48);
};
document.getElementById('whole')!.onclick=()=>{shownId='';display();viewer?.reset();};
document.getElementById('zoom')!.onclick=()=>viewer?.zoom(.6);
document.getElementById('out')!.onclick=()=>viewer?.zoom(1.5);
document.getElementById('reset')!.onclick=()=>viewer?.reset();
(document.getElementById('pawn') as HTMLInputElement).onchange=e=>viewer?.setPawnVisible((e.target as HTMLInputElement).checked);
document.getElementById('next')!.onclick=()=>{const m=models[(models.findIndex(m=>m.id===current?.id)+1)%models.length];species.value=m.species;seed.value=String(m.seed);void load();};
fetch(base+'manifest.json').then(r=>{if(!r.ok)throw Error('Pilot manifest unavailable');return r.json();}).then(data=>{
 models=data.models;species.replaceChildren(...[...new Set(models.map(m=>m.species))].map(s=>new Option(s.replaceAll('-',' '),s)));
 document.getElementById('collection-summary')!.textContent=`${models.length} saved models · 100 mm voxels · existing geometry and decisions preserved${collection?' · staged palette expansion':''}`;
 if(!viewer)throw Error('WebGL2 is unavailable');selectSpecies();
}).catch(e=>status.textContent=String(e));
