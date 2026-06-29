// braid.cpp — Braid v0.1.0 implementation (macOS).
//
// Two spots below are sensitive to the exact prebuilt Dawn / RGFW versions and
// are marked [VERIFY]:
//   1. Adapter/device request (Dawn moved this to a future+callback API).
//   2. The macOS Metal surface (CAMetalLayer attach + surface descriptor).
// Both are isolated so they can be reconciled without touching the rest.

#include "braid.h"
#include "braid_detail.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include <fmt/core.h>
#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>

// RGFW: single-header windowing. Implementation compiled here, macOS/Cocoa.
#define RGFW_IMPLEMENTATION
#include <RGFW.h>

#include <objc/message.h>
#include <objc/runtime.h>

#include <webgpu/webgpu_cpp.h>

namespace braid {

// ===========================================================================
// Blend presets
// ===========================================================================
namespace Blend {
static wgpu::BlendState makeAlpha() {
    wgpu::BlendState b{};
    b.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha,
               wgpu::BlendFactor::OneMinusSrcAlpha};
    b.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One,
               wgpu::BlendFactor::OneMinusSrcAlpha};
    return b;
}
static wgpu::BlendState makeAdditive() {
    wgpu::BlendState b{};
    b.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha, wgpu::BlendFactor::One};
    b.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::One};
    return b;
}
static wgpu::BlendState makeNone() {
    wgpu::BlendState b{};
    b.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::Zero};
    b.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::Zero};
    return b;
}
const wgpu::BlendState Alpha = makeAlpha();
const wgpu::BlendState Additive = makeAdditive();
const wgpu::BlendState None = makeNone();
}  // namespace Blend

// ===========================================================================
// Internal: Context + Compositor — the single blit engine behind every Surface
// operation (show, composite, transforms, feedback, save). One fullscreen-quad
// shader that samples a source view with a UV transform + invert + tint and
// blends it into a destination view.
// ===========================================================================
namespace detail {

// Context type is declared in braid_detail.h (shared with addon TUs).
static Context g_ctx;
void setContext(wgpu::Instance i, wgpu::Device d) { g_ctx.instance = i; g_ctx.device = d; }
Context& ctx() { return g_ctx; }

struct BlitUniforms {
    glm::vec2 uvScale{1, 1};
    glm::vec2 uvOffset{0, 0};
    glm::vec4 tint{1, 1, 1, 1};
    float rot = 0.0f;
    float invert = 0.0f;
    glm::vec2 _pad{0, 0};
};

static const char* kBlitWGSL = R"WGSL(
struct Blit {
  uvScale : vec2<f32>, uvOffset : vec2<f32>, tint : vec4<f32>,
  rot : f32, invert : f32, pad : vec2<f32>,
};
@group(0) @binding(0) var<uniform> u : Blit;
@group(0) @binding(1) var tex : texture_2d<f32>;
@group(0) @binding(2) var samp : sampler;
struct VO { @builtin(position) pos : vec4<f32>, @location(0) uv : vec2<f32> };
@vertex fn vs(@builtin(vertex_index) i : u32) -> VO {
  var ps = array<vec2<f32>, 3>(vec2<f32>(-1.0,-1.0), vec2<f32>(3.0,-1.0), vec2<f32>(-1.0,3.0));
  let p = ps[i];
  var o : VO;
  o.pos = vec4<f32>(p, 0.0, 1.0);
  o.uv  = vec2<f32>((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
  return o;
}
@fragment fn fs(in : VO) -> @location(0) vec4<f32> {
  var uv = in.uv - vec2<f32>(0.5, 0.5);
  let c = cos(u.rot); let s = sin(u.rot);
  uv = vec2<f32>(uv.x * c - uv.y * s, uv.x * s + uv.y * c);
  uv = uv / u.uvScale;
  uv = uv + vec2<f32>(0.5, 0.5) + u.uvOffset;
  var col = textureSample(tex, samp, uv);
  if (u.invert > 0.5) { col = vec4<f32>(vec3<f32>(1.0) - col.rgb, col.a); }
  return col * u.tint;
}
)WGSL";

// Placed-quad paste: sample the full source across a quad that is scaled,
// rotated, and positioned in NDC. Same bind layout as the blit (uniform/tex/
// sampler) — a full-frame quad (center 0, half 1, rot 0) reproduces the source.
struct QuadUniforms {
    glm::vec2 center{0, 0};    // NDC, [-1,1]
    glm::vec2 halfSize{1, 1};  // NDC half-extent (before rotation)
    glm::vec4 tint{1, 1, 1, 1};
    float rot = 0.0f;          // radians, about quad center
    float invert = 0.0f;
    glm::vec2 _pad{0, 0};
};

static const char* kQuadWGSL = R"WGSL(
struct Quad {
  center : vec2<f32>, halfSize : vec2<f32>, tint : vec4<f32>,
  rot : f32, invert : f32, pad : vec2<f32>,
};
@group(0) @binding(0) var<uniform> u : Quad;
@group(0) @binding(1) var tex : texture_2d<f32>;
@group(0) @binding(2) var samp : sampler;
struct VO { @builtin(position) pos : vec4<f32>, @location(0) uv : vec2<f32> };
@vertex fn vs(@builtin(vertex_index) i : u32) -> VO {
  var cs = array<vec2<f32>, 6>(
    vec2<f32>(-1.0,-1.0), vec2<f32>( 1.0,-1.0), vec2<f32>(-1.0, 1.0),
    vec2<f32>(-1.0, 1.0), vec2<f32>( 1.0,-1.0), vec2<f32>( 1.0, 1.0));
  let corner = cs[i];
  let c = cos(u.rot); let s = sin(u.rot);
  let sc = corner * u.halfSize;
  let rt = vec2<f32>(sc.x * c - sc.y * s, sc.x * s + sc.y * c);
  let p = rt + u.center;
  var o : VO;
  o.pos = vec4<f32>(p, 0.0, 1.0);
  o.uv  = vec2<f32>((corner.x + 1.0) * 0.5, 1.0 - (corner.y + 1.0) * 0.5);
  return o;
}
@fragment fn fs(in : VO) -> @location(0) vec4<f32> {
  var col = textureSample(tex, samp, in.uv);
  if (u.invert > 0.5) { col = vec4<f32>(vec3<f32>(1.0) - col.rgb, col.a); }
  return col * u.tint;
}
)WGSL";

class Compositor {
public:
    void init(wgpu::Device device) {
        if (ready_) return;
        device_ = device;

        wgpu::ShaderSourceWGSL src{};
        src.code = kBlitWGSL;
        wgpu::ShaderModuleDescriptor smd{};
        smd.nextInChain = &src;
        smd.label = "braid-blit";
        module_ = device_.CreateShaderModule(&smd);

        wgpu::ShaderSourceWGSL qsrc{};
        qsrc.code = kQuadWGSL;
        wgpu::ShaderModuleDescriptor qsmd{};
        qsmd.nextInChain = &qsrc;
        qsmd.label = "braid-quad";
        quadModule_ = device_.CreateShaderModule(&qsmd);

        wgpu::SamplerDescriptor sd{};
        sd.magFilter = wgpu::FilterMode::Linear;
        sd.minFilter = wgpu::FilterMode::Linear;
        sd.addressModeU = wgpu::AddressMode::ClampToEdge;
        sd.addressModeV = wgpu::AddressMode::ClampToEdge;
        sampler_ = device_.CreateSampler(&sd);

        std::array<wgpu::BindGroupLayoutEntry, 3> e{};
        e[0].binding = 0;
        e[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        e[0].buffer.type = wgpu::BufferBindingType::Uniform;
        e[1].binding = 1;
        e[1].visibility = wgpu::ShaderStage::Fragment;
        e[1].texture.sampleType = wgpu::TextureSampleType::Float;
        e[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        e[2].binding = 2;
        e[2].visibility = wgpu::ShaderStage::Fragment;
        e[2].sampler.type = wgpu::SamplerBindingType::Filtering;
        wgpu::BindGroupLayoutDescriptor bgld{};
        bgld.entryCount = e.size();
        bgld.entries = e.data();
        bgl_ = device_.CreateBindGroupLayout(&bgld);

        wgpu::PipelineLayoutDescriptor pld{};
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &bgl_;
        pl_ = device_.CreatePipelineLayout(&pld);
        ready_ = true;
    }

    void blit(wgpu::CommandEncoder& enc, wgpu::TextureView dest, wgpu::TextureFormat destFormat,
              wgpu::TextureView src, const BlitUniforms& u, const wgpu::BlendState& blend,
              wgpu::LoadOp loadOp) {
        // uniform from a tiny ring
        wgpu::Buffer ub = ring_[ringIndex_];
        if (!ub) {
            wgpu::BufferDescriptor bd{};
            bd.size = sizeof(BlitUniforms);
            bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            ub = device_.CreateBuffer(&bd);
            ring_[ringIndex_] = ub;
        }
        ringIndex_ = (ringIndex_ + 1) % ring_.size();
        device_.GetQueue().WriteBuffer(ub, 0, &u, sizeof(u));

        std::array<wgpu::BindGroupEntry, 3> be{};
        be[0].binding = 0;
        be[0].buffer = ub;
        be[0].size = sizeof(BlitUniforms);
        be[1].binding = 1;
        be[1].textureView = src;
        be[2].binding = 2;
        be[2].sampler = sampler_;
        wgpu::BindGroupDescriptor bgd{};
        bgd.layout = bgl_;
        bgd.entryCount = be.size();
        bgd.entries = be.data();
        wgpu::BindGroup bg = device_.CreateBindGroup(&bgd);

        wgpu::RenderPassColorAttachment att{};
        att.view = dest;
        att.loadOp = loadOp;
        att.storeOp = wgpu::StoreOp::Store;
        att.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor rpd{};
        rpd.colorAttachmentCount = 1;
        rpd.colorAttachments = &att;

        wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rpd);
        pass.SetPipeline(pipelineFor(destFormat, blend));
        pass.SetBindGroup(0, bg);
        pass.Draw(3);
        pass.End();
    }

    // Paste `src` onto a placed/rotated/scaled quad of `dest` (preserve via Load).
    void quad(wgpu::CommandEncoder& enc, wgpu::TextureView dest, wgpu::TextureFormat destFormat,
              wgpu::TextureView src, const QuadUniforms& u, const wgpu::BlendState& blend,
              wgpu::LoadOp loadOp) {
        wgpu::Buffer ub = ring_[ringIndex_];
        if (!ub) {
            wgpu::BufferDescriptor bd{};
            bd.size = sizeof(QuadUniforms);
            bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
            ub = device_.CreateBuffer(&bd);
            ring_[ringIndex_] = ub;
        }
        ringIndex_ = (ringIndex_ + 1) % ring_.size();
        device_.GetQueue().WriteBuffer(ub, 0, &u, sizeof(u));

        std::array<wgpu::BindGroupEntry, 3> be{};
        be[0].binding = 0;
        be[0].buffer = ub;
        be[0].size = sizeof(QuadUniforms);
        be[1].binding = 1;
        be[1].textureView = src;
        be[2].binding = 2;
        be[2].sampler = sampler_;
        wgpu::BindGroupDescriptor bgd{};
        bgd.layout = bgl_;
        bgd.entryCount = be.size();
        bgd.entries = be.data();
        wgpu::BindGroup bg = device_.CreateBindGroup(&bgd);

        wgpu::RenderPassColorAttachment att{};
        att.view = dest;
        att.loadOp = loadOp;
        att.storeOp = wgpu::StoreOp::Store;
        att.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor rpd{};
        rpd.colorAttachmentCount = 1;
        rpd.colorAttachments = &att;

        wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rpd);
        pass.SetPipeline(quadPipelineFor(destFormat, blend));
        pass.SetBindGroup(0, bg);
        pass.Draw(6);
        pass.End();
    }

private:
    wgpu::RenderPipeline pipelineFor(wgpu::TextureFormat fmt, const wgpu::BlendState& blend) {
        for (auto& [f, b, p] : cache_)
            if (f == fmt && b == &blend) return p;

        wgpu::ColorTargetState target{};
        target.format = fmt;
        target.blend = &blend;
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
        rpd.fragment = &frag;
        rpd.multisample.count = 1;
        rpd.multisample.mask = 0xFFFFFFFF;
        wgpu::RenderPipeline p = device_.CreateRenderPipeline(&rpd);
        cache_.push_back({fmt, &blend, p});
        return p;
    }

    wgpu::RenderPipeline quadPipelineFor(wgpu::TextureFormat fmt, const wgpu::BlendState& blend) {
        for (auto& [f, b, p] : quadCache_)
            if (f == fmt && b == &blend) return p;

        wgpu::ColorTargetState target{};
        target.format = fmt;
        target.blend = &blend;
        target.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState frag{};
        frag.module = quadModule_;
        frag.entryPoint = "fs";
        frag.targetCount = 1;
        frag.targets = &target;
        wgpu::RenderPipelineDescriptor rpd{};
        rpd.layout = pl_;
        rpd.vertex.module = quadModule_;
        rpd.vertex.entryPoint = "vs";
        rpd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        rpd.fragment = &frag;
        rpd.multisample.count = 1;
        rpd.multisample.mask = 0xFFFFFFFF;
        wgpu::RenderPipeline p = device_.CreateRenderPipeline(&rpd);
        quadCache_.push_back({fmt, &blend, p});
        return p;
    }

    wgpu::Device device_;
    wgpu::ShaderModule module_;
    wgpu::ShaderModule quadModule_;
    wgpu::Sampler sampler_;
    wgpu::BindGroupLayout bgl_;
    wgpu::PipelineLayout pl_;
    std::array<wgpu::Buffer, 3> ring_{};
    int ringIndex_ = 0;
    std::vector<std::tuple<wgpu::TextureFormat, const void*, wgpu::RenderPipeline>> cache_;
    std::vector<std::tuple<wgpu::TextureFormat, const void*, wgpu::RenderPipeline>> quadCache_;
    bool ready_ = false;
};

static Compositor g_compositor;
Compositor& compositor() {
    g_compositor.init(ctx().device);
    return g_compositor;
}

}  // namespace detail

// ===========================================================================
// Timer — ofTimerFps port
// ===========================================================================
Timer::Timer(int targetFps) : targetFps_(targetFps) {
    interval_ = nanos(1'000'000'000LL / std::max(1, targetFps));
    reset();
}

void Timer::setFps(int fps) {
    targetFps_ = std::max(1, fps);
    interval_ = nanos(1'000'000'000LL / targetFps_);
}

void Timer::reset() {
    startTime_ = clock::now();
    wakeTime_ = startTime_ + interval_;
    lastWakeTime_ = startTime_;
    frames_ = 0;
    delta_ = 0.0f;
    elapsed_ = 0.0f;
}

void Timer::waitNext() {
    // Uncapped: targetFps <= 0 means run free — no pacing, just measure.
    if (targetFps_ <= 0) {
        const auto now = clock::now();
        delta_ = std::chrono::duration<float>(now - lastWakeTime_).count();
        elapsed_ = std::chrono::duration<float>(now - startTime_).count();
        lastWakeTime_ = now;
        ++frames_;
        return;
    }
    // 1) Lazy sleep until ~3ms before the target wake time.
    const auto slack = std::chrono::milliseconds(3);
    if (wakeTime_ - slack > clock::now()) {
        std::this_thread::sleep_until(wakeTime_ - slack);
    }
    // 2) Tight yield spin for the final stretch.
    while (clock::now() < wakeTime_) {
        std::this_thread::yield();
    }
    // 3) Bookkeeping + advance the target.
    const auto now = clock::now();
    delta_ = std::chrono::duration<float>(now - lastWakeTime_).count();
    elapsed_ = std::chrono::duration<float>(now - startTime_).count();
    lastWakeTime_ = now;
    wakeTime_ += interval_;
    // If we fell badly behind, don't spiral — resync the target.
    if (wakeTime_ < now) wakeTime_ = now + interval_;
    ++frames_;
}

int Timer::currentFps() const {
    return delta_ > 0.0f ? static_cast<int>(std::lround(1.0f / delta_)) : targetFps_;
}

// ===========================================================================
// Surface
// ===========================================================================
Surface::Surface(wgpu::Device device, int width, int height, wgpu::TextureFormat format)
    : device_(device), width_(width), height_(height), format_(format), swapchain_(false) {
    wgpu::TextureDescriptor td{};
    td.size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    td.format = format;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
               wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
    texture_ = device_.CreateTexture(&td);
    view_ = texture_.CreateView();
}

Surface::Surface(wgpu::Surface surface, int width, int height, wgpu::TextureFormat format)
    : surface_(surface), width_(width), height_(height), format_(format), swapchain_(true) {}

Result<Surface> Surface::clone() const {
    if (swapchain_) {
        return Result<Surface>::failure("Surface::clone() is not valid on the swapchain Surface");
    }
    Surface out(device_, width_, height_, format_);
    // GPU→GPU copy of the texture contents.
    wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src{}, dst{};
    src.texture = texture_;
    dst.texture = out.texture_;
    wgpu::Extent3D ext{static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
    enc.CopyTextureToTexture(&src, &dst, &ext);
    wgpu::CommandBuffer cmd = enc.Finish();
    device_.GetQueue().Submit(1, &cmd);
    return Result<Surface>::success(std::move(out));
}

wgpu::RenderPassEncoder Surface::begin(wgpu::CommandEncoder& encoder, glm::vec4 clearColor) {
    wgpu::RenderPassColorAttachment color{};
    color.view = view_;
    color.loadOp = wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};

    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    return encoder.BeginRenderPass(&desc);
}

void Surface::end(wgpu::RenderPassEncoder& pass) { pass.End(); }

wgpu::TextureView Surface::asTexture() const { return view_; }
bool Surface::isValid() const { return swapchain_ ? (surface_ != nullptr) : (texture_ != nullptr); }
void Surface::setCurrentView(wgpu::TextureView view) { view_ = view; }

void Surface::resize(int width, int height) {
    width_ = width;
    height_ = height;
    if (!swapchain_) {
        wgpu::TextureDescriptor td{};
        td.size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        td.format = format_;
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                   wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
        texture_ = device_.CreateTexture(&td);
        view_ = texture_.CreateView();
        scratch_ = nullptr;  // dropped; lazily re-made at new size
        scratchView_ = nullptr;
    }
}

wgpu::RenderPassEncoder Surface::beginLoad(wgpu::CommandEncoder& encoder) {
    wgpu::RenderPassColorAttachment color{};
    color.view = view_;
    color.loadOp = wgpu::LoadOp::Load;  // preserve existing contents (feedback)
    color.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    return encoder.BeginRenderPass(&desc);
}

// --- Surface algebra ---------------------------------------------------------
// Total semantics (Fix: no guards, only definitions): an invalid/empty operand
// is the additive identity (no-op); size/format differences are absorbed by the
// blit (UV-mapped sample), never an error.
Surface& Surface::compositeFrom(const Surface& src, const wgpu::BlendState& blend, glm::vec4 tint) {
    if (swapchain_ || !src.view_ || !view_) return *this;  // empty = identity
    detail::BlitUniforms u{};
    u.tint = tint;
    wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
    detail::compositor().blit(enc, view_, format_, src.view_, u, blend, wgpu::LoadOp::Load);
    wgpu::CommandBuffer cmd = enc.Finish();
    device_.GetQueue().Submit(1, &cmd);
    return *this;
}

Surface& Surface::operator+=(const Surface& src) { return compositeFrom(src, Blend::Additive); }
Surface& Surface::over(const Surface& src) { return compositeFrom(src, Blend::Alpha); }

// --- In-place transforms (hidden ping-pong: render self → scratch, swap) ------
void Surface::ensureScratch() {
    if (scratch_) return;
    wgpu::TextureDescriptor td{};
    td.size = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
    td.format = format_;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
               wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
    scratch_ = device_.CreateTexture(&td);
    scratchView_ = scratch_.CreateView();
}

void Surface::swapScratch() {
    std::swap(texture_, scratch_);
    std::swap(view_, scratchView_);
}

void Surface::selfTransform(glm::vec2 uvScale, glm::vec2 uvOffset, float rot, glm::vec4 tint,
                            bool invertFlag) {
    if (swapchain_ || !view_) return;
    ensureScratch();
    detail::BlitUniforms u{};
    u.uvScale = uvScale;
    u.uvOffset = uvOffset;
    u.rot = rot;
    u.tint = tint;
    u.invert = invertFlag ? 1.0f : 0.0f;
    wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
    detail::compositor().blit(enc, scratchView_, format_, view_, u, Blend::None, wgpu::LoadOp::Clear);
    wgpu::CommandBuffer cmd = enc.Finish();
    device_.GetQueue().Submit(1, &cmd);
    swapScratch();  // scratch now holds the transformed result → becomes main
}

Surface& Surface::zoom(float factor) { selfTransform({factor, factor}, {0, 0}, 0, {1, 1, 1, 1}, false); return *this; }
Surface& Surface::rotate(float radians) { selfTransform({1, 1}, {0, 0}, radians, {1, 1, 1, 1}, false); return *this; }
Surface& Surface::shift(float dx, float dy) {
    selfTransform({1, 1}, {-dx / width_, dy / height_}, 0, {1, 1, 1, 1}, false);
    return *this;
}
Surface& Surface::invert() { selfTransform({1, 1}, {0, 0}, 0, {1, 1, 1, 1}, true); return *this; }
Surface& Surface::multiply(glm::vec4 c) { selfTransform({1, 1}, {0, 0}, 0, c, false); return *this; }

Surface& Surface::clear(glm::vec4 c) {
    if (!view_) return *this;
    wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
    auto pass = begin(enc, c);
    end(pass);
    wgpu::CommandBuffer cmd = enc.Finish();
    device_.GetQueue().Submit(1, &cmd);
    return *this;
}

// --- Self-feedback: transform current contents, then decay by gain ------------
Surface& Surface::feedback(float gain, const std::function<void(Surface&)>& transform) {
    transform(*this);
    if (gain != 1.0f) multiply({gain, gain, gain, 1.0f});
    return *this;
}

// --- Placed paste (positioned/rotated/scaled quad) ---------------------------
static detail::QuadUniforms makeQuadUniforms(int w, int h, glm::vec2 center, glm::vec2 size,
                                             float rot, glm::vec4 tint, bool invert) {
    detail::QuadUniforms u{};
    // pixel (top-left origin) → NDC (y-up). half-extent in NDC is size/dim.
    u.center = {center.x / w * 2.0f - 1.0f, 1.0f - center.y / h * 2.0f};
    u.halfSize = {size.x / w, size.y / h};
    u.rot = rot;
    u.tint = tint;
    u.invert = invert ? 1.0f : 0.0f;
    return u;
}

Surface& Surface::paste(const Surface& src, glm::vec2 center, glm::vec2 size, float rotation,
                        const wgpu::BlendState& blend, glm::vec4 tint, bool invert) {
    if (swapchain_ || !view_ || !src.view_) return *this;  // total: empty = identity
    auto u = makeQuadUniforms(width_, height_, center, size, rotation, tint, invert);
    wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
    detail::compositor().quad(enc, view_, format_, src.view_, u, blend, wgpu::LoadOp::Load);
    wgpu::CommandBuffer cmd = enc.Finish();
    device_.GetQueue().Submit(1, &cmd);
    return *this;
}

Surface& Surface::pasteSelf(glm::vec2 center, glm::vec2 size, float rotation,
                            const wgpu::BlendState& blend, glm::vec4 tint, bool invert) {
    if (swapchain_ || !view_) return *this;
    ensureScratch();
    // Snapshot current contents into scratch so the read source is distinct from
    // the render target (can't sample + write the same texture in one pass).
    wgpu::CommandEncoder cp = device_.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo s{}, d{};
    s.texture = texture_;
    d.texture = scratch_;
    wgpu::Extent3D ext{static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
    cp.CopyTextureToTexture(&s, &d, &ext);
    wgpu::CommandBuffer cpc = cp.Finish();
    device_.GetQueue().Submit(1, &cpc);
    // Paste the snapshot back as a placed quad over the (preserved) contents.
    auto u = makeQuadUniforms(width_, height_, center, size, rotation, tint, invert);
    wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
    detail::compositor().quad(enc, view_, format_, scratchView_, u, blend, wgpu::LoadOp::Load);
    wgpu::CommandBuffer cmd = enc.Finish();
    device_.GetQueue().Submit(1, &cmd);
    return *this;
}

// Surface::load / Surface::save (image file I/O) are defined in the braid-image
// addon (braid_image.cpp), which links the mango decode/encode stack. braid-core
// stays free of mango and all its codec archives — link braid-image to enable.

// ===========================================================================
// Shader
// ===========================================================================
wgpu::Buffer Shader::UniformRing::allocate(wgpu::Device device, size_t size) {
    auto& buf = buffers[index];
    auto& cap = capacities[index];
    if (!buf || cap < size) {
        wgpu::BufferDescriptor bd{};
        bd.size = size;
        bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        buf = device.CreateBuffer(&bd);
        cap = size;
    }
    wgpu::Buffer chosen = buf;
    index = (index + 1) % 3;
    return chosen;
}

Result<Shader> Shader::load(wgpu::Device device, const LoadOptions& opts) {
    if (!opts.wgsl) return Result<Shader>::failure("Shader::load: wgsl source is null");

    wgpu::ShaderSourceWGSL wgslDesc{};
    wgslDesc.code = opts.wgsl;

    wgpu::ShaderModuleDescriptor smd{};
    smd.nextInChain = &wgslDesc;
    smd.label = opts.label;

    Shader s;
    s.device_ = device;
    s.module_ = device.CreateShaderModule(&smd);
    s.vertexEntry_ = opts.vertexEntry;
    s.fragmentEntry_ = opts.fragmentEntry;
    if (!s.module_) return Result<Shader>::failure("Shader::load: module creation failed");
    return Result<Shader>::success(std::move(s));
}

Result<Shader> Shader::loadFile(wgpu::Device device, const char* path, bool debug) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return Result<Shader>::failure(fmt::format("Shader::loadFile: cannot open {}", path));
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string src(static_cast<size_t>(n), '\0');
    std::fread(src.data(), 1, static_cast<size_t>(n), f);
    std::fclose(f);
    LoadOptions opts{};
    opts.wgsl = src.c_str();
    opts.label = path;
    opts.debug = debug;
    return load(device, opts);
}

bool Shader::PipelineKey::operator==(const PipelineKey& o) const {
    return layoutHash == o.layoutHash && format == o.format && blend == o.blend &&
           topology == o.topology;
}
size_t Shader::PipelineKeyHash::operator()(const PipelineKey& k) const {
    size_t h = k.layoutHash;
    h ^= static_cast<size_t>(k.format) * 0x9e3779b97f4a7c15ULL;
    h ^= reinterpret_cast<size_t>(k.blend) << 1;
    h ^= static_cast<size_t>(k.topology) << 3;
    return h;
}

static uint64_t hashVertexLayout(const wgpu::VertexBufferLayout& layout) {
    uint64_t h = layout.arrayStride ^ (static_cast<uint64_t>(layout.attributeCount) << 32);
    for (size_t i = 0; i < layout.attributeCount; ++i) {
        const auto& a = layout.attributes[i];
        h = h * 1099511628211ULL + (static_cast<uint64_t>(a.format) << 8) + a.shaderLocation;
        h ^= a.offset;
    }
    return h;
}

wgpu::RenderPipeline Shader::getPipeline(wgpu::VertexBufferLayout vertexLayout,
                                         wgpu::TextureFormat colorFormat,
                                         const wgpu::BlendState& blend,
                                         wgpu::PrimitiveTopology topology) {
    PipelineKey key{hashVertexLayout(vertexLayout), colorFormat, &blend, topology};
    for (auto& [k, p] : pipelineCache_) {
        if (k == key) return p;
    }

    wgpu::ColorTargetState target{};
    target.format = colorFormat;
    target.blend = &blend;
    target.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag{};
    frag.module = module_;
    frag.entryPoint = fragmentEntry_.c_str();
    frag.targetCount = 1;
    frag.targets = &target;

    wgpu::RenderPipelineDescriptor desc{};
    desc.layout = nullptr;  // auto pipeline layout (bind groups inferred)
    desc.vertex.module = module_;
    desc.vertex.entryPoint = vertexEntry_.c_str();
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &vertexLayout;
    desc.fragment = &frag;
    desc.primitive.topology = topology;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFF;

    wgpu::RenderPipeline pipeline = device_.CreateRenderPipeline(&desc);
    pipelineCache_.push_back({key, pipeline});
    return pipeline;
}

wgpu::BindGroup Shader::bindUniform(wgpu::RenderPipeline pipeline, int group, int binding,
                                    const void* data, size_t size) {
    wgpu::Buffer buf = uniformRing_.allocate(device_, size);
    device_.GetQueue().WriteBuffer(buf, 0, data, size);

    wgpu::BindGroupEntry entry{};
    entry.binding = static_cast<uint32_t>(binding);
    entry.buffer = buf;
    entry.offset = 0;
    entry.size = size;

    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = pipeline.GetBindGroupLayout(static_cast<uint32_t>(group));
    bgd.entryCount = 1;
    bgd.entries = &entry;
    return device_.CreateBindGroup(&bgd);
}

// ===========================================================================
// Mesh
// ===========================================================================
Mesh::Mesh(wgpu::Device device) : device_(device) {}

wgpu::VertexBufferLayout Mesh::vertexLayout() {
    static const std::array<wgpu::VertexAttribute, 4> attrs = {{
        {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, position), .shaderLocation = 0},
        {.format = wgpu::VertexFormat::Float32x2, .offset = offsetof(Vertex, texCoord), .shaderLocation = 1},
        {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, normal), .shaderLocation = 2},
        {.format = wgpu::VertexFormat::Float32x4, .offset = offsetof(Vertex, color), .shaderLocation = 3},
    }};
    wgpu::VertexBufferLayout layout{};
    layout.arrayStride = sizeof(Vertex);
    layout.stepMode = wgpu::VertexStepMode::Vertex;
    layout.attributeCount = attrs.size();
    layout.attributes = attrs.data();
    return layout;
}

Result<void> Mesh::setVertices(std::span<const Vertex> vertices) {
    if (vertices.empty()) return Result<void>::failure("Mesh::setVertices: empty span");
    cpuVertices_.assign(vertices.begin(), vertices.end());
    const size_t bytes = vertices.size_bytes();

    wgpu::BufferDescriptor bd{};
    bd.size = bytes;
    bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    vertexBuffer_ = device_.CreateBuffer(&bd);
    if (!vertexBuffer_) return Result<void>::failure("Mesh::setVertices: buffer creation failed");

    device_.GetQueue().WriteBuffer(vertexBuffer_, 0, vertices.data(), bytes);
    vertexCount_ = vertices.size();
    return Result<void>::success();
}

Result<void> Mesh::setIndices(std::span<const uint32_t> indices) {
    if (indices.empty()) return Result<void>::failure("Mesh::setIndices: empty span");
    cpuIndices_.assign(indices.begin(), indices.end());
    const size_t bytes = indices.size_bytes();

    wgpu::BufferDescriptor bd{};
    bd.size = bytes;
    bd.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
    indexBuffer_ = device_.CreateBuffer(&bd);
    if (!indexBuffer_) return Result<void>::failure("Mesh::setIndices: buffer creation failed");

    device_.GetQueue().WriteBuffer(indexBuffer_, 0, indices.data(), bytes);
    indexCount_ = indices.size();
    return Result<void>::success();
}

void Mesh::draw(wgpu::RenderPassEncoder& pass, uint32_t instanceCount) {
    if (!vertexBuffer_) return;
    pass.SetVertexBuffer(0, vertexBuffer_, 0, cpuVertices_.size() * sizeof(Vertex));
    if (indexCount_ > 0) {
        pass.SetIndexBuffer(indexBuffer_, wgpu::IndexFormat::Uint32, 0,
                            cpuIndices_.size() * sizeof(uint32_t));
        pass.DrawIndexed(static_cast<uint32_t>(indexCount_), instanceCount);
    } else {
        pass.Draw(static_cast<uint32_t>(vertexCount_), instanceCount);
    }
}

Result<Mesh> Mesh::clone() const {
    Mesh out(device_);
    if (!cpuVertices_.empty()) {
        auto r = out.setVertices(cpuVertices_);
        if (!r) return Result<Mesh>::failure(r.error);
    }
    if (!cpuIndices_.empty()) {
        auto r = out.setIndices(cpuIndices_);
        if (!r) return Result<Mesh>::failure(r.error);
    }
    return Result<Mesh>::success(std::move(out));
}

Result<Mesh> Mesh::triangle(wgpu::Device device, glm::vec3 a, glm::vec3 b, glm::vec3 c) {
    Mesh m(device);
    Vertex va{a, {0, 0}, {0, 0, 1}, {1, 1, 1, 1}};
    Vertex vb{b, {1, 0}, {0, 0, 1}, {1, 1, 1, 1}};
    Vertex vc{c, {0.5f, 1}, {0, 0, 1}, {1, 1, 1, 1}};
    std::array<Vertex, 3> v{va, vb, vc};
    auto r = m.setVertices(v);
    if (!r) return Result<Mesh>::failure(r.error);
    return Result<Mesh>::success(std::move(m));
}

Result<Mesh> Mesh::plane(wgpu::Device device, float w, float h, int cols, int rows) {
    Mesh m(device);
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    const float x0 = -w * 0.5f, y0 = -h * 0.5f;
    for (int r = 0; r <= rows; ++r) {
        for (int c = 0; c <= cols; ++c) {
            float u = static_cast<float>(c) / cols, v = static_cast<float>(r) / rows;
            verts.push_back({{x0 + u * w, y0 + v * h, 0}, {u, v}, {0, 0, 1}, {1, 1, 1, 1}});
        }
    }
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            uint32_t i = r * (cols + 1) + c;
            uint32_t j = i + cols + 1;
            idx.insert(idx.end(), {i, j, i + 1, i + 1, j, j + 1});
        }
    }
    if (auto rr = m.setVertices(verts); !rr) return Result<Mesh>::failure(rr.error);
    if (auto rr = m.setIndices(idx); !rr) return Result<Mesh>::failure(rr.error);
    return Result<Mesh>::success(std::move(m));
}

Result<Mesh> Mesh::cube(wgpu::Device device, float size) {
    Mesh m(device);
    const float s = size * 0.5f;
    const glm::vec3 p[8] = {{-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s},
                            {-s, -s, s},  {s, -s, s},  {s, s, s},  {-s, s, s}};
    const int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
                             {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
    const glm::vec3 nrm[6] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    for (int f = 0; f < 6; ++f) {
        uint32_t base = static_cast<uint32_t>(verts.size());
        const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int k = 0; k < 4; ++k)
            verts.push_back({p[faces[f][k]], uv[k], nrm[f], {1, 1, 1, 1}});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    if (auto rr = m.setVertices(verts); !rr) return Result<Mesh>::failure(rr.error);
    if (auto rr = m.setIndices(idx); !rr) return Result<Mesh>::failure(rr.error);
    return Result<Mesh>::success(std::move(m));
}

Result<Mesh> Mesh::fullscreenQuad(wgpu::Device device) {
    Mesh m(device);
    std::array<Vertex, 6> v{{
        {{-1, -1, 0}, {0, 1}, {0, 0, 1}, {1, 1, 1, 1}},
        {{1, -1, 0}, {1, 1}, {0, 0, 1}, {1, 1, 1, 1}},
        {{1, 1, 0}, {1, 0}, {0, 0, 1}, {1, 1, 1, 1}},
        {{-1, -1, 0}, {0, 1}, {0, 0, 1}, {1, 1, 1, 1}},
        {{1, 1, 0}, {1, 0}, {0, 0, 1}, {1, 1, 1, 1}},
        {{-1, 1, 0}, {0, 0}, {0, 0, 1}, {1, 1, 1, 1}},
    }};
    if (auto rr = m.setVertices(v); !rr) return Result<Mesh>::failure(rr.error);
    return Result<Mesh>::success(std::move(m));
}

Result<Mesh> Mesh::line(wgpu::Device device, std::span<const glm::vec3> points) {
    Mesh m(device);
    std::vector<Vertex> verts;
    verts.reserve(points.size());
    for (auto& p : points) verts.push_back({p, {0, 0}, {0, 0, 1}, {1, 1, 1, 1}});
    if (auto rr = m.setVertices(verts); !rr) return Result<Mesh>::failure(rr.error);
    return Result<Mesh>::success(std::move(m));
}

Result<Mesh> Mesh::line(wgpu::Device device, std::span<const glm::vec2> points) {
    std::vector<glm::vec3> p3;
    p3.reserve(points.size());
    for (auto& p : points) p3.emplace_back(p.x, p.y, 0.0f);
    return line(device, std::span<const glm::vec3>(p3));
}

// ===========================================================================
// Helpers
// ===========================================================================
// WebGPU StringViews use a WGPU_STRLEN sentinel for null-terminated strings.
static std::string sv(wgpu::StringView s) {
    if (s.data == nullptr) return {};
    if (s.length == WGPU_STRLEN) return std::string(s.data);
    return std::string(s.data, s.length);
}

// Creates a CAMetalLayer and attaches it to the RGFW window's view using RGFW's
// own Cocoa helpers (no hand-rolled objc_msgSend needed).
static void* attachMetalLayer(void* rgfwWindow) {
    auto* win = static_cast<RGFW_window*>(rgfwWindow);
    void* layer = RGFW_getLayer_OSX();  // [CAMetalLayer layer]
    if (!layer) return nullptr;
    RGFW_window_setLayer_OSX(win, layer);
    return layer;
}

// RGFW mouse button order (Left, Middle, Right) -> braid (Left, Right, Middle).
static std::optional<MouseButton> mapButton(RGFW_mouseButton b) {
    switch (b) {
        case RGFW_mouseLeft: return MouseButton::Left;
        case RGFW_mouseMiddle: return MouseButton::Middle;
        case RGFW_mouseRight: return MouseButton::Right;
        default: return std::nullopt;
    }
}

// RGFW keycode -> braid::Key (the windowing seam: nothing above sees an RGFW value).
// RGFW already gives printable keys their ASCII value, and braid::Key matches that,
// so printables pass straight through; only control/navigation keys are remapped.
static Key mapKey(RGFW_key v) {
    switch (v) {
        case RGFW_keyEscape:    return Key::Escape;
        case RGFW_keyReturn:    return Key::Enter;     // == RGFW_keyEnter
        case RGFW_keyTab:       return Key::Tab;
        case RGFW_keyBackSpace: return Key::Backspace;
        case RGFW_keyDelete:    return Key::Delete;
        case RGFW_keyInsert:    return Key::Insert;
        case RGFW_keyMenu:      return Key::Menu;
        case RGFW_keyLeft:      return Key::Left;
        case RGFW_keyRight:     return Key::Right;
        case RGFW_keyUp:        return Key::Up;
        case RGFW_keyDown:      return Key::Down;
        case RGFW_keyHome:      return Key::Home;
        case RGFW_keyEnd:       return Key::End;
        case RGFW_keyPageUp:    return Key::PageUp;
        case RGFW_keyPageDown:  return Key::PageDown;
        case RGFW_keyCapsLock:  return Key::CapsLock;
        case RGFW_keyNumLock:   return Key::NumLock;
        case RGFW_keyShiftL:    case RGFW_keyShiftR:   return Key::Shift;
        case RGFW_keyControlL:  case RGFW_keyControlR: return Key::Control;
        case RGFW_keyAltL:      case RGFW_keyAltR:     return Key::Alt;
        case RGFW_keySuperL:    case RGFW_keySuperR:   return Key::Super;
        case RGFW_keyF1:  return Key::F1;   case RGFW_keyF2:  return Key::F2;
        case RGFW_keyF3:  return Key::F3;   case RGFW_keyF4:  return Key::F4;
        case RGFW_keyF5:  return Key::F5;   case RGFW_keyF6:  return Key::F6;
        case RGFW_keyF7:  return Key::F7;   case RGFW_keyF8:  return Key::F8;
        case RGFW_keyF9:  return Key::F9;   case RGFW_keyF10: return Key::F10;
        case RGFW_keyF11: return Key::F11;  case RGFW_keyF12: return Key::F12;
        default: break;
    }
    if (v >= 32 && v < 127) return static_cast<Key>(v);  // printable ASCII passthrough
    return Key::Unknown;
}

// The printable character a key produces ('s', ' ', …), or 0 for control keys.
static char printableChar(RGFW_key v) {
    return (v >= 32 && v < 127) ? static_cast<char>(v) : 0;
}

// ===========================================================================
// App
// ===========================================================================
App::App() : App(Settings{}) {}
App::App(const Settings& settings) : settings_(settings), timer_(settings.targetFps) {}
App::~App() {
    if (window_) RGFW_window_close(static_cast<RGFW_window*>(window_));
    RGFW_deinit();
}

int App::width() const { return settings_.width; }
int App::height() const { return settings_.height; }

wgpu::CommandEncoder& App::encoder() { return frameEncoder_; }
Surface& App::surface() { return *mainSurface_; }

void App::setWindowTitle(const char* title) {
    if (!window_) return;
    // Set the NSWindow title under our own autorelease pool. RGFW_window_setName
    // autoreleases the NSString into RGFW's event pool, and calling it from draw()
    // (outside that pool's scope) corrupts the balance and trips shouldClose.
    auto* win = static_cast<RGFW_window*>(window_);
    using Msg = id (*)(id, SEL);
    using MsgStr = id (*)(id, SEL, const char*);
    using MsgId = void (*)(id, SEL, id);
    id pool = reinterpret_cast<Msg>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSAutoreleasePool")), sel_registerName("alloc"));
    pool = reinterpret_cast<Msg>(objc_msgSend)(pool, sel_registerName("init"));
    id str = reinterpret_cast<MsgStr>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSString")), sel_registerName("stringWithUTF8String:"),
        title);
    reinterpret_cast<MsgId>(objc_msgSend)(reinterpret_cast<id>(win->src.window),
                                          sel_registerName("setTitle:"), str);
    reinterpret_cast<Msg>(objc_msgSend)(pool, sel_registerName("drain"));
}

Result<void> App::initWindow() {
    RGFW_init("braid", (RGFW_initFlags)0);
    RGFW_windowFlags flags = RGFW_windowCenter | RGFW_windowFocus | RGFW_windowFocusOnShow;
    if (!settings_.resizable) flags |= RGFW_windowNoResize;
    RGFW_window* win = RGFW_createWindow(settings_.title, 0, 0, settings_.width, settings_.height,
                                         flags);
    if (!win) return Result<void>::failure("RGFW_createWindow failed");
    window_ = win;
    mousePos_ = {settings_.width * 0.5f, settings_.height * 0.5f};  // neutral until moved
    RGFW_window_setExitKey(win, RGFW_keyEscape);  // Esc quits (Cmd+Q handled in pump)
    RGFW_window_show(win);
    RGFW_window_raise(win);
    RGFW_window_focus(win);
    return Result<void>::success();
}

Result<void> App::initWebGPU() {
    wgpu::InstanceDescriptor instDesc{};
    instance_ = wgpu::CreateInstance(&instDesc);
    if (!instance_) return Result<void>::failure("wgpu::CreateInstance failed");

    // --- Surface from the Metal layer [VERIFY] ---
    void* metalLayer = attachMetalLayer(window_);
    if (!metalLayer) return Result<void>::failure("attachMetalLayer failed");
    wgpu::SurfaceSourceMetalLayer metalSrc{};
    metalSrc.layer = metalLayer;
    wgpu::SurfaceDescriptor surfDesc{};
    surfDesc.nextInChain = &metalSrc;
    surface_ = instance_.CreateSurface(&surfDesc);
    if (!surface_) return Result<void>::failure("CreateSurface failed");

    // --- Adapter request (future + ProcessEvents pump) ---
    wgpu::RequestAdapterOptions adapterOpts{};
    adapterOpts.compatibleSurface = surface_;
    adapterOpts.powerPreference = wgpu::PowerPreference::HighPerformance;

    std::string requestError;
    bool adapterDone = false;
    instance_.RequestAdapter(
        &adapterOpts, wgpu::CallbackMode::AllowProcessEvents,
        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView message) {
            if (status == wgpu::RequestAdapterStatus::Success) adapter_ = std::move(a);
            else requestError = sv(message);
            adapterDone = true;
        });
    while (!adapterDone) instance_.ProcessEvents();
    if (!adapter_) return Result<void>::failure("RequestAdapter: " + requestError);

    // --- Device request ---
    wgpu::DeviceDescriptor devDesc{};
    devDesc.label = "braid-device";
    devDesc.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
            fmt::print(stderr, "[braid] WebGPU error ({}): {}\n", static_cast<int>(type), sv(msg));
        });
    devDesc.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowProcessEvents,
        [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
            if (reason != wgpu::DeviceLostReason::Destroyed)
                fmt::print(stderr, "[braid] device lost: {}\n", sv(msg));
        });

    bool deviceDone = false;
    adapter_.RequestDevice(
        &devDesc, wgpu::CallbackMode::AllowProcessEvents,
        [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView message) {
            if (status == wgpu::RequestDeviceStatus::Success) device_ = std::move(d);
            else requestError = sv(message);
            deviceDone = true;
        });
    while (!deviceDone) instance_.ProcessEvents();
    if (!device_) return Result<void>::failure("RequestDevice: " + requestError);

    queue_ = device_.GetQueue();
    detail::setContext(instance_, device_);
    configureSurface();
    // swapchain wrap (present target, 8-bit) + persistent offscreen main draw
    // target in 16-bit float (smooth feedback, HDR accumulation).
    swapSurface_.emplace(surface_, settings_.width, settings_.height, settings_.format);
    mainSurface_.emplace(device_, settings_.width, settings_.height,
                         wgpu::TextureFormat::RGBA16Float);
    return Result<void>::success();
}

void App::configureSurface() {
    wgpu::SurfaceConfiguration config{};
    config.device = device_;
    config.format = settings_.format;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.width = static_cast<uint32_t>(settings_.width);
    config.height = static_cast<uint32_t>(settings_.height);
    config.presentMode = settings_.vsync ? wgpu::PresentMode::Fifo : wgpu::PresentMode::Immediate;
    config.alphaMode = wgpu::CompositeAlphaMode::Auto;
    surface_.Configure(&config);
}

void App::pumpEvents() {
    auto* win = static_cast<RGFW_window*>(window_);
    RGFW_event ev;
    while (RGFW_window_checkEvent(win, &ev)) {
        switch (ev.type) {
            case RGFW_keyPressed: {
                KeyEvent e{};
                e.key = mapKey(ev.key.value);
                e.ch = printableChar(ev.key.value);
                e.pressed = true;
                e.repeat = ev.key.repeat != 0;
                e.shift = (ev.key.mod & RGFW_modShift) != 0;
                e.ctrl = (ev.key.mod & RGFW_modControl) != 0;
                e.alt = (ev.key.mod & RGFW_modAlt) != 0;
                e.super = (ev.key.mod & RGFW_modSuper) != 0;
                if (e.super && e.key == Key::Q) running_ = false;  // Cmd+Q quits
                keyEvents.push(e);
                keyPressed(e);
                break;
            }
            case RGFW_keyReleased: {
                KeyEvent e{};
                e.key = mapKey(ev.key.value);
                e.ch = printableChar(ev.key.value);
                e.shift = (ev.key.mod & RGFW_modShift) != 0;
                e.ctrl = (ev.key.mod & RGFW_modControl) != 0;
                e.alt = (ev.key.mod & RGFW_modAlt) != 0;
                e.super = (ev.key.mod & RGFW_modSuper) != 0;
                keyEvents.push(e);
                keyReleased(e);
                break;
            }
            case RGFW_mouseButtonPressed: {
                MouseEvent e{};
                e.button = mapButton(ev.button.value);
                e.pos = mousePos_;  // button events carry no position; use latest
                e.pressed = true;
                mouseEvents.push(e);
                mousePressed(e);
                break;
            }
            case RGFW_mouseButtonReleased: {
                MouseEvent e{};
                e.button = mapButton(ev.button.value);
                e.pos = mousePos_;
                e.pressed = false;
                mouseEvents.push(e);
                mouseReleased(e);
                break;
            }
            case RGFW_mouseMotion: {
                MouseEvent e{};
                e.pos = {static_cast<float>(ev.mouse.x), static_cast<float>(ev.mouse.y)};
                e.delta = e.pos - mousePos_;
                mousePos_ = e.pos;
                mouseEvents.push(e);
                mouseMoved(e);
                break;
            }
            case RGFW_mouseScroll: {
                ScrollEvent e{{ev.delta.x, ev.delta.y}};
                scrollEvents.push(e);
                break;
            }
            case RGFW_windowResized: {
                int w = ev.update.w, h = ev.update.h;
                settings_.width = w;
                settings_.height = h;
                configureSurface();
                if (swapSurface_) swapSurface_->resize(w, h);
                if (mainSurface_) mainSurface_->resize(w, h);
                WindowEvent we{WindowEvent::Resized, {static_cast<float>(w), static_cast<float>(h)}};
                windowEvents.push(we);
                windowResized(we);
                break;
            }
            case RGFW_windowClose:
                running_ = false;
                break;
            default:
                break;
        }
    }
    // Drain channels so subscribed callbacks fire on this (main) thread.
    while (keyEvents.pop()) {}
    while (mouseEvents.pop()) {}
    while (scrollEvents.pop()) {}
    while (windowEvents.pop()) {}
    while (dropEvents.pop()) {}
}

bool App::beginFrame() {
    wgpu::SurfaceTexture surfTex;
    surface_.GetCurrentTexture(&surfTex);
    if (surfTex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surfTex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        // Lost/outdated surface — reconfigure and skip this frame.
        configureSurface();
        return false;
    }
    frameView_ = surfTex.texture.CreateView();
    frameEncoder_ = device_.CreateCommandEncoder();
    swapSurface_->setCurrentView(frameView_);
    return true;
}

void App::endFrame() {
    // "The screen is the Surface you show": blit the persistent main Surface onto
    // the swapchain. This single copy is what makes screenshot/record/feedback free.
    detail::BlitUniforms u{};
    detail::compositor().blit(frameEncoder_, swapSurface_->asTexture(), settings_.format,
                              mainSurface_->asTexture(), u, Blend::None, wgpu::LoadOp::Clear);
    wgpu::CommandBuffer cmd = frameEncoder_.Finish();
    queue_.Submit(1, &cmd);
    surface_.Present();
    instance_.ProcessEvents();  // service async work (maps, device callbacks)
    frameEncoder_ = nullptr;
    frameView_ = nullptr;
}

Result<void> App::run() {
    if (auto r = initWindow(); !r) return r;
    if (auto r = initWebGPU(); !r) return r;

    setup();
    running_ = true;
    timer_.reset();

    auto* win = static_cast<RGFW_window*>(window_);
    while (running_ && !RGFW_window_shouldClose(win)) {
        timer_.waitNext();
        pumpEvents();
        update();
        if (beginFrame()) {
            beforeDraw();
            draw();
            afterDraw();
            endFrame();
        }
    }

    exit();
    return Result<void>::success();
}

void App::close() { running_ = false; }

// ===========================================================================
// SketchApp
// ===========================================================================
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

// A fresh uniform buffer + bind group per draw. SketchApp issues hundreds of
// draws into one command buffer, so they cannot share Shader's 3-slot ring
// (which is for one-uniform-per-frame); each needs its own buffer, kept alive by
// the bind group until the frame is submitted.
static wgpu::BindGroup freshUniform(wgpu::Device dev, wgpu::RenderPipeline pipeline,
                                    const void* data, size_t size) {
    wgpu::BufferDescriptor bd{};
    bd.size = size;
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer ub = dev.CreateBuffer(&bd);
    dev.GetQueue().WriteBuffer(ub, 0, data, size);
    wgpu::BindGroupEntry e{};
    e.binding = 0;
    e.buffer = ub;
    e.size = size;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = pipeline.GetBindGroupLayout(0);
    bgd.entryCount = 1;
    bgd.entries = &e;
    return dev.CreateBindGroup(&bgd);
}

SketchApp::SketchApp() : SketchApp(Settings{}) {}
SketchApp::SketchApp(const Settings& settings) : App(settings) {}

// Lazy init so a user-overridden setup() never clashes with ours.
void SketchApp::ensureReady() {
    if (ready_) return;
    proj_ = glm::ortho(0.0f, static_cast<float>(settings_.width),
                       static_cast<float>(settings_.height), 0.0f, -1.0f, 1.0f);
    view_ = glm::mat4(1.0f);
    Shader::LoadOptions opts{};
    opts.wgsl = kDefaultWGSL;
    opts.label = "braid-default";
    if (auto r = Shader::load(device_, opts)) defaultShader_ = std::move(*r);
    else fmt::print(stderr, "[braid] default shader failed: {}\n", r.error);
    scratch_.emplace(device_);
    ready_ = true;
}

void SketchApp::beforeDraw() {
    ensureReady();
    transform_.current = glm::mat4(1.0f);
    transform_.stack.clear();
    bgRequested_ = false;  // omit background() to accumulate (feedback)
}

void SketchApp::afterDraw() {
    // If background() was requested but nothing opened a pass, clear anyway.
    if (bgRequested_ && !passOpen_) ensurePass();
    flush();
}

void SketchApp::background(float r, float g, float b, float a) { clearColor_ = {r, g, b, a}; bgRequested_ = true; }
void SketchApp::background(glm::vec4 c) { clearColor_ = c; bgRequested_ = true; }
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
        currentPass_ = bgRequested_ ? surface().begin(encoder(), clearColor_)
                                    : surface().beginLoad(encoder());
        passOpen_ = true;
    }
}

void SketchApp::drawTris(std::span<const Vertex> verts) {
    if (!state_.fillEnabled || verts.empty() || !defaultShader_.isValid()) return;
    ensurePass();

    // Tint with the current fill color; geometry carries white vertex color.
    std::vector<Vertex> tinted(verts.begin(), verts.end());
    if (auto r = scratch_->setVertices(tinted); !r) return;

    auto pipeline = defaultShader_.getPipeline(Mesh::vertexLayout(), surface().format(),
                                               Blend::Alpha);
    SketchUniforms u{proj_ * view_ * transform_.current, state_.fill};
    auto bg = freshUniform(device_, pipeline, &u, sizeof(u));

    currentPass_.SetPipeline(pipeline);
    currentPass_.SetBindGroup(0, bg);
    scratch_->draw(currentPass_);
}

void SketchApp::drawLines(std::span<const Vertex> verts) {
    if (verts.empty() || !defaultShader_.isValid()) return;
    ensurePass();
    std::vector<Vertex> v(verts.begin(), verts.end());
    if (auto r = scratch_->setVertices(v); !r) return;
    auto pipeline = defaultShader_.getPipeline(Mesh::vertexLayout(), surface().format(),
                                               Blend::Alpha, wgpu::PrimitiveTopology::LineList);
    SketchUniforms u{proj_ * view_ * transform_.current, state_.fill};
    auto bg = freshUniform(device_, pipeline, &u, sizeof(u));
    currentPass_.SetPipeline(pipeline);
    currentPass_.SetBindGroup(0, bg);
    scratch_->draw(currentPass_);
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
}

void SketchApp::triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    std::array<Vertex, 3> v{Vertex{{a, 0}, {}, {0, 0, 1}, {1, 1, 1, 1}},
                            Vertex{{b, 0}, {}, {0, 0, 1}, {1, 1, 1, 1}},
                            Vertex{{c, 0}, {}, {0, 0, 1}, {1, 1, 1, 1}}};
    drawTris(v);
}

void SketchApp::quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d) {
    auto V = [](glm::vec2 p) { return Vertex{{p, 0}, {}, {0, 0, 1}, {1, 1, 1, 1}}; };
    std::array<Vertex, 6> v{V(a), V(b), V(c), V(a), V(c), V(d)};
    drawTris(v);
}

void SketchApp::ellipse(float x, float y, float w, float h) {
    const int seg = 48;
    std::vector<Vertex> v;
    v.reserve(seg * 3);
    const float rx = w * 0.5f, ry = h * 0.5f;
    for (int i = 0; i < seg; ++i) {
        float a0 = (float(i) / seg) * 6.2831853f;
        float a1 = (float(i + 1) / seg) * 6.2831853f;
        auto V = [](float px, float py) { return Vertex{{px, py, 0}, {}, {0, 0, 1}, {1, 1, 1, 1}}; };
        v.push_back(V(x, y));
        v.push_back(V(x + std::cos(a0) * rx, y + std::sin(a0) * ry));
        v.push_back(V(x + std::cos(a1) * rx, y + std::sin(a1) * ry));
    }
    drawTris(v);
}

void SketchApp::circle(float x, float y, float r) { ellipse(x, y, r * 2, r * 2); }

void SketchApp::line(float x1, float y1, float x2, float y2) {
    // Expand to a quad of strokeWeight thickness.
    glm::vec2 a{x1, y1}, b{x2, y2};
    glm::vec2 dir = b - a;
    float len = glm::length(dir);
    if (len < 1e-5f) return;
    glm::vec2 n = glm::vec2(-dir.y, dir.x) / len * (state_.strokeWeight * 0.5f);
    glm::vec4 save = state_.fill;
    bool savedEnabled = state_.fillEnabled;
    state_.fill = state_.stroke;
    state_.fillEnabled = state_.strokeEnabled;
    quad(a + n, b + n, b - n, a - n);
    state_.fill = save;
    state_.fillEnabled = savedEnabled;
}

void SketchApp::point(float x, float y) {
    float r = state_.strokeWeight * 0.5f;
    glm::vec4 save = state_.fill;
    state_.fill = state_.stroke;
    ellipse(x, y, r * 2, r * 2);
    state_.fill = save;
}

wgpu::RenderPassEncoder& SketchApp::pass() { ensurePass(); return currentPass_; }

void SketchApp::flush() {
    if (passOpen_) {
        surface().end(currentPass_);
        passOpen_ = false;
    }
}

}  // namespace braid
