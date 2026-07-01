// braid_sketch.cpp — SketchApp (Tier 2): the Processing-like facade. Client-side
// color/style state + a transform stack + camera/projection, plus the default
// always-colored shader and the per-frame batching engine. Primitives don't draw
// immediately — they append geometry + a DrawCmd; emitBatch() uploads the whole
// frame in two writes (one growing vertex buffer + one 256-aligned uniform pool
// addressed by dynamic offset) and replays them as draws.
#include "braid.h"
#include "braid_detail.h"

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace braid {

// Default shader: MVP uniform at @group(0) @binding(0), per-vertex color.
static const char* kDefaultWGSL = R"WGSL(
struct Uniforms { mvp : mat4x4<f32>, tint : vec4<f32> };
@group(0) @binding(0) var<uniform> u : Uniforms;

struct VSOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) color : vec4<f32>,
};

@vertex
fn vs(@location(0) position : vec3<f32>,
      @location(1) uv : vec2<f32>,
      @location(2) normal : vec3<f32>,
      @location(3) color : vec4<f32>) -> VSOut {
    var o : VSOut;
    o.pos = u.mvp * vec4<f32>(position, 1.0);
    o.color = color * u.tint;
    return o;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4<f32> {
    return in.color;
}
)WGSL";

struct SketchUniforms {
    glm::mat4 mvp;
    glm::vec4 tint;
};

// Dynamic-offset alignment: 256 is the spec's max for minUniformBufferOffsetAlignment,
// so a 256-byte stride is valid on every device without querying limits.
static constexpr uint64_t kUniformStride = 256;

SketchApp::SketchApp() : SketchApp(Settings{}) {}
SketchApp::SketchApp(const Settings& settings) : App(settings) {}
SketchApp::SketchApp(Application& app, const Settings& settings) : App(app, settings) {}

// Lazy init so a user-overridden setup() never clashes with ours. Builds the
// default shader module + an explicit pipeline layout whose uniform binding uses
// a dynamic offset (Shader's auto-layout can't, hence the dedicated pipeline).
void SketchApp::ensureReady() {
    if (ready_) return;
    proj_ = glm::ortho(0.0f, static_cast<float>(settings_.width),
                       static_cast<float>(settings_.height), 0.0f, -1.0f, 1.0f);
    view_ = glm::mat4(1.0f);

    wgpu::ShaderSourceWGSL src{};
    src.code = kDefaultWGSL;
    wgpu::ShaderModuleDescriptor smd{};
    smd.nextInChain = &src;
    smd.label = "braid-default";
    sketchModule_ = device_.CreateShaderModule(&smd);

    wgpu::BindGroupLayoutEntry e{};
    e.binding = 0;
    e.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    e.buffer.type = wgpu::BufferBindingType::Uniform;
    e.buffer.hasDynamicOffset = true;
    e.buffer.minBindingSize = sizeof(SketchUniforms);
    wgpu::BindGroupLayoutDescriptor bgld{};
    bgld.entryCount = 1;
    bgld.entries = &e;
    sketchBGL_ = device_.CreateBindGroupLayout(&bgld);

    wgpu::PipelineLayoutDescriptor pld{};
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &sketchBGL_;
    sketchPL_ = device_.CreatePipelineLayout(&pld);

    ready_ = true;
}

void SketchApp::beforeDraw() {
    ensureReady();
    transform_.current = glm::mat4(1.0f);
    transform_.stack.clear();
    bgRequested_ = false;  // omit background() to accumulate (feedback)
    // Pass opens lazily (see ensurePass()/pass()) on first actual draw, after any
    // feedback()/zoom()/rotate()/blur()/multiply()/invert() the user calls in draw()
    // — those ping-pong the Surface's texture, so opening the pass before they run
    // would bind it to the wrong (about-to-be-swapped-out) buffer.
}

void SketchApp::afterDraw() {
    emitBatch();  // upload the frame's geometry + uniforms, record the draws
    flush();      // end the pass
}

void SketchApp::background(float r, float g, float b, float a) {
    clearColor_ = {r, g, b, a};
    bgRequested_ = true;
    // Immediate clear so addons that draw into the active pass see a clean surface.
    wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
    auto pass = surface().begin(enc, clearColor_);
    surface().end(pass);
    wgpu::CommandBuffer cmd = enc.Finish();
    queue_.Submit(1, &cmd);
}
void SketchApp::background(glm::vec4 c) { background(c.r, c.g, c.b, c.a); }
void SketchApp::fill(float r, float g, float b, float a) { state_.fill = {r, g, b, a}; state_.fillEnabled = true; }
void SketchApp::fill(glm::vec4 c) { state_.fill = c; state_.fillEnabled = true; }
void SketchApp::noFill() { state_.fillEnabled = false; }
void SketchApp::stroke(float r, float g, float b, float a) { state_.stroke = {r, g, b, a}; state_.strokeEnabled = true; }
void SketchApp::stroke(glm::vec4 c) { state_.stroke = c; state_.strokeEnabled = true; }
void SketchApp::noStroke() { state_.strokeEnabled = false; }
void SketchApp::strokeWeight(float w) { state_.strokeWeight = w; }

void SketchApp::pushMatrix() { transform_.push(); }
void SketchApp::popMatrix() { transform_.pop(); }
void SketchApp::translate(float x, float y, float z) { transform_.current = glm::translate(transform_.current, {x, y, z}); }
void SketchApp::translate(glm::vec3 t) { transform_.current = glm::translate(transform_.current, t); }
void SketchApp::rotate(float angle) { transform_.current = glm::rotate(transform_.current, angle, {0, 0, 1}); }
void SketchApp::rotate(float angle, float x, float y, float z) { transform_.current = glm::rotate(transform_.current, angle, {x, y, z}); }
void SketchApp::rotateX(float angle) { transform_.current = glm::rotate(transform_.current, angle, {1, 0, 0}); }
void SketchApp::rotateY(float angle) { transform_.current = glm::rotate(transform_.current, angle, {0, 1, 0}); }
void SketchApp::rotateZ(float angle) { transform_.current = glm::rotate(transform_.current, angle, {0, 0, 1}); }
void SketchApp::scale(float s) { transform_.current = glm::scale(transform_.current, {s, s, s}); }
void SketchApp::scale(float x, float y, float z) { transform_.current = glm::scale(transform_.current, {x, y, z}); }

void SketchApp::camera(glm::vec3 eye, glm::vec3 center, glm::vec3 up) { view_ = glm::lookAt(eye, center, up); }
void SketchApp::perspective(float fov, float nearZ, float farZ) {
    float aspect = static_cast<float>(settings_.width) / static_cast<float>(settings_.height);
    proj_ = glm::perspective(fov, aspect, nearZ, farZ);
}
void SketchApp::ortho(float l, float r, float b, float t, float nearZ, float farZ) {
    proj_ = glm::ortho(l, r, b, t, nearZ, farZ);
}

void SketchApp::ensurePass() {
    ensureReady();
    if (!passOpen_) {
        currentPass_ = surface().beginLoad(encoder());
        passOpen_ = true;
        detail::ctx().currentPass = &currentPass_;
    }
}

// Append-only: record geometry + the current MVP/fill; the GPU work happens once
// in emitBatch(). firstVertex indexes into the shared per-frame vertex buffer.
void SketchApp::drawTris(std::span<const Vertex> verts) {
    if (!state_.fillEnabled || verts.empty()) return;
    DrawCmd c;
    c.firstVertex = static_cast<uint32_t>(batchVerts_.size());
    c.vertexCount = static_cast<uint32_t>(verts.size());
    c.mvp = proj_ * view_ * transform_.current;
    c.tint = state_.fill;
    c.lines = false;
    batchVerts_.insert(batchVerts_.end(), verts.begin(), verts.end());
    batchCmds_.push_back(c);
}

void SketchApp::drawLines(std::span<const Vertex> verts) {
    if (verts.empty()) return;
    DrawCmd c;
    c.firstVertex = static_cast<uint32_t>(batchVerts_.size());
    c.vertexCount = static_cast<uint32_t>(verts.size());
    c.mvp = proj_ * view_ * transform_.current;
    c.tint = state_.fill;
    c.lines = true;
    batchVerts_.insert(batchVerts_.end(), verts.begin(), verts.end());
    batchCmds_.push_back(c);
}

wgpu::RenderPipeline SketchApp::sketchPipeline(wgpu::TextureFormat fmt, bool lines) {
    for (auto& sp : sketchPipelines_)
        if (sp.format == fmt && sp.lines == lines) return sp.pipeline;

    wgpu::ColorTargetState target{};
    target.format = fmt;
    target.blend = &Blend::Alpha;
    target.writeMask = wgpu::ColorWriteMask::All;
    wgpu::FragmentState frag{};
    frag.module = sketchModule_;
    frag.entryPoint = "fs";
    frag.targetCount = 1;
    frag.targets = &target;
    wgpu::VertexBufferLayout vbl = Mesh::vertexLayout();
    wgpu::RenderPipelineDescriptor desc{};
    desc.layout = sketchPL_;
    desc.vertex.module = sketchModule_;
    desc.vertex.entryPoint = "vs";
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &vbl;
    desc.fragment = &frag;
    desc.primitive.topology =
        lines ? wgpu::PrimitiveTopology::LineList : wgpu::PrimitiveTopology::TriangleList;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFF;
    wgpu::RenderPipeline p = device_.CreateRenderPipeline(&desc);
    sketchPipelines_.push_back({fmt, lines, p});
    return p;
}

void SketchApp::emitBatch() {
    if (batchCmds_.empty()) return;
    ensureReady();

    // 1) Grow + upload the vertex buffer (one write for the whole frame).
    const size_t needVerts = batchVerts_.size();
    if (needVerts > vboCapacity_) {
        vboCapacity_ = std::max<size_t>(needVerts, vboCapacity_ ? vboCapacity_ * 2 : 4096);
        wgpu::BufferDescriptor bd{};
        bd.size = vboCapacity_ * sizeof(Vertex);
        bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        vbo_ = device_.CreateBuffer(&bd);
    }
    device_.GetQueue().WriteBuffer(vbo_, 0, batchVerts_.data(), needVerts * sizeof(Vertex));

    // 2) Grow + upload the uniform pool — one 256-aligned slot per draw. Growing
    //    re-creates the single bind group bound over the whole pool (offset = dynamic).
    const size_t needSlots = batchCmds_.size();
    if (needSlots > uboCapacity_) {
        uboCapacity_ = std::max<size_t>(needSlots, uboCapacity_ ? uboCapacity_ * 2 : 256);
        wgpu::BufferDescriptor bd{};
        bd.size = uboCapacity_ * kUniformStride;
        bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        ubo_ = device_.CreateBuffer(&bd);
        wgpu::BindGroupEntry e{};
        e.binding = 0;
        e.buffer = ubo_;
        e.offset = 0;
        e.size = sizeof(SketchUniforms);  // the dynamic window, not the whole pool
        wgpu::BindGroupDescriptor bgd{};
        bgd.layout = sketchBGL_;
        bgd.entryCount = 1;
        bgd.entries = &e;
        uboBindGroup_ = device_.CreateBindGroup(&bgd);
    }
    std::vector<uint8_t> staging(needSlots * kUniformStride, 0);
    for (size_t i = 0; i < needSlots; ++i) {
        SketchUniforms u{batchCmds_[i].mvp, batchCmds_[i].tint};
        std::memcpy(staging.data() + i * kUniformStride, &u, sizeof(u));
    }
    device_.GetQueue().WriteBuffer(ubo_, 0, staging.data(), staging.size());

    // 3) Replay: one pass, pipeline switched only when topology changes.
    ensurePass();
    const wgpu::TextureFormat fmt = surface().format();
    WGPURenderPipeline cur = nullptr;
    for (size_t i = 0; i < needSlots; ++i) {
        const DrawCmd& c = batchCmds_[i];
        wgpu::RenderPipeline p = sketchPipeline(fmt, c.lines);
        if (p.Get() != cur) { currentPass_.SetPipeline(p); cur = p.Get(); }
        currentPass_.SetVertexBuffer(0, vbo_, c.firstVertex * sizeof(Vertex),
                                     c.vertexCount * sizeof(Vertex));
        uint32_t off = static_cast<uint32_t>(i * kUniformStride);
        currentPass_.SetBindGroup(0, uboBindGroup_, 1, &off);
        currentPass_.Draw(c.vertexCount);
    }
    batchVerts_.clear();
    batchCmds_.clear();
}

void SketchApp::box(float size) { box(size, size, size); }

void SketchApp::box(float w, float h, float d) {
    const float x = w * 0.5f, y = h * 0.5f, z = d * 0.5f;
    const glm::vec3 c[8] = {{-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z},
                            {-x, -y, z},  {x, -y, z},  {x, y, z},  {-x, y, z}};
    static const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0},   // back face
                                 {4, 5}, {5, 6}, {6, 7}, {7, 4},   // front face
                                 {0, 4}, {1, 5}, {2, 6}, {3, 7}};  // connecting edges
    std::array<Vertex, 24> v{};
    for (int i = 0; i < 12; ++i) {
        v[i * 2 + 0] = {c[e[i][0]], {}, {0, 0, 1}, {1, 1, 1, 1}};
        v[i * 2 + 1] = {c[e[i][1]], {}, {0, 0, 1}, {1, 1, 1, 1}};
    }
    drawLines(v);
}

void SketchApp::rect(float x, float y, float w, float h) {
    std::array<Vertex, 6> v{};
    auto V = [](float px, float py) {
        return Vertex{{px, py, 0}, {0, 0}, {0, 0, 1}, {1, 1, 1, 1}};
    };
    v = {V(x, y), V(x + w, y), V(x + w, y + h), V(x, y), V(x + w, y + h), V(x, y + h)};
    drawTris(v);
    std::array<glm::vec2, 4> pts{glm::vec2{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    strokeOutline(pts, true);
}

void SketchApp::triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    std::array<Vertex, 3> v{Vertex{{a, 0}, {}, {0, 0, 1}, {1, 1, 1, 1}},
                            Vertex{{b, 0}, {}, {0, 0, 1}, {1, 1, 1, 1}},
                            Vertex{{c, 0}, {}, {0, 0, 1}, {1, 1, 1, 1}}};
    drawTris(v);
    std::array<glm::vec2, 3> pts{a, b, c};
    strokeOutline(pts, true);
}

void SketchApp::fillQuad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d) {
    auto V = [](glm::vec2 p) { return Vertex{{p, 0}, {}, {0, 0, 1}, {1, 1, 1, 1}}; };
    std::array<Vertex, 6> v{V(a), V(b), V(c), V(a), V(c), V(d)};
    drawTris(v);
}

void SketchApp::quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d) {
    fillQuad(a, b, c, d);
    std::array<glm::vec2, 4> pts{a, b, c, d};
    strokeOutline(pts, true);
}

void SketchApp::ellipse(float x, float y, float w, float h) {
    const int seg = 48;
    const float rx = w * 0.5f, ry = h * 0.5f;
    std::vector<glm::vec2> pts(seg);
    for (int i = 0; i < seg; ++i) {
        float a = (float(i) / seg) * 6.2831853f;
        pts[i] = {x + std::cos(a) * rx, y + std::sin(a) * ry};
    }
    std::vector<Vertex> v;
    v.reserve(seg * 3);
    auto V = [](float px, float py) { return Vertex{{px, py, 0}, {}, {0, 0, 1}, {1, 1, 1, 1}}; };
    for (int i = 0; i < seg; ++i) {
        int j = (i + 1) % seg;
        v.push_back(V(x, y));
        v.push_back(V(pts[i].x, pts[i].y));
        v.push_back(V(pts[j].x, pts[j].y));
    }
    drawTris(v);
    strokeOutline(pts, true);
}

void SketchApp::circle(float x, float y, float r) { ellipse(x, y, r * 2, r * 2); }

void SketchApp::strokeOutline(std::span<const glm::vec2> pts, bool closed) {
    if (!state_.strokeEnabled || pts.size() < 2) return;
    glm::vec4 save = state_.fill;
    bool savedEnabled = state_.fillEnabled;
    state_.fill = state_.stroke;
    state_.fillEnabled = true;
    const size_t n = pts.size();
    const size_t segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i) {
        glm::vec2 a = pts[i], b = pts[(i + 1) % n];
        glm::vec2 dir = b - a;
        float len = glm::length(dir);
        if (len < 1e-5f) continue;
        glm::vec2 nrm = glm::vec2(-dir.y, dir.x) / len * (state_.strokeWeight * 0.5f);
        fillQuad(a + nrm, b + nrm, b - nrm, a - nrm);
    }
    state_.fill = save;
    state_.fillEnabled = savedEnabled;
}

void SketchApp::line(float x1, float y1, float x2, float y2) {
    std::array<glm::vec2, 2> pts{glm::vec2{x1, y1}, {x2, y2}};
    strokeOutline(pts, false);
}

void SketchApp::point(float x, float y) {
    float r = state_.strokeWeight * 0.5f;
    glm::vec4 save = state_.fill;
    bool savedStroke = state_.strokeEnabled;
    state_.fill = state_.stroke;
    state_.strokeEnabled = false;  // suppress ellipse()'s own outline pass
    ellipse(x, y, r * 2, r * 2);
    state_.fill = save;
    state_.strokeEnabled = savedStroke;
}

wgpu::RenderPassEncoder& SketchApp::pass() { emitBatch(); ensurePass(); return currentPass_; }

void SketchApp::flush() {
    if (passOpen_) {
        surface().end(currentPass_);
        passOpen_ = false;
        detail::ctx().currentPass = nullptr;
    }
}

void SketchApp::submitFrame() {
    emitBatch();  // upload + record any primitives drawn so far this frame
    flush();      // close the pass so its commands are legal to submit
    App::submitFrame();
}

}  // namespace braid
