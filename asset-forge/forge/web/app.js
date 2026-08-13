/* asset-forge frontend.
 *
 * One page, no framework. The state that matters is a single spec object; the
 * sliders write into it, generation reads it, and the server validates it.
 * Everything else is presentation.
 */

const $ = (id) => document.getElementById(id);

const state = {
  kind: "tree",      // the section being authored; scopes everything below it
  kinds: [],
  schema: null,
  spec: null,        // live, edited
  saved: null,       // last loaded from disk, for Revert
  job: null,
  poll: null,
  seeds: [],
  kept: new Set(),   // "species-seed" of things already in the library
  tab: "gallery",
  view: "3d",        // detail overlay: "3d" orbit viewer or "2d" flat render
  viewer: null,
  palette: {},
  viewToken: 0,      // guards against a slow voxel fetch landing after a newer one
};

/* --- dotted paths ------------------------------------------------------- */

const getPath = (obj, path) =>
  path.split(".").reduce((o, k) => (o == null ? undefined : o[k]), obj);

function setPath(obj, path, value) {
  const keys = path.split(".");
  let node = obj;
  for (const k of keys.slice(0, -1)) node = node[k] ?? (node[k] = {});
  node[keys.at(-1)] = value;
}

/* --- boot --------------------------------------------------------------- */

async function boot() {
  state.palette = await fetch("/api/palette").then((r) => r.json());
  state.kinds = await fetch("/api/kinds").then((r) => r.json());

  state.viewer = Viewer.create($("detailCanvas"));
  if (!state.viewer) {
    // No WebGL2 — fall back to the flat render rather than an empty canvas.
    $("detailCanvas").classList.add("hidden");
    $("view3d").disabled = true;
    state.view = "2d";
  }

  fetch("/api/vocabulary")
    .then((r) => r.json())
    .then((v) => {
      state.vocabulary = v;
      $("askMsg").textContent = `${v.concepts.length} things I understand`;
    });

  buildKindBar();
  wire();

  const wantKind = /#kind=(\w+)/.exec(location.hash);
  const want = /#seed=(\d+)/.exec(location.hash);
  if (want) state.pendingOpen = Number(want[1]);
  const tab = /#tab=(\w+)/.exec(location.hash);

  const kind = state.kinds.find((k) => k.key === wantKind?.[1] && k.ready)?.key ?? "tree";
  // generate() switches to the gallery when its response lands, so a deep-linked
  // tab has to be applied after it resolves or the gallery wins the race.
  await loadKind(kind, { generate: false });
  generate().then(() => {
    if (tab && tab[1] === "biomes") { showTab("biomes"); refreshCoverage(); }
    if (tab && tab[1] === "library") { showTab("library"); }
  });
}

/* --- asset kinds -------------------------------------------------------- */

function buildKindBar() {
  $("kinds").innerHTML = state.kinds
    .map(
      (k) => `<button class="kind${k.ready ? "" : " soon"}" data-kind="${k.key}"
        ${k.ready ? "" : "disabled"}
        title="${escape(k.blurb)}${k.ready ? "" : " — no generator yet"}">
        ${escape(k.label)}<span class="kcount">${k.ready ? k.species : "soon"}</span></button>`
    )
    .join("");
  $("kinds").querySelectorAll("[data-kind]").forEach((b) => {
    b.onclick = () => loadKind(b.dataset.kind);
  });
}

/* Switch section. Everything the designer sees is scoped by this: which
 * parameters exist, which species are listed, which library entries show, and
 * which biomes the coverage tab reports on. A rock section that still offered
 * foliage sliders and listed oaks would not be a section at all. */
async function loadKind(kind, { generate: gen = true } = {}) {
  const meta = state.kinds.find((k) => k.key === kind);
  if (!meta || !meta.ready) return;
  state.kind = kind;

  $("kinds").querySelectorAll("[data-kind]").forEach((b) => {
    b.classList.toggle("on", b.dataset.kind === kind);
  });
  $("kindLabel").textContent = meta.label;
  $("kindBlurb").textContent = meta.blurb;

  state.schema = await fetch(`/api/schema?kind=${kind}`).then((r) => r.json());
  const specs = await fetch(`/api/specs?kind=${kind}`).then((r) => r.json());

  $("species").innerHTML = specs
    .map((s) => `<option value="${s.name}">${s.name}</option>`)
    .join("");

  await refreshLibrary();

  // Open on a representative species rather than whatever sorts first.
  const first =
    specs.find((s) => s.name === DEFAULT_SPECIES[kind]) ?? specs[0];
  if (first) {
    $("species").value = first.name;
    await loadSpec(first.name);
    if (gen) generate();
  } else {
    // A kind with a generator but no species yet: say so instead of showing an
    // empty dropdown and a stale gallery from the previous section.
    state.spec = null;
    $("params").innerHTML = "";
    $("gallery").innerHTML =
      `<div class="status">No ${escape(meta.label.toLowerCase())} authored yet.</div>`;
  }
  if (state.tab === "biomes") refreshCoverage();
  history.replaceState(null, "", `#kind=${kind}`);
}

const DEFAULT_SPECIES = {
  tree: "temperate-oak",
  bush: "bramble-thicket",
  rock: "granite-boulder",
  grass: "meadow-grass",
  reed: "water-reed",
  flower: "meadow-daisy",
  fish: "brown-trout",
  cetacean: "bottlenose-dolphin",
  bird: "european-robin",
};

/* Kinds with no branch structure. Branch statistics on any of these would be a
 * column of zeroes pretending to mean something, so the kind decides which rows
 * the detail sheet has. Mirrors `pipeline.BRANCHLESS`. */
const BRANCHLESS = new Set(["rock", "grass", "reed", "flower", "fish", "cetacean",
                            "bird"]);

async function loadSpec(name) {
  const r = await fetch(`/api/spec?name=${encodeURIComponent(name)}`).then((x) => x.json());
  state.spec = r.spec;
  state.saved = JSON.parse(JSON.stringify(r.spec));
  $("hash").textContent = r.hash;
  $("specName").value = getPath(r.spec, "name") ?? name;
  buildParams();
}

/* --- parameter panel ---------------------------------------------------- */

function buildParams() {
  const byGroup = new Map();
  for (const p of state.schema.params) {
    if (!byGroup.has(p.group)) byGroup.set(p.group, []);
    byGroup.get(p.group).push(p);
  }

  const openByDefault = new Set(["general", "crown", "trunk"]);
  const html = [];
  for (const [group, params] of byGroup) {
    html.push(
      `<details class="group"${openByDefault.has(group) ? " open" : ""}>` +
        `<summary>${group}</summary><div class="body">` +
        params.map(control).join("") +
        `</div></details>`
    );
  }
  $("params").innerHTML = html.join("");

  $("params").querySelectorAll("[data-path]").forEach((el) => {
    el.addEventListener("input", onParamInput);
    el.addEventListener("change", onParamCommit);
  });
  markChanged();
}

function control(p) {
  const v = getPath(state.spec, p.path);
  const help = p.help ? `<div class="help">${escape(p.help)}</div>` : "";
  const id = `p_${p.path.replace(/\./g, "_")}`;

  if (p.kind === "bool") {
    return `<div class="row" data-row="${p.path}">
      <label class="check"><input type="checkbox" data-path="${p.path}" data-kind="bool"
        ${v ? "checked" : ""}> ${escape(p.label)}</label>${help}</div>`;
  }
  if (p.kind === "choice") {
    const opts = p.choices
      .map((c) => `<option value="${c}"${c === v ? " selected" : ""}>${c}</option>`)
      .join("");
    return `<div class="row" data-row="${p.path}"><div class="rowtop">
      <label for="${id}">${escape(p.label)}</label></div>
      <select id="${id}" data-path="${p.path}" data-kind="choice">${opts}</select>${help}</div>`;
  }
  if (p.kind === "text") {
    return `<div class="row" data-row="${p.path}"><div class="rowtop">
      <label for="${id}">${escape(p.label)}</label></div>
      <input id="${id}" type="text" data-path="${p.path}" data-kind="text"
        value="${escape(String(v ?? ""))}">${help}</div>`;
  }
  const step = p.kind === "int" ? Math.max(1, p.step) : p.step;
  return `<div class="row" data-row="${p.path}"><div class="rowtop">
      <label for="${id}">${escape(p.label)}</label><output>${fmt(v, p)}</output></div>
      <input id="${id}" type="range" data-path="${p.path}" data-kind="${p.kind}"
        min="${p.lo}" max="${p.hi}" step="${step}" value="${v}">${help}</div>`;
}

function onParamInput(e) {
  const el = e.target;
  const path = el.dataset.path;
  const p = state.schema.params.find((x) => x.path === path);
  let value;
  if (el.dataset.kind === "bool") value = el.checked;
  else if (el.dataset.kind === "int") value = Math.round(Number(el.value));
  else if (el.dataset.kind === "float") value = Number(el.value);
  else value = el.value;

  setPath(state.spec, path, value);
  const out = el.closest(".row")?.querySelector("output");
  if (out) out.textContent = fmt(value, p);
  markChanged();
  if (path === "name") $("specName").value = value;
}

function onParamCommit() {
  // Regenerate on release, not on every pixel of a drag — an asset costs
  // hundreds of milliseconds and mid-drag renders would only ever be stale.
  if ($("auto").checked) generate();
}

/* On a phone the drawer covers the gallery, so a change made in it has nowhere
 * to show. Closing on commit is what makes the drawer usable: move a slider,
 * see the result. Desktop keeps the sidebar open — there it sits beside the
 * gallery and closing it would be pure loss. */
function maybeCloseDrawer() {
  if (isNarrow()) setPanel(false);
}

function markChanged() {
  for (const row of $("params").querySelectorAll("[data-row]")) {
    const path = row.dataset.row;
    const a = JSON.stringify(getPath(state.spec, path));
    const b = JSON.stringify(getPath(state.saved, path));
    row.classList.toggle("changed", a !== b);
  }
}

const fmt = (v, p) =>
  p && p.kind === "int" ? String(v) : Number(v).toFixed(String(p?.step ?? "0.01").split(".")[1]?.length ?? 2);

const escape = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

/* --- generation --------------------------------------------------------- */

async function generate(append = false) {
  if (!state.spec) return;   // section with a generator but nothing authored yet
  const count = Math.max(1, Math.min(200, Number($("count").value) || 12));
  const start = append
    ? (state.seeds.at(-1) ?? 0) + 1
    : Math.max(0, Number($("seedStart").value) || 1);

  const r = await fetch("/api/generate", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ spec: state.spec, seed_start: start, count }),
  }).then((x) => x.json());

  if (r.error) return toast("generate failed");
  $("hash").textContent = r.hash;
  if (r.warnings?.length) toast(r.warnings[0]);

  showTab("gallery");
  maybeCloseDrawer();
  state.job = r.job;
  state.seeds = append ? state.seeds.concat(r.seeds) : r.seeds;

  if (!append) $("gallery").innerHTML = "";
  for (const seed of r.seeds) $("gallery").insertAdjacentHTML("beforeend", placeholder(seed));

  startPolling();
}

const placeholder = (seed) => `
  <div class="tile" id="tile-${seed}" data-seed="${seed}">
    <div class="imgwrap"><div class="spinner">growing…</div></div>
    <div class="cap">seed ${seed}<div class="sub">…</div></div>
  </div>`;

function startPolling() {
  clearInterval(state.poll);
  const t0 = performance.now();
  state.poll = setInterval(async () => {
    const j = await fetch(`/api/job?job=${state.job}`).then((r) => r.json());
    if (j.error) return clearInterval(state.poll);

    $("bar").style.width = `${(j.done / j.total) * 100}%`;
    const secs = ((performance.now() - t0) / 1000).toFixed(1);
    const noun = (state.kinds.find((k) => k.key === state.kind)?.label ?? "assets").toLowerCase();
    $("status").textContent =
      j.done < j.total ? `${j.done}/${j.total}  ${secs}s` : `${j.total} ${noun} in ${secs}s`;

    for (const [seed, t] of Object.entries(j.tiles)) {
      if (!t.ready) continue;
      const el = $(`tile-${seed}`);
      if (!el || el.dataset.done) continue;
      el.dataset.done = "1";
      fillTile(el, Number(seed), t);
    }
    if (j.done >= j.total) clearInterval(state.poll);
  }, 350);
}

function fillTile(el, seed, t) {
  if (t.error) {
    el.querySelector(".imgwrap").innerHTML = `<div class="spinner">failed</div>`;
    el.querySelector(".cap .sub").textContent = "error";
    return;
  }
  const s = t.stats || {};
  const id = `${getPath(state.spec, "name")}-${String(seed).padStart(4, "0")}`;
  el.querySelector(".imgwrap").innerHTML =
    `<img loading="lazy" src="/api/tile?job=${state.job}&seed=${seed}" alt="seed ${seed}">`;
  el.querySelector(".cap").innerHTML =
    `seed ${seed}<div class="sub">${(s.height_m ?? 0).toFixed(1)} m · ${(s.voxels ?? 0).toLocaleString()} vox</div>`;
  if (t.problems?.length) {
    el.insertAdjacentHTML("beforeend", `<div class="flag" title="${escape(t.problems.join("\n"))}"></div>`);
  }
  if (state.kept.has(id)) el.classList.add("kept");
  // Keeping straight from the tile. Approving a batch is the common case --
  // generate forty, take the six that read well -- and routing every one of
  // those six through the detail overlay is six round trips of opening and
  // closing for a decision already made from the thumbnail.
  el.insertAdjacentHTML(
    "beforeend",
    `<button class="tilekeep" data-keep="${seed}" title="Keep to library">+</button>`
  );
  el.querySelector("[data-keep]").onclick = (ev) => {
    ev.stopPropagation();
    keepTree(seed, id);
  };
  el.onclick = () => openDetail(seed, t);

  // Deep link: #seed=N opens that variant straight away, so a particular tree
  // can be bookmarked or handed to someone else.
  if (state.pendingOpen === seed) {
    state.pendingOpen = null;
    openDetail(seed, t);
  }
}

/* --- detail ------------------------------------------------------------- */

function openDetail(seed, t) {
  history.replaceState(null, "", `#seed=${seed}`);
  const name = getPath(state.spec, "name");
  const id = `${name}-${String(seed).padStart(4, "0")}`;
  const s = t.stats || {};

  $("detailTitle").textContent = `${name} · seed ${seed}`;
  $("detailImg").src = `/api/detail?job=${state.job}&seed=${seed}`;
  loadVoxels(`/api/voxels?job=${state.job}&seed=${seed}${VIEW_BUDGET}`);
  $("detailProblems").innerHTML = (t.problems || [])
    .map((p) => `<div class="problem${p.startsWith("broken") ? " bad" : ""}">! ${escape(p)}</div>`)
    .join("");

  // Branch statistics on a rock or a fish would be a column of zeroes
  // pretending to mean something, so the kind decides which rows exist.
  const branchy = !BRANCHLESS.has(s.kind ?? state.kind);
  const rows = [
    ["voxel size", `${s.voxel_cm ?? 10} cm (preview)`],
    ["height", `${(s.height_m ?? 0).toFixed(1)} m`],
    ["footprint", `${(s.footprint_m || [0, 0]).map((v) => v.toFixed(1)).join(" × ")} m`],
    ["extent", `${(s.extent_vox || []).join(" × ")} voxels`],
    ["solid voxels", (s.voxels ?? 0).toLocaleString()],
    ...(branchy
      ? [
          ["skeleton nodes", (s.nodes ?? 0).toLocaleString()],
          ["max branch order", s.max_order],
          ["foliage clumps", (s.clumps ?? 0).toLocaleString()],
          ["wood connected", `${((s.wood_connected ?? 1) * 100).toFixed(2)}%`],
        ]
      : []),
    ["ground contact", `${s.ground_contact} voxels`],
    ["build time", `${s.ms_total} ms`],
    ["spec hash", s.spec_hash],
  ];
  $("detailStats").innerHTML = rows
    .map(([k, v]) => `<tr><td>${k}</td><td>${escape(String(v ?? "—"))}</td></tr>`)
    .join("");

  $("keepMsg").textContent = state.kept.has(id) ? "already in the library" : "";
  $("keep").onclick = () => keepTree(seed, id);
  $("detailDownloads").innerHTML = state.kept.has(id) ? downloadLinks(id) : "";
  setView(state.viewer ? state.view : "2d");
  $("overlay").classList.remove("hidden");
}

/* --- plain-language edits ---------------------------------------------- */

async function askEdit() {
  const request = $("ask").value.trim();
  if (!request) return;
  $("askGo").disabled = true;
  $("askEdits").innerHTML = "";

  try {
    const r = await fetch("/api/interpret", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ spec: state.spec, request }),
    }).then((x) => x.json());

    if (r.error) {
      $("askMsg").textContent = r.error;
      return;
    }

    const parts = [];
    if (r.understood.length) {
      parts.push(...r.understood.map((u) => `<div class="askedit">· ${escape(u)}</div>`));
    }
    // An unrecognised word is a gap in the vocabulary, not a silent no-op —
    // always say which words went unused.
    if (r.ignored.length) {
      parts.push(
        `<div class="askedit miss">didn't understand: ${r.ignored.map(escape).join(", ")}</div>`
      );
    }
    if (r.edits.length) {
      parts.push(
        ...r.edits.map(
          (e) => `<div class="askedit">&nbsp;&nbsp;<b>${escape(e.label)}</b> <code>${fmtVal(e.from)} → ${fmtVal(e.to)}</code></div>`
        )
      );
    }
    $("askEdits").innerHTML = parts.join("");
    $("askMsg").textContent = r.edits.length
      ? `${r.edits.length} parameter${r.edits.length === 1 ? "" : "s"} changed`
      : "nothing recognised";

    if (!r.edits.length) return;
    // Writes to the same spec a slider drag does, so the panel just rebuilds
    // and the moved sliders show exactly what changed.
    state.spec = r.spec;
    buildParams();
    for (const w of r.warnings || []) toast(w);
    generate();
  } catch {
    $("askMsg").textContent = "interpret failed";
  } finally {
    $("askGo").disabled = false;
  }
}

const fmtVal = (v) =>
  typeof v === "number" ? (Number.isInteger(v) ? v : v.toFixed(2)) : String(v);

/* --- 3D --------------------------------------------------------------- */

// Touch and mouse do the same three things by different gestures; say which.
const TOUCH = window.matchMedia("(hover: none)").matches;

// Ask the server for a lattice a phone can actually hold. This trades voxel
// size for load time and frame rate, and the viewer says out loud when it had
// to step coarser than the asset is authored at, so nothing is hidden.
const VIEW_BUDGET = TOUCH ? "&max=400000" : "";

const GESTURES = TOUCH
  ? "drag to spin · pinch to zoom · double-tap to reset"
  : "drag to spin · scroll to zoom";

async function loadVoxels(url) {
  if (!state.viewer) return;
  const token = ++state.viewToken;
  $("viewHint").textContent = "loading…";
  try {
    const res = await fetch(url);
    const shown = res.headers.get("X-Voxel-Cm");
    const authored = res.headers.get("X-Authored-Cm");
    const buf = await res.arrayBuffer();
    if (token !== state.viewToken) return; // a newer tree was opened meanwhile
    const { offsets, colors, dims, count } = Viewer.decode(buf, state.palette);
    state.viewer.setInstances(offsets, colors, dims);
    // Say plainly when the viewer had to step coarser than the asset is
    // authored at, so nobody mistakes the preview lattice for the export.
    const note =
      shown && authored && shown !== authored
        ? ` · shown at ${shown} cm (exports at ${authored} cm)`
        : shown
        ? ` · ${shown} cm voxels`
        : "";
    $("viewHint").textContent =
      `${count.toLocaleString()} surface voxels${note} · ${GESTURES}`;
  } catch {
    if (token === state.viewToken) $("viewHint").textContent = "3D load failed";
  }
}

function setView(which) {
  state.view = which;
  $("detailCanvas").classList.toggle("hidden", which !== "3d");
  $("detailImg").classList.toggle("hidden", which !== "2d");
  $("viewHint").classList.toggle("hidden", which !== "3d");
  $("view3d").classList.toggle("on", which === "3d");
  $("view2d").classList.toggle("on", which === "2d");
  if (which === "3d") state.viewer?.invalidate();
}

const downloadLinks = (id) => `
  <a href="/api/download?id=${id}&fmt=vox">.vox</a>
  <a href="/api/download?id=${id}&fmt=vxa">.vxa</a>
  <a href="/api/download?id=${id}&fmt=spec">spec.json</a>`;

async function keepTree(seed, id, { quiet = false } = {}) {
  if (state.kept.has(id)) {
    if (!quiet) $("keepMsg").textContent = "already in the library";
    return true;
  }
  if (!quiet) $("keepMsg").textContent = "saving…";
  $(`tile-${seed}`)?.classList.add("saving");
  const m = await fetch("/api/keep", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ spec: state.spec, seed }),
  })
    .then((r) => r.json())
    .catch(() => ({ error: "network" }));

  $(`tile-${seed}`)?.classList.remove("saving");
  if (m.error) {
    if (!quiet) $("keepMsg").textContent = "failed";
    return false;
  }
  state.kept.add(m.id);
  if (!quiet) {
    $("keepMsg").textContent = `saved · ${m.vox_models} vox model${m.vox_models === 1 ? "" : "s"}`;
    $("detailDownloads").innerHTML = downloadLinks(m.id);
  }
  $(`tile-${seed}`)?.classList.add("kept");
  if (!quiet) refreshLibrary();
  return true;
}

/* Keep every tile in the gallery that generated cleanly. The flagged ones are
 * skipped on purpose: a health problem is exactly the case that deserves a look
 * before it goes in the library, and this is the one control that could put a
 * broken asset in there forty at a time. */
async function keepAllClean() {
  const tiles = [...$("gallery").querySelectorAll(".tile[data-done]")].filter(
    (el) => !el.querySelector(".flag") && !el.classList.contains("kept")
  );
  if (!tiles.length) return toast("nothing new to keep");
  const name = getPath(state.spec, "name");
  $("keepAll").disabled = true;
  let saved = 0;
  for (const el of tiles) {
    const seed = Number(el.dataset.seed);
    const id = `${name}-${String(seed).padStart(4, "0")}`;
    if (await keepTree(seed, id, { quiet: true })) saved++;
    $("status").textContent = `keeping ${saved}/${tiles.length}…`;
  }
  $("keepAll").disabled = false;
  const flagged = $("gallery").querySelectorAll(".tile .flag").length;
  toast(`kept ${saved}` + (flagged ? ` · skipped ${flagged} flagged` : ""));
  $("status").textContent = `kept ${saved}`;
  refreshLibrary();
}

/* --- library ------------------------------------------------------------ */

async function refreshLibrary() {
  const all = await fetch("/api/library").then((r) => r.json());
  // `kept` stays over the WHOLE library, not the current section: it drives the
  // "already saved" badge on gallery tiles, and an id is unique across kinds.
  state.kept = new Set(all.map((i) => i.id));
  const items = all.filter((i) => (i.kind ?? "tree") === state.kind);
  const label = state.kinds.find((k) => k.key === state.kind)?.label ?? "assets";
  $("library").innerHTML =
    items.length === 0
      ? `<div class="status">No ${escape(label.toLowerCase())} kept yet. Open one and press “Keep to library”.</div>`
      : items
          .map(
            (i) => `<div class="tile" data-lib="${i.id}">
              <div class="imgwrap"><img loading="lazy" src="/api/library/thumb?id=${i.id}" alt=""></div>
              <div class="cap">${escape(i.id)}
                <div class="sub">${(i.stats?.height_m ?? 0).toFixed(1)} m · ${(i.stats?.voxels ?? 0).toLocaleString()} vox</div>
                <div class="downloads" style="margin-top:6px">${downloadLinks(i.id)}</div>
                <div class="cardactions">
                  <button data-act="inspect" data-id="${i.id}">Inspect in 3D</button>
                  <button data-act="vary" data-id="${i.id}">More like this</button>
                  <button data-act="delete" data-id="${i.id}" class="danger">Delete</button>
                </div>
              </div></div>`
          )
          .join("");

  $("library").querySelectorAll("[data-act]").forEach((b) => {
    b.onclick = () => libraryAction(b.dataset.act, b.dataset.id);
  });
}

async function libraryAction(action, id) {
  if (action === "delete") {
    if (!confirm(`Delete ${id} from the library?\n\nThis removes its spec, exports and thumbnail from disk.`)) return;
    const r = await fetch("/api/library/delete", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id }),
    }).then((x) => x.json());
    if (r.error) return toast("delete failed");
    toast(`deleted ${id}`);
    return refreshLibrary();
  }

  const r = await fetch(`/api/library/spec?id=${encodeURIComponent(id)}`).then((x) => x.json());
  if (r.error) return toast("could not load that entry");

  if (action === "inspect") {
    // Open the saved tree itself, at its own seed — not a fresh variant.
    state.spec = r.spec;
    state.saved = JSON.parse(JSON.stringify(r.spec));
    buildParams();
    $("specName").value = getPath(r.spec, "name");
    $("hash").textContent = r.hash;
    openLibraryDetail(id, r.seed, getPath(r.spec, "name"));
    return;
  }

  if (action === "vary") {
    // Load the kept tree's exact spec, then generate a fresh block of seeds
    // from it. The approved parameters stay; only the individual changes.
    state.spec = r.spec;
    state.saved = JSON.parse(JSON.stringify(r.spec));
    buildParams();
    $("specName").value = getPath(r.spec, "name");
    $("seedStart").value = Math.floor(Math.random() * 90000) + 1;
    toast(`variations of ${id}`);
    generate();
  }
}

function openLibraryDetail(id, seed, name) {
  $("detailTitle").textContent = `${name} · seed ${seed} · in library`;
  $("detailImg").src = `/api/library/thumb?id=${id}`;
  $("detailProblems").innerHTML = "";
  $("detailStats").innerHTML = "";
  $("keepMsg").textContent = "already in the library";
  $("keep").onclick = () => toast("already kept");
  $("detailDownloads").innerHTML = downloadLinks(id);
  loadVoxels(`/api/voxels?id=${encodeURIComponent(id)}${VIEW_BUDGET}`);
  setView(state.viewer ? "3d" : "2d");
  $("overlay").classList.remove("hidden");
}

/* --- misc --------------------------------------------------------------- */

const isNarrow = () => window.matchMedia("(max-width: 820px)").matches;

function setPanel(open) {
  document.body.classList.toggle("panelopen", open);
  $("panelToggle").setAttribute("aria-expanded", String(open));
  $("scrim").classList.toggle("hidden", !open);
}

function showTab(which) {
  setPanel(false);
  state.tab = which;
  for (const [id, tab] of [["gallery","tabGallery"],["library","tabLibrary"],["biomes","tabBiomes"]]) {
    $(id).classList.toggle("hidden", which !== id);
    $(tab).classList.toggle("on", which === id);
  }
}

/* --- biome coverage ----------------------------------------------------- */

async function refreshCoverage() {
  const c = await fetch(`/api/coverage?kind=${state.kind}`).then((r) => r.json());
  const label = state.kinds.find((k) => k.key === state.kind)?.label ?? "assets";
  const cards = c.biomes.map((b) => {
    if (!b.hosts) {
      return `<div class="biome none"><h3>${escape(b.label)}<span class="count">no ${escape(label.toLowerCase())}</span></h3>
        <div class="climate">${escape(b.climate)}</div></div>`;
    }
    // A biome with species but none approved is as much a gap as one with no
    // species at all — say both out loud rather than only counting rows.
    const gap = b.species.length === 0
      ? `no ${label.toLowerCase()} authored for this biome yet`
      : b.kept === 0 ? "species exist but none approved to the library yet" : "";
    const rows = b.species.map((s) => `
      <div class="sp" data-species="${escape(s.name)}">
        <span class="spname">${escape(s.name)}</span>
        <span class="spbar"><i style="width:${Math.round(s.weight * 100)}%"></i></span>
        <span class="spkept${s.kept ? "" : " zero"}">${s.kept ? s.kept + " kept" : "0 kept"}</span>
      </div>`).join("");
    return `<div class="biome${b.species.length ? (b.kept ? "" : " empty") : " empty"}">
      <h3>${escape(b.label)}<span class="count">${b.species.length} species · ${b.kept} approved</span></h3>
      <div class="climate">${escape(b.climate)} · surface ${escape(b.surface)}</div>
      ${gap ? `<div class="warn">${gap}</div>` : ""}
      ${rows}</div>`;
  });
  if (c.unassigned.length) {
    cards.push(`<div class="biome empty"><h3>Unassigned<span class="count">${c.unassigned.length}</span></h3>
      <div class="warn">no biome weight set — these will never be placed</div>
      ${c.unassigned.map((n) => `<div class="sp" data-species="${escape(n)}"><span class="spname">${escape(n)}</span></div>`).join("")}</div>`);
  }
  $("biomes").innerHTML = cards.join("");
  $("biomes").querySelectorAll("[data-species]").forEach((el) => {
    el.onclick = async () => {
      $("species").value = el.dataset.species;
      await loadSpec(el.dataset.species);
      generate();
    };
  });
}

let toastTimer = null;
function toast(msg) {
  $("toast").textContent = msg;
  $("toast").classList.remove("hidden");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => $("toast").classList.add("hidden"), 3200);
}

function wire() {
  $("species").onchange = async (e) => { await loadSpec(e.target.value); generate(); };
  $("generate").onclick = () => generate();
  $("more").onclick = () => generate(true);
  $("keepAll").onclick = keepAllClean;
  $("reroll").onclick = () => {
    $("seedStart").value = Math.floor(Math.random() * 90000) + 1;
    generate();
  };
  $("reset").onclick = () => {
    state.spec = JSON.parse(JSON.stringify(state.saved));
    buildParams();
    generate();
  };
  $("specName").oninput = (e) => { setPath(state.spec, "name", e.target.value); markChanged(); };
  $("saveSpec").onclick = async () => {
    const r = await fetch("/api/save-spec", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ spec: state.spec }),
    }).then((x) => x.json());
    if (r.error) return toast("save failed");
    toast(`saved specs/${r.saved}`);
    state.saved = JSON.parse(JSON.stringify(state.spec));
    markChanged();
    const specs = await fetch(`/api/specs?kind=${state.kind}`).then((x) => x.json());
    const cur = getPath(state.spec, "name");
    $("species").innerHTML = specs.map((s) => `<option value="${s.name}">${s.name}</option>`).join("");
    $("species").value = cur;
    // A fork adds to this section's count, which is on the kind bar.
    state.kinds = await fetch("/api/kinds").then((x) => x.json());
    buildKindBar();
    $("kinds").querySelector(`[data-kind="${state.kind}"]`)?.classList.add("on");
  };
  $("askGo").onclick = askEdit;
  $("ask").addEventListener("keydown", (e) => {
    // Enter applies; Shift+Enter is a newline.
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      askEdit();
    }
  });
  // The slider panel is a drawer on a narrow screen and a fixed sidebar on a
  // wide one; these controls only exist in the narrow layout.
  $("panelToggle").onclick = () => setPanel(!document.body.classList.contains("panelopen"));
  $("panelClose").onclick = () => setPanel(false);
  $("scrim").onclick = () => setPanel(false);

  $("view3d").onclick = () => setView("3d");
  $("view2d").onclick = () => setView("2d");
  $("viewReset").onclick = () => state.viewer?.reset();
  $("tabGallery").onclick = () => showTab("gallery");
  $("tabLibrary").onclick = () => { showTab("library"); refreshLibrary(); };
  $("tabBiomes").onclick = () => { showTab("biomes"); refreshCoverage(); };
  $("closeDetail").onclick = () => $("overlay").classList.add("hidden");
  $("overlay").onclick = (e) => { if (e.target === $("overlay")) $("overlay").classList.add("hidden"); };
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") $("overlay").classList.add("hidden");
    if (e.key === "g" && !/input|select|textarea/i.test(document.activeElement.tagName)) generate();
  });
}

boot();
