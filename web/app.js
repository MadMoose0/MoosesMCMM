'use strict';


//////////////// HELPERS
const $  = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
const mobileQuery = window.matchMedia('(max-width: 620px)');
const isMobile = () => mobileQuery.matches;

function clampInt(v, lo, hi) {
  const n = parseInt(v, 10);
  if (!Number.isFinite(n)) return lo;
  return Math.min(hi, Math.max(lo, n));
}


//////////////// ENGINE
let M = null;    // wasm module, null until loaded
let pollTimer = 0;

const api = {
  init:            () => M._mural_init(),
  poll:            () => M._mural_poll(),
  isProcessing:    () => !!M._mural_is_processing(),
  hasImage:        () => !!M._mural_has_image(),
  progress:        () => M._mural_progress(),
  heartbeat:       () => M._mural_heartbeat(),
  sourceWanted:    () => !!M._mural_source_wanted(),

  blocksJson:      () => M.ccall('mural_get_blocks_json', 'string', [], []),
  blockGroupsJson: () => M.ccall('mural_get_block_groups_json', 'string', [], []),
  blockCount:      () => M._mural_block_count(),
  // addresses leave wasm as signed 32 bit, negative above 2 GiB -- >>> 0 makes them usable as offsets
  blockAtlas:      () => M._mural_block_atlas() >>> 0,
  blockAtlasCols:  () => M._mural_block_atlas_cols(),

  overlaysJson:    () => M.ccall('mural_get_overlays_json', 'string', [], []),
  overlayCount:    () => M._mural_overlay_count(),
  overlayAtlas:    () => M._mural_overlay_atlas() >>> 0,
  overlayAtlasCols:() => M._mural_overlay_atlas_cols(),

  versionsJson:        () => M.ccall('mural_get_versions_json', 'string', [], []),
  overlayVersionsJson: () => M.ccall('mural_get_overlay_versions_json', 'string', [], []),
  setGameVersion:      i => M._mural_set_game_version(i),

  setAllowedBlocks:  ids => M.ccall('mural_set_allowed_blocks',   null, ['string'], [ids]),
  setAllowedOverlays:ids => M.ccall('mural_set_allowed_overlays', null, ['string'], [ids]),

  setSize:      (w, h, lock) => M._mural_set_size(w, h, lock ? 1 : 0),
  setName:      n => M.ccall('mural_set_name', null, ['string'], [n]),
  setPlacement: (o, v, h, b) => M._mural_set_placement(o, v, h, b),
  setBgMode:    m => M._mural_set_bg_mode(m),
  setTweaks:    t => M._mural_set_tweaks(t.bright, t.contrast, t.sat, t.hue, t.denoise, t.sharpen),
  setWeights:   w => M._mural_set_weights(w.wColor, w.wSpatial, w.wTexture),
  setDither:    d => M._mural_set_dither(d.method, d.intensity),
  setPass2:     p => M._mural_set_pass2(p.enable ? 1 : 0, p.chunk, p.threshold, p.wMSE, p.wSSIM, p.passes || 1, p.candidates || 3),

  // ui has a box per feature, engine wants one search switch + the amounts
  setEnhance:   e => {
    const search = searchWanted(e);
    M._mural_set_enhance((search || e.feather) ? 1 : 0, e.grainOn ? e.grain : 0.5, e.flatOn ? e.flat : 0,
      (search && e.protectOn) ? e.protect : 0, e.feather ? 1 : 0, 1, search ? 1 : 0);
  },
  setUseOverlays: on => M._mural_set_use_overlays(on ? 1 : 0),
  configJson:   () => M.ccall('mural_get_config_json', 'string', [], []),

  requestPreview: () => M._mural_request_preview(),
  snapshot:       () => M._mural_snapshot(),
  previewW:       () => M._mural_preview_w(),
  previewH:       () => M._mural_preview_h(),
  previewGrid:    () => M._mural_preview_grid() >>> 0,
  previewMean:    () => M._mural_preview_mean() >>> 0,
  previewAtlas:   () => M._mural_preview_atlas() >>> 0,
  previewCols:    () => M._mural_preview_atlas_cols(),
  previewCount:   () => M._mural_preview_atlas_count(),
  pickAndDisallow:(c, r) => M.ccall('mural_pick_and_disallow', 'string', ['number','number'], [c, r]),
  bgColor:        m => M.ccall('mural_bg_color', 'string', ['number'], [m]),

  log:       msg => M.ccall('mural_log', null, ['string'], [msg]),
  logCount:  () => M._mural_log_count(),
  logLine:   i => M.ccall('mural_log_line', 'string', ['number'], [i]),
  statusJson:() => M.ccall('mural_get_status_json', 'string', [], []),

  generateDatapack:  () => M._mural_generate_datapack(),
  generateLitematic: () => M._mural_generate_litematic()
};


//////////////// DEBUG LOG
const VERBOSE = false;
const dbg = VERBOSE ? (...args) => console.log(...args) : () => {};

let globalStartTime = 0;    // request start, debug timing only (ms)


//////////////// STATE
const DEFAULTS = {
  tweaks:  { bright: 0, contrast: 1, sat: 1, hue: 0, denoise: 0, sharpen: 0 },
  weights: { wColor: 5, wSpatial: 5, wTexture: 5 },
  dither:  { method: 0, intensity: 0.3 },
  pass2:   { enable: true, chunk: 8, passes: 1, candidates: 10, threshold: 0.25, wMSE: 5, wSSIM: 5 },
  enhance: { grainOn: false, grain: 0.5, flatOn: false, flat: 1, protectOn: false, protect: 0.35, feather: false },
  placement: { orientation: 0, dirVert: 0, dirHorz: 0, biome: 0 }
};

const S = {
  width: 50, height: 70, lock: true, name: 'my_mural',
  bgMode: 0,
  version: 0,

  useOverlays: false,
  ...structuredClone(DEFAULTS.placement),
  tweaks:  structuredClone(DEFAULTS.tweaks),
  weights: structuredClone(DEFAULTS.weights),
  dither:  structuredClone(DEFAULTS.dither),
  pass2:   structuredClone(DEFAULTS.pass2),
  enhance: structuredClone(DEFAULTS.enhance)
};

const BIOMES = ['Plains', 'Forest', 'Jungle', 'Swamp', 'Badlands', 'Desert/Savanna', 'Taiga', 'Dark Forest'];
const DITHER = ['None', 'Floyd-Steinberg', 'Atkinson', 'Bayer 4x4', 'Bayer 8x8', 'Random Noise'];

// patch sizes offered for refinement: engine index, label, blocks per side
const CHUNKS = [[0, '1x1 Block', 1], [1, '2x2 Blocks', 2], [3, '4x4 Blocks', 4], [5, '16x16 Blocks', 16], [8, 'Whole Image', Infinity]];
const CHUNK_WHOLE = 8;

// all sizes the engine has, by index -- old saves can hold one thats not offered anymore
const ENGINE_CHUNK_BLOCKS = [1, 2, 3, 4, 8, 16, 32, 64, Infinity];

const FACING_VERT = ['North', 'South', 'East', 'West'];
const FACING_HORZ = ['Top', 'Bottom'];

let blocks = [];
let allowedSet = new Set();
let overlays = [];
let overlayAllowedSet = new Set();

// game versions, oldest first -- blocks and overlays store an index into this
let versions = [];
let overlayVersions = [];    // same order as overlays
const selAllowed = new Set(), selDisallowed = new Set();
const selOvAllowed = new Set(), selOvDisallowed = new Set();


//////////////// ELEMENTS
const el = {
  boot: $('#boot'), bootMsg: $('#boot-msg'), app: $('#app'),
  path: $('#image-path'), file: $('#file-input'),
  width: $('#in-width'), height: $('#in-height'), lock: $('#in-lock'), name: $('#in-name'),
  orientation: $('#in-orientation'), facing: $('#in-facing'), biome: $('#in-biome'),
  version: $('#in-version'),
  dither: $('#in-dither'), pass2: $('#in-pass2'), pass2Fields: $('#pass2-fields'),
  chunk: $('#in-chunk'), thresholdRow: $('#row-threshold'),
  expGrain: $('#in-exp-grain'), expFlat: $('#in-exp-flat'), expProtect: $('#in-exp-protect'),
  enhFeather: $('#in-enh-feather'),
  search: $('#in-search'), searchbox: $('#searchbox'), searchClear: $('#btn-search-clear'),
  filterBtn: $('#btn-filters'), filterMenu: $('#filter-menu'),
  listAllowed: $('#list-allowed'), listDisallowed: $('#list-disallowed'),
  listOvAllowed: $('#list-ov-allowed'), listOvDisallowed: $('#list-ov-disallowed'),
  overlays: $('#in-overlays'), overlayFields: $('#overlay-fields'),
  moveBtn: $('#btn-move'), ovMoveBtn: $('#btn-ov-move'),
  warning: $('#warning'),
  canvas: $('#canvas'), wrap: $('#canvas-wrap'),
  empty: $('#canvas-empty'), busy: $('#canvas-busy'),
  emptyTitle: $('#canvas-empty strong'), emptyHint: $('#canvas-empty span'),
  log: $('#log'), status: $('#status'),
  right: $('#right'), consoleToggle: $('#console-toggle'), consoleLast: $('#console-last'),
  toast: $('#toast')
};


//////////////// ANALYTICS
// tested: google drops an event without send_to, amplitude.track works once the tag manager stub is on the page
const GA_ID = 'G-09DZ10TMF5';
const ampWaiting = [];
let ampTries = 0;

// one event to both tools -- a blocked or missing tool must never break the app
function track(name, props = {}) {
  try {
    window.dataLayer = window.dataLayer || [];
    (function () { window.dataLayer.push(arguments); })('event', name, { ...props, send_to: GA_ID });
  } catch (_) { }
  ampWaiting.push([name, props]);
  flushAmplitude();
}

// tag manager adds amplitude a little after the page, a blocked one never comes: 30 tries, 2 s apart
function flushAmplitude() {
  const amp = window.amplitude;
  if (amp && typeof amp.track === 'function') {
    while (ampWaiting.length) {
      const [name, props] = ampWaiting.shift();
      try { amp.track(name, props); } catch (_) { }
    }
    return;
  }
  if (flushAmplitude.timer || ampTries >= 30) return;
  ampTries++;
  flushAmplitude.timer = setTimeout(() => { flushAmplitude.timer = 0; flushAmplitude(); }, 2000);
}

//////// clicks
// name of a button for its click event: id, then its data attribute, then its kind
function buttonName(b) {
  if (b.id) return b.id.replace(/^btn-/, '');
  for (const k of ['tab', 'help', 'reset', 'bg']) {
    if (b.dataset[k] !== undefined) return k + '_' + b.dataset[k];
  }
  const kinds = {
    'filter-item': 'filter_group', 'cfg-row': 'save_slot', 'cfg-row-main': 'preset_load',
    'cfg-row-del': 'preset_delete', 'strip-del': 'snapshot_discard', 'boot-reload': 'boot_reload'
  };
  for (const c of b.classList) {
    if (kinds[c]) return kinds[c];
  }
  return b.classList[0] || 'button';
}

// capture phase, so buttons that stop their own clicks are counted too
document.addEventListener('click', e => {
  const b = e.target.closest && e.target.closest('button, [role="button"]');
  if (!b) return;
  const props = b.classList.contains('filter-item') ? { group: b.textContent.trim().slice(0, 60) } : {};
  track(('click_' + buttonName(b)).replace(/[^a-zA-Z0-9_]/g, '_').slice(0, 40), props);
}, true);


//////////////// BOOT

// icons are used by #id, that only works if the sprite is inline in the page
async function loadSprite() {
  try {
    const res = await fetch('resources/sprite.svg');
    if (!res.ok) throw new Error(res.status);
    const host = document.createElement('div');
    host.style.display = 'none';
    host.innerHTML = await res.text();
    document.body.insertBefore(host, document.body.firstChild);
  } catch {
  }
}

loadSprite();

function bootCannotRun(reason, detail = '') {
  track('engine_failed', { reason, detail: String(detail).slice(0, 100) });
  el.boot.classList.add('is-error');
  el.bootMsg.textContent = "This browser can't run the converter engine — try the latest Chrome, Firefox, or Edge.";
}

// tested: page not isolated = engine cannot pass its memory to its threads and waits for ever, so say it now
if (!self.crossOriginIsolated) {
  bootCannotRun('not_isolated');
  console.error('[boot] page is not cross-origin isolated — check the COOP/COEP headers for this browser');
} else {
  createMuralModule().then(mod => {
    M = mod;
    el.bootMsg.textContent = 'Decoding block textures…';
    api.init();
    pollTimer = setInterval(tick, 100);
  }).catch(err => {
    bootCannotRun('load_error', err && err.message || err);
    console.error('[boot] engine failed to load — check that the page is served with COOP/COEP headers', err);
  });
}

//////// start guard
// tested: one worker thread whose script never arrives = engine waits for ever, nothing said
const BOOT_RETRY_KEY = 'mmmm-boot-retry';
const BOOT_STALL_MS = 20000;    // counted from the engine files being in, a slow download is not a stall

function bootFilesIn() {
  const got = performance.getEntriesByType('resource');
  return ['mural.wasm', 'mural.data'].every(name => got.some(e => e.responseEnd > 0 && e.name.split('?')[0].endsWith(name)));
}

let bootFilesAt = 0;
const bootGuard = setInterval(() => {
  if (!el.boot.isConnected || el.boot.classList.contains('is-error')) { clearInterval(bootGuard); return; }
  if (!bootFilesAt) { if (bootFilesIn()) bootFilesAt = performance.now(); return; }
  if (performance.now() - bootFilesAt < BOOT_STALL_MS) return;
  clearInterval(bootGuard);
  bootStuck();
}, 1000);

function bootStuck() {
  // no storage = no way to count the tries, so no reload without asking
  let first = false;
  try {
    first = sessionStorage.getItem(BOOT_RETRY_KEY) === null;
    if (first) sessionStorage.setItem(BOOT_RETRY_KEY, '1');
  } catch (_) { }

  // first time: reload without asking, nothing is loaded yet that could get lost
  if (first) { location.reload(); return; }

  el.boot.classList.add('is-error');
  el.bootMsg.textContent = 'The engine did not start. Reload the page to try again.';

  // where it stopped, for a bug report -- the first mural.js is the page loading the engine itself
  const threads = Math.max(0, performance.getEntriesByType('resource').filter(e => e.name.split('?')[0].endsWith('mural.js')).length - 1);
  const where = document.createElement('p');
  where.className = 'boot-detail';
  where.textContent = !M ? `Stopped while starting worker threads (${threads} loaded).` : 'Stopped while decoding block textures.';
  el.bootMsg.after(where);
  track('engine_failed', { reason: 'did_not_start', stage: !M ? 'threads' : 'textures', threads });

  const again = document.createElement('button');
  again.type = 'button';
  again.className = 'btn boot-reload';
  again.textContent = 'Reload';
  again.addEventListener('click', () => location.reload());
  where.after(again);
}

function onDatabaseReady() {
  blocks = JSON.parse(api.blocksJson());
  blocks.forEach((b, i) => { b.atlasIndex = i; });    // position in the icon atlas, sorting below would lose it
  blocks.sort((a, b) => a.ord - b.ord);
  overlays = JSON.parse(api.overlaysJson());

  versions = JSON.parse(api.versionsJson());
  overlayVersions = JSON.parse(api.overlayVersionsJson());
  S.version = Math.max(0, versions.length - 1);

  buildAtlasVar('--block-atlas', api.blockAtlas(), api.blockAtlasCols(), blocks.length);
  buildAtlasVar('--overlay-atlas', api.overlayAtlas(), api.overlayAtlasCols(), overlays.length);

  buildFilterGroups();

  // creative-only blocks start disallowed
  blocks.forEach(b => { if (!isCreative(b)) allowedSet.add(b.id); });
  overlayAllowedSet = new Set();

  buildSelects();
  buildSliders();
  wireControls();
  wireConfigTools();
  wireSnapshotUI();
  pushAllSettings();
  renderBlockLists();
  renderOverlayLists();
  refreshSwatches();
  restoreAfterCrash();    // only does something after an out-of-memory reload

  try { sessionStorage.removeItem(BOOT_RETRY_KEY); } catch (_) { }
  track('engine_ready', { ms: Math.round(performance.now()) });
  el.boot.remove();
  el.app.hidden = false;
  requestDraw();

  if (!blocks.length) {
    toast('No blocks loaded — check that block_textures/ and allowed_ids.csv are in the preload package.');
  }
}

//////// icon atlases
const atlasUrls = {};

function buildAtlasVar(varName, ptr, cols, count) {
  const rows = Math.max(1, Math.ceil(count / cols));
  const w = cols * 16, h = rows * 16;
  const bytes = new Uint8ClampedArray(w * h * 4);
  bytes.set(new Uint8Array(M.HEAPU8.buffer, ptr, w * h * 4));

  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  c.getContext('2d').putImageData(new ImageData(bytes, w, h), 0, 0);

  // tested: a data url got decoded again on every list repaint (about 1 s), a blob url doesnt
  const png = atob(c.toDataURL('image/png').split(',')[1]);
  const raw = new Uint8Array(png.length);
  for (let i = 0; i < png.length; i++) raw[i] = png.charCodeAt(i);
  const url = URL.createObjectURL(new Blob([raw], { type: 'image/png' }));
  document.documentElement.style.setProperty(varName, `url(${url})`);
  if (atlasUrls[varName]) URL.revokeObjectURL(atlasUrls[varName]);
  atlasUrls[varName] = url;
}

function iconPos(index, cols) {
  return `${-(index % cols) * 16}px ${-Math.floor(index / cols) * 16}px`;
}


//////////////// CONTROLS
function fillSelect(select, items, value) {
  select.innerHTML = '';
  items.forEach((label, i) => {
    const o = document.createElement('option');
    o.value = String(i);
    o.textContent = label;
    select.appendChild(o);
  });
  select.value = String(value);
}

function buildSelects() {
  fillSelect(el.version, versions, S.version);
  fillSelect(el.biome, BIOMES, S.biome);
  fillSelect(el.dither, DITHER, S.dither.method);
  el.chunk.replaceChildren(...CHUNKS.map(([idx, label]) => new Option(label, String(idx))));
  syncPass2Dom();
  syncEnhanceDom();
  refreshFacing();
}

function refreshFacing() {
  const vertical = S.orientation === 0;
  fillSelect(el.facing, vertical ? FACING_VERT : FACING_HORZ, vertical ? S.dirVert : S.dirHorz);
}

//////// sliders
const sliders = new Map();

class Slider {
  constructor(host) {
    this.host = host;
    this.key = host.dataset.slider;
    this.min = parseFloat(host.dataset.min);
    this.max = parseFloat(host.dataset.max);
    this.step = parseFloat(host.dataset.step);
    this.dp = parseInt(host.dataset.dp, 10);
    this.label = host.dataset.label || '';
    this.suffix = host.dataset.suffix || '';
    this.displayScale = parseFloat(host.dataset.displayScale || '1');
    this.wheelStep = this.label ? (this.max - this.min) * 0.02 : this.step;

    this.input = document.createElement('input');
    this.input.type = 'range';
    this.input.min = this.min; this.input.max = this.max; this.input.step = this.step;
    this.input.setAttribute('aria-label', this.label || this.key);

    this.text = document.createElement('span');
    this.text.className = 'sv';

    host.append(this.input, this.text);

    this.input.addEventListener('input', () => this.commit(parseFloat(this.input.value)));
  }

  get value() { return parseFloat(this.input.value); }

  set(v, silent) {
    const clamped = Math.min(this.max, Math.max(this.min, v));
    this.input.value = String(clamped);
    this.text.textContent =
      (this.label ? this.label + ': ' : '') + (clamped * this.displayScale).toFixed(this.dp) + this.suffix;
    if (!silent) onSliderChanged(this.key, clamped);
  }

  commit(v) { this.set(v, false); }
}

function buildSliders() {
  $$('.slider').forEach(host => {
    const s = new Slider(host);
    sliders.set(s.key, s);
  });
  syncSlidersFromState(true);
}

function syncSlidersFromState(silent) {
  const map = {
    denoise: S.tweaks.denoise, sharpen: S.tweaks.sharpen, contrast: S.tweaks.contrast,
    bright: S.tweaks.bright, sat: S.tweaks.sat, hue: S.tweaks.hue,
    wColor: S.weights.wColor, wSpatial: S.weights.wSpatial, wTexture: S.weights.wTexture,
    ditherIntensity: S.dither.intensity,
    pass2Threshold: S.pass2.threshold, wMSE: S.pass2.wMSE, wSSIM: S.pass2.wSSIM,
    pass2Passes: S.pass2.passes || 1, pass2Candidates: S.pass2.candidates || 3,
    enhGrain: S.enhance.grain, enhFlat: S.enhance.flat, enhProtect: S.enhance.protect
  };
  for (const [key, value] of Object.entries(map)) sliders.get(key)?.set(value, silent);
}

function onSliderChanged(key, v) {

  // slider of a switched-off feature: value is kept, no new preview
  switch (key) {
    case 'denoise': case 'sharpen': case 'contrast':
    case 'bright': case 'sat': case 'hue':
      S.tweaks[key] = v; api.setTweaks(S.tweaks); break;
    case 'wColor': case 'wSpatial': case 'wTexture':
      S.weights[key] = v; api.setWeights(S.weights); break;
    case 'ditherIntensity':
      S.dither.intensity = v; api.setDither(S.dither);
      if (S.dither.method === 0) return;
      break;
    case 'pass2Threshold': S.pass2.threshold = v; api.setPass2(S.pass2); if (!S.pass2.enable) return; break;
    case 'wMSE':  S.pass2.wMSE = v;  api.setPass2(S.pass2); if (!S.pass2.enable) return; break;
    case 'wSSIM': S.pass2.wSSIM = v; api.setPass2(S.pass2); if (!S.pass2.enable) return; break;
    case 'pass2Passes': S.pass2.passes = Math.round(v); api.setPass2(S.pass2); if (!S.pass2.enable) return; break;
    case 'pass2Candidates': S.pass2.candidates = Math.round(v); api.setPass2(S.pass2); if (!S.pass2.enable) return; break;
    case 'enhGrain':   S.enhance.grain = v;   api.setEnhance(S.enhance); if (!S.enhance.grainOn) return; break;
    case 'enhFlat':    S.enhance.flat = v;    api.setEnhance(S.enhance); if (!S.enhance.flatOn) return; break;
    case 'enhProtect': S.enhance.protect = v; api.setEnhance(S.enhance); if (!(S.enhance.protectOn && searchWanted(S.enhance))) return; break;
  }
  schedulePreview();
}

//////// refinement and experimental boxes
function searchWanted(e) {
  return !!(e.grainOn || e.flatOn);
}

function syncPass2Dom() {
  el.pass2.checked = S.pass2.enable;
  el.pass2Fields.disabled = !S.pass2.enable;
  el.chunk.value = String(S.pass2.chunk);
  el.thresholdRow.hidden = S.pass2.chunk === CHUNK_WHOLE;    // whole image is one patch, no worst share to pick
}

function syncEnhanceDom() {
  const e = S.enhance, search = searchWanted(e);
  el.expGrain.checked = e.grainOn;
  el.expFlat.checked = e.flatOn;
  el.expProtect.checked = e.protectOn;
  el.enhFeather.checked = e.feather;
  el.expProtect.disabled = !search;    // only does anything inside the block search
  $('#row-exp-grain').classList.toggle('is-off', !e.grainOn);
  $('#row-exp-flat').classList.toggle('is-off', !e.flatOn);
  $('#row-exp-protect').classList.toggle('is-off', !(e.protectOn && search));
}

//////// engine sync
function pushAllSettings() {
  api.setGameVersion(S.version);
  api.setSize(S.width, S.height, S.lock);
  api.setName(S.name);
  api.setPlacement(S.orientation, S.dirVert, S.dirHorz, S.biome);
  api.setBgMode(S.bgMode);
  api.setTweaks(S.tweaks);
  api.setWeights(S.weights);
  api.setDither(S.dither);
  api.setPass2(S.pass2);
  api.setEnhance(S.enhance);
  api.setUseOverlays(S.useOverlays);
  pushAllowedBlocks();
  pushAllowedOverlays();
}

function pushPlacement() {
  api.setPlacement(S.orientation, S.dirVert, S.dirHorz, S.biome);
  schedulePreview();
}

//////// events
function wireControls() {

  // tabs
  $$('.tab').forEach(tab => tab.addEventListener('click', () => {
    $$('.tab').forEach(t => t.classList.toggle('is-active', t === tab));
    $$('.tabpane').forEach(p => p.classList.toggle('is-active', p.id === 'pane-' + tab.dataset.tab));
  }));

  // image
  $('#btn-browse').addEventListener('click', () => el.file.click());
  el.file.addEventListener('change', () => {
    if (el.file.files[0]) loadImageFile(el.file.files[0]);
    el.file.value = '';    // otherwise picking the same file again does nothing
  });

  // basic settings
  const MAX_SIZE = 1000;    // tested: bigger doesnt fit in browser memory (blocks per side)
  const applySize = source => {
    const rawW = parseInt(el.width.value, 10);
    const rawH = parseInt(el.height.value, 10);
    let w = clampInt(el.width.value, 1, MAX_SIZE);
    let h = clampInt(el.height.value, 1, MAX_SIZE);
    const aspect = S.aspect || 0;

    let hitLimit = (Number.isFinite(rawW) && rawW > MAX_SIZE) || (Number.isFinite(rawH) && rawH > MAX_SIZE);

    if (S.lock && aspect > 0) {
      if (source === 'w') {
        h = Math.max(1, Math.round(w / aspect));
        if (h > MAX_SIZE) { h = MAX_SIZE; w = Math.max(1, Math.round(h * aspect)); hitLimit = true; }
      } else {
        w = Math.max(1, Math.round(h * aspect));
        if (w > MAX_SIZE) { w = MAX_SIZE; h = Math.max(1, Math.round(w / aspect)); hitLimit = true; }
      }
    }
    if (hitLimit) toast(`Murals are capped at ${MAX_SIZE} blocks per side.`);

    S.width = w; S.height = h;
    el.width.value = w; el.height.value = h;
    api.setSize(w, h, S.lock);
    pendingAutoFit = true;
    schedulePreview();
  };
  el.width.addEventListener('change', () => applySize('w'));
  el.height.addEventListener('change', () => applySize('h'));
  el.lock.addEventListener('change', () => {
    S.lock = el.lock.checked;
    api.setSize(S.width, S.height, S.lock);
  });

  el.name.addEventListener('input', () => { S.name = el.name.value || 'my_mural'; api.setName(S.name); });

  // placement and game version
  el.orientation.addEventListener('change', () => {
    S.orientation = parseInt(el.orientation.value, 10);
    refreshFacing();
    pushPlacement();
  });
  el.facing.addEventListener('change', () => {
    const v = parseInt(el.facing.value, 10);
    if (S.orientation === 0) S.dirVert = v; else S.dirHorz = v;
    pushPlacement();
  });
  el.biome.addEventListener('change', () => {
    S.biome = parseInt(el.biome.value, 10);
    pushPlacement();
    refreshSwatches();
    buildAtlasVar('--overlay-atlas', api.overlayAtlas(), api.overlayAtlasCols(), overlays.length);
  });
  el.version.addEventListener('change', () => {
    applyVersion(parseInt(el.version.value, 10));
  });

  // dithering and refinement
  el.dither.addEventListener('change', () => {
    S.dither.method = parseInt(el.dither.value, 10);
    api.setDither(S.dither);
    schedulePreview();
  });
  el.pass2.addEventListener('change', () => {
    S.pass2.enable = el.pass2.checked;
    syncPass2Dom();
    api.setPass2(S.pass2);
    schedulePreview();
  });
  el.chunk.addEventListener('change', () => {
    S.pass2.chunk = parseInt(el.chunk.value, 10);
    syncPass2Dom();
    api.setPass2(S.pass2);
    if (S.pass2.enable) schedulePreview();
  });

  // experimental
  [[el.expGrain, 'grainOn'], [el.expFlat, 'flatOn'], [el.expProtect, 'protectOn'], [el.enhFeather, 'feather']]
    .forEach(([box, key]) => box.addEventListener('change', () => {
      S.enhance[key] = box.checked;
      syncEnhanceDom();
      api.setEnhance(S.enhance);
      schedulePreview();
    }));

  // resets
  $$('.reset').forEach(btn => btn.addEventListener('click', () => resetGroup(btn.dataset.reset)));
  $('#btn-cfg-reset').addEventListener('click', resetAllSettings);

  // block palette
  wireSearch();

  wireList(el.listAllowed, selAllowed, () => renderBlockLists());
  wireList(el.listDisallowed, selDisallowed, () => renderBlockLists());
  wireList(el.listOvAllowed, selOvAllowed, () => renderOverlayLists());
  wireList(el.listOvDisallowed, selOvDisallowed, () => renderOverlayLists());

  el.moveBtn.addEventListener('click', moveSelectedBlocks);
  $('#btn-all-allow').addEventListener('click', () => {
    const match = searchQuery();
    if (match) availableBlocks().forEach(b => { if (match(b)) allowedSet.add(b.id); });
    else allowedSet = new Set(availableBlocks().map(b => b.id));
    afterBlockChange();
  });
  $('#btn-all-disallow').addEventListener('click', () => {
    const match = searchQuery();
    if (match) availableBlocks().forEach(b => { if (match(b)) allowedSet.delete(b.id); });
    else allowedSet = new Set();
    afterBlockChange();
  });
  $('#btn-deselect').addEventListener('click', () => { selAllowed.clear(); selDisallowed.clear(); renderBlockLists(); });
  $('#btn-moose').addEventListener('click', () => {
    allowedSet = new Set(availableBlocks().filter(b => b.moose).map(b => b.id));
    afterBlockChange();
  });

  // overlays
  el.overlays.addEventListener('change', () => {
    S.useOverlays = el.overlays.checked;
    el.overlayFields.disabled = !S.useOverlays;
    api.setUseOverlays(S.useOverlays);
    schedulePreview();
  });
  el.ovMoveBtn.addEventListener('click', moveSelectedOverlays);
  $('#btn-ov-all-allow').addEventListener('click', () => { overlayAllowedSet = new Set(availableOverlays()); afterOverlayChange(); });
  $('#btn-ov-all-disallow').addEventListener('click', () => { overlayAllowedSet = new Set(); afterOverlayChange(); });
  el.overlayFields.disabled = true;

  // preview toolbar
  $$('#bg-picker .swatch').forEach(sw => sw.addEventListener('click', () => {
    const mode = parseInt(sw.dataset.bg, 10);
    if (mode === S.bgMode) return;
    S.bgMode = mode;
    api.setBgMode(mode);
    refreshSwatches();
    schedulePreview();
  }));
  $('#btn-reset-view').addEventListener('click', () => { autoFit = true; requestDraw(); });

  // exports
  $('#btn-datapack').addEventListener('click', () => {
    if (!api.hasImage()) return toast('Load an image first.');
    if (api.isProcessing()) return toast('Still processing — try again in a moment.');
    dbg('[JS] Triggering Datapack generation in C++...');
    toast('Generating Datapack...');
    track('export_made', { type: 'datapack', width: preview.w, height: preview.h, version: versions[S.version] || '' });
    api.generateDatapack();
  });

  $('#btn-litematic').addEventListener('click', () => {
    if (!api.hasImage()) return toast('Load an image first.');
    if (api.isProcessing()) return toast('Still processing — try again in a moment.');
    dbg('[JS] Triggering Litematic generation in C++...');
    toast('Generating Litematic schematic...');
    track('export_made', { type: 'litematic', width: preview.w, height: preview.h, version: versions[S.version] || '' });
    api.generateLitematic();
  });

  // scroll to top button
  const scrollBtn = $('#btn-scroll-top');
  const checkScroll = () => {
    const isMobile = window.matchMedia("(max-width: 1100px)").matches;
    scrollBtn.classList.toggle('is-visible', isMobile && window.scrollY > 150);
  };

  window.addEventListener('scroll', checkScroll);
  window.addEventListener('resize', checkScroll);

  scrollBtn.addEventListener('click', () => {
    window.scrollTo({ top: 0, behavior: 'smooth' });
  });

  // parts with their own setup
  wireHelp();
  wireCanvas();
  wireDropTarget();
  wireSplitter();
  wireConsoleFold();
}


//////////////// HELP POPOVERS
const HELP_TEXT = {
  tweaks: {
    title: 'Image pre-processing',
    body: 'Applied to the source image before actual block-matching starts. Pretty much what you could do in a photo editor before uploading',
    items: [
      ['Noise redux', 'Blurs away speckle and JPEG artefacts. High values flatten fine detail into solid areas.'],
      ['Sharpness', 'Boosts local contrast at edges, which may help small murals keep their outlines.'],
      ['Contrast', 'Spreads light and dark apart. Above 1 deepens shadows, below 1 flattens the image.'],
      ['Brightness', 'Shifts everything lighter or darker. Useful when a palette has no dark blocks to match with.'],
      ['Saturation', 'Colour intensity. 0 is greyscale, which is worth trying when a palette is mostly stone.'],
      ['Hue shift', 'Rotates every colour around the wheel. Lets you steer an image toward the hues a palette actually has.']
    ]
  },
  placement: {
    title: 'Placement in world',
    body: 'How the finished mural sits in the world. This changes which face of each block you see and changes the possible block rotations.',
    items: [
      ['Orientation', 'Vertical builds a wall, horizontal builds a floor or ceiling.'],
      ['Facing', 'Which way the viewer stands. Affects multi-face blocks and rotations.'],
      ['Biome tint', 'Grass, leaves, leaf litter and vines take their colour from the biome. Pick the one you will build in or they will not match the preview.']
    ]
  },
  weights: {
    title: 'Algorithm tuning',
    body: 'How the matcher scores a candidate block against a target chunk of the image. The three are relative, so only their ratio matters.',
    items: [
      ['Macro colour', 'Average colour of the whole block. Raise it for smooth gradients.'],
      ['Micro spatial', 'Where the light and dark parts sit inside the texture. Raise it to keep edges and outlines readable.'],
      ['Micro texture', 'How noisy or busy the texture is. Raise it to match grain, lower it to avoid visual clutter.']
    ]
  },
  dither: {
    title: 'Dithering',
    body: 'Trades a bit of noise for a wider apparent colour range by mixing neighbouring blocks. More effective on larger murals',
    items: [
      ['Floyd-Steinberg', 'Pushes error to neighbours as it scans. Best gradients, but slow to compute.'],
      ['Atkinson', 'Spreads less error than Floyd-Steinberg, so it keeps contrast but can cause banding. Slow to compute'],
      ['Bayer 4x4 / 8x8', 'A fixed crosshatch. Fast and predictable, and it tiles rather than drifting.'],
      ['Random noise', 'Scatters error randomly. Hides banding but looks grainy up close.'],
      ['Intensity', 'How strongly. At 1 the dithering is complete; at lower values refinement and feathering keep more of their say.']
    ]
  },
  enhance: {
    title: 'Experimental',
    body: 'Extra refinements that run after everything above. That search is slow and limited to murals of up to 90k blocks.',
    items: [
      ['Grain in flat areas', 'How rough a texture of block could be where the picture is smooth. Low keeps skin and sky to calm blocks, high lets grainy ones in.'],
      ['Flat areas: colour over texture', 'How strongly a block is chosen by its overall colour instead of its pattern in smooth areas.'],
      ['Protect fine texture', 'Prevents post-processing refinements from altering areas of fine, messy detail (hair, fur, foliage, etc.).'],
      ['Feathering', 'Smoothes the harsh edges of two tones in gradients by spreading the transition.']
    ]
  },
  pass2: {
    title: 'Multi-pass refinement',
    body: 'After the first pass, the worst-matching patches are revisited and re-solved with the context of nearby blocks.',
    items: [
      ['Error matrix size', 'How large a patch is judged at once.'],
      ['Passes', 'How many times each revisited patch is gone over.'],
      ['Candidates per block', 'How many top candidates each block may switch between. More gives the refinement more to try and makes the first pass slower.'],
      ['Error-seeking threshold', 'What share of the mural gets revisited. 0.25 means the worst quarter.'],
      ['Recursive denoising (MSE)', 'Weights raw colour error. Raise it to reduce speckle.'],
      ['Edge preservation (SSIM)', 'Weights structural similarity. Raise it to protect edges and shapes.']
    ]
  },
  bg: {
    title: 'Background colour',
    body: 'What sits behind transparent and partly transparent blocks, both in this preview and in the matching itself.',
    items: [
      ['Dark / Light', 'A neutral backdrop. Use the one closest to what will actually be behind the mural.'],
      ['Biome grass', 'Matches the tint of the selected biome.'],
      ['Transparent', 'Transparency of the image is transferred directly into the picked blocks. Requires a palette with transparent blocks.']
    ]
  },
  overlays: {
    title: 'Overlays (experimental)',
    body: 'Places a second thin block on top of the base one to reach colours and textures the base palette cannot.',
    items: [
      ['Performance', 'Multiplies the palette by every allowed overlay state, so calculation gets slower and uses far more memory. Particularly slow when used with Floyd-Steinberg and Atkinson dithering algorithms.'],
      ['In game', 'Surface and adjacency rules (for redstone dust and rails) are considered in both preview and the matching process. Rail placement may require some grinding when rebuilt in survival.']
    ]
  }
};

let helpPop = null;

function wireHelp() {
  helpPop = $('#help-pop');
  if (!helpPop) return;

  const close = () => { helpPop.hidden = true; };

  $$('.help').forEach(btn => {
    const open = () => {
      const info = HELP_TEXT[btn.dataset.help];
      if (!info) return;

      helpPop.replaceChildren();

      const title = document.createElement('div');
      title.className = 'cfg-menu-title';
      title.textContent = info.title;
      helpPop.appendChild(title);

      const body = document.createElement('p');
      body.className = 'help-body';
      body.textContent = info.body;
      helpPop.appendChild(body);

      const list = document.createElement('dl');
      list.className = 'help-list';
      info.items.forEach(([term, text]) => {
        const dt = document.createElement('dt');
        dt.textContent = term;
        const dd = document.createElement('dd');
        dd.textContent = text;
        list.append(dt, dd);
      });
      helpPop.appendChild(list);

      helpPop.hidden = false;

      // under the marker, above it if theres no room
      const r = btn.getBoundingClientRect();
      const w = helpPop.offsetWidth || 300;
      const h = helpPop.offsetHeight || 200;
      let left = Math.min(r.left, window.innerWidth - w - 8);
      let top = r.bottom + 6;
      if (top + h > window.innerHeight - 8) top = Math.max(8, r.top - h - 6);
      helpPop.style.left = Math.max(8, left) + 'px';
      helpPop.style.top = top + 'px';
    };

    btn.addEventListener('mouseenter', open);
    btn.addEventListener('focus', open);
    btn.addEventListener('mouseleave', close);
    btn.addEventListener('blur', close);

    // no hover on touch, a tap toggles it
    btn.addEventListener('click', e => {
      e.preventDefault();
      e.stopPropagation();
      if (helpPop.hidden) open(); else close();
    });
  });

  helpPop.addEventListener('mouseenter', () => { helpPop.hidden = false; });
  helpPop.addEventListener('mouseleave', close);
  document.addEventListener('click', e => {
    if (!helpPop.hidden && !helpPop.contains(e.target) && !e.target.closest('.help')) close();
  });
  document.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });
  window.addEventListener('scroll', close, true);    // capture: scrolling an inner panel should close it too
}


//////////////// RESETS
function resetAllSettings() {

  // placement stays -- its where the mural stands, not how its tuned
  ['tweaks', 'weights', 'dither', 'pass2', 'enhance'].forEach(applyGroupDefaults);

  S.bgMode = 0;
  api.setBgMode(0);
  refreshSwatches();

  syncSlidersFromState(true);
  schedulePreview();
  logAction('Settings reset to defaults.');
}

// resetGroup for several groups in a row: no slider sync, no preview each time
function applyGroupDefaults(name) {
  if (name === 'tweaks') {
    S.tweaks = structuredClone(DEFAULTS.tweaks);
    api.setTweaks(S.tweaks);
  } else if (name === 'weights') {
    S.weights = structuredClone(DEFAULTS.weights);
    api.setWeights(S.weights);
  } else if (name === 'dither') {
    S.dither = structuredClone(DEFAULTS.dither);
    el.dither.value = String(S.dither.method);
    api.setDither(S.dither);
  } else if (name === 'pass2') {
    S.pass2 = structuredClone(DEFAULTS.pass2);
    syncPass2Dom();
    api.setPass2(S.pass2);
  } else if (name === 'enhance') {
    S.enhance = structuredClone(DEFAULTS.enhance);
    syncEnhanceDom();
    api.setEnhance(S.enhance);
  } else if (name === 'placement') {
    Object.assign(S, structuredClone(DEFAULTS.placement));
    el.orientation.value = String(S.orientation);
    el.biome.value = String(S.biome);
    refreshFacing();
    api.setPlacement(S.orientation, S.dirVert, S.dirHorz, S.biome);
    refreshSwatches();
  }
}

function resetGroup(name) {
  if (name === 'tweaks') {
    S.tweaks = structuredClone(DEFAULTS.tweaks);
    api.setTweaks(S.tweaks);
  } else if (name === 'weights') {
    S.weights = structuredClone(DEFAULTS.weights);
    api.setWeights(S.weights);
  } else if (name === 'dither') {
    S.dither = structuredClone(DEFAULTS.dither);
    el.dither.value = String(S.dither.method);
    api.setDither(S.dither);
  } else if (name === 'pass2') {
    S.pass2 = structuredClone(DEFAULTS.pass2);
    syncPass2Dom();
    api.setPass2(S.pass2);
  } else if (name === 'enhance') {
    const d = DEFAULTS.enhance;    // amounts only, ticked boxes stay as they are
    S.enhance = { ...S.enhance, grain: d.grain, flat: d.flat, protect: d.protect };
    syncEnhanceDom();
    api.setEnhance(S.enhance);
  } else if (name === 'placement') {
    Object.assign(S, structuredClone(DEFAULTS.placement));
    el.orientation.value = String(S.orientation);
    el.biome.value = String(S.biome);
    refreshFacing();
    api.setPlacement(S.orientation, S.dirVert, S.dirHorz, S.biome);
    refreshSwatches();
  }
  syncSlidersFromState(true);
  schedulePreview();
}


//////////////// BLOCK LISTS
function availableBlocks() {
  return blocks.filter(b => b.ver <= S.version);
}

function availableOverlays() {
  return overlays.filter((id, i) => overlayVersions[i] <= S.version);
}

function pushAllowedBlocks() {
  api.setAllowedBlocks(availableBlocks().filter(b => allowedSet.has(b.id)).map(b => b.id).join(','));
}

function pushAllowedOverlays() {
  api.setAllowedOverlays(availableOverlays().filter(id => overlayAllowedSet.has(id)).join(','));
}

function applyVersion(next) {
  const prev = S.version;
  if (next === prev) return;
  S.version = next;

  if (next > prev) {
    blocks.forEach(b => { if (b.ver > prev && b.ver <= next && !isCreative(b)) allowedSet.add(b.id); });
  } else {
    blocks.forEach(b => { if (b.ver > next) allowedSet.delete(b.id); });
    overlays.forEach((id, i) => { if (overlayVersions[i] > next) overlayAllowedSet.delete(id); });
  }

  api.setGameVersion(next);
  afterBlockChange();
  afterOverlayChange();
}

function afterBlockChange() {
  selAllowed.clear(); selDisallowed.clear();
  pushAllowedBlocks();
  renderBlockLists();
  schedulePreview();
}

function afterOverlayChange() {
  selOvAllowed.clear(); selOvDisallowed.clear();
  pushAllowedOverlays();
  renderOverlayLists();
  if (S.useOverlays) schedulePreview();
}

function renderBlockLists() {
  const match = searchQuery();
  const avail = availableBlocks();
  const allowed = avail.filter(b => allowedSet.has(b.id));
  const disallowed = avail.filter(b => !allowedSet.has(b.id));
  const cols = api.blockAtlasCols();

  paintList(el.listAllowed, allowed, selAllowed, match, cols, false);
  paintList(el.listDisallowed, disallowed, selDisallowed, match, cols, false);
  refreshMoveButton();

  $('#btn-all-allow').title = match ? 'Allow all matching blocks' : 'Allow all blocks';
  $('#btn-all-disallow').title = match ? 'Disallow all matching blocks' : 'Disallow all blocks';
}

function renderOverlayLists() {
  const cols = api.overlayAtlasCols();
  const avail = availableOverlays();
  const allowed = avail.filter(id => overlayAllowedSet.has(id));
  const disallowed = avail.filter(id => !overlayAllowedSet.has(id));
  const wrap = id => ({ id, atlasIndex: overlays.indexOf(id) });

  paintList(el.listOvAllowed, allowed.map(wrap), selOvAllowed, null, cols, true);
  paintList(el.listOvDisallowed, disallowed.map(wrap), selOvDisallowed, null, cols, true);
  refreshOverlayMoveButton();
}

function paintList(container, items, selected, match, cols, isOverlay) {
  const frag = document.createDocumentFragment();
  const matched = [], missed = [];

  items.forEach(item => {
    if (!match || match(item)) matched.push(item);
    else missed.push(item);
  });

  const add = (item, dim) => {
    const index = item.atlasIndex;
    const row = document.createElement('div');
    row.className = 'brow' + (selected.has(item.id) ? ' is-selected' : '') + (dim ? ' is-dim' : '');
    row.dataset.id = item.id;

    const ico = document.createElement('i');
    ico.className = 'ico' + (isOverlay ? ' ico--ov' : '');
    ico.style.backgroundPosition = iconPos(index, cols);

    const label = document.createElement('span');
    label.textContent = item.id;

    row.append(ico, label);
    frag.appendChild(row);
  };

  matched.forEach(i => add(i, false));
  missed.forEach(i => add(i, true));

  container.replaceChildren(frag);
}

//////// drag selection

// on phones only the left part of a row drag-selects, the rest scrolls (share of row width)
const DRAG_ZONE = 0.6;

let dragging = false, dragSelects = false;    // dragSelects: this drag adds rows, or removes them

function wireList(container, selected, rerender) {
  let lastId = null;

  const apply = id => {
    if (dragSelects) selected.add(id); else selected.delete(id);
    const row = container.querySelector(`.brow[data-id="${CSS.escape(id)}"]`);
    if (row) row.classList.toggle('is-selected', dragSelects);
  };

  const inDragZone = clientX => {
    const r = container.getBoundingClientRect();
    return clientX <= r.left + r.width * DRAG_ZONE;
  };

  // non-passive, otherwise the scroll cant be blocked inside the drag zone
  container.addEventListener('touchstart', e => {
    if (!isMobile()) return;
    const t = e.touches[0];
    if (!t || !e.target.closest('.brow')) return;
    if (inDragZone(t.clientX)) e.preventDefault();
  }, { passive: false });

  container.addEventListener('pointerdown', e => {
    const row = e.target.closest('.brow');
    if (!row) return;
    if (e.pointerType === 'touch' && isMobile() && !inDragZone(e.clientX)) return;

    e.preventDefault();

    // keeps moves coming when the pointer leaves the list
    try { container.setPointerCapture(e.pointerId); } catch { }

    dragging = true;
    dragSelects = !selected.has(row.dataset.id);
    lastId = row.dataset.id;
    apply(lastId);
    refreshMoveButton();
    refreshOverlayMoveButton();
  });

  container.addEventListener('pointermove', e => {
    if (!dragging) return;

    // a captured pointer always reports the list as target, so find the row by position
    const under = document.elementFromPoint(e.clientX, e.clientY);
    const row = under && under.closest('.brow');
    if (!row || !container.contains(row)) return;
    if (row.dataset.id === lastId) return;

    lastId = row.dataset.id;
    apply(lastId);
    refreshMoveButton();
    refreshOverlayMoveButton();
  });

  const endDrag = e => {
    if (e && container.hasPointerCapture && container.hasPointerCapture(e.pointerId)) {
      container.releasePointerCapture(e.pointerId);
    }
    dragging = false;
    lastId = null;
  };

  container.addEventListener('pointerup', endDrag);
  container.addEventListener('pointercancel', endDrag);
}

// released outside the lists
document.addEventListener('pointerup', () => { dragging = false; });

//////// move buttons
function setMoveIcon(btn, hasLeft, hasRight) {
  const use = btn.querySelector('use');
  if (!use) return;

  let name = 'arrow_right', title = 'Move to Disallowed';
  if (hasLeft && hasRight) { name = 'arrow_swap'; title = 'Swap selections'; }
  else if (hasRight && !hasLeft) { name = 'arrow_left'; title = 'Move to Allowed'; }

  use.setAttribute('href', '#icon-' + name);
  btn.title = title;
}

const refreshMoveButton = () => setMoveIcon(el.moveBtn, selAllowed.size > 0, selDisallowed.size > 0);
const refreshOverlayMoveButton = () => setMoveIcon(el.ovMoveBtn, selOvAllowed.size > 0, selOvDisallowed.size > 0);

function moveSelectedBlocks() {
  if (!selAllowed.size && !selDisallowed.size) return;
  selAllowed.forEach(id => allowedSet.delete(id));
  selDisallowed.forEach(id => allowedSet.add(id));
  afterBlockChange();
}

function moveSelectedOverlays() {
  if (!selOvAllowed.size && !selOvDisallowed.size) return;
  selOvAllowed.forEach(id => overlayAllowedSet.delete(id));
  selOvDisallowed.forEach(id => overlayAllowedSet.add(id));
  afterOverlayChange();
}


//////////////// SEARCH AND FILTER GROUPS
let filterGroups = [];
let creativeBit = -1;    // group bit of creative-only blocks, -1 if there is none

function buildFilterGroups() {
  const props = [
    ['entity', 'Is Block Entity'], ['light', 'Emits Light'], ['gravity', 'Obeys Gravity'],
    ['transparent', 'Transparent'], ['pain', 'Is Pain to Get'], ['deteriorates', 'Deteriorates']
  ];
  filterGroups = props.map(([key, label]) => ({ key, label, test: b => !!b[key] }));
  JSON.parse(api.blockGroupsJson()).forEach((label, bit) => {
    filterGroups.push({ key: 'g' + bit, label, test: b => !!(b.groups & (1 << bit)) });
    if (/creative/i.test(label)) creativeBit = bit;
  });
}

const isCreative = b => creativeBit >= 0 && !!(b.groups & (1 << creativeBit));

const searchTags = [];
let allTagsSelected = false;    // ctrl+a pressed: tags count as selected together with the text

function searchQuery() {
  const text = el.search.value.trim().toLowerCase();
  const groups = searchTags.map(key => filterGroups.find(g => g.key === key)).filter(Boolean);
  if (!text && !groups.length) return null;

  // groups add up (any of them), text narrows what they give (text AND group)
  return b => (text === '' || b.id.toLowerCase().includes(text)) && (!groups.length || groups.some(g => g.test(b)));
}

const tagNodes = () => [...el.searchbox.querySelectorAll('.stag')];

function syncSearchUi() {
  el.searchClear.hidden = !(searchTags.length || el.search.value);
  tagNodes().forEach(n => n.classList.toggle('is-selected', allTagsSelected));
}

function renderSearchTags() {
  tagNodes().forEach(n => n.remove());
  searchTags.forEach(key => {
    const group = filterGroups.find(g => g.key === key);
    if (!group) return;
    const tag = document.createElement('span');
    tag.className = 'stag';
    tag.tabIndex = -1;
    tag.dataset.key = key;
    tag.setAttribute('role', 'button');
    tag.setAttribute('aria-label', group.label + ', group filter. Delete removes it.');
    tag.textContent = group.label;
    el.searchbox.insertBefore(tag, el.search);
  });
  syncSearchUi();
}

function searchChanged() {
  allTagsSelected = false;
  renderSearchTags();
  renderBlockLists();
}

function focusSearchText(caret) {
  el.search.focus();
  if (caret !== undefined) el.search.setSelectionRange(caret, caret);
}

function toggleSearchTag(key) {
  const at = searchTags.indexOf(key);
  if (at >= 0) searchTags.splice(at, 1); else searchTags.push(key);
  searchChanged();
}

function clearSearch() {
  searchTags.length = 0;
  el.search.value = '';
  searchChanged();
}

function wireSearch() {
  el.search.addEventListener('input', () => {
    if (allTagsSelected) searchTags.length = 0;    // typing over a select-all replaces the tags too
    searchChanged();
  });

  el.search.addEventListener('keydown', e => {
    const key = e.key;
    if ((e.ctrlKey || e.metaKey) && key.toLowerCase() === 'a') {
      allTagsSelected = searchTags.length > 0;    // no preventDefault -- browser still has to select the text
      syncSearchUi();
      return;
    }
    if (allTagsSelected && (key === 'Backspace' || key === 'Delete')) {
      e.preventDefault();
      clearSearch();
      return;
    }
    if (e.ctrlKey || e.metaKey || ['Shift', 'Control', 'Meta', 'Alt'].includes(key)) return;

    if (allTagsSelected && (key.startsWith('Arrow') || key === 'Home' || key === 'End' || key === 'Escape')) {
      allTagsSelected = false;
      syncSearchUi();
    }

    const atStart = el.search.selectionStart === 0 && el.search.selectionEnd === 0;
    if (!atStart || !searchTags.length) return;
    if (key === 'Backspace') {
      e.preventDefault();
      searchTags.pop();
      searchChanged();
    } else if (key === 'ArrowLeft' && !e.shiftKey) {
      e.preventDefault();
      tagNodes().pop().focus();
    }
  });

  el.search.addEventListener('blur', () => {
    if (!allTagsSelected) return;
    allTagsSelected = false;
    syncSearchUi();
  });

  el.searchbox.addEventListener('keydown', e => {
    const tag = e.target.closest('.stag');
    if (!tag) return;
    const tags = tagNodes(), at = tags.indexOf(tag);

    if (e.key === 'ArrowLeft' || e.key === 'Home') {
      e.preventDefault();
      tags[e.key === 'Home' ? 0 : Math.max(0, at - 1)].focus();
    } else if (e.key === 'ArrowRight') {
      e.preventDefault();
      if (at + 1 < tags.length) tags[at + 1].focus(); else focusSearchText(0);
    } else if (e.key === 'End' || e.key === 'Escape') {
      e.preventDefault();
      focusSearchText(e.key === 'End' ? el.search.value.length : 0);
    } else if (e.key === 'Backspace' || e.key === 'Delete') {
      e.preventDefault();
      searchTags.splice(at, 1);
      searchChanged();
      const left = tagNodes();
      if (at < left.length) left[at].focus(); else focusSearchText(0);
    } else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) {
      focusSearchText(0);    // focus first, then the typed character lands in the text
    }
  });

  el.searchbox.addEventListener('pointerdown', e => {
    const tag = e.target.closest('.stag');
    if (tag) {
      if (e.pointerType === 'touch' && document.activeElement === tag) {
        e.preventDefault();
        toggleSearchTag(tag.dataset.key);    // no delete key on touch: second tap on a focused tag removes it
      }
      return;
    }
    if (e.target === el.search || e.target.closest('button')) return;
    e.preventDefault();
    focusSearchText(el.search.value.length);    // a click on the padding should still land in the field
  });

  el.searchClear.addEventListener('click', () => { clearSearch(); focusSearchText(0); });

  wireFilterMenu();
}

function wireFilterMenu() {
  const menu = el.filterMenu, btn = el.filterBtn;

  const close = () => {
    menu.hidden = true;
    btn.setAttribute('aria-expanded', 'false');
  };

  const open = () => {
    menu.replaceChildren(...filterGroups.map(group => {
      const on = searchTags.includes(group.key);
      const item = document.createElement('button');
      item.type = 'button';
      item.className = 'filter-item' + (on ? ' is-on' : '');
      item.dataset.key = group.key;
      item.setAttribute('role', 'option');
      item.setAttribute('aria-selected', on ? 'true' : 'false');
      item.textContent = group.label;
      return item;
    }));
    menu.hidden = false;
    btn.setAttribute('aria-expanded', 'true');

    const r = btn.getBoundingClientRect();
    const w = menu.offsetWidth, h = menu.offsetHeight;
    let top = r.bottom + 4;
    if (top + h > window.innerHeight - 8) top = Math.max(8, r.top - h - 4);
    menu.style.left = Math.max(8, Math.min(r.right - w, window.innerWidth - w - 8)) + 'px';
    menu.style.top = top + 'px';
  };

  btn.addEventListener('click', e => {
    e.stopPropagation();
    if (menu.hidden) open(); else close();
  });

  menu.addEventListener('click', e => {
    const item = e.target.closest('.filter-item');
    if (!item) return;
    toggleSearchTag(item.dataset.key);
    close();
    if (!isMobile()) focusSearchText(el.search.value.length);    // would pop the keyboard up on phones
  });

  document.addEventListener('click', e => {
    if (!menu.hidden && !menu.contains(e.target)) close();
  });
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape' && !menu.hidden) { close(); btn.focus(); }
  });
}


//////////////// PREVIEW
const view = { zoom: 3, offX: 50, offY: 50 };

// what the last draw actually used (device px), ctrl+click needs it to find the block
const rendered = { scale: 3, ox: 50, oy: 50, dpr: 1 };

let autoFit = true;
let pendingAutoFit = false;    // fit when the next preview arrives, size isnt known before
let drawQueued = false;
let bgCss = 'rgb(21,21,21)';

const preview = {
  w: 0, h: 0,
  grid: null,    // atlas slot per block
  mean: null,    // canvas, 1 px per block, used when zoomed far out
  atlas: null,    // canvas, 16x16 tile per palette entry in use
  cols: 32
};

function refreshSwatches() {
  $$('#bg-picker .swatch').forEach(sw => {
    const mode = parseInt(sw.dataset.bg, 10);
    sw.classList.toggle('is-active', mode === S.bgMode);
    if (mode !== 3) sw.style.background = `rgb(${api.bgColor(mode)})`;
  });
  bgCss = `rgb(${api.bgColor(S.bgMode)})`;
  requestDraw();
}

//////// new preview from the engine
function onPreviewReady() {
  dbg('[JS] Engine flags ready. Fetching snapshot & rendering canvas...');
  const tRenderStart = performance.now();

  api.snapshot();

  const w = api.previewW(), h = api.previewH();
  preview.w = w; preview.h = h;

  // fit here, with the new size -- otherwise the first frame of a new mural is drawn at the old zoom
  if (pendingAutoFit && w > 0 && h > 0) {
    const dpr = window.devicePixelRatio || 1;
    const stripW = (cfgEl.strip && !cfgEl.strip.hidden) ? cfgEl.strip.offsetWidth : 0;
    const cw = Math.max(1, el.wrap.clientWidth - stripW);
    const ch = Math.max(1, el.wrap.clientHeight);
    const bw = Math.max(1, Math.round(cw * dpr)), bh = Math.max(1, Math.round(ch * dpr));
    const cssW = bw / dpr, cssH = bh / dpr;
    const pad = 30;
    let z = Math.min((cssW - pad) / w, (cssH - pad) / h);
    if (z * dpr >= 1) z = Math.floor(z * dpr) / dpr;
    view.zoom = Math.max(0.02, z);
    view.offX = (cssW - w * view.zoom) * 0.5;
    view.offY = (cssH - h * view.zoom) * 0.5;
    pendingAutoFit = false;
    autoFit = false;    // otherwise draw() fits it again
  }

  if (w > 0 && h > 0) {
    const grid = new Int32Array(w * h);    // copy -- engine reuses this memory for the next preview
    grid.set(new Int32Array(M.HEAP32.buffer, api.previewGrid(), w * h));
    preview.grid = grid;

    preview.mean = rgbaToCanvas(api.previewMean(), w, h);

    preview.cols = api.previewCols();
    const count = api.previewCount();
    const rows = Math.max(1, Math.ceil(count / preview.cols));
    preview.atlas = rgbaToCanvas(api.previewAtlas(), preview.cols * 16, rows * 16);
  } else {
    preview.grid = preview.mean = preview.atlas = null;
  }

  // with aspect lock the engine picks the other side, so read the size back
  const cfg = JSON.parse(api.configJson());
  S.aspect = cfg.aspect;

  // not while the user is typing there
  if (document.activeElement !== el.width && document.activeElement !== el.height) {
    S.width = cfg.width; S.height = cfg.height;
    el.width.value = cfg.width;
    el.height.value = cfg.height;
  }

  // image loaded but nothing to draw: show the reason from the engine on the canvas
  const stalled = preview.w === 0 && el.path.value !== '';
  const why = stalled ? (JSON.parse(api.statusJson()).warning || '') : '';
  el.emptyTitle.textContent = why ? 'Nothing to draw' : 'No preview yet';
  el.emptyHint.textContent = why || 'Drop an image anywhere, or use Browse.';
  el.empty.hidden = (preview.w > 0) || (el.path.value !== '' && !why);
  requestDraw();

  const tRenderEnd = performance.now();
  dbg(`[JS] Canvas render complete in ${(tRenderEnd - tRenderStart).toFixed(2)} ms.`);

  const ms = globalStartTime > 0 ? Math.round(tRenderEnd - globalStartTime) : null;
  if (globalStartTime > 0) {
    dbg(`[JS] Total round trip: ${(tRenderEnd - globalStartTime).toFixed(2)} ms`);
    globalStartTime = 0;
  }
  if (w > 0) trackMural(w, h, ms);
}

// sent once a mural has stood for 3 s, so a dragged slider is one event, not twenty
let muralTrackTimer = 0;
function trackMural(w, h, ms) {
  const text = sel => sel.options[sel.selectedIndex] ? sel.options[sel.selectedIndex].textContent.trim() : '';
  const props = {
    width: w, height: h, blocks: w * h, ms,
    orientation: text(el.orientation), facing: text(el.facing), version: versions[S.version] || '',
    overlays: S.useOverlays ? overlayAllowedSet.size : 0, dither: text(el.dither), refinement: !!S.pass2.enable,
    palette: availableBlocks().filter(b => allowedSet.has(b.id)).length
  };
  clearTimeout(muralTrackTimer);
  muralTrackTimer = setTimeout(() => track('mural_calculated', props), 3000);
}

//////// drawing
function rgbaToCanvas(ptr, w, h) {
  const bytes = new Uint8ClampedArray(w * h * 4);
  bytes.set(new Uint8Array(M.HEAPU8.buffer, ptr, w * h * 4));
  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  c.getContext('2d').putImageData(new ImageData(bytes, w, h), 0, 0);
  return c;
}

function requestDraw() {
  if (drawQueued) return;
  drawQueued = true;
  requestAnimationFrame(() => { drawQueued = false; draw(); });
}

function draw() {
  const dpr = window.devicePixelRatio || 1;

  const stripW = (cfgEl.strip && !cfgEl.strip.hidden) ? cfgEl.strip.offsetWidth : 0;
  const cw = Math.max(1, el.wrap.clientWidth - stripW);
  const ch = Math.max(1, el.wrap.clientHeight);

  // bitmap in device px, css box exactly the same size -- any mismatch gets resampled and blurred
  const bw = Math.max(1, Math.round(cw * dpr));
  const bh = Math.max(1, Math.round(ch * dpr));
  if (el.canvas.width !== bw || el.canvas.height !== bh) {
    el.canvas.width = bw;
    el.canvas.height = bh;
    el.canvas.style.width = (bw / dpr) + 'px';
    el.canvas.style.height = (bh / dpr) + 'px';
  }

  const ctx = el.canvas.getContext('2d');
  ctx.setTransform(1, 0, 0, 1, 0, 0);

  const cssW = bw / dpr, cssH = bh / dpr;

  if (autoFit && preview.w) {
    const pad = 30;    // free space around the mural (css px)
    let z = Math.min((cssW - pad) / preview.w, (cssH - pad) / preview.h);
    if (z * dpr >= 1) z = Math.floor(z * dpr) / dpr;    // whole device px per block, rounded down so it still fits
    view.zoom = Math.max(0.02, z);
    view.offX = (cssW - preview.w * view.zoom) * 0.5;
    view.offY = (cssH - preview.h * view.zoom) * 0.5;
    autoFit = false;
  }

  // zoomed in: a block is a whole number of device px, so tiles land on the pixel grid
  const zDev = view.zoom * dpr;
  const magnify = zDev >= 1;
  const scale = magnify ? Math.max(1, Math.round(zDev)) : zDev;
  const ox = Math.round(view.offX * dpr);
  const oy = Math.round(view.offY * dpr);

  rendered.scale = scale; rendered.ox = ox; rendered.oy = oy; rendered.dpr = dpr;

  ctx.imageSmoothingEnabled = !magnify;
  if (!magnify) ctx.imageSmoothingQuality = 'high';

  if (S.bgMode === 3) drawChecker(ctx, bw, bh, dpr);
  else { ctx.fillStyle = bgCss; ctx.fillRect(0, 0, bw, bh); }

  // viewed snapshot replaces the live mural, same position and zoom
  if (hoverSnap) {
    const src = (decodedFor === hoverSnap && decodedImg) ? decodedImg : hoverSnap.thumbImg;
    if (src && src.complete && src.naturalWidth) {
      ctx.imageSmoothingEnabled = scale < hoverSnap.px;
      if (ctx.imageSmoothingEnabled) ctx.imageSmoothingQuality = 'high';
      ctx.drawImage(src, ox, oy, hoverSnap.w * scale, hoverSnap.h * scale);
    }
    return;
  }

  if (!preview.w || !preview.grid) return;

  // real textures only if blocks are big enough to see them and few enough to draw one by one
  const visible = (bw / scale) * (bh / scale);
  if (preview.atlas && scale >= 8 && visible <= 40000) {
    drawFromAtlas(ctx, bw, bh, scale, ox, oy);
  } else {
    ctx.drawImage(preview.mean, ox, oy, preview.w * scale, preview.h * scale);
  }
}

function drawChecker(ctx, bw, bh, dpr) {
  const s = Math.max(8, Math.round(16 * dpr));
  for (let y = 0; y < bh; y += s) {
    for (let x = 0; x < bw; x += s) {
      ctx.fillStyle = ((x / s | 0) + (y / s | 0)) % 2 === 0 ? '#646464' : '#969696';
      ctx.fillRect(x, y, Math.min(s, bw - x), Math.min(s, bh - y));
    }
  }
}

function drawFromAtlas(ctx, bw, bh, scale, ox, oy) {
  const cols = preview.cols;
  const c0 = Math.max(0, Math.floor(-ox / scale));
  const c1 = Math.min(preview.w, Math.ceil((bw - ox) / scale));
  const r0 = Math.max(0, Math.floor(-oy / scale));
  const r1 = Math.min(preview.h, Math.ceil((bh - oy) / scale));

  for (let r = r0; r < r1; r++) {
    const dy = oy + r * scale;
    for (let c = c0; c < c1; c++) {
      const slot = preview.grid[r * preview.w + c];
      if (slot < 0) continue;
      ctx.drawImage(preview.atlas,
        (slot % cols) * 16, Math.floor(slot / cols) * 16, 16, 16,
        ox + c * scale, dy, scale, scale);
    }
  }
}

//////// mouse and touch
function wireCanvas() {
  const c = el.canvas;
  const pointers = new Map();
  let pinch = null;
  const PINCH_SLOP = 12;    // fingers must move this far apart or together before its a pinch (css px)

  const zoomTo = (px, py, target, nudge) => {
    const dpr = window.devicePixelRatio || 1;
    const old = view.zoom;
    let z = target;

    // zoom snaps to whole device px per block; nudge forces a step when rounding would eat a wheel click
    if (z * dpr >= 1) {
      let dev = Math.round(z * dpr);
      if (nudge) {
        const oldDev = Math.round(old * dpr);
        if (dev === oldDev) dev = oldDev + (target > old ? 1 : -1);
      }
      z = Math.max(1, dev) / dpr;
    }

    z = Math.min(200, Math.max(0.02, z));
    if (z === old) return;

    view.zoom = z;
    view.offX = px - (px - view.offX) * (z / old);    // keeps the point under the cursor in place
    view.offY = py - (py - view.offY) * (z / old);
    requestDraw();
  };

  c.addEventListener('wheel', e => {
    e.preventDefault();
    const r = c.getBoundingClientRect();
    zoomTo(e.clientX - r.left, e.clientY - r.top,
      view.zoom * (e.deltaY < 0 ? 1.15 : 1 / 1.15), true);
  }, { passive: false });

  c.addEventListener('pointerdown', e => {
    const r = c.getBoundingClientRect();
    const px = e.clientX - r.left, py = e.clientY - r.top;

    // ctrl+click disallows the block under the cursor
    if ((e.ctrlKey || e.metaKey) && e.pointerType === 'mouse') {
      const col = Math.floor((px * rendered.dpr - rendered.ox) / rendered.scale);
      const row = Math.floor((py * rendered.dpr - rendered.oy) / rendered.scale);
      const moved = api.pickAndDisallow(col, row);
      if (moved) {
        allowedSet.delete(moved);
        renderBlockLists();
        schedulePreview();
      }
      return;
    }

    c.setPointerCapture(e.pointerId);
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (pointers.size >= 2) pinch = null;    // another finger joined, restart the pinch from the new spread
    c.classList.add('is-panning');
  });

  c.addEventListener('pointermove', e => {
    if (!pointers.size && e.pointerType === 'mouse') {
      c.classList.toggle('is-picking', e.ctrlKey || e.metaKey);
    }

    const prev = pointers.get(e.pointerId);
    if (!prev) return;

    if (pointers.size >= 2) {
      pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
      const [a, b] = Array.from(pointers.values());
      const dist = Math.hypot(a.x - b.x, a.y - b.y);
      const r = c.getBoundingClientRect();

      const mx = (a.x + b.x) / 2 - r.left;
      const my = (a.y + b.y) / 2 - r.top;

      if (!pinch) {
        pinch = { dist, zoom: view.zoom, active: false, mx, my };
        return;
      }

      view.offX += mx - pinch.mx;
      view.offY += my - pinch.my;
      pinch.mx = mx;
      pinch.my = my;

      if (!pinch.active && Math.abs(dist - pinch.dist) >= PINCH_SLOP) {
        pinch.active = true;
        pinch.dist = dist;    // measure from here, otherwise crossing the threshold jumps the zoom
        pinch.zoom = view.zoom;
      }

      if (pinch.active && pinch.dist > 0) {
        zoomTo(mx, my, pinch.zoom * (dist / pinch.dist), false);
      }
      requestDraw();
      return;
    }

    view.offX += e.clientX - prev.x;
    view.offY += e.clientY - prev.y;
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    requestDraw();
  });

  const release = e => {
    pointers.delete(e.pointerId);
    if (pointers.size < 2) pinch = null;
    if (!pointers.size) c.classList.remove('is-panning');
  };
  c.addEventListener('pointerup', release);
  c.addEventListener('pointercancel', release);

  window.addEventListener('keydown', e => {
    if (e.key === 'Control' || e.key === 'Meta') c.classList.add('is-picking');
  });
  window.addEventListener('keyup', e => {
    if (e.key === 'Control' || e.key === 'Meta') c.classList.remove('is-picking');
  });

  // no image yet: a click anywhere on the preview opens the file picker
  el.wrap.addEventListener('click', e => {
    if (e.target.closest('#filmstrip')) return;
    if (!M || api.hasImage()) return;
    el.file.click();
  });

  new ResizeObserver(requestDraw).observe(el.wrap);
}


//////////////// IMAGES

// kept for out-of-memory recovery
let lastLoadedFile = null;

// engine keeps a big picture at the size the mural reads -- another size = it asks for the picture again
let sourceBlob = null, sourceName = '', resending = false;

async function resendSource() {
  if (resending) return;
  resending = true;
  try {
    if (!sourceBlob) throw new Error('no picture kept');
    await decodeIntoEngine(sourceBlob, sourceName, true);
  } catch (err) {
    console.error(err);
    M._mural_resend_image(0, 0, 0);
  }
  resending = false;
  api.requestPreview();
}

async function decodeIntoEngine(blob, name, again = false) {
  const bitmap = await createImageBitmap(blob);

  // largest mural reads 16 px per block, more than that is only memory
  const srcW = bitmap.width, srcH = bitmap.height;
  const scale = Math.min(1, MAX_SOURCE_SIDE / Math.max(srcW, srcH));
  const c = document.createElement('canvas');
  c.width = Math.max(1, Math.round(srcW * scale)); c.height = Math.max(1, Math.round(srcH * scale));
  const ctx = c.getContext('2d', { willReadFrequently: true });
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(bitmap, 0, 0, c.width, c.height);
  bitmap.close?.();
  if (scale < 1 && M && !again) api.log(`Picture is ${srcW}x${srcH}, read at ${c.width}x${c.height} (all the largest mural can use).`);

  const data = ctx.getImageData(0, 0, c.width, c.height).data;
  const ptr = M._malloc(data.length) >>> 0;
  M.HEAPU8.set(data, ptr);
  if (again) M._mural_resend_image(ptr, c.width, c.height);
  else M.ccall('mural_load_image', null, ['number', 'number', 'number', 'string'], [ptr, c.width, c.height, name]);
  M._free(ptr);
  if (again) return;

  sourceBlob = blob; sourceName = name;
  el.path.value = name;
  clearSnapshots();
  pendingAutoFit = true;
  return { w: srcW, h: srcH };
}

async function loadImageFile(file) {
  if (!file || !file.type.startsWith('image/')) return toast('That file format is not supported.');
  dbg(`[JS] Image selected: ${file.name}`);
  globalStartTime = performance.now();
  el.empty.hidden = true;

  try {
    const size = await decodeIntoEngine(file, file.name);
    lastLoadedFile = file;
    track('picture_loaded', { type: file.type.replace('image/', ''), width_px: size.w, height_px: size.h,
      megapixels: Math.round(size.w * size.h / 1e5) / 10 });

    // mural gets the file name, without extension
    const stem = file.name.replace(/\.[^.]*$/, '').trim().slice(0, 120);
    if (stem) {
      S.name = stem;
      el.name.value = stem;
      api.setName(stem);
    }

    api.requestPreview();
  } catch (err) {
    console.error(err);
    toast('Could not decode that image.');
  }
}

function wireDropTarget() {
  window.addEventListener('dragenter', e => e.preventDefault());
  window.addEventListener('dragover', e => e.preventDefault());
  window.addEventListener('drop', e => {
    e.preventDefault();
    const file = e.dataTransfer?.files?.[0];
    if (file) loadImageFile(file);
  });
}


//////////////// CONSOLE
let logCount = 0;
let lastStatus = '';

function pumpLog() {
  const n = api.logCount();
  if (n === logCount) return;

  // follow new lines only if the user hasnt scrolled up
  const atBottom = el.log.scrollTop + el.log.clientHeight >= el.log.scrollHeight - 8;

  const frag = document.createDocumentFragment();

  for (let i = logCount; i < n; i++) {
    const text = api.logLine(i);
    const line = document.createElement('div');
    line.className = 'logline';
    line.textContent = text;

    if (text.startsWith('/')) {    // minecraft commands, click to copy
      line.classList.add('cmd');
      line.title = 'Click to copy';
      line.addEventListener('click', () => copyCommand(text, line));
    }
    frag.appendChild(line);
  }

  el.log.appendChild(frag);
  logCount = n;
  if (atBottom) el.log.scrollTop = el.log.scrollHeight;

  lastLogLine = api.logLine(n - 1);
  if (consoleFolded()) el.consoleLast.textContent = lastLogLine;
}

//////// fold
const CONSOLE_FOLD_KEY = 'mmmm.consoleFolded.v1';
let lastLogLine = '';    // shown in the bar while folded

function consoleFolded() {
  return el.right.classList.contains('is-console-folded');
}

function setConsoleFolded(folded) {
  el.right.classList.toggle('is-console-folded', folded);
  el.consoleToggle.setAttribute('aria-expanded', String(!folded));
  el.consoleToggle.title = folded ? 'Show the console' : 'Minimise the console';
  el.consoleLast.textContent = folded ? lastLogLine : '';
  if (!folded) el.log.scrollTop = el.log.scrollHeight;
}

function wireConsoleFold() {
  let stored = null;

  // storage can be blocked, then it starts open
  try { stored = localStorage.getItem(CONSOLE_FOLD_KEY); } catch (_) { }
  setConsoleFolded(stored !== '0');

  el.consoleToggle.addEventListener('click', () => {
    const folded = !consoleFolded();
    setConsoleFolded(folded);
    try { localStorage.setItem(CONSOLE_FOLD_KEY, folded ? '1' : '0'); } catch (_) { }
  });
}

//////// command copy
async function copyCommand(text, node) {
  const hash = text.indexOf('   #');    // 3 spaces + # starts a remark, not part of the command
  const cmd = hash === -1 ? text : text.slice(0, hash);
  try {
    await navigator.clipboard.writeText(cmd);
  } catch {
    const ta = document.createElement('textarea');
    ta.value = cmd;
    document.body.appendChild(ta);
    ta.select();
    document.execCommand('copy');    // no clipboard access on some pages
    ta.remove();
  }
  node.classList.add('is-copied');
  setTimeout(() => node.classList.remove('is-copied'), 220);
  toast('Command copied to your clipboard!');
}

//////// status
function describeConfig(cfg, title) {
  const c = cfg || {};
  const t = { ...DEFAULTS.tweaks, ...(c.tweaks || {}) };
  const w = { ...DEFAULTS.weights, ...(c.weights || {}) };
  const di = { ...DEFAULTS.dither, ...(c.dither || {}) };
  const p = normalisePass2(c.pass2, DEFAULTS.pass2);
  const e = normaliseEnhance(c.enhance);
  const signed = (v, dp) => (v >= 0 ? '+' : '') + (+v).toFixed(dp);
  const vertical = (c.orientation || 0) === 0;
  const facing = (vertical ? FACING_VERT[c.dirVert || 0] : FACING_HORZ[c.dirHorz || 0]) || 'North';
  const sum = (w.wColor + w.wSpatial + w.wTexture) || 1;
  const pct = v => (v / sum * 100).toFixed(1) + '%';

  const exp = searchWanted(e)
    ? `Grain: ${(e.grainOn ? e.grain : 0.5).toFixed(2)} | Colour over texture: ${(e.flatOn ? e.flat : 0).toFixed(2)}`
      + ` | Protect texture: ${(e.protectOn ? e.protect : 0).toFixed(2)} | Feathering: ${e.feather ? 'ON' : 'OFF'}`
    : `Feathering: ${e.feather ? 'ON' : 'OFF'}`;

  // same four lines the engine writes for the live settings
  const lines = [
    `${title} | ${vertical ? 'vertical' : 'horizontal'} facing ${facing.toLowerCase()} | Biome: ${BIOMES[c.biome || 0] || 'Plains'} | Overlays: ${c.useOverlays ? 'ON' : 'OFF'}`,
    `Pre-Process -> Denoise: ${(+t.denoise).toFixed(1)} | Sharpen: ${(+t.sharpen).toFixed(1)} | Contrast: ${(+t.contrast).toFixed(2)} | Bright: ${signed(t.bright, 2)} | Sat: ${(+t.sat).toFixed(2)} | Hue: ${signed(t.hue, 0)}`,
    `Weights -> Color: ${pct(w.wColor)} | Spatial: ${pct(w.wSpatial)} | Texture: ${pct(w.wTexture)} | Dither: ${DITHER[di.method] || 'None'}, ${(+di.intensity).toFixed(2)}`
  ];

  if (p.enable) {
    const chunk = CHUNKS.find(x => x[0] === p.chunk);
    const whole = p.chunk === CHUNK_WHOLE;
    lines.push(`Pass 2: ON | Chunk: ${whole ? 'Whole image' : chunk[2] + 'x' + chunk[2]} | Passes: ${p.passes} | Candidates: ${p.candidates}`
      + (whole ? '' : ` | Target: Worst ${(p.threshold * 100).toFixed(0)}%`)
      + ` | MSE: ${(p.wMSE * 0.1).toFixed(2)} | SSIM: ${(p.wSSIM * 0.1).toFixed(2)} | ${exp}`);
  } else {
    lines.push(`Pass 2: OFF | ${exp}`);
  }
  return lines;
}

// summary of a hovered preset or snapshot, shown instead of live settings
let statusOverride = null;

function showConfigStatus(cfg, title) {
  statusOverride = describeConfig(cfg, title).join('\n');
  lastStatus = null;    // forces a repaint in syncStatus
  syncStatus();
}

function clearConfigStatus() {
  if (statusOverride === null) return;
  statusOverride = null;
  lastStatus = null;
  syncStatus();
}

function syncStatus() {
  const raw = api.statusJson();
  if (raw === lastStatus) return;
  lastStatus = raw;

  const st = JSON.parse(raw);
  el.status.hidden = !st.has && statusOverride === null;
  el.status.classList.toggle('is-borrowed', statusOverride !== null);
  if (statusOverride !== null) el.status.textContent = statusOverride;
  else if (st.has) el.status.textContent = st.lines.join('\n');

  el.warning.hidden = !st.warning;
  showCautions(st.warning || '');
}

// engine text: every caution starts with "CAUTION: " -- one line each, the word in brackets + in its own weight
function showCautions(text) {
  const lines = text.split('CAUTION: ').map((part, i) => {
    const line = document.createElement('span');
    line.className = 'warning-line';
    if (i > 0) {
      const tag = document.createElement('span');
      tag.className = 'warning-tag';
      tag.textContent = '[CAUTION]';
      line.append(tag, ' ');
    }
    line.append(part.trim());
    return line;
  });
  el.warning.replaceChildren(...lines.filter(line => line.textContent !== ''));
}

//////// toast and action log
function toast(msg) {
  el.toast.textContent = msg;
  el.toast.hidden = false;
  clearTimeout(toast._t);
  toast._t = setTimeout(() => { el.toast.hidden = true; }, 3200);
}

// page actions go to the log too, otherwise it only shows the engine
function logAction(msg) {
  toast(msg);
  if (M) api.log(msg);
}


//////////////// CRASH RECOVERY

// no progress for this long = out-of-memory hang (ms)
const STALL_MS = 15000;

// longer side of a picture as the engine gets it: 1000 blocks x 16 px
const MAX_SOURCE_SIDE = 16000;
const RECOVERY_KEY = 'mmmm.recovery.v1';
let stallProgress = -1, stallSince = 0, engineWedged = false;
let shownPct = -1;    // percent the badge shows now

//////// image store
const IDB_NAME = 'mmmm-recovery', IDB_STORE = 'image', IDB_KEY = 'current';

function idbRun(mode, fn) {
  return new Promise((resolve, reject) => {
    const open = indexedDB.open(IDB_NAME, 1);
    open.onupgradeneeded = () => open.result.createObjectStore(IDB_STORE);
    open.onerror = () => reject(open.error);
    open.onsuccess = () => {
      const db = open.result;
      let req;
      try {
        const tx = db.transaction(IDB_STORE, mode);
        req = fn(tx.objectStore(IDB_STORE));
        tx.oncomplete = () => { db.close(); resolve(req ? req.result : undefined); };
        tx.onerror = tx.onabort = () => { db.close(); reject(tx.error); };
      } catch (e) { db.close(); reject(e); }
    };
  });
}

async function stashImageForRecovery() {
  if (!lastLoadedFile) return false;
  try {
    const name = lastLoadedFile.name || el.path.value || 'image';
    await idbRun('readwrite', s => s.put({ blob: lastLoadedFile, name }, IDB_KEY));
    return true;
  } catch (_) { return false; }
}

async function takeStashedImage() {
  try {
    const rec = await idbRun('readonly', s => s.get(IDB_KEY));
    await idbRun('readwrite', s => s.delete(IDB_KEY));
    return rec || null;
  } catch (_) { return null; }
}

async function clearStashedImage() {
  try { await idbRun('readwrite', s => s.delete(IDB_KEY)); } catch (_) { }
}

//////// watchdog
function watchdog() {
  if (engineWedged) return;
  if (!api.isProcessing()) { stallProgress = -1; stallSince = 0; return; }

  // heartbeat moves in the phases the bar has no share for too (rail builder, exports)
  const p = api.heartbeat();
  const now = performance.now();
  if (p !== stallProgress) { stallProgress = p; stallSince = now; return; }
  if (stallSince && now - stallSince > STALL_MS) declareWedged();
}

function declareWedged() {
  engineWedged = true;
  clearTimeout(previewTimer);
  track('engine_failed', { reason: 'out_of_memory', width: S.width, height: S.height });

  let saved = false;
  try { localStorage.setItem(RECOVERY_KEY, JSON.stringify(currentConfig())); saved = true; } catch (_) { }

  el.busy.hidden = false;
  el.busy.textContent = 'Out of memory — click to reload';
  el.busy.style.background = 'rgba(120,20,20,0.92)';
  el.busy.style.borderColor = 'rgba(230,60,60,1)';
  el.busy.style.color = '#fff';
  el.busy.style.cursor = 'pointer';
  el.busy.style.pointerEvents = 'auto';

  // save it now already, the page may get reloaded some other way
  stashImageForRecovery();

  el.busy.onclick = async () => {
    el.busy.onclick = null;
    el.busy.textContent = 'Saving your image…';
    await stashImageForRecovery();
    location.reload();
  };

  logAction(saved
    ? 'The engine ran out of memory and stopped. Your settings were saved — reload and try a smaller mural.'
    : 'The engine ran out of memory and stopped. Reload the page and try a smaller mural.');
}

// after the reload: put back settings and image saved by declareWedged, once
async function restoreAfterCrash() {
  let raw = null;
  try { raw = localStorage.getItem(RECOVERY_KEY); localStorage.removeItem(RECOVERY_KEY); } catch (_) { }

  // normal start -- drop an image left over by a tab closed mid-save
  if (!raw) { clearStashedImage(); return; }

  try { applyConfig(JSON.parse(raw)); } catch (_) { }

  const rec = await takeStashedImage();
  if (rec && rec.blob) {
    try {
      const name = rec.name || 'restored image';
      await decodeIntoEngine(rec.blob, name);
      lastLoadedFile = (rec.blob instanceof File)
        ? rec.blob
        : new File([rec.blob], name, { type: rec.blob.type || 'image/png' });
      el.empty.hidden = true;
      api.requestPreview();
      logAction('Settings and image restored. Mural size is back to its default.');
      return;
    } catch (_) { }
  }
  logAction('Settings restored after the out-of-memory stop. Mural size is back to its default — re-select your image.');
}


//////////////// MAIN LOOP
let previewTimer = 0;

function schedulePreview() {
  if (engineWedged) return;
  clearTimeout(previewTimer);
  previewTimer = setTimeout(() => {
    if (globalStartTime === 0) globalStartTime = performance.now();
    dbg('[JS] Requesting calculation from C++ engine...');
    api.requestPreview();
  }, 250);    // debounce, dragging a slider should ask once (ms)
}

function tick() {
  const flags = api.poll();
  if (api.sourceWanted() && !resending) resendSource();

  // engine flags: 1 database ready, 2 new log lines, 4 new preview, 8 processing started or stopped
  if (flags & 1) onDatabaseReady();
  if (flags & 2) pumpLog();
  if (flags & 4) onPreviewReady();

  if ((flags & 8) && !engineWedged) {
    el.busy.hidden = !api.isProcessing();
    if (el.busy.hidden) { el.busy.style.background = ''; shownPct = -1; }
  }

  watchdog();

  // out-of-memory banner uses the same badge, dont paint over it
  if (!el.busy.hidden && !engineWedged) {
    let real = api.progress();
    const pct = Math.min(100, Math.max(0, Math.round(real * 100)));

    // badge is repainted when the number moves, not on every frame
    if (pct !== shownPct) {
      shownPct = pct;
      el.busy.textContent = `Processing image… ${pct}%`;
      el.busy.style.background = `linear-gradient(90deg, rgba(0,128,230,0.85) ${pct}%, rgba(40,40,40,0.92) ${pct}%)`;
      el.busy.style.borderColor = 'rgba(0,128,230,1)';
      el.busy.style.color = '#fff';
    }
  }

  if (!el.app.hidden) syncStatus();
}


//////////////// SPLITTER
function wireSplitter() {
  const sp = $('#splitter');
  let active = false;

  sp.addEventListener('pointerdown', e => {
    active = true;
    sp.setPointerCapture(e.pointerId);
    sp.classList.add('is-dragging');
  });

  sp.addEventListener('pointermove', e => {
    if (!active) return;

    // both panels keep a usable width (css px)
    const w = Math.min(window.innerWidth - 320, Math.max(360, e.clientX));
    document.documentElement.style.setProperty('--left-w', w + 'px');
    requestDraw();
  });

  const stop = () => { active = false; sp.classList.remove('is-dragging'); };
  sp.addEventListener('pointerup', stop);
  sp.addEventListener('pointercancel', stop);

  sp.addEventListener('keydown', e => {
    const cur = parseInt(getComputedStyle(document.documentElement).getPropertyValue('--left-w'), 10);
    if (e.key === 'ArrowLeft')  document.documentElement.style.setProperty('--left-w', Math.max(360, cur - 20) + 'px');
    if (e.key === 'ArrowRight') document.documentElement.style.setProperty('--left-w', Math.min(window.innerWidth - 320, cur + 20) + 'px');
    requestDraw();
  });
}


//////////////// CONFIG
function currentConfig() {
  return {
    lock: S.lock, name: S.name,
    version: versions[S.version] || '',
    orientation: S.orientation, dirVert: S.dirVert, dirHorz: S.dirHorz,
    biome: S.biome, bgMode: S.bgMode, useOverlays: S.useOverlays,
    tweaks: { ...S.tweaks },
    weights: { ...S.weights },
    dither: { ...S.dither },
    pass2: { ...S.pass2 },
    enhance: { ...S.enhance },
    allowedBlocks: availableBlocks().filter(b => allowedSet.has(b.id)).map(b => b.id),
    allowedOverlays: availableOverlays().filter(id => overlayAllowedSet.has(id))
  };
}

function applyConfig(cfg) {
  if (!cfg) return 0;

  const num = (v, fallback) => (Number.isFinite(+v) ? +v : fallback);

  S.lock = !!cfg.lock;
  S.name = cfg.name || 'my_mural';

  // old configs say 1.21, list says 1.21.9 -- no exact name? match on major.minor
  const minor = v => String(v || '').split('.').slice(0, 2).join('.');

  // hand-edited file can hold anything -- a value outside its list or slider = app default + a console line
  const reset = [];

  let verIdx = versions.indexOf(cfg.version);
  if (verIdx < 0 && cfg.version) verIdx = versions.findIndex(v => minor(v) === minor(cfg.version));
  if (verIdx >= 0) S.version = verIdx;
  else if (cfg.version !== undefined) reset.push('game version');

  const pick = (v, count, label) => {
    const n = num(v, NaN);
    if (Number.isInteger(n) && n >= 0 && n < count) return n;
    if (v !== undefined) reset.push(label);
    return 0;
  };
  S.orientation = pick(cfg.orientation, 2, 'orientation');
  S.dirVert = pick(cfg.dirVert, FACING_VERT.length, 'wall facing');
  S.dirHorz = pick(cfg.dirHorz, FACING_HORZ.length, 'floor or ceiling');
  S.biome = pick(cfg.biome, BIOMES.length, 'biome');
  S.bgMode = pick(cfg.bgMode, 4, 'background');
  S.useOverlays = !!cfg.useOverlays;

  // slider values: key in the file -> slider that holds its range; missing key keeps the value in use
  const ranged = (group, from, defaults, keys) => {
    const out = { ...group };
    for (const [k, sliderKey] of Object.entries(keys)) {
      if (!from || !(k in from)) continue;
      const sl = sliders.get(sliderKey), n = num(from[k], NaN);
      if (Number.isFinite(n) && (!sl || (n >= sl.min && n <= sl.max))) out[k] = n;
      else { out[k] = defaults[k]; reset.push(sl?.label || k); }
    }
    return out;
  };
  const same = keys => Object.fromEntries(keys.map(k => [k, k]));
  if (cfg.tweaks) S.tweaks = ranged(S.tweaks, cfg.tweaks, DEFAULTS.tweaks, same(Object.keys(DEFAULTS.tweaks)));
  if (cfg.weights) S.weights = ranged(S.weights, cfg.weights, DEFAULTS.weights, same(Object.keys(DEFAULTS.weights)));
  if (cfg.dither) {
    S.dither = ranged(S.dither, cfg.dither, DEFAULTS.dither, { intensity: 'ditherIntensity' });
    if ('method' in cfg.dither) S.dither.method = pick(cfg.dither.method, DITHER.length, 'dithering method');
  }
  const p2 = cfg.pass2 && ranged(cfg.pass2, cfg.pass2, DEFAULTS.pass2,
    { threshold: 'pass2Threshold', passes: 'pass2Passes', candidates: 'pass2Candidates', wMSE: 'wMSE', wSSIM: 'wSSIM' });
  S.pass2 = normalisePass2(p2, S.pass2);
  const enh = cfg.enhance && ranged(cfg.enhance, cfg.enhance, DEFAULTS.enhance, { grain: 'enhGrain', flat: 'enhFlat', protect: 'enhProtect' });
  S.enhance = normaliseEnhance(enh);
  if (reset.length && M) api.log(`[Config] ${reset.join(', ')}: not a value the app offers, default used.`);

  let skipped = 0;    // block ids this data doesnt know
  if (Array.isArray(cfg.allowedBlocks)) {
    const known = new Set(availableBlocks().map(b => b.id));
    allowedSet = new Set(cfg.allowedBlocks.filter(id => {
      if (known.has(id)) return true;
      skipped++;
      return false;
    }));
  }
  if (Array.isArray(cfg.allowedOverlays)) {
    const knownOv = new Set(availableOverlays());
    overlayAllowedSet = new Set(cfg.allowedOverlays.filter(id => knownOv.has(id)));
  }

  syncDomFromState();
  pushAllSettings();
  renderBlockLists();
  renderOverlayLists();
  refreshSwatches();

  pendingAutoFit = true;
  api.requestPreview();

  return skipped;
}

function syncDomFromState() {
  el.width.value = S.width;
  el.height.value = S.height;
  el.lock.checked = S.lock;
  el.name.value = S.name;
  el.version.value = String(S.version);
  el.orientation.value = String(S.orientation);
  refreshFacing();
  el.biome.value = String(S.biome);
  el.dither.value = String(S.dither.method);
  syncPass2Dom();
  syncEnhanceDom();
  el.overlays.checked = S.useOverlays;
  el.overlayFields.disabled = !S.useOverlays;
  syncSlidersFromState(true);
}

//////// saved settings
function normalisePass2(saved, fallback) {
  const d = DEFAULTS.pass2, num = (v, f) => (Number.isFinite(+v) ? +v : f);
  if (!saved) return { ...(fallback || d) };
  const p = { ...d, ...saved };

  // presets saved without these two ran 1 pass over the 3 best blocks, keep that
  if (!('passes' in saved)) p.passes = 1;
  if (!('candidates' in saved)) p.candidates = 3;

  p.passes = Math.min(3, Math.max(1, Math.round(num(p.passes, 1))));
  p.candidates = Math.min(16, Math.max(3, Math.round(num(p.candidates, 3))));
  p.chunk = nearestOfferedChunk(Math.round(num(p.chunk, d.chunk)));
  p.enable = !!p.enable;
  return p;
}

// a saved size thats not offered anymore becomes the nearest one that is
function nearestOfferedChunk(idx) {
  if (CHUNKS.some(c => c[0] === idx)) return idx;
  const blocks = ENGINE_CHUNK_BLOCKS[idx];
  if (!Number.isFinite(blocks)) return DEFAULTS.pass2.chunk;

  // nearest by ratio, larger one on a tie; whole image never gets picked this way
  let best = CHUNKS[0];
  for (const c of CHUNKS) {
    if (!Number.isFinite(c[2])) continue;
    const far = v => Math.abs(Math.log(v / blocks));
    if (far(c[2]) <= far(best[2])) best = c;
  }
  return best[0];
}

function normaliseEnhance(saved) {
  const d = structuredClone(DEFAULTS.enhance), num = (v, f) => (Number.isFinite(+v) ? +v : f);
  if (!saved) return { ...d, feather: false };    // preset from before the group existed, none of it ran
  if ('enable' in saved) {    // preset with one switch for the whole group
    const on = !!saved.enable;
    return {
      grainOn: on, grain: num(saved.grain, d.grain),
      flatOn: on, flat: num(saved.flat, d.flat),
      protectOn: on && num(saved.protect, 0) > 0, protect: num(saved.protect, 0) > 0 ? +saved.protect : d.protect,
      feather: on && saved.feather !== false
    };
  }
  const e = { ...d, ...saved };
  return {
    grainOn: !!e.grainOn, grain: num(e.grain, d.grain),
    flatOn: !!e.flatOn, flat: num(e.flat, d.flat),
    protectOn: !!e.protectOn, protect: num(e.protect, d.protect),
    feather: !!e.feather
  };
}


//////////////// PRESET STORAGE
const PRESET_KEY = 'mmmm.presets.v1';
const PRESET_LIMIT = 20;

function readPresets() {
  try {
    const list = JSON.parse(localStorage.getItem(PRESET_KEY) || '[]');
    return Array.isArray(list) ? list : [];
  } catch {
    return [];    // broken or blocked storage = no presets
  }
}

function writePresets(list) {
  try {
    localStorage.setItem(PRESET_KEY, JSON.stringify(list.slice(0, PRESET_LIMIT)));
    return true;
  } catch {
    toast('Could not save — browser storage is full or blocked.');
    return false;
  }
}

function describePreset(p) {
  const c = p.config || {};
  const bits = [DITHER[c.dither && c.dither.method] || 'no dither'];
  if (c.useOverlays) bits.push('overlays');
  bits.push(BIOMES[c.biome] || 'Plains');
  return bits.join(' · ') + ' · ' + relativeTime(p.saved);
}

function relativeTime(ts) {
  if (!ts) return 'unknown';
  const mins = Math.round((Date.now() - ts) / 60000);
  if (mins < 1) return 'just now';
  if (mins < 60) return mins + ' min ago';
  const hrs = Math.round(mins / 60);
  if (hrs < 24) return hrs + (hrs === 1 ? ' hour ago' : ' hours ago');
  const days = Math.round(hrs / 24);
  return days + (days === 1 ? ' day ago' : ' days ago');
}


//////////////// CONFIG MENUS
let openMenuEl = null;

const cfgEl = {
  menus: {},
  saveName: null, saveExisting: null, saveSlots: null,
  loadList: null, loadEmpty: null,
  exportName: null,
  importDrop: null, importFile: null,
  strip: null, stripItems: null, ring: null, badge: null
};
let replaceTarget = null;    // preset picked from the list to overwrite

function closeCfgMenu() {
  if (openMenuEl) { openMenuEl.hidden = true; openMenuEl = null; }
  clearConfigStatus();
}

function toggleCfgMenu(kind, anchor) {
  const menu = cfgEl.menus[kind];
  if (!menu) return;
  const wasOpen = openMenuEl === menu;
  closeCfgMenu();
  if (wasOpen) return;

  if (kind === 'save') refreshSaveMenu();
  if (kind === 'load') refreshLoadMenu();
  if (kind === 'export') cfgEl.exportName.value = (S.name || 'mural') + '.mmmm.json';

  menu.hidden = false;
  openMenuEl = menu;

  const r = anchor.getBoundingClientRect();
  const w = menu.offsetWidth || 264;
  menu.style.left = Math.max(8, Math.min(r.left, window.innerWidth - w - 8)) + 'px';
  menu.style.top = (r.bottom + 6) + 'px';

  const first = menu.querySelector('input:not([hidden]), button');
  if (first) first.focus();
}

function refreshSaveMenu() {
  cfgEl.saveName.value = S.name || '';

  const list = readPresets();
  cfgEl.saveExisting.hidden = list.length === 0;
  cfgEl.saveSlots.replaceChildren();

  list.forEach(p => {
    const row = document.createElement('button');
    row.className = 'cfg-row';
    row.innerHTML = '<span class="cfg-row-name"></span><span class="cfg-row-meta"></span>';
    row.querySelector('.cfg-row-name').textContent = p.name;
    row.querySelector('.cfg-row-meta').textContent = relativeTime(p.saved);
    row.addEventListener('click', () => {
      cfgEl.saveName.value = p.name;
      replaceTarget = p.name;    // picked slot = overwrite, the same name typed by hand is not
      cfgEl.saveName.focus();
    });
    cfgEl.saveSlots.appendChild(row);
  });
}

function refreshLoadMenu() {
  const list = readPresets();
  cfgEl.loadEmpty.hidden = list.length > 0;
  cfgEl.loadList.hidden = list.length === 0;
  cfgEl.loadList.replaceChildren();

  list.forEach(p => {
    const row = document.createElement('div');
    row.className = 'cfg-row cfg-row--tall';

    const main = document.createElement('button');
    main.className = 'cfg-row-main';
    main.innerHTML = '<span class="cfg-row-name"></span><span class="cfg-row-sub"></span>';
    main.querySelector('.cfg-row-name').textContent = p.name;
    main.querySelector('.cfg-row-sub').textContent = describePreset(p);
    const peek = () => showConfigStatus(p.config, `Preset "${p.name}"`);
    main.addEventListener('mouseenter', peek);
    main.addEventListener('focus', peek);
    main.addEventListener('mouseleave', clearConfigStatus);
    main.addEventListener('blur', clearConfigStatus);
    main.addEventListener('click', () => {
      const skipped = applyConfig(p.config);
      closeCfgMenu();
      logAction(skipped
        ? `Preset "${p.name}" loaded — ${skipped} unknown blocks skipped.`
        : `Preset "${p.name}" loaded.`);
    });

    const del = document.createElement('button');
    del.className = 'cfg-row-del';
    del.title = 'Delete preset';
    del.setAttribute('aria-label', `Delete ${p.name}`);
    del.textContent = '×';
    del.addEventListener('click', e => {
      e.stopPropagation();
      writePresets(readPresets().filter(x => x.name !== p.name));
      refreshLoadMenu();
      logAction(`Preset "${p.name}" deleted.`);
    });

    row.append(main, del);
    cfgEl.loadList.appendChild(row);
  });
}

function uniquePresetName(base, list) {
  if (!list.some(p => p.name === base)) return base;
  let n = 2;
  while (list.some(p => p.name === `${base} ${n}`)) n++;
  return `${base} ${n}`;
}

function savePreset() {
  const typed = cfgEl.saveName.value.trim();
  if (!typed) { cfgEl.saveName.focus(); return toast('Give the preset a name.'); }

  const list = readPresets();
  const replacing = replaceTarget === typed && list.some(p => p.name === typed);
  const name = replacing ? typed : uniquePresetName(typed, list);

  const entry = { v: 1, name, saved: Date.now(), config: currentConfig() };
  const at = list.findIndex(p => p.name === name);
  if (at >= 0) list[at] = entry; else list.unshift(entry);

  if (!writePresets(list)) return;
  replaceTarget = null;
  closeCfgMenu();

  if (replacing) {
    logAction(`Preset "${name}" replaced.`);
  } else if (name !== typed) {
    logAction(`Preset saved as "${name}" — "${typed}" was taken.`);
  } else {
    logAction(`Preset "${name}" saved to browser.`);
  }
}

function exportConfig() {
  const name = cfgEl.exportName.value.trim() || 'mural.mmmm.json';
  const payload = { v: 1, name: S.name, saved: Date.now(), config: currentConfig() };
  const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);

  const a = document.createElement('a');
  a.href = url;
  a.download = name.endsWith('.json') ? name : name + '.json';
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);

  closeCfgMenu();
  logAction(`Settings exported as ${a.download}`);
}

async function importConfigFile(file) {
  if (!file) return;
  try {
    const parsed = JSON.parse(await file.text());
    const cfg = parsed.config || parsed;    // exports wrap the config, a bare one works too
    if (!cfg || typeof cfg !== 'object') throw new Error('no config');
    const skipped = applyConfig(cfg);
    closeCfgMenu();
    logAction(skipped
      ? `Settings imported from ${file.name} — ${skipped} unknown blocks skipped.`
      : `Settings imported from ${file.name}.`);
  } catch {
    toast('That file is not a valid config export.');
  }
}

function wireConfigTools() {
  cfgEl.menus = {
    save: $('#menu-save'), load: $('#menu-load'),
    export: $('#menu-export'), import: $('#menu-import')
  };
  cfgEl.saveName = $('#save-name');
  cfgEl.saveExisting = $('#save-existing');
  cfgEl.saveSlots = $('#save-slots');
  cfgEl.loadList = $('#load-list');
  cfgEl.loadEmpty = $('#load-empty');
  cfgEl.exportName = $('#export-name');
  cfgEl.importDrop = $('#import-drop');
  cfgEl.importFile = $('#import-file');

  [['save', '#btn-cfg-save'], ['load', '#btn-cfg-load'],
   ['export', '#btn-cfg-export'], ['import', '#btn-cfg-import']].forEach(([kind, sel]) => {
    const btn = $(sel);
    if (btn) btn.addEventListener('click', e => { e.stopPropagation(); toggleCfgMenu(kind, btn); });
  });

  $('#btn-save-preset').addEventListener('click', savePreset);
  cfgEl.saveName.addEventListener('input', () => { replaceTarget = null; });    // edited name is a new name
  cfgEl.saveName.addEventListener('keydown', e => { if (e.key === 'Enter') savePreset(); });
  $('#btn-export-go').addEventListener('click', exportConfig);
  cfgEl.exportName.addEventListener('keydown', e => { if (e.key === 'Enter') exportConfig(); });

  $('#btn-import-pick').addEventListener('click', () => cfgEl.importFile.click());
  cfgEl.importFile.addEventListener('change', () => {
    importConfigFile(cfgEl.importFile.files[0]);
    cfgEl.importFile.value = '';
  });

  cfgEl.importDrop.addEventListener('dragover', e => {
    e.preventDefault();
    e.stopPropagation();
    cfgEl.importDrop.classList.add('is-over');
  });
  cfgEl.importDrop.addEventListener('dragleave', () => cfgEl.importDrop.classList.remove('is-over'));
  cfgEl.importDrop.addEventListener('drop', e => {
    e.preventDefault();
    e.stopPropagation();
    cfgEl.importDrop.classList.remove('is-over');
    importConfigFile(e.dataTransfer && e.dataTransfer.files[0]);
  });

  document.addEventListener('click', e => {
    if (openMenuEl && !openMenuEl.contains(e.target)) closeCfgMenu();
  });
  document.addEventListener('keydown', e => { if (e.key === 'Escape') closeCfgMenu(); });
}


//////////////// SNAPSHOTS
const SNAPSHOT_LIMIT = 12;
const SNAPSHOT_BLOCK_PX = 16;    // full detail (px per block)
const SNAPSHOT_PIXEL_BUDGET = 16e6;    // bigger murals get fewer px per block to stay under this (px)
const SNAPSHOT_QUALITY = 0.97;
const THUMB_WIDTH = 128;    // 2x the 64 px strip, for dense screens (px)
const LONG_PRESS_MS = 500;    // hold that restores a snapshot (ms)
const LONG_PRESS_SLOP = 10;    // finger drift that cancels the hold (css px)

const snapshots = [];
let snapSeq = 0;
let hoverSnap = null;
let snapPinned = false;    // opened by tap and kept open, not just hovered

// full-size image only for the snapshot being viewed, the rest stay thumbnails (memory)
let decodedFor = null;
let decodedImg = null;

function wireSnapshotUI() {
  cfgEl.strip = $('#filmstrip');
  cfgEl.stripItems = $('#strip-items');
  cfgEl.ring = $('#snap-ring');
  cfgEl.badge = $('#snap-badge');

  const btn = $('#btn-snapshot');
  if (btn) btn.addEventListener('click', takeSnapshot);
}

function snapshotBlockPx(w, h) {
  for (const px of [SNAPSHOT_BLOCK_PX, 8, 4, 2, 1]) {
    if (w * px * h * px <= SNAPSHOT_PIXEL_BUDGET) return px;
  }
  return 1;
}

function renderFullPreview(px) {
  const c = document.createElement('canvas');
  c.width = preview.w * px;
  c.height = preview.h * px;

  const ctx = c.getContext('2d');

  if (preview.atlas && preview.grid) {
    ctx.imageSmoothingEnabled = px < 16;    // tiles are 16 px, smooth only when shrinking
    if (px < 16) ctx.imageSmoothingQuality = 'high';

    const cols = preview.cols;
    for (let r = 0; r < preview.h; r++) {
      const dy = r * px;
      const row = r * preview.w;
      for (let col = 0; col < preview.w; col++) {
        const slot = preview.grid[row + col];
        if (slot < 0) continue;
        ctx.drawImage(preview.atlas,
          (slot % cols) * 16, Math.floor(slot / cols) * 16, 16, 16,
          col * px, dy, px, px);
      }
    }
  } else if (preview.mean) {
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(preview.mean, 0, 0, c.width, c.height);
  }

  return c;
}

function encodeWebp(canvas, quality) {
  return new Promise(resolve => {
    if (!canvas.toBlob) return resolve(null);
    canvas.toBlob(blob => resolve(blob), 'image/webp', quality);
  });
}

function makeThumb(src) {
  const w = Math.max(1, Math.min(THUMB_WIDTH, src.width));
  const h = Math.max(1, Math.round(w * (src.height / src.width)));
  const t = document.createElement('canvas');
  t.width = w;
  t.height = h;
  const tx = t.getContext('2d');
  tx.imageSmoothingEnabled = true;
  tx.imageSmoothingQuality = 'high';
  tx.drawImage(src, 0, 0, w, h);
  return t.toDataURL('image/webp', 0.9);
}

function disposeSnapshot(sn) {
  if (!sn) return;
  if (sn.url) URL.revokeObjectURL(sn.url);
  if (decodedFor === sn) { decodedFor = null; decodedImg = null; }
}

async function ensureDecoded(sn) {
  if (!sn || !sn.url) return;
  if (decodedFor === sn && decodedImg) return;

  const img = new Image();
  img.src = sn.url;
  try {
    await img.decode();
  } catch {
    return;
  }
  if (hoverSnap !== sn) return;    // pointer moved on while decoding

  decodedFor = sn;
  decodedImg = img;
  requestDraw();
}

function snapshotBytes() {
  return snapshots.reduce((n, sn) => n + (sn.bytes || 0), 0);
}

async function takeSnapshot() {
  if (!preview.w || (!preview.atlas && !preview.mean)) {
    return toast('Nothing to snapshot yet.');
  }

  const px = snapshotBlockPx(preview.w, preview.h);
  if (px < SNAPSHOT_BLOCK_PX) {
    toast(`Capturing at ${px}px per block — the mural is too large for full detail.`);
  }

  // let the toast show first, the render blocks the page
  await new Promise(r => requestAnimationFrame(r));

  const full = renderFullPreview(px);
  const thumb = makeThumb(full);
  const blob = await encodeWebp(full, SNAPSHOT_QUALITY);

  const sn = {
    n: ++snapSeq,
    url: blob ? URL.createObjectURL(blob) : full.toDataURL('image/png'),
    bytes: blob ? blob.size : 0,
    thumb,
    thumbImg: null,
    px,
    w: preview.w,
    h: preview.h,
    label: `${preview.w}×${preview.h}`,
    sub: DITHER[S.dither.method] || 'no dither',
    config: currentConfig()
  };

  const ti = new Image();
  ti.src = thumb;
  sn.thumbImg = ti;

  snapshots.push(sn);
  while (snapshots.length > SNAPSHOT_LIMIT) disposeSnapshot(snapshots.shift());

  // free the big canvas right away
  full.width = full.height = 0;

  renderFilmstrip();
  toast(`Snapshot ${snapSeq} saved · ${formatBytes(sn.bytes)}`);
}

function formatBytes(n) {
  if (!n) return 'unknown size';
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return Math.round(n / 1024) + ' KB';
  return (n / (1024 * 1024)).toFixed(1) + ' MB';
}

function clearSnapshots() {
  snapshots.forEach(disposeSnapshot);
  snapshots.length = 0;
  snapSeq = 0;
  hideSnap();
  renderFilmstrip();
}

//////// filmstrip
function renderFilmstrip() {
  if (!cfgEl.strip) return;

  cfgEl.strip.hidden = snapshots.length === 0;
  cfgEl.stripItems.replaceChildren();

  const head = cfgEl.strip.querySelector('.strip-head');
  if (head) head.textContent = snapshots.length ? `Shots · ${formatBytes(snapshotBytes())}` : 'Shots';

  if (!snapshots.length) { requestDraw(); return; }

  snapshots.slice().reverse().forEach(sn => {
    const item = document.createElement('div');
    item.className = 'strip-item';
    item.tabIndex = 0;
    item.title = `Snapshot ${sn.n} — click to restore these settings`;

    const im = document.createElement('img');
    im.src = sn.thumb;
    im.alt = `Snapshot ${sn.n}`;

    const cap = document.createElement('div');
    cap.className = 'strip-cap';
    cap.textContent = sn.label;

    const del = document.createElement('button');
    del.className = 'strip-del';
    del.textContent = '×';
    del.title = 'Discard snapshot';
    del.setAttribute('aria-label', `Discard snapshot ${sn.n}`);
    del.addEventListener('click', e => {
      e.stopPropagation();
      const at = snapshots.indexOf(sn);
      if (at >= 0) { disposeSnapshot(sn); snapshots.splice(at, 1); }
      if (hoverSnap === sn) hideSnap();
      renderFilmstrip();
    });

    item._snap = sn;

    const restore = () => {
      hideSnap();
      requestAnimationFrame(() => {
        applyConfig(sn.config);
        toast(`Restored snapshot ${sn.n}.`);
      });
    };

    // mouse + keyboard: hover or focus previews, click restores
    item.addEventListener('mouseenter', () => { if (!snapPinned) showSnap(sn, false); });
    item.addEventListener('mouseleave', () => { if (!snapPinned) hideSnap(); });
    item.addEventListener('focus', () => { if (!snapPinned) showSnap(sn, false); });
    item.addEventListener('blur', () => { if (!snapPinned) hideSnap(); });

    // touch: tap previews and keeps it open, hold restores
    let pressTimer = 0;
    let pressFired = false;
    let pressActive = false;
    let pressX = 0, pressY = 0;
    let pointerKind = 'mouse';

    const cancelPress = () => {
      clearTimeout(pressTimer);
      pressTimer = 0;
      item.classList.remove('is-holding');
    };

    item.addEventListener('pointerdown', e => {
      pointerKind = e.pointerType;
      if (e.pointerType !== 'touch') return;

      pressFired = false;
      pressActive = true;
      pressX = e.clientX;
      pressY = e.clientY;
      item.classList.add('is-holding');

      try { item.setPointerCapture(e.pointerId); } catch { }

      clearTimeout(pressTimer);
      pressTimer = setTimeout(() => {
        pressFired = true;
        pressActive = false;
        cancelPress();
        restore();
      }, LONG_PRESS_MS);
    });

    item.addEventListener('pointermove', e => {
      if (!pressTimer || e.pointerType !== 'touch') return;
      if (Math.hypot(e.clientX - pressX, e.clientY - pressY) > LONG_PRESS_SLOP) cancelPress();
    });

    item.addEventListener('pointerup', e => {
      if (e.pointerType !== 'touch') return;
      if (!pressActive) return;    // press started on another thumbnail

      const handled = pressFired;
      pressActive = false;
      cancelPress();
      if (handled) return;

      if (hoverSnap === sn && snapPinned) hideSnap();
      else showSnap(sn, true);
    });

    item.addEventListener('pointercancel', () => { pressActive = false; cancelPress(); });
    item.addEventListener('contextmenu', e => {
      if (pointerKind === 'touch') e.preventDefault();    // a hold would open the browser menu
    });

    item.addEventListener('click', e => {
      if (pointerKind === 'touch') { e.preventDefault(); return; }    // touch is handled in pointerup
      restore();
    });

    item.addEventListener('keydown', e => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); restore(); }
    });

    item.append(im, cap, del);
    cfgEl.stripItems.appendChild(item);
  });

  markPinned();
  requestDraw();
}

//////// snapshot on the canvas
function showSnap(sn, pinned) {
  hoverSnap = sn;
  snapPinned = !!pinned;
  showConfigStatus(sn.config, `Snapshot ${sn.n} | Matrix: ${sn.w}x${sn.h}`);
  paintSnapChrome(sn);
  markPinned();
  requestDraw();
  ensureDecoded(sn);
}

function hideSnap() {
  hoverSnap = null;
  snapPinned = false;
  clearConfigStatus();
  paintSnapChrome(null);
  markPinned();
  requestDraw();
}

function markPinned() {
  if (!cfgEl.stripItems) return;
  for (const item of cfgEl.stripItems.children) {
    item.classList.toggle('is-pinned', snapPinned && item._snap === hoverSnap);
  }
}

function paintSnapChrome(sn) {
  if (!cfgEl.ring || !cfgEl.badge) return;
  cfgEl.ring.hidden = !sn;
  cfgEl.badge.hidden = !sn;
  if (!sn) return;

  const how = snapPinned ? 'tap again to hide, hold to restore' : 'click to restore';
  cfgEl.badge.textContent = `Snapshot ${sn.n} · ${how}`;
}