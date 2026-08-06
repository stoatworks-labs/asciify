/**
 * The page: chrome, clock, canvas, inspector, and the honest bit at the top.
 *
 * A demo module hands `mountDemo` a description of one plugin — its identity,
 * its parameter declarations, which generated clips suit it, and a render
 * object that owns the ported shaders. Everything else on the page is here,
 * once, so six demos cannot end up making six slightly different claims about
 * what they are.
 *
 * The rule this file exists to enforce, from `pages-demo/README.md`: **it
 * always says it is a demo**. Nobody should be able to mistake this for the
 * plugin. The banner is not optional and the "how this differs" disclosure is
 * built from the plugin's own list of caveats rather than from a template, so
 * it cannot quietly go out of date by being generic.
 */

import { createContext, Quad, GLError } from './gl.js';
import { Params, buildPanel } from './params.js';
import { SOURCES, SourceRenderer } from './sources.js';

const RESOLUTIONS = [
  { id: '640', name: '640 × 360', width: 640, height: 360 },
  { id: '960', name: '960 × 540', width: 960, height: 540 },
  { id: '1280', name: '1280 × 720', width: 1280, height: 720 },
  { id: '1920', name: '1920 × 1080', width: 1920, height: 1080 },
];

const BACKDROPS = [
  { id: 'checker', name: 'Checkerboard' },
  { id: 'black', name: 'Black' },
  { id: 'white', name: 'White' },
];

/**
 * `?clip=<id>` — which generated clip to run. Ignored if the plugin does not
 * list it, because a demo's `sources` is a statement about what suits that
 * plugin, not a menu the URL may override.
 */
function pickSource(demo, query) {
  const wanted = query.get('clip');
  return wanted && demo.sources.includes(wanted) ? wanted : demo.sources[0];
}

/**
 * `?size=1920x1080`, or one of the preset ids. Free-form rather than a preset
 * list because an embed is sized by whatever is consuming it, and an OBS
 * browser source is routinely a raster no menu here would have guessed.
 *
 * Embeds default to 1280x720 rather than the page's 960x540: this is a video
 * source, and the page's default is chosen to sit beside an inspector.
 */
function pickResolution(query, embed) {
  const wanted = query.get('size');
  if (wanted) {
    const preset = RESOLUTIONS.find((r) => r.id === wanted);
    if (preset) return preset;

    const match = /^(\d{2,5})x(\d{2,5})$/.exec(wanted);
    if (match) {
      const width = Number(match[1]);
      const height = Number(match[2]);
      // A canvas larger than the GPU will allocate fails at draw time with a
      // blank frame, which in a mixer reads as "the source is broken".
      if (width > 0 && height > 0 && width <= 7680 && height <= 4320) {
        return { id: wanted, name: `${width} × ${height}`, width, height };
      }
    }
  }
  return embed ? RESOLUTIONS[2] : RESOLUTIONS[1];
}

/**
 * `?bg=checker|black|white`. Embeds default to black rather than the
 * checkerboard: the checker exists to make alpha legible to a reader, and
 * baking it into a video source would be a bug.
 */
function pickBackdrop(query, embed) {
  const wanted = query.get('bg');
  if (BACKDROPS.some((b) => b.id === wanted)) return wanted;
  return embed ? 'black' : 'checker';
}

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text != null) node.textContent = text;
  return node;
}

/**
 * @param {object} demo
 * @param {string} demo.name          plugin name, as the host shows it
 * @param {string} demo.pluginId      FFGL id, e.g. "PH01"
 * @param {string} demo.tagline
 * @param {string} demo.repo          GitHub URL
 * @param {string} demo.page          stoatworks-labs.com project page
 * @param {string} [demo.video]       YouTube watch URL
 * @param {string[]} demo.differences what this page does NOT reproduce
 * @param {Array} demo.params         parameter declarations (see params.js)
 * @param {string[]} demo.sources     ids from sources.js, first is the default
 * @param {object} [demo.presets]     name -> partial parameter values
 * @param {boolean} [demo.needFloat]  the chain needs EXT_color_buffer_float
 * @param {boolean} [demo.showBackdrop] the output has meaningful alpha
 * @param {Function} demo.createRenderer  (gl, quad) => renderer
 */
export function mountDemo(demo) {
  const root = document.querySelector('#demo');
  root.innerHTML = '';

  const query = new URLSearchParams(window.location.search);

  // ---- embed mode ---------------------------------------------------------
  //
  // `?embed=1` renders the output and nothing else: no banner, no inspector,
  // no transport, no footer, canvas filling the viewport. It exists so the
  // render can be a *video source* — an OBS browser source, a WebLinked URL —
  // rather than a page someone reads.
  //
  // **This is a deliberate exception to the rule at the top of this file**,
  // that the page always says it is a demo. That rule protects a reader from
  // mistaking the demo for the plugin, and in embed mode there is no reader:
  // the output is a texture in somebody's mixer. What survives is everything a
  // reader or a crawler would actually meet — the document title, the meta
  // description, and a visually-hidden statement in the DOM. What is removed is
  // only the furniture that would otherwise be *burnt into the video*.
  //
  // Do not "fix" this by putting a watermark back. A caption baked into a live
  // output is not a disclosure, it is a defect in somebody's show.
  const embed = query.has('embed') && query.get('embed') !== '0';

  const params = new Params(demo.params);
  const state = {
    sourceId: pickSource(demo, query),
    resolution: pickResolution(query, embed),
    backdrop: pickBackdrop(query, embed),
    playing: true,
    time: 0,
    lastFrame: 0,
    frameIndex: 0,
    // Which bundle, where a repo ships more than one from the same class.
    // Downpour is the case: `Downpour` (a source) and `Downpour Over` (an
    // effect) differ by a constructor flag and a #define handed to the shader
    // compiler. It belongs here and not in the inspector, because it is not
    // something the plugin declares as a parameter — it is which plugin.
    variant: demo.variants?.default ?? null,
  };

  const canvasWrap = el('div', 'stage__canvas');
  const canvas = el('canvas', 'stage__gl');
  canvasWrap.append(canvas);

  const status = el('p', 'stage__status');
  status.hidden = true;

  let transport = null;

  if (embed) {
    // `data-embed` on <body>, not on #demo: the support footer is injected as a
    // sibling of #demo by a separate script, so the only rule that reliably
    // catches it hides body's other children. Setting it here rather than in
    // the page keeps every repo's index.html identical.
    document.body.dataset.embed = '';

    // The disclosure a reader or a crawler still meets. Not decoration: it is
    // what makes removing the banner defensible.
    const hidden = el(
      'p',
      'visually-hidden',
      `${demo.name}: a browser demo of an FFGL plugin's own shader, ported to WebGL2. This is not the plugin.`,
    );
    root.append(hidden, canvasWrap, status);

    // The inspector normally does this on the way to building its panel.
    params.fromQuery(query);
  } else {
    root.append(buildHeader(demo));

    const stage = el('div', 'stage');
    transport = buildTransport(demo, state, params, () => request());
    stage.append(canvasWrap, transport, status);

    const inspector = el('aside', 'inspector');
    inspector.append(buildInspectorHead(demo, params));
    const panelBody = el('div', 'inspector__body');
    inspector.append(panelBody);

    const layout = el('div', 'layout');
    layout.append(stage, inspector);
    root.append(layout);
    root.append(buildDifferences(demo));

    buildPanel(params, panelBody);
  }

  // ---- GL -----------------------------------------------------------------
  let gl;
  let quad;
  let sources;
  let renderer;

  try {
    gl = createContext(canvas, {
      needFloatRender: demo.needFloat === true,
      needFloatBlend: demo.needFloatBlend === true,
    });
    quad = new Quad(gl);
    sources = new SourceRenderer(gl, quad);
    renderer = demo.createRenderer(gl, quad);
  } catch (error) {
    fail(error);
    return;
  }

  function fail(error) {
    status.hidden = false;
    status.className = 'stage__status is-error';
    status.textContent =
      error instanceof GLError
        ? error.message
        : `The demo could not start: ${error.message}`;
    canvasWrap.hidden = true;
    transport.hidden = true;
    // eslint-disable-next-line no-console
    console.error(error);
  }

  // ---- clock --------------------------------------------------------------
  //
  // The plugins that need a clock ask the host for one (SetTimeSupported), so
  // that re-rendering the same composition gives the same picture rather than
  // whatever the wall clock said. Same here: `state.time` is accumulated from
  // frame deltas and is the only clock any ported shader sees, which is what
  // makes pause and single-step exact.
  let pending = null;

  function request() {
    if (pending === null) pending = requestAnimationFrame(frame);
  }

  function frame(now) {
    pending = null;
    const previous = state.lastFrame || now;
    state.lastFrame = now;

    if (state.playing) {
      state.time += Math.min(0.1, (now - previous) / 1000);
      state.frameIndex = (state.frameIndex + 1) % 100000;
    }

    try {
      draw();
    } catch (error) {
      fail(error);
      return;
    }

    if (state.playing) request();
  }

  function draw() {
    const { width, height } = state.resolution;
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }

    const clip = SOURCES.find((s) => s.id === state.sourceId) ?? SOURCES[0];
    const input = sources.render(state.sourceId === 'user' ? 'user' : clip, width, height, state.time);

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.disable(gl.BLEND);

    renderer.render({
      input,
      params,
      width,
      height,
      time: state.time,
      frameIndex: state.frameIndex,
      variant: state.variant,
    });
  }

  params.addEventListener('change', () => { if (!state.playing) request(); });

  transport?.addEventListener('demo:state', () => {
    canvasWrap.dataset.backdrop = demo.showBackdrop ? state.backdrop : 'black';
    request();
  });

  // A still or a video the visitor picked. It is decoded here and uploaded to a
  // texture; it never goes near a network.
  transport?.addEventListener('demo:adopt', async (event) => {
    try {
      const [width, height] = await sources.adopt(event.detail);
      state.sourceId = 'user';
      state.resolution = { id: 'user', name: `${width} × ${height}`, width, height };
      status.hidden = false;
      status.className = 'stage__status';
      status.textContent = `Using your own ${event.detail.type.startsWith('video/') ? 'video' : 'image'} at ${width} × ${height}. It stays in this page.`;
      request();
    } catch (error) {
      status.hidden = false;
      status.className = 'stage__status is-error';
      status.textContent = `That file could not be decoded: ${error.message}`;
    }
  });

  canvasWrap.dataset.backdrop = state.backdrop;
  if (!demo.showBackdrop) canvasWrap.dataset.backdrop = 'black';

  request();

  // Stop burning a GPU on a tab nobody is looking at.
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && state.playing) request();
  });

  return { params, state, redraw: request };
}

function buildHeader(demo) {
  const header = el('header', 'head');

  const banner = el('div', 'banner');
  banner.append(el('strong', 'banner__tag', 'Browser demo'));
  banner.append(
    el(
      'p',
      'banner__text',
      `This is not the plugin. It is ${demo.name}'s own GLSL, ported from the repository to WebGL2 and running on generated clips in this page — same parameters, same maths, no install. The plugin itself is a native FFGL effect for Resolume Arena and Avenue.`,
    ),
  );
  header.append(banner);

  const title = el('div', 'head__title');
  title.append(el('h1', null, demo.name));
  if (demo.pluginId) title.append(el('span', 'head__id', demo.pluginId));
  header.append(title);
  header.append(el('p', 'head__tagline', demo.tagline));

  const links = el('nav', 'head__links');
  const add = (href, text, external = true) => {
    const a = el('a', 'head__link', text);
    a.href = href;
    if (external) { a.rel = 'noopener'; a.target = '_blank'; }
    links.append(a);
  };
  add(demo.page, 'Project page and downloads');
  add(demo.repo, 'Source on GitHub');
  if (demo.video) add(demo.video, 'Video');
  header.append(links);

  return header;
}

function buildInspectorHead(demo, params) {
  const head = el('div', 'inspector__head');
  head.append(el('h2', 'inspector__title', 'Parameters'));

  const note = el('p', 'inspector__note');
  note.textContent =
    `Every parameter ${demo.name} declares, in the plugin's own order and groups, with its own names and defaults. The number beside a slider is the value the plugin converts it to; the host itself sees 0..1.`;
  head.append(note);

  const actions = el('div', 'inspector__actions');

  if (demo.presets) {
    const select = el('select', 'inspector__presets');
    select.append(new Option('Presets…', ''));
    for (const name of Object.keys(demo.presets)) select.append(new Option(name, name));
    select.addEventListener('change', () => {
      if (!select.value) return;
      params.apply(demo.presets[select.value]);
      select.value = '';
    });
    actions.append(select);
  }

  const reset = el('button', 'btn', 'Defaults');
  reset.type = 'button';
  reset.title = "Back to the values the plugin's constructor sets";
  reset.addEventListener('click', () => params.reset());
  actions.append(reset);

  const share = el('button', 'btn', 'Copy link');
  share.type = 'button';
  share.title = 'A URL carrying exactly these settings';
  share.addEventListener('click', async () => {
    const url = new URL(window.location.href);
    url.search = params.toQuery().toString();
    try {
      await navigator.clipboard.writeText(url.toString());
      share.textContent = 'Copied';
    } catch {
      share.textContent = 'Copy failed';
    }
    setTimeout(() => { share.textContent = 'Copy link'; }, 1600);
  });
  actions.append(share);

  head.append(actions);

  params.fromQuery(new URLSearchParams(window.location.search));

  return head;
}

function buildTransport(demo, state, params, request) {
  const bar = el('div', 'transport');
  const announce = () => bar.dispatchEvent(new CustomEvent('demo:state', { bubbles: true }));

  const play = el('button', 'btn btn--play', 'Pause');
  play.type = 'button';
  play.addEventListener('click', () => {
    state.playing = !state.playing;
    play.textContent = state.playing ? 'Pause' : 'Play';
    state.lastFrame = 0;
    request();
  });

  const step = el('button', 'btn', 'Step');
  step.type = 'button';
  step.title = 'One frame at 60fps — every clip is a function of time alone, so a step is exact';
  step.addEventListener('click', () => {
    state.time += 1 / 60;
    state.frameIndex = (state.frameIndex + 1) % 100000;
    request();
  });

  const restart = el('button', 'btn', 'Restart');
  restart.type = 'button';
  restart.addEventListener('click', () => {
    state.time = 0;
    state.frameIndex = 0;
    request();
  });

  bar.append(play, step, restart);

  // ---- which bundle -------------------------------------------------------
  if (demo.variants) {
    const label = el('label', 'transport__field');
    label.append(el('span', 'transport__label', demo.variants.label));
    const select = el('select', 'transport__select');
    for (const option of demo.variants.options) {
      const node = new Option(option.name, option.id);
      node.title = option.hint ?? '';
      select.append(node);
    }
    select.value = state.variant;
    select.addEventListener('change', () => {
      state.variant = select.value;
      announce();
    });
    label.append(select);
    bar.append(label);
  }

  // ---- clip ---------------------------------------------------------------
  const clipLabel = el('label', 'transport__field');
  clipLabel.append(el('span', 'transport__label', 'Clip'));
  const clip = el('select', 'transport__select');
  for (const id of demo.sources) {
    const source = SOURCES.find((s) => s.id === id);
    if (!source) continue;
    const option = new Option(source.name, id);
    option.title = source.hint;
    clip.append(option);
  }
  clip.value = state.sourceId;
  clip.addEventListener('change', () => {
    state.sourceId = clip.value;
    announce();
  });
  clipLabel.append(clip);
  bar.append(clipLabel);

  // ---- own file -----------------------------------------------------------
  const fileLabel = el('label', 'transport__file');
  fileLabel.append(el('span', 'btn', 'Use my own…'));
  const file = el('input');
  file.type = 'file';
  file.accept = 'image/*,video/*';
  file.hidden = true;
  fileLabel.title = 'An image or a video from this machine. It is decoded in the page and never uploaded anywhere.';
  fileLabel.append(file);
  bar.append(fileLabel);

  // ---- resolution ---------------------------------------------------------
  const resLabel = el('label', 'transport__field');
  resLabel.append(el('span', 'transport__label', 'Composition'));
  const res = el('select', 'transport__select');
  for (const r of RESOLUTIONS) res.append(new Option(r.name, r.id));
  res.value = state.resolution.id;
  res.addEventListener('change', () => {
    state.resolution = RESOLUTIONS.find((r) => r.id === res.value);
    announce();
  });
  resLabel.append(res);
  bar.append(resLabel);

  // ---- backdrop -----------------------------------------------------------
  if (demo.showBackdrop) {
    const backLabel = el('label', 'transport__field');
    backLabel.append(el('span', 'transport__label', 'Behind'));
    const back = el('select', 'transport__select');
    for (const b of BACKDROPS) back.append(new Option(b.name, b.id));
    back.value = state.backdrop;
    back.title = 'What sits under the effect. The output carries real alpha, and in Resolume that would be the layers below.';
    back.addEventListener('change', () => {
      state.backdrop = back.value;
      announce();
    });
    backLabel.append(back);
    bar.append(backLabel);
  }

  file.addEventListener('change', async () => {
    if (!file.files?.length) return;
    bar.dispatchEvent(new CustomEvent('demo:adopt', { bubbles: true, detail: file.files[0] }));
  });

  return bar;
}

function buildDifferences(demo) {
  const details = el('details', 'differences');
  details.append(el('summary', null, 'What this page does not reproduce'));

  const intro = el('p', null,
    'The shader maths is the plugin\'s, copied across rather than rewritten. Everything around it is not the plugin, and these are the ways that shows:');
  details.append(intro);

  const list = el('ul');
  const common = [
    'The host is a browser, not Resolume: there is no composition, no layer stack, no clip, no automation and no Resolume UI. The parameter panel here is a stand-in for the host\'s inspector, not a picture of it.',
    'The plugin is desktop OpenGL 4.1 core; this runs GLSL ES 3.00 in WebGL2. The source is the same text, but the driver, the precision hints and the rounding are not the same, so a pixel here is not evidence about a pixel there.',
    'The clips are generated in the page. They are built to show what each control does, which is not the same as being representative footage.',
    'Nothing here is measured. Each plugin has an offline harness that checks its maths numerically, and that harness — not this page — is the reason to believe the maths.',
  ];
  for (const text of [...common, ...(demo.differences ?? [])]) {
    list.append(el('li', null, text));
  }
  details.append(list);

  return details;
}
