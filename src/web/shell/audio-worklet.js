/* audio-worklet.js - playback ring for the emulated SAI output.
 *
 * The worker pushes 16-bit mono chunks whenever it has run; this pulls at the
 * device rate. The ring absorbs the difference, and the fill level is reported
 * back so the worker can steer its production and keep latency put.
 */

const RING = 32768;

class GWPlayer extends AudioWorkletProcessor {
    constructor() {
        super();
        this.ring = new Float32Array(RING);
        this.rd = 0;
        this.wr = 0;
        this.last = 0;
        this.report = 0;
        this.port.onmessage = (e) => {
            if (e.data.reset) { this.rd = this.wr = 0; return; }
            const pcm = new Int16Array(e.data.pcm);
            for (let i = 0; i < pcm.length; i++) {
                const next = (this.wr + 1) % RING;
                if (next === this.rd) break;      /* full: drop rather than lag */
                this.ring[this.wr] = pcm[i] / 32768;
                this.wr = next;
            }
        };
    }

    get queued() { return (this.wr - this.rd + RING) % RING; }

    process(inputs, outputs) {
        const out = outputs[0];
        const ch = out[0];
        const n = ch.length;

        for (let i = 0; i < n; i++) {
            if (this.rd !== this.wr) {
                this.last = this.ring[this.rd];
                this.rd = (this.rd + 1) % RING;
            }
            /* Underrun holds the last sample rather than dropping to silence,
               which clicks far more audibly than a held level. */
            ch[i] = this.last;
        }
        for (let c = 1; c < out.length; c++) out[c].set(ch);

        this.report += n;
        if (this.report >= 1024) {
            this.report = 0;
            this.port.postMessage({ queued: this.queued });
        }
        return true;
    }
}

registerProcessor('gw-player', GWPlayer);
