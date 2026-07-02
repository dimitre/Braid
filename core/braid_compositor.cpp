// braid_compositor.cpp — the internal blit engine + the two process-wide
// singletons it depends on (the WebGPU Context and the Compositor itself).
//
// One fullscreen-quad shader family samples a source view with a UV transform +
// invert + tint and blends it into a destination view; separate WGSL handles the
// separable Gaussian blur, the HDR brightpass, and the placed-quad paste. Every
// Surface sampling operation (show/composite/transform/feedback/blur/threshold/
// paste) and App's present blit route through here.
//
// NOTE (multi-window): g_ctx and g_compositor are the two global singletons the
// roadmap calls out as the blockers for multi-window. They are intentionally kept
// together in this one TU so the future "per-device context" refactor has a single
// place to start.
#include "braid.h"
#include "braid_compositor.h"
#include "braid_detail.h"

#include <array>

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

namespace detail {

// ===========================================================================
// Context — the single instance+device, set once by App during init.
// ===========================================================================
static Context g_ctx;
void setContext(wgpu::Instance i, wgpu::Device d) { g_ctx.instance = i; g_ctx.device = d; }
Context& ctx() { return g_ctx; }

// Uniform layouts that never leave this TU (the public ones live in the header).
struct BlurUniforms {
    glm::vec2 texelSize;    // 1/w, 1/h
    glm::vec2 dir;          // (1,0) H pass · (0,1) V pass
    float radius = 4.0f;    // pixels
    float _pad0 = 0;
    glm::vec2 _pad1{};
};

struct ThresholdUniforms {
    float level = 1.0f;
    float knee  = 0.1f;
    glm::vec2 _pad{};
};

struct ContourUniforms {
    glm::vec2 texelSize;   // 1/w, 1/h
    float level = 0.5f;    // luma threshold ("dif" in the ofworks source)
    float radius = 1.0f;   // neighbor sample offset, in pixels ("raio")
    int32_t mode = 0;      // 0: 8-neighbor +/X, 1: + only, 2: X only, 3: 2-point
    float _pad0 = 0;
    glm::vec2 _pad1{};
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

// Separable 1D Gaussian — run H then V for a full 2D blur.
// Uniform layout matches the shared bgl_ (binding 0 = uniform, 1 = tex, 2 = sampler).
static const char* kBlurWGSL = R"WGSL(
struct Blur {
  texelSize : vec2<f32>,
  dir       : vec2<f32>,
  radius    : f32,
  _pad0     : f32,
  _pad1     : vec2<f32>,
};
@group(0) @binding(0) var<uniform> u : Blur;
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
  let sigma = max(u.radius / 3.0, 0.5);
  let taps = min(i32(ceil(u.radius)) + 1, 32);
  var col = vec4<f32>(0.0);
  var wsum = 0.0;
  for (var i = -taps; i <= taps; i++) {
    let fi = f32(i);
    let w = exp(-fi * fi / (2.0 * sigma * sigma));
    // textureSampleLevel (lod=0) is valid outside uniform control flow;
    // textureSample would require uniform CF which dynamic loops don't guarantee.
    col  += textureSampleLevel(tex, samp, in.uv + u.dir * u.texelSize * fi, 0.0) * w;
    wsum += w;
  }
  return col / wsum;
}
)WGSL";

// Brightpass filter — keeps pixels whose luma exceeds `level`.
// Output alpha=1 so additive blend (+=) adds full RGB energy.
static const char* kThresholdWGSL = R"WGSL(
struct Thresh {
  level : f32,
  knee  : f32,
  _pad  : vec2<f32>,
};
@group(0) @binding(0) var<uniform> u : Thresh;
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
  let col  = textureSample(tex, samp, in.uv);
  let luma = dot(col.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
  let t    = clamp((luma - u.level) / max(u.knee, 0.0001), 0.0, 1.0);
  return vec4<f32>(col.rgb * t, 1.0);
}
)WGSL";

// Isoline/edge detector — ported from ofworks contour2.frag. For each pixel,
// samples a neighbor pattern (picked by `mode`, offset by `radius` texels) and
// counts how many neighbors fall below the `level` luma threshold; draws white
// where that count crosses the pattern's cutoff (~52.5% of its sample count) —
// a binary contour line at the level set, unlike threshold's flat brightpass.
// mode 0 = all 8 (+ and X), 1 = + only (4), 2 = X only (4), 3 = 2-point (+ subset).
static const char* kContourWGSL = R"WGSL(
struct Contour {
  texelSize : vec2<f32>,
  level     : f32,
  radius    : f32,
  mode      : i32,
  _pad0     : f32,
  _pad1     : vec2<f32>,
};
@group(0) @binding(0) var<uniform> u : Contour;
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
  let offsets = array<vec2<f32>, 8>(
    vec2<f32>(-1.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0), vec2<f32>(0.0, -1.0),
    vec2<f32>(-1.0, -1.0), vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, 1.0), vec2<f32>(1.0, -1.0));
  let starts = array<i32, 4>(0, 0, 4, 0);
  let counts = array<i32, 4>(8, 4, 4, 2);
  let m = clamp(u.mode, 0, 3);
  let start = starts[m];
  let count = counts[m];
  var c = 0.0;
  for (var i = 0; i < count; i++) {
    let off = offsets[start + i] * u.radius * u.texelSize;
    // textureSampleLevel (lod=0): the loop bound comes from a uniform, but WGSL
    // doesn't guarantee that's "uniform control flow" for textureSample — same
    // reasoning as the blur pass above.
    let s = textureSampleLevel(tex, samp, in.uv + off, 0.0);
    let luma = dot(s.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
    if (luma < u.level) { c += 1.0; }
  }
  let cutoff = f32(count) * 0.525;
  var d = 1.0;
  if (c == 0.0) { d = 0.0; }
  if (c >= cutoff) { d = 0.0; }
  return vec4<f32>(d, d, d, d);
}
)WGSL";

// Placed-quad paste: sample the full source across a quad that is scaled,
// rotated, and positioned in NDC. Same bind layout as the blit (uniform/tex/
// sampler) — a full-frame quad (center 0, half 1, rot 0) reproduces the source.
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

// ===========================================================================
// Compositor
// ===========================================================================
void Compositor::init(wgpu::Device device) {
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

    wgpu::ShaderSourceWGSL blursrc{};
    blursrc.code = kBlurWGSL;
    wgpu::ShaderModuleDescriptor blursmd{};
    blursmd.nextInChain = &blursrc;
    blursmd.label = "braid-blur";
    blurModule_ = device_.CreateShaderModule(&blursmd);

    wgpu::ShaderSourceWGSL thrsrc{};
    thrsrc.code = kThresholdWGSL;
    wgpu::ShaderModuleDescriptor thrsmd{};
    thrsmd.nextInChain = &thrsrc;
    thrsmd.label = "braid-threshold";
    thresholdModule_ = device_.CreateShaderModule(&thrsmd);

    wgpu::ShaderSourceWGSL ctrsrc{};
    ctrsrc.code = kContourWGSL;
    wgpu::ShaderModuleDescriptor ctrsmd{};
    ctrsmd.nextInChain = &ctrsrc;
    ctrsmd.label = "braid-contour";
    contourModule_ = device_.CreateShaderModule(&ctrsmd);

    wgpu::SamplerDescriptor sd{};
    sd.magFilter = wgpu::FilterMode::Linear;
    sd.minFilter = wgpu::FilterMode::Linear;
    // Nearest was tried globally (2026-07-01) and rejected: feedback()'s iterated
    // fractional resampling degrades badly under it. If chunky pixels are wanted
    // (LED/pixel-art), make it a per-Surface opt-in instead (roadmap item 2).
    // sd.magFilter = wgpu::FilterMode::Nearest;
    // sd.minFilter = wgpu::FilterMode::Nearest;
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

void Compositor::blit(wgpu::CommandEncoder& enc, wgpu::TextureView dest,
                      wgpu::TextureFormat destFormat, wgpu::TextureView src,
                      const BlitUniforms& u, const wgpu::BlendState& blend, wgpu::LoadOp loadOp) {
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

void Compositor::quad(wgpu::CommandEncoder& enc, wgpu::TextureView dest,
                      wgpu::TextureFormat destFormat, wgpu::TextureView src,
                      const QuadUniforms& u, const wgpu::BlendState& blend, wgpu::LoadOp loadOp) {
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

void Compositor::blurPass(wgpu::CommandEncoder& enc, wgpu::TextureView dest,
                          wgpu::TextureFormat destFormat, wgpu::TextureView src,
                          glm::vec2 texelSize, glm::vec2 dir, float radius) {
    BlurUniforms u{};
    u.texelSize = texelSize;
    u.dir = dir;
    u.radius = radius;
    wgpu::BufferDescriptor bd{};
    bd.size = sizeof(BlurUniforms);
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer ub = device_.CreateBuffer(&bd);
    device_.GetQueue().WriteBuffer(ub, 0, &u, sizeof(u));

    std::array<wgpu::BindGroupEntry, 3> be{};
    be[0].binding = 0; be[0].buffer = ub; be[0].size = sizeof(BlurUniforms);
    be[1].binding = 1; be[1].textureView = src;
    be[2].binding = 2; be[2].sampler = sampler_;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = bgl_; bgd.entryCount = be.size(); bgd.entries = be.data();
    wgpu::BindGroup bg = device_.CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment att{};
    att.view = dest; att.loadOp = wgpu::LoadOp::Clear; att.storeOp = wgpu::StoreOp::Store;
    att.clearValue = {0, 0, 0, 1};
    wgpu::RenderPassDescriptor rpd{};
    rpd.colorAttachmentCount = 1; rpd.colorAttachments = &att;

    wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rpd);
    pass.SetPipeline(blurPipelineFor(destFormat));
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

void Compositor::thresholdPass(wgpu::CommandEncoder& enc, wgpu::TextureView dest,
                               wgpu::TextureFormat destFormat, wgpu::TextureView src, float level,
                               float knee) {
    ThresholdUniforms u{};
    u.level = level; u.knee = knee;
    wgpu::BufferDescriptor bd{};
    bd.size = sizeof(ThresholdUniforms);
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer ub = device_.CreateBuffer(&bd);
    device_.GetQueue().WriteBuffer(ub, 0, &u, sizeof(u));

    std::array<wgpu::BindGroupEntry, 3> be{};
    be[0].binding = 0; be[0].buffer = ub; be[0].size = sizeof(ThresholdUniforms);
    be[1].binding = 1; be[1].textureView = src;
    be[2].binding = 2; be[2].sampler = sampler_;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = bgl_; bgd.entryCount = be.size(); bgd.entries = be.data();
    wgpu::BindGroup bg = device_.CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment att{};
    att.view = dest; att.loadOp = wgpu::LoadOp::Clear; att.storeOp = wgpu::StoreOp::Store;
    att.clearValue = {0, 0, 0, 0};
    wgpu::RenderPassDescriptor rpd{};
    rpd.colorAttachmentCount = 1; rpd.colorAttachments = &att;

    wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rpd);
    pass.SetPipeline(thresholdPipelineFor(destFormat));
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

void Compositor::contourPass(wgpu::CommandEncoder& enc, wgpu::TextureView dest,
                             wgpu::TextureFormat destFormat, wgpu::TextureView src,
                             glm::vec2 texelSize, float level, float radius, int mode) {
    ContourUniforms u{};
    u.texelSize = texelSize; u.level = level; u.radius = radius; u.mode = mode;
    wgpu::BufferDescriptor bd{};
    bd.size = sizeof(ContourUniforms);
    bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer ub = device_.CreateBuffer(&bd);
    device_.GetQueue().WriteBuffer(ub, 0, &u, sizeof(u));

    std::array<wgpu::BindGroupEntry, 3> be{};
    be[0].binding = 0; be[0].buffer = ub; be[0].size = sizeof(ContourUniforms);
    be[1].binding = 1; be[1].textureView = src;
    be[2].binding = 2; be[2].sampler = sampler_;
    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = bgl_; bgd.entryCount = be.size(); bgd.entries = be.data();
    wgpu::BindGroup bg = device_.CreateBindGroup(&bgd);

    wgpu::RenderPassColorAttachment att{};
    att.view = dest; att.loadOp = wgpu::LoadOp::Clear; att.storeOp = wgpu::StoreOp::Store;
    att.clearValue = {0, 0, 0, 0};
    wgpu::RenderPassDescriptor rpd{};
    rpd.colorAttachmentCount = 1; rpd.colorAttachments = &att;

    wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rpd);
    pass.SetPipeline(contourPipelineFor(destFormat));
    pass.SetBindGroup(0, bg);
    pass.Draw(3);
    pass.End();
}

wgpu::RenderPipeline Compositor::pipelineFor(wgpu::TextureFormat fmt, const wgpu::BlendState& blend) {
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

wgpu::RenderPipeline Compositor::quadPipelineFor(wgpu::TextureFormat fmt,
                                                 const wgpu::BlendState& blend) {
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

wgpu::RenderPipeline Compositor::blurPipelineFor(wgpu::TextureFormat fmt) {
    for (auto& [f, p] : blurCache_) if (f == fmt) return p;
    wgpu::ColorTargetState target{};
    target.format = fmt;
    target.writeMask = wgpu::ColorWriteMask::All;
    wgpu::FragmentState frag{};
    frag.module = blurModule_; frag.entryPoint = "fs";
    frag.targetCount = 1; frag.targets = &target;
    wgpu::RenderPipelineDescriptor rpd{};
    rpd.layout = pl_;
    rpd.vertex.module = blurModule_; rpd.vertex.entryPoint = "vs";
    rpd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    rpd.fragment = &frag;
    rpd.multisample.count = 1; rpd.multisample.mask = 0xFFFFFFFF;
    wgpu::RenderPipeline p = device_.CreateRenderPipeline(&rpd);
    blurCache_.push_back({fmt, p});
    return p;
}

wgpu::RenderPipeline Compositor::thresholdPipelineFor(wgpu::TextureFormat fmt) {
    for (auto& [f, p] : thresholdCache_) if (f == fmt) return p;
    wgpu::ColorTargetState target{};
    target.format = fmt;
    target.writeMask = wgpu::ColorWriteMask::All;
    wgpu::FragmentState frag{};
    frag.module = thresholdModule_; frag.entryPoint = "fs";
    frag.targetCount = 1; frag.targets = &target;
    wgpu::RenderPipelineDescriptor rpd{};
    rpd.layout = pl_;
    rpd.vertex.module = thresholdModule_; rpd.vertex.entryPoint = "vs";
    rpd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    rpd.fragment = &frag;
    rpd.multisample.count = 1; rpd.multisample.mask = 0xFFFFFFFF;
    wgpu::RenderPipeline p = device_.CreateRenderPipeline(&rpd);
    thresholdCache_.push_back({fmt, p});
    return p;
}

wgpu::RenderPipeline Compositor::contourPipelineFor(wgpu::TextureFormat fmt) {
    for (auto& [f, p] : contourCache_) if (f == fmt) return p;
    wgpu::ColorTargetState target{};
    target.format = fmt;
    target.writeMask = wgpu::ColorWriteMask::All;
    wgpu::FragmentState frag{};
    frag.module = contourModule_; frag.entryPoint = "fs";
    frag.targetCount = 1; frag.targets = &target;
    wgpu::RenderPipelineDescriptor rpd{};
    rpd.layout = pl_;
    rpd.vertex.module = contourModule_; rpd.vertex.entryPoint = "vs";
    rpd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    rpd.fragment = &frag;
    rpd.multisample.count = 1; rpd.multisample.mask = 0xFFFFFFFF;
    wgpu::RenderPipeline p = device_.CreateRenderPipeline(&rpd);
    contourCache_.push_back({fmt, p});
    return p;
}

static Compositor g_compositor;
Compositor& compositor() {
    g_compositor.init(ctx().device);
    return g_compositor;
}

}  // namespace detail
}  // namespace braid
