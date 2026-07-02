# Braid UI — Design Notes

**Status:** M1 implemented (2026-07-01) — `core::Addon`/`Window::addAddon` plumbing plus `addons/braid-ui/` (TinyUI). M2 (text) and M3+ (more widgets) not started.  
**Scope:** a retained-mode UI addon for Braid (persistent widget objects, redrawn each frame), window-scoped, with typed widgets and name-based value lookup.

This doc captures the design decisions from the 2026-07-01 session so the next implementation pass doesn't re-derive them.

**M1 implementation notes** (things the design left open that the code had to decide):
- `Element::theme` — a `const UISettings*` set by `TinyUI` when a widget is added. `draw(Surface&)` has no settings parameter (§3.1), so this is how widgets reach `colorBg`/`colorFg`/`labelBaseline` for rendering.
- `TinyUI::settings` — a public `UISettings` member (implied by §6, not shown in the §4.1 synopsis).
- Rects are drawn via a small self-contained flat-color WGSL pipeline private to `braid_ui.cpp` (`fillRect`), built only from `Surface`'s public API (`device()`/`beginLoad()`/`format()`/...). Each call records and submits its own tiny command buffer immediately — the same convention `Surface::clear()`/`compositeFrom()` already use — so no core/`Compositor` changes were needed for M1's rect-only rendering.
- Demo: `examples/ui_demo.cpp` + `examples/ui_demo.txt`, built via the new `braid-ui` chalet target.

---

## 1. Goals

- Small, OF/ofxMicroUI-like UI for creative-coding / installation use.
- Load widget layouts from a `.txt` file.
- Support the first widgets: `label`, `bool` (checkbox), `float` (slider), `int` (slider).
- Widgets own their values internally; the sketch reads them by name (`ui.get<float>("radius")`). No pointer binding in v1 — see §12.
- Render into the UI's **own** `braid::Surface`, composited over the main surface at present — so PNG export / video recording can include the UI or not (§9).
- Live on a `braid::Window` as an addon, not globally on `App`.

**Milestones:**

1. **M1 — blind widgets.** Rect-only rendering (no text anywhere, `drawText` is an empty stub), internal UI surface + present-time compositing, mouse capture, presets. Testable end-to-end.
2. **M2 — text.** Embedded bitmap font (font8x8-class): labels and numeric readouts. `drawText()` goes from stub to real; nothing else changes.
3. **M3+ — more widgets.** `button`, `radio`, `color`, groups (§12).

---

## 2. Architectural decisions already locked

### 2.1 Addons are window-scoped

The UI addon registers on the control window it belongs to. Output/display windows stay addon-free. This matches Braid's broader design: addons need a surface and mouse coordinates, so they belong to a `Window`, not to `App`.

See `md/braid_roadmap.md` §1: "Addons are window-scoped, not app-scoped."

### 2.2 One global frame pace

`Application` owns one `Timer`; only the primary window's `AppSettings::targetFps` is read. Every window advances the same simulation step and draws on the same cadence. Per-window differences are handled by **vsync** (`AppSettings::vsync` / swapchain `PresentMode`) and by frame-skipping inside an individual window's `draw()`, not by independent timers.

### 2.3 Three-layer widget hierarchy

1. **`Element`** — bare skeleton: name, label, position, size, plus virtual draw, mouse hooks, and serialization hooks (`setFromString` / `toString` / `resetToDefault`).
2. **`TypedElement<T>`** — adds typed value, default value, and reset.
3. **Concrete widgets** — `Label`, `Checkbox`, `FloatSlider`, `IntSlider`, etc. Implement drawing and mouse handling. Numeric sliders keep their own `min`/`max`; there is no separate `RangedElement` layer.

`Label` is a plain `Element`, not a `TypedElement`, because it has no value.

Groups (`radio`, `vec3`, color pickers) are future widgets that fit naturally as `Element`s containing child `Element`s.

---

## 3. Class design

### 3.1 `Element` — skeleton + serialization

```cpp
class Element {
public:
    std::string name;    // key / lookup / serialization (single token, no spaces)
    std::string label;   // text shown to the user
    glm::ivec2 pos{0, 0};
    glm::ivec2 size{200, 20};

    virtual ~Element() = default;

    virtual void draw(Surface&) = 0;
    virtual bool hitTest(glm::ivec2 p) const;

    virtual void onMousePress(glm::ivec2 p) {}
    virtual void onMouseDrag(glm::ivec2 p) {}
    virtual void onMouseRelease(glm::ivec2 p) {}

    // Generic serialization / presets — no-op for labels
    virtual void setFromString(const std::string&) {}
    virtual std::string toString() const { return {}; }

    // Generic reset — no-op default
    virtual void resetToDefault() {}
};
```

Serialization lives on the skeleton so presets can walk the whole array without knowing each widget's concrete type:

```cpp
for (auto& e : widgets_) {
    std::string s = e->toString();
    if (!s.empty()) {
        out << e->name << " " << s << "\n";
    }
}
```

Labels return an empty `toString()` and are skipped.

### 3.2 `TypedElement<T>` — typed value semantics

```cpp
template<typename T>
class TypedElement : public Element {
public:
    T def{};  // default value

    T get() const { return value_; }
    virtual void set(T value) { value_ = value; }

    void resetToDefault() override { set(def); }

protected:
    T value_{};
};
```

Values live inside the widget — there is no pointer binding in v1 (see §12). The layout file / preset is trivially the single source of truth, because there is no sketch variable to disagree with.

This layer is intentionally thin: it only deals with the typed value and default. Serialization is handled by the concrete widget overriding `setFromString` / `toString` from `Element`. `get()` is non-virtual; `set()` is virtual because clamping overrides depend on it.

### 3.3 Concrete widgets

```cpp
class Label : public Element {
public:
    Label(std::string text, glm::ivec2 pos);
    void draw(Surface&) override;
};

class Checkbox : public TypedElement<bool> {
public:
    Checkbox(std::string name, glm::ivec2 pos, bool def);

    void draw(Surface&) override;
    void onMousePress(glm::ivec2 p) override;

    void setFromString(const std::string& s) override;
    std::string toString() const override;
};

class FloatSlider : public TypedElement<float> {
public:
    float min = 0.0f;
    float max = 1.0f;

    FloatSlider(std::string name, glm::ivec2 pos,
                float min, float max, float def);

    void draw(Surface&) override;
    void onMouseDrag(glm::ivec2 p) override;

    void set(float v) override { TypedElement<float>::set(std::clamp(v, min, max)); }
    void setFromString(const std::string& s) override;
    std::string toString() const override;
};

class IntSlider : public TypedElement<int> {
public:
    int min = 0;
    int max = 100;

    IntSlider(std::string name, glm::ivec2 pos,
              int min, int max, int def);

    void draw(Surface&) override;
    void onMouseDrag(glm::ivec2 p) override;

    void set(int v) override { TypedElement<int>::set(std::clamp(v, min, max)); }
    void setFromString(const std::string& s) override;
    std::string toString() const override;
};
```

Numeric sliders clamp in `set()`. The text-file factory passes parsed values through `set()`, so out-of-range defaults are clamped automatically. All mutation paths — drag, preset load, `resetToDefault`, `setFromString` — funnel through the virtual `set()`.

**IntSlider drag mapping:** round to nearest (`std::lround`) so the max value is reachable.

**Drawing:** clamp the *drawn fraction* to `[0,1]`, but do not clamp the bound variable itself if the sketch writes outside `[min,max]` directly.

### 3.4 Groups (future)

`ofxMicroUI` uses groups for radios, color pickers, and `vec3` editors. Our hierarchy supports the same pattern without extra machinery:

```cpp
class Group : public Element {
public:
    std::vector<std::unique_ptr<Element>> children;

    void draw(Surface&) override;
    bool hitTest(glm::ivec2) const override;
    void onMousePress(glm::ivec2) override;
    void resetToDefault() override;
};
```

Not needed for checkbox / float / int, but the door is open.

---

## 4. Minimal addon interface

TinyUI is the first stateful runtime addon, so this is also the first `Addon` base class. Keep it minimal: just lifecycle hooks. There is deliberately **no priority/ordering knob** (removed 2026-07-01): addons run in registration order, and "I must see the finished frame" is expressed by the `afterDraw` phase, not by out-numbering the other addons.

```cpp
namespace braid {

class Addon {
public:
    virtual ~Addon() = default;
    virtual void setup(Window&) {}
    virtual void update(Window&) {}
    virtual void draw(Window&) {}

    // Runs after the sketch AND every addon's draw() — the frame is final.
    // Readers of the finished frame (recorders, publishers) belong here.
    virtual void afterDraw(Window&) {}

    // Optional overlay layer, blitted over the swapchain at present time
    // (after the mainSurface_ blit) with Blend::Alpha. nullptr = no overlay.
    virtual Surface* overlay() { return nullptr; }
};

class Window {
public:
    void addAddon(Addon& addon);          // sketch owns lifetime
    void addAddon(std::shared_ptr<Addon>); // shared ownership
    // ...
};

} // namespace braid
```

`Window` stores non-owning references for sketch-owned addons and shared_ptrs for heap-owned addons. The application loop calls addon hooks in registration order after the window's own hooks; within a frame the phases are: sketch `draw` → addon `draw`s → addon `afterDraw`s → present (`mainSurface_` blit, then `overlay()`s).

At present time the window blits `mainSurface_` to the swapchain as it does today (`braid_app.cpp` — the "single copy" that makes screenshot/record/feedback free), then blits each addon's non-null `overlay()` over it. The overlay blit is the same `Compositor::blit` call with `Blend::Alpha` + `LoadOp::Load` instead of `Blend::None` + `LoadOp::Clear`. **`mainSurface_` never contains the UI** — see §9.

**Lifetime rule for the non-owning overload:** in the usage example the sketch subclass *is* the `Window`, so a member addon is destroyed before the `Window` base. That's safe only if `Window` never calls into addons during teardown — lock that as a rule. (If it ever becomes a problem, make `addAddon` return a `braid::Subscription`-style handle that unregisters on destroy, matching the existing Channel idiom.)

### 4.1 `TinyUI` addon

```cpp
class TinyUI : public braid::Addon {
public:
    explicit TinyUI(const std::string& path);

    void setup(braid::Window& w) override;
    void draw(braid::Window& w) override;

    // The UI's own layer: blitted over the swapchain at present when visible,
    // never written into window.surface().
    braid::Surface* overlay() override;
    braid::Surface& surface();  // for explicit compositing into exports

    bool visible = true;  // false: no overlay blit, no mouse dispatch; values/presets still work

    void add(std::unique_ptr<Element> e);

    // Typed lookup by name
    template<typename T>
    T get(const std::string& name) const;

    template<typename T>
    void set(const std::string& name, T value);

    void resetAll();
    void reset(const std::string& name);

    // Preset serialization
    void savePreset(const std::string& path) const;
    void loadPreset(const std::string& path);

    // True if the UI is currently using the mouse (hovering or dragging).
    bool wantsMouse() const;

private:
    std::vector<std::unique_ptr<Element>> widgets_;
    std::optional<braid::Surface> ui_;  // created in setup() — the ctor runs before the device exists
    braid::Subscription mouseSub_;  // one sub: press/move/release all arrive on mouseEvents

    Element* active_ = nullptr;

    Element* find(const std::string& name) const;
};
```

`setup()` subscribes to the window's `mouseEvents` channel and allocates the UI surface (window-sized; the constructor cannot do this because it runs before the device exists — same `std::optional` pattern as `mainSurface_`). `draw()` clears the UI surface to transparent `{0,0,0,0}` and renders every widget into it. On `WindowEvent::Resized` the UI surface resizes with the window.

---

## 5. Text file format

One widget per line. Empty lines and lines starting with `#` are ignored.

```txt
# type name [type-specific args]

label  "Global Controls"
bool   enabled   1
float  radius    0 1 0.5
int    count     1 10 5
```

| Type   | Columns                         |
|--------|---------------------------------|
| label  | `label text`                    |
| bool   | `bool name def`                 |
| float  | `float name min max def`        |
| int    | `int name min max def`          |

Names must be single tokens (no spaces). Label text is the only quoted field, and labels do not serialize.

The parser auto-layouts widgets in a vertical stack. Explicit `pos` in ctors is for programmatic creation only.

---

## 6. Layout settings

A small `UISettings` struct drives the parser's auto-layout and widget theming:

```cpp
struct UISettings {
    glm::ivec2 elementSize{200, 20};
    int elementSpacing = 4;
    int elementPadding = 5;
    int labelBaseline = 5;

    glm::vec4 colorBg{0.5f, 0.5f, 0.5f, 1.0f};
    glm::vec4 colorFg{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 colorLabel{1.0f, 1.0f, 1.0f, 1.0f};
};
```

The parser maintains a running `y` cursor. Each widget is placed at the current cursor, then the cursor advances by `elementSize.y + elementSpacing`.

---

## 7. Usage example

```cpp
class MyShow : public braid::SketchApp {
    braid::TinyUI ui{"show.txt"};

public:
    using SketchApp::SketchApp;

    void setup() override {
        addAddon(ui);  // Window::addAddon, because App is a Window
    }

    void draw() override {
        background(0);

        if (ui.get<bool>("enabled")) {
            float radius = ui.get<float>("radius");
            int count = ui.get<int>("count");

            fill(1, 0, 0);
            for (int i = 0; i < count; ++i) {
                float a = i / float(count) * glm::two_pi<float>();
                circle(width() * 0.5f + cos(a) * radius * 300,
                       height() * 0.5f + sin(a) * radius * 300, 10);
            }
        }
    }
};
```

Per-frame `get<T>` is a linear name search plus a `dynamic_cast` — negligible at UI scale (tens of widgets). Read once per frame into a local, as above. Remember the failure policy from §10: a misspelled name logs once and returns `T{}` forever after, so if a control seems dead, check the name against the layout file first.

---

## 8. Mouse dispatch

TinyUI maintains an `active_` element for capture semantics:

- **press:** hit-test all widgets; if one hits, set `active_` and forward.
- **drag:** forward to `active_` regardless of hit-test (dragging a slider past the end of its track must keep working).
- **release:** forward to `active_`, then clear it.

`wantsMouse()` returns true while hovering or while `active_` is set, so a sketch with camera-drag can ignore the mouse when the UI has it.

Everything arrives on the single `Channel<MouseEvent> mouseEvents`; `button`/`pressed` distinguish press, move, and release. "Drag" is not a separate event — it's a move while `active_` is set. Note button events carry no position of their own: core fills `e.pos` with the latest cached move position (`braid_app.cpp`), so press handlers can trust `e.pos` as-is.

**DPI note:** Braid core currently has no HiDPI/backing-scale handling. Verify on a Retina display that `mouseEvents` coordinates and the window surface are in the same unit space; if not, `hitTest` will need a scale factor from `Window` once HiDPI lands.

---

## 9. Drawing

### 9.1 The UI owns its own Surface

TinyUI renders into its **own offscreen `Surface`** (window-sized), not into `window.surface()`:

1. `draw()` clears the UI surface to transparent `{0,0,0,0}` and renders every widget into it.
2. At present, the window blits `mainSurface_` to the swapchain (the existing "single copy"), then blits each visible addon `overlay()` over it with `Blend::Alpha`.
3. `mainSurface_` therefore never contains the UI: PNG export, video recording, and feedback loops are clean **by default**.

Baking the UI into an export is explicit, and it's just Surface algebra:

```cpp
surface().compositeFrom(ui.surface(), braid::Blend::Alpha);
```

`TinyUI::visible` toggles the overlay blit **and** mouse dispatch: a hidden UI ignores the mouse and `wantsMouse()` returns false (invisible widgets must not eat clicks meant for the sketch). Values, `get<T>`, and presets keep working while hidden.

This is *not* the ofxMicroUI dirty-flag FBO cache — the UI surface is still redrawn every frame. But now that the layer exists, a dirty flag that skips the redraw becomes a five-line optimization later, if profiling ever asks for it.

### 9.2 M1 is blind: text is stubbed

- A single `drawText(Surface&, glm::ivec2, const std::string&)` helper exists but is an **empty function**. Every widget calls it where its text will eventually go (label, value readout), so M2 fills in one function and everything lights up at once.
- `Label::draw()` draws nothing, but the Label still occupies its slot in the vertical stack — layouts don't shift when text arrives.
- Sliders draw track + fill; checkboxes draw box + inner mark. You tell widgets apart by their order in the layout file.
- Rect rendering is all M1 needs: one shared quad `Mesh` + flat-color `Shader` (or the compositor's quad path), nothing more.

M2 replaces the stub with an embedded public-domain bitmap font (font8x8-class) baked into one texture at startup — labels and numeric readouts with no `braid-syntype` dependency. `braid-syntype` stays the optional pretty upgrade.

---

## 10. Serialization / reset / presets

- Each `TypedElement<T>` widget implements `toString()` / `setFromString()`.
- `Element::resetToDefault()` is virtual with a no-op default; `TypedElement<T>` overrides it to set `def`.
- `TinyUI::resetAll()` iterates the array and calls `resetToDefault()` on each widget.
- Presets save/load walk the array generically, calling `toString()` / `setFromString()` without dynamic casts.
- `get<T>` / `set<T>` use `dynamic_cast<TypedElement<T>*>`. Failure policy: unknown name or wrong type → log once, return `T{}` / no-op. `get<float>("count")` does **not** convert.
- Parser warns on duplicate names (first wins).
- `loadPreset` ignores unknown names with a warning so layouts can evolve without invalidating old presets.
- Floats round-trip with `%.9g`.
- All mutation paths funnel through the virtual `set()` — that's the single place clamping lives.

This makes "reset UI to default mid-run" trivial and uniform across widget types.

---

## 11. What this fixes over ofxMicroUI

| ofxMicroUI approach | Braid TinyUI approach |
|---|---|
| Single base class with raw pointers to every type (`bool* b`, `int* i`, `float* f`…) | Each widget owns exactly one typed value inside `TypedElement<T>`; no raw pointers at all |
| `slider` handles float and int via `isInt` flag + dual pointers | `FloatSlider` and `IntSlider` are separate `TypedElement<T>` subclasses |
| Inconsistent mixins (`varKindString`, `colorBase`, `group`) | Clean layered hierarchy: `Element` → `TypedElement<T>` → concrete widget |
| Serialization handled per-widget with ad-hoc XML | Serialization hooks live on `Element`; presets walk the array generically |
| Heavy FBO caching in the base design | Draw directly to surface; add caching only if needed |

---

## 12. Open questions / future

- **Theming:** expand `UISettings` with more colors, label positioning, and per-widget overrides.
- **More widgets:** `button`, `radio`, `color`, `vec2/vec3`, `string`, `Group` containers.
- **Event propagation:** per-frame `get<T>` polling is enough for v1. Add per-widget `std::function<void(T)> onChange` later, forced by `button` (a button has no value to poll).
- **Pointer binding:** deliberately dropped from v1 (2026-07-01). If per-frame `get<T>` ever feels clunky, an optional `Binding<T>` (pointer + fallback, push-on-bind semantics) can be reintroduced inside `TypedElement<T>` without touching the lookup API — the full design lives in this file's git history.
- **Layout:** v1 is a vertical stack. Add columns, horizontal flow, and explicit grid later.
- **Hot reload:** watch the layout file's mtime; on change, save current values to an in-memory preset, rebuild widgets, load the preset back by name. Nearly free once presets exist.
- **MIDI/OSC mapping:** the typed `set()` and `get()` make external control straightforward.

---

## 13. Reference implementations

These OF-based UIs were inspected while designing Braid TinyUI. Kept here for reference when adding new widget types or behavior.

- **TinyUI** — minimal OF UI with auto-registered draw/mouse listeners.  
  `/Users/z/mirabilis/TinyUI/src/ofxTinyUI.h`  
  `/Users/z/mirabilis/TinyUI/src/ofxTinyUI.cpp`

- **ofxMicroUI** — fuller-featured OF UI with groups, presets, radios, color pickers, lists, etc.  
  `/Users/z/Dmtr/ofworks/addons/ofxMicroUI/src/ofxMicroUI.h`  
  `/Users/z/Dmtr/ofworks/addons/ofxMicroUI/src/ofxMicroUIElements.h`  
  `/Users/z/Dmtr/ofworks/addons/ofxMicroUI/src/ofxMicroUIElements.cpp`  
  `/Users/z/Dmtr/ofworks/addons/ofxMicroUI/src/ofxMicroUIParseText.cpp`

---

## 14. What to avoid

- One bloated base class with pointers to every possible type.
- A separate `RangedElement` abstraction for now. Min/max live directly in `FloatSlider`/`IntSlider`.
- Global/App-scoped UI ownership. The UI is a `Window` addon.
- Independent per-window FPS. The UI updates on the global frame pace.
- Premature dirty-flag caching. The UI surface is redrawn every frame; add the dirty flag only if profiling asks.
- Text rendering in M1. Ship blind widgets first; `drawText` stays an empty stub until M2.
