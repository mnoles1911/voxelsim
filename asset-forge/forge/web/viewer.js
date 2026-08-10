/* 3D voxel viewer — orbit, zoom, inspect.
 *
 * Hand-written WebGL2 rather than three.js: the whole thing is one instanced
 * draw call of unit cubes, which is a few hundred lines, and vendoring a
 * megabyte of library into a local tool that must work offline is a worse
 * trade than writing the matrix math.
 *
 * The server sends only surface voxels — a solid trunk's interior never
 * reaches the browser — so a 120k-voxel oak arrives as roughly 90k instances,
 * which WebGL2 instancing draws without effort.
 */

const Viewer = (() => {
  const VERT = `#version 300 es
  in vec3 aPos;
  in vec3 aNormal;
  in vec3 aOffset;
  in vec3 aColor;
  uniform mat4 uMVP;
  out vec3 vColor;
  out vec3 vNormal;
  void main() {
    gl_Position = uMVP * vec4(aPos + aOffset, 1.0);
    vColor = aColor;
    vNormal = aNormal;
  }`;

  const FRAG = `#version 300 es
  precision mediump float;
  in vec3 vColor;
  in vec3 vNormal;
  uniform vec3 uLight;
  out vec4 frag;
  void main() {
    vec3 n = normalize(vNormal);
    // Key light plus a weak fill from below, so faces turned away from the
    // key are shaded rather than black — the same read as the 2D renderer.
    float key = max(dot(n, uLight), 0.0);
    float fill = max(dot(n, vec3(-0.3, -0.4, -0.2)), 0.0);
    frag = vec4(vColor * (0.42 + 0.62 * key + 0.12 * fill), 1.0);
  }`;

  // Unit cube: 6 faces x 2 triangles, position and face normal per vertex.
  function cubeGeometry() {
    const faces = [
      [[0, 0, 1], [[0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]]],
      [[0, 0, -1], [[0, 1, 0], [1, 1, 0], [1, 0, 0], [0, 0, 0]]],
      [[1, 0, 0], [[1, 0, 0], [1, 1, 0], [1, 1, 1], [1, 0, 1]]],
      [[-1, 0, 0], [[0, 0, 1], [0, 1, 1], [0, 1, 0], [0, 0, 0]]],
      [[0, 1, 0], [[0, 1, 1], [1, 1, 1], [1, 1, 0], [0, 1, 0]]],
      [[0, -1, 0], [[0, 0, 0], [1, 0, 0], [1, 0, 1], [0, 0, 1]]],
    ];
    const pos = [];
    const nrm = [];
    for (const [n, quad] of faces) {
      for (const i of [0, 1, 2, 0, 2, 3]) {
        pos.push(...quad[i]);
        nrm.push(...n);
      }
    }
    return { pos: new Float32Array(pos), nrm: new Float32Array(nrm) };
  }

  /* --- matrices (column-major, as WebGL wants) --- */

  function perspective(fovy, aspect, near, far) {
    const f = 1 / Math.tan(fovy / 2);
    const nf = 1 / (near - far);
    return new Float32Array([
      f / aspect, 0, 0, 0,
      0, f, 0, 0,
      0, 0, (far + near) * nf, -1,
      0, 0, 2 * far * near * nf, 0,
    ]);
  }

  function lookAt(eye, target, up) {
    const z = norm(sub(eye, target));
    const x = norm(cross(up, z));
    const y = cross(z, x);
    return new Float32Array([
      x[0], y[0], z[0], 0,
      x[1], y[1], z[1], 0,
      x[2], y[2], z[2], 0,
      -dot(x, eye), -dot(y, eye), -dot(z, eye), 1,
    ]);
  }

  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const cross = (a, b) => [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
  function norm(v) {
    const l = Math.hypot(v[0], v[1], v[2]) || 1;
    return [v[0] / l, v[1] / l, v[2] / l];
  }

  function multiply(a, b) {
    const out = new Float32Array(16);
    for (let c = 0; c < 4; c++) {
      for (let r = 0; r < 4; r++) {
        let s = 0;
        for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
        out[c * 4 + r] = s;
      }
    }
    return out;
  }

  /* --- viewer --- */

  function create(canvas) {
    const gl = canvas.getContext("webgl2", { antialias: true });
    if (!gl) return null;

    const prog = link(gl, VERT, FRAG);
    const geo = cubeGeometry();
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);

    bindAttrib(gl, prog, "aPos", geo.pos, 3, 0);
    bindAttrib(gl, prog, "aNormal", geo.nrm, 3, 0);
    const offsetBuf = gl.createBuffer();
    const colorBuf = gl.createBuffer();

    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.CULL_FACE);
    gl.cullFace(gl.BACK);

    const uMVP = gl.getUniformLocation(prog, "uMVP");
    const uLight = gl.getUniformLocation(prog, "uLight");

    const state = {
      count: 0,
      center: [0, 0, 0],
      radius: 100,
      azimuth: -0.9,
      elevation: 0.45,
      dirty: true,
    };

    function setInstances(offsets, colors, dims) {
      gl.bindVertexArray(vao);
      instanceAttrib(gl, prog, "aOffset", offsetBuf, offsets, 3, gl.FLOAT, false);
      instanceAttrib(gl, prog, "aColor", colorBuf, colors, 3, gl.UNSIGNED_BYTE, true);
      state.count = offsets.length / 3;
      state.center = [dims[0] / 2, dims[1] / 2, dims[2] / 2];
      // Frame the whole model: back off far enough that its bounding sphere
      // fits the vertical field of view.
      state.radius = Math.max(...dims) * 1.5;
      state.home = state.radius;
      state.dirty = true;
    }

    function draw() {
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const w = Math.max(1, Math.floor(canvas.clientWidth * dpr));
      const h = Math.max(1, Math.floor(canvas.clientHeight * dpr));
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
        state.dirty = true;
      }
      if (!state.dirty || !state.count) return;
      state.dirty = false;

      gl.viewport(0, 0, w, h);
      gl.clearColor(0.078, 0.09, 0.11, 1);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

      const ce = Math.cos(state.elevation);
      const eye = [
        state.center[0] + state.radius * ce * Math.cos(state.azimuth),
        state.center[1] + state.radius * ce * Math.sin(state.azimuth),
        state.center[2] + state.radius * Math.sin(state.elevation),
      ];
      const view = lookAt(eye, state.center, [0, 0, 1]);
      const proj = perspective(0.7, w / h, 0.5, state.radius * 8 + 500);

      gl.useProgram(prog);
      gl.uniformMatrix4fv(uMVP, false, multiply(proj, view));
      gl.uniform3fv(uLight, norm([0.45, 0.35, 0.82]));
      gl.bindVertexArray(vao);
      gl.drawArraysInstanced(gl.TRIANGLES, 0, 36, state.count);
    }

    /* --- input: drag to orbit, wheel or pinch to zoom ---
     *
     * Every pointer is tracked rather than just "is a drag happening", because
     * a phone has no wheel: pinch IS the zoom, and pinch means reading two
     * pointers at once. With one pointer this behaves exactly as a mouse drag
     * did, so the same code serves both without a touch/mouse branch. */

    const pointers = new Map();
    let pinch = 0;

    const spread = () => {
      const [a, b] = [...pointers.values()];
      return Math.hypot(a.x - b.x, a.y - b.y);
    };
    const zoomTo = (r) =>
      Math.max(state.home * 0.15, Math.min(state.home * 6, r));

    canvas.addEventListener("pointerdown", (e) => {
      pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
      try { canvas.setPointerCapture(e.pointerId); } catch { /* fine */ }
      if (pointers.size === 2) pinch = spread();
    });

    const release = (e) => {
      pointers.delete(e.pointerId);
      try { canvas.releasePointerCapture(e.pointerId); } catch { /* already gone */ }
      // A finger lifting off a pinch must not be read as a huge drag by the one
      // still down, so re-baseline whatever is left.
      if (pointers.size === 2) pinch = spread();
    };
    canvas.addEventListener("pointerup", release);
    canvas.addEventListener("pointercancel", release);

    canvas.addEventListener("pointermove", (e) => {
      const prev = pointers.get(e.pointerId);
      if (!prev) return;
      const dx = e.clientX - prev.x;
      const dy = e.clientY - prev.y;
      prev.x = e.clientX;
      prev.y = e.clientY;

      if (pointers.size === 1) {
        state.azimuth -= dx * 0.008;
        // Clamp just shy of the poles: at exactly vertical the up vector and the
        // view direction are parallel and lookAt degenerates.
        state.elevation = Math.max(-1.5, Math.min(1.5, state.elevation + dy * 0.006));
        state.dirty = true;
      } else if (pointers.size === 2) {
        const d = spread();
        if (pinch > 0 && d > 0) {
          state.radius = zoomTo(state.radius * (pinch / d));
          state.dirty = true;
        }
        pinch = d;
      }
    });

    canvas.addEventListener("wheel", (e) => {
      e.preventDefault();
      state.radius = zoomTo(state.radius * Math.exp(e.deltaY * 0.0012));
      state.dirty = true;
    }, { passive: false });

    // Double-tap or double-click back to the default view. On a phone there is
    // no "Reset view" button within reach of the thumb doing the spinning.
    canvas.addEventListener("dblclick", () => {
      state.azimuth = -0.9;
      state.elevation = 0.45;
      state.radius = state.home ?? state.radius;
      state.dirty = true;
    });

    let raf = requestAnimationFrame(function loop() {
      draw();
      raf = requestAnimationFrame(loop);
    });

    return {
      setInstances,
      reset() {
        state.azimuth = -0.9;
        state.elevation = 0.45;
        state.radius = state.home ?? state.radius;
        state.dirty = true;
      },
      invalidate() { state.dirty = true; },
      dispose() { cancelAnimationFrame(raf); },
    };
  }

  /* --- gl helpers --- */

  function link(gl, vsrc, fsrc) {
    const p = gl.createProgram();
    for (const [type, src] of [[gl.VERTEX_SHADER, vsrc], [gl.FRAGMENT_SHADER, fsrc]]) {
      const s = gl.createShader(type);
      gl.shaderSource(s, src.trim());
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
        throw new Error(gl.getShaderInfoLog(s));
      }
      gl.attachShader(p, s);
    }
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    return p;
  }

  function bindAttrib(gl, prog, name, data, size) {
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(prog, name);
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, size, gl.FLOAT, false, 0, 0);
  }

  function instanceAttrib(gl, prog, name, buf, data, size, type, normalized) {
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(prog, name);
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, size, type, normalized, 0, 0);
    gl.vertexAttribDivisor(loc, 1);
  }

  /* --- decoding the server's binary blob --- */

  function decode(buffer, palette) {
    const head = new Uint32Array(buffer, 0, 4);
    const dims = [head[0], head[1], head[2]];
    const count = head[3];
    const pos = new Int16Array(buffer, 16, count * 3);
    const mats = new Uint8Array(buffer, 16 + count * 6, count);

    const offsets = new Float32Array(count * 3);
    const colors = new Uint8Array(count * 3);
    for (let i = 0; i < count; i++) {
      offsets[i * 3] = pos[i * 3];
      offsets[i * 3 + 1] = pos[i * 3 + 1];
      offsets[i * 3 + 2] = pos[i * 3 + 2];
      const c = palette[mats[i]] || [255, 0, 255];
      colors[i * 3] = c[0];
      colors[i * 3 + 1] = c[1];
      colors[i * 3 + 2] = c[2];
    }
    return { offsets, colors, dims, count };
  }

  return { create, decode };
})();
