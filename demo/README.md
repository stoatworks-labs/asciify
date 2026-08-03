# demo/ — the browser demo

Live at **https://asciify-demo.stoatworks-labs.com**, linked from the
[project page](https://stoatworks-labs.com/software/asciify/) and from the
[Resolume suite page](https://stoatworks-labs.com/resolume/).

**This is not the plugin.** It is the GLSL from [`source/Shaders.cpp`](../source/Shaders.cpp),
copied across unedited and run in WebGL2 over clips generated in the page, with
the parameters the plugin's constructor declares. The page says so in a banner,
and lists what it does not reproduce at the foot.

## Editing it

- `plugin.js` — this plugin's parameters and its shaders. **When the shader in
  `source/Shaders.cpp` changes, change it here too.** The two copies exist because the demo
  cannot include a C++ file; nothing enforces that they agree.
- `vendor/` — the shared kit, vendored from `stoatworks-backend/resolume-demo/`.
  **Do not edit these.** Fix the master and re-run `./sync.sh`; `./sync.sh
  --check` reports drift.

## Deploying

No build step. From the repo root:

```bash
cf-run npx wrangler deploy
```

Then verify by content rather than by status code — a wrong page still answers
200:

```bash
curl -s 'https://asciify-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```
