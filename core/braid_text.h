// braid_text.h — built-in bitmap font helper for SketchApp.
// This is the core-owned debug/HUD font. TTF/SDF typography belongs in a
// future braid-text addon, not here.
#pragma once

#include "braid.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace braid {

// A single baked bitmap font, uploaded to a WebGPU texture once on creation.
// Currently hard-wired to the Cherry 13-r font; the API is generic enough that
// it can be pointed at other fixed-cell BDF-derived atlases later.
class BitmapFont {
public:
    struct Glyph {
        uint16_t atlasX = 0;
        uint16_t atlasY = 0;
        uint8_t  width = 0;
        uint8_t  height = 0;
        int8_t   xoff = 0;
        int8_t   yoff = 0;
        uint8_t  advance = 0;
    };

    explicit BitmapFont(wgpu::Device device);

    // Build two triangles per visible character, ready for the SketchApp batch.
    std::vector<Vertex> buildQuads(std::string_view text, float x, float y,
                                   const glm::vec4& color, float scale = 1.0f) const;

    // CPU-side string metrics (same rules as buildQuads).
    glm::vec2 measure(std::string_view text) const;

    const Glyph* glyph(int codepoint) const;

    wgpu::TextureView   view() const { return view_; }
    wgpu::Sampler       sampler() const { return sampler_; }
    wgpu::BindGroup     bindGroup() const { return bindGroup_; }
    wgpu::BindGroupLayout bindGroupLayout() const { return bgl_; }

    int atlasWidth() const { return atlasWidth_; }
    int atlasHeight() const { return atlasHeight_; }
    int cellWidth() const { return cellWidth_; }
    int cellHeight() const { return cellHeight_; }
    int lineHeight() const { return lineHeight_; }

private:
    wgpu::Device device_;
    wgpu::Texture atlas_;
    wgpu::TextureView view_;
    wgpu::Sampler sampler_;
    wgpu::BindGroupLayout bgl_;
    wgpu::BindGroup bindGroup_;

    std::array<Glyph, 256> glyphs_{};

    int atlasWidth_ = 0;
    int atlasHeight_ = 0;
    int cellWidth_ = 0;
    int cellHeight_ = 0;
    int lineHeight_ = 0;
};

}  // namespace braid
