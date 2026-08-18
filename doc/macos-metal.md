# Metal rendering on macOS via ANGLE (OpenGL ES 3.0)

Status: working prototype, tested on Apple M3 Pro / macOS 26.5 against the
Steam AQtion content. Verified in-game: correct output, native-resolution
Retina rendering, smoother frame pacing than the Apple OpenGL stack.

## Why

Apple deprecated OpenGL; on Apple Silicon it runs through a legacy
compatibility layer (`GLEngine` / `AppleMetalOpenGLRenderer`) with no further
optimization, and its presentation path goes through WindowServer compositing
with queued frames (higher input latency, no direct scanout).

Q2PRO already has a complete OpenGL ES 3.0 rendering path (`gl_profile es3.0`)
and SDL2's macOS backend can create ES contexts through
[ANGLE](https://github.com/google/angle), whose Metal backend is
production-grade (it is what Chrome uses for WebGL on macOS). That combination
gives us every draw call executing natively in Metal with **no renderer
rewrite** — only the small engine fix in this branch plus runtime setup.

## What this branch changes

`src/unix/video/sdl.c`: SDL's Cocoa EGL path reports the window's *logical*
size, but ANGLE backs its EGL surface with a `CAMetalLayer` at *native
(Retina) pixel* scale. Without correction the engine's viewport covers only
the bottom-left quarter of the screen on a HiDPI display. Changes (all
no-ops for desktop GL contexts and non-Apple platforms):

- `mode_changed()` queries the true surface size from EGL
  (`eglQuerySurface`) when an EGL context is active. Side effect: the game
  renders at full native resolution on Retina displays.
- `mode_changed()` now runs on `SDL_WINDOWEVENT_SIZE_CHANGED` (RESIZED is
  the external-only subset of it) and on `SDL_WINDOWEVENT_DISPLAY_CHANGED`,
  so moving the window to a display with a different backing scale —
  which changes the drawable size without a window resize — re-syncs.
- ANGLE applies layer resizes at *swap* time, so an event-time query can
  still see the previous size; after every swap the EGL surface size is
  re-checked and the engine re-syncs if it drifted. This also covers
  fullscreen transitions, where `set_mode()` queries before any swap has
  resized the surface.
- EGL symbol resolution retries until libEGL is actually loaded, so a
  desktop-GL fallback boot followed by `vid_restart` into es3.0 still gets
  the fix.
- **Self-configuration**: when `libEGL.dylib` + `libGLESv2.dylib` sit next
  to the executable, the engine preloads them by absolute path (so SDL's
  and qal.c's leaf-name `dlopen` resolve without `DYLD_LIBRARY_PATH`),
  sets `SDL_OPENGL_ES_DRIVER=1` and `ANGLE_DEFAULT_PLATFORM=metal`
  (no-overwrite), and defaults `gl_profile` to `es3.0`. The user
  environment and `+set` always win; without the dylibs nothing changes.
- **Tripwire**: on macOS an ES context whose `GL_RENDERER` doesn't contain
  `ANGLE Metal Renderer` logs a warning — the guard against silently
  landing back on a translation stack.

## Runtime setup

Drop `libEGL.dylib` + `libGLESv2.dylib` next to the `q2pro` binary and run
it — the engine self-configures (see above). Escape hatches:

- `+set gl_profile ""` (or any desktop profile) forces Apple's GL stack.
- Delete/rename the dylibs: the engine boots desktop GL as if nothing
  happened.
- Exporting any of the env vars yourself overrides the bootstrap's values.

Verified boot drills (all with a scrubbed environment): dylibs present →
`ANGLE Metal Renderer` + OpenAL, no `DYLD_LIBRARY_PATH` needed; dylibs
absent → clean desktop GL boot; `+set gl_profile ""` → desktop GL wins
over the default.

For binaries without the bootstrap (upstream builds, older prototypes) the
manual environment still works:

```sh
export DYLD_LIBRARY_PATH="/path/to/angle:$DYLD_LIBRARY_PATH"
export SDL_OPENGL_ES_DRIVER=1
# without this a GL-backend-enabled ANGLE picks OpenGL on macOS and you
# get ES -> Apple GL -> Metal double translation (~10x slower, see below)
export ANGLE_DEFAULT_PLATFORM=metal
./q2pro +set gl_profile es3.0
```

Sanity checks that you are really on Metal:

- The console prints `Legacy rendering backend not available. / Using GLSL
  rendering backend.` (an ES context has no legacy fixed-function path).
- `lsof -p <pid>` shows `libGLESv2.dylib` and `AGXMetal*` loaded.
- If ANGLE reports its renderer string, it must contain `ANGLE Metal
  Renderer`, not `OpenGL 4.1 Metal` (the latter means the GL backend wrapped
  Apple's stack — set `ANGLE_DEFAULT_PLATFORM=metal`).

For prototyping, the ANGLE dylibs shipped inside Google Chrome work
(`Google Chrome.app/Contents/Frameworks/Google Chrome
Framework.framework/Versions/<ver>/Libraries/`); they are universal
x86_64+arm64 binaries. For distribution, build ANGLE from source (BSD
license) rather than redistributing Chrome's copies.

## Build

Nothing special — standard meson build. ES 3.0 supports `GL_UNSIGNED_INT`
indices natively, so the `opengl-es1`/`USE_GLES` option is *not* required.

```sh
meson setup build   # buildtype=release is the project default
ninja -C build
```

## Building ANGLE locally

Chrome's bundled dylibs are fine for prototyping; a local build removes the
Chrome dependency and, built Metal-only, makes the double-translation trap
impossible (there is no GL backend to fall into). First fetch downloads
depot_tools plus a large Chromium toolchain (~15–20 GB); the build itself is
minutes on Apple silicon.

```sh
git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$PWD/depot_tools:$PATH"
mkdir angle && cd angle && fetch angle    # long; resumable with gclient sync

gn gen out/Metal --args='is_debug=false is_component_build=false
  angle_enable_metal=true angle_enable_gl=false angle_enable_vulkan=false
  angle_enable_swiftshader=false angle_build_tests=false'
autoninja -C out/Metal libEGL libGLESv2

# install: copy out/Metal/{libEGL,libGLESv2}.dylib next to q2pro,
# plus LICENSE (BSD) as ANGLE-LICENSE. Note the pinned commit (git rev-parse
# HEAD) so the build is reproducible; update via gclient sync, not blindly.
```

Verify in-game after swapping dylibs: `GL_RENDERER` must still say
`ANGLE Metal Renderer` (the init log and the tripwire both check this).

Optional latency experiment for a local build: in
`src/libANGLE/renderer/metal/SurfaceMtl.mm`, where the `CAMetalLayer` is
configured, set `maximumDrawableCount = 2` (default 3) — bounds the present
queue at ~1 frame like `gl_finish 1` does, but on the layer itself. Keep it
env-gated if patching, and treat it as an experiment, not the default.

## Measured results (M3 Pro, bwcity2, `timerefresh`, 1280x720 windowed)

| Path                                             | fps    |
| ------------------------------------------------ | ------ |
| ANGLE default backend (ES→Apple GL→Metal, broken) | ~95    |
| ANGLE Metal backend                               | ~480   |
| Apple OpenGL (current release path)               | ~1100  |

Interpretation (from CPU sampling, not guesswork):

- On the broken default path, ~10 ms/frame is lost inside Apple GLEngine's
  `glBufferSubData`, which submits a Metal command buffer per buffer update.
  **`ANGLE_DEFAULT_PLATFORM=metal` is mandatory.**
- On the Metal backend, actual render+submit work is ~0.15 ms/frame; 92% of
  the frame is spent blocked in `CAMetalLayer nextDrawable`. The ~480 fps
  windowed ceiling is drawable turnaround through the compositor, i.e. flow
  control — not rendering cost.
- Apple GL's higher uncapped number comes from having almost no backpressure:
  swaps enqueue and return, and frames beyond the refresh rate are discarded.
  Metal's regulated pipeline holds fewer queued frames, which is why it feels
  better: lower input latency and more consistent pacing, especially with
  `gl_swapinterval 1` on high-refresh displays. Fullscreen with direct
  scanout the uncapped ceiling is higher than the windowed number.

## Verifying smoothness / latency knobs

The end goal is pacing and input latency, not uncapped fps. Tools and knobs,
in order of usefulness:

- `MTL_HUD_ENABLED=1` (or `launch-metal.sh hud`) overlays Apple's Metal
  performance HUD: present mode (**Direct** scanout vs **Composited**),
  actual frame-interval graph, GPU time. This is the definitive instrument.
  Windowed is always composited; fullscreen on a display the layer covers
  should read Direct — if it does not, latency is being left on the table.
- `gl_finish 1` (console, live): `R_BeginFrame` then calls `glFinish`,
  bounding CPU run-ahead to ~1 frame. With ~0.15 ms GPU frames the
  throughput cost is nil, and it cuts worst-case present-queue latency:
  the `CAMetalLayer` holds the default 3 drawables, i.e. up to ~2 queued
  frames ≈ 8 ms at 240 Hz. Good A/B knob for input feel.
- `maximumDrawableCount = 2` would be the cleaner fix for the same thing,
  but ANGLE exposes no knob for it (verified: no setter call in the
  shipped dylib). One-line patch in `WindowSurfaceMtl` once we build ANGLE
  from source in CI.
- The shipped Chrome dylibs honor `ANGLE_FEATURE_OVERRIDES_ENABLED` /
  `ANGLE_FEATURE_OVERRIDES_DISABLED` env vars; defaults are Chrome-tuned
  for Apple silicon and profiling shows nothing worth overriding.
- When judging feel by eye, disable log flushing and debug spam
  (`launch-metal.sh quiet`): per-line synchronous log writes during
  firefights are tiny but nonzero jitter.

## Caveats / open items

- ANGLE's per-draw translation tax makes raw uncapped throughput ~2x lower
  than Apple GL. Irrelevant below the display refresh rate, but it is not the
  path to synthetic 1000+ fps numbers.
- `eglSwapInterval(0)` is honored by ANGLE Metal on macOS
  (`displaySyncEnabled = NO`); windowed frame rate is still bounded by
  compositor drawable turnaround.
- Productionizing means: build ANGLE in CI, ship the two dylibs next to the
  binary, export the two env vars in the launcher, default
  `gl_profile es3.0` on macOS with automatic fallback to desktop GL if
  context creation fails (the failsafe path already handles this).
