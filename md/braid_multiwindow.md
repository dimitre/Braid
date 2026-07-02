# Braid — Multi-window design (start-thinking)

**Status:** implemented (`Application`/`Window`/`App` split, `examples/multiwindow.cpp`,
monitor-spanning). See `braid_multiwindow_plan.md`'s "Status: implemented" section
for the bugs found and fixed in Kimi's first pass, and §6 below for multi-monitor
spanning. This doc otherwise still describes the design accurately — the RGFW/WebGPU
facts below were verified directly against `libs/macos/include/RGFW.h`.
**Prereq landed:** the TU split (roadmap #2) is done — `core/braid.cpp` is gone;
the implementation is `braid_{timer,compositor,surface,shader,mesh,app,sketch}.cpp`
behind the single public header `core/braid.h`. `braid_app.cpp` is the *only* TU
that compiles `RGFW_IMPLEMENTATION` + Cocoa; that isolation is what makes the
window/device separation below tractable.

This doc answers the two questions the task posed:
1. **How does multi-window work in RGFW?** (grounded in `libs/macos/include/RGFW.h`)
2. **How do we share the WebGPU context across windows?** (the Dawn answer)

Then it proposes the smallest refactor of Braid that gets us there and ends in a
demo (`examples/multiwindow.cpp`), per the roadmap's "every milestone is a thing
you can see/run" rule.

---

## 1. RGFW is already multi-window — the constraints are *global state* + *one event queue*

Verified in the bundled RGFW header:

- **Many windows, one library instance.** `RGFW_createWindow(...)` can be called
  repeatedly; each returns an independent `RGFW_window*` with its own view/layer.
  But `RGFW_init()` installs a *single* `static RGFW_info _rgfwGlobal` as the
  process-wide state (`RGFW_init` → `RGFW_init_ptr(&_rgfwGlobal)`), and
  `RGFW_deinit()` tears that down for **everyone**. (There is a `_ptr` family —
  `RGFW_init_ptr`/`RGFW_setInfo`/`RGFW_getInfo` — for supplying your own info
  struct, e.g. per-thread; we don't need it for a single-threaded multi-window app.)
  → **init once at app start, deinit once at app end.** Not per-window.

- **Per-window Metal layer.** `RGFW_getLayer_OSX()` mints a fresh `CAMetalLayer`;
  `RGFW_window_setLayer_OSX(win, layer)` attaches it to a specific window. So every
  window can own its own layer → its own `wgpu::Surface`. This is exactly what
  `attachMetalLayer()` in `braid_app.cpp` already does — it just needs to run once
  per window instead of once per process.

- **One shared event queue, drained per window.** `RGFW_window_checkEvent(win,&ev)`
  is *lazy*: on the first call of a frame it flips `queueEvents = TRUE`, calls
  `RGFW_pollEvents()` once (which pumps **all** OS events for **all** windows into a
  shared queue, `RGFW_MAX_EVENTS = 32`), then `RGFW_window_checkQueuedEvent(win,&ev)`
  pops only the events belonging to `win`. When a window drains its slice it resets
  `polledEvents = FALSE`.

  **Consequence:** the current per-window `while (RGFW_window_checkEvent(win,&ev))`
  loop is fine for *one* window but races for *N* — each window would re-trigger a
  global poll, and the 32-slot queue can drop events for windows that haven't drained
  yet. The correct multi-window pattern RGFW documents is:

  ```c
  RGFW_pollEvents();                                  // once per frame, globally
  for (each window w)
      while (RGFW_window_checkQueuedEvent(w, &ev))    // dispatch w's slice
          handle(w, ev);
  ```

  → **the run loop must move up to an application level**: poll once, dispatch to
  each window, then render each window.

**Net:** RGFW imposes no per-window context. What blocks Braid today is purely how
`App` fuses *library init + device + window + loop* into one object — and that
`App::~App()` calls `RGFW_deinit()`, so destroying any window kills windowing for
all of them.

---

## 2. Sharing the WebGPU context is the easy half — it's *one device for everything*

The OpenGL reflex is "shared contexts / share groups so textures cross windows."
**WebGPU has no per-window context.** The `wgpu::Device` *is* the context, and it is
not tied to any surface. So:

> **Share = create one `wgpu::Instance` + `wgpu::Adapter` + `wgpu::Device`, and use
> it for every window.** Every texture, buffer, pipeline, bind group, and the whole
> `detail::Compositor` already lives on the device and works with any surface
> configured on that same device. Nothing special to "share."

Per **window** you still need its own *swapchain*:
- a `wgpu::Surface` from that window's `CAMetalLayer`,
- `surface.Configure({device, format, w, h, presentMode})`,
- a swapchain-wrapping `braid::Surface` (present target) + a persistent offscreen
  `braid::Surface` (the 16F draw target) — same pair `App` builds now, just one pair
  per window.

Caveats worth stating up front:
- **Adapter/`compatibleSurface`.** `initWebGPU()` requests the adapter with
  `compatibleSurface = surface_`. Request the adapter **once**, against the first
  window's surface; the resulting device drives all other same-GPU windows. On
  macOS/Metal there's a single GPU path so this is a non-issue; multi-GPU (two
  adapters) is explicitly out of scope for v1 — one device, N windows.
- **Compositor cache is device-scoped, not window-scoped.** `detail::Compositor`
  keys pipelines on `(format, blend)`. Two windows with the *same* swapchain format
  share cache entries for free; a window with a different format just adds entries.
  No change needed — it already behaves correctly across windows.
- **This means the current global singletons are only *half* wrong.** `detail::g_ctx`
  (instance+device) and `detail::g_compositor` model "one shared device" — which is
  *exactly right* for the one-device/N-window case. They are wrong for (a) multi-device,
  (b) deterministic teardown, and (c) unit testing (roadmap #4). So the multi-window
  refactor does **not** force us to de-singleton the device; it forces us to
  de-singleton the **window**.

### The one genuinely shared-state hazard: `detail::ctx().currentPass`

`braid_detail.h`'s `Context` carries `wgpu::RenderPassEncoder* currentPass`, set by
`SketchApp` during `draw()` so addons (e.g. braid-syntype) can draw into "the current
pass." With N windows each opening their own pass, a single global `currentPass` is
ambiguous. Options, cheapest first:
1. **It's fine for now** — windows render sequentially on the main thread, so
   `currentPass` is only ever set for the window currently in `draw()`; set it on
   pass-open, clear on pass-close (SketchApp already does exactly this). As long as
   we never interleave two open passes, the global stays correct.
2. Move `currentPass` onto the per-window object and hand addons the target
   explicitly. Cleaner; do it if/when addons need to target a specific window.

→ v1 can keep option 1 (no addon API change) as long as the loop renders windows
one at a time, which it does.

---

## 3. Proposed refactor — split *application lifetime* from *window lifetime*

Matches roadmap #1's bullets. Two shapes; recommendation follows.

### Shape A — `Application` owns `Window`s (the "correct" long-term form)

```cpp
class Window {                     // per-window: RGFW window + surfaces + hooks
public:
    struct Settings { int width, height; const char* title; /* … */ };
    virtual void setup() {}
    virtual void update() {}
    virtual void draw() {}
    // events, surface(), width()/height(), mousePos()… (what App exposes today)
protected:
    void* rgfwWindow_ = nullptr;
    wgpu::Surface surface_;                 // this window's swapchain
    std::optional<Surface> swapSurface_, mainSurface_;
    // borrows the shared device from its Application
};

class Application {                 // app lifetime: RGFW init + device + loop
public:
    Result<void> init();           // RGFW_init once; create Instance/Adapter/Device
    template <class W, class... A> W& add(A&&... args);   // make a Window, build its surface
    Result<void> run();            // poll once → dispatch per window → render each
    ~Application();                 // RGFW_deinit once; destroy device last
private:
    wgpu::Instance instance_; wgpu::Adapter adapter_; wgpu::Device device_;
    std::vector<std::unique_ptr<Window>> windows_;
};
```

Loop body:
```cpp
timer_.waitNext();
RGFW_pollEvents();                              // once, globally
for (auto& w : windows_) w->pumpQueued();       // RGFW_window_checkQueuedEvent(w, …)
for (auto& w : windows_) w->update();
for (auto& w : windows_) if (w->beginFrame()) { w->beforeDraw(); w->draw();
                                                w->afterDraw(); w->endFrame(); }
windows_.erase(closed…);                        // a closed window leaves; app runs on
```

- Device/instance/adapter created **once** by `Application`; `detail::setContext`
  called once; `detail::compositor()` shared. First window's surface used for the
  adapter request.
- `~Window` closes just its `RGFW_window`; only `~Application` calls `RGFW_deinit()`.

### Shape B — keep `App` as the primary window, spawn secondaries

`App` stays the entry point and owns the device (as today); add
`App::createWindow(settings) -> Window&` that shares `App`'s device and registers
into `App`'s loop. **Backward-compatible**: every existing example (all subclass
`App`/`SketchApp`) compiles and runs unchanged; multi-window is opt-in.

### Recommendation

Ship the **guts of A** but keep a **B-shaped façade** so nothing breaks:

1. Extract an internal `DeviceContext` (instance/adapter/device/queue + `setContext`)
   and pull `RGFW_init`/`RGFW_deinit` and the run loop out of `App`. This is also
   roadmap #4 (reduce global state) — the device becomes *owned*, borrowed by
   surfaces, instead of reached through a global.
2. Reparent the current `App` members (`window_`, `surface_`, `swapSurface_`,
   `mainSurface_`, event channels, `draw()` hooks) onto a `Window` base. `App`
   becomes "the process's first `Window` + the owner of the `DeviceContext`," so
   `class SketchApp : public App` and every example keep working verbatim.
3. Add `App::createWindow<T>(settings)` for secondaries; they borrow the device and
   join the loop.
4. Fix the teardown ordering bug now regardless: **only the last window / the app
   owner calls `RGFW_deinit()`**, and the device outlives all surfaces.

This lands multi-window without a breaking API change, which is the roadmap's whole
posture ("no public API change" for the split; keep the examples runnable).

---

## 4. Concrete edits when we implement (checklist, not code)

- `braid_app.cpp`: move `RGFW_init` out of `initWindow()` and `RGFW_deinit` out of
  `~App` into app-lifetime scope; make the metal-layer attach + surface build + swap/
  main `Surface` pair a per-window routine.
- Loop: one `RGFW_pollEvents()` per frame; per-window `RGFW_window_checkQueuedEvent`
  dispatch (replaces per-window `RGFW_window_checkEvent`).
- Per window: own `wgpu::Surface`, `configureSurface()`, `beginFrame/endFrame`,
  resize handling (`RGFW_windowResized` already carries the window; route to the
  matching `Window`). The end-of-frame blit (`mainSurface_ → swapSurface_`) is
  per window.
- Keep `detail::g_ctx` / `detail::g_compositor` shared (correct for one device);
  note the de-singleton for multi-device/testing as a *separate* later step (#4).
- Decide `currentPass`: keep the single global (safe while windows render serially)
  or move per-window if an addon needs to target a specific window.
- `chalet.yaml`: add `multiwindow` executable target (braid-core only).

## 5. Proof (the demo that closes the milestone)

`examples/multiwindow.cpp`: open **two** windows from one `main()`, each its own
sketch (e.g. one running the `feedback` tunnel, one running `cubes`), both rendering
independently at 60fps off **one shared device**. Closing one window leaves the other
running; closing the last exits. Screenshot both → proof the shared context works.

---

## 6. Multi-monitor spanning — one window, several physical displays

Real ask (grounded in a prior openFrameworks rig, `TheOne/src/main.cpp` +
`ofAppGLFWWindow.h/.cpp`): one process, a normal window for control/preview, and a
**second window that spans an arbitrary set of monitors** (e.g. indices 2,3,4,5) as
one continuous borderless canvas — the "output" surface for an installation.

**What oF does (GLFW):** `ofWindowSettings::shareContextWith` passes the first
window's `GLFWwindow*` into `glfwCreateWindow`'s `share` param so the second
window's GL context can see the first's textures/programs — GLFW/OpenGL contexts
don't share by default. `fullscreenDisplays = {1,2,3}` is a list of monitor
indices; `ofMonitors::getRectFromMonitors()` unions their rects (each monitor's
`{x, y, width, height}` in the shared virtual-desktop space, from
`glfwGetMonitorPos`/`glfwGetVideoMode`), and on macOS the window is simply made
`NSWindowStyleMaskBorderless` and resized/moved to that union rect — not
`glfwSetWindowMonitor` (that path is Windows/Linux/game-mode only).

**Why Braid needs less:** there is no `shareContextWith` equivalent to build —
every window already shares the one process-wide `wgpu::Device` unconditionally
(§2 above). The only real gap was monitor geometry + absolute placement, and
RGFW already has it (verified in `libs/macos/include/RGFW.h`):

- `RGFW_getMonitors(&count)` → `RGFW_monitor**`, each with `.x, .y` and
  `.mode.w, .mode.h` — the same shared-virtual-desktop-space rect data as oF's
  `ofMonitors::rects`. The outer array is heap-allocated (`RGFW_FREE` it); the
  `RGFW_monitor*` entries are owned by RGFW's own monitor list.
- `RGFW_windowNoBorder` — passed to `RGFW_createWindow`, achieves a borderless
  window on macOS via `RGFW_window_setBorder(win, 0)` right after creation
  (`setStyleMask:NSWindowStyleMaskBorderless` — confirmed in the header, not
  just inferred).
- Passing explicit `x, y` to `RGFW_createWindow` (previously always `0,0` plus
  `RGFW_windowCenter`) positions the window directly — Cocoa's
  `initWithContentRect:` uses `win->x`/`win->y` as given when `RGFW_windowCenter`
  isn't requested.

**Design landed** (`core/braid.h` + `core/braid_app.cpp`): a `Monitors::list()` /
`Monitors::unionOf(indices)` pair (a renamed, RGFW-backed port of `ofMonitors`),
plus `AppSettings::position` (explicit placement) and `AppSettings::monitors`
(span these indices as one borderless window — overrides width/height/position).
`Window::create()` picks, in order: `monitors` (union rect, borderless) →
`position` (explicit, no centering) → the original centered default. Secondaries
with neither set no longer default to dead-center-on-the-primary (the overlap
bug from the plan doc's review) — they default onto the next connected monitor.

Usage, mirroring the oF example:
```cpp
braid::App::Settings out{};
out.title = "output";
out.monitors = {2, 3, 4, 5};   // union of these four screens, borderless
app.createWindow<OutputWindow>(out);
```

Not yet verified on real hardware — the union-rect math and the borderless flag
have only been checked by reading the code and a single/dual-monitor smoke run
(process stays alive, registers as a foreground app, no crash). See the plan
doc's "Follow-up work" for what a real multi-monitor pass should confirm.

---

## TL;DR
- **RGFW:** multi-window works; init/deinit are global (do them once), and events go
  through one shared queue you must drain per-window after a single `RGFW_pollEvents()`.
  Each window gets its own `CAMetalLayer` → its own `wgpu::Surface`.
- **WebGPU:** "share context" = **one `Device` for all windows**; there is no
  per-window context. Per-window is just a surface + swapchain.
- **Braid:** the blocker isn't the shared device (the current singletons already model
  that correctly) — it's that `App` fuses library-init + device + window + loop.
  Split *application lifetime* from *window lifetime*, keep the device shared, and
  keep `App` as the first window so no example breaks. End in `examples/multiwindow.cpp`.
