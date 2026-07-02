// braid_compositor.h — internal seam for the fullscreen-quad blit engine (NOT
// part of the public API). The Compositor is the single piece of machinery behind
// every Surface operation that samples a texture: show, composite, transforms,
// feedback, blur, threshold, placed-quad paste, and App's end-of-frame blit.
//
// It is shared by braid_surface.cpp (algebra/transforms) and braid_app.cpp
// (present blit), so its declaration lives here while the implementation +
// WGSL + pipeline caches stay in braid_compositor.cpp. Only core .cpp files
// include this; public code includes braid.h.
#pragma once

#include <array>
#include <tuple>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <webgpu/webgpu_cpp.h>

namespace braid {
namespace detail {

// Uniform layouts shared across TUs (Surface fills these, the Compositor consumes
// them). BlurUniforms/ThresholdUniforms are constructed entirely inside the
// Compositor, so they stay private to braid_compositor.cpp.
struct BlitUniforms {
    glm::vec2 uvScale{1, 1};
    glm::vec2 uvOffset{0, 0};
    glm::vec4 tint{1, 1, 1, 1};
    float rot = 0.0f;
    float invert = 0.0f;
    glm::vec2 _pad{0, 0};
};

struct QuadUniforms {
    glm::vec2 center{0, 0};    // NDC, [-1,1]
    glm::vec2 halfSize{1, 1};  // NDC half-extent (before rotation)
    glm::vec4 tint{1, 1, 1, 1};
    float rot = 0.0f;          // radians, about quad center
    float invert = 0.0f;
    glm::vec2 _pad{0, 0};
};

// One fullscreen-quad shader family that samples a source view with a UV
// transform + invert + tint and blends it into a destination view. Pipelines are
// cached per (format, blend). Lazily initialized via detail::compositor().
class Compositor {
public:
    void init(wgpu::Device device);

    void blit(wgpu::CommandEncoder& enc, wgpu::TextureView dest, wgpu::TextureFormat destFormat,
              wgpu::TextureView src, const BlitUniforms& u, const wgpu::BlendState& blend,
              wgpu::LoadOp loadOp);

    // Paste `src` onto a placed/rotated/scaled quad of `dest` (preserve via Load).
    void quad(wgpu::CommandEncoder& enc, wgpu::TextureView dest, wgpu::TextureFormat destFormat,
              wgpu::TextureView src, const QuadUniforms& u, const wgpu::BlendState& blend,
              wgpu::LoadOp loadOp);

    // One separable blur pass along `dir` (1,0)=H or (0,1)=V. Always overwrites dest.
    void blurPass(wgpu::CommandEncoder& enc, wgpu::TextureView dest, wgpu::TextureFormat destFormat,
                  wgpu::TextureView src, glm::vec2 texelSize, glm::vec2 dir, float radius);

    void thresholdPass(wgpu::CommandEncoder& enc, wgpu::TextureView dest,
                       wgpu::TextureFormat destFormat, wgpu::TextureView src, float level,
                       float knee);

    // Isoline/edge pass (ported from ofworks contour2.frag) — see Surface::contour.
    void contourPass(wgpu::CommandEncoder& enc, wgpu::TextureView dest,
                     wgpu::TextureFormat destFormat, wgpu::TextureView src, glm::vec2 texelSize,
                     float level, float radius, int mode);

private:
    wgpu::RenderPipeline pipelineFor(wgpu::TextureFormat fmt, const wgpu::BlendState& blend);
    wgpu::RenderPipeline quadPipelineFor(wgpu::TextureFormat fmt, const wgpu::BlendState& blend);
    wgpu::RenderPipeline blurPipelineFor(wgpu::TextureFormat fmt);
    wgpu::RenderPipeline thresholdPipelineFor(wgpu::TextureFormat fmt);
    wgpu::RenderPipeline contourPipelineFor(wgpu::TextureFormat fmt);

    wgpu::Device device_;
    wgpu::ShaderModule module_;
    wgpu::ShaderModule quadModule_;
    wgpu::ShaderModule blurModule_;
    wgpu::ShaderModule thresholdModule_;
    wgpu::ShaderModule contourModule_;
    wgpu::Sampler sampler_;
    wgpu::BindGroupLayout bgl_;
    wgpu::PipelineLayout pl_;
    std::array<wgpu::Buffer, 3> ring_{};
    int ringIndex_ = 0;
    std::vector<std::tuple<wgpu::TextureFormat, const void*, wgpu::RenderPipeline>> cache_;
    std::vector<std::tuple<wgpu::TextureFormat, const void*, wgpu::RenderPipeline>> quadCache_;
    std::vector<std::pair<wgpu::TextureFormat, wgpu::RenderPipeline>> blurCache_;
    std::vector<std::pair<wgpu::TextureFormat, wgpu::RenderPipeline>> thresholdCache_;
    std::vector<std::pair<wgpu::TextureFormat, wgpu::RenderPipeline>> contourCache_;
    bool ready_ = false;
};

// The single process-wide Compositor (lazily initialized against ctx().device).
// Lives here for now; de-singletoning it is part of the multi-window refactor.
Compositor& compositor();

}  // namespace detail
}  // namespace braid
