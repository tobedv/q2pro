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
the bottom-left quarter of the screen on a HiDPI display. `mode_changed()` now
queries the true surface size from EGL (`eglQuerySurface`) when an EGL context
is active; this is a no-op for desktop GL contexts and non-Apple platforms.
Side effect: the game renders at full native resolution on Retina displays.

## Runtime setup

The engine needs ANGLE's dylibs and two environment variables:

```sh
# libEGL.dylib + libGLESv2.dylib must be findable by dlopen
export DYLD_LIBRARY_PATH="/path/to/angle:$DYLD_LIBRARY_PATH"
# make SDL create the ES context through ANGLE
export SDL_OPENGL_ES_DRIVER=1
# IMPORTANT: without this, ANGLE picks its OpenGL backend on macOS and you
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
