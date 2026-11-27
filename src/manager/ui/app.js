/* iHeart Radio Station Manager - UI logic. Talks to the C# side through window.chrome.webview messages. */
"use strict";

// ------------------------------------------------------------------ messages to the app
const rpc = (() => {
  let n = 0;
  const pending = new Map();
  const wv = window.chrome && window.chrome.webview;
  if (wv) wv.addEventListener("message", (e) => {
    const m = e.data, p = pending.get(m.id);
    if (!p) return;
    pending.delete(m.id);
    m.ok ? p.resolve(m.result) : p.reject(new Error(m.error || "error"));
  });
  return (method, params = {}) => new Promise((resolve, reject) => {
    if (!wv) return reject(new Error("not running inside the app"));
    const id = ++n;
    pending.set(id, { resolve, reject });
    wv.postMessage({ id, method, params });
  });
})();

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const CYAN = [94, 246, 255];
const ICON = 512;

const state = {
  status: null,
  stations: [],
  selectedId: null,
  setup: null,        // { mode, id, callLetters, name, fm, volume, an, cands, key, gap, invert, noLogo, keepLogo, ... }
};

// ------------------------------------------------------------------ small helpers
function footer(text) { $("footStatus").textContent = text; }

let toastTimer = null;
function toast(title, text, err) {
  $("toastTitle").textContent = title;
  $("toastText").textContent = text || "";
  $("toast").classList.toggle("err", !!err);
  $("toast").classList.remove("hidden");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => $("toast").classList.add("hidden"), err ? 9000 : 6500);
}

function modal(title, text, buttons) {
  return new Promise((resolve) => {
    $("modalTitle").textContent = title;
    $("modalText").innerHTML = text;
    const box = $("modalActions");
    box.innerHTML = "";
    buttons.forEach((b) => {
      const el = document.createElement("button");
      el.className = "btn " + (b.cls || "btn-ghost");
      el.textContent = b.label;
      el.onclick = () => { $("modal").classList.add("hidden"); resolve(b.value); };
      box.appendChild(el);
    });
    $("modal").classList.remove("hidden");
  });
}

function loadImage(src) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = () => reject(new Error("could not read the image"));
    img.src = src;
  });
}

// image -> RGBA at most 512 px on its longest side
function imageData(img, max = 512) {
  const s = Math.min(1, max / Math.max(img.naturalWidth, img.naturalHeight));
  const w = Math.max(1, Math.round(img.naturalWidth * s)), h = Math.max(1, Math.round(img.naturalHeight * s));
  const c = document.createElement("canvas");
  c.width = w; c.height = h;
  const g = c.getContext("2d");
  g.imageSmoothingQuality = "high";
  g.drawImage(img, 0, 0, w, h);
  return { w, h, rgba: g.getImageData(0, 0, w, h).data };
}

// draw a white-on-transparent alpha map the way the game shows it: tinted, with a soft glow
function drawIcon(canvas, alpha, size, glow) {
  const g = canvas.getContext("2d");
  g.clearRect(0, 0, canvas.width, canvas.height);
  if (!alpha) return;
  const tmp = document.createElement("canvas");
  tmp.width = size; tmp.height = size;
  const tg = tmp.getContext("2d"), id = tg.createImageData(size, size);
  for (let i = 0; i < size * size; i++) {
    id.data[i * 4] = CYAN[0]; id.data[i * 4 + 1] = CYAN[1]; id.data[i * 4 + 2] = CYAN[2]; id.data[i * 4 + 3] = alpha[i];
  }
  tg.putImageData(id, 0, 0);
  if (glow) { g.shadowColor = "rgba(94,246,255,0.55)"; g.shadowBlur = canvas.width / 28; }
  g.drawImage(tmp, 0, 0, canvas.width, canvas.height);
  g.shadowBlur = 0;
}

// alpha map from a saved icon PNG (white on transparent)
async function alphaFromPng(src, size) {
  const img = await loadImage(src);
  const c = document.createElement("canvas");
  c.width = size; c.height = size;
  const g = c.getContext("2d");
  g.drawImage(img, 0, 0, size, size);
  const d = g.getImageData(0, 0, size, size).data, a = new Uint8Array(size * size);
  for (let i = 0; i < size * size; i++) a[i] = d[i * 4 + 3];
  return a;
}

// 512x512 alpha map -> PNG data URL (white on transparent) for the app to turn into a game texture
function iconPng(alpha) {
  const c = document.createElement("canvas");
  c.width = ICON; c.height = ICON;
  const g = c.getContext("2d"), id = g.createImageData(ICON, ICON);
  for (let i = 0; i < ICON * ICON; i++) { id.data[i * 4] = id.data[i * 4 + 1] = id.data[i * 4 + 2] = 255; id.data[i * 4 + 3] = alpha[i]; }
  g.putImageData(id, 0, 0);
  return c.toDataURL("image/png");
}

const nextFrame = () => new Promise((r) => requestAnimationFrame(() => setTimeout(r, 0)));

// ------------------------------------------------------------------ status: chips + banners
let applying = false;
async function refreshStatus() {
  let st;
  try { st = await rpc("status"); } catch (e) { footer("APP NOT CONNECTED"); return; }
  const prev = state.status;
  state.status = st;
  $("ver").textContent = "v" + st.version;
  const chip = (label, cls, title) => `<div class="chip ${cls}" title="${esc(title || "")}">${label}</div>`;
  let chips = "";
  chips += chip("GAME", st.gameFound ? "ok" : "bad", st.gameDir || "Cyberpunk 2077 not found");
  if (st.gameFound) {
    chips += chip("RADIOEXT", st.radioExt ? "ok" : "bad", "RadioExt (required)");
    chips += chip("BRIDGE", st.plugin ? "ok" : "bad", "iHeart Radio's RED4ext plugin");
  }
  chips += chip("FFMPEG", st.ffmpeg ? "ok" : "bad", st.ffmpeg || "needed to play iHeart streams");
  chips += chip(st.gameRunning ? "GAME RUNNING" : "GAME CLOSED", st.gameRunning ? "warn" : "", "");
  $("chips").innerHTML = chips;

  let b = "";
  if (!st.gameFound)
    b += `<div class="banner"><b>CYBERPUNK 2077 NOT FOUND</b><span>Point the manager at your game folder (the one with bin, archive and r6).</span><button class="btn btn-red small" data-act="browse">SELECT FOLDER</button></div>`;
  else {
    if (!st.radioExt) b += `<div class="banner"><b>RADIOEXT MISSING</b><span>The stations play through RadioExt. Install it with your mod manager.</span><button class="btn small" data-act="radioext">GET RADIOEXT</button></div>`;
    if (!st.plugin) b += `<div class="banner"><b>BRIDGE MISSING</b><span>red4ext\\plugins\\iHeartRadio is not installed - reinstall the iHeart Radio mod.</span></div>`;
  }
  if (!st.ffmpeg) b += `<div class="banner"><b>FFMPEG MISSING</b><span>The free tool that converts iHeart's audio for the game.</span><button class="btn btn-red small" data-act="ffmpeg">INSTALL FFMPEG</button></div>`;
  if (st.pendingLogos) b += `<div class="banner warn"><b>LOGOS WAITING</b><span>The game had the logo file open. They'll be applied as soon as the game is closed.</span><button class="btn small" data-act="apply">APPLY NOW</button></div>`;
  else if (st.gameRunning) b += `<div class="banner info"><b>GAME RUNNING</b><span>You can still add and edit stations: they show up the next time you start the game.</span></div>`;
  $("banners").innerHTML = b;

  // logos waiting and the game just closed: apply them
  if (st.pendingLogos && !st.gameRunning && !applying) {
    applying = true;
    try { const r = await rpc("applyLogos"); if (r.logosApplied) toast("LOGOS APPLIED", "Start the game to see them."); } catch (e) { }
    applying = false;
    if (!prev || prev.pendingLogos) setTimeout(refreshStatus, 300);
  }
}

document.addEventListener("click", async (e) => {
  const act = e.target.closest("[data-act]");
  if (!act) return;
  const a = act.dataset.act;
  if (a === "browse") {
    try { await rpc("browseGame"); await refreshStatus(); await loadStations(); } catch (err) { toast("NOT THE GAME FOLDER", err.message, true); }
  } else if (a === "radioext") rpc("open", { what: "https://www.nexusmods.com/cyberpunk2077/mods/4591" });
  else if (a === "ffmpeg") {
    act.disabled = true; act.textContent = "INSTALLING...";
    footer("INSTALLING FFMPEG - THIS CAN TAKE A MINUTE");
    try { await rpc("installFfmpeg"); toast("FFMPEG INSTALLED", "iHeart streams can now play in the game."); }
    catch (err) { toast("FFMPEG NOT INSTALLED", err.message, true); }
    footer("READY");
    refreshStatus();
  } else if (a === "apply") {
    const r = await rpc("applyLogos").catch((err) => ({ error: err.message }));
    if (r.logosApplied) toast("LOGOS APPLIED", "Start the game to see them.");
    else toast("CLOSE THE GAME FIRST", "Cyberpunk 2077 keeps the logo file open while it runs.", true);
    refreshStatus();
  }
});

// ------------------------------------------------------------------ installed stations (left)
async function loadStations() {
  if (!state.status || !state.status.gameFound) { state.stations = []; renderStations(); return; }
  try { state.stations = await rpc("list"); } catch (e) { toast("COULDN'T READ STATIONS", e.message, true); state.stations = []; }
  for (const s of state.stations) s.alpha = s.logo ? await alphaFromPng(s.logo, 108) : null;
  renderStations();
}

function renderStations() {
  const list = $("stationList");
  $("count").textContent = state.stations.length + (state.stations.length === 1 ? " STATION" : " STATIONS");
  if (!state.stations.length) {
    list.innerHTML = `<div class="list-empty">No iHeart stations yet.<br><br>Press <b>ADD STATION</b>, search for your local station
      and install it. It shows up in the car radio, the pocket radio and the radios in Night City.</div>`;
    return;
  }
  list.innerHTML = "";
  for (const s of state.stations) {
    const el = document.createElement("div");
    el.className = "station" + (s.id === state.selectedId ? " sel" : "");
    el.innerHTML = `<div><div class="code">4049181<br>4650180<br>0001364<br>0001219</div><div class="eq"><i></i><i></i><i></i></div></div>
      <canvas width="108" height="108"></canvas>
      <div><div class="nm">${esc(s.displayName)}</div><div class="sub2">IHEART ${esc(s.id)}${s.logo ? "" : " · NO LOGO"}</div></div>`;
    drawIcon(el.querySelector("canvas"), s.alpha, 108, false);
    el.onclick = () => openStation(s);
    list.appendChild(el);
  }
}

// ------------------------------------------------------------------ search (right)
function showView(name) {
  $("viewSearch").classList.toggle("hidden", name !== "search");
  $("viewSetup").classList.toggle("hidden", name !== "setup");
}

async function search(q) {
  q = (q || "").trim();
  if (!q) return;
  $("searchInput").value = q;
  showView("search");
  const box = $("results");
  box.innerHTML = `<div class="empty"><div class="spinner" style="margin:0 auto"></div><div class="empty-text">SCANNING FREQUENCIES...</div></div>`;
  footer("SEARCHING IHEARTRADIO FOR " + q.toUpperCase());
  let res;
  try { res = await rpc("search", { q }); }
  catch (e) { box.innerHTML = `<div class="empty"><div class="empty-title">NO SIGNAL</div><div class="empty-text">${esc(e.message)}</div></div>`; footer("READY"); return; }
  footer(res.length + " STATIONS FOUND");
  if (!res.length) { box.innerHTML = `<div class="empty"><div class="empty-title">NO STATIONS</div><div class="empty-text">Nothing on iHeartRadio matches "${esc(q)}". Try call letters (WHTZ) or a city.</div></div>`; return; }
  box.innerHTML = "";
  for (const r of res) {
    const el = document.createElement("div");
    el.className = "card";
    el.innerHTML = `${r.installed ? '<span class="badge">INSTALLED</span>' : ""}
      <img src="${esc(r.thumb)}" alt="" loading="lazy" referrerpolicy="no-referrer">
      <div><div class="cn">${esc(r.name)}</div><div class="cm">${esc([r.callLetters, r.frequency].filter(Boolean).join(" · "))}</div>
      <div class="cg">${esc(r.genre || "")}</div></div>`;
    el.onclick = () => {
      const inst = state.stations.find((s) => s.id === String(r.id));
      inst ? openStation(inst) : newStation(r.id);
    };
    box.appendChild(el);
  }
}

// ------------------------------------------------------------------ setup / edit (right)
function setupFields(s) {
  $("fName").value = s.name || "";
  $("fFm").value = s.fm || "";
  $("fVol").value = s.volume || 1;
  $("fGap").value = s.gap;
  $("fInvert").checked = !!s.invert;
  updatePreviewText();
}

function updatePreviewText() {
  const s = state.setup;
  if (!s) return;
  const name = $("fName").value.trim() || "UNNAMED STATION";
  $("rpName").textContent = name;
  $("artName").textContent = s.id ? "IHEART_" + String(s.id).padStart(8, "0") : "-";
  const vol = Math.round(parseFloat($("fVol").value) * 100);
  $("volText").textContent = vol + "%";
  $("rpVol").textContent = vol + "%";
  // a slice of the station list around this station, like the game shows it
  const fm = parseFloat($("fFm").value) || 0;
  const others = state.stations.filter((x) => x.id !== String(s.id)).map((x) => ({ n: x.displayName, fm: x.fm }));
  const vanilla = [[88.9, "Pacific Dreams"], [89.3, "Radio Vexelstrom"], [89.7, "Growl FM"], [91.9, "Royal Blue Radio"], [92.9, "Night FM"],
    [95.2, "Samizdat Radio"], [96.1, "Ritual FM"], [98.7, "Body Heat Radio"], [101.9, "The Dirge"], [103.5, "Radio PEBKAC"], [106.9, "30 Principales"], [107.3, "Morro Rock Radio"]]
    .map(([f, n]) => ({ n: f + " " + n, fm: f }));
  const all = others.concat(vanilla, [{ n: name, fm, me: true }]).sort((a, b) => a.fm - b.fm);
  const i = all.findIndex((x) => x.me);
  const slice = all.slice(Math.max(0, i - 2), Math.max(0, i - 2) + 5);
  $("rpList").innerHTML = slice.map((x) => `<div class="rp-item ${x.me ? "sel" : ""}">${x.me ? '<span class="eq"><i style="height:35%"></i><i style="height:65%"></i><i style="height:100%"></i></span>' : ""}${esc(x.n)}</div>`).join("");
}

async function newStation(id) {
  state.selectedId = null; renderStations();
  state.setup = { mode: "new", id: String(id), gap: 3, invert: false, volume: 1, name: "", fm: "" };
  openSetupView("NEW STATION");
  $("logoBusy").classList.remove("hidden");
  footer("TUNING IN TO STATION " + id);
  try {
    const st = await rpc("station", { id });
    Object.assign(state.setup, { name: st.displayName, fm: st.fm, callLetters: st.callLetters, source: st.logo, info: st });
    $("setupMeta").textContent = [st.callLetters, st.city, st.description].filter(Boolean).join(" · ").toUpperCase().slice(0, 90);
    setupFields(state.setup);
    if (!st.hasStream) toast("NO STREAM", "iHeart lists no live stream for this station: it may not play.", true);
    if (st.logo) await processLogo(st.logo); else noLogoMode();
  } catch (e) {
    toast("COULDN'T LOAD STATION", e.message, true);
    showView("search");
  }
  $("logoBusy").classList.add("hidden");
  footer("READY");
}

async function openStation(s) {
  state.selectedId = s.id; renderStations();
  let saved = {};
  try { saved = s.logoSettings ? JSON.parse(s.logoSettings) : {}; } catch (e) { }
  state.setup = { mode: "edit", id: s.id, name: s.displayName, fm: s.fm, volume: s.volume, gap: saved.gap != null ? saved.gap : 3,
    invert: !!saved.invert, savedKey: saved.key, keepLogo: !!s.logo, savedAlpha: s.logo ? await alphaFromPng(s.logo, ICON) : null };
  openSetupView("EDIT STATION");
  setupFields(state.setup);
  $("setupMeta").textContent = "IHEART STATION " + s.id;
  // show the installed logo right away; load the original for re-styling in the background
  drawIcon($("previewCanvas"), state.setup.savedAlpha, ICON, true);
  $("styles").innerHTML = `<div class="fine">${s.logo ? "Current logo shown. Loading the original logo to change its style..." : "No logo yet. Loading the station logo..."}</div>`;
  try {
    const st = await rpc("station", { id: s.id });
    if (state.setup.id !== s.id) return;
    state.setup.callLetters = st.callLetters;
    $("setupMeta").textContent = [st.callLetters, st.city].filter(Boolean).join(" · ").toUpperCase();
    if (st.logo && saved.source !== "custom") await processLogo(st.logo, true);
    else $("styles").innerHTML = `<div class="fine">${saved.source === "custom" ? "This station uses your own image. Pick USE MY OWN IMAGE to change it." : "iHeart has no logo for this station."}</div>`;
  } catch (e) {
    $("styles").innerHTML = `<div class="fine">Couldn't reach iHeartRadio to load the original logo (${esc(e.message)}).</div>`;
  }
}

function openSetupView(title) {
  showView("setup");
  $("setupTitle").textContent = title;
  $("saveBtn").textContent = state.setup.mode === "new" ? "INSTALL STATION" : "SAVE CHANGES";
  $("removeBtn").classList.toggle("hidden", state.setup.mode === "new");
  $("setupFine").textContent = "Changes show up the next time you start Cyberpunk 2077. Stations play through the iHeart Radio bridge, " +
    "which starts with the game.";
  $("styles").innerHTML = "";
  drawIcon($("previewCanvas"), null, ICON);
  updatePreviewText();
}

// run the logo engine on an image (data URL); keepSaved = leave the installed logo selected until the user picks
async function processLogo(src, keepSaved) {
  const s = state.setup;
  $("logoBusy").classList.remove("hidden");
  await nextFrame();
  try {
    const img = await loadImage(src);
    const d = imageData(img, 512);
    s.an = LogoCore.analyze(d.rgba, d.w, d.h);
    s.noLogo = false;
    computeCandidates(keepSaved ? s.savedKey : null, keepSaved);
  } catch (e) {
    toast("LOGO PROBLEM", e.message, true);
  }
  $("logoBusy").classList.add("hidden");
}

function computeCandidates(preferKey, keepSaved) {
  const s = state.setup;
  if (!s.an) return;
  s.cands = LogoCore.candidates(s.an, { gap: s.gap });
  s.thumbs = s.cands.map((c) => LogoCore.fit(c.ink, s.an.w, s.an.h, 128, { invert: s.invert }));
  const found = preferKey && s.cands.find((c) => c.key === preferKey);
  s.key = found ? found.key : s.cands.length ? s.cands[0].key : null;
  s.keepLogo = !!keepSaved && s.keepLogo;
  renderStyles();
  renderPreview();
}

function renderStyles() {
  const s = state.setup, box = $("styles");
  box.innerHTML = "";
  if (s.savedAlpha) {   // the logo installed now
    const el = document.createElement("div");
    el.className = "style" + (s.keepLogo ? " sel" : "");
    el.innerHTML = `<span class="best cur">CURRENT</span><canvas width="128" height="128"></canvas><div class="sn">Installed logo</div>`;
    const small = new Uint8Array(128 * 128);
    for (let y = 0; y < 128; y++) for (let x = 0; x < 128; x++) small[y * 128 + x] = s.savedAlpha[(y * 4) * ICON + x * 4];
    drawIcon(el.querySelector("canvas"), small, 128, false);
    el.onclick = () => { s.keepLogo = true; s.noLogo = false; renderStyles(); renderPreview(); };
    box.appendChild(el);
  }
  (s.cands || []).forEach((c, i) => {
    const el = document.createElement("div");
    el.className = "style" + (!s.keepLogo && !s.noLogo && c.key === s.key ? " sel" : "");
    el.innerHTML = `${i === 0 ? '<span class="best">AUTO</span>' : ""}<canvas width="128" height="128"></canvas><div class="sn">${esc(c.label)}</div>`;
    drawIcon(el.querySelector("canvas"), s.thumbs[i], 128, false);
    el.onclick = () => { s.key = c.key; s.keepLogo = false; s.noLogo = false; renderStyles(); renderPreview(); };
    box.appendChild(el);
  });
}

function currentAlpha() {
  const s = state.setup;
  if (s.noLogo) return null;
  if (s.keepLogo) return s.savedAlpha;
  const c = s.cands && s.cands.find((x) => x.key === s.key);
  if (!c) return s.savedAlpha || null;
  if (!s.full || s.full.key !== c.key || s.full.invert !== s.invert || s.full.gap !== s.gap)
    s.full = { key: c.key, invert: s.invert, gap: s.gap, alpha: LogoCore.fit(c.ink, s.an.w, s.an.h, ICON, { invert: s.invert }) };
  return s.full.alpha;
}

function renderPreview() { drawIcon($("previewCanvas"), currentAlpha(), ICON, true); }

function noLogoMode() {
  const s = state.setup;
  s.noLogo = true; s.keepLogo = false;
  renderStyles();
  renderPreview();
  $("styles").insertAdjacentHTML("afterbegin", `<div class="fine" style="grid-column:1/-1">No logo: the game's default radio icon is used.</div>`);
}

async function save() {
  const s = state.setup;
  if (!s) return;
  const name = $("fName").value.trim();
  if (!name) { toast("NAME NEEDED", "Give the station a name.", true); $("fName").focus(); return; }
  const params = {
    id: s.id, displayName: name, fm: parseFloat($("fFm").value) || 0, volume: parseFloat($("fVol").value) || 1,
    callLetters: s.callLetters || "", icon: "", removeIcon: false, logoSettings: "",
  };
  if (s.noLogo) params.removeIcon = true;
  else if (!s.keepLogo) {
    const a = currentAlpha();
    if (a) {
      params.icon = iconPng(a);
      params.logoSettings = JSON.stringify({ key: s.key, gap: s.gap, invert: s.invert, source: s.custom ? "custom" : "iheart" });
    }
  }
  $("saveBtn").disabled = true;
  footer("WRITING STATION FILES");
  try {
    const r = await rpc("install", params);
    const what = s.mode === "new" ? "INSTALLED" : "SAVED";
    if (r.logosApplied === false)
      toast(name.toUpperCase() + " " + what, "The game is running and keeps the logo file open: the logo is applied as soon as you close it. Restart the game to tune in.");
    else toast(name.toUpperCase() + " " + what, "Restart Cyberpunk 2077 to tune in.");
    state.selectedId = s.id;
    s.mode = "edit";
    if (params.icon) { s.savedAlpha = currentAlpha(); s.savedKey = s.key; s.keepLogo = true; }
    if (params.removeIcon) s.savedAlpha = null;
    renderStyles();
    $("saveBtn").textContent = "SAVE CHANGES";
    $("removeBtn").classList.remove("hidden");
    $("setupTitle").textContent = "EDIT STATION";
    await loadStations();
    refreshStatus();
  } catch (e) {
    toast("COULDN'T SAVE", e.message, true);
  }
  $("saveBtn").disabled = false;
  footer("READY");
}

async function remove() {
  const s = state.setup;
  if (!s) return;
  const name = $("fName").value.trim() || "this station";
  const ok = await modal("REMOVE STATION", `Remove <b style="color:var(--cyan)">${esc(name.toUpperCase())}</b> from the game? You can add it again any time.`,
    [{ label: "CANCEL", value: false }, { label: "REMOVE", value: true, cls: "btn-red" }]);
  if (!ok) return;
  try {
    await rpc("remove", { id: s.id });
    toast("STATION REMOVED", name + " is gone from the game's radio after a restart.");
    state.selectedId = null; state.setup = null;
    await loadStations();
    showView("search");
    refreshStatus();
  } catch (e) { toast("COULDN'T REMOVE", e.message, true); }
}

// ------------------------------------------------------------------ wiring
$("searchForm").addEventListener("submit", (e) => { e.preventDefault(); search($("searchInput").value); });
document.querySelectorAll(".hint a").forEach((a) => a.addEventListener("click", () => search(a.dataset.q)));
$("addBtn").onclick = () => { state.selectedId = null; renderStations(); showView("search"); $("searchInput").focus(); $("searchInput").select(); };
$("backBtn").onclick = () => { state.selectedId = null; renderStations(); showView("search"); };
$("fName").addEventListener("input", updatePreviewText);
$("fFm").addEventListener("input", updatePreviewText);
$("fVol").addEventListener("input", updatePreviewText);
$("fGap").addEventListener("change", () => { const s = state.setup; if (!s) return; s.gap = parseInt($("fGap").value, 10); s.keepLogo = false; computeCandidates(s.key); });
$("fInvert").addEventListener("change", () => { const s = state.setup; if (!s) return; s.invert = $("fInvert").checked; s.keepLogo = false; computeCandidates(s.key); });
$("noLogo").onclick = () => { if (state.setup) noLogoMode(); };
$("ownImage").onclick = () => $("fileInput").click();
$("fileInput").addEventListener("change", async () => {
  const f = $("fileInput").files[0];
  $("fileInput").value = "";
  if (!f || !state.setup) return;
  const url = await new Promise((r) => { const fr = new FileReader(); fr.onload = () => r(fr.result); fr.readAsDataURL(f); });
  state.setup.custom = true;
  state.setup.keepLogo = false;
  await processLogo(url);
});
$("saveBtn").onclick = save;
$("removeBtn").onclick = remove;
$("removeAll").onclick = async () => {
  if (!state.stations.length) { toast("NOTHING TO REMOVE", "No iHeart stations are installed."); return; }
  const ok = await modal("REMOVE ALL STATIONS",
    `Remove all <b style="color:var(--cyan)">${state.stations.length}</b> iHeart stations and their logos from the game?<br><br>` +
    `Do this before uninstalling the mod: the stations are files this manager wrote, so your mod manager doesn't remove them.`,
    [{ label: "CANCEL", value: false }, { label: "REMOVE ALL", value: true, cls: "btn-red" }]);
  if (!ok) return;
  try {
    const r = await rpc("removeAll");
    toast("ALL STATIONS REMOVED", r.logosApplied === false ? "Close the game to remove the logo file too." : "The game's radio is back to normal after a restart.");
    state.selectedId = null; state.setup = null;
    await loadStations(); showView("search"); refreshStatus();
  } catch (e) { toast("COULDN'T REMOVE", e.message, true); }
};
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape") {
    if (!$("modal").classList.contains("hidden")) return;
    state.selectedId = null; renderStations(); showView("search");
  }
  if (e.key === "F5") { e.preventDefault(); refreshStatus().then(loadStations); }
});

(async function start() {
  await refreshStatus();
  await loadStations();
  // rebuild the logo file with this version's writer (repairs logo files made by older versions)
  const st = state.status;
  if (st && st.gameFound && !st.gameRunning && state.stations.some((s) => s.logo)) {
    try { await rpc("applyLogos"); } catch (e) { toast("LOGOS NOT UPDATED", e.message, true); }
    refreshStatus();
  }
  setInterval(refreshStatus, 4000);
  $("searchInput").focus();
})();
