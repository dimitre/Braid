// braid_ui.cpp — TinyUI implementation. Widgets draw flat-colored rects via a
// small self-contained pipeline (fillRect, below) — no dependency on the core
// Compositor, so this addon only needs Surface's public API. Each fillRect
// call records and submits its own tiny command buffer immediately (the same
// "own encoder, own submit" convention Surface::clear()/compositeFrom() use),
// which keeps per-rect uniforms trivially correct with no ring-buffer to
// manage — plenty fast at UI scale (tens of widgets).
#include "braid_ui.h"
#include "braid_text.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include <fmt/core.h>

namespace braid {

// ===========================================================================
// fillRect — the one primitive M1 needs. Draws an axis-aligned rect into
// `target` with a flat color, preserving existing contents (Load). Coordinates
// arrive in widget point-space and are multiplied by `scale` (the window's
// pixelRatio) — TinyUI's own logical→pixel projection (md/hidpi.md §8); the
// NDC divisor stays the target's real texel size.
// ===========================================================================
namespace {

struct RectUniforms {
    glm::vec2 posPx{0, 0};
    glm::vec2 sizePx{0, 0};
    glm::vec4 color{1, 1, 1, 1};
    glm::vec2 surfaceSizePx{1, 1};
    glm::vec2 _pad{0, 0};
};

const char* kRectWGSL = R"WGSL(
struct Rect {
  posPx : vec2<f32>,
  sizePx : vec2<f32>,
  color : vec4<f32>,
  surfaceSizePx : vec2<f32>,
  _pad : vec2<f32>,
};
@group(0) @binding(0) var<uniform> u : Rect;
struct VO { @builtin(position) pos : vec4<f32> };
@vertex fn vs(@builtin(vertex_index) i : u32) -> VO {
  var cs = array<vec2<f32>, 6>(
    vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
    vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0));
  let corner = cs[i];
  let px = u.posPx + corner * u.sizePx;
  let ndc = vec2<f32>(px.x / u.surfaceSizePx.x * 2.0 - 1.0,
                       1.0 - px.y / u.surfaceSizePx.y * 2.0);
  var o : VO;
  o.pos = vec4<f32>(ndc, 0.0, 1.0);
  return o;
}
@fragment fn fs(in : VO) -> @location(0) vec4<f32> {
  return u.color;
}
)WGSL";

class RectRenderer {
public:
    void ensure(wgpu::Device device) {
        if (ready_) return;
        device_ = device;

        wgpu::ShaderSourceWGSL src{};
        src.code = kRectWGSL;
        wgpu::ShaderModuleDescriptor smd{};
        smd.nextInChain = &src;
        smd.label = "braid-ui-rect";
        module_ = device_.CreateShaderModule(&smd);

        wgpu::BindGroupLayoutEntry e{};
        e.binding = 0;
        e.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        e.buffer.type = wgpu::BufferBindingType::Uniform;
        wgpu::BindGroupLayoutDescriptor bgld{};
        bgld.entryCount = 1;
        bgld.entries = &e;
        bgl_ = device_.CreateBindGroupLayout(&bgld);

        wgpu::PipelineLayoutDescriptor pld{};
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &bgl_;
        pl_ = device_.CreatePipelineLayout(&pld);
        ready_ = true;
    }

    wgpu::RenderPipeline pipelineFor(wgpu::TextureFormat fmt) {
        for (auto& [f, p] : cache_)
            if (f == fmt) return p;

        wgpu::ColorTargetState target{};
        target.format = fmt;
        target.blend = &Blend::Alpha;
        target.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = module_;
        frag.entryPoint = "fs";
        frag.targetCount = 1;
        frag.targets = &target;
        wgpu::RenderPipelineDescriptor rpd{};
        rpd.layout = pl_;
        rpd.vertex.module = module_;
        rpd.vertex.entryPoint = "vs";
        rpd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        rpd.multisample.count = 1;
        rpd.multisample.mask = 0xFFFFFFFF;
        rpd.fragment = &frag;
        wgpu::RenderPipeline p = device_.CreateRenderPipeline(&rpd);
        cache_.push_back({fmt, p});
        return p;
    }

    wgpu::BindGroupLayout layout() const { return bgl_; }

private:
    bool ready_ = false;
    wgpu::Device device_;
    wgpu::ShaderModule module_;
    wgpu::BindGroupLayout bgl_;
    wgpu::PipelineLayout pl_;
    std::vector<std::pair<wgpu::TextureFormat, wgpu::RenderPipeline>> cache_;
};

RectRenderer& rectRenderer() {
    static RectRenderer r;
    return r;
}

void fillRect(Surface& target, glm::vec2 pos, glm::vec2 size, glm::vec4 color, float scale) {
    if (!target.isValid() || size.x <= 0.0f || size.y <= 0.0f) return;
    RectRenderer& r = rectRenderer();
    r.ensure(target.device());

    RectUniforms u{};
    u.posPx = pos * scale;
    u.sizePx = size * scale;
    u.color = color;
    u.surfaceSizePx = {static_cast<float>(target.width()), static_cast<float>(target.height())};

    wgpu::BufferDescriptor bd{};
    bd.size = sizeof(RectUniforms);
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer ub = target.device().CreateBuffer(&bd);
    target.device().GetQueue().WriteBuffer(ub, 0, &u, sizeof(u));

    wgpu::BindGroupEntry be{};
    be.binding = 0;
    be.buffer = ub;
    be.size = sizeof(RectUniforms);
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = r.layout();
    bgd.entryCount = 1;
    bgd.entries = &be;
    wgpu::BindGroup bg = target.device().CreateBindGroup(&bgd);

    wgpu::CommandEncoder enc = target.device().CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = target.beginLoad(enc);
    pass.SetPipeline(r.pipelineFor(target.format()));
    pass.SetBindGroup(0, bg);
    pass.Draw(6);
    target.end(pass);
    wgpu::CommandBuffer cmd = enc.Finish();
    target.device().GetQueue().Submit(1, &cmd);
}

// ===========================================================================
// TextRenderer — built-in bitmap font for widget labels.
// ===========================================================================
const char* kTextWGSL = R"WGSL(
@group(0) @binding(0) var fontTex : texture_2d<f32>;
@group(0) @binding(1) var fontSampler : sampler;

struct VSOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv : vec2<f32>,
    @location(1) color : vec4<f32>,
};

@vertex
fn vs(@location(0) position : vec3<f32>,
      @location(1) uv : vec2<f32>,
      @location(2) normal : vec3<f32>,
      @location(3) color : vec4<f32>) -> VSOut {
    var o : VSOut;
    o.pos = vec4<f32>(position, 1.0);
    o.uv = uv;
    o.color = color;
    return o;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4<f32> {
    let a = textureSample(fontTex, fontSampler, in.uv).r;
    return vec4<f32>(in.color.rgb, in.color.a * a);
}
)WGSL";

class TextRenderer {
public:
    void ensure(wgpu::Device device) {
        if (ready_) return;
        device_ = device;

        font_ = std::make_unique<BitmapFont>(device_);

        wgpu::ShaderSourceWGSL src{};
        src.code = kTextWGSL;
        wgpu::ShaderModuleDescriptor smd{};
        smd.nextInChain = &src;
        smd.label = "braid-ui-text";
        module_ = device_.CreateShaderModule(&smd);

        bgl_ = font_->bindGroupLayout();
        wgpu::PipelineLayoutDescriptor pld{};
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &bgl_;
        pl_ = device_.CreatePipelineLayout(&pld);
        ready_ = true;
    }

    void draw(Surface& target, glm::ivec2 pos, const std::string& text, glm::vec4 color,
              float scale) {
        if (!target.isValid() || text.empty()) return;
        ensure(target.device());

        // Origin and glyph metrics both × scale: point-space text lands at texel
        // positions with integer-scaled glyphs (md/hidpi.md §6).
        std::vector<Vertex> verts = font_->buildQuads(text, static_cast<float>(pos.x) * scale,
                                                       static_cast<float>(pos.y) * scale, color, scale);
        if (verts.empty()) return;

        // BitmapFont gives pixel-space positions; the UI surface is drawn in NDC.
        const float w = static_cast<float>(target.width());
        const float h = static_cast<float>(target.height());
        for (Vertex& v : verts) {
            v.position.x = v.position.x / w * 2.0f - 1.0f;
            v.position.y = 1.0f - v.position.y / h * 2.0f;
        }

        const size_t vboSize = verts.size() * sizeof(Vertex);
        wgpu::BufferDescriptor bd{};
        bd.size = vboSize;
        bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        wgpu::Buffer vbo = device_.CreateBuffer(&bd);
        device_.GetQueue().WriteBuffer(vbo, 0, verts.data(), vboSize);

        wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
        wgpu::RenderPassEncoder pass = target.beginLoad(enc);
        pass.SetPipeline(pipelineFor(target.format()));
        pass.SetVertexBuffer(0, vbo, 0, vboSize);
        pass.SetBindGroup(0, font_->bindGroup());
        pass.Draw(static_cast<uint32_t>(verts.size()));
        target.end(pass);
        wgpu::CommandBuffer cmd = enc.Finish();
        device_.GetQueue().Submit(1, &cmd);
    }

    glm::vec2 measure(const std::string& text) const {
        if (!font_ || text.empty()) return {0.0f, 0.0f};
        return font_->measure(text);
    }

private:
    wgpu::RenderPipeline pipelineFor(wgpu::TextureFormat fmt) {
        for (auto& [f, p] : cache_)
            if (f == fmt) return p;

        wgpu::ColorTargetState target{};
        target.format = fmt;
        target.blend = &Blend::Alpha;
        target.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = module_;
        frag.entryPoint = "fs";
        frag.targetCount = 1;
        frag.targets = &target;
        wgpu::VertexBufferLayout vbl = Mesh::vertexLayout();
        wgpu::RenderPipelineDescriptor rpd{};
        rpd.layout = pl_;
        rpd.vertex.module = module_;
        rpd.vertex.entryPoint = "vs";
        rpd.vertex.bufferCount = 1;
        rpd.vertex.buffers = &vbl;
        rpd.fragment = &frag;
        rpd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        rpd.multisample.count = 1;
        rpd.multisample.mask = 0xFFFFFFFF;
        wgpu::RenderPipeline p = device_.CreateRenderPipeline(&rpd);
        cache_.push_back({fmt, p});
        return p;
    }

    bool ready_ = false;
    wgpu::Device device_;
    std::unique_ptr<BitmapFont> font_;
    wgpu::ShaderModule module_;
    wgpu::BindGroupLayout bgl_;
    wgpu::PipelineLayout pl_;
    std::vector<std::pair<wgpu::TextureFormat, wgpu::RenderPipeline>> cache_;
};

TextRenderer& textRenderer() {
    static TextRenderer t;
    return t;
}

// Splits a layout/preset line into tokens; a "quoted string" is one token
// (used by label text, the only field allowed to contain spaces).
std::vector<std::string> tokenizeLine(const std::string& line) {
    std::vector<std::string> toks;
    size_t i = 0, n = line.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= n) break;
        if (line[i] == '"') {
            size_t j = line.find('"', i + 1);
            if (j == std::string::npos) j = n;
            toks.push_back(line.substr(i + 1, j - i - 1));
            i = (j < n) ? j + 1 : n;
        } else {
            size_t j = i;
            while (j < n && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            toks.push_back(line.substr(i, j - i));
            i = j;
        }
    }
    return toks;
}

}  // namespace

void drawText(Surface& target, glm::ivec2 pos, const std::string& text, glm::vec4 color,
              float scale) {
    textRenderer().draw(target, pos, text, color, scale);
}

// ===========================================================================
// Element
// ===========================================================================
bool Element::hitTest(glm::ivec2 p) const {
    return p.x >= pos.x && p.x < pos.x + size.x && p.y >= pos.y && p.y < pos.y + size.y;
}

// ===========================================================================
// Label
// ===========================================================================
Label::Label(std::string text, glm::ivec2 pos_) {
    label = std::move(text);
    pos = pos_;
}

void Label::draw(Surface& s) {
    glm::vec4 c = theme ? theme->colorLabel : glm::vec4{1.0f, 1.0f, 1.0f, 1.0f};
    drawText(s, pos, label, c, drawScale);
}

// ===========================================================================
// Checkbox
// ===========================================================================
Checkbox::Checkbox(std::string name_, glm::ivec2 pos_, bool def_) {
    name = std::move(name_);
    label = name;
    pos = pos_;
    def = def_;
    set(def_);
}

void Checkbox::draw(Surface& s) {
    glm::vec4 bg = theme ? theme->colorBg : glm::vec4{0.5f, 0.5f, 0.5f, 1.0f};
    glm::vec4 fg = theme ? theme->colorFg : glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};
    int baseline = theme ? theme->labelBaseline : 5;

    float box = static_cast<float>(size.y);
    fillRect(s, glm::vec2(pos), glm::vec2(box, box), bg, drawScale);
    if (get()) {
        float inset = box * 0.25f;
        fillRect(s, glm::vec2(pos) + glm::vec2(inset), glm::vec2(box - inset * 2.0f, box - inset * 2.0f), fg,
                 drawScale);
    }
    glm::vec4 labelColor = theme ? theme->colorLabel : glm::vec4{1.0f, 1.0f, 1.0f, 1.0f};
    drawText(s, {pos.x + size.y + 8, pos.y + baseline}, label, labelColor, drawScale);
}

void Checkbox::onMousePress(glm::ivec2) { set(!get()); }

void Checkbox::setFromString(const std::string& s) { set(std::atoi(s.c_str()) != 0); }
std::string Checkbox::toString() const { return get() ? "1" : "0"; }

// ===========================================================================
// FloatSlider
// ===========================================================================
FloatSlider::FloatSlider(std::string name_, glm::ivec2 pos_, float min_, float max_, float def_)
    : min(min_), max(max_) {
    name = std::move(name_);
    label = name;
    pos = pos_;
    def = def_;
    set(def_);  // funnels through the virtual set() so an out-of-range default clamps
}

void FloatSlider::draw(Surface& s) {
    glm::vec4 bg = theme ? theme->colorBg : glm::vec4{0.5f, 0.5f, 0.5f, 1.0f};
    glm::vec4 fg = theme ? theme->colorFg : glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};
    int baseline = theme ? theme->labelBaseline : 5;
    glm::vec4 labelColor = theme ? theme->colorLabel : glm::vec4{1.0f, 1.0f, 1.0f, 1.0f};

    fillRect(s, glm::vec2(pos), glm::vec2(size), bg, drawScale);
    // Clamp the drawn fraction, but never clamp the bound value itself here —
    // a sketch writing outside [min,max] directly should still be visible as
    // pinned-at-the-end, not silently altered.
    float t = (max > min) ? std::clamp((get() - min) / (max - min), 0.0f, 1.0f) : 0.0f;
    fillRect(s, glm::vec2(pos), glm::vec2(static_cast<float>(size.x) * t, static_cast<float>(size.y)), fg,
             drawScale);

    textRenderer().ensure(s.device());
    drawText(s, {pos.x + 4, pos.y + baseline}, label, labelColor, drawScale);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", get());
    std::string valueStr = buf;
    int valueW = static_cast<int>(textRenderer().measure(valueStr).x);  // point-space, like pos/size
    drawText(s, {pos.x + size.x - 4 - valueW, pos.y + baseline}, valueStr, labelColor, drawScale);
}

void FloatSlider::onMouseDrag(glm::ivec2 p) {
    set(remap(static_cast<float>(p.x), static_cast<float>(pos.x), static_cast<float>(pos.x + size.x), min, max, true));
}
void FloatSlider::onMousePress(glm::ivec2 p) { onMouseDrag(p); }

void FloatSlider::setFromString(const std::string& s) { set(std::stof(s)); }
std::string FloatSlider::toString() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", get());
    return buf;
}

// ===========================================================================
// IntSlider
// ===========================================================================
IntSlider::IntSlider(std::string name_, glm::ivec2 pos_, int min_, int max_, int def_)
    : min(min_), max(max_) {
    name = std::move(name_);
    label = name;
    pos = pos_;
    def = def_;
    set(def_);
}

void IntSlider::draw(Surface& s) {
    glm::vec4 bg = theme ? theme->colorBg : glm::vec4{0.5f, 0.5f, 0.5f, 1.0f};
    glm::vec4 fg = theme ? theme->colorFg : glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};
    int baseline = theme ? theme->labelBaseline : 5;
    glm::vec4 labelColor = theme ? theme->colorLabel : glm::vec4{1.0f, 1.0f, 1.0f, 1.0f};

    fillRect(s, glm::vec2(pos), glm::vec2(size), bg, drawScale);
    float t = (max > min) ? std::clamp(static_cast<float>(get() - min) / static_cast<float>(max - min), 0.0f, 1.0f) : 0.0f;
    fillRect(s, glm::vec2(pos), glm::vec2(static_cast<float>(size.x) * t, static_cast<float>(size.y)), fg,
             drawScale);

    textRenderer().ensure(s.device());
    drawText(s, {pos.x + 4, pos.y + baseline}, label, labelColor, drawScale);

    std::string valueStr = std::to_string(get());
    int valueW = static_cast<int>(textRenderer().measure(valueStr).x);  // point-space, like pos/size
    drawText(s, {pos.x + size.x - 4 - valueW, pos.y + baseline}, valueStr, labelColor, drawScale);
}

void IntSlider::onMouseDrag(glm::ivec2 p) {
    // Round to nearest so `max` is reachable (truncation would clip it short).
    float t = remap(static_cast<float>(p.x), static_cast<float>(pos.x), static_cast<float>(pos.x + size.x),
                    static_cast<float>(min), static_cast<float>(max), true);
    set(static_cast<int>(std::lround(t)));
}
void IntSlider::onMousePress(glm::ivec2 p) { onMouseDrag(p); }

void IntSlider::setFromString(const std::string& s) { set(std::stoi(s)); }
std::string IntSlider::toString() const { return std::to_string(get()); }

// ===========================================================================
// TinyUI
// ===========================================================================
TinyUI::TinyUI(const std::string& path) { loadLayout(path); }

void TinyUI::setup(Window& w) {
    // Overlay at physical pixels, widget coordinates in points — fillRect/drawText's
    // drawScale bridges them (md/hidpi.md §8). The UI stays crisp even when the
    // sketch opts down with hidpi=false, because the overlay blits onto the
    // always-physical swapchain.
    drawScale_ = w.pixelRatio();
    for (auto& el : widgets_) el->drawScale = drawScale_;
    ui_.emplace(w.device(), w.pixelWidth(), w.pixelHeight());
    mouseSub_ = w.mouseEvents.subscribe([this](MouseEvent e) { handleMouse(e); });
    windowSub_ = w.windowEvents.subscribe([this, &w](WindowEvent e) {
        if (e.type == WindowEvent::Resized && ui_) {
            // Backing-scale migrations arrive as Resized too — re-query the
            // window rather than deriving from the (point-space) payload.
            drawScale_ = w.pixelRatio();
            for (auto& el : widgets_) el->drawScale = drawScale_;
            ui_->resize(w.pixelWidth(), w.pixelHeight());
        }
    });
}

void TinyUI::draw(Window&) {
    if (!ui_) return;
    ui_->clear({0, 0, 0, 0});
    for (auto& w : widgets_) w->draw(*ui_);
}

Surface* TinyUI::overlay() { return (visible && ui_) ? &*ui_ : nullptr; }
Surface& TinyUI::surface() { return *ui_; }

void TinyUI::insert(std::unique_ptr<Element> e) {
    if (!e->name.empty() && find(e->name)) {
        fmt::print(stderr, "[braid-ui] duplicate widget name '{}' — keeping the first\n", e->name);
        return;
    }
    e->theme = &settings;
    e->drawScale = drawScale_;
    widgets_.push_back(std::move(e));
}

void TinyUI::add(std::unique_ptr<Element> e) { insert(std::move(e)); }

Element* TinyUI::find(const std::string& name) const {
    for (auto& w : widgets_)
        if (w->name == name) return w.get();
    return nullptr;
}

void TinyUI::loadLayout(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        fmt::print(stderr, "[braid-ui] TinyUI: could not open layout file '{}'\n", path);
        return;
    }

    glm::ivec2 cursor{settings.elementPadding, settings.elementPadding};
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos || line[start] == '#') continue;

        std::vector<std::string> toks = tokenizeLine(line);
        if (toks.empty()) continue;
        const std::string& type = toks[0];

        std::unique_ptr<Element> e;
        if (type == "label" && toks.size() >= 2) {
            e = std::make_unique<Label>(toks[1], cursor);
        } else if (type == "bool" && toks.size() >= 3) {
            e = std::make_unique<Checkbox>(toks[1], cursor, std::atoi(toks[2].c_str()) != 0);
        } else if (type == "float" && toks.size() >= 5) {
            e = std::make_unique<FloatSlider>(toks[1], cursor, std::stof(toks[2]), std::stof(toks[3]),
                                              std::stof(toks[4]));
        } else if (type == "int" && toks.size() >= 5) {
            e = std::make_unique<IntSlider>(toks[1], cursor, std::stoi(toks[2]), std::stoi(toks[3]),
                                            std::stoi(toks[4]));
        } else {
            fmt::print(stderr, "[braid-ui] {}:{}: unrecognized line '{}'\n", path, lineNo, line);
            continue;
        }
        e->size = settings.elementSize;
        insert(std::move(e));
        cursor.y += settings.elementSize.y + settings.elementSpacing;
    }
}

void TinyUI::handleMouse(const MouseEvent& e) {
    if (!visible) {
        active_ = nullptr;
        hover_ = nullptr;
        return;
    }
    glm::ivec2 p{static_cast<int>(e.pos.x), static_cast<int>(e.pos.y)};

    if (e.button.has_value()) {
        if (e.pressed) {
            active_ = nullptr;
            for (auto& w : widgets_) {
                if (w->hitTest(p)) { active_ = w.get(); break; }
            }
            if (active_) active_->onMousePress(p);
        } else if (active_) {
            active_->onMouseRelease(p);
            active_ = nullptr;
        }
        return;
    }

    // Pure move: forward to the captured widget regardless of hit-test (so
    // dragging a slider past the end of its track keeps working); otherwise
    // just update hover for wantsMouse().
    if (active_) {
        active_->onMouseDrag(p);
        return;
    }
    hover_ = nullptr;
    for (auto& w : widgets_) {
        if (w->hitTest(p)) { hover_ = w.get(); break; }
    }
}

bool TinyUI::wantsMouse() const { return visible && (active_ != nullptr || hover_ != nullptr); }

void TinyUI::resetAll() {
    for (auto& w : widgets_) w->resetToDefault();
}

void TinyUI::reset(const std::string& name) {
    if (Element* e = find(name)) e->resetToDefault();
}

void TinyUI::savePreset(const std::string& path) const {
    std::ofstream out(path);
    if (!out) {
        fmt::print(stderr, "[braid-ui] TinyUI: could not write preset '{}'\n", path);
        return;
    }
    for (auto& w : widgets_) {
        std::string s = w->toString();
        if (!s.empty()) out << w->name << " " << s << "\n";
    }
}

void TinyUI::loadPreset(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        fmt::print(stderr, "[braid-ui] TinyUI: could not read preset '{}'\n", path);
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> toks = tokenizeLine(line);
        if (toks.size() < 2) continue;
        Element* e = find(toks[0]);
        if (!e) {
            fmt::print(stderr, "[braid-ui] preset: unknown widget '{}', ignoring\n", toks[0]);
            continue;
        }
        e->setFromString(toks[1]);
    }
}

void TinyUI::warnOnce(const std::string& name, const std::string& msg) const {
    if (warned_.insert(name).second) {
        fmt::print(stderr, "[braid-ui] '{}': {}\n", name, msg);
    }
}

}  // namespace braid
