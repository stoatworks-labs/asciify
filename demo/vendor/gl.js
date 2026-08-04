/**
 * WebGL2 harness for the Resolume plugin demos.
 *
 * The plugins are desktop GL 4.1 core (`#version 410 core`). The browser gives
 * us GLSL ES 3.00, which is close but not identical, so `port()` below is the
 * one place the differences are handled. Every demo's shader source is
 * otherwise the plugin's own text, copied across unedited — that is the whole
 * point of the exercise, and it is why the translation lives here rather than
 * being hand-applied per plugin.
 *
 * Differences that actually bite, all of them handled by port():
 *   - version line and the required default float precision
 *   - `layout( location = N ) in` on a vertex attribute is legal in ES 3.00,
 *     so attribute declarations pass through unchanged
 *   - ES 3.00 has no implicit `highp` for int/sampler in fragment shaders
 *   - `const float x[3] = float[]( ... )` is legal in both, but ES 3.00 is
 *     stricter about the sized form, so `float[N](...)` is normalised
 *
 * What is NOT handled, deliberately: anything that would change the maths. If
 * a plugin's shader will not compile here, the answer is to say so on the page,
 * not to quietly rewrite the shader into something that is no longer the thing
 * being demonstrated.
 */

export class GLError extends Error {}

/**
 * Desktop GL 4.1 core source -> GLSL ES 3.00.
 *
 * Kept textual and minimal. It is not a GLSL parser and must never become one:
 * the moment it is clever enough to rewrite expressions, the demo stops being
 * the plugin's shader.
 */
export function port(source, { stage = 'fragment' } = {}) {
  let out = source.replace(/^\s*#version\s+410\s+core\s*\n/, '');

  // ES 3.00 requires a default precision for float in the fragment stage, and
  // highp for int wherever a hash is being done in integers — old-cathode and
  // downpour both depend on 32-bit uint wraparound, which mediump would not
  // give.
  //
  // The integer samplers have no default precision at all in ES 3.00 — unlike
  // sampler2D, which at least has one in the fragment stage — so a shader that
  // reads a usampler2D fails to compile with "No precision specified" and
  // nothing else. Desktop GL needs none of this, which is why the plugins'
  // sources carry none. coinop reads its playfield through a usampler2D.
  const precision =
    stage === 'fragment'
      ? 'precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n'
        + 'precision highp usampler2D;\nprecision highp isampler2D;\n'
      : 'precision highp float;\nprecision highp int;\n';

  // `float[]( a, b )` -> `float[N]( a, b )`. Both spellings are legal in
  // desktop GL; ES 3.00 wants the size on the constructor when the array is
  // declared with one.
  out = out.replace(/\bfloat\s*\[\s*\]\s*\(/g, (m, offset) => {
    const upto = out.slice(0, offset);
    const decl = /\[\s*(\d+)\s*\]\s*=\s*$/.exec(upto);
    return decl ? `float[${decl[1]}](` : 'float[](';
  });

  return `#version 300 es\n${precision}${out}`;
}

function compile(gl, type, source, label) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(shader);
    gl.deleteShader(shader);
    throw new GLError(`${label} shader failed to compile:\n${log}`);
  }
  return shader;
}

/**
 * A compiled program plus the uniform-setting sugar the plugins' own
 * `shader.Set( "Name", ... )` calls read like. Unknown names are ignored the
 * way glUniform(-1) is a no-op in GL — but they are counted, because a uniform
 * name that does not match the shader is exactly the silent failure old-cathode
 * documents (`glGetUniformLocation` returns -1 and the control is stone dead).
 */
export class Program {
  /**
   * @param {object} [options]
   * @param {Record<string, number>} [options.attribs] attribute name -> location,
   *   for a pass whose geometry is not the standard quad. Scopes' graticule is
   *   the case: `vPosition` is a vec2 and location 1 is `vBrightness`, not `vUV`.
   */
  constructor(gl, vertexSource, fragmentSource, label = 'program', options = {}) {
    this.gl = gl;
    this.label = label;
    const vs = compile(gl, gl.VERTEX_SHADER, port(vertexSource, { stage: 'vertex' }), `${label} vertex`);
    const fs = compile(gl, gl.FRAGMENT_SHADER, port(fragmentSource, { stage: 'fragment' }), `${label} fragment`);

    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    // The plugins' quad uses location 0 for position and 1 for UV.
    const attribs = options.attribs ?? { vPosition: 0, vUV: 1 };
    for (const [name, location] of Object.entries(attribs)) {
      gl.bindAttribLocation(program, location, name);
    }
    gl.linkProgram(program);
    gl.deleteShader(vs);
    gl.deleteShader(fs);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      const log = gl.getProgramInfoLog(program);
      gl.deleteProgram(program);
      throw new GLError(`${label} failed to link:\n${log}`);
    }

    this.program = program;
    this.locations = new Map();
    this.missing = new Set();
  }

  use() {
    this.gl.useProgram(this.program);
    return this;
  }

  location(name) {
    if (!this.locations.has(name)) {
      this.locations.set(name, this.gl.getUniformLocation(this.program, name));
    }
    return this.locations.get(name);
  }

  /** Mirrors FFGLShader::Set — the value shape picks the glUniform overload. */
  set(name, ...values) {
    const loc = this.location(name);
    if (loc === null) {
      this.missing.add(name);
      return this;
    }
    const gl = this.gl;
    const [a, b, c, d] = values;

    if (values.length === 1 && typeof a === 'object' && a !== null && 'length' in a) {
      const v = a;
      if (v.length === 2) gl.uniform2f(loc, v[0], v[1]);
      else if (v.length === 3) gl.uniform3f(loc, v[0], v[1], v[2]);
      else if (v.length === 4) gl.uniform4f(loc, v[0], v[1], v[2], v[3]);
      else gl.uniform1fv(loc, v);
      return this;
    }

    switch (values.length) {
      case 1:
        if (Number.isInteger(a) && Object.is(a, Math.trunc(a)) && this._isIntUniform(name)) gl.uniform1i(loc, a);
        else gl.uniform1f(loc, a);
        break;
      case 2: gl.uniform2f(loc, a, b); break;
      case 3: gl.uniform3f(loc, a, b, c); break;
      case 4: gl.uniform4f(loc, a, b, c, d); break;
      default: throw new GLError(`set(${name}) with ${values.length} values`);
    }
    return this;
  }

  /** Explicit integer/uint setters, for the uniforms declared as such. */
  setInt(name, value) {
    const loc = this.location(name);
    if (loc === null) { this.missing.add(name); return this; }
    this.gl.uniform1i(loc, value | 0);
    return this;
  }

  setUint(name, value) {
    const loc = this.location(name);
    if (loc === null) { this.missing.add(name); return this; }
    this.gl.uniform1ui(loc, value >>> 0);
    return this;
  }

  setSampler(name, unit) {
    return this.setInt(name, unit);
  }

  /**
   * A whole `uniform float name[N]`, `uniform vec3 name[N]` or
   * `uniform vec4 name[N]` at once.
   *
   * `components` has to be passed and has to be right: glUniform1fv on a vec4
   * array is rejected as a size mismatch, and the uniform keeps whatever it held
   * before — which for orrery's instance arrays is every shape at the origin at
   * zero size, i.e. a plugin that draws nothing and looks like it is off-screen.
   */
  setArray(name, values, components = 1) {
    const loc = this.location(`${name}[0]`) ?? this.location(name);
    if (loc === null) { this.missing.add(name); return this; }
    if (components === 4) this.gl.uniform4fv(loc, values);
    else if (components === 3) this.gl.uniform3fv(loc, values);
    else this.gl.uniform1fv(loc, values);
    return this;
  }

  _isIntUniform() {
    // set() is only ever handed floats by the ported call sites; the integer
    // uniforms go through setInt/setUint/setSampler explicitly.
    return false;
  }

  dispose() {
    this.gl.deleteProgram(this.program);
  }
}

/**
 * The full-frame quad the FFGL SDK draws, in the same winding and with the same
 * attribute layout, so the ported shaders see the uv they expect.
 */
export class Quad {
  constructor(gl) {
    this.gl = gl;
    this.vao = gl.createVertexArray();
    this.buffer = gl.createBuffer();

    // x, y, u, v — a triangle strip covering the frame, uv origin bottom-left
    // to match GL's texture origin (and therefore the plugins').
    const data = new Float32Array([
      -1, -1, 0, 0,
      1, -1, 1, 0,
      -1, 1, 0, 1,
      1, 1, 1, 1,
    ]);

    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 16, 8);
    gl.bindVertexArray(null);
  }

  draw() {
    const gl = this.gl;
    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.bindVertexArray(null);
  }

  dispose() {
    this.gl.deleteVertexArray(this.vao);
    this.gl.deleteBuffer(this.buffer);
  }
}

/**
 * An off-screen colour target, the browser equivalent of the plugins'
 * PassBuffer / ScopeBuffer. `ensure()` reallocates only on a real size or
 * format change, as theirs does.
 */
export class PassBuffer {
  constructor(gl, { filter = 'linear', mip = false } = {}) {
    this.gl = gl;
    this.filter = filter;
    this.mip = mip;
    this.width = 0;
    this.height = 0;
    this.internalFormat = null;
    this.texture = null;
    this.fbo = null;
  }

  ensure(width, height, internalFormat = null) {
    const gl = this.gl;
    const target = internalFormat ?? gl.RGBA8;
    width = Math.max(1, Math.floor(width));
    height = Math.max(1, Math.floor(height));

    if (this.texture && this.width === width && this.height === height && this.internalFormat === target) {
      return this;
    }

    this.dispose();

    this.width = width;
    this.height = height;
    this.internalFormat = target;

    this.texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.texture);
    gl.texStorage2D(gl.TEXTURE_2D, this.mip ? mipLevels(width, height) : 1, target, width, height);

    const min = this.mip
      ? (this.filter === 'nearest' ? gl.NEAREST_MIPMAP_NEAREST : gl.LINEAR_MIPMAP_LINEAR)
      : (this.filter === 'nearest' ? gl.NEAREST : gl.LINEAR);
    const mag = this.filter === 'nearest' ? gl.NEAREST : gl.LINEAR;
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, min);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, mag);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

    this.fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.texture, 0);

    const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.bindTexture(gl.TEXTURE_2D, null);

    if (status !== gl.FRAMEBUFFER_COMPLETE) {
      throw new GLError(`pass buffer incomplete (0x${status.toString(16)}) at ${width}x${height}`);
    }
    return this;
  }

  bind() {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.viewport(0, 0, this.width, this.height);
    return this;
  }

  /** Bind and clear. The scope buffer's background is written premultiplied. */
  clearTo(r, g, b, a) {
    const gl = this.gl;
    this.bind();
    gl.disable(gl.BLEND);
    gl.clearColor(r, g, b, a);
    gl.clear(gl.COLOR_BUFFER_BIT);
    return this;
  }

  generateMipmap() {
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.texture);
    gl.generateMipmap(gl.TEXTURE_2D);
    gl.bindTexture(gl.TEXTURE_2D, null);
    return this;
  }

  dispose() {
    const gl = this.gl;
    if (this.fbo) gl.deleteFramebuffer(this.fbo);
    if (this.texture) gl.deleteTexture(this.texture);
    this.fbo = null;
    this.texture = null;
    this.width = 0;
    this.height = 0;
  }
}

export function mipLevels(width, height) {
  return 1 + Math.floor(Math.log2(Math.max(width, height)));
}

export function bindTexture(gl, unit, texture) {
  gl.activeTexture(gl.TEXTURE0 + unit);
  gl.bindTexture(gl.TEXTURE_2D, texture);
}

/**
 * A WebGL2 context with the extensions the ports need, or a thrown error
 * naming the one that is missing. Failing loudly here is deliberate: a demo
 * that silently drops to 8-bit intermediates would render a *plausible* wrong
 * picture, which is the failure mode these plugins' own test harnesses exist
 * to prevent.
 */
export function createContext(canvas, { needFloatRender = false, needFloatBlend = false } = {}) {
  const gl = canvas.getContext('webgl2', {
    alpha: true,
    antialias: false,
    depth: false,
    stencil: false,
    premultipliedAlpha: true,
    preserveDrawingBuffer: false,
    powerPreference: 'high-performance',
  });

  if (!gl) {
    throw new GLError('This browser has no WebGL2. The demo needs it — the plugins are GL 4.1 and the port has nowhere shallower to go.');
  }

  gl.getExtension('OES_texture_float_linear');
  if (needFloatRender && !gl.getExtension('EXT_color_buffer_float')) {
    throw new GLError('EXT_color_buffer_float is missing. These passes need a float render target — 8 bits mid-chain would quantise the intermediate and produce a plausible wrong picture rather than an obvious one.');
  }

  // Blending into a 32-bit float target is core on desktop GL but an extension
  // in WebGL2. Scopes needs it: the histogram accumulates by additive blending
  // into RGBA32F, and it has to be 32 and not 16 because adding 1.0 to a
  // half-float bin holding 4096 rounds straight back to 4096 — the bins that
  // stall first are the tallest, which are exactly the clipped whites and
  // crushed blacks the histogram was opened to find.
  if (needFloatBlend && !gl.getExtension('EXT_float_blend')) {
    throw new GLError('EXT_float_blend is missing. The histogram accumulates by blending into a 32-bit float target, and without it the tallest bins would silently stop counting.');
  }

  return gl;
}
