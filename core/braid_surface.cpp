// braid_surface.cpp — Surface: the one primitive. A renderable target backed by
// either an owned offscreen texture or the window swapchain, plus the total
// algebra (composite/transform/feedback/blur/threshold/bloom/paste). Every
// sampling operation routes through detail::compositor(); the ping-pong behind
// the in-place transforms is hidden in a lazily-allocated scratch texture.
//
// Surface::load / Surface::save are declared in braid.h but DEFINED in the
// braid-image addon (braid_image.cpp) — link-to-enable, keeps mango out of core.
#include "braid.h"
#include "braid_compositor.h"

#include <utility>

namespace braid {

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

// Separable blur: H pass (main→scratch), then V pass (scratch→main).
// Two separate submits guarantee the GPU barrier between the passes.
Surface& Surface::blur(float radius) {
    if (swapchain_ || !view_ || radius <= 0) return *this;
    ensureScratch();
    glm::vec2 ts{1.0f / width_, 1.0f / height_};
    {
        wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
        detail::compositor().blurPass(enc, scratchView_, format_, view_, ts, {1, 0}, radius);
        wgpu::CommandBuffer cmd = enc.Finish();
        device_.GetQueue().Submit(1, &cmd);
    }
    {
        wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
        detail::compositor().blurPass(enc, view_, format_, scratchView_, ts, {0, 1}, radius);
        wgpu::CommandBuffer cmd = enc.Finish();
        device_.GetQueue().Submit(1, &cmd);
    }
    return *this;
}

Surface& Surface::threshold(float level, float knee) {
    if (swapchain_ || !view_) return *this;
    ensureScratch();
    wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
    detail::compositor().thresholdPass(enc, scratchView_, format_, view_, level, knee);
    wgpu::CommandBuffer cmd = enc.Finish();
    device_.GetQueue().Submit(1, &cmd);
    swapScratch();
    return *this;
}

Surface& Surface::contour(float level, float radius, int mode) {
    if (swapchain_ || !view_) return *this;
    ensureScratch();
    glm::vec2 ts{1.0f / width_, 1.0f / height_};
    wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
    detail::compositor().contourPass(enc, scratchView_, format_, view_, ts, level, radius, mode);
    wgpu::CommandBuffer cmd = enc.Finish();
    device_.GetQueue().Submit(1, &cmd);
    swapScratch();
    return *this;
}

// bloom = clone → threshold → blur → additive-composite back.
// passes scales the blur radius (passes=5 → ~20px). Total: three render submits + one clone.
Surface& Surface::bloom(float threshold_level, float intensity, int passes) {
    auto glowR = clone();
    if (!glowR) return *this;
    auto& glow = *glowR;
    glow.threshold(threshold_level);
    glow.blur(4.0f * float(passes));
    if (intensity != 1.0f) glow.multiply({intensity, intensity, intensity, 1.0f});
    *this += glow;
    return *this;
}

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

}  // namespace braid
