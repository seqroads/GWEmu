/* renderer.js - the emulated screen, blitted with WebGL.
 *
 * The core composes ARGB8888, which in memory is B,G,R,A. Rather than swizzle
 * 77k pixels on the CPU every frame, the texture is uploaded as-is and the
 * fragment shader reads .bgr. Scaling is the sampler's job too, so filling
 * the window costs nothing.
 *
 * The same code runs on a canvas in the page and on an OffscreenCanvas in the
 * worker, because it only ever touches getContext.
 */

const VERT = `
attribute vec2 pos;
varying vec2 uv;
void main() {
    uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
    gl_Position = vec4(pos, 0.0, 1.0);
}`;

const FRAG = `
precision mediump float;
varying vec2 uv;
uniform sampler2D tex;
void main() { gl_FragColor = vec4(texture2D(tex, uv).bgr, 1.0); }`;

function compile(gl, type, src) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
        throw new Error('shader: ' + gl.getShaderInfoLog(s));
    return s;
}

export function createRenderer(canvas) {
    const gl = canvas.getContext('webgl', {
        alpha: false,
        depth: false,
        stencil: false,
        antialias: false,
        preserveDrawingBuffer: true,   /* so a screenshot can read it back */
        powerPreference: 'high-performance',
    });
    if (!gl) throw new Error('This browser has no WebGL.');

    const prog = gl.createProgram();
    gl.attachShader(prog, compile(gl, gl.VERTEX_SHADER, VERT));
    gl.attachShader(prog, compile(gl, gl.FRAGMENT_SHADER, FRAG));
    gl.bindAttribLocation(prog, 0, 'pos');
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS))
        throw new Error('link: ' + gl.getProgramInfoLog(prog));
    gl.useProgram(prog);
    gl.uniform1i(gl.getUniformLocation(prog, 'tex'), 0);

    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER,
        new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);

    let texW = 0, texH = 0;
    let lastW = 320, lastH = 240;

    /* Nearest, always: the panel is 320x240 and its pixels are the artwork. */
    function applyFilter() {
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    }
    applyFilter();

    /* Letterboxed and centred, filling whatever the window gives it. */
    function viewport() {
        const cw = canvas.width, ch = canvas.height;
        const s = Math.min(cw / lastW, ch / lastH);
        const w = Math.round(lastW * s), h = Math.round(lastH * s);
        gl.viewport(Math.floor((cw - w) / 2), Math.floor((ch - h) / 2), w, h);
    }

    return {
        get gl() { return gl; },

        /* pixels is a Uint8Array view of w*h ARGB8888 words. */
        draw(pixels, w, h) {
            lastW = w; lastH = h;
            gl.bindTexture(gl.TEXTURE_2D, tex);
            if (w !== texW || h !== texH) {
                texW = w; texH = h;
                gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0,
                              gl.RGBA, gl.UNSIGNED_BYTE, pixels);
                applyFilter();
            } else {
                gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h,
                                 gl.RGBA, gl.UNSIGNED_BYTE, pixels);
            }
            viewport();
            gl.drawArrays(gl.TRIANGLES, 0, 3);
        },

        clear() {
            gl.viewport(0, 0, canvas.width, canvas.height);
            gl.clearColor(0, 0, 0, 1);
            gl.clear(gl.COLOR_BUFFER_BIT);
        },

        resize(w, h) {
            if (canvas.width === w && canvas.height === h) return;
            canvas.width = w;
            canvas.height = h;
        },

    };
}
