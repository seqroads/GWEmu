/* input.js - keyboard and gamepad, reduced to one button mask.
 *
 * The mask is the core's EmuButton order, so the worker can hand it straight
 * to gw_set_buttons. Bindings match the desktop defaults: arrows for the
 * D-pad, X and Z for A and B, and 1..5 for the console buttons in the order
 * they are labelled.
 */

export const BUTTONS = [
    { id: 'left',   bit: 0,  label: 'Left'   },
    { id: 'right',  bit: 1,  label: 'Right'  },
    { id: 'up',     bit: 2,  label: 'Up'     },
    { id: 'down',   bit: 3,  label: 'Down'   },
    { id: 'a',      bit: 4,  label: 'A'      },
    { id: 'b',      bit: 5,  label: 'B'      },
    { id: 'time',   bit: 6,  label: 'TIME'   },
    { id: 'game',   bit: 7,  label: 'GAME'   },
    { id: 'pause',  bit: 8,  label: 'PAUSE / SET' },
    { id: 'select', bit: 9,  label: 'SELECT' },
    { id: 'start',  bit: 10, label: 'START'  },
];

/* One control per console button: a key, a pad button, or one direction of an
   axis - the same three shapes the desktop build's Binding has. Keys are
   KeyboardEvent.code so a binding follows the physical key rather than the
   layout: Z and X stay where they are on AZERTY and QWERTZ. Pad and axis codes
   index the Standard Gamepad mapping. */
export const DEFAULT_BINDS = {
    left:  { t: 'key', c: 'ArrowLeft'  },
    right: { t: 'key', c: 'ArrowRight' },
    up:    { t: 'key', c: 'ArrowUp'    },
    down:  { t: 'key', c: 'ArrowDown'  },
    a:     { t: 'key', c: 'KeyX'  },
    b:     { t: 'key', c: 'KeyZ'  },
    time:  { t: 'key', c: 'Digit2' },
    game:  { t: 'key', c: 'Digit1' },
    pause: { t: 'key', c: 'Digit3' },
    select:{ t: 'key', c: 'Digit4' },
    start: { t: 'key', c: 'Digit5' },
};

const AXIS_DEADZONE = 0.45;      /* past this an axis counts as held */
const AXIS_BIND     = 0.7;       /* and this much to claim a slot */

export function createInput(getBindings) {
    const held = new Set();          /* physical keys currently down */
    let padIndex = null;

    /* A rebind parks here; the next key or pad press resolves it. */
    let capture = null;

    addEventListener('keydown', (e) => {
        held.add(e.code);
        if (!capture) return;
        e.preventDefault();
        if (e.repeat) return;

        const done = capture;
        capture = null;
        done.resolve(e.code === 'Escape' ? null : { t: 'key', c: e.code });
    });
    addEventListener('keyup', (e) => held.delete(e.code));
    /* Alt-tabbing away with a key down would otherwise leave it stuck. */
    addEventListener('blur', () => held.clear());

    addEventListener('gamepadconnected', (e) => {
        if (padIndex === null) padIndex = e.gamepad.index;
    });
    addEventListener('gamepaddisconnected', (e) => {
        if (padIndex === e.gamepad.index) padIndex = null;
    });

    function pad() {
        if (!navigator.getGamepads) return null;
        const pads = navigator.getGamepads();
        if (padIndex !== null && pads[padIndex]) return pads[padIndex];
        /* Browsers only hand a pad over once a button on it has been pressed,
           and may never fire the connect event before that. */
        for (const p of pads) {
            if (p && p.connected) { padIndex = p.index; return p; }
        }
        return null;
    }

    /* A trigger reports as an analogue button on the standard mapping, and
       some browsers only raise .pressed at their own threshold. */
    function down(btn) {
        return !!btn && (btn.pressed || btn.value > 0.5);
    }

    /* Each direction of an axis is its own control, which is what lets a stick
       stand in for a D-pad. A trigger reporting as an axis binds the way it
       travels when pulled, and reads as held only while it is held there. */
    function axisHeld(p, bind) {
        const v = p.axes[bind.c];
        if (v === undefined) return false;
        return bind.d > 0 ? v > AXIS_DEADZONE : v < -AXIS_DEADZONE;
    }

    return {
        /* True while a rebind is listening, so hotkeys stand down. */
        get capturing() { return capture !== null; },

        mask() {
            const binds = getBindings();
            const p = pad();
            let m = 0;

            for (const b of BUTTONS) {
                const bind = binds[b.id];
                if (!bind) continue;
                let on = false;
                if (bind.t === 'key') on = held.has(bind.c);
                else if (!p) on = false;
                else if (bind.t === 'pad') on = down(p.buttons[bind.c]);
                else if (bind.t === 'axis') on = axisHeld(p, bind);
                if (on) m |= 1 << b.bit;
            }
            return m;
        },

        /* Resolves on whichever comes first - a key, a pad button, or an axis
           pushed past its threshold - so one slot serves any of them. */
        capture() {
            return new Promise((resolve) => {
                const self = { resolve };
                capture = self;

                /* The poll below is scheduled a frame ahead, so it can outlive
                   the capture it belongs to. Both paths check they are still
                   the current one, or a stale loop answers the next capture. */
                const finish = (bind) => {
                    if (capture !== self) return;
                    capture = null;
                    self.resolve(bind);
                };

                /* Anything already down or off centre has to be let go before
                   it can claim the slot - otherwise it binds itself instantly,
                   and a trigger sitting at its -1 rest counts as off centre.
                   The reading is taken from the first frame the pad is really
                   visible rather than from when the rebind opened: browsers
                   hand a pad over only once something on it has been pressed,
                   so at that point there may have been nothing to read. */
                const skipBtn = new Set();
                const skipAxis = new Map();   /* axis -> the way it was already pushed */
                let seeded = false;

                const t0 = performance.now();
                const poll = () => {
                    if (capture !== self) return;      /* a key got there first */
                    const p = pad();
                    if (p) {
                        if (!seeded) {
                            seeded = true;
                            p.buttons.forEach((b, i) => { if (down(b)) skipBtn.add(i); });
                            p.axes.forEach((v, i) => {
                                if (Math.abs(v) > AXIS_BIND) skipAxis.set(i, v > 0 ? 1 : -1);
                            });
                        }
                        for (let i = 0; i < p.buttons.length; i++) {
                            if (!down(p.buttons[i])) { skipBtn.delete(i); continue; }
                            if (!skipBtn.has(i)) return finish({ t: 'pad', c: i });
                        }
                        /* Bound harder than it is read, so a drifting stick
                           cannot bind itself but a light push still works.
                           Only the way an axis was already pushed is skipped:
                           a trigger sitting at -1 is waved through the moment
                           it is pulled the other way, without having to pass
                           through the middle in a frame we happened to see. */
                        for (let i = 0; i < p.axes.length; i++) {
                            const v = p.axes[i];
                            if (Math.abs(v) <= AXIS_BIND) { skipAxis.delete(i); continue; }
                            const d = v > 0 ? 1 : -1;
                            if (skipAxis.get(i) !== d) return finish({ t: 'axis', c: i, d });
                        }
                    }
                    if (performance.now() - t0 > 10000) return finish(null);
                    requestAnimationFrame(poll);
                };
                requestAnimationFrame(poll);
            });
        },

        cancelCapture() {
            if (capture) { capture.resolve(null); capture = null; }
        },
    };
}

const PAD_NAMES = {
    0: 'A / Cross', 1: 'B / Circle', 2: 'X / Square', 3: 'Y / Triangle',
    4: 'L1', 5: 'R1', 6: 'L2', 7: 'R2', 8: 'Select / Back', 9: 'Start',
    10: 'L3', 11: 'R3', 12: 'D-Pad Up', 13: 'D-Pad Down',
    14: 'D-Pad Left', 15: 'D-Pad Right', 16: 'Guide',
};

/* Axis 0/1 are the left stick and 2/3 the right on the standard mapping. A pad
   that goes its own way gets the axis number, which is at least honest. */
const AXIS_NAMES = {
    '0-': 'Left Stick Left',  '0+': 'Left Stick Right',
    '1-': 'Left Stick Up',    '1+': 'Left Stick Down',
    '2-': 'Right Stick Left', '2+': 'Right Stick Right',
    '3-': 'Right Stick Up',   '3+': 'Right Stick Down',
};

/* What to print on the slot, whichever kind of control is in it. */
export function bindName(bind) {
    if (!bind) return '—';
    if (bind.t === 'pad') return PAD_NAMES[bind.c] || ('Button ' + bind.c);
    if (bind.t === 'axis') {
        const key = bind.c + (bind.d > 0 ? '+' : '-');
        return AXIS_NAMES[key] || ('Axis ' + bind.c + (bind.d > 0 ? ' +' : ' −'));
    }
    return keyName(bind.c);
}

function keyName(code) {
    if (!code) return '—';
    if (code.startsWith('Key')) return code.slice(3);
    if (code.startsWith('Digit')) return code.slice(5);
    if (code.startsWith('Arrow')) return code.slice(5) + ' Arrow';
    if (code.startsWith('Numpad')) return 'Numpad ' + code.slice(6);
    return code;
}
