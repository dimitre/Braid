# Braid v0.1 — Build History & Design Origin

**Date:** 2026-06-28  
**Status:** ✅ Verified 60fps on macOS arm64  
**Scope:** v0.1.0 bring-up notes + v0.1.1 critical-fixes summary + the design origin story

---

## 1. RGFW API reconciliation (spec vs reality)

The spec assumed one RGFW API shape; the actual library differed throughout:

| Spec assumed | Actual RGFW |
|---|---|
| `RGFW_createWindow(name, RGFW_RECT, flags)` | `RGFW_createWindow(name, x, y, w, h, flags)` — no `RGFW_RECT` |
| `RGFW_event* = checkEvent(win)` | `bool checkEvent(win, &event)` — out-param |
| `ev->key`, `ev->button`, `ev->point` | union fields: `ev.key.value`, `ev.button.value`, `ev.mouse.x/y`, `ev.update.w/h` |
| `RGFW_quit` | `RGFW_windowClose` |
| hand-rolled `CAMetalLayer` attach | use RGFW's `RGFW_getLayer_OSX()` + `RGFW_window_setLayer_OSX()` |
| (none) | `RGFW_init()` / `RGFW_deinit()` required |
| `RGFW_mouse{Left,Middle,Right}` = 0,1,2 | mapped to `braid::MouseButton{Left,Right,Middle}` |

Dawn linkage: spec assumed `libdawn.dylib`; reality is a monolithic static `libwebgpu_dawn.a` from the ofxDawn scaffold.

---

## 2. Bugs found during bring-up (all fixed)

1. `MouseEvent` inline-enum-in-`optional` → declare `enum class MouseButton` separately.
2. `Result<T>` required default-constructible `T` (Screen/Mesh aren't) → optional storage.
3. `wgpu::VertexAttribute` has leading `nextInChain` → designated initializers must skip it.
4. `RGFW_quit` doesn't exist → `RGFW_windowClose`.
5. Nested `Settings{}` default-arg hit clang limitation → split constructors + delegation.
6. `fmt::format` needs `<fmt/format.h>`, not just `core.h`.
7. Chalet barfed on inline `# comments` inside framework name lists → removed them.
8. `RGFW_init()` / `RGFW_deinit()` missing → added.
9. **RGFW DND crash**: `RGFW_windowAllowDND` → `NSInvalidArgumentException` with nil array → dropped flag for v0.1.0.
10. Window not foregrounding → added `RGFW_window_show/raise/focus` + focus flags.

---

## 3. v0.1.1 critical fixes — summary (all applied, Claude review)

*Source: Claude's pre-implementation review of the deployment spec.*

| # | Issue | Fix |
|---|---|---|
| 1 | Inline enum in optional | Separate `enum class MouseButton` |
| 2 | `Mesh::line` missing device | Add `wgpu::Device` param |
| 3 | `clone()` signatures | Table + code aligned |
| 4 | Error handling inconsistency | Unified `Result<T>` everywhere |
| 5 | Channel thread safety | Restored table; callbacks fire in `pop()` on main thread |
| 6 | Sketch tier internals | State struct + TransformStack + defaultShader_ |
| 7 | Dawn binary | Prebuilt static `.a` (not dylib) |
| 8 | Validation default | `#ifdef NDEBUG` — off in release, on in debug |
| 9 | `bindUniform` lifetime | 3-slot rotating ring buffer |
| 10 | "Single-header" claim | Rephrased to "single-include" |

---

## 4. Design origin — Surface as the one primitive

*Dimitre's writing, preserved verbatim. This is the origin story of the design thesis.*

### The gem: ofFbo as a magic mirror

The real gold of openFrameworks isn't its breadth — it's `ofFbo`. An FBO is simultaneously a **destination** (you draw *into* it) and a **source** (you sample *from* it). That dual nature is the whole trick. Because of it you can throw stuff at it and remix the result, draw one inside another, ping-pong two for feedback, treat last frame as this frame's input.

It's **design for the elastic mind** — the same object is a canvas *and* a texture. Cinder never crystallized this as cleanly.

### The `+=` dream (and why it drowned)

Once dreamt that in OF you could sum one FBO into another — `fbo += fbo2` — layers added like math. Woke, wrote the PR. People loved it. But it drowned in *ifs*: what if sizes differ, what if depths differ, what if one is uninitialized. Nobody merged; it wasn't pushed.

**The insight the ifs were hiding:** those weren't errors, they were *undefined points in an algebra*. Math has no ifs — it has definitions. `0 + B = B`; you don't throw on adding zero. The PR died because the edges were treated as **guards** instead of **definitions**.

| "if" that killed it | Total definition |
|---|---|
| one is uninitialized | uninitialized = additive identity (`A += empty` → A unchanged; `empty += B` becomes B) |
| sizes differ | composite at origin, clipped — any size pair has a result |
| formats differ | promote to the richer format (like `int + float → float`) |

**Cultural note:** it didn't merge because committee review optimizes safety over audacity — a thing whose whole value is its boldness can't survive a "what could go wrong" gate. Braid is *yours*. Here the dream is allowed to exist. The discipline isn't *fewer* guards bolted on; it's **defining the algebra so guards are unnecessary.**

### The ouroboros (self-feedback)

*Lived precedent (analog video feedback):* point a camera at its own monitor. Frame N is a transform of frame N−1. Classic moves: **invert** (chromatic snake-eating-tail), **recursive zoom at 101–105%** → tunneling. This is `surface += surface.transformed()`, and the crucial thing: **it's ONE surface feeding itself.** Not two.

**The model is the ouroboros; ping-pong is an implementation artifact.** GPUs can't read+write the same texture in one pass, so feedback is *implemented* with two buffers and a swap. But the artist never thinks in two buffers — they think in one loop closing on itself. **Braid should expose the ouroboros and hide the ping-pong.**

**Gain is the hand on the loop.** Pure accumulation blows out; pure replacement collapses. Analog feedback is stable *because of decay* — phosphor persistence, gain < 1. The beautiful standing patterns live at the edge where loop gain ≈ 1. Same physics as audio feedback: a coefficient turns a screaming loop into a controlled standing wave (Hendrix vs. noise). One knob; the API makes it first-class: `s.feedback(0.97, [](Surface& s){ s.zoom(1.05); s.invert(); });`

### One-line creed

*Everything is a Surface; you draw into Surfaces; the screen is just the Surface you show. Surfaces add like math — total, never erroring, beautiful enough to just exist. A Surface can eat its own tail; the framework hides the ping-pong, the artist holds the gain.*
