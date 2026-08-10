/* asset-forge frontend.
 *
 * One page, no framework. The state that matters is a single spec object; the
 * sliders write into it, generation reads it, and the server validates it.
 * Everything else is presentation.
 */

const $ = (id) => document.getElementById(id);

const state = {
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
  state.schema = await fetch("/api/schema").then((r) => r.json());
  state.palette = await fetch("/api/palette").then((r) => r.json());
  const specs = await fetch("/api/specs").then((r) => r.json());

  state.viewer = Viewer.create($("detailCanvas"));
  if (!state.viewer) {
    // No WebGL2 — fall back to the flat render rather than an empty canvas.
    $("detailCanvas").classList.add("hidden");
    $("view3d").disabled = true;
    state.view = "2d";
  }

  $("species").innerHTML = specs
    .map((s) => `<option value="${s.name}">${s.name}</option>`)
    .join("");

  fetch("/api/nl-status")
    .then((r) => r.json())
    .then((s) => {
      if (!s.available) {
        $("ask").disabled = true;
        $("askGo").disabled = true;
        $("ask").placeholder = `Plain-language edits unavailable — ${s.reason}`;
      } else {
        $("askMsg").textContent = s.model;
      }
    });

  await refreshLibrary();
  // Open on a representative species rather than whatever sorts first.
  const first = specs.find((s) => s.name === "temperate-oak") ?? specs[0];
  if (first) {
    $("species").value = first.name;
    await loadSpec(first.name);
  }
  wire();
  const want = /#seed=(\d+)/.exec(location.hash);
  if (want) state.pendingOpen = Number(want[1]);
  generate();
}

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
  // Regenerate on release, not on every pixel of a drag — a tree costs
  // hundreds of milliseconds and mid-drag renders would only ever be stale.
  if ($("auto").checked) generate();
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
    $("status").textContent =
      j.done < j.total ? `${j.done}/${j.total}  ${secs}s` : `${j.total} trees in ${secs}s`;

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
  loadVoxels(`/api/voxels?job=${state.job}&seed=${seed}`);
  $("detailProblems").innerHTML = (t.problems || [])
    .map((p) => `<div class="problem${p.startsWith("broken") ? " bad" : ""}">! ${escape(p)}</div>`)
    .join("");

  const rows = [
    ["voxel size", `${s.voxel_cm ?? 10} cm (preview)`],
    ["height", `${(s.height_m ?? 0).toFixed(1)} m`],
    ["footprint", `${(s.footprint_m || [0, 0]).map((v) => v.toFixed(1)).join(" × ")} m`],
    ["extent", `${(s.extent_vox || []).join(" × ")} voxels`],
    ["solid voxels", (s.voxels ?? 0).toLocaleString()],
    ["skeleton nodes", (s.nodes ?? 0).toLocaleString()],
    ["max branch order", s.max_order],
    ["foliage clumps", (s.clumps ?? 0).toLocaleString()],
    ["wood connected", `${((s.wood_connected ?? 1) * 100).toFixed(2)}%`],
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
  $("askMsg").textContent = "thinking…";
  $("askEdits").innerHTML = "";

  try {
    const r = await fetch("/api/nl-edit", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ spec: state.spec, request }),
    }).then((x) => x.json());

    if (r.error) {
      $("askMsg").textContent = r.error;
      return;
    }
    if (!r.edits.length) {
      $("askMsg").textContent = "no parameters changed";
      $("askEdits").innerHTML = `<div class="askedit">${escape(r.explanation || "")}</div>`;
      return;
    }

    // The model edits the same spec the sliders do, so the panel just rebuilds
    // and the moved sliders show exactly what changed.
    state.spec = r.spec;
    buildParams();
    $("askMsg").textContent = `${r.edits.length} parameter${r.edits.length === 1 ? "" : "s"} changed`;
    $("askEdits").innerHTML =
      `<div class="askedit">${escape(r.explanation)}</div>` +
      r.edits
        .map((e) => `<div class="askedit">· <b>${escape(e.label)}</b> → <code>${escape(String(e.value))}</code>${e.why ? ` — ${escape(e.why)}` : ""}</div>`)
        .join("");
    for (const w of r.warnings || []) toast(w);
    generate();
  } catch {
    $("askMsg").textContent = "request failed";
  } finally {
    $("askGo").disabled = false;
  }
}

/* --- 3D --------------------------------------------------------------- */

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
      `${count.toLocaleString()} surface voxels${note} · drag to spin · scroll to zoom`;
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

async function keepTree(seed, id) {
  $("keepMsg").textContent = "saving…";
  const m = await fetch("/api/keep", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ spec: state.spec, seed }),
  }).then((r) => r.json());

  if (m.error) return ($("keepMsg").textContent = "failed");
  state.kept.add(m.id);
  $("keepMsg").textContent = `saved · ${m.vox_models} vox model${m.vox_models === 1 ? "" : "s"}`;
  $("detailDownloads").innerHTML = downloadLinks(m.id);
  $(`tile-${seed}`)?.classList.add("kept");
  refreshLibrary();
}

/* --- library ------------------------------------------------------------ */

async function refreshLibrary() {
  const items = await fetch("/api/library").then((r) => r.json());
  state.kept = new Set(items.map((i) => i.id));
  $("library").innerHTML =
    items.length === 0
      ? `<div class="status">Nothing kept yet. Open a tree and press “Keep to library”.</div>`
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
  loadVoxels(`/api/voxels?id=${encodeURIComponent(id)}`);
  setView(state.viewer ? "3d" : "2d");
  $("overlay").classList.remove("hidden");
}

/* --- misc --------------------------------------------------------------- */

function showTab(which) {
  state.tab = which;
  $("gallery").classList.toggle("hidden", which !== "gallery");
  $("library").classList.toggle("hidden", which !== "library");
  $("tabGallery").classList.toggle("on", which === "gallery");
  $("tabLibrary").classList.toggle("on", which === "library");
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
    const specs = await fetch("/api/specs").then((x) => x.json());
    const cur = getPath(state.spec, "name");
    $("species").innerHTML = specs.map((s) => `<option value="${s.name}">${s.name}</option>`).join("");
    $("species").value = cur;
  };
  $("askGo").onclick = askEdit;
  $("ask").addEventListener("keydown", (e) => {
    // Enter applies; Shift+Enter is a newline.
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      askEdit();
    }
  });
  $("view3d").onclick = () => setView("3d");
  $("view2d").onclick = () => setView("2d");
  $("viewReset").onclick = () => state.viewer?.reset();
  $("tabGallery").onclick = () => showTab("gallery");
  $("tabLibrary").onclick = () => { showTab("library"); refreshLibrary(); };
  $("closeDetail").onclick = () => $("overlay").classList.add("hidden");
  $("overlay").onclick = (e) => { if (e.target === $("overlay")) $("overlay").classList.add("hidden"); };
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") $("overlay").classList.add("hidden");
    if (e.key === "g" && !/input|select|textarea/i.test(document.activeElement.tagName)) generate();
  });
}

boot();
