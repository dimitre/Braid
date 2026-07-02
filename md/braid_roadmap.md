# Braid — Roadmap

**Updated:** 2026-07-02
**Now:** v0.1.0 → **v0.2.3 core implemented and verified** (macOS/arm64). Surface is the
one primitive; algebra + self-feedback work; the ouroboros tunnel was captured to a file.
**Spirit:** a *fast* creative-coding microframework. Every milestone is a thing you can
**see/run**, not a pile of plumbing. If a step doesn't end in a demo, it's too big.

---

## ⏭ Immediate (review 2026-06-30)
Fresh read of `README.md`, `core/braid.h`, `core/braid.cpp`, `core/braid_image.cpp`,
`chalet.yaml`, and the examples. These are the items that surfaced as live concerns,
ordered by the user's stated priority.

1. **Multi-window support — implemented; future: output-window simplification.**
   The original blocker was that `App` fused library-init + device + window + loop.
   That is now split into `Application` (shared device + global loop) and `Window`
   (per-window RGFW window + swapchain + surfaces + hooks), with `App` kept as the
   backward-compatible primary window. Historical design details live in
   `md/braid_multiwindow.md`; implementation blow-by-blow lives in
   `md/braid_multiwindow_plan.md`. This roadmap item tracks what is decided and what
   comes next.

   **Done.**
   - ✅ `Application`/`Window`/`App` split, one shared `wgpu::Device`, global loop with
     one `RGFW_pollEvents()` per frame and per-window event dispatch. `App` and every
     existing example continue to compile unchanged.
   - ✅ `examples/multiwindow.cpp`: primary preview window + secondary output window,
     independent rendering, clean per-window close, last-window-exits behavior.
   - ✅ Multi-monitor spanning: `braid::Monitors::list()/unionOf()`,
     `AppSettings::position` and `AppSettings::monitors`. A secondary can span monitors
     `{1, 2}` as one borderless native-resolution canvas.
   - ✅ Review fixes landed: primary close no longer kills secondaries; primary surfaces
     released before device teardown; secondaries default onto the next monitor instead
     of stacking on the primary; vendored RGFW.h `RGFW_window_raise` always-on-top bug
     fixed.

   **Design decisions (locked).**
   - **One shared `wgpu::Device`.** WebGPU has no per-window context; the device *is*
     the shared context. This is not changing.
   - **One global frame pace / target FPS for the whole app.** `Application` owns one
     `Timer`; only the primary window's `AppSettings::targetFps` is read. Every window
     advances the same simulation step and draws on the same cadence. Per-window
     differences are handled by **vsync** (`AppSettings::vsync` / swapchain
     `PresentMode`) and by frame-skipping inside an individual window's `draw()`, not
     by independent timers. Rationale: a show has one clock; output windows show the
     same content, just at different sizes; independent FPS would desync the shared
     scene or force redundant frame duplication.
   - **Addons are window-scoped, not app-scoped.** A UI addon registers on the control
     window it belongs to; output windows stay addon-free. This keeps the architecture
     honest (addons need a surface + mouse coordinates) while still allowing the user's
     “one brain, many dumb displays” usage pattern.

   **Future changes / follow-up.**
   - [ ] **Dumb output window / `DisplayWindow` abstraction.** For install rigs the
     common pattern is one interactive control window + N non-interactive output
     windows that just composite a shared `Surface`. Today every secondary must
     subclass `Window`/`SketchApp` and override `draw()`, which is ceremony for a
     display-only role. Add a lightweight output-window type — either a base class
     or an `App::createDisplay(settings, surface)` helper — that owns no UI hooks,
     no event channels, and simply shows a given `Surface` each frame. Keep `Window`
     as the foundation; this is a convenience layer for the "projection / LED"
     secondaries. Important but **not in this session** — note only.
   - [ ] **Physical multi-monitor spanning test.** Verified on a single-monitor machine
     only; real spanning across two physical side-by-side displays has never been
     tested. When the two-output rig is connected: confirm the startup monitor log
     maps indices `{1, 2}` to the expected physical screens (enumeration order isn't
     stable across reconnects — adjust `examples/multiwindow.cpp` if needed), confirm
     the output window is genuinely borderless and seamless across the seam, and check
     mixed HiDPI (one `contentsScale` per window is a macOS constraint; matched-DPI
     displays work, mixed-DPI spanning does not — fix is one window per monitor, see
     item 2 below).
   - [ ] **De-singleton `detail::Context` and `detail::Compositor`.** For multi-device
     support and deterministic teardown / unit testing, move the globals onto the
     `Application`/`DeviceContext` and pass them explicitly. Not required for the
     one-device/N-windows case, which is the current scope. (Roadmap #5.)
   - [ ] **HiDPI / `contentsScale`.** Set `CAMetalLayer.contentsScale` from the monitor's
     reported pixel ratio and configure surfaces at physical pixel size; keep mouse
     coordinates in logical points. See item 2 below.

2. **HiDPI / Retina rendering — designed and settled; implementation spec: `md/hidpi.md`.**
   Root cause (verified): `attachMetalLayer()` leaves `CAMetalLayer.contentsScale` at 1.0
   and `configureSurface()` configures the swapchain at logical point size
   (`braid_app.cpp:47,535`) — Retina renders at 1× and AppKit upscales, hence the blur.
   The full design — unit law, flag semantics, RGFW facts (incl. the `RGFW_scaleUpdated`
   payload trap), step-by-step plan, TinyUI follow-up, verification protocol, fortress
   rules — lives in **`md/hidpi.md`** (settled 2026-07-02; supersedes all earlier
   discussion here). Decisions in brief:
   - **Logical points in the sketch-facing API; pixels in `Surface`; `Window` is the only
     translator** (`× pixelRatio` in exactly two places: `braid_app.cpp` surface
     config/creation and SketchApp's projection). Existing sketches run unchanged, just
     sharper; mouse space == draw space; exports come out at native pixels (a 1280×720
     window on Retina saves a crisp 2560×1440 PNG).
   - **`AppSettings::hidpi = true` by default**, per-window opt-out as a *performance*
     hatch (2× ratio = 4× fragment work; feedback/blur already GPU-bound at 1×).
     Opt-out shape: swapchain stays physical; only `mainSurface_` shrinks; the existing
     size-absorbing `endFrame()` blit upscales inside Braid, never Core Animation.
   - **pixelRatio exists only at the Window boundary** — `Surface(device, w, h)` stays
     exact pixels (the LED-wall path); user canvases never see any window's scale.
   - **Sampling stays Linear globally** — Nearest was tried live (2026-07-01) and
     rejected: `feedback()`'s iterated fractional resampling degrades badly. The
     commented-out lines remain at `braid_compositor.cpp:309-317`; chunky pixels for
     LED/pixel-art arrive later as a per-`Surface` `Filter::Nearest` opt-in.
   - **Mixed-DPI spanning has no layer-level fix** (one `contentsScale` per window is an
     OS constraint) — the answer to item 1's TODO is **one borderless window per
     physical monitor**, already supported by the `Application`/`Window` split.
3. **Break up the single big translation unit — ✅ done 06-30.**
   `core/braid.cpp` (~1,900 lines) is gone; the implementation is now focused TUs behind
   the single public header `core/braid.h`:
   - `braid_timer.cpp` (Timer — pure CPU, headless-testable)
   - `braid_compositor.{h,cpp}` (internal blit/blur/threshold/quad engine + the two
     process-wide singletons `detail::Context` and `detail::Compositor`, co-located so
     the future de-singleton has one home; new internal header shares the uniform structs)
   - `braid_surface.cpp` (Surface algebra, transforms, feedback, paste)
   - `braid_mesh.cpp` (Mesh generators + buffers)
   - `braid_shader.cpp` (WGSL compile + pipeline cache + uniform ring)
   - `braid_app.cpp` (window + device + event pump + run loop — the **only** TU that
     compiles `RGFW_IMPLEMENTATION` + Cocoa/Metal glue)
   - `braid_sketch.cpp` (SketchApp state + batching + default shader)
   - Blend presets live in `braid_compositor.cpp`; addons unchanged (`braid_image.cpp`
     and syntype reach core only via `detail::ctx()` in `braid_detail.h`).
   - Verified: `chalet build` of braid-core + all 11 executables (incl. both addons)
     succeeds — no duplicate symbols, no undefined refs; `cubes` runs clean (no
     validation errors). No public API change. (`braid_result.cpp`/`braid_channel.cpp`
     were unneeded — Result/Channel are header-only templates.)

4. **Correct the README/marketing about "single-file-ish."**
   The README says "small, single-file-ish." This is not a virtue, and it is not
   accurate (`core/braid.cpp` is ~1,900 lines, plus `braid_image.cpp`, plus headers).
   Replace with honest framing: **single public header (`#include "braid.h"`) with a
   modular implementation.** The header is the contract; the implementation can be
   many files. Don't celebrate monolithic source.

5. **Audit and reduce global state.**
   Related to multi-window: `detail::g_ctx` and `detail::g_compositor` are global.
   Even for single-window, this makes unit testing and deterministic cleanup harder.
   Move toward explicit ownership: `App` (or a new `DeviceContext`) holds the
   `Context`, and Surfaces borrow it on construction. This enables both multi-window
   and a future non-GPU test harness.

6. **Add non-GPU tests before the refactor grows.**
   The public API has testable seams: `Result<T>`, `Channel<T>`, `Timer`, `remap()`,
   `Mesh` CPU generators. Add a small test target (even a single `tests/braid_test.cpp`)
   that exercises these without touching Dawn/RGFW. Run it in CI. This pays off
   especially once the TU split lands.

7. **Shader hot-reload / asset watcher (creative-loop priority).**
   For a creative-coding tool, iteration speed dominates. Add a minimal file watcher
   that reloads `.wgsl` files into `Shader` objects and images into `Surface::load()`
   on disk change. This is higher user value than compute/text/audio for now.

8. **User-injectable custom shader passes (found 2026-07-01, reviewing `playground.cpp`).**
   Every `Surface` effect (`blit`/`quad`/`blur`/`threshold`) is a WGSL string hardcoded
   into `Compositor`, each with its own uniform struct, pipeline cache, and pass function
   (`core/braid_compositor.cpp`). There is **no hook for a sketch's own fragment shader** —
   today the only extension point is composing existing `Surface` methods inside
   `feedback()`. A user with their own effect has to edit core to add it. **Still open** —
   `Surface::contour()` below was landed the lightweight way (hardcoded, like blur/threshold)
   specifically *because* this hook doesn't exist yet; it's now the reference case to
   generalize from whenever the generic pass gets built.
   - Fix: a generic pass, shaped like `thresholdPass`/`blurPass` but taking user WGSL —
     fixed fullscreen-triangle vertex stage (reuse the blit's), user-supplied fragment
     source, a small uniform buffer the sketch fills. Something like
     `Surface::pass(Shader& customFrag, const void* uniforms, size_t size)`. Braid owns
     pipeline/bind-group plumbing; the sketch owns the WGSL.
   - Proof: a sketch supplies raw WGSL with no core edit and it runs through the
     compositor.
   - ✅ **`Surface::contour()` ported 2026-07-01** — the motivating shader
     (`contour2.frag`, `~/Dmtr/ofworks/apps/apps2025/_shaders/2d/contour2.*`) landed as a
     real core method rather than waiting on the hook above: samples a neighbor pattern
     (`mode` 0-3: 8-neighbor +/X, + only, X only, 2-point) offset by `radius` px, counts how
     many neighbors fall below a `level` luma threshold, and draws white where that count
     crosses the pattern's cutoff — a binary isoline at the level set, genuinely different
     from `threshold()`'s flat brightpass (which only ever feeds `bloom()`'s glow layer).
     Same shape as `blur`/`threshold`: WGSL in `braid_compositor.cpp`
     (`kContourWGSL`/`ContourUniforms`/`contourPass`/`contourPipelineFor`), self-transform
     in `braid_surface.cpp`, declared in `braid.h`. New permanent example:
     `examples/contour.cpp` (1-4 = pattern, Up/Down = level, `[`/`]` = radius), wired into
     `chalet.yaml` and `.zed/tasks.json`. Verified: builds clean across all 13 targets;
     ran ~3s with no Dawn validation errors (Dawn is loud about those — silence is
     meaningful) and a window registered with the OS; user visually confirmed it renders
     correctly (this session's screencapture is blank — terminal-only capture, a known
     limitation of this environment, not the app). If/when the generic hook above lands,
     this is the first candidate to move out to a user-supplied shader instead of core.

---

## ⏭ Immediate (synthesized 2026-06-29)
Distilled from a cross-read of every `.md` (review, unmapped-todos, dependency-concerns,
geometry). The ordering is by **convergence** (where independent sessions agree) and
**urgency** (which item gets more expensive every day). Several older docs predate the
Surface/algebra/feedback/image work, which is now done — these are what's left and live.

1. **`braid::Key` enum + keycode mapping at the pump** — *do first.* Cited by three docs
   (dependency-concerns makes it the #1 RGFW mitigation; v0.1.1; unmapped #8 builds on it).
   The only item whose cost **grows every session**: `KeyEvent.key` exposed the raw RGFW
   code, and new sketches (`feed`, `feedback`, `image`) already compare raw chars. Own the
   joint before more sketches weld to RGFW.  *(in progress)*
2. **SketchApp batching** — ✅ done 06-29. Primitives now defer: each appends geometry +
   a `DrawCmd`; at flush the frame uploads in **two writes** (one growing vertex buffer +
   one 256-aligned uniform pool addressed by **dynamic offset**, single bind group) and
   replays as draws, switching pipeline only on topology change. A frame of N shapes
   allocates **nothing** in steady state (was: a buffer + bind group *per primitive*).
   Needed a dedicated SketchApp pipeline (explicit layout w/ `hasDynamicOffset`, since
   `Shader::getPipeline` uses auto-layout). `cubes.cpp` (~400 boxes/frame) runs clean.
3. **Depth + always-lit default shader → solid `box`/`sphere`** — finishes v0.2.3's "sketch
   tier complete". Two docs agree: `Surface` needs an optional depth attachment (correctness
   blocker for filled 3D), then the default shader goes always-lit (geometry-roadmap option
   3: one directional light + ambient; 2D shapes get `normal=(0,0,1)`), which unlocks solids.

**Sweep while in the code (small, all cited):** wire `Settings::enableValidation` into device
creation · fix `Channel` reentrancy (callbacks fire under `cbMtx_` during `pop()` → snapshot
handlers, fire outside the lock) · clear the `braid.cpp` Metal `[VERIFY]` tag.
**Foundational before the batching refactor (optional but cheap):** a non-GPU test harness
(`Result`, `Channel`, `Timer`, `Mesh` generators, `remap`).

---

### Implementation status (2026-06-30)
- ✅ **v0.2.0 Surface pivot** — `Screen`→`Surface`; App draws into a persistent offscreen
  `Surface`, blitted to the swapchain at present. (`save()` later moved to braid-image, v0.3.0b.)
- ✅ **v0.2.1 Algebra** — `+=`, `over`, `zoom/rotate/shift/invert/multiply/clear`; total
  semantics (empty operand = identity; size/format absorbed by the blit).
- ✅ **v0.2.2 Self-feedback** — `surface.feedback(gain, transform)`, ping-pong hidden via
  an internal scratch+swap; accumulate (no `background()`) vs replace; `examples/feedback.cpp`
  verified (see scratchpad `feedback.png`).
- 🟡 **v0.2.3 Sketch tier** — 2D path + **3D wireframe `box()`** + perspective/camera, all
  through Surface. **Microframework port done** = `examples/cubes.cpp` (nested rotating
  wireframe lattice). Fixed: per-primitive uniform collision (hundreds of draws were sharing
  the 3-slot ring → now a fresh uniform buffer per primitive).
  Batching done 06-29 (deferred draws → shared vertex buffer + dynamic-offset uniform pool).
  ✅ **Real stroke outlines** — done 06-30. `rect`/`triangle`/`quad`/`ellipse`/`circle` now
  stroke their edges (strokeWeight-thick quads via a new `strokeOutline()` helper, same
  technique `line()` used for one segment); `line()`/`point()` refactored onto the same
  path. Verified via `feedback.cpp` smoke test → `stroke_test.png` (filled+stroked rect,
  stroked circle, `noFill()` stroked triangle/quad all render correctly).
  **remaining:** `image()` placed-quad, solid `box`/`sphere` fill (needs depth).
- Examples: `hello`, `sketch`, `feedback`, `playground`, `cubes`, `image`, `bloom` — all
  build+run, all in `.zed`. Cmd+Shift+R runs playground; Cmd+Q/Esc quit; fps-in-title helper.

- ✅ **v0.3.0a Image load** — `Surface::load(path)` (mango SIMD decode → `WriteTexture`,
  single-copy). The texture object *is* a Surface, so loaded images compose with the whole
  algebra. `examples/image.cpp` verified (load→composite→readback round-trip pixel-identical).
- ✅ **v0.3.0b braid-image split** — `load`/`save` moved out of braid-core into the
  `braid-image` addon (`braid_image.cpp`), the first proof of the core-vs-addon architecture.
  **TGA removed from core**: `save()` now reads back + encodes by file extension (`.png`/`.jpg`/…)
  via mango's encoder; verified emitting a real 900×900 8-bit RGBA PNG. braid-core is now
  mango-free AND tga-free — the mango stack (mango + deflate/z/lcms2/zstd/bz2/lz4) links only
  where image I/O is used (`image`, `feedback`). Measured: pure-core sketches (cubes/playground/
  hello) ≈10.4 MB, 0 mango symbols; addon-linked (image/feedback) ≈14.25 MB, 1939 mango symbols
  — the ~3.8 MB codec stack is no longer paid for by sketches that don't load/save.
  `load`/`save` are *declared* in `braid.h` but *defined* in the addon (link-to-enable).
  Shared seam: `braid_detail.h` exposes `detail::ctx()` to addon TUs.
  **remaining:** sRGB color policy, `loadAsync`, `image()` placed-quad — see `braid_v0.3_textures.md`.
- ✅ **v0.3.0c Bloom** — done 06-30 (commit `4320619`). `Surface::blur(radius)` (separable
  Gaussian, H+V in one submit), `Surface::threshold(level, knee)` (HDR-honest brightpass —
  16F means "bright" is literally >1.0), and `Surface::bloom(threshold, intensity, passes)`
  as sugar over `s += blur(threshold(clone(s)))`. `examples/bloom.cpp` verified. Per
  `braid_v0.3_bloom.md`: the dual-filter/pyramid downsample (phase 0.3.0c in that doc) was
  *not* taken — current `blur` is a direct separable pass, so very wide radii are not yet
  cheap; fine for the radii bloom actually uses, worth revisiting if a sketch wants `blur(64)`+.

---

## North stars (the why, so the tasks don't drift)
1. **Surface is the one primitive.** You can only draw via a Surface; the screen is
   just the Surface you show. (Collapses Screen / image / video-out / feedback.)
2. **Two tiers, one vocabulary.** Fallible things (load, compile) return `Result<T>`.
   Expressive things (Surface algebra) are **total** — never error.
3. **Model the artist's mind, not the GPU's hazard.** Expose the ouroboros, hide the
   ping-pong. One knob (gain), not two buffers.
4. **Beautiful enough to just exist.** `surface += surface.transformed()` should
   compile and do the obvious thing. Define the algebra so guards are unnecessary.

---

## Architecture — core vs addons (what's the heart, what plugs in)

**The test:** *does the core need it, or does it need the core?*
Surface algebra depends on the GPU device — no WebGPU, no Surface, no `+=`, no feedback.
But Surface does not depend on audio, images, video, or MIDI: an image *becomes* a
Surface, audio *modulates* one, video is *Surfaces over time* — they reach toward Surface,
Surface never reaches back. So:

> **Anything that depends on the Surface but that the Surface doesn't depend on is an
> addon.** WebGPU fails that test (the core depends on it) → WebGPU is the heart, not an addon.

**Three layers, not a flat bag of plugins:**

| Layer | What | Status |
|---|---|---|
| **platform** | RGFW + Dawn (window + device, Metal surface) | the *floor* — always present, kept behind seams (swappable joint), never opted out |
| **core** | Surface + algebra + feedback + Compositor + the loop | the *heart* — this **is** Braid's identity; WebGPU is what it's made of |
| **addons** | image · audio · video · MIDI · text/fonts | *peripherals* — plug into the core, optional by **dependency weight** |

**Principles:**
1. **The core is *exactly* Surface + algebra + the loop. Everything else is an addon.**
   (Not "everything is an addon" — that hollows out the identity, the oF mistake. Strength
   is a small strong core + optional peripherals; the discipline is what you *refuse* to add.)
2. **Modularize by dependency weight, not feature category.** image earns a compiled
   module (mango + 7 codec libs) → `braid-image`, optional by simply not linking it.
   3D primitives have *zero* deps → a header in core, never a module. Don't ceremony the
   cheap stuff.
3. **WebGPU is already the abstraction — don't wrap it.** The only realistic "swaps" are
   other WebGPU impls (wgpu-native, browser under Emscripten), same API shape. A GPU-API
   layer over WebGPU is an abstraction over an abstraction: large tax, tiny win. Keep the
   *platform* specifics behind seams (window lib, Dawn-vs-wgpu, Metal glue) — that's where
   backend flexibility actually lives, and it's cheap.

**Module boundaries:** `braid-core` (no mango, no tga) ✅ · `braid-image` (mango
load/save) ✅ done v0.3.0b · later `braid-audio`, `braid-video`, `braid-midi`, `braid-text`
— each a chalet target you link only when you need it. braid-image is the template the rest
copy: declare the public entry points in `braid.h`, define them in the addon TU, reach core
internals through `braid_detail.h`, list the heavy archives only on the consuming target.

**Bumper sticker:** *Surface is the heart; everything that flows into or out of a Surface
is an addon; WebGPU is what the heart is made of.*

---

## ✅ v0.1.0 — Bootstrap (DONE)
- [x] RGFW + Dawn (WebGPU) on macOS; window, surface, device
- [x] `Screen`, `Shader`, `Mesh`, `App` loop, `Timer` (ofTimerFps), `Channel`, `Result<T>`
- [x] `hello.cpp` — red triangle
- [x] chalet build wired to the ofWorks/ofxDawn libs
- **Proof:** `presented 120 frames over 2.00s (60 fps)`, zero validation errors.

---

## v0.1.1 — Tidy-up (small, not blocking)
Window already shows and runs via `chalet run` — visibility is **not** a problem.
(The earlier black screenshot was just the fullscreen-terminal Space, not the app.)
So this milestone is minor polish, done whenever.
- [ ] 2–3 tiny examples (clear color, spinning mesh, mouse-follow)
- [ ] Resolve RGFW drag-and-drop crash (drop the flag cleanly, or patch/report upstream)
- [x] **`braid::Key` enum + keycode mapping at the pump** — done 06-29. `KeyEvent.key` is now
      `braid::Key` (Braid-owned; printables == ASCII, control keys 256+) mapped from RGFW in
      `mapKey()` at the pump; added `KeyEvent.ch` (printable char) for `if (e.ch=='s')` ergonomics.
      No sketch sees an RGFW code → windowing stays a swappable joint. Examples migrated.
- [ ] **Harden the Metal surface** — clear the `braid.cpp:909 [VERIFY]` tag once confirmed
      across resize + HiDPI; it's the platform code with the least margin *(review 06-29)*
- [ ] **HiDPI / content-scale** — moved to the top of Immediate (2026-06-30), see item 2
      there for the confirmed root cause and fix.
- [ ] **Channel reentrancy** — callbacks fire under `cbMtx_` during `pop()`; a callback that
      (un)subscribes would deadlock. Snapshot handlers then fire outside the lock, or
      document the constraint *(review 06-29)*
- [ ] *(parked, trivial)* `.app` bundle = a `chalet distribution` section, like ofxDawn —
      nothing to design, do it when packaging matters
- **Proof:** a couple of runnable examples beyond the triangle.

---

## v0.2.0 — The Surface pivot (the identity work)
Write `braid_v0.2_surface.md` first (spec → critical-fixes → impl, the cycle that worked).
- [ ] Rename `Screen` → **`Surface`** (keep `braid::Surface`; `wgpu::` stays qualified)
  - 💬 **Opinion (review 06-29):** I'd have kept `Screen`. `braid::Surface` collides head-on
    with `wgpu::Surface` — the constructor literally takes a `wgpu::Surface` to build a
    `braid::Surface` (braid.h:257), and the two appear all over the same `.cpp`. `Screen`
    was unambiguous *and* matched the thesis ("the screen is just the Surface you show" reads
    fine with `Screen`). This is a real cognitive tax for everyone reading the code, including
    the LLM co-developer. **Not blocking, and I respect it's decided** — the chosen mitigation
    (always-qualify `wgpu::`) holds *if* enforced consistently; consider a clang-tidy/lint rule
    so it can't drift. If a rename ever feels cheap again, `Screen` is the lower-friction name.
- [ ] **All drawing routes through a Surface.** No bypass. `present()` → `show(surface)`;
      swapchain demoted to "the Surface you show"
- [ ] Surface duality: draw-into (`begin/end`) **and** sample-from (`asTexture`),
      usage `RenderAttachment | TextureBinding | CopySrc`; lazy allocation
- [x] `surface.save("frame.png")` via mango — done v0.3.0b (encode by file extension)
- **Proof:** draw scene → a Surface → both shown on screen *and* saved to PNG, same path.

---

## v0.2.1 — Surface algebra (the dream, made native)
- [ ] Total operators: `+=` (additive blend); consider `*=`, `-=` (all **total**)
- [ ] Total semantics — no guards, only definitions:
      uninitialized = additive identity · size mismatch = composite-at-origin/clip ·
      format mismatch = promote-to-richer
- [ ] Surface→Surface transforms (composable): `zoom`, `invert`, `rotate`, `shift`
- **Proof:** `a += b` with mismatched sizes/empty operands just works, never throws.

---

## v0.2.2 — Self-feedback (the ouroboros)
- [ ] One-surface self-feedback; **ping-pong hidden** under the hood
- [ ] `surface.feedback(gain, transform)` — weighted recurrence `s ← transform(s)·gain`
- [ ] Replace (`s = s.transformed()`, the tunnel) vs accumulate (`s += …`, the trails)
- [ ] **`Surface::trails(gain, blurRadius, tint)`** — OF-style feedback primitive:
  snapshot, clear, redraw previous frame with decay + optional blur + tint, then draw
  new content on top. Distinct from `feedback()` (in-place transform + accumulate);
  this is the clean "fresh background + soft echo" look from `surfaces2026.h`.
- [ ] Examples: invert-feedback (snake eating tail), 105% recursive zoom (tunneling)
- **Proof:** point Braid at itself — live video-feedback tunnels/mandalas at 60fps.

---

## v0.2.3 — Sketch tier complete (the Processing face)
- [ ] Finish `SketchApp`: stroke rendering, batching, `image()`, 3D (`box`/`sphere`)
- [ ] `pushMatrix`/`popMatrix`, camera/ortho/perspective fully wired through Surface
- [ ] **Port the old microframework sketch onto Braid**
- **Proof:** your previous bare-bones sketch runs unchanged-in-spirit on Braid.

---

## v0.3.0 — Media in / out
- [ ] `Texture::load` + `loadAsync` via mango (lib already present) — fast image decode
  - [ ] **Single-copy upload** (not "zero-copy" — impossible to discrete VRAM): mango decodes
        straight into a mapped `MapWrite|CopySrc` buffer at a 256-aligned stride with RGB→RGBA
        in-decode → `copyBufferToTexture`. Sync default uses `Queue::writeTexture` (no alignment
        rule). I/O thread decodes into the main-owned mapped pointer *(review 06-29)*
  - [ ] **Color policy:** sRGB/none → upload to an `-srgb` format (GPU sampler linearizes free);
        non-sRGB ICC → mango's bundled **lcms2** transform during decode. Expose the embedded
        profile so we can branch instead of always/never converting *(review 06-29)*
  - [ ] **`Surface::saveAsync`** — current `save()` spins `while (!done) ProcessEvents()`
        and hitches the frame. Add async variant with callback/Channel so readback completes
        next frame without stalling the tunnel.
  - [ ] **Keep mango behind the `Texture` seam** — no `mango::*` type in any public signature
        (ofWorks `ofImage.cpp` already proves this pattern) *(review 06-29)*
- [ ] `Buffer` + async readback
- [ ] **Video record:** encode a Surface's frames over time (the funnel pays off again)
- **Proof:** load an image, remix it through Surfaces, record the result to a file.

---

## v0.3.x — Reach (pick per appetite)
- [ ] `ComputePass` — GPU particles (Dawn compute; ofxDawn has a reference)
- [ ] Text (SDF atlas) · Audio (rtaudio/miniaudio — libs in scaffold)
- [ ] Shader hot-reload (file watcher) — tightens the creative loop
- [ ] **Asset hot-reload** beyond shaders: `AssetWatcher` that reloads WGSL → `Shader`,
      image → `Surface`, mesh → `Mesh` on disk change. Difference between a framework and
      a pleasant tool.
- [ ] **Syphon / Spout / NDI output** — route a `Surface` to Syphon (macOS), Spout
      (Windows), or NDI: `surface.publish("braid-out")`. Natural fit for the "Surface is
      routable" thesis; major differentiator for VJ/installation use.
- [ ] **OSC** — minimal `OscIn`/`OscOut` on top of `Channel<T>`. Installations speak OSC.
- [ ] **Frame-time overlay** — built-in fps + draw-call count + GPU memory HUD (or minimal
      ImGui example). Needed for iteration; easy to defer, easy to miss.
- **Proof:** a million-particle compute demo; text on screen; a sound playing.

---

## Later / if wanted (not committed)
- [ ] Cross-platform: Linux / Windows (RGFW supports both; Dawn builds exist)
  - [ ] Linux deep-color reality check: verify `surface.GetCapabilities()` on the actual
        install/VP box (Wayland 10-bit/HDR is compositor-gated — *not* an RGFW-vs-GLFW thing,
        both are passthrough; the swapchain format is Dawn's call) *(review 06-29)*
- [ ] **High-bit-depth / Virtual Production output** — internal Surface is already `RGBA16Float` ✅.
      Add real 10/12-bit delivery: request `RGB10A2Unorm`/`RGBA16Float` swapchain via
      `GetCapabilities`; and a **`Playout` target distinct from window present** —
      `Surface → AJA NTV2 / Blackmagic DeckLink` SDI. Simple path: `copyTextureToBuffer` → map →
      card SDK (one frame latency). Fast path: GPUDirect DMA via Dawn native-interop (open
      question if worth it). Let the LED processor do the final dither; festivals dither to 8-bit,
      VP keeps the bits all the way out *(review 06-29)*
- [ ] **Web/Emscripten** — interesting because Dawn ≈ browser WebGPU; sketches in a tab
- [ ] **CI builds** — GitHub Actions: build all examples on macOS on every push; catches
      Dawn/RGFW drift early; add before the codebase grows.
- [ ] **Semver discipline** — `VERSIONING.md` (one page: what v0.x.y means, what's a
      breaking change, dep update policy for Dawn/RGFW). Avoids panic when a dep update
      breaks the Metal surface code.
- [ ] Shared `libdawn.dylib` if shipping many sketches (move the ~10MB out of each binary)
- [ ] **Lint rule: always qualify `wgpu::`** — `braid::Surface` / `wgpu::Surface` collision
      acknowledged but not enforced; a `clang-tidy` check or pre-commit script stops it
      from drifting.
- [ ] **iOS** — doable, but not free. Dawn's Metal backend runs on iOS, and `CAMetalLayer` works
      on `UIView`. The blockers are RGFW (desktop-only), the input model (touch vs mouse/keyboard),
      the app lifecycle (UIKit owns the run loop), and sandboxed file access. The right prep is to
      hide RGFW behind a `PlatformWindow` abstraction now so an iOS backend can be plugged in later
      without a rewrite.

---

## Sequencing rationale (1 paragraph)
Visibility is already solved (`chalet run`), so **v0.1.1 is just polish** and the real
next move is the **Surface pivot (v0.2.0)** — it comes before the Sketch tier grows so
there's less to refactor; it's the architectural spine. **Algebra → feedback
(v0.2.1–.2)** is the soul and should land while the design is fresh; it's also the
cheapest *wow* per line. Sketch-tier completion and media follow once the spine and the
soul are set. Compute/text/audio/cross-platform are reach goals, gated by appetite, not
dependency. Bundling, packaging, and similar are trivial chalet sections — do them when
they matter, not before. Resist any step that doesn't end in something you can show.
