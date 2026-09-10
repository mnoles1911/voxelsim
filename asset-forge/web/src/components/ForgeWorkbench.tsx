import * as React from 'react';
import type { World } from '../App';
import type { ForgeRequest } from '../lib/schema';
import type { LibraryEntry } from '../lib/schema';
import { allowedBiomes } from '../lib/schema';
import { api } from '../lib/api';
import { Button } from './ui/button';
import { VoxelCanvas } from './VariantViewer';

const field='rounded border border-stone-600 bg-stone-900 p-2 text-sm text-parch-100';
type Run={id:string;species:string[];status:string;completed:number;total:number;message?:string;error?:string;failures?:{seed:number;error:string}[]};

export function ForgeWorkbench({world,request,onOpenLibrary}:{world:World;request:ForgeRequest|null;onOpenLibrary:(name:string)=>void}) {
  const label=(id:string)=>world.specs.find(s=>s.name===id)?.design?.display_name||id.replaceAll('-',' ');
  const [name,setName]=React.useState(new URLSearchParams(location.search).get('species')||'temperate-oak');
  const [query,setQuery]=React.useState('');
  const [category,setCategory]=React.useState('environment');
  const [kind,setKind]=React.useState('tree');
  const [biome,setBiome]=React.useState('all');
  const [seed,setSeed]=React.useState(1);
  const [count,setCount]=React.useState(36);
  const [filter,setFilter]=React.useState('all');
  const [inspectId,setInspectId]=React.useState<string|null>(null);
  const [busy,setBusy]=React.useState(false);
  const [error,setError]=React.useState('');
  const [runs,setRuns]=React.useState<Run[]>([]);
  const completion=React.useRef('');
  React.useEffect(()=>{if(request){setName(String(request.spec.name));setKind(String(request.spec.kind||''));setCategory('');setSeed(request.seedStart);}},[request]);
  React.useEffect(()=>{let alive=true;const poll=async()=>{try{const next=await api.generationRuns() as Run[];if(alive)setRuns(next);}catch(e){if(alive)setError(String(e));}};void poll();const timer=setInterval(()=>void poll(),3000);return()=>{alive=false;clearInterval(timer);};},[]);
  React.useEffect(()=>{const signature=runs.filter(r=>!['running','starting'].includes(r.status)).map(r=>r.id+r.status).join('|');if(signature!==completion.current){completion.current=signature;void world.refreshLibrary();}},[runs,world.refreshLibrary]);
  const species=world.specs.find(s=>s.name===name);
  const design=species?.design;
  const entries=world.variants.filter(e=>e.species===name).sort((a,b)=>a.seed-b.seed);
  const endorsed=entries.filter(e=>!e.inventory_candidate);
  const approvedCounts=React.useMemo(()=>{
    const counts=new Map<string,number>();
    for(const entry of world.variants) if(!entry.inventory_candidate) counts.set(entry.species,(counts.get(entry.species)||0)+1);
    return counts;
  },[world.variants]);
  const inspected=entries.find(e=>e.id===inspectId);
  const reference=entries.find(e=>e.id===design?.reference_variant_id);
  const displayed=inspected||reference||entries[0];
  const visible=entries.filter(e=>filter==='all'||(filter==='pending' ? e.inventory_candidate : !e.inventory_candidate));
  const catalog=world.specs.filter(s=>{
    if((category&&s.category!==category)||(kind&&s.kind!==kind)||!`${s.name.replaceAll('-',' ')} ${label(s.name)}`.toLowerCase().includes(query.toLowerCase().replaceAll('-',' ')))return false;
    if(biome==='all')return true;
    const allowed=allowedBiomes(s,world.biomes);
    return biome==='unassigned'?allowed.length===0:allowed.includes(biome);
  });
  const reviewOrder=catalog.flatMap(s=>world.variants.filter(e=>e.species===s.name).sort((a,b)=>a.seed-b.seed));
  const currentIndex=reviewOrder.findIndex(e=>e.id===displayed?.id);
  const nextPending=[...reviewOrder.slice(currentIndex+1),...reviewOrder.slice(0,Math.max(0,currentIndex))]
    .find(e=>e.inventory_candidate);
  const run=runs.find(r=>r.species?.includes(name));
  const active=runs.some(r=>['running','starting'].includes(r.status));
  const imported=entries.some(e=>e.imported);
  async function action(work:()=>Promise<unknown>){setBusy(true);setError('');try{await work();await Promise.all([world.refreshSpecs(),world.refreshLibrary()]);}catch(e){setError(String(e));}finally{setBusy(false);}}
  function choose(next:string){setName(next);setInspectId(null);setError('');}
  return <div className="flex min-h-0 flex-1 text-parch-200">
    <aside className="flex w-64 shrink-0 flex-col gap-3 border-r border-stone-700 bg-stone-850 p-4">
      <h1 className="font-display text-xl text-gold-400">Species & sources</h1>
      <input className={field} aria-label="Find species" placeholder="Find a species…" value={query} onChange={e=>setQuery(e.target.value)}/>
      <select className={field} aria-label="Asset category" value={category} onChange={e=>{setCategory(e.target.value);setKind('');}}><option value="">All assets</option><option value="environment">Environment</option><option value="creature">Creatures</option><option value="craftable">Craftable items</option></select>
      <select className={field} aria-label="Asset type" value={kind} onChange={e=>setKind(e.target.value)}><option value="">All types</option>{world.kinds.filter(k=>!category||k.category===category).map(k=><option key={k.key} value={k.key}>{k.label}</option>)}</select>
      <select className={field} aria-label="Biome" value={biome} onChange={e=>setBiome(e.target.value)}><option value="all">All biomes</option><option value="unassigned">No biome assigned</option>{world.biomes.filter(b=>b.hosts.length>0).map(b=><option key={b.key} value={b.key}>{b.label}</option>)}</select>
      <p className="text-xs text-parch-500">{catalog.length} matching species</p>
      <div className="min-h-0 flex-1 overflow-y-auto">{catalog.map(s=><button key={s.name} onClick={()=>choose(s.name)} className={'mb-1 block w-full rounded p-3 text-left text-sm '+(s.name===name?'bg-stone-600 text-gold-300':'hover:bg-stone-700')}><span className="flex items-center justify-between gap-2"><span className="capitalize">{label(s.name)}</span><span className="shrink-0 text-xs text-green-300">{approvedCounts.get(s.name)||0} approved</span></span><span className="text-xs text-parch-500">{s.design?.generator_approved?'Approved source':s.design?.review_status||'Source needs review'}</span></button>)}</div>
      <p className="text-xs text-parch-500">Species = source design. Seed = numbered input. Variant = generated asset.</p>
    </aside>
    <main className="min-w-0 flex-1 overflow-y-auto p-5">
      <div className="mb-4 flex flex-wrap items-center justify-between gap-3"><div><h2 className="font-display text-2xl capitalize text-gold-400">{label(name)}</h2><p className="text-sm text-parch-400">{species?.kind} · {entries.length} variants · {endorsed.length} endorsed</p></div><div className="flex gap-2"><Button disabled={busy||!nextPending} title={nextPending?`Next pending: ${label(nextPending.species)} · seed ${nextPending.seed}`:'No other pending variants in the current filters'} onClick={()=>{if(nextPending){choose(nextPending.species);setInspectId(nextPending.id);if(filter==='endorsed')setFilter('pending');}}}>Next</Button><Button onClick={()=>onOpenLibrary(name)}>Open species collection</Button></div></div>
      {error&&<p role="alert" className="mb-4 rounded border border-rust-600 p-3 text-rust-400">{error}</p>}
      <div className="grid gap-5 xl:grid-cols-[minmax(0,1fr)_320px]">
        <section className="overflow-hidden rounded border border-stone-700 bg-stone-900">
          <div className="flex items-center justify-between p-3 text-sm"><span>{displayed ? `${label(displayed.species)} · seed ${displayed.seed}`:'No saved preview yet'}</span><span className="text-gold-400">{displayed?.id===design?.reference_variant_id?'Authoritative reference':displayed?.inventory_candidate?'Pending review':displayed?'Endorsed variant':''}</span></div>
          {displayed?<VoxelCanvas src={api.voxelsUrl(displayed.id)} palette={world.palette} className="h-[430px]"/>:<div className="flex h-[430px] items-center justify-center text-parch-500">Generate candidates to inspect this source.</div>}
          {displayed&&<div className="flex flex-wrap gap-2 border-t border-stone-700 p-3">
            {!displayed.imported&&<Button size="sm" disabled={busy||displayed.id===design?.reference_variant_id} onClick={()=>void action(()=>api.setReference(displayed.id))}>Use as reference</Button>}
            {displayed.inventory_candidate&&<Button size="sm" variant="gold" disabled={busy} onClick={()=>void action(()=>api.keepInventory(displayed.id))}>Endorse variant</Button>}
            <Button size="sm" disabled={busy} onClick={()=>void action(async()=>{await api.rejectVariant(displayed.id);setInspectId(null);})}>Reject variant</Button>
            {reference&&displayed.id!==reference.id&&<Button size="sm" onClick={()=>setInspectId(reference.id)}>Show reference</Button>}
            <span className="ml-auto self-center text-xs text-parch-500">{displayed.stats?.height_m?.toFixed(1)} m tall · {(displayed.stats?.voxel_cm||species?.resolution_cm||10)*10} mm voxels</span>
          </div>}
        </section>
        <div className="space-y-4">
          <section className="rounded border border-stone-700 p-4"><h3 className="mb-2 font-display text-lg">Source design</h3><p className="text-sm text-gold-400">{design?.generator_approved?'Approved generator':design?.review_status||'Not reference-reviewed'}</p><p className="mt-2 text-sm text-parch-400">{design?.reference_notes||'This generator has not yet completed a documented species reference review. Generated candidates still need visual inspection.'}</p>{design&&<p className="mt-2 text-xs">Reference seed {design.reference_seed} · {design.baseline_current?'Current baseline':'Baseline changed — select a new reference'}</p>}{design?.sources?.map((url,i)=><a key={url} className="mt-2 block text-sm text-gold-400 underline" href={url} target="_blank" rel="noreferrer">Botanical reference {i+1}</a>)}{design?.baseline_current&&design.reference_available&&!design.generator_approved&&<Button className="mt-3" size="sm" disabled={busy} onClick={()=>void action(()=>api.approveGenerator(name))}>Approve source generator</Button>}</section>
          <section className="rounded border border-stone-700 p-4"><h3 className="mb-2 font-display text-lg">Generate variants</h3><p className="mb-3 text-xs text-parch-400">Runs locally from this species baseline. New outputs stay pending until endorsed.</p><div className="grid grid-cols-2 gap-3"><label className="text-xs">Starting seed<input className={field+' mt-1 w-full'} type="number" min="0" step="1" value={seed} onChange={e=>setSeed(Number(e.target.value))}/></label><label className="text-xs">Variant count<input className={field+' mt-1 w-full'} type="number" min="1" max="144" step="1" value={count} onChange={e=>setCount(Number(e.target.value))}/></label></div><Button variant="gold" className="mt-3 w-full" disabled={busy||active||imported||!species||!Number.isInteger(seed)||seed<0||!Number.isInteger(count)||count<1||count>144} onClick={()=>void action(async()=>{await api.generateVariants(name,seed,count,3);setRuns(await api.generationRuns());})}>{busy?'Working…':active?'Generation in progress…':`Generate ${count} variants`}</Button>{imported&&<p className="mt-2 text-xs">Imported geometry has no procedural seed generator.</p>}{run&&<div role="status" className="mt-3 text-xs"><p>{label(name)} · {run.status.replaceAll('_',' ')} · {run.completed}/{run.total}</p><p className="mt-1 text-parch-400">{run.message||run.error}</p>{run.failures?.slice(0,3).map(f=><p key={f.seed} className="text-rust-400">Seed {f.seed}: {f.error}</p>)}</div>}</section>
        </div>
      </div>
      <div className="my-4 flex items-center justify-between"><h3 className="font-display text-xl">{label(name)} variants</h3><select className={field} aria-label="Variant review status" value={filter} onChange={e=>setFilter(e.target.value)}><option value="all">All variants</option><option value="pending">Pending endorsement</option><option value="endorsed">Endorsed</option></select></div>
      <div className="grid grid-cols-2 gap-3 md:grid-cols-3 xl:grid-cols-5">{visible.map((entry:LibraryEntry)=><button key={entry.id} onClick={()=>setInspectId(entry.id)} className={'overflow-hidden rounded border text-left '+(!entry.inventory_candidate?'bg-green-200/15 ':'')+(displayed?.id===entry.id?'border-gold-400':!entry.inventory_candidate?'border-green-300/60 hover:border-green-200':'border-stone-700 hover:border-stone-500')}><img src={api.thumbUrl(entry.id)} loading="lazy" className="aspect-square w-full object-contain bg-stone-900" alt={`${label(name)} seed ${entry.seed}`}/><div className="p-3 text-sm"><span className="capitalize">{label(name)}</span><p>Seed {entry.seed} · {entry.stats?.height_m?.toFixed(1)} m</p><p className={"text-xs "+(entry.inventory_candidate?"text-parch-500":"text-green-200")}>{entry.inventory_candidate?'Pending endorsement':'Endorsed'}{entry.id===design?.reference_variant_id?' · Reference':''}</p></div></button>)}</div>
      {!visible.length&&<p className="p-8 text-center text-parch-500">No {filter==='all'?'saved':filter} variants for this species yet.</p>}
    </main>
  </div>;
}
