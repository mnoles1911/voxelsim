"""Generate final human-readable handover from validated collection records."""
import json,time,urllib.request
from pathlib import Path
import _path
from forge import inventory
from forge.understory_profiles import PROFILES
ROOT=Path(__file__).resolve().parents[1]
def main():
 audit=json.loads((ROOT/'out/understory-collection/audit-report.json').read_text())
 live=json.load(urllib.request.urlopen('http://127.0.0.1:8731/api/specs'))
 rows={r['name']:r for r in live if r['name'] in PROFILES}
 assert len(rows)==113
 for n,r in rows.items():
  d=r['design'];assert d['generator_approved'] and d['baseline_current'] and d['reference_available'],n
  assert r['resolution_cm']=='2.5' and len(d['variants'])==36 and d['reference_variant_id']==f'{n}-0007',n
 inventory.write_json(ROOT/'out/understory-review/live-audit.json',dict(timestamp=time.time(),profiles=113,variants=4068,all_sources_current=True,all_references_available=True,all_25mm=True))
 p=ROOT/'out/understory-review/checkpoint.json';checkpoint=json.loads(p.read_text());checkpoint.update(server_session=64906,server_note='Final live HTTP audit passed: 113 current sources, 4068 variants, all reference instances available',updated_at=time.time());inventory.write_json(p,checkpoint)
 text='''# Temperate understory completion report

Completed: 113 reviewed authoritative sources and 4,068 deterministic variants, 36 per profile. All use 25 mm cubic voxels and spring appearance. Source seed 7 is the authoritative reference instance. All variants remain pending user endorsement; source acceptance does not approve world placement.

Scope includes 20 bushes, 10 reeds, 38 grass-category profiles and 45 flowers. Existing grass categories also contain ferns, mosses and aquatic plants. Wetland and aquatic species require their correct biome microhabitats. The two jungle-labelled legacy IDs are retained with documented temperate analogues: Great wood-rush (Luzula sylvatica) and False Solomon's seal (Maianthemum racemosum).

## Review and pipeline

Actual remote botanical photographs were inspected, followed by small/medium/large pilots (seeds 1, 4 and 7) and all 36-variant contact sheets. Per-profile records bind acceptance to generator identity, image sheets and generated artifacts. Refactors distinguish woody branch systems, cane shrubs, fronds, basal fans, creeping runners, rosettes, moss colonies, floating pads and submerged whorled shoots. Summer-flowering species use vegetative spring forms; limited late-spring blooms are recorded individually.

Authoritative baseline, current generator digest, reference ID and seed inventory live in `library/<species>/species.json`; photo observations and geometry findings are in `reference-review.json` beside it. Pending artifact bytes live in `out/forge-candidates/<species>/<variant>/`. Forge and Asset Library use these existing persistent records. The 1.8 m scale pawn remains available in the preview. No global game banks were published by this collection pass.

Generator dependency isolation preserves all 49 previously accepted tree sources. Relevant architecture/helper edits invalidate affected understory sources; unrelated profile edits preserve their identity. Identity migrations were allowed only after rebuilding saved voxels to identical bytes. User decisions and endorsed geometry were preserved.

## Validation and retirement

- All 113 sources have current approved generator identities, available seed-7 references, spring baselines and 36 distinct 25 mm VXA geometries.
- Artifact hashes and deterministic rebuilds passed. Independent root-agent payload checks passed all 4,068 VXA files; 24 explicitly vegetative profiles also passed leaf/bark-only material checks.
- Fresh-session endorsement/rejection persistence and stale/corrupt cache guards passed. Existing 686 unrelated recipe hashes and 49 accepted tree sources remain valid.
- Active inventory sweep found exactly 4,068 scoped variants, zero old active assets and zero orphan VXA folders.
- Twelve pre-review assets were retired after replacements passed: three each for meadow-grass, bramble-thicket, meadow-daisy and water-reed. No protected endorsed/rejected asset was removed. Historical review evidence remains offline as provenance.
- Final port-8731 HTTP audit passed all 113 current sources and reference availability after server reload.

## Cost and limitations

No paid generation API calls were used. Latest recorded per-profile generation batches sum to 631.96 seconds (about 10.5 minutes); this excludes Astra research/review time, earlier iterations and elapsed wall time. This is a CPU geometry pipeline, not an image-generation-per-variant workflow.

25 mm cannot resolve subvoxel leaves, fern pinnules, fine grass blades, hairs or tiny flowers faithfully. These remain coarse connected colonies or silhouettes at correct physical scale; plant organs were not enlarged to conceal this limit. Small flowers and dense leaves can merge. No new transparency or flower-cutout exception was introduced. Root connectors must embed into soil/sediment; floating plants need waterline alignment and submerged species need appropriate placement depth. No automatic world placement or biome-density tuning is claimed by this pass.

Remote reference photos remain remotely hosted. Some host-signed image URLs expire; stable source-page links and inspection observations are retained. Refresh from the credited source when required.

## Review artifacts

- [All sources, photos, pilots and sheets](http://127.0.0.1:8731/static/understory-review/index.html)
- `out/understory-review/manual-findings.json`: artifact-bound visual decisions
- `out/understory-collection/audit-report.json`: per-profile distinctness, pitch, timing and hashes
- `out/understory-collection/active-inventory-audit.json`: old/orphan asset sweep
- `out/understory-review/live-audit.json`: final application visibility
- `out/understory-collection/*-retirement.json`: contained retirement provenance

## Completed profile inventory

Each row has 36 seeds, seed 7 as reference, 25 mm pitch, and pending variant endorsements.

| Profile | Botanical reference | Architecture |
|---|---|---|
'''
 for n,p in sorted(PROFILES.items()):text+=f"| {n} | {p['taxon']} | {p['architecture']} |\n"
 (ROOT/'docs/understory-completion-report.md').write_text(text,encoding='utf8')
 plan=ROOT/'docs/understory-completion-plan.md';old=plan.read_text();marker='## Completion status';old=old.split(marker)[0];plan.write_text(old+'\n## Completion status\n\nAll 113 profiles and 4,068 variants are complete. The final report supersedes intermediate checkpoints below or in historical logs. See [completion report](understory-completion-report.md) for authoritative final counts, validation and limitations.\n',encoding='utf8')
 print('Final HTTP audit and completion report: PASS 113 / 4068')
if __name__=='__main__':main()
