/* worker.js - the emulator, off the main thread.
 *
 * Everything expensive lives here: the interpreter, the frame compose and the
 * audio resample. The page keeps only input, storage and chrome, so a slow
 * frame stutters the game rather than freezing the interface.
 *
 * The screen goes straight to an OffscreenCanvas where the browser has one.
 * Where it does not, frames are posted over and the page draws them with the
 * same renderer; the buffers are handed back and reused, so nothing is
 * allocated per frame either way.
 */

import GWCore from './gwcore.js';
import { createRenderer } from './renderer.js';

let M = null;                 /* the wasm module */
let renderer = null;          /* set when we own an OffscreenCanvas */
let ready = false;

/* Firmware, kept so a reset or a fresh open does not need the page again. */
let intPtr = 0, intLen = 0, extPtr = 0, extLen = 0;

let running = false;          /* the pacing loop is armed */
let paused = true;            /* emulation is halted by the user or the page */
let opened = false;
let timer = null;

let coreHz = 88000000;
let lastFrame = -1;
let fbPtr = 0, fbW = 320, fbH = 240;

/* Wall-clock origin for the cycle budget, exactly as the desktop build does
   it: emulated time chases real time and gives up on deep arrears. */
let origin = 0, emuBase = 0;

/* Frame buffers recycled with the page on the no-OffscreenCanvas path. */
const spare = [];

/* Audio is produced to match elapsed real time, then trimmed by what the
   playback ring reports it still holds, so latency settles at a target
   instead of drifting one way for the whole session. */
let audioRate = 48000;
let audioPtr = 0, audioCap = 0;
let audioQueued = 0;          /* samples the page has that are not played yet */
let audioMuted = false;
const AUDIO_TARGET = 2400;    /* ~50 ms at 48 kHz */
const AUDIO_MAX    = 8192;

const log = (s) => post({ type: 'log', line: s });
function post(msg, transfer) { self.postMessage(msg, transfer || []); }

globalThis.gwOnLog = (s) => log(s);

/* ------------------------------------------------------------------ */
/* pacing                                                              */
/* ------------------------------------------------------------------ */

const TICK_MS = 8;            /* two ticks per display frame, for audio latency */

function stop() {
    running = false;
    if (timer !== null) { clearTimeout(timer); timer = null; }
}

function start() {
    if (running || !opened) return;
    running = true;
    origin = performance.now();
    emuBase = 0;
    tick();
}

function tick() {
    if (!running) return;
    const now = performance.now();

    if (!paused && !M._gw_halted()) {
        const want = ((now - origin) / 1000) * coreHz;

        /* More than a fifth of a second behind means something stalled - a
           tab in the background, a long GC. Write the debt off rather than
           sprinting, which would only stall the next frame too. */
        const debt = coreHz / 5;
        if (want - emuBase > debt) emuBase = want - debt;

        /* Cap a single pass at a display frame's worth so input and audio are
           serviced even when the interpreter cannot keep up. */
        const budget = Math.min(want - emuBase, coreHz / 50);
        if (budget > 0) {
            emuBase += M._gw_run(budget | 0);
        }

        pumpAudio(now);
        present();
    }


    timer = setTimeout(tick, TICK_MS);
}

/* ------------------------------------------------------------------ */
/* screen                                                              */
/* ------------------------------------------------------------------ */

function present() {
    const frame = M._gw_frame_counter();
    if (frame === lastFrame) return;
    lastFrame = frame;

    fbW = M._gw_fb_width();
    fbH = M._gw_fb_height();
    const bytes = fbW * fbH * 4;

    if (renderer) {
        renderer.draw(M.HEAPU8.subarray(fbPtr, fbPtr + bytes), fbW, fbH);
        return;
    }

    let buf = spare.pop();
    if (!buf || buf.byteLength !== bytes) buf = new ArrayBuffer(bytes);
    new Uint8Array(buf).set(M.HEAPU8.subarray(fbPtr, fbPtr + bytes));
    post({ type: 'frame', buf, w: fbW, h: fbH }, [buf]);
}

/* ------------------------------------------------------------------ */
/* audio                                                               */
/* ------------------------------------------------------------------ */

let audioClock = 0;

function pumpAudio(now) {
    if (!audioPtr || audioMuted) return;

    /* How many samples real time has consumed since the last pass, corrected
       towards the target queue depth. */
    const elapsed = (now - audioClock) / 1000;
    audioClock = now;
    let want = Math.round(elapsed * audioRate);
    if (want <= 0) return;

    const drift = audioQueued - AUDIO_TARGET;
    want -= Math.round(drift / 8);              /* gentle, over many passes */
    if (want <= 0) return;
    if (want > audioCap) want = audioCap;
    if (audioQueued + want > AUDIO_MAX) want = AUDIO_MAX - audioQueued;
    if (want <= 0) return;

    M._gw_audio_render(audioPtr, want, audioRate);

    const out = new Int16Array(want);
    out.set(M.HEAP16.subarray(audioPtr >> 1, (audioPtr >> 1) + want));
    audioQueued += want;
    post({ type: 'audio', pcm: out.buffer, frames: want }, [out.buffer]);
}

/* ------------------------------------------------------------------ */
/* firmware                                                            */
/* ------------------------------------------------------------------ */

function freeFirmware() {
    if (intPtr) M._gw_free(intPtr);
    if (extPtr) M._gw_free(extPtr);
    intPtr = extPtr = 0; intLen = extLen = 0;
}

function copyIn(bytes) {
    const p = M._gw_alloc(bytes.length);
    M.HEAPU8.set(bytes, p);
    return p;
}

function openFirmware(msg) {
    const int8 = new Uint8Array(msg.int);
    const ext8 = new Uint8Array(msg.ext);

    M._gw_close();
    freeFirmware();
    intPtr = copyIn(int8); intLen = int8.length;
    extPtr = copyIn(ext8); extLen = ext8.length;

    if (!M._gw_open(intPtr, intLen, extPtr, extLen, 0)) {
        freeFirmware();
        post({ type: 'error', message: 'The emulator could not start on that firmware.' });
        return;
    }

    opened = true;
    coreHz = M._gw_core_hz();
    fbPtr = M._gw_framebuffer();
    lastFrame = -1;

    if (msg.nvram && msg.nvram.byteLength) {
        const n = new Uint8Array(msg.nvram);
        const p = copyIn(n);
        M._gw_nvram_load(p, n.length);
        M._gw_free(p);
    }

    post({ type: 'opened', id: M.UTF8ToString(M._gw_firmware_id()), coreHz });
    paused = false;
    audioClock = performance.now();
    start();
}

function readNvram() {
    const size = M._gw_nvram_size();
    const p = M._gw_alloc(size);
    const n = M._gw_nvram_save(p, size);
    const out = new Uint8Array(n);
    out.set(M.HEAPU8.subarray(p, p + n));
    M._gw_free(p);
    return out;
}

/* ------------------------------------------------------------------ */
/* messages                                                            */
/* ------------------------------------------------------------------ */

self.onmessage = async (e) => {
    const msg = e.data;

    if (msg.type === 'init') {
        M = await GWCore();
        if (msg.canvas) {
            try {
                renderer = createRenderer(msg.canvas);
                renderer.clear();
            } catch (err) {
                renderer = null;
                log('[web] no WebGL in the worker, drawing on the page instead');
            }
        }
        audioRate = msg.audioRate || 48000;
        audioCap = 8192;
        audioPtr = M._gw_alloc(audioCap * 2);
        ready = true;
        post({ type: 'ready', offscreen: !!renderer });
        return;
    }
    if (!ready) return;

    switch (msg.type) {
    case 'open':
        openFirmware(msg);
        break;

    case 'close':
        stop();
        M._gw_close();
        freeFirmware();
        opened = false;
        if (renderer) renderer.clear();
        break;

    case 'buttons':
        if (opened) M._gw_set_buttons(msg.mask >>> 0);
        break;

    case 'power':
        if (opened) M._gw_power_tap();
        break;

    case 'reset':
        if (opened) { M._gw_reset(); lastFrame = -1; }
        break;

    case 'pause':
        paused = !!msg.paused;
        if (opened) M._gw_release_all();
        if (paused) {
            stop();
        } else {
            /* start() re-anchors the clock, so the resume does not try to make
               up everything that passed while it was stopped. */
            audioClock = performance.now();
            start();
        }
        break;

    case 'config':
        if (msg.muted !== undefined) audioMuted = msg.muted;
        break;

    case 'canvasSize':
        if (renderer) {
            renderer.resize(msg.w, msg.h);
            if (opened) lastFrame = -1; else renderer.clear();
        }
        break;

    case 'recycle':
        if (spare.length < 3) spare.push(msg.buf);
        break;

    case 'audioAck':
        audioQueued = msg.queued;
        break;

    case 'readNvram':
        if (opened) post({ type: 'nvramData', buf: readNvram().buffer });
        break;

    }
};
