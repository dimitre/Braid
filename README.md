# Braid

Imagined by Dimitre after working more than one decade with openFrameworks and collaborating in Core development.
It derives a lot of ideas from ofWorks (openFrameworks fork).  
After ofxDawn made possible for ofWorks with the help of Claude Opus 4.8 and Kimi 2.7 I started to discuss the usefulness of ofFbo in openFrameworks system and how to make a very compact framework of itself only based on WebGPU and modern C++
Specification was discussed over a weekend (27, 28 june 2026) and LLM assisted coding soon after.

> [!WARNING]
> API drift. some things can change soon


Braid is a **WebGPU creative-coding framework for macOS**. It is small, exposes a single public header (`#include "braid.h"`), and is built on a single unifying idea: **a `Surface` is the only thing you draw into.**

The screen is a Surface. An offscreen buffer is a Surface. An image is a Surface. They all compose with the same algebra — add, blend, zoom, rotate, shift, invert — so layers, feedback loops, and post-processing are all one vocabulary, not three different APIs.

---

## The two tiers

Braid gives you two ways to work, but one shared vocabulary.

### Sketch tier — `SketchApp`

Processing-like: set state, draw primitives. This is where you start.

```cpp
#include "braid.h"

class MySketch : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;

    void draw() override {
        background(0.02f, 0.02f, 0.03f);
        fill(1.0f, 0.6f, 0.2f);
        circle(width() * 0.5f, height() * 0.5f, 80.0f);
    }
};

int main() {
    braid::App::Settings s{.title = "hello", .width = 900, .height = 900};
    MySketch app(s);
    return app.run() ? 0 : 1;
}
```

### Explicit tier — `App`

Drop down to raw `Shader` + `Mesh` + render passes when you need a custom pipeline. Same `Surface` type, same composability.

---

## Surface algebra (the whole idea)

Surfaces are elastic. You can treat whole layers like numbers.

```cpp
s += other;                 // additive composite (energy adds)
s.over(other);              // alpha composite (other over s)
s.zoom(1.02f);              // magnify toward center
s.rotate(0.0016f);          // radians, about center
s.shift(dx, dy);            // pixels
s.invert();                 // rgb -> 1-rgb
s.multiply({r,g,b,a});      // per-channel gain/tint
```

### Self-feedback (the ouroboros)

One Surface feeding itself. `feedback(gain, transform)` applies a transform to the current contents, then decays by `gain`. The double-buffer is hidden — one knob.

```cpp
surface().feedback(0.99f, [](braid::Surface& s) {
    s.zoom(1.02f);
    s.rotate(0.0016f);
    s.invert();
});
```

---

## Addons are separate

`braid-core` = Surface + algebra + the loop. Everything that flows **into or out of** a Surface is an addon you link only when you need it.

- `braid-image` — `Surface::load("photo.png")` and `surface().save("frame.png")` via the mango codec stack.
- (More addons later: audio, video, …)

---

## Build

Uses [chalet](https://chalet-work.space) (config in `chalet.yaml`).

```bash
chalet build                 # build everything
chalet buildrun playground   # build + run the scratch canvas
chalet buildrun feedback     # build + run the feedback demo
```

---

## Stack

- **Dawn** — WebGPU implementation (tag `add` from ofWorks/ofLibs)
- **RGFW** — single-header windowing
- **glm** — math
- **fmt** — formatting
- **mango** — image codecs (via `braid-image` addon)

---

## Design notes

- **16-bit float Surfaces** by default (smooth feedback, HDR headroom). The swapchain is 8-bit BGRA — conversion is automatic.
- **Custom WGSL clip space is `[0,1]`** (Metal/D3D/Vulkan style, not GL's `[-1,1]`). `glm` is already configured for this.
- **Move-only GPU resources** — `std::move` them, store in `std::optional`, or `clone()` for an explicit GPU copy.
- **Fallible functions return `Result<T>`** — expressive things never fail; resource things can.
