// braid_syntype.h — Stick-letter text renderer for Braid
// WebGPU line-strip text using quad-extruded thick strokes.
#pragma once

#include "braid.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace braid {

// ── Glyph Data ───────────────────────────────────────────────────

struct Glyph {
    std::vector<std::vector<glm::vec2>> strokes;  // each stroke = polyline of vec2 points
    float advance = 0.0f;                          // horizontal spacing width
};

// ── Font Loader ──────────────────────────────────────────────────

class SyntypeFont {
public:
    float baseScale = 22.0f;  // design-space scale; loaded glyphs are pre-scaled by this

    // Load from a .txt stroke-font file or a directory containing one.
    // If path is a file ending in .txt, loads it directly.
    // If path is a directory, loads the first .txt file found inside.
    static Result<SyntypeFont> load(const std::string& path);

    // Direct insertion for programmatic fonts
    void insert(char c, Glyph glyph);

    const Glyph* glyph(char c) const;
    bool hasGlyph(char c) const;

private:
    std::unordered_map<char, Glyph> glyphs_;
};

// ── Renderer ─────────────────────────────────────────────────────

class Syntype {
public:
    // Initialize with a Device reference from braid::App
    explicit Syntype(wgpu::Device device);

    // Simple text draw — analogous to ofDrawBitmapString
    // pos = baseline origin (left, bottom in screen pixels)
    // size = pixel height on screen
    // color = RGBA (0-1)
    void draw(Surface& target,
              const SyntypeFont& font,
              const std::string& text,
              glm::vec2 pos,
              float size = 16.0f,
              glm::vec4 color = {1, 1, 1, 1});

    // Audio-reactive variant — each stroke gets a distortion vector
    // distortionOffsets: per-stroke offset (size >= number of strokes in text, or reused cyclically)
    void drawDistorted(Surface& target,
                       const SyntypeFont& font,
                       const std::string& text,
                       glm::vec2 pos,
                       float size,
                       glm::vec4 color,
                       const std::vector<glm::vec2>& distortionOffsets);

    // Bounding box of a string (for hit-testing, centering, etc.)
    // Returns width and height in screen pixels at given size
    glm::vec2 measure(const SyntypeFont& font, const std::string& text, float size) const;

    // Draw from center (convenience)
    void drawCentered(Surface& target,
                      const SyntypeFont& font,
                      const std::string& text,
                      glm::vec2 center,
                      float size = 16.0f,
                      glm::vec4 color = {1, 1, 1, 1});

private:
    wgpu::Device device_;
    wgpu::BindGroupLayout bindGroupLayout_;
    std::vector<std::pair<wgpu::TextureFormat, wgpu::RenderPipeline>> pipelineCache_;

    // Reusable GPU buffers for batching
    wgpu::Buffer vertexBuffer_;
    wgpu::Buffer indexBuffer_;
    size_t vertexBufferCapacity_ = 0;
    size_t indexBufferCapacity_ = 0;

    struct Vertex {
        glm::vec2 position;
        float side;
        glm::vec2 tangent;
        glm::vec2 distortion;
    };

    struct alignas(16) Uniforms {
        glm::mat4 transform;
        glm::vec4 color;
        float thickness;
        float _pad[3];
    };

    void ensureBufferCapacity(size_t vertexCount, size_t indexCount);
    void uploadBatch(const std::vector<Vertex>& verts, const std::vector<uint16_t>& indices);
    wgpu::RenderPipeline ensurePipeline(wgpu::TextureFormat fmt);
    void drawInternal(Surface& target,
                      const SyntypeFont& font,
                      const std::string& text,
                      glm::vec2 pos,
                      float size,
                      glm::vec4 color,
                      const std::vector<glm::vec2>* distortionOffsets);
};

} // namespace braid
