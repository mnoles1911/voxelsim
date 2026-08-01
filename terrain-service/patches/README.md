# terrain-diffusion patches

`terrain-diffusion` is a third-party repo (`github.com/xandergos/terrain-diffusion`)
that we import by path rather than fork. These patches carry the worldgen
changes voxelsim owns.

## `terrain-diffusion-worldgen.patch`

Applies to upstream commit **`82a0431`** ("Revert \"Update website\"").
`bootstrap_pod.sh` pins that same commit in `TD_COMMIT` and applies this patch
right after cloning. **Bump the pin and the patch together, never one alone.**

It adds two parameters to `make_synthetic_map_factory` / `WorldPipeline`:

| parameter | what it does |
|---|---|
| `orographic` | Windward-enhancement / lee rain-shadow multiplier on precipitation, derived from how much higher the terrain is *upwind*. Upstream couples temperature to elevation via a lapse rate and couples precipitation to nothing, so before this a desert could sit on the wet side of a range. |
| `elev_gain` / `elev_gain_power` | Monotone stretch of the elevation quantile table's above-sea-level knots, anchored at v=0 so coastlines never move. The only elevation-variance lever that exists — `seed` picks a realization of a fixed process and `frequency_mult` cannot change a marginal that quantile matching pins by construction. |

Both default to neutral (`orographic=None`, `elev_gain=1.0`), so an unpatched
call site reproduces upstream's exact output.

### Why a patch and not a fork

A fork is one more thing to keep in sync, and voxelsim is the repo that owns
the decision about what the world looks like. The patch keeps the pin honest:
`git apply --check` fails loudly if upstream moves under us, where a fork would
quietly diverge.

### The failure mode this guards against

**Upstream's `WorldPipeline.__init__` ends in `**deprecated_kwargs`, which is
read exactly once and only for `histogram_raw`.** Against an unpatched
checkout, `orographic=` and `elev_gain=` are therefore *accepted and silently
discarded* — no `TypeError`, no warning. `from_pretrained` splats a plain dict
(`cls(**config)`) with no key filtering, so nothing upstream will ever raise.

That matters because `provider_id()` hashes `as_pipeline_kwargs()` wholesale.
An unpatched pod would generate tiles with **no rain shadow and unstretched
relief**, stamped with an identity claiming both. Nothing in the output
distinguishes them, so the mislabeling is unrecoverable after the fact.

Three things defend against it, deliberately layered:

1. `bootstrap_pod.sh` **dies** if the patch is missing or does not apply.
2. `DiffusionProvider._load_pipeline` checks
   `inspect.signature(WorldPipeline.__init__)` against
   `as_pipeline_kwargs()` and refuses to run if any key would be swallowed.
   This is the one that catches a pod whose `clone` stamp was set by an older
   bootstrap, where step 1 is skipped entirely.
3. `terrain_diffusion_version` records `<upstream sha>+worldgen.<patch sha>`,
   so the identity distinguishes patched from unpatched even if both ran.

### Regenerating

From a terrain-diffusion checkout at the pinned commit with the changes applied:

```sh
git -C <terrain-diffusion> diff > terrain-service/patches/terrain-diffusion-worldgen.patch
sha256sum terrain-service/patches/terrain-diffusion-worldgen.patch   # first 12 hex chars
```

Then update `terrain_diffusion_version` in `providers/diffusion.py` with the
new patch hash. Verify without a scratch clone by reverse-applying against the
patched tree — if it reverse-applies cleanly it forward-applies cleanly:

```sh
git -C <terrain-diffusion> apply --check --reverse terrain-service/patches/terrain-diffusion-worldgen.patch
```
