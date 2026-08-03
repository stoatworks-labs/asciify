/**
 * The parameter model and the inspector panel.
 *
 * A demo declares its parameters in the shape FFGL declares them — the same
 * names, the same types, the same groups, the same 0..1 defaults — and this
 * builds the controls and holds the values. Two rules make that worth doing
 * rather than hand-rolling sliders per page:
 *
 *   1. **Every parameter is a plain 0..1 float**, exactly as it is in the
 *      plugins. `SetParamInfo` clamps an FF_TYPE_STANDARD default into 0..1
 *      before a range can be attached (SDK b1afaf9), so all five plugins that
 *      have a control in degrees or columns or MHz store it as 0..1 and convert
 *      on the way to the shader. The demos keep that split: `value` is the
 *      host's number, `display` is the plugin's own conversion function, and
 *      the panel shows both.
 *   2. **Option parameters carry their element list**, so a dropdown here has
 *      the same entries in the same order as the host's, and the value is the
 *      element index the shader branches on.
 *
 * Types, matching the FF_TYPE_* the plugins declare:
 *   standard   0..1 slider
 *   boolean    toggle, 0 or 1
 *   option     dropdown, value is the element index
 *   text       free text
 *   colour     three consecutive RED/GREEN/BLUE params shown as one swatch
 */

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);

/**
 * A parameter's value, in the range that parameter actually has.
 *
 * **Not everything is 0..1, and this is the one place that matters.** A
 * continuous FF_TYPE_STANDARD really is 0..1 — the SDK clamps its default into
 * that range before a range can be attached, which is why all five plugins
 * convert to degrees or columns or MHz on the way to the shader. But an
 * FF_TYPE_OPTION stores the *element index*, and those go up to 8 (Downpour's
 * Source). Clamping those to 0..1 too meant every dropdown silently snapped
 * back to its second entry: Histogram read as Vectorscope, VHS EP as VHS SP,
 * Box drawing as Letters. The controls looked live and did nothing, which is
 * the exact failure old-cathode's own notes warn about in GLSL and is no better
 * here.
 */
function coerce(param, value) {
  if (param.type === 'text') return String(value);
  const n = Number(value);
  if (!Number.isFinite(n)) return 0;
  if (param.type === 'option') {
    const last = Math.max(0, (param.elements?.length ?? 1) - 1);
    return Math.min(last, Math.max(0, Math.round(n)));
  }
  return clamp01(n);
}

export class Params extends EventTarget {
  /**
   * @param {Array} spec  parameter descriptors, in declaration order
   */
  constructor(spec) {
    super();
    this.spec = spec;
    this.byId = new Map();
    this.values = {};

    for (const p of spec) {
      this.byId.set(p.id, p);
      this.values[p.id] = p.type === 'text' ? (p.default ?? '') : (p.default ?? 0);
    }
    this.defaults = { ...this.values };
  }

  get(id) {
    return this.values[id];
  }

  /** The option index for an option parameter, rounded the way the plugins do. */
  option(id) {
    return Math.round(this.values[id]);
  }

  /** The plugin-space value: `display` applied, or the raw 0..1 if there is none. */
  real(id) {
    const p = this.byId.get(id);
    return p?.convert ? p.convert(this.values[id]) : this.values[id];
  }

  set(id, value, { silent = false } = {}) {
    const p = this.byId.get(id);
    if (!p) return;
    const next = coerce(p, value);
    if (this.values[id] === next) return;
    this.values[id] = next;
    if (!silent) this.dispatchEvent(new CustomEvent('change', { detail: { id, value: next } }));
  }

  reset() {
    this.values = { ...this.defaults };
    this.dispatchEvent(new CustomEvent('change', { detail: { id: null, value: null } }));
    this.dispatchEvent(new CustomEvent('reset'));
  }

  /**
   * A preset is a partial set of values; anything it does not name goes back to
   * its default, so a preset is a whole state rather than a diff on whatever
   * was on screen.
   */
  apply(values) {
    this.values = { ...this.defaults };
    for (const [id, value] of Object.entries(values)) {
      if (this.byId.has(id)) {
        this.values[id] = coerce(this.byId.get(id), value);
      }
    }
    this.dispatchEvent(new CustomEvent('change', { detail: { id: null, value: null } }));
    this.dispatchEvent(new CustomEvent('reset'));
  }

  /** Everything that differs from default, for the shareable URL. */
  toQuery() {
    const out = new URLSearchParams();
    for (const p of this.spec) {
      const v = this.values[p.id];
      if (v === this.defaults[p.id]) continue;
      out.set(p.id, p.type === 'text' ? v : String(Math.round(v * 1000) / 1000));
    }
    return out;
  }

  fromQuery(query) {
    let any = false;
    for (const [id, raw] of query) {
      if (!this.byId.has(id)) continue;
      this.values[id] = coerce(this.byId.get(id), raw);
      any = true;
    }
    if (any) {
      this.dispatchEvent(new CustomEvent('change', { detail: { id: null, value: null } }));
      this.dispatchEvent(new CustomEvent('reset'));
    }
    return any;
  }
}

/** Groups in declaration order, the way SetParamGroup collapses runs. */
function grouped(spec) {
  const groups = [];
  let current = null;
  for (const p of spec) {
    const name = p.group ?? '';
    if (!current || current.name !== name) {
      current = { name, params: [] };
      groups.push(current);
    }
    current.params.push(p);
  }
  return groups;
}

/**
 * Collapse three consecutive RED/GREEN/BLUE parameters into one swatch row,
 * which is what a host does with them and why the plugins name them that way.
 */
function foldColours(params) {
  const rows = [];
  for (let i = 0; i < params.length; i += 1) {
    const p = params[i];
    if (p.type === 'colour' && params[i + 1]?.type === 'colour' && params[i + 2]?.type === 'colour') {
      rows.push({ kind: 'colour', name: p.name, parts: [p, params[i + 1], params[i + 2]] });
      i += 2;
      continue;
    }
    rows.push({ kind: 'single', param: p });
  }
  return rows;
}

const toHex = (r, g, b) =>
  `#${[r, g, b].map((c) => Math.round(clamp01(c) * 255).toString(16).padStart(2, '0')).join('')}`;

/** Builds the inspector DOM and keeps it in step with a Params instance. */
export function buildPanel(params, container) {
  const controls = new Map();

  for (const group of grouped(params.spec)) {
    const section = document.createElement('section');
    section.className = 'pgroup';

    if (group.name) {
      const heading = document.createElement('h3');
      heading.className = 'pgroup__name';
      heading.textContent = group.name;
      section.append(heading);
    }

    for (const row of foldColours(group.params)) {
      section.append(row.kind === 'colour' ? colourRow(params, row, controls) : singleRow(params, row.param, controls));
    }

    container.append(section);
  }

  const sync = () => {
    for (const update of controls.values()) update();
  };
  params.addEventListener('reset', sync);
  params.addEventListener('change', (e) => {
    if (e.detail?.id === null) sync();
    else controls.get(e.detail.id)?.();
  });
  sync();

  return { sync };
}

function labelFor(param) {
  const label = document.createElement('label');
  label.className = 'prow__name';
  label.textContent = param.name;
  if (param.hint) label.title = param.hint;
  return label;
}

function singleRow(params, param, controls) {
  const row = document.createElement('div');
  row.className = `prow prow--${param.type}`;
  const label = labelFor(param);
  row.append(label);

  if (param.type === 'option') {
    const select = document.createElement('select');
    select.className = 'prow__select';
    param.elements.forEach((name, index) => {
      const opt = document.createElement('option');
      opt.value = String(index);
      opt.textContent = name;
      select.append(opt);
    });
    select.addEventListener('change', () => params.set(param.id, Number(select.value)));
    label.htmlFor = select.id = `p-${param.id}`;
    row.append(select);
    controls.set(param.id, () => { select.value = String(Math.round(params.get(param.id))); });
    return row;
  }

  if (param.type === 'boolean') {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'prow__toggle';
    button.addEventListener('click', () => params.set(param.id, params.get(param.id) > 0.5 ? 0 : 1));
    row.append(button);
    controls.set(param.id, () => {
      const on = params.get(param.id) > 0.5;
      button.classList.toggle('is-on', on);
      button.setAttribute('aria-pressed', String(on));
      button.textContent = on ? 'On' : 'Off';
    });
    return row;
  }

  if (param.type === 'text') {
    const input = document.createElement('input');
    input.type = 'text';
    input.className = 'prow__text';
    input.spellcheck = false;
    if (param.placeholder) input.placeholder = param.placeholder;
    input.addEventListener('input', () => params.set(param.id, input.value));
    label.htmlFor = input.id = `p-${param.id}`;
    row.append(input);
    controls.set(param.id, () => {
      if (input.value !== params.get(param.id)) input.value = params.get(param.id);
    });
    return row;
  }

  // standard, and the individual channels of a colour when they are not folded
  const slider = document.createElement('input');
  slider.type = 'range';
  slider.min = '0';
  slider.max = '1';
  slider.step = '0.001';
  slider.className = 'prow__slider';
  label.htmlFor = slider.id = `p-${param.id}`;

  const readout = document.createElement('output');
  readout.className = 'prow__value';

  slider.addEventListener('input', () => params.set(param.id, Number(slider.value)));
  // Double-click a slider to put it back where the plugin's constructor had it.
  slider.addEventListener('dblclick', () => params.set(param.id, params.defaults[param.id]));

  row.append(slider, readout);
  controls.set(param.id, () => {
    const v = params.get(param.id);
    slider.value = String(v);
    readout.textContent = format(param, v);
    readout.title = `host value ${v.toFixed(3)} — double-click the slider for the default`;
  });
  return row;
}

function colourRow(params, row, controls) {
  const el = document.createElement('div');
  el.className = 'prow prow--colour';

  const label = document.createElement('label');
  label.className = 'prow__name';
  label.textContent = row.name;
  el.append(label);

  const input = document.createElement('input');
  input.type = 'color';
  input.className = 'prow__swatch';
  label.htmlFor = input.id = `p-${row.parts[0].id}`;

  input.addEventListener('input', () => {
    const hex = input.value;
    const channels = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 255);
    row.parts.forEach((p, i) => params.set(p.id, channels[i]));
  });

  el.append(input);

  const update = () => {
    input.value = toHex(...row.parts.map((p) => params.get(p.id)));
  };
  row.parts.forEach((p) => controls.set(p.id, update));
  return el;
}

/**
 * What the readout says. `display` is the plugin's own conversion — degrees for
 * porthole's field of view, columns for asciify, rows per second for downpour —
 * so the number beside the slider is the number the plugin is working in, not
 * a made-up scale.
 */
function format(param, value) {
  if (param.display) return param.display(value);
  return value.toFixed(2);
}
