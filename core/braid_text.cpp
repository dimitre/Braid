// braid_text.cpp — BitmapFont implementation for the Cherry 13-r built-in font.
#include "braid_text.h"
#include "braid_font_cherry13.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace braid {

namespace {

constexpr int kTabStop = 8;

namespace G = braid::font::cherry13;

BitmapFont::Glyph ToGlyph(const G::Glyph& g) {
    return BitmapFont::Glyph{
        g.atlasX, g.atlasY,
        g.width,  g.height,
        g.xoff,   g.yoff,
        g.advance,
    };
}

}  // namespace

BitmapFont::BitmapFont(wgpu::Device device) : device_(device) {
    for (int i = 0; i < 256; ++i) {
        glyphs_[i] = ToGlyph(G::kGlyphs[i]);
    }

    atlasWidth_  = G::kAtlasWidth;
    atlasHeight_ = G::kAtlasHeight;
    cellWidth_   = G::kGlyphWidth;
    cellHeight_  = G::kGlyphHeight;
    lineHeight_  = G::kLineHeight;

    // 1) Upload the atlas as a single-channel R8 texture.
    wgpu::TextureDescriptor tdesc{};
    tdesc.size = {static_cast<uint32_t>(atlasWidth_),
                  static_cast<uint32_t>(atlasHeight_), 1};
    tdesc.format = wgpu::TextureFormat::R8Unorm;
    tdesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    atlas_ = device_.CreateTexture(&tdesc);
    view_ = atlas_.CreateView();

    wgpu::TexelCopyTextureInfo dst{};
    dst.texture = atlas_;
    dst.origin = {0, 0, 0};
    dst.aspect = wgpu::TextureAspect::All;
    wgpu::TexelCopyBufferLayout layout{};
    layout.offset = 0;
    layout.bytesPerRow = static_cast<uint32_t>(atlasWidth_);
    layout.rowsPerImage = static_cast<uint32_t>(atlasHeight_);
    device_.GetQueue().WriteTexture(&dst, G::kAtlas,
                                    static_cast<size_t>(atlasWidth_ * atlasHeight_),
                                    &layout, &tdesc.size);

    // 2) Nearest sampling keeps bitmap text crisp on Retina.
    wgpu::SamplerDescriptor sdesc{};
    sdesc.magFilter = wgpu::FilterMode::Nearest;
    sdesc.minFilter = wgpu::FilterMode::Nearest;
    sampler_ = device_.CreateSampler(&sdesc);

    // 3) Bind group layout: texture @ 0, sampler @ 1.
    std::array<wgpu::BindGroupLayoutEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;

    wgpu::BindGroupLayoutDescriptor bgld{};
    bgld.entryCount = entries.size();
    bgld.entries = entries.data();
    bgl_ = device_.CreateBindGroupLayout(&bgld);

    // 4) Bind group.
    std::array<wgpu::BindGroupEntry, 2> bgEntries{};
    bgEntries[0].binding = 0;
    bgEntries[0].textureView = view_;
    bgEntries[1].binding = 1;
    bgEntries[1].sampler = sampler_;

    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = bgl_;
    bgd.entryCount = bgEntries.size();
    bgd.entries = bgEntries.data();
    bindGroup_ = device_.CreateBindGroup(&bgd);
}

const BitmapFont::Glyph* BitmapFont::glyph(int codepoint) const {
    if (codepoint < 0 || codepoint > 255) return nullptr;
    const Glyph& g = glyphs_[codepoint];
    if (g.advance == 0) return nullptr;
    return &g;
}

std::vector<Vertex> BitmapFont::buildQuads(std::string_view text, float x, float y,
                                           const glm::vec4& color, float scale) const {
    std::vector<Vertex> out;
    out.reserve(text.size() * 6);

    float penX = x;
    float penY = y;
    int col = 0;
    const float advance = static_cast<float>(cellWidth_) * scale;
    const float lineH = static_cast<float>(lineHeight_) * scale;
    const float invW = 1.0f / static_cast<float>(atlasWidth_);
    const float invH = 1.0f / static_cast<float>(atlasHeight_);

    for (unsigned char ch : text) {
        if (ch == '\n') {
            penX = x;
            penY += lineH;
            col = 0;
            continue;
        }
        if (ch == '\t') {
            int nextTab = ((col / kTabStop) + 1) * kTabStop;
            penX += static_cast<float>(nextTab - col) * advance;
            col = nextTab;
            continue;
        }

        const Glyph* g = glyph(static_cast<int>(ch));
        if (!g) {
            penX += advance;
            ++col;
            continue;
        }

        float gx = penX + static_cast<float>(g->xoff) * scale;
        float gy = penY;  // BDF rows are top-to-bottom; top-left placement is correct.
        float gw = static_cast<float>(g->width) * scale;
        float gh = static_cast<float>(g->height) * scale;

        float u0 = static_cast<float>(g->atlasX) * invW;
        float v0 = static_cast<float>(g->atlasY) * invH;
        float u1 = static_cast<float>(g->atlasX + g->width) * invW;
        float v1 = static_cast<float>(g->atlasY + g->height) * invH;

        auto V = [&](float px, float py, float u, float v) {
            return Vertex{{px, py, 0.0f}, {u, v}, {0.0f, 0.0f, 1.0f}, color};
        };

        // First triangle.
        out.push_back(V(gx, gy, u0, v0));
        out.push_back(V(gx + gw, gy, u1, v0));
        out.push_back(V(gx + gw, gy + gh, u1, v1));
        // Second triangle.
        out.push_back(V(gx, gy, u0, v0));
        out.push_back(V(gx + gw, gy + gh, u1, v1));
        out.push_back(V(gx, gy + gh, u0, v1));

        penX += advance;
        ++col;
    }

    return out;
}

glm::vec2 BitmapFont::measure(std::string_view text) const {
    if (text.empty()) return {0.0f, 0.0f};

    float maxW = 0.0f;
    float w = 0.0f;
    int lines = 1;
    int col = 0;
    const float advance = static_cast<float>(cellWidth_);

    for (unsigned char ch : text) {
        if (ch == '\n') {
            maxW = std::max(maxW, w);
            w = 0.0f;
            ++lines;
            col = 0;
        } else if (ch == '\t') {
            int nextTab = ((col / kTabStop) + 1) * kTabStop;
            w += static_cast<float>(nextTab - col) * advance;
            col = nextTab;
        } else {
            w += advance;
            ++col;
        }
    }
    maxW = std::max(maxW, w);
    return {maxW, static_cast<float>(lines * lineHeight_)};
}

}  // namespace braid
