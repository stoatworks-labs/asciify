"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. `asctest --match` will not
catch it either -- it checks that the GPU and the C++ agree about the *matching*
maths, and a dead uniform makes them agree perfectly on the wrong thing.

So: render each parameter at both ends of its range against a baseline where the
effect is actually doing something, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

Three things about this plugin in particular that will fool you:

  * **A control can be alive and still measure as dead if its context is
    switched off.** The Ink colour does nothing at Tint = 1, Paper Opacity does
    nothing against a near-black paper, and none of the colour controls do
    anything at Mix = 0. CONTEXT below gives those parameters a baseline in
    which they can be seen.

  * **Custom Set is a text parameter and cannot be swept with --set.** It gets
    its own check at the bottom, through `asctest --custom`, because a control
    that the sweep structurally cannot reach is exactly the one that quietly
    stops working.

  * **Structure is front-loaded and that is honest.** Almost all of its visible
    effect is in the first quarter of the slider, because the smallest non-zero
    allowance is already enough to change every near-tie in the ramp. It passes
    this test easily; it is mentioned so that a small number here is not read as
    a fault.
"""
import subprocess, sys, tempfile, struct, zlib

SC = tempfile.mkdtemp(prefix="ascsweep")

# A baseline where the effect is genuinely typesetting something, so that
# nothing reads dead merely because the thing it modifies is switched off.
BASE = {
    "Columns": 0.5,
    "Characters": 0,       # ASCII
    "Structure": 0.35,
    "Tone": 0.5,
    "Contrast": 0.5,
    "Invert": 0,
    "Dither": 0.5,
    "Tint": 0.0,
    "Ink": 0.6, "Ink_Green": 1.0, "Ink_Blue": 0.7,
    "Paper": 0.02, "Paper_Green": 0.05, "Paper_Blue": 0.03,
    "Paper Opacity": 1.0,
    "Glyph Edge": 0,
    "Mix": 1.0,
}

# Options are discrete; sweep them across their real element range. Everything
# else is a plain 0..1 float.
DISCRETE = {
    "Characters": (0, 8),
    "Invert": (0, 1),
    "Glyph Edge": (0, 1),
}

# A few controls need a baseline of their own to be visible at all.
CONTEXT = {
    # Paper is what shows where there is no ink, so it has to be a colour that
    # is not already the black the frame is composited onto.
    "Paper":         {"Paper": 0.0, "Paper_Green": 0.0, "Paper_Blue": 0.0},
    "Paper_Green":   {"Paper": 0.0, "Paper_Blue": 0.0},
    "Paper_Blue":    {"Paper": 0.0, "Paper_Green": 0.0},
    "Paper Opacity": {"Paper": 0.9, "Paper_Green": 0.4, "Paper_Blue": 0.1},
    # Tint fades between the ink colour and the picture's, so the two have to
    # differ. The type card's bands are warm; the ink is green.
    "Tint":          {"Ink": 0.0, "Ink_Green": 1.0, "Ink_Blue": 0.0},
    # Glyph Edge changes the filtering of a magnified glyph, which needs cells
    # big enough for the filtering to have somewhere to happen.
    "Glyph Edge":    {"Columns": 0.25},
    # Dither perturbs the tone by one quantisation step, so it is worth exactly
    # as much as a step is wide. Against the 95-character ASCII set a step is a
    # three-hundredth of the range and it barely registers -- correctly. The
    # ten-character Classic ramp, with Structure off so nothing else is moving
    # the choice, is where it does the work it exists for.
    "Dither":        {"Characters": 4, "Structure": 0.0, "Columns": 0.4},
}

# Not reachable through --set. Checked separately at the bottom.
TEXT_PARAMS = {"Custom Set"}


def render(path, overrides, custom=None):
    args = ["./build/asctest", "--out", path, "--width", "960", "--height", "540"]
    merged = dict(BASE)
    merged.update(overrides)
    for k, v in merged.items():
        args += ["--set", f"{k}={v}"]
    if custom is not None:
        args += ["--custom", custom]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", r.stdout, r.stderr)
        sys.exit(1)
    return open(path, "rb").read()


def pixels(png):
    i = 8
    idat = b""
    w = h = 0
    while i < len(png):
        ln = struct.unpack(">I", png[i:i + 4])[0]
        t = png[i + 4:i + 8]
        d = png[i + 8:i + 8 + ln]
        if t == b"IHDR":
            w, h = struct.unpack(">II", d[:8])
        if t == b"IDAT":
            idat += d
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h))


def diff(a, b):
    pa, pb = pixels(a), pixels(b)
    n = len(pa)
    changed = 0
    total = 0
    for i in range(0, n, 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / (n / 4) * 100, total / (n / 4)


names = subprocess.run(["./build/asctest", "--list"], capture_output=True, text=True).stdout
params = [" ".join(l.split()[1:-1]) for l in names.strip().splitlines()]

# The About block is a text field and browser buttons, declared last. They
# never touch a pixel, so sweeping them only buries a real dead control.
if "About" in params:
    params = params[:params.index("About")]

print(f"{'parameter':<16} {'pixels changed':>15} {'mean delta':>11}   verdict")
dead = []
checked = 0
for p in params:
    if p in TEXT_PARAMS:
        continue
    checked += 1
    lo, hi = DISCRETE.get(p, (0.0, 1.0))
    context = CONTEXT.get(p, {})
    a = render(f"{SC}/a.png", {**context, p: lo})
    b = render(f"{SC}/b.png", {**context, p: hi})
    pct, mean = diff(a, b)
    ok = pct > 0.5
    if not ok:
        dead.append(p)
    print(f"{p:<16} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}")

# The text parameter, which --set cannot reach. Two alphabets that could not
# possibly render the same picture: five characters of ramp against one solid
# block, in Custom mode so that the string is actually read.
checked += 1
a = render(f"{SC}/a.png", {"Characters": 8}, custom=" .oO@")
b = render(f"{SC}/b.png", {"Characters": 8}, custom=" |-/\\")
pct, mean = diff(a, b)
ok = pct > 0.5
if not ok:
    dead.append("Custom Set")
print(f"{'Custom Set':<16} {pct:14.2f}% {mean:11.3f}   {'ok' if ok else '*** NO EFFECT ***'}")

print()
if dead:
    print("DEAD CONTROLS:", ", ".join(dead))
    sys.exit(1)
print(f"all {checked} parameters affect the output")
