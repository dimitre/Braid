# AGENTS.md — Braid

Braid is a WebGPU creative-coding framework for macOS. This file is the fast path
for an agent (or a human) to write, build, and run a Braid sketch. Everything below
is real API from `braid.h` and the `examples/` — copy it.

## The one idea

**A `Surface` is the only thing you draw into.** The screen is just the Surface you
show. Images, feedback buffers, and offscreen layers are all the same `Surface` type,
so they compose with one algebra. Get this and the rest follows.

Two tiers, one vocabulary:
- **Sketch tier (`SketchApp`)** — Processing-like: `fill()`, `circle()`, `box()`,
  `pushMatrix()`. Start here. This is what 90% of sketches use.
- **Explicit tier (`App`)** — raw `Shader` + `Mesh` + render passes. Drop down only
  when you need a custom pipeline.

## Code style

- **Hard tabs, width 4** for all C/C++ source and headers. Zed is configured for this
  in `.zed/settings.json`; keep it that way.
- **Never convert existing files to spaces.** When editing a file, match whatever
  indentation it already uses; for new Braid code, use hard tabs.
- If you generate a new file or paste code that arrives with spaces, normalize it to
  hard tabs before finishing:

  ```bash
  # convert leading spaces to tabs (run from repo root)
  find core examples addons -name '*.cpp' -o -name '*.h' -o -name '*.hpp' | \
    xargs -I {} bash -c 'unexpand -t 4 "$1" > "$1.tmp" && mv "$1.tmp" "$1"' _ {}
  ```

## Minimal sketch (the skeleton to copy)

```cpp
#include <cmath>
#include "braid.h"

class MySketch : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;   // inherit constructors

    void setup() override {}             // once, before the first frame (optional)
    void draw() override {               // every frame — draw here
        background(0.02f, 0.02f, 0.03f);
        fill(1.0f, 0.6f, 0.2f);
        circle(width() * 0.5f, height() * 0.5f, 80.0f);
    }
};

int main() {
    braid::App::Settings s{};
    s.title = "my sketch";
    s.width = 900;
    s.height = 900;
    MySketch app(s);
    return app.run() ? 0 : 1;
}
```

To iterate quickly, **edit `examples/playground.cpp`** and run it — it's the scratch canvas.

## Lifecycle hooks (override what you need)

```cpp
void setup();                    // once, before frame 1
void update();                   // each frame, before draw
void draw();                     // each frame — the only one you usually need
void exit();                     // on quit
void keyPressed(braid::KeyEvent e);    // e.key (braid::Key), e.ch (char), e.shift/ctrl/alt/super
void keyReleased(braid::KeyEvent e);
void mousePressed(braid::MouseEvent e);   // e.pos, e.button
void mouseMoved(braid::MouseEvent e);
void windowResized(braid::WindowEvent e);
```

Per-frame info available anywhere: `width()`, `height()`, `mouseX()`, `mouseY()`
(or `mousePos()` as a `glm::vec2`), `elapsedTime()`, `deltaTime()`, `frameCount()`,
`currentFps()`.
`setWindowTitle("text")` is cheap (throttle it, e.g. `if (frameCount()%15==0)`).
`close()` quits. The user can always quit with **Cmd+Q** or **Esc**.

Keys: use **`e.ch`** for letter/number keys (`if (e.ch == 's')`) and **`e.key`** for
named keys (`if (e.key == braid::Key::Left)`, `Key::Space`, `Key::Escape`, …). Never
compare against raw platform keycodes — `braid::Key` is the stable, windowing-independent
contract.

## Sketch-tier vocabulary (`SketchApp`)

```cpp
// color / style (state — set it, then draw)
background(r,g,b);  background(r,g,b,a);   // clear the frame this color
fill(r,g,b,a);  noFill();
stroke(r,g,b,a);  noStroke();  strokeWeight(w);

// transform stack (like OpenGL/Processing)
pushMatrix();  popMatrix();
translate(x,y,z);  scale(s);  scale(x,y,z);
rotate(angle);                 // 2D, radians, about +Z
rotateX(a);  rotateY(a);  rotateZ(a);   // 3D, radians

// camera / projection (needed before 3D)
perspective(fovRadians, nearZ, farZ);
camera(eye, center, up);       // glm::vec3 each
ortho(left,right,bottom,top);

// 2D primitives
rect(x,y,w,h);  circle(x,y,r);  ellipse(x,y,w,h);
triangle(a,b,c);  quad(a,b,c,d);     // glm::vec2 corners
line(x1,y1,x2,y2);  point(x,y);

// 3D
box(size);  box(w,h,d);        // wireframe cube (needs a perspective camera)
```

Angles are **radians** — use `glm::radians(deg)`. Colors are floats in `[0,1]`.

## Math helpers (free functions in `braid::`)

```cpp
// re-map a value between ranges (ofMap / Processing map). clamp is optional.
float r = braid::remap(v, inLo, inHi, outLo, outHi);          // unclamped
float r = braid::remap(mouseX(), 0, width(), 0, 1, true);     // clamped to [0,1]
```

## Surface algebra (the magic mirror)

A `Surface` is elastic: you can add, blend, and transform whole layers as if they were
numbers. These are **total** — they never error, never need guard checks.

```cpp
braid::Surface& s = surface();   // the frame you're drawing into

s += other;                 // additive composite (energy adds)
s.over(other);              // alpha composite (other over s)
s.compositeFrom(other);     // draw other as a fullscreen layer

s.zoom(1.02f);              // >1 magnifies toward center (tunnel in)
s.rotate(0.0016f);          // radians, about center
s.shift(dx, dy);            // pixels
s.invert();                 // rgb -> 1-rgb
s.multiply({r,g,b,a});      // per-channel gain/tint
s.clear({0,0,0,1});
```

### Self-feedback (the ouroboros)

One Surface feeding itself. `feedback(gain, transform)` applies `transform` to the
current contents, then decays by `gain`. The double-buffer is hidden — one knob.

```cpp
void draw() override {
    // NOTE: do NOT call background() — feedback needs the Surface to accumulate.
    surface().feedback(0.99f, [](braid::Surface& s) {
        s.zoom(1.02f);
        s.rotate(0.0016f);
        s.invert();
    });
    // ...then draw new input on top each frame.
    noStroke();
    fill(1.0f, 0.6f, 0.2f);
    circle(width()*0.5f, height()*0.5f, 40.0f);
}
```

Gain `< 1.0` = trails fade (replace-ish). Gain `1.0` = pure accumulation.

### Built-in effects (current API; will migrate to pluggable shaders)

These replace the contents of the Surface they are called on:

```cpp
s.blur(13.0f);                      // separable Gaussian blur, radius in pixels
s.threshold(1.0f, 0.1f);            // brightpass: keep luma > level (soft knee)
s.bloom(1.0f, 1.0f, 5);             // threshold -> blur -> additive composite back
s.contour(0.5f, 1.0f, 0);           // isoline/edge detector (mode 0=8-neighbor +/X)
```

`paste` and `pasteSelf` draw a source Surface as a placed, rotated quad instead of
fullscreen:

```cpp
s.paste(src, {cx, cy}, {w, h}, rotation);  // draw src onto s as a quad
s.pasteSelf({cx, cy}, {w, h}, 0.02f);      // snapshot s, paste it back over itself
```

## Image I/O — the `braid-image` addon

Loading/saving images is an **addon**, not core. The methods are declared on `Surface`
but only work if you link `braid-image` (see chalet below). An image *is* a Surface, so
it composes with everything above.

```cpp
if (auto r = braid::Surface::load("photo.png")) {     // PNG/JPG/... (mango decode)
    braid::Surface img = std::move(*r);
    surface().compositeFrom(img);
} else {
    std::fprintf(stderr, "%s\n", r.error.c_str());
}

surface().save("frame.png");   // encodes by extension: .png/.jpg/...
```

## Fallible vs total — the `Result<T>` rule

- **Expressive things never fail** (Surface algebra, sketch primitives) → just call them.
- **Resource things can fail** (`load`, `Shader::load`, `Mesh::*`) → they return
  `Result<T>`. Check it:

```cpp
if (auto r = braid::Surface::load("x.png")) {
    use(*r);                      // *r and r-> reach the value
} else {
    std::fprintf(stderr, "%s\n", r.error.c_str());   // r.error is the message
}
```

GPU resources (`Surface`, `Shader`, `Mesh`) are **move-only** — `std::move` them, store
in `std::optional<T>` as members, and `clone()` for an explicit GPU copy.

## Build & run

Build system is **chalet** (config in `chalet.yaml`). Targets are the examples.

```bash
chalet build                 # build everything
chalet buildrun playground   # build + run one target
chalet buildrun cubes
chalet clean --all
```

Targets: `playground` (scratch canvas), `hello` (Tier-1 red triangle), `sketch`,
`feedback`, `cubes` (3D wireframe), `image` (load/save), `feed`, `bloom`, `contour`,
`multiwindow`, `syntype_basic`, `syntype_ui`, `syntype_audio`.
Other tasks are in `.zed/tasks.json`.

### Adding a new example

**Every new example needs its own target in `chalet.yaml`** — there is no auto-discovery.
Just adding the `.cpp` file is not enough; without a target, `chalet build` won't see it.

1. Write `examples/foo.cpp`.
2. Add a target under `targets:` in `chalet.yaml`. Minimal form (links core only):

   ```yaml
     foo:
       kind: executable
       runWorkingDirectory: ${cwd}/bin
       settings:Cxx:
         staticLinks:
           - braid-core
       files:
         include:
           - examples/foo.cpp
   ```

3. If it calls `Surface::load`/`save`, also link `braid-image` **and** the mango
   archives — copy the `image` target's full `staticLinks` list — otherwise those
   symbols are unresolved at link time.
4. Add a task for it in `.zed/tasks.json` so it appears in Zed's task runner.
5. Then `chalet buildrun foo`.

## App settings

Common `braid::App::Settings` fields:

```cpp
braid::App::Settings s{};
s.title = "my sketch";
s.width = 900;
s.height = 900;
s.resizable = true;                 // false for a fixed-size window
s.vsync = true;                     // false to let targetFps drive pacing
s.targetFps = 60;                   // 0 = uncapped (still yields to vsync if on)
s.enableValidation = true;          // WebGPU validation; on in Debug by default
s.position = {100, 100};            // optional top-left position
s.monitors = {2, 3, 4, 5};          // span these monitors as one borderless window
```

Use `Monitors::list()` to query connected displays and `Monitors::unionOf(indices)`
to get the bounding rect for spanning.

## Multiwindow

`App` can spawn additional windows sharing the same device and run loop:

```cpp
class Secondary : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;
    void draw() override { background(0.1f); circle(100, 100, 40); }
};

class Primary : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;
    void setup() override {
        braid::App::Settings s2{};
        s2.title = "secondary";
        s2.width = 400;
        s2.height = 400;
        createWindow<Secondary>(s2);
    }
};
```

The primary window's `targetFps` drives process-wide frame pacing; per-window `vsync`
is independent. The run loop blocks until the last window closes.

## Architecture in one line

`braid-core` = Surface + algebra + the loop (WebGPU is what it's made of).
Everything that flows **into or out of** a Surface (image, later audio/video) is an
**addon** — a separate target you link only when you use it. New addons copy the
`braid-image` pattern: declare entry points in `braid.h`, define them in the addon TU,
reach core internals via `braid_detail.h`, list heavy archives only on the consumer.

## Gotchas

- **Feedback + `background()` don't mix** — `background()` clears every frame and kills
  accumulation. Omit it for feedback.
- **Radians everywhere.** `glm::radians(deg)` if you think in degrees.
- **3D needs a camera** — call `perspective()` + `camera()` before `box()`.
- **Surfaces are 16-bit float by default** (smooth feedback, HDR headroom). The
  swapchain is 8-bit BGRA — that conversion is automatic.
- **Custom WGSL clip space is `[0,1]`** (Metal/D3D/Vulkan style, not GL's `[-1,1]`).
  glm is already configured for this; just don't assume GL conventions.
- **`save()` only works on offscreen Surfaces**, not the swapchain (it reads back the
  texture). `surface()` is offscreen, so it's fine.
