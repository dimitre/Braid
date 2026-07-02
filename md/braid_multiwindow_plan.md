# Plan — Multi-window support for Braid (roadmap #1)

**Status:** implemented 2026-07-01.  
**One deviation from the original plan:** `Window` had to be declared in the public header (`core/braid.h`) because C++ requires a base class to be complete at the point of class definition; `App` and `SketchApp` inherit from it. The implementation details of `Application` and `DeviceContext` remain internal to `core/braid_app.cpp`.

## Goal
Enable multiple RGFW/WebGPU windows from one process, sharing a single Dawn device, without breaking any existing example. Close with a runnable `examples/multiwindow.cpp` demo where two windows render independently at 60 fps.

## Completion criterion
- `chalet build` succeeds for braid-core + all existing examples (no public API breaks).
- New `multiwindow` target builds and runs: two windows, each with its own sketch, both rendering; closing one keeps the other alive; closing the last exits cleanly.
- No validation errors or crashes during create/resize/destroy.

---

## Current state

- `core/braid_app.cpp` fuses four lifetimes in `App`:
  1. RGFW library init/deinit (`RGFW_init` in `initWindow()`, `RGFW_deinit` in `~App`).
  2. WebGPU instance/adapter/device creation (`initWebGPU()`).
  3. One RGFW window + one `wgpu::Surface` + one swap/main `Surface` pair.
  4. The run loop (`run()`).
- `App::run()` pumps events with `RGFW_window_checkEvent`, which calls `RGFW_pollEvents()` lazily per window. With N windows this re-polls globally and can drop events from the shared 32-slot queue.
- `detail::Context` and `detail::Compositor` are process-wide singletons. For the one-device/N-windows case this is actually correct; the refactor only needs to de-singleton the **window**.
- `SketchApp` subclasses `App`; existing examples rely on this.

## Design — split application lifetime from window lifetime, keep App as first window

Ship the **guts of the Application/Window split** behind a **backward-compatible App façade** so nothing breaks.

### New internal types in `core/braid_app.cpp`

1. **`struct DeviceContext`** — owns `wgpu::Instance`, `wgpu::Adapter`, `wgpu::Device`, `wgpu::Queue`.
   - Created once at app start.
   - Calls `detail::setContext(instance, device)` once.
   - Destroyed after all windows/surfaces are gone.
   - First window's Metal layer is used as the `compatibleSurface` for adapter request (existing logic).

2. **`class Window`** — per-window state, internal to `braid_app.cpp`.
   - Owns `RGFW_window*`, `wgpu::Surface`, `swapSurface_`, `mainSurface_`, per-frame encoder/view.
   - Owns event channels, `mousePos_`, `running_` flag, virtual hooks (`setup/update/draw/exit`, event callbacks).
   - Has `pumpQueued()`, `update()`, `beginFrame()`, `endFrame()`, `close()`.
   - Has `createSurface()` (Metal layer attach + `wgpu::Surface` build) and `configureSurface()`.
   - Resize routes to the matching `Window` via the `RGFW_window*` pointer.

3. **`class Application`** — owns one `DeviceContext` and a vector of `std::unique_ptr<Window>`.
   - `init()`: `RGFW_init` once; create device; set context.
   - `run()`: the global loop:
     ```
     timer.waitNext()
     RGFW_pollEvents()                         // once per frame, globally
     for each window: w->pumpQueued()          // drain queued events for that window
     for each window: w->setupOnce(); w->update()
     for each window: w->beginFrame(); w->beforeDraw(); w->draw(); w->afterDraw(); w->endFrame()
     erase windows whose shouldClose is true
     ```
   - `~Application()`: destroy windows, then device context, then `RGFW_deinit()` once.
   - `addWindow<T>(settings, args...)` constructs a `T` (must derive from `Window`), builds its RGFW window + surface, and returns `T&`.

4. **`class App` becomes a backwards-compatible subclass of `Window` that also hosts the `Application`.**
   - `App` accesses a process-wide `Application` singleton created lazily on first `run()`/`createWindow()`.
   - `App::run()` registers itself as the primary window with the application, then runs the app loop.
   - `App::close()` marks this window closed; when the last window closes, the loop exits.

### Public API surface

- No breaking changes to `App` or `SketchApp`.
- Add `App::createWindow<T>(const Settings&, Args&&...)` template so a sketch can spawn secondaries:
  ```cpp
  auto& win2 = app.createWindow<SketchApp>({"second", 640, 480});
  ```
- New internal `Window` base is not in `braid.h` initially; keep it in `braid_app.cpp` to minimize scope.

### Event loop changes

- Replace per-window `RGFW_window_checkEvent` with one global `RGFW_pollEvents()` followed by per-window `RGFW_window_checkQueuedEvent`.
- Map `RGFW_windowResized` to the correct `Window` by matching the event's window pointer; fall back to a global `RGFW_window* → Window*` map if needed.
- Each `Window` drains its own `Channel`s at the end of `pumpQueued()`.

### `detail::Context` / `Compositor`

- Keep the singletons for v1. They model "one device shared across windows," which is correct.
- `currentPass` stays safe because windows render sequentially on the main thread.

---

## Step-by-step implementation

### Step 1 — Introduce `Window` base class in `core/braid_app.cpp`
Move per-window members out of `App` into a new `Window` class:
- `void* window_` (RGFW window)
- `wgpu::Surface surface_`
- `std::optional<Surface> swapSurface_, mainSurface_`
- `wgpu::CommandEncoder frameEncoder_`, `wgpu::TextureView frameView_`
- `Settings settings_` (per-window copy)
- `glm::vec2 mousePos_`
- `bool running_`
- Event channels

Move per-window methods onto `Window`:
- `create()` (was `initWindow()`)
- `createSurface()` / `configureSurface()`
- `pumpQueued()` (was `pumpEvents()`)
- `beginFrame()` / `endFrame()`
- Virtual hooks and accessors

`Window` holds a borrowed `Application&` (or `DeviceContext&`) for device/queue/timer access.

### Step 2 — Introduce `DeviceContext` in `core/braid_app.cpp`
- Encapsulate instance/adapter/device/queue creation.
- Constructor takes the first window's `CAMetalLayer` for `compatibleSurface`.
- Call `detail::setContext(instance, device)` after device creation.

### Step 3 — Introduce `Application` in `core/braid_app.cpp`
- Members: `std::unique_ptr<DeviceContext> ctx_`, `std::vector<std::unique_ptr<Window>> windows_`, `Timer timer_`, `bool running_`.
- `Result<void> init()`: `RGFW_init` once.
- `Result<void> run()`: create device on first call, then global loop.
- Template `addWindow<T, Args...>(settings, args...)` constructs window, creates RGFW window + surface, adds to vector, returns `T&`.
- `registerPrimaryWindow(Window*)` for the existing `App` self-registration pattern.
- Loop body:
  1. `timer_.waitNext()`
  2. `RGFW_pollEvents()` once
  3. Per-window `pumpQueued()` + shouldClose check
  4. Erase closed windows; exit if none left
  5. Per-window `setup()` (once) + `update()`
  6. Per-window `beginFrame()` → `beforeDraw()` → `draw()` → `afterDraw()` → `endFrame()`
  7. `ctx_->instance().ProcessEvents()`
- `~Application()`: destroy windows, then `ctx_`, then `RGFW_deinit()`.

### Step 4 — Refactor `App` to be a `Window` facade
- `class App : public Window` internally; public declaration in `braid.h` unchanged.
- `App` accesses a lazily-created process-wide `Application` singleton.
- `App::run()` registers `this` as primary window and runs the application loop.
- `App::~App()` no longer calls `RGFW_deinit` or closes the window.
- Keep all existing accessors (`device()`, `queue()`, `surface()`, `width()`, `mouseX()`, etc.) working as aliases.

### Step 5 — Add `App::createWindow<T>()`
- Declare template in `braid.h`:
  ```cpp
  template <class W, class... Args>
  W& createWindow(const Settings& s, Args&&... args);
  ```
- Implement in `braid_app.cpp`; explicitly instantiate for `Window`, `App`, `SketchApp`.

### Step 6 — Fix resize routing
- Use the event's window pointer to route `RGFW_windowResized` to the correct `Window`.
- Fall back to a global `RGFW_window* → Window*` map if RGFW does not expose the window pointer in events.

### Step 7 — Move `setWindowTitle` helper
- Keep the Objective-C autorelease-pool trick; move it onto `Window`.

### Step 8 — Update `braid.h`
- Add the `createWindow` template declaration only.

### Step 9 — Add `examples/multiwindow.cpp`
- Two `SketchApp`-derived windows:
  - Primary: feedback tunnel (from `examples/feedback.cpp`).
  - Secondary: rotating wireframe cubes (from `examples/cubes.cpp`) at smaller size.
- `main()`:
  ```cpp
  struct MainWin : SketchApp { ... };
  struct SideWin : SketchApp { ... };
  MainWin app;
  auto& side = app.createWindow<SideWin>({640, 480, "Side"});
  return app.run() ? 0 : 1;
  ```

### Step 10 — Add `multiwindow` target to `chalet.yaml`
- Copy the `cubes` target block, name it `multiwindow`, file `examples/multiwindow.cpp`, links `braid-core` only.

### Step 11 — Build and verify
- `chalet build` all targets.
- Run `multiwindow`: confirm two windows, independent rendering, clean close.
- Run existing `cubes`, `feedback`, `sketch`, `hello`, `bloom`, `image` to ensure no regressions.
- Check with validation enabled (debug build).

---

## Key correctness constraints

1. **RGFW init/deinit once.** `Application` owns both; `~Window` must not call `RGFW_deinit`.
2. **Device outlives surfaces.** Destroy all `Surface` objects before destroying the `wgpu::Device`.
3. **One global `RGFW_pollEvents()` per frame.** Never let each window re-trigger a poll.
4. **Per-window swapchain config.** Each `wgpu::Surface` is configured with that window's current size.
5. **No public API break.** Existing `App a; a.run();` and `class MyApp : public SketchApp` continue to compile.

---

## Testing / verification

- Build: `chalet build`.
- Run new demo: `chalet run multiwindow`.
- Regression run: `chalet run hello`, `chalet run sketch`, `chalet run cubes`, `chalet run feedback`, `chalet run bloom`, `chalet run image`.
- Visual check: both windows render at ~60 fps; closing one leaves the other; closing last exits; no validation spam in debug build.

---

## Risks and mitigations

| Risk | Mitigation |
|------|------------|
| `App` object lifetime is awkward because it is both window and caller. | Use `Application::registerPrimaryWindow(this)` so the existing `App` instance becomes the first window; no slicing. |
| RGFW event `window` field missing. | Build a `RGFW_window* → Window*` map; look up in pump. |
| Device destroyed while surfaces still alive. | `Application` destructor destroys windows vector first, then `DeviceContext`. |
| Static/global `Application` singleton causes order issues. | Lazily create inside `App::application()` on first `run()`/`createWindow()` call. |
| `SketchApp` batch state accidentally shared across windows. | Batch state is per-`SketchApp` instance; each window has its own `SketchApp`. |

---

## Status: implemented (Kimi's pass + follow-up fixes)

Kimi landed the `Application`/`Window`/`App` split described above, including the
`RGFW_window* → Window*` map (the plan's own fallback for event routing, used
unconditionally rather than relying on `RGFW_window_checkQueuedEvent`'s internal
per-window scan). A review pass on top found three real bugs, fixed below —
plus the multi-monitor spanning feature this doc didn't originally scope.

### RGFW-level fix: windows always-on-top

Independent of the bugs below: `libs/macos/include/RGFW.h`'s
`RGFW_window_raise()` (called by `Window::create()` for every window) passed
`kCGNormalWindowLevelKey` to Cocoa's `-setLevel:`. That constant is a
`CGWindowLevelKey` lookup-table *index* (value 4), not a real `CGWindowLevel` —
the real "normal" level is `kCGNormalWindowLevel` (0). Every Braid window has
therefore been opening one level above normal, i.e. always-on-top over other
apps. Same root cause in `RGFW_window_setFloating`/`RGFW_window_isFloating`.
Fixed by using the real level constants (`kCGNormalWindowLevel`,
`kCGStatusWindowLevel`) directly — matches a fix the user filed upstream with
RGFW's maintainer (`@ColleagueRiley`) the same day, found independently via
their own `openFrameworks`-on-RGFW fork.

### Bugs found and fixed

1. **Closing the primary killed every window, contradicting the milestone's own
   demo comment** ("press Q to close just this window" on the *primary*, which
   actually ended the whole app). `Application::run()` special-cased
   `primary_->shouldClose()` as a hard `running_ = false`, ignoring open
   secondaries. Fixed: any window (primary or secondary) can now close
   independently; the app only exits once none are left. Since the primary's
   `Window` object is caller-owned (usually the `App` on `main()`'s stack, not
   heap-owned by `Application` the way secondaries are), it can't be erased from
   a vector — instead `Window::closeNative()` (new) releases its RGFW window and
   GPU surfaces immediately and a `primaryClosed_` flag tells the loop to stop
   touching it, without destroying the C++ object.
2. **Primary-window surfaces were destroyed after the device**, contradicting
   this doc's own constraint #2 ("device outlives surfaces"). Because `App`'s
   `Application` is a *derived-class* member while the surfaces live on the
   *base* `Window`, plain C++ destruction order tears down `app_` (device
   included) before the base class's `swapSurface_`/`mainSurface_` — backwards
   for the primary specifically (secondaries were already fine, since
   `secondaries_.clear()` runs before `ctx_.reset()`). Fixed by having
   `Window::closeNative()` explicitly release surfaces up front, called from
   `App::~App()` before its `app_` member is destroyed.
3. **Per-window `targetFps` was dead code.** Frame pacing is one shared
   `Timer` owned by `Application`, seeded once from the *primary's*
   `AppSettings::targetFps` — a secondary's value is never read. The demo set
   `sideSettings.targetFps = 0` as if it did something; removed, and
   `AppSettings::targetFps` now says so explicitly. Per-window `vsync` *is*
   real (each window configures its own swapchain presentMode).
4. **Two windows spawned overlapping.** `Window::create()` always passed
   `RGFW_windowCenter` with hardcoded `(0,0)`, so every window — primary and
   secondary alike — centered on the same monitor. Fixed as part of the
   monitor-spanning work below: secondaries with no explicit placement now
   default onto the next connected monitor (centered on it, at their own size),
   falling back to a cascade offset on single-monitor rigs.

### Multi-monitor spanning (new capability, not in the original doc)

Requirement: one process, N windows sharing the one device (already free — see
§2 above), where a window can be positioned on an arbitrary monitor or made to
**span several monitors as one borderless canvas** — the pattern used by
`ofAppGLFWWindow`'s `fullscreenDisplays` + `shareContextWith` in openFrameworks
(see e.g. `TheOne/src/main.cpp`). Braid needs no context-sharing equivalent
(one `wgpu::Device` for every window, unconditionally); the only real gap was
monitor geometry + placement, which RGFW already exposes:

- `RGFW_getMonitors(&count)` — array of `RGFW_monitor*`, each with `.x, .y,
  .mode.w, .mode.h` in the shared virtual-desktop coordinate space (same data
  as oF's `ofMonitors::rects`).
- `RGFW_windowNoBorder` — borderless window flag (`NSWindowStyleMaskBorderless`
  on macOS, applied via `RGFW_window_setBorder` right after creation).
- `RGFW_window_move`/`RGFW_window_resize` and passing explicit `x,y` to
  `RGFW_createWindow` — absolute placement (previously unused; every window
  hardcoded `(0,0)` + center).

Added to `core/braid.h`:
```cpp
struct MonitorRect { int x, y, width, height; };
namespace Monitors {
    std::vector<MonitorRect> list();               // enumerate, RGFW order
    MonitorRect unionOf(std::span<const int> indices);  // bounding rect
}
```
and two new `AppSettings` fields:
```cpp
std::optional<glm::ivec2> position;  // explicit top-left; unset = auto-placed
std::vector<int> monitors;           // span these monitor indices as one
                                      // borderless window (e.g. {2,3,4,5})
```
`Window::create()` honors `monitors` (union rect + borderless, overrides
width/height/position) over `position` (explicit, no centering) over the
original default (centered). Implemented in `core/braid_app.cpp`
(`Monitors::list/unionOf`, `Window::create`).

### Verification

- `chalet build` succeeds for braid-core and every example target (`hello`,
  `sketch`, `feedback`, `feed`, `playground`, `cubes`, `multiwindow`, `bloom`,
  `image`) — no regressions.
- `multiwindow` launches, registers as a foreground app, and stays alive
  without crashing (checked via process state + `lsappinfo`, since this
  session's `screencapture` returns a blank image regardless of which app is
  frontmost — a permission gap in this environment, not a code issue).
- **Manually confirmed by the user (single-monitor machine, 2026-07-01):**
  the two windows spawn offset (not stacked); pressing `q` on the primary
  closes only the primary and the secondary keeps running; closing the
  secondary then exits the process cleanly. Matches the fixed behavior above.
  **Still unverified: real spanning across several physical monitors**
  (e.g. indices 2-5) — needs a multi-display rig.

---

## Follow-up work

Future multi-window work is now tracked in `md/braid_roadmap.md` §1 (Multi-window
support). The live items are:

- Dumb output window / `DisplayWindow` abstraction.
- Physical multi-monitor spanning test.
- De-singleton `detail::Context` and `detail::Compositor`.
- HiDPI / `CAMetalLayer.contentsScale`.
- One global frame pace / target FPS for the whole app (documented decision).

This file is kept as the historical implementation record for the 2026-07-01
`Application`/`Window`/`App` split.
