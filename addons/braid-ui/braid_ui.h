// braid_ui.h — TinyUI: a small, retained-mode UI addon for Braid. See
// md/ui.md for the full design writeup this implements.
//
// M1 (current): rect-only rendering — drawText() is an empty stub, filled in
// by M2 with an embedded bitmap font. Layouts load from a .txt file (§5 in
// md/ui.md); widgets own their values internally (ui.get<float>("name")), no
// pointer binding. TinyUI is a window-scoped Addon: register it with
// Window::addAddon(), it renders into its own offscreen Surface and is
// composited over the swapchain at present time — never into the window's
// main Surface, so screenshot/record/feedback stay clean by default.
#pragma once

#include "braid.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace braid {

// ---------------------------------------------------------------------------
// UISettings — drives the layout parser's auto-layout and widget theming.
// ---------------------------------------------------------------------------
struct UISettings {
    glm::ivec2 elementSize{200, 20};
    int elementSpacing = 4;
    int elementPadding = 5;
    int labelBaseline = 5;

    glm::vec4 colorBg{0.5f, 0.5f, 0.5f, 1.0f};
    glm::vec4 colorFg{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 colorLabel{1.0f, 1.0f, 1.0f, 1.0f};
};

// ---------------------------------------------------------------------------
// Element — bare skeleton: name/label/pos/size, draw + mouse hooks, and
// serialization hooks. Serialization lives here (not on TypedElement) so
// presets can walk the widget array without knowing each concrete type.
// ---------------------------------------------------------------------------
class Element {
public:
    std::string name;    // key / lookup / serialization (single token, no spaces)
    std::string label;   // text shown to the user
    glm::ivec2 pos{0, 0};
    glm::ivec2 size{200, 20};

    // Set by TinyUI when the widget is added. draw() has no settings parameter
    // (see md/ui.md §3.1), so widgets reach theming through this instead.
    const UISettings* theme = nullptr;

    // Set by TinyUI alongside theme: overlay texels per point (the window's
    // pixelRatio — md/hidpi.md §8). draw() code passes it to fillRect/drawText;
    // pos/size and hitTest stay in logical points, matching the mouse.
    float drawScale = 1.0f;

    virtual ~Element() = default;

    virtual void draw(Surface&) = 0;
    virtual bool hitTest(glm::ivec2 p) const;

    virtual void onMousePress(glm::ivec2) {}
    virtual void onMouseDrag(glm::ivec2) {}
    virtual void onMouseRelease(glm::ivec2) {}

    // Generic serialization / presets — no-op for labels.
    virtual void setFromString(const std::string&) {}
    virtual std::string toString() const { return {}; }

    // Generic reset — no-op default.
    virtual void resetToDefault() {}
};

// ---------------------------------------------------------------------------
// TypedElement<T> — typed value semantics. Values live inside the widget: no
// pointer binding in v1 (md/ui.md §12). get() is non-virtual; set() is
// virtual so clamping overrides (sliders) can depend on it — every mutation
// path (drag, preset load, resetToDefault, setFromString) funnels through it.
// ---------------------------------------------------------------------------
template <typename T>
class TypedElement : public Element {
public:
    T def{};  // default value

    T get() const { return value_; }
    virtual void set(T value) { value_ = value; }

    void resetToDefault() override { set(def); }

protected:
    T value_{};
};

// ---------------------------------------------------------------------------
// Concrete widgets (M1: label, bool, float, int — see md/ui.md §3.3).
// ---------------------------------------------------------------------------
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

    FloatSlider(std::string name, glm::ivec2 pos, float min, float max, float def);

    void draw(Surface&) override;
    void onMousePress(glm::ivec2 p) override;
    void onMouseDrag(glm::ivec2 p) override;

    void set(float v) override { TypedElement<float>::set(std::clamp(v, min, max)); }
    void setFromString(const std::string& s) override;
    std::string toString() const override;
};

class IntSlider : public TypedElement<int> {
public:
    int min = 0;
    int max = 100;

    IntSlider(std::string name, glm::ivec2 pos, int min, int max, int def);

    void draw(Surface&) override;
    void onMousePress(glm::ivec2 p) override;
    void onMouseDrag(glm::ivec2 p) override;

    void set(int v) override { TypedElement<int>::set(std::clamp(v, min, max)); }
    void setFromString(const std::string& s) override;
    std::string toString() const override;
};

// Draw a bitmap-text label into a UI surface. Tint defaults to white so labels
// can use it as-is; widgets pass their theme foreground color. `scale` is
// texels per point (Element::drawScale): glyphs integer-scale with it so text
// keeps its physical size and stays crisp on HiDPI (md/hidpi.md §6).
void drawText(Surface&, glm::ivec2, const std::string&, glm::vec4 color = {1, 1, 1, 1},
              float scale = 1.0f);

// ---------------------------------------------------------------------------
// TinyUI — the addon. Loads a layout file, renders every frame into its own
// offscreen Surface (never into window.surface()), and dispatches mouse
// events with single-widget capture semantics (md/ui.md §8).
// ---------------------------------------------------------------------------
class TinyUI : public Addon {
public:
    explicit TinyUI(const std::string& path);

    void setup(Window& w) override;
    void draw(Window& w) override;

    // The UI's own layer: blitted over the swapchain at present when visible,
    // never written into window.surface().
    Surface* overlay() override;
    Surface& surface();  // for explicit compositing into exports

    bool visible = true;  // false: no overlay blit, no mouse dispatch; values/presets still work
    UISettings settings;

    void add(std::unique_ptr<Element> e);

    // Typed lookup by name. Failure policy: unknown name or wrong type logs
    // once and returns T{} / no-ops — get<float>("count") does NOT convert.
    template <typename T>
    T get(const std::string& name) const {
        Element* e = find(name);
        if (!e) { warnOnce(name, "no widget with this name"); return T{}; }
        auto* te = dynamic_cast<TypedElement<T>*>(e);
        if (!te) { warnOnce(name, "widget exists but is a different type"); return T{}; }
        return te->get();
    }

    template <typename T>
    void set(const std::string& name, T value) {
        Element* e = find(name);
        if (!e) { warnOnce(name, "no widget with this name"); return; }
        auto* te = dynamic_cast<TypedElement<T>*>(e);
        if (!te) { warnOnce(name, "widget exists but is a different type"); return; }
        te->set(value);
    }

    void resetAll();
    void reset(const std::string& name);

    // Preset serialization — walks the widget array generically via
    // toString()/setFromString(), no dynamic casts. Unknown names in a loaded
    // preset are ignored with a warning so layouts can evolve freely.
    void savePreset(const std::string& path) const;
    void loadPreset(const std::string& path);

    // True if the UI is currently using the mouse (hovering or dragging).
    bool wantsMouse() const;

private:
    std::vector<std::unique_ptr<Element>> widgets_;
    std::optional<Surface> ui_;  // created in setup() — the ctor runs before the device exists
    Subscription mouseSub_;      // press/move/release all arrive on mouseEvents
    Subscription windowSub_;     // resizes the UI surface with the window

    Element* active_ = nullptr;  // captured widget: press set it, drag/release target it
    Element* hover_ = nullptr;   // hit-tested on move when nothing is captured
    float drawScale_ = 1.0f;     // window pixelRatio, mirrored into every widget's drawScale

    mutable std::unordered_set<std::string> warned_;  // get<T>/set<T> log-once tracking

    Element* find(const std::string& name) const;
    void insert(std::unique_ptr<Element> e);  // dedup-by-name + theme, used by add() and the parser
    void loadLayout(const std::string& path);
    void handleMouse(const MouseEvent& e);
    void warnOnce(const std::string& name, const std::string& msg) const;
};

}  // namespace braid
