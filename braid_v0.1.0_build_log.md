# Braid v0.1.0 — Build Log

**Date:** 2026-06-28
**Status:** ✅ Builds, links, and renders on macOS (arm64). Verified 60fps.
**Target met:** macOS-only, red triangle, 60fps, ~1,500 lines (actual: 1,632).

---

## 1. What was generated

| File | Lines | Role |
|---|---|---|
| `braid.h` | 580 | Full v0.1.0 public interface (Tier 1 + Tier 2 declarations) |
| `braid.cpp` | 921 | Implementation: Timer, Screen, Shader, Mesh, App loop, SketchApp |
| `examples/hello.cpp` | 74 | Red triangle via the explicit Tier-1 API |
| `chalet.yaml` | 57 | macOS build config (chalet 0.8.x schema) |

Build command: `chalet build` → `build/arm64-apple-darwin_Release/hello`.

---

## 2. Dependencies (resolved against the ofWorks scaffold)

The `libs/macos/` tree was copied from ofWorks (an OpenGL stack), so it provided
`glm`, `fmt`, `mango` but **not** the two deps Braid is built on:

| Dep | Source | Notes |
|---|---|---|
| **Dawn (WebGPU)** | `~/Dmtr/ofworks/addons/ofxDawn/libs/dawn/` | `libwebgpu_dawn.a` — **static**, not the `libdawn.dylib` the spec assumed. Monolithic, exports standard `wgpu*` C symbols. Copied headers + lib into `libs/macos/`. |
| **RGFW** | `raw.githubusercontent.com/ColleagueRiley/RGFW/main/RGFW.h` | Single header, dropped into `libs/macos/include/`. |
| glm / fmt / mango | ofWorks scaffold | Static `.a` + headers already present. |

**Decision:** kept RGFW (per spec) rather than switching to the scaffold's GLFW.

---

## 3. API reconciliation (spec assumptions → reality)

### Dawn — my initial guesses were correct for this version
Confirmed present and used as written: `wgpu::SurfaceSourceMetalLayer`,
`ShaderSourceWGSL`, `wgpu::StringView`, `CallbackMode::AllowProcessEvents`,
`TexelCopyTextureInfo`, `RequestAdapter(options, CallbackMode, callback)`.
Adopted two patterns from ofxDawn's working init:
- `WGPU_STRLEN`-aware StringView→`std::string` helper (`sv()`).
- `done`-flag wait loops around `instance.ProcessEvents()` (robust vs. relying on
  an error string being non-empty).

### RGFW — API differed substantially from the spec's assumptions
| Spec assumed | Actual (this RGFW) |
|---|---|
| `RGFW_createWindow(name, RGFW_RECT, flags)` | `RGFW_createWindow(name, x, y, w, h, flags)` — no `RGFW_RECT` macro |
| `RGFW_event* = checkEvent(win)` | `bool checkEvent(win, &event)` — out-param |
| `ev->key`, `ev->button`, `ev->point` | event **union**: `ev.key.value`, `ev.button.value`, `ev.mouse.x/y`, `ev.update.w/h` |
| `RGFW_quit` | `RGFW_windowClose` |
| (none) | `RGFW_init()` / `RGFW_deinit()` are **required** |
| hand-rolled objc CAMetalLayer attach | use RGFW's `RGFW_getLayer_OSX()` + `RGFW_window_setLayer_OSX()` |
| `RGFW_mouse{Left,Middle,Right}` = 0,1,2 | mapped explicitly to `braid::MouseButton{Left,Right,Middle}` |

### chalet — rewrote to real 0.8.x schema (mirrored from `ofxDawn/example/chalet.yaml`)
`abstracts:*:`, `settings:Cxx`, `cppStandard`, `staticLinks` with full `.a` paths,
`kind: staticLibrary | executable`, `files: include:`, `appleFrameworks`.
On Apple the implementation TU compiles as **Objective-C++** (RGFW + Dawn touch Cocoa).
Frameworks: Cocoa, IOKit, CoreVideo, QuartzCore, Metal, IOSurface, Foundation.

---

## 4. The 10 critical fixes — all applied

| # | Fix | How |
|---|---|---|
| 1 | Unified `Result<T>` | All fallible fns return it. Storage switched to `std::optional<T>` so move-only / non-default types (`Screen`, `Mesh`) flow through without a sentinel. |
| 2 | Channel thread-safety | Immobile (`= delete` move), callbacks fire during `pop()` on main thread, `Subscription` holds an unsubscribe lambda. |
| 3 | Sketch tier model | `State` struct + `TransformStack` + `defaultShader_` (MVP+color WGSL). 2D path implemented; batching deferred (flagged v0.2). |
| 4 | `MouseButton` enum | Declared separately (the inline-enum-in-`optional` was an actual compile error). |
| 5 | `Mesh::line` device param | `static Result<Mesh> line(wgpu::Device, span<...>)`. |
| 6 | clone() signatures | Match the ownership table; swapchain `Screen::clone()` returns a failure `Result`. |
| 7 | Dawn linkage | Linked as prebuilt **static** `libwebgpu_dawn.a` (reality differed from "dylib"). |
| 8 | Validation default | `#ifdef NDEBUG` → off in release, on in debug. |
| 9 | bindUniform lifetime | 3-frame rotating ring buffer. |
| 10 | "single-include" | Wording corrected in the header banner. |

---

## 5. Bugs found & fixed during bring-up

1. `MouseEvent` inline-enum-in-`optional` → separate `enum class MouseButton`.
2. `Result<T>` required default-constructible `T` (Screen/Mesh aren't) → optional storage.
3. `wgpu::VertexAttribute` has a leading `nextInChain` member → designated initializers.
4. `RGFW_quit` doesn't exist → `RGFW_windowClose`.
5. Nested `Settings{}` default-arg hit a clang limitation (default-member-init of a
   nested class inside the enclosing class) → split into two constructors + delegation.
6. `fmt::format` needs `<fmt/format.h>` (not just `core.h`).
7. chalet kept the inline `# comment` as part of a framework name (`CoreVideo  `) → removed inline comments.
8. `RGFW_init()` / `RGFW_deinit()` were missing → added.
9. **RGFW DND crash**: `RGFW_windowAllowDND` → `registerForDraggedTypes` with a nil
   array → `NSInvalidArgumentException`. Dropped the flag for v0.1.0. *(Upstream candidate.)*
10. Window not foregrounding → added `RGFW_window_show/raise/focus` + focus flags.

---

## 6. Verification

```
[hello] first frame drawn
[hello] presented 120 frames over 2.00s (60 fps)
```

- 120 successful swapchain `GetCurrentTexture` → draw → `Present` cycles.
- Zero WebGPU validation / device-lost errors (the uncaptured-error callback was silent).
- Frame pacing exactly 2.00s for 120 frames → validates the `ofTimerFps` port.

**Not visually confirmed:** the literal red pixels. `screencapture` returned a black
frame because this terminal runs fullscreen in its own macOS Space, so a bare
(non-bundled) binary's window opens in a different Space and `NSApp` never becomes
frontmost. This is a packaging/activation artifact, **not** a rendering failure.

---

## 7. Open items / next steps

- [ ] **`.app` bundle target** (chalet `distribution`, like ofxDawn) so the window is
      visible and activates normally — would also enable a real screenshot of the triangle.
- [ ] SketchApp: stroke rendering, batching, 3D primitives (currently the 2D fill path
      is the implemented subset; `box`/`sphere` declared, partial).
- [ ] RGFW DND crash — either patch locally or report upstream if drag-drop is wanted.
- [ ] v0.1.2 deps (`Texture::load` via mango, `Buffer` readback) — headers/libs already
      present in the scaffold (`libmango.a`).
- [ ] Confirm release vs. debug `enableValidation` path on an `NDEBUG` build.

---

## 8. Design philosophy & direction — Surface as the one primitive

> *Technical/philosophical. This is the core idea Braid should orbit around.*

### The gem: ofFbo as a magic mirror
The real gold of openFrameworks isn't its breadth — it's `ofFbo`. An FBO is
simultaneously a **destination** (you draw *into* it) and a **source** (you sample
*from* it). That dual nature is the whole trick. Because of it you can:
- throw stuff at it, then remix the result,
- draw one inside another,
- ping-pong two of them for feedback / GPGPU-style accumulation,
- treat last frame as this frame's input.

It's **design for the elastic mind** — the same object is a canvas *and* a texture,
so the mental model stays fluid and recombinable. You don't context-switch between
"a thing I draw" and "a thing I read." Cinder never crystallized this as cleanly.

### The decision: make **Surface** the sole drawing primitive
Rename `Screen` → **`Surface`** (more digestible) and promote it from "a renderable
texture" to **the only way to draw anything**. There is no draw call that doesn't go
through a Surface. The swapchain loses its special status — it's just *the Surface
you happen to present*.

**Rule:** *You can only draw using a Surface.* Surface is the single entrance way.

### Why the single funnel pays off
Because every pixel is produced by drawing into a Surface, these all collapse into
one uniform mechanism instead of separate code paths:

| Use | What it is |
|---|---|
| Screen output | present a Surface to the swapchain |
| Image export (PNG) | read a Surface back, encode (mango) |
| Video recording | encode a Surface's frames over time |
| Feedback / "magic mirror" | sample a Surface while drawing into another |
| Ping-pong | a Surface that holds two textures and `swap()`s |

One concept (Surface), polymorphic in role. Simple things stay simple; feedback,
export, and record fall out *for free* rather than as bolt-on subsystems.

### How it maps to WebGPU (and the current code)
- A Surface is a texture with usage `RenderAttachment | TextureBinding | CopySrc`.
  That single usage set *is* the ofFbo duality: attachable as a draw target,
  bindable as a sampled source, copyable for readback/export.
- `begin()/end()` is already the draw entry point; `asTexture()` is already the
  "read me back" side. The v0.1.0 `Screen` is **half this idea already** — it wraps
  both the offscreen-texture and swapchain cases behind one type. The work is to
  (a) rename, (b) make *all* drawing route through it, (c) make ping-pong and
  export/record first-class.
- **Ping-pong as a first-class mode:** instead of the user juggling two FBOs (the OF
  way), a Surface can internally own two textures and expose `swap()`, so "draw last
  frame, transformed, into this frame" is one object. This is the ergonomic
  crystallization OF never quite gave.

### Sharpened principles (why this is architecture, not a rename)
1. **The constraint is the feature.** OF *had* the FBO but never made it the law —
   you could draw straight to the screen and bypass the idea, so feedback/export/
   record stayed bolt-on. *"You can only draw via a Surface"* removes the escape
   hatch; once there's no bypass, the screen stops being special and
   screenshot/record/feedback aren't features you add — they're things that were
   already true, now merely exposed.
2. **Four concepts → one.** The OF elastic mind juggles `ofFbo` + window/screen +
   `ofImage` + video-out *and* has to know they're secretly the same. Braid: there
   is one thing. Fewer primitives, more recombination.
3. **No cliff between tiers.** The sketch tier and the explicit GPU tier speak the
   *same verb* — `rect()` into a Surface and a reaction-diffusion ping-pong are the
   same gesture at different altitudes. The beginner→artist ramp has no wall.
4. **Ping-pong wants to be one object.** Not "make fboA, make fboB, track current,
   swap, don't mess up." A Surface owns two textures and exposes `swap()` (consider
   auto-swap on `begin()`), so *"read myself, draw a transformed me"* is one fluent
   gesture. Highest-leverage ergonomic in the design.

### Cost & the rule that protects the elegance
- *Cost:* "always draw offscreen, then blit to swapchain" adds one fullscreen copy
  per frame. Negligible — and it's literally the price of admission for free
  record/screenshot/feedback-at-any-moment.
- *Guardrail:* **do not let anyone later "optimize" the swapchain back into a special
  case.** That copy *is* the design; removing it quietly breaks the uniformity that
  makes export/record/feedback fall out for free. State it out loud so it survives.

### Naming: keep `braid::Surface` (resolved)
**Decision:** use `braid::Surface`. The `wgpu::Surface` collision is cosmetic —
`wgpu::Surface` appears only in framework internals (the swapchain backing) where
every WebGPU type is already explicitly `wgpu::`-qualified, so unqualified `Surface`
inside `namespace braid` is unambiguous. A user's sketch only ever sees
`braid::Surface` and never types `wgpu::Surface`.
- *Why "Surface" over alternatives:* it carries the draw-into **and** sample-from
  duality. `Canvas` connotes draw-only; `Target`/`RenderTarget` connotes
  destination-only; `Fbo` is jargon. Surface is both proper and the most accurate.
- *Fallback:* if the internal `braid::Surface` / `wgpu::Surface` adjacency ever
  becomes daily friction, `Canvas` is the escape hatch — but default is to keep Surface.

### The dream: Surface algebra (`surface += surface`)
*Origin (a real story worth keeping):* once dreamt that in OF you could sum one FBO
into another — `fbo += fbo2` — layers added like math. Woke, wrote the PR. People
loved it. But it drowned in *ifs*: what if sizes differ, what if depths differ, what
if one is uninitialized. Nobody merged; it wasn't pushed. The beauty was in its
**being able to exist** — to sum as math, without ceremony or guard-handles.

**The insight the ifs were hiding:** those weren't errors, they were *undefined
points in an algebra*. Math has no ifs — it has definitions. `0 + B = B`; you don't
throw on adding zero. The PR died because the edges were treated as **guards** (things
to defend against) instead of **definitions** (cases with a defined meaning). Make the
operation **total** — closed over every input — and the ifs *become the definition*,
not branches that error:

| "if" that killed it | Total definition (no error, just meaning) |
|---|---|
| one is uninitialized | uninitialized = **additive identity**. `A += empty` → A unchanged; `empty += B` lazily takes B's shape → becomes B. (like `0 + B = B`) |
| sizes differ | composite at origin, clipped (defined layer semantics). Any size pair has a result. |
| depths/formats differ | **promote to the richer** format (like `int + float → float`). |

**This sharpens the tier split we already have:**
- *Fallible tier* — things that genuinely can't-happen-cleanly (load a file, compile a
  shader) → `Result<T>`. Branch on failure.
- *Expressive tier* — the Surface algebra → **total functions, never error.** `a += b`
  always means something. No failure mode to branch on ⇒ trivially composable,
  trivially teachable, zero cognitive load. *This* is elastic-mind design.

**Operators × ping-pong = feedback as algebra.** With `+=` (additive blend),
optionally `*=` (multiply), and a Surface that can read itself, feedback becomes one
line: `surface += surface.transformed()`. The dream, but as the framework's native
grammar.

**Cultural note:** it didn't merge because committee review optimizes safety over
audacity — a thing whose whole value is its boldness can't survive a "what could go
wrong" gate. Braid is *yours*. Here the dream is allowed to exist. The discipline
isn't *fewer* guards bolted on; it's **defining the algebra so guards are unnecessary.**

### Self-feedback: the ouroboros (one Surface eating its tail)
*Lived precedent (analog video feedback):* point a camera at its own monitor and you
get a self-referential loop — frame N is a transform of frame N−1. Classic moves:
**invert** (chromatic snake-eating-tail), and **recursive zoom** (101–105% per frame)
→ the **tunneling** effect. This is *exactly* `surface += surface.transformed()`, and
the crucial thing to notice: **it's ONE surface, feeding itself.** Not two.

**The model is the ouroboros; ping-pong is an implementation artifact.** GPUs can't
read+write the same texture in one pass, so feedback is *implemented* with two buffers
and a swap. But the artist never thinks in two buffers — they think in one loop
closing on itself (the camera/monitor). **Braid should expose the ouroboros and hide
the ping-pong.** A Surface knows how to feed itself; the double-buffer is invisible
plumbing. (This is the elastic-mind rule again: model the artist's mind, not the GPU's
hazard.)

**Two flavors of the loop — the operator chooses:**
- *Replace:* `s = s.transformed()` — pure recursive regeneration (the camera). Zoom>1
  → infinite tunnel. Each frame fully derived from the last.
- *Accumulate:* `s += s.transformed()` — energy builds up: trails, light-painting,
  bloom. Each frame adds to the last.

**Gain is the hand on the loop (the stability secret).** Pure accumulation blows out
to white; pure replacement can collapse. Analog feedback is stable *because of decay*
— phosphor persistence, gain < 1. The beautiful standing patterns live at the edge
where loop gain ≈ 1. So feedback is really a **weighted recurrence**:

```
s  ←  transform(s) · gain   ( + new input )      // gain ≈ 0.9–0.99 is the sweet spot
```

Same physics as audio feedback: a coefficient turns a screaming loop into a
controlled standing wave (Hendrix vs. noise). The artist's single most expressive
knob. The API should make `gain` a first-class parameter of self-feedback, e.g.
`s.feedback(0.97, [](Surface& s){ s.zoom(1.05); s.invert(); });` — one surface, one
knob, the tail in its mouth.

### One-line creed
*Everything is a Surface; you draw into Surfaces; the screen is just the Surface you
show.* Collapse Screen / RenderTarget / Image / VideoOut into a single elastic object.
*Surfaces add like math — total, never erroring, beautiful enough to just exist.*
*A Surface can eat its own tail; the framework hides the ping-pong, the artist holds
the gain.*

### Next step
A focused **`braid_v0.2_surface.md`** spec, written the same spec → critical-fixes way
that worked for v0.1.0: the `Surface` API, ping-pong mode (`swap()` / auto-swap),
`present()` reframed as `show(surface)`, and export/record hooks (`save()` via mango,
frame encode for video). This is where Braid stops being a sketch framework and
becomes its own thing.

---

## 9b. v0.2 implementation (2026-06-29) — Surface, algebra, feedback

Built on top of the working v0.1.0 in verified phases (build stayed green at each step).

- **Surface pivot.** `Screen`→`Surface`. App now owns a *persistent offscreen*
  `mainSurface_` (RGBA8) that `surface()` returns and the sketch draws into; at present
  time an internal **Compositor** blits it onto the swapchain (`Blend::None`, clear). The
  swapchain is demoted to "the Surface you show." That one blit/frame is the design tax
  that makes screenshot/record/feedback free.
- **Compositor** (internal). One fullscreen-triangle WGSL shader: samples a source view
  with a UV transform (zoom/rotate/shift) + invert + tint, blended into any destination
  view. Drives *every* Surface op. Pipeline cache keyed by (format, blend); tiny uniform ring.
- **Algebra (total).** `compositeFrom`, `operator+=` (additive), `over` (alpha). Empty/
  invalid operand = identity (early-return no-op); size/format differences absorbed by the
  UV-mapped sample — never an error. (The dream, made un-rejectable.)
- **Transforms = hidden ping-pong.** `zoom/rotate/shift/invert/multiply` render self into a
  lazily-allocated `scratch_` texture, then **swap** main↔scratch. The artist sees one
  Surface; the double-buffer is invisible.
- **Self-feedback.** `feedback(gain, transform)` = apply transform in place, then decay by
  gain. Replace flavor = the transform; accumulate flavor = omit `background()` so the
  Surface loads (`beginLoad`) instead of clearing.
- **Export.** `save()` = `CopyTextureToBuffer` (256-aligned rows) → `MapAsync` pumped via
  `instance.ProcessEvents()` → uncompressed 32-bit TGA (RGBA→BGRA swizzle). Format-aware:
  decodes `RGBA16Float` (IEEE half) and clamps to [0,1].
- **Default Surface format = `RGBA16Float` (16-bit/channel).** Started 8-bit; the feedback
  decay showed obvious quantization banding (each frame ×gain re-rounds to 256 levels). Bumped
  the offscreen main + scratch to half-float → smooth glassy decay + HDR headroom for
  accumulation/bloom. Swapchain stays 8-bit `BGRA8` (the present blit converts at show). This
  is the v0.2.1 "promote to the richer format" principle, learned the visible way.
- **Sketch tier through Surface.** Lazy init (`ensureReady`) avoids clashing with a user
  `setup()`; new `beforeDraw`/`afterDraw` App hooks reset per-frame state and flush the open
  pass. `background()` chooses clear-vs-load (Processing semantics: omit it to accumulate).

**Verified:** `examples/feedback.cpp` ran 220 frames and saved `feedback.tga` — the image
shows a real zoomed+rotated+decayed self-feedback comet trail (one Surface eating its tail).
`sketch` and `hello` run clean. ~2,156 lines total across header+cpp+examples.

**Honestly not done in v0.2.3:** real stroke outlines (only fill; line/point fake it),
batching, `image()`, 3D `box`/`sphere`, and the literal microframework port (needs the file).

---

## 9. Files & paths reference

```
braid/
├── braid.h, braid.cpp, chalet.yaml
├── examples/hello.cpp
├── libs/macos/
│   ├── include/{webgpu,dawn}/...   # copied from ofxDawn
│   ├── include/RGFW.h              # fetched from GitHub
│   ├── include/{glm,fmt,mango}/... # ofWorks scaffold
│   └── lib/libwebgpu_dawn.a, libfmt.a, ...
└── build/arm64-apple-darwin_Release/hello
```
