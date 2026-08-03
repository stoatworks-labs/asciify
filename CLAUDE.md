# asciify

Character renderer (ASCII art) as an FFGL effect for Resolume Arena/Avenue.
C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. Public
MIT repo.

Read `AGENTS.md` before changing the matching maths or the font.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/asctest --out /tmp/frame.png`
- List parameters: `./build/asctest --list`
- Look at the font: `./build/asctest --atlas /tmp/atlas.png`
- Put real footage through the real shaders (for the project video):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/asctest --pipe --width W --height H [--script cues.txt] | ffmpeg …`

## OpenFX build
- `source/ofx/AsciifyOFX.cpp` → `build/Asciify.ofx.bundle` (target `AsciifyOFX`,
  `-DBUILD_OFX=OFF` to skip) for Resolve/Nuke/Natron/Vegas. Links Match/Font/
  Alphabet/Controls straight from source — only the three GPU passes are
  mirrored on the CPU. Change a pass's arithmetic in Shaders.cpp, change the
  matching phase there too.
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Smoke test (ofxprobe from resolume-ofx-bridge; `--out` writes input|output BMP):
  `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.asciify --size 640x360 --out /tmp/a.bmp`
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Verify
- Everything: `tools/verify.sh`
- GLSL vs C++ matching maths: `./build/asctest --match`
- The font's own invariants: `./build/asctest --font`
- The measured ramp: `./build/asctest --ramp [--set "Characters=N"]`
- No dead controls: `python3 tools/sweep.py`

## Notes
- Three passes at three resolutions: copy (picture size, mipmapped), cell (one
  pixel per character, does the matching), type (output size, draws them). A
  character is chosen for a **cell**, never for a pixel.
- The matching maths exists twice, in `Match.cpp` and in the cell shader in
  `Shaders.cpp`. Every mirrored line is marked `//= mirrored`. `--match`
  measures one against the other. Change one, change both, run it.
- Nothing hard-codes a dark-to-light ordering. Ink coverage is measured off the
  font bitmaps and the ramp is a consequence. `--ramp` prints it.
- The cell buffer **must** be sampled with `GL_NEAREST`. Its alpha channel is a
  glyph index, not a quantity.
- All host parameters are 0..1 and mapped in `Controls.cpp`. `SetParamInfo`
  clamps a standard default into 0..1 before `SetParamRange` can widen it, so a
  columns parameter cannot declare a columns default.
- `FFGLScopedFBOBinding.h` is not in `FFGLSDK.h`; include it by hand.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- `flat`, `active`, `filter`, `input`, `output`, `sample`, `common` are GLSL
  reserved words. Shader errors only surface at runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. With three passes it records which one,
and logs the GL vendor/renderer/version next to it.

    ~/Library/Logs/asciify/asciify.YYYY-MM-DD.log
