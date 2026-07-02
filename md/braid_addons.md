# Braid Addons — Architecture Plan

**Status:** design-in-progress.  
**Scope:** how optional functionality is added to Braid without bloating core.

Braid has a small, opinionated core: `Surface` + algebra + the loop. Everything that depends on the core but that the core does not depend on is an addon. This doc defines the two addon shapes and how they plug in.

---

## 1. Core principle

> **If a feature depends on `Surface` but `Surface` does not depend on it, it is an addon.**

WebGPU/Dawn fails this test — the core depends on it — so it is the floor, not an addon. Images, text, audio, MIDI, video, UI, etc., all pass the test.

This mirrors the existing split:

| Layer | What lives there | Addon? |
|---|---|---|
| Platform | RGFW + Dawn (window + device) | No — floor |
| Core | `Surface`, algebra, feedback, compositor, loop | No — identity |
| Compiled addon | `Surface::load/save` (braid-image), `Syntype` text (braid-syntype) | Yes |
| Runtime addon | TinyUI, future audio/MIDI/OSC services | Yes |

---

## 2. Two kinds of addons

### 2.1 Link-to-enable compiled addons

These are static libraries that add functionality by defining symbols the core declares but does not define. They are linked only by sketches that need them.

- **braid-image** defines `Surface::load()` / `Surface::save()` using the mango codec stack. The core declares these methods in `braid.h`; the addon provides the implementation.
- **braid-syntype** provides stick-letter text rendering via `braid_syntype.h`.

Pattern:

```cpp
// braid.h — declared in core, defined nowhere in core
class Surface {
public:
    static Result<Surface> load(const char* path);
    Result<void> save(const char* path) const;
};
```

```cpp
// addons/braid-image/braid_image.cpp — defined in the addon
Result<Surface> Surface::load(const char* path) { /* mango decode */ }
Result<void> Surface::save(const char* path) const { /* mango encode */ }
```

Build integration in `chalet.yaml`:

```yaml
braid-image:
  kind: staticLibrary
  files:
    include:
      - core/braid_image.cpp

image:
  kind: executable
  staticLinks:
    - braid-core
    - braid-image
    - libs/macos/lib/libmango.a
    # ... codec archives
```

Sketches that do not link `braid-image` never pay for mango.

### 2.2 Runtime stateful addons

These are objects the sketch creates and attaches to a `Window`. They receive lifecycle hooks and can subscribe to event channels. TinyUI is the first example.

Pattern:

```cpp
namespace braid {

class Addon {
public:
    virtual ~Addon() = default;
    virtual void setup(Window&) {}
    virtual void update(Window&) {}
    virtual void draw(Window&) {}
    virtual int priority() const { return 0; }  // lower = earlier
};

class Window {
public:
    void addAddon(Addon& addon);          // sketch owns lifetime
    void addAddon(std::shared_ptr<Addon>); // shared ownership
};

} // namespace braid
```

The application loop calls addons in priority order:

```cpp
// during update phase
for (auto& a : sorted(window.addons_)) a->update(window);
window.update();

// during draw phase
window.beforeDraw();
window.draw();
for (auto& a : sorted(window.addons_)) a->draw(window);  // UI draws on top
window.afterDraw();
```

Addons can also subscribe to the window's `Channel<T>` queues:

```cpp
class MyAddon : public braid::Addon {
    braid::Subscription sub_;
public:
    void setup(braid::Window& w) override {
        sub_ = w.mouseEvents.subscribe([](braid::MouseEvent e) {
            // handle mouse
        });
    }
};
```

---

## 3. How addons reach core internals

Compiled addons sometimes need the shared WebGPU instance/device/queue or the internal compositor. The seam is `core/braid_detail.h`:

```cpp
namespace braid::detail {

Context& ctx();
Compositor& compositor();
void setContext(wgpu::Instance, wgpu::Device);

} // namespace braid::detail
```

Addons include `braid.h` for the public API and `braid_detail.h` for the internal seam. This is intentionally narrow — addons should not depend on RGFW or platform internals.

---

## 4. Addon lifecycle

### 4.1 Compiled addon

- **Compile time:** linked if the sketch's target lists it in `staticLinks`.
- **Runtime:** functions become available as soon as the program starts; no registration needed.
- **Cleanup:** tied to object lifetimes (e.g., `Surface` owns its texture).

### 4.2 Runtime addon

- **Construction:** sketch creates the addon (e.g., `braid::TinyUI ui{"show.txt"};`).
- **Registration:** sketch calls `window.addAddon(ui)` (usually in `setup()`).
- **Setup:** `Addon::setup(Window&)` is called by the application loop on the first frame.
- **Update/draw:** called every frame in priority order.
- **Events:** subscriptions live until the addon is destroyed.
- **Destruction:** if the sketch owns the addon, it is destroyed when the sketch is. If `shared_ptr`, destruction is automatic.

---

## 5. Window-scoped vs Application-scoped

Addons are **window-scoped by default**. A UI addon belongs to the control window because it needs that window's surface and mouse coordinates. This is the right default even when the sketch has only one meaningful window.

Future **Application-scoped services** (audio input, OSC, MIDI, recorder) may be added if they genuinely need to exist before any window or be shared across windows without a surface. They would live on `Application`, not `Window`. For now, no such service exists, so the API is not invented prematurely.

---

## 6. Current and planned addons

| Addon | Kind | Status | Notes |
|---|---|---|---|
| braid-image | compiled | ✅ done | `Surface::load/save` via mango |
| braid-syntype | compiled | ✅ done | stick-letter text rendering |
| braid-tinyui | runtime | design | first `Addon` base class user |
| braid-audio | compiled + runtime | future | likely needs a service object for capture |
| braid-midi | compiled + runtime | future | service object + channel of events |
| braid-osc | compiled + runtime | future | `OscIn`/`OscOut` over `Channel<T>` |

---

## 7. Design rules

1. **Core stays small.** If a feature can be an addon, it is.
2. **No global addon registry.** Addons register on a `Window` or are linked explicitly.
3. **No reflection / plugin loading.** Addons are normal C++ libraries linked at build time.
4. **Addons don't see RGFW.** The windowing seam is `braid::Window`; raw RGFW stays inside `core/braid_app.cpp`.
5. **Prefer window-scoped.** Only introduce Application-scoped services when a feature truly needs it.

---

## 8. Example: a runtime addon skeleton

```cpp
// addons/braid-myaddon/braid_myaddon.h
#pragma once
#include "braid.h"

namespace braid {

class MyAddon : public Addon {
public:
    explicit MyAddon(const std::string& config);

    void setup(Window&) override;
    void update(Window&) override;
    void draw(Window&) override;

private:
    braid::Subscription mouseSub_;
};

} // namespace braid
```

```cpp
// sketch
class Sketch : public braid::SketchApp {
    braid::MyAddon addon{"config.txt"};
public:
    using SketchApp::SketchApp;
    void setup() override { addAddon(addon); }
};
```

---

## 9. Implementation caveats

These are not blockers, but decisions to make explicitly while the first runtime addon is being written.

1. **Prefer `shared_ptr` for `addAddon`.** The raw-reference overload `void addAddon(Addon&)` is convenient for sketch-member addons, but it silently dangles if a stack-local addon is passed in. Make `std::shared_ptr<Addon>` the primary API; keep the raw reference only for the "sketch member, lifetime guaranteed" case.

2. **Compiled addons can add new classes too.** The `braid-image` example shows adding method definitions to an existing core class, but `braid-syntype` adds an entirely new class (`Syntype`). Both patterns are valid compiled-addon shapes.

3. **`priority()` may not survive a second runtime addon.** It works for TinyUI, but an addon that must draw *before* the sketch (e.g., a background grid) cannot express that with a single scalar. Expect to replace `priority()` with draw phases (`Background`, `Scene`, `Overlay`) once a second addon needs it.

4. **"Compiled + runtime" addons need clarification.** Audio/MIDI/OSC will likely ship as a compiled TU (driver/codec) plus a runtime service object. Don't mix the two shapes in one public class; keep the driver compiled and the service runtime.

---

## 10. Relation to UI.md

`md/ui.md` is the first concrete runtime-addon design. It expands the minimal `Addon` base shown here into a working UI. The two docs should stay consistent: `braid_addons.md` defines the generic mechanism; `ui.md` defines the first consumer.
