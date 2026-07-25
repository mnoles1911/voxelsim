#!/usr/bin/env python3
"""Build a self-contained HTML viewer with a LIVE water-level slider.

WHY A SEPARATE FILE RATHER THAN A PATCH TO THE EXPLORER. terrain-diffusion's
explorer is a third-party repo we only clone; editing its index.html would put
us on a fork and every upstream pull would conflict. It also serves no CORS
headers, so a page opened from file:// cannot call its API. Both problems go
away by fetching the window ONCE here, server-side, and baking the numbers into
the page.

That split is the right one anyway. Panning to new ground costs a model run
(~35 s for a 768 km window, minutes for real 30 m detail); changing the WATER
LEVEL costs nothing at all, because it is a rendering decision over data you
already have. So: pay once for the window, then explore waterlines, relief and
styling at 60 fps with the network unplugged.

WHAT THE SLIDER DOES AND DOES NOT MEAN. It is a VISUALISATION control. The
game's sea level is z = 0 -- not a setting but a load-bearing constant baked
into the tile datum (ETOPO's own zero), biome.h's coastal band, caves.h's
"the implicit ocean owns everything below z=0", and the water system. Dragging
this shows a HYPOTHETICAL world; it changes nothing about the one you generate.
The page says so on its face.

Two gates move with the slider and one deliberately does not: ocean and beach
are relative to the waterline, because that is what a waterline means; the
treeline stays at its absolute elevation, because it is set by temperature and
lapse rate, which do not care where the sea is.

Usage:
    python tools/water_viewer.py --window=-58,72,-76,54 --out viewer.html
    # then just open viewer.html
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from world_map import BIOMES, _read_constants, fetch  # noqa: E402

_HTML = """<!doctype html>
<meta charset="utf-8">
<title>terrain water-level viewer</title>
<style>
  :root { color-scheme: dark; }
  body { margin:0; background:#14161a; color:#dfe3e8;
         font:13px/1.45 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif; }
  #wrap { display:flex; gap:18px; padding:16px; align-items:flex-start; flex-wrap:wrap; }
  #cv { image-rendering:auto; border-radius:6px; box-shadow:0 2px 18px #0008; max-width:100%; }
  #panel { width:290px; flex:0 0 290px; }
  h1 { font-size:15px; margin:0 0 2px; font-weight:600; }
  .sub { color:#8b939e; font-size:11.5px; margin-bottom:14px; }
  .row { margin:11px 0; }
  label { display:flex; justify-content:space-between; font-size:12px; color:#b6bec8;
          margin-bottom:3px; }
  label b { color:#fff; font-variant-numeric:tabular-nums; font-weight:600; }
  input[type=range] { width:100%; accent-color:#4c8fd6; }
  select, button { width:100%; background:#212630; color:#dfe3e8; border:1px solid #333b47;
                   border-radius:5px; padding:5px 7px; font-size:12px; }
  button { cursor:pointer; margin-top:6px; }
  button:hover { background:#2a313d; }
  .chk { display:flex; align-items:center; gap:7px; font-size:12px; color:#b6bec8; }
  .chk input { accent-color:#4c8fd6; }
  table { border-collapse:collapse; width:100%; font-size:11.5px; margin-top:5px; }
  td { padding:1.5px 0; }
  td.sw { width:13px; } td.sw i { display:block; width:11px; height:11px; border-radius:2px; }
  td.pc { text-align:right; color:#fff; font-variant-numeric:tabular-nums; }
  .note { color:#7d8590; font-size:11px; margin-top:14px; border-top:1px solid #262c35;
          padding-top:9px; }
  .warn { color:#e0a34a; }
</style>
<div id="wrap">
  <canvas id="cv"></canvas>
  <div id="panel">
    <h1>Water level viewer</h1>
    <div class="sub">__SPAN__ km across &middot; seed __SEED__ &middot; coarse cells of 7.68 km</div>

    <div class="row">
      <label>Water level <b><span id="wlv">0</span> m</b></label>
      <input type="range" id="water" min="-2000" max="2000" step="10" value="0">
    </div>
    <div class="row chk"><input type="checkbox" id="nowater"><label for="nowater"
      style="margin:0;display:inline">Hide water entirely (bare topography)</label></div>
    <div class="row">
      <label>Vertical exaggeration <b><span id="rlv">3.0</span>&times;</b></label>
      <input type="range" id="relief" min="0.5" max="8" step="0.25" value="3">
    </div>
    <div class="row">
      <label>Biome tint <b><span id="btv">38</span>%</b></label>
      <input type="range" id="btint" min="0" max="100" step="1" value="38">
    </div>
    <div class="row">
      <label style="display:block">Colour ramp</label>
      <select id="ramp">
        <option value="hypso">Hypsometric (green &rarr; brown &rarr; white)</option>
        <option value="gray">Greyscale heightmap</option>
        <option value="viridis">Viridis</option>
      </select>
    </div>
    <div class="row chk"><input type="checkbox" id="shore" checked><label for="shore"
      style="margin:0;display:inline">Shoreline outline</label></div>
    <button id="reset">Reset to game sea level (z = 0)</button>

    <div class="row" style="margin-top:16px">
      <label>Land at this waterline <b><span id="landpc">0</span>%</b></label>
      <table id="legend"></table>
    </div>

    <div class="note">
      The slider is a <b>view</b>, not a world setting. Sea level in the game is
      <b>z&nbsp;=&nbsp;0</b> &mdash; inherited from ETOPO's datum, and baked into the tile
      format, the biome coastal band, the cave rules and the water system alike.
      Moving it here shows a hypothetical.<br><br>
      Ocean and beach follow the waterline. The <b>treeline does not</b>: it is set by
      temperature and lapse rate, which do not care where the sea is.<br><br>
      <span class="warn">Coarse data.</span> One cell is 7.68&nbsp;km; between-cell
      detail is interpolated, and the cliff gate (BARE_ROCK) is absent because slope is
      meaningless at this scale.
    </div>
  </div>
</div>
<script>
const D = __DATA__;
const K = __CONST__;
const BIOME_COLS = __COLS__, BIOME_NAMES = __NAMES__;
const H = D.h, W = D.w, F = 6;                    // F = render upsample
const elev = Float32Array.from(D.elev), temp = Float32Array.from(D.temp);
const prec = Float32Array.from(D.prec), tstd = Float32Array.from(D.tstd);

const cv = document.getElementById('cv'), cx = cv.getContext('2d');
const OW = W * F, OH = H * F;
cv.width = OW; cv.height = OH;
cv.style.width = Math.min(OW, 900) + 'px';
const img = cx.createImageData(OW, OH);

// --- upsample: nearest replicate, then a box blur. Interpolation only; it adds
// no information, it removes a 7.68 km staircase that otherwise dominates the
// shading and hides real landforms.
function upsample(src) {
  const big = new Float32Array(OW * OH);
  for (let y = 0; y < OH; y++) { const sy = (y / F) | 0;
    for (let x = 0; x < OW; x++) big[y * OW + x] = src[sy * W + ((x / F) | 0)]; }
  return blur(blur(big, F), F);
}
function blur(a, r) {                              // separable box blur
  const t = new Float32Array(a.length), o = new Float32Array(a.length);
  for (let y = 0; y < OH; y++) { let s = 0;
    for (let x = -r; x <= r; x++) s += a[y * OW + Math.min(OW - 1, Math.max(0, x))];
    for (let x = 0; x < OW; x++) { t[y * OW + x] = s / (2 * r + 1);
      s += a[y * OW + Math.min(OW - 1, x + r + 1)] - a[y * OW + Math.max(0, x - r)]; } }
  for (let x = 0; x < OW; x++) { let s = 0;
    for (let y = -r; y <= r; y++) s += t[Math.min(OH - 1, Math.max(0, y)) * OW + x];
    for (let y = 0; y < OH; y++) { o[y * OW + x] = s / (2 * r + 1);
      s += t[Math.min(OH - 1, y + r + 1) * OW + x] - t[Math.max(0, y - r) * OW + x]; } }
  return o;
}
const E = upsample(elev);
const cellM = 256 * 30 / F;

// Nearest-replicate the climate too. Biome is CATEGORICAL, so it is replicated
// and never interpolated -- blending DESERT into OCEAN would invent a biome.
function rep(src) {
  const big = new Float32Array(OW * OH);
  for (let y = 0; y < OH; y++) { const sy = (y / F) | 0;
    for (let x = 0; x < OW; x++) big[y * OW + x] = src[sy * W + ((x / F) | 0)]; }
  return big;
}
const T = rep(temp), P = rep(prec), S = rep(tstd);

// --- multi-scale hillshade, four azimuths (see world_map.py for why both).
function hillshade(relief) {
  const e = new Float32Array(OW * OH);
  for (let i = 0; i < e.length; i++) e[i] = E[i] * relief;
  const big = blur(e, 5), small = blur(e, 2), out = new Float32Array(OW * OH);
  const AZ = [315, 45, 135, 225].map(a => (360 - a + 90) * Math.PI / 180);
  const alt = 45 * Math.PI / 180;
  for (const [src, wgt] of [[big, 0.75], [small, 0.25]]) {
    for (let y = 0; y < OH; y++) for (let x = 0; x < OW; x++) {
      const i = y * OW + x;
      const xl = x > 0 ? i - 1 : i, xr = x < OW - 1 ? i + 1 : i;
      const yu = y > 0 ? i - OW : i, yd = y < OH - 1 ? i + OW : i;
      const dx = (src[xr] - src[xl]) / (2 * cellM), dy = (src[yd] - src[yu]) / (2 * cellM);
      const slope = Math.atan(Math.hypot(dx, dy)), asp = Math.atan2(-dx, dy);
      let acc = 0;
      for (const az of AZ) acc += Math.max(0, Math.sin(alt) * Math.cos(slope)
        + Math.cos(alt) * Math.sin(slope) * Math.cos(az - asp));
      out[i] += wgt * acc / AZ.length;
    }
  }
  for (let i = 0; i < out.length; i++) out[i] = Math.pow(Math.min(1, out[i]), 0.85);
  return out;
}
let shade = hillshade(3.0), lastRelief = 3.0;

// --- colour ramps, all keyed to HEIGHT ABOVE THE CURRENT WATERLINE so raising
// the sea re-bases the lowland green onto the new shore.
const HYPSO = [[0,[92,130,79]],[200,[133,153,89]],[600,[181,173,107]],
               [1200,[184,148,97]],[2000,[158,120,97]],[2800,[179,173,173]],[3600,[242,242,245]]];
function ramp(h, mode) {
  if (mode === 'gray') { const v = Math.max(0, Math.min(1, h / 4000)) * 235 + 20; return [v,v,v]; }
  if (mode === 'viridis') { const t = Math.max(0, Math.min(1, h / 4000));
    return [Math.round(255*(0.28+0.55*t*t)), Math.round(255*(0.03+0.9*Math.sqrt(t))),
            Math.round(255*(0.38+0.3*Math.sin(3.14*t)))]; }
  let i = 0; while (i < HYPSO.length - 2 && h > HYPSO[i+1][0]) i++;
  const [a, ca] = HYPSO[i], [b, cb] = HYPSO[i+1];
  const t = Math.max(0, Math.min(1, (h - a) / (b - a)));
  return [ca[0]+(cb[0]-ca[0])*t, ca[1]+(cb[1]-ca[1])*t, ca[2]+(cb[2]-ca[2])*t];
}

// --- biome, mirroring biome.h's gate ORDER. Sea and beach are relative to the
// waterline; the treeline is absolute (temperature sets it, not the sea).
function biomeAt(i, water) {
  const e = E[i], t = T[i], p = P[i], s = S[i];
  if (e < water + K.beachLo) return 0;                       // OCEAN
  if (e <= water + K.beachHi) return 1;                      // BEACH
  if (e > Math.max(K.treeBase + t * K.treePerC, K.beachHi)) return 8;  // TUNDRA_ALPINE
  if (t < K.tempCold) return 7;                              // TAIGA
  const warm = t >= K.tempWarm, hot = t >= K.tempHot, seas = s >= K.seasonHigh;
  if (p < K.precArid) return hot ? 5 : 2;
  if (p < K.precSemi) return (warm && seas) ? 6 : 2;
  if (p < K.precMod)  return (warm && seas) ? 6 : 3;
  return warm ? 4 : 3;
}

function draw() {
  const water = +document.getElementById('water').value;
  const hide = document.getElementById('nowater').checked;
  const relief = +document.getElementById('relief').value;
  const tint = +document.getElementById('btint').value / 100;
  const mode = document.getElementById('ramp').value;
  const shore = document.getElementById('shore').checked;

  if (relief !== lastRelief) { shade = hillshade(relief); lastRelief = relief; }

  const counts = new Array(9).fill(0); let land = 0;
  const d = img.data;
  for (let i = 0; i < OW * OH; i++) {
    const h = E[i] - water;
    const b = biomeAt(i, water);
    if (!hide && h < K.beachLo) {                      // water: depth ramp
      const dep = Math.min(1, -h / 6000);
      var r = (0.18 - 0.12 * dep) * 255, g = (0.38 - 0.24 * dep) * 255,
          bl = (0.62 - 0.30 * dep) * 255;
      const w = 0.25; const sh = 0.45 + 1.15 * shade[i];
      r *= (1 - w + w * sh); g *= (1 - w + w * sh); bl *= (1 - w + w * sh);
    } else {
      // With water hidden, colour the sea floor by its true depth so basins
      // still read -- that is the point of the mode.
      const c = ramp(hide ? E[i] + 6000 : h, mode);
      var r = c[0], g = c[1], bl = c[2];
      if (tint > 0 && !hide) {
        const bc = BIOME_COLS[b];
        r = r * (1 - tint) + bc[0] * tint; g = g * (1 - tint) + bc[1] * tint;
        bl = bl * (1 - tint) + bc[2] * tint;
      }
      const sh = 0.45 + 1.15 * shade[i];
      r *= sh; g *= sh; bl *= sh;
    }
    if (shore && !hide) {                              // 1-cell shoreline band
      const h2 = E[i] - water;
      if (h2 > K.beachLo && h2 < K.beachLo + 60) { r *= 0.35; g *= 0.35; bl *= 0.35; }
    }
    const o = i * 4;
    d[o] = Math.max(0, Math.min(255, r)); d[o+1] = Math.max(0, Math.min(255, g));
    d[o+2] = Math.max(0, Math.min(255, bl)); d[o+3] = 255;
    counts[b]++; if (h > K.beachHi) land++;
  }
  cx.putImageData(img, 0, 0);

  document.getElementById('wlv').textContent = water;
  document.getElementById('rlv').textContent = relief.toFixed(1);
  document.getElementById('btv').textContent = Math.round(tint * 100);
  document.getElementById('landpc').textContent = (100 * land / (OW * OH)).toFixed(1);
  const tot = OW * OH;
  document.getElementById('legend').innerHTML = BIOME_NAMES.map((n, i) =>
    counts[i] / tot < 0.001 ? '' :
    `<tr><td class="sw"><i style="background:rgb(${BIOME_COLS[i].join(',')})"></i></td>
     <td>${n}</td><td class="pc">${(100*counts[i]/tot).toFixed(1)}%</td></tr>`).join('');
}

for (const id of ['water','nowater','relief','btint','ramp','shore'])
  document.getElementById(id).addEventListener('input', draw);
document.getElementById('reset').addEventListener('click', () => {
  document.getElementById('water').value = 0;
  document.getElementById('nowater').checked = false; draw();
});
draw();
</script>
"""


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="http://127.0.0.1:8899")
    ap.add_argument("--window", default="-50,50,-50,50", help="ci0,ci1,cj0,cj1")
    ap.add_argument("--out", default="water_viewer.html")
    ap.add_argument("--seed", default="20260719")
    args = ap.parse_args()

    ci0, ci1, cj0, cj1 = (int(v) for v in args.window.split(","))
    span = (cj1 - cj0) * 256 * 30 / 1000
    print(f"fetching coarse window = {span:.0f} km across "
          "(one model run; after this the page needs no network) ...")
    d = fetch(args.url, ci0, ci1, cj0, cj1)
    k = _read_constants()

    payload = {
        "h": int(d["elev"].shape[0]), "w": int(d["elev"].shape[1]),
        "elev": [round(float(v), 1) for v in d["elev"].ravel()],
        "temp": [round(float(v), 2) for v in d["temp"].ravel()],
        "prec": [round(float(v), 1) for v in d["precip"].ravel()],
        "tstd": [round(float(v), 1) for v in d["tstd"].ravel()],
    }
    consts = {
        "beachLo": k["beach_lower_m"], "beachHi": k["beach_upper_m"],
        "treeBase": k["treeline_base_m"], "treePerC": k["treeline_m_per_c"],
        "tempCold": k["temp_cold_c"], "tempWarm": k["temp_warm_c"], "tempHot": k["temp_hot_c"],
        "precArid": k["precip_arid_mm"], "precSemi": k["precip_semi_mm"],
        "precMod": k["precip_mod_mm"], "seasonHigh": k["seasonal_high"],
    }
    html = (_HTML
            .replace("__DATA__", json.dumps(payload, separators=(",", ":")))
            .replace("__CONST__", json.dumps(consts))
            .replace("__COLS__", json.dumps([[round(c * 255) for c in col] for _, col in BIOMES]))
            .replace("__NAMES__", json.dumps([n for n, _ in BIOMES]))
            .replace("__SPAN__", f"{span:.0f}")
            .replace("__SEED__", args.seed))
    Path(args.out).write_text(html, encoding="utf-8")
    kb = len(html) / 1024
    print(f"wrote {args.out} ({kb:.0f} KB, self-contained -- open it in a browser)")
    print("thresholds baked in from biome.h:")
    print(f"  cold <{k['temp_cold_c']:g}C  warm >={k['temp_warm_c']:g}C  hot >={k['temp_hot_c']:g}C")
    print(f"  arid <{k['precip_arid_mm']:g}  semi <{k['precip_semi_mm']:g}  "
          f"moderate <{k['precip_mod_mm']:g} mm/yr")


if __name__ == "__main__":
    main()
