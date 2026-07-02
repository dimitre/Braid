# Braid HiDPI — logical points in the API, pixels in the Surface

**Status:** design, settled 2026-07-02. Next implementation step after TinyUI M1, *before*
M2 text. Rationale: text rendering, UI hit-testing, and every future addon inherit whatever
unit law core establishes. oF never had one law and paid for it forever; this is the fortress.

---

## 1. The law

> **The sketch-facing API speaks logical points. `Surface` speaks pixels. `Window` is the
> only translator between them.**

- **Point space (what sketches see):** `Window::width()/height()`, `MouseEvent.pos`,
  SketchApp draw coordinates, TinyUI rects, `AppSettings::width/height`, window position,
  monitor rects. A sketch written today runs unchanged on Retina — just sharper.
- **Pixel space (what textures are):** every `Surface`. `width_`/`height_` are real texel
  dimensions by construction — `mainSurface_`, the swapchain, and user canvases alike.
  `Surface` has no notion of points and never will.
- **The one translation:** `Window` multiplies by `pixelRatio` in exactly two mechanisms —
  surface configuration/creation (`braid_app.cpp`) and SketchApp's ortho projection, which
  maps logical size onto the pixel-sized `mainSurface_` (`braid_sketch.cpp`).
- `Window::pixelRatio()` (read-only `float`, 1.0 or 2.0 on macOS) and a physical-size
  accessor are exposed for the few who need physical sizing. They are *reads*, never units
  users must convert with.
- **The honest seam:** on Retina, `window.surface().width()` reports 2560 while
  `window.width()` reports 1280. Correct, not a bug — each type speaks its own single unit.
- **pixelRatio exists only at the Window boundary.** `Surface(device, w, h)` is exact
  pixels — the LED-wall path. A `BigScene`-style canvas keeps its authoritative pixel
  dimensions no matter which window previews it, at any scale.

Why points and not pixels-first: mouse space == draw space with no conversion, every
existing sketch keeps its proportions, stroke weights scale correctly, and exports come
out crisp. The pixels-first fear ("parallel unit APIs") is answered structurally: *each
type* has exactly one unit — `Window` points, `Surface` pixels — never both on one type.

## 2. The `hidpi` flag (performance escape hatch, not a correctness mode)

- `AppSettings::hidpi = true` by default — oF's convention: native-res out of the box, you
  opt *down*, not up. The opt-out exists because 2× ratio is 4× fragment work and Braid's
  feedback/blur chains are already GPU-bound at 1×; a 5K install rig needs the hatch.
- **Opt-out shape: shrink `mainSurface_` only.** The swapchain always configures at
  physical pixels; `hidpi = false` just allocates `mainSurface_` at point size, and the
  existing size-absorbing `endFrame()` blit does the upscale *inside Braid* — never Core
  Animation. Cross-platform, and the final blit at physical res is one fetch/pixel.
- Addons and sketches must not branch on the flag — they see the same point-space API
  either way.
- A general `renderScale` knob (super/undersampling at any ratio) is a separate future
  feature; don't half-design it here.

## 3. Current state (verified in code, 2026-07-01)

Braid today is coherently **1× everywhere**, which is why TinyUI M1 hit-testing works:

- Window created in points: `RGFW_createWindow(…, w, h, …)`.
- Swapchain configured at point size: `configureSurface()` uses `settings_.width/height`
  — `braid_app.cpp:535,540-541`. Dawn sets the CAMetalLayer `drawableSize` from this.
- `swapSurface_`/`mainSurface_` allocated and resized in points — `braid_app.cpp:547-554`
  and the resize case at `:618-629`.
- Mouse in points straight from Cocoa: `ev.mouse.x/y` — `braid_app.cpp:606`.
- The layer: `attachMetalLayer()` (`braid_app.cpp:47`) attaches a bare `CAMetalLayer` via
  RGFW's `setLayer:` — **`contentsScale` is never set**, stays 1.0. Result on Retina:
  quarter-resolution drawable, AppKit upscales → the current blur.

## 4. RGFW facts (verified in vendored RGFW.h)

- `RGFW_monitor.pixelRatio` — on macOS read from real `backingScaleFactor` (`RGFW.h:14750`).
  (On Windows/X11 it's a dpi≥192 heuristic — revisit per-platform when braid ports;
  macOS-only for now.)
- `RGFW_scaleUpdated` fires from `viewDidChangeBackingProperties` (`RGFW.h:13610`,
  registered at `:14014`) — the correct Apple trigger for "this window's backing scale
  changed", covering drags between differently-scaled displays.
- ⚠️ **Payload trap: the event carries `mon->scaleX/scaleY`, a PPI-derived value
  (`ppi_width / (96 × backingScaleFactor)`, `RGFW.h:14750-14755`) — NOT the backing
  scale.** Treat the event purely as a signal; on receipt re-query the authoritative
  source and ignore the payload.
- Dawn's Metal backend sets `CAMetalLayer.drawableSize` from the WGPU surface config,
  decoupling it from `bounds × contentsScale` — so configuring the surface at pixel size
  is the load-bearing change. Still set `layer.contentsScale` (one msgSend;
  `braid_app.cpp` is already the only Cocoa TU) so CA compositing hints stay honest.
  Verify drawableSize behavior at implementation time.

## 5. Implementation plan (all in `braid_app.cpp` + `braid_sketch.cpp` + small `braid.h` additions)

1. **`Window::pixelRatio_`** (`float`, default 1.0) + public `float pixelRatio() const`.
   Authoritative source on macOS: `NSWindow.backingScaleFactor` via `objc_msgSend` on
   `win->src.window` — same raw-msgSend idiom as the title setter. Fallback:
   `RGFW_window_getMonitor(win)->pixelRatio`. Query right after `RGFW_createWindow`.
   *Always re-query the authoritative value; never accumulate deltas or trust event payloads.*
2. **Layer:** after `attachMetalLayer`, set `layer.contentsScale = pixelRatio_` (one
   msgSend). Re-set whenever scale changes.
3. **Pixel size helper:** `pxSize() = { lround(settings_.width × pixelRatio_),
   lround(settings_.height × pixelRatio_) }`. `settings_` stays in points.
4. **`configureSurface()`:** configure `config.width/height` from `pxSize()` — always
   physical, regardless of the `hidpi` flag (see §2).
5. **`createSwapAndMainSurfaces()`:** `swapSurface_` at `pxSize()`; `mainSurface_` at
   `settings_.hidpi ? pxSize() : point size`.
6. **Mouse: unchanged.** Stays in points end to end — no scale applied anywhere.
7. **Resize:** `ev.update.w/h` are points → update `settings_`, reconfigure swapchain +
   resize surfaces per steps 4-5. `WindowEvent::Resized` payload stays in **points**
   (unchanged semantics — sketches compare it against `width()/height()`, also points).
8. **New case `RGFW_scaleUpdated`:** re-query scale (step 1's source, ignore the event
   payload), update `pixelRatio_` + layer `contentsScale`, rerun the step-7 resize path,
   emit `WindowEvent::Resized` — point size is unchanged but the backing surfaces
   reallocated, and sketches + TinyUI already handle Resized. No new event type in v1.
9. **`Window::width()/height()`: unchanged** — logical points. Add the physical-size
   accessor and `pixelRatio()` beside them, with a comment stating the law.
10. **SketchApp projection:** build the ortho from logical size mapped onto the
    pixel-sized `mainSurface_` viewport — the *only* `× pixelRatio` outside
    `braid_app.cpp`.
11. **Startup debug line:** print requested points, detected scale, drawable pixels.

Untouched by design: the Compositor (blits are UV-based, size-agnostic), Surface algebra,
feedback examples, user-created canvases. Exports (`save()` / future video) read
`mainSurface_` and simply get native resolution — a 1280×720 window on Retina saves a
crisp 2560×1440 PNG. Intended; `hidpi = false` is the hatch when exact-logical output
dimensions are required.

## 6. Sampling / filtering (settled by live test, 2026-07-01)

- **The global sampler stays Linear.** Nearest was flipped globally and rejected on
  sight: `feedback()`'s iterated fractional resampling degrades badly (per-frame
  quantization accumulates). The Nearest lines remain commented out next to the sampler
  (`braid_compositor.cpp:309-317`) as the record.
- Effect passes are filter-agnostic — blur taps integer texel offsets
  (`u.dir * u.texelSize * fi`, `braid_compositor.cpp:145`), threshold is 1:1, contour
  offsets are radius-scaled texel steps — so any filter knob only ever affects scaling
  composites and `feedback()` resampling.
- **Future:** per-`Surface` filter preference (`Filter::Linear` default,
  `Filter::Nearest` opt-in), honored wherever that surface is sampled — the chunky-pixel
  path for LED walls / pixel-art, one knob, no per-call ceremony.

## 7. Spanning windows / installations

For monitor-union borderless windows (the `settings_.monitors` path), AppKit assigns the
window *one* backing scale (the display it's "on"); migration fires
`viewDidChangeBackingProperties` → handled by step 8. Projector/LED rigs are typically 1×,
so installations see `pixelRatio = 1.0` and behave exactly as today. Mixed-DPI unions have
no layer-level fix (one `contentsScale` per window is an OS constraint) — the answer is
**one borderless window per physical monitor**, which the `Application`/`Window` split
already supports: composite the same canvas into each window instead of spanning one.

## 8. TinyUI follow-up (small, after core lands)

- Under the points law, hit-testing needs **no change**: mouse and widget rects share
  point space. Widget metrics (200×20 sliders) keep their physical size automatically —
  the projection scale handles it; no `uiScale` multiplication in the layout parser.
- The overlay `Surface` must be allocated at pixel size and drawn through the same
  logical→pixel projection as `mainSurface_` — that's what makes UI lines/text crisp
  (UI is where 1× blur is most visible).
- M2 bitmap font: glyphs go through the scaled projection; for crisp text the atlas/glyph
  rendering should be scale-aware at M2 (integer scale for the embedded bitmap font).
  Defer details to M2.
- Mid-run scale migration: nothing to re-layout (metrics are points); only surfaces
  reallocate via step 8. Retires the `md/ui.md` §8 DPI warning once verified.

## 9. Verification protocol (needs human eyes on the Retina rig)

1. Startup line: `requested 1280x720, scale 2.0, drawable 2560x1440`.
2. **Corner test:** cursor at bottom-right of the window → `mousePos()` ≈
   `(width()-1, height()-1)` — both in points.
3. **Export test:** press S in `feed` → saved PNG is 2560×1440 and *crisp* (today:
   1280×720 and soft).
4. **Migration test:** drag the window Retina laptop ↔ 1× external: expect one resize
   transition, no blur or stretch on either display, no size feedback loop (watch for
   repeated Resized events — a classic HiDPI bug when points/pixels get re-fed).
5. **UI test:** `ui_demo` slider knob tracks exactly under the cursor on both displays.
6. **Opt-out test:** `hidpi = false` on one window → soft-but-fast output, exports at
   logical size, upscale still done by Braid's blit (not CA).

## 10. The fortress rules (what oF got wrong; never do these)

1. **One unit per type.** `Window` speaks points; `Surface` speaks pixels. The moment a
   type answers in both units, every call site becomes a guessing game.
2. **Scale conversion lives in two known places.** If a `× pixelRatio` appears anywhere
   but the `braid_app.cpp` seam and SketchApp's projection setup, it's a bug.
3. **Re-query, don't track.** The authoritative scale is `backingScaleFactor`, read fresh
   at every scale event; accumulated or cached-and-forgotten scale is how the
   2×-applied-twice bug is born. And never trust the `RGFW_scaleUpdated` payload (§4).
4. **`hidpi` is a performance hatch, not a mode.** Addons and sketches never branch on
   it; the point-space API is identical either way.
5. **User Surfaces are sacred pixels.** Nothing in `Window`'s scale plumbing may touch
   `Surface(device, w, h)` dimensions — that's the LED path, and it stays exact.
