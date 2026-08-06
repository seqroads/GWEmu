/* main.js - the page: files, input, audio, storage and chrome.
 *
 * No emulation happens here. This thread reads the gamepad, forwards a button
 * mask, hands audio to the device and puts bytes in IndexedDB; the worker does
 * the rest. Where the browser allows it the worker also owns the canvas, and
 * this file never touches a pixel.
 */

import { storage, prefs } from './storage.js';
import { createInput, BUTTONS, DEFAULT_BINDS, bindName } from './input.js';

const $ = (id) => document.getElementById(id);

const el = {
    stage: $('stage'), screen: $('screen'),
    welcome: $('welcome'), drop: $('drop'), toast: $('toast'),
    panel: $('input-panel'), file: $('file'),
};

/* Only the bindings are worth keeping; everything else is either the device's
   own business or something a browser should not be deciding for the user. */
const DEFAULTS = { binds: { ...DEFAULT_BINDS } };

let settings = prefs.load(DEFAULTS);
settings.binds = { ...DEFAULT_BINDS, ...(settings.binds || {}) };

let worker = null;
let workerReady = false;
let offscreen = false;
let localRenderer = null;      /* only when the worker could not take the canvas */

let firmwareId = null;
let loaded = false;
let paused = false;
let lastMask = -1;
let autosaveTimer = null;

const input = createInput(() => settings.binds);

/* ------------------------------------------------------------------ */
/* chrome                                                              */
/* ------------------------------------------------------------------ */

/* Errors only. Anything that went right is already visible on the screen the
   emulator is drawing, and did not need saying twice. */
let toastTimer = null;
function toast(msg) {
    el.toast.textContent = msg;
    el.toast.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.toast.classList.remove('show'), 4500);
    console.warn(msg);
}

/* The core's log is a diagnostic, not something the page should be showing.
   The browser console is where anyone looking for it would look. */
function logLine(s) {
    console.log(s.replace(/\n$/, ''));
}

function setLoaded(on) {
    loaded = on;
    for (const id of ['btn-power', 'btn-reset', 'btn-full']) $(id).disabled = !on;
    el.welcome.classList.toggle('hidden', on);
}

/* ------------------------------------------------------------------ */
/* audio                                                               */
/* ------------------------------------------------------------------ */

let audioCtx = null, audioNode = null, audioRate = 48000;

async function initAudio() {
    if (audioCtx) return;
    try {
        audioCtx = new (window.AudioContext || window.webkitAudioContext)({
            latencyHint: 'interactive',
        });
        audioRate = audioCtx.sampleRate;
        await audioCtx.audioWorklet.addModule('audio-worklet.js');
        audioNode = new AudioWorkletNode(audioCtx, 'gw-player', { outputChannelCount: [1] });
        audioNode.connect(audioCtx.destination);
        audioNode.port.onmessage = (e) => {
            if (worker) worker.postMessage({ type: 'audioAck', queued: e.data.queued });
        };
    } catch (err) {
        logLine('[web] audio unavailable: ' + err + '\n');
        audioCtx = null;
    }
}

/* Browsers will not start a context until the user has done something. */
function unlockAudio() {
    if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
}
addEventListener('pointerdown', unlockAudio);
addEventListener('keydown', unlockAudio);

/* ------------------------------------------------------------------ */
/* worker                                                              */
/* ------------------------------------------------------------------ */

async function startWorker() {
    await initAudio();

    worker = new Worker('worker.js', { type: 'module' });
    worker.onmessage = onWorkerMessage;
    worker.onerror = (e) => {
        logLine('[web] worker error: ' + e.message + '\n');
        toast('The emulator failed to start. See the browser console.');
    };

    const init = { type: 'init', audioRate };
    const transfer = [];

    /* Handing the canvas over means the worker draws without ever waking this
       thread. Older browsers keep the canvas here and take posted frames. */
    if (typeof el.screen.transferControlToOffscreen === 'function') {
        try {
            init.canvas = el.screen.transferControlToOffscreen();
            transfer.push(init.canvas);
        } catch { /* unsupported after all */ }
    }
    worker.postMessage(init, transfer);
}

function onWorkerMessage(e) {
    const m = e.data;
    switch (m.type) {
    case 'ready':
        workerReady = true;
        offscreen = m.offscreen;
        if (!offscreen) setupLocalRenderer();
        resizeCanvas();
        purgeFirmware();
        break;

    case 'log':
        logLine(m.line);
        break;

    case 'opened':
        firmwareId = m.id;
        setLoaded(true);
        paused = false;
        startAutosave();
        break;

    case 'error':
        toast(m.message);
        break;

    case 'frame':
        if (localRenderer) {
            localRenderer.draw(new Uint8Array(m.buf), m.w, m.h);
            worker.postMessage({ type: 'recycle', buf: m.buf }, [m.buf]);
        }
        break;

    case 'audio':
        if (audioNode) audioNode.port.postMessage({ pcm: m.pcm }, [m.pcm]);
        break;

    case 'nvramData':
        if (m.buf.byteLength && firmwareId)
            storage.put('nvram', firmwareId, m.buf).catch(() => {});
        break;
    }
}

/* ------------------------------------------------------------------ */
/* canvas                                                              */
/* ------------------------------------------------------------------ */

async function setupLocalRenderer() {
    const { createRenderer } = await import('./renderer.js');
    try {
        localRenderer = createRenderer(el.screen);
        localRenderer.clear();
        resizeCanvas();
    } catch (err) {
        toast('This browser cannot draw the screen: ' + err.message);
    }
}

function resizeCanvas() {
    const r = el.stage.getBoundingClientRect();
    const dpr = Math.min(devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.round(r.width * dpr));
    const h = Math.max(1, Math.round(r.height * dpr));
    if (offscreen) worker.postMessage({ type: 'canvasSize', w, h });
    else if (localRenderer) localRenderer.resize(w, h);
}

addEventListener('resize', resizeCanvas);

/* ------------------------------------------------------------------ */
/* firmware                                                            */
/* ------------------------------------------------------------------ */

/* 128 KiB is the internal flash; 1, 2 or 4 MiB is the external one. That is
   the whole of the identification - dumps are named inconsistently. */
function classify(n) {
    if (n === 128 * 1024) return 'int';
    for (let s = 1024 * 1024; s <= 4096 * 1024; s *= 2) if (n === s) return 'ext';
    return null;
}

async function loadFiles(files) {
    let int = null, ext = null, intName = '';

    for (const f of files) {
        const buf = await f.arrayBuffer();
        const kind = classify(buf.byteLength);
        if (kind === 'int' && !int) { int = buf; intName = f.name; }
        else if (kind === 'ext' && !ext) ext = buf;
    }

    if (!int && !ext) { toast('Neither file is the right size for a Game & Watch dump.'); return; }
    if (!int) { toast('That is the external flash. The 128 KiB internal image is missing.'); return; }
    if (!ext) { toast('That is the internal flash. The 1 or 4 MiB external image is missing.'); return; }

    await openFirmware(int, ext, intName);
}

async function openFirmware(int, ext, name) {
    if (!workerReady) return;

    const id = fnv1a(new Uint8Array(int));
    let nvram = null;
    try { nvram = await storage.get('nvram', id); } catch { /* first run */ }

    /* Copies, transferred rather than cloned: the originals stay here for
       storage, since a transferred buffer is detached on this side. */
    const intCopy = int.slice(0), extCopy = ext.slice(0);
    const transfer = [intCopy, extCopy];
    if (nvram) transfer.push(nvram);

    worker.postMessage({
        type: 'open',
        int: intCopy, ext: extCopy,
        nvram: nvram || null,
    }, transfer);

    document.title = (name ? name.replace(/\.[^.]+$/, '') + ' — ' : '') + 'GWEmu';
}

/* The same FNV-1a the core and the desktop build key their save files with. */
function fnv1a(bytes) {
    let h = 0xcbf29ce484222325n;
    const P = 0x100000001b3n, M = 0xffffffffffffffffn;
    for (let i = 0; i < bytes.length; i++) {
        h = (h ^ BigInt(bytes[i])) & M;
        h = (h * P) & M;
    }
    return h.toString(16).padStart(16, '0');
}

/* The firmware images are never written to storage. An earlier build did keep
   them, and there is no longer any screen offering to clear that, so anything
   a previous version left behind goes on the way past - along with the save
   states it also used to hold. Save data stays: that is the device's own
   memory, and dropping it would be dropping the player's progress. */
async function purgeFirmware() {
    for (const store of ['firmware', 'states']) {
        try {
            for (const k of await storage.keys(store)) await storage.del(store, k);
        } catch { /* the store was never created, which is the normal case */ }
    }
}

/* ------------------------------------------------------------------ */
/* save data                                                           */
/* ------------------------------------------------------------------ */

function startAutosave() {
    clearInterval(autosaveTimer);
    /* The firmware commits its save area and its clock to flash as it goes and
       again on power-off, so copying the whole thing out on a timer is enough
       to survive a closed tab. */
    autosaveTimer = setInterval(() => {
        if (loaded && !paused && !document.hidden)
            worker.postMessage({ type: 'readNvram' });
    }, 5000);
}

/* ------------------------------------------------------------------ */
/* input pump                                                          */
/* ------------------------------------------------------------------ */

function pump() {
    requestAnimationFrame(pump);
    if (!loaded || paused || input.capturing) return;
    const m = input.mask();
    if (m !== lastMask) {
        lastMask = m;
        worker.postMessage({ type: 'buttons', mask: m });
    }
}
requestAnimationFrame(pump);

/* The game keys must not also scroll the page or activate a focused button. */
const SWALLOW = new Set(['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Space']);

addEventListener('keydown', (e) => {
    if (input.capturing) return;
    const tag = document.activeElement && document.activeElement.tagName;
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;

    if (e.ctrlKey || e.metaKey) {
        if (e.code === 'KeyO') { $('btn-open').click(); e.preventDefault(); }
        else if (e.code === 'KeyR') { doReset(); e.preventDefault(); }
        return;
    }

    if (e.code === 'F11') { toggleFullscreen(); e.preventDefault(); return; }
    if (e.code === 'Escape' && !el.panel.classList.contains('hidden')) { closePanel(); return; }
    if (loaded && SWALLOW.has(e.code)) e.preventDefault();
});

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

function setPaused(on) {
    if (!loaded || paused === on) return;
    paused = on;
    lastMask = -1;
    worker.postMessage({ type: 'pause', paused });
    if (audioNode) audioNode.port.postMessage({ reset: true });
}

function doReset() {
    if (loaded) worker.postMessage({ type: 'reset' });
}

function toggleFullscreen() {
    if (document.fullscreenElement) document.exitFullscreen();
    else if (el.stage.requestFullscreen) el.stage.requestFullscreen();
}
document.addEventListener('fullscreenchange', () => setTimeout(resizeCanvas, 60));

$('btn-open').onclick = $('btn-open2').onclick = () => el.file.click();
el.file.onchange = () => { if (el.file.files.length) loadFiles([...el.file.files]); el.file.value = ''; };

$('btn-power').onclick = () => { if (loaded) worker.postMessage({ type: 'power' }); };
$('btn-reset').onclick = doReset;
$('btn-full').onclick = toggleFullscreen;

/* ------------------------------------------------------------------ */
/* drag and drop                                                       */
/* ------------------------------------------------------------------ */

let dragDepth = 0;
addEventListener('dragenter', (e) => {
    e.preventDefault();
    if (++dragDepth === 1) el.drop.classList.remove('hidden');
});
addEventListener('dragover', (e) => e.preventDefault());
addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; el.drop.classList.add('hidden'); } });
addEventListener('drop', (e) => {
    e.preventDefault();
    dragDepth = 0;
    el.drop.classList.add('hidden');
    if (e.dataTransfer.files.length) loadFiles([...e.dataTransfer.files]);
});

/* ------------------------------------------------------------------ */
/* settings panel                                                      */
/* ------------------------------------------------------------------ */

function save() { prefs.save(settings); }

function closePanel() { el.panel.classList.add('hidden'); input.cancelCapture(); }
$('btn-input').onclick = () => el.panel.classList.toggle('hidden');
$('btn-close-input').onclick = closePanel;

/* ---- rebinding ---- */

/* The two directions of one axis are different controls, so the direction has
   to be part of the comparison: without it, binding Left to the stick pushed
   left and Right to the same stick pushed right would read as the same
   control and the first would be cleared. */
function sameBind(a, b) {
    if (!a || !b || a.t !== b.t || a.c !== b.c) return false;
    return a.t !== 'axis' || a.d === b.d;
}

/* Click a slot, press the control you want on it. That is the whole of it. */
function bindButton(b) {
    const btn = document.createElement('button');
    const paint = () => { btn.textContent = bindName(settings.binds[b.id]); };
    paint();

    btn.onclick = async () => {
        btn.classList.add('listening');
        btn.textContent = 'Press a key or button…';
        const bind = await input.capture();
        btn.classList.remove('listening');
        if (!bind) { paint(); return; }

        /* One control per button; take it off whoever had it. */
        for (const k of Object.keys(settings.binds))
            if (sameBind(settings.binds[k], bind)) delete settings.binds[k];
        settings.binds[b.id] = bind;
        save();
        buildBinds();
    };
    return btn;
}

function buildBinds() {
    const box = $('binds');
    box.innerHTML = '';
    for (const b of BUTTONS) {
        const label = document.createElement('label');
        label.textContent = b.label;
        box.append(label, bindButton(b));
    }
}
buildBinds();

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* A hidden tab gets its timers throttled to a crawl, which would starve the
   audio and then flood it on return. Stop cleanly instead. */
document.addEventListener('visibilitychange', () => {
    if (!loaded) return;
    if (document.hidden) {
        worker.postMessage({ type: 'readNvram' });
        setPaused(true);
    } else {
        setPaused(false);
    }
});

addEventListener('pagehide', () => {
    if (loaded) worker.postMessage({ type: 'readNvram' });
});

if (!window.WebAssembly) {
    toast('This browser has no WebAssembly.');
} else {
    setLoaded(false);
    startWorker();
}
