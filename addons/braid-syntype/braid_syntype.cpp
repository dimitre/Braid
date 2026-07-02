// braid_syntype.cpp — Stick-letter text renderer for Braid
#include "braid_syntype.h"
#include "braid_detail.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace braid {

// ── String helpers ───────────────────────────────────────────────

static std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) out.push_back(item);
    return out;
}

// ── Glyph name → char mapping ────────────────────────────────────

static char mapGlyphName(const std::string& name) {
    if (name.empty()) return 0;
    // Direct single-character mapping
    if (name.size() == 1) {
        unsigned char c = name[0];
        if (c >= 32 && c < 127) return static_cast<char>(c);
        return 0;
    }
    static const std::unordered_map<std::string, char> kMap = {
        {"space", ' '},
        {"zero", '0'}, {"one", '1'}, {"two", '2'}, {"three", '3'}, {"four", '4'},
        {"five", '5'}, {"six", '6'}, {"seven", '7'}, {"eight", '8'}, {"nine", '9'},
        {"period", '.'}, {"comma", ','}, {"colon", ':'}, {"semicolon", ';'},
        {"exclam", '!'}, {"exclamdown", '\xA1'},
        {"question", '?'}, {"questiondown", '\xBF'},
        {"hyphen", '-'}, {"underscore", '_'}, {"emdash", '\x97'}, {"endash", '\x96'},
        {"slash", '/'}, {"backslash", '\\'}, {"bar", '|'}, {"brokenbar", '\xA6'},
        {"parenleft", '('}, {"parenright", ')'},
        {"braceleft", '{'}, {"braceright", '}'},
        {"bracketleft", '['}, {"bracketright", ']'},
        {"less", '<'}, {"greater", '>'},
        {"equal", '='}, {"plus", '+'}, {"minus", '-'},
        {"multiply", '\xD7'}, {"divide", '\xF7'}, {"plusminus", '\xB1'},
        {"percent", '%'}, {"numbersign", '#'}, {"at", '@'}, {"ampersand", '&'},
        {"dollar", '$'}, {"sterling", '\xA3'}, {"Euro", '\x80'}, {"yen", '\xA5'}, {"cent", '\xA2'},
        {"currency", '\xA4'},
        {"quotedbl", '"'}, {"quotesingle", '\''}, {"quoteleft", '`'}, {"quoteright", '\''},
        {"quotedblleft", '"'}, {"quotedblright", '"'},
        {"asciitilde", '~'}, {"asciicircum", '^'},
        {"dieresis", '\xA8'}, {"cedilla", '\xB8'}, {"ring", '\xB0'},
        {"macron", '\xAF'}, {"breve", 0}, {"caron", 0}, {"circumflex", 0},
        {"dotaccent", 0}, {"ogonek", 0}, {"periodcentered", '\xB7'},
        {"bullet", '\x95'}, {"ellipsis", '\x85'},
        {"asterisk", '*'}, {"dagger", '\x86'}, {"daggerdbl", '\x87'},
        {"section", '\xA7'}, {"paragraph", '\xB6'},
        {"copyright", '\xA9'}, {"registered", '\xAE'}, {"trademark", '\x99'},
        {"ordfeminine", '\xAA'}, {"ordmasculine", '\xBA'},
        {"guillemotleft", '\xAB'}, {"guillemotright", '\xBB'},
        {"guilsinglleft", '\x8B'}, {"guilsinglright", '\x9B'},
        {"onequarter", '\xBC'}, {"onehalf", '\xBD'}, {"threequarters", '\xBE'},
        {"uni00B9", '\xB9'}, {"uni00B2", '\xB2'}, {"uni00B3", '\xB3'},
        {"ae", '\xE6'}, {"AE", '\xC6'}, {"oe", 0}, {"OE", 0},
        {"oslash", '\xF8'}, {"Oslash", '\xD8'},
        {"thorn", '\xFE'}, {"Thorn", '\xDE'}, {"eth", '\xF0'}, {"Eth", '\xD0'},
        {"germandbls", '\xDF'},
        {"mu", '\xB5'}, {"uni03BC", '\xB5'},
        {"dotlessi", 0}, {"dotlessj", 0}, {"uni0237", 0},
        {"fraction", 0},
        {"arrowup", 0}, {"arrowdown", 0},
        {"arrowleft", 0}, {"arrowright", 0},
        {"arrowupdn", 0}, {"arrowboth", 0},
        {"uni2196", 0}, {"uni2197", 0},
        {"uni2198", 0}, {"uni2199", 0},
    };
    auto it = kMap.find(name);
    if (it != kMap.end()) return it->second;
    return 0;
}

// ── SyntypeFont ──────────────────────────────────────────────────

Result<SyntypeFont> SyntypeFont::load(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path p(path);
    std::vector<fs::path> files;

    if (fs::is_regular_file(p) && p.extension() == ".txt") {
        files.push_back(p);
    } else if (fs::is_directory(p)) {
        for (const auto& entry : fs::directory_iterator(p)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                files.push_back(entry.path());
            }
        }
    }

    if (files.empty()) {
        return Result<SyntypeFont>::failure("SyntypeFont::load: no .txt font file found at: " + path);
    }

    SyntypeFont font;
    std::unordered_set<char> seen;

    for (const auto& file : files) {
        std::ifstream f(file);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;

            auto colonPos = line.find(':');
            if (colonPos == std::string::npos) continue;

            std::string name = trim(line.substr(0, colonPos));
            if (name.empty()) continue;

            // Strip variant suffix (.alt, .alt2, etc.)
            std::string baseName = name;
            auto dotPos = baseName.find('.');
            if (dotPos != std::string::npos) baseName = baseName.substr(0, dotPos);

            char glyphChar = mapGlyphName(baseName);
            if (glyphChar == 0) continue;
            if (seen.count(glyphChar)) continue;  // keep first variant
            seen.insert(glyphChar);

            std::string data = line.substr(colonPos + 1);
            // Attempt to strip trailing :w,h scale factor
            auto lastColon = data.rfind(':');
            if (lastColon != std::string::npos) {
                std::string scaleStr = trim(data.substr(lastColon + 1));
                auto comma = scaleStr.find(',');
                if (comma != std::string::npos) {
                    try {
                        std::stof(scaleStr.substr(0, comma));
                        std::stof(scaleStr.substr(comma + 1));
                        data = data.substr(0, lastColon);
                    } catch (...) {
                        // not a scale factor
                    }
                }
            }

            auto strokeTokens = split(data, '|');
            Glyph g;
            float maxX = 0.0f;

            for (auto& strokeToken : strokeTokens) {
                strokeToken = trim(strokeToken);
                if (strokeToken.empty()) continue;

                std::vector<glm::vec2> points;
                auto coordTokens = split(strokeToken, ' ');
                for (auto& coord : coordTokens) {
                    coord = trim(coord);
                    if (coord.empty()) continue;
                    auto commaPos = coord.find(',');
                    if (commaPos == std::string::npos) continue;
                    float x = std::stof(coord.substr(0, commaPos)) * font.baseScale;
                    float y = -std::stof(coord.substr(commaPos + 1)) * font.baseScale;
                    points.push_back({x, y});
                    maxX = std::max(maxX, x);
                }
                if (points.size() >= 2) {
                    g.strokes.push_back(std::move(points));
                }
            }

            g.advance = maxX + (5.0f * font.baseScale);
            font.glyphs_[glyphChar] = std::move(g);
        }
    }

    if (!font.hasGlyph(' ')) {
        Glyph space;
        space.advance = 5.0f * font.baseScale;
        font.glyphs_[' '] = std::move(space);
    }

    if (font.glyphs_.empty()) {
        return Result<SyntypeFont>::failure("SyntypeFont::load: no glyphs parsed from: " + path);
    }

    return Result<SyntypeFont>::success(std::move(font));
}

void SyntypeFont::insert(char c, Glyph glyph) {
    glyphs_[c] = std::move(glyph);
}

const Glyph* SyntypeFont::glyph(char c) const {
    auto it = glyphs_.find(c);
    return it != glyphs_.end() ? &it->second : nullptr;
}

bool SyntypeFont::hasGlyph(char c) const {
    return glyphs_.find(c) != glyphs_.end();
}

// ── WGSL shader (embedded for reliability) ───────────────────────

static const char* kSyntypeWGSL = R"WGSL(
struct VertexInput {
    @location(0) position: vec2<f32>,
    @location(1) side: f32,
    @location(2) tangent: vec2<f32>,
    @location(3) distortion: vec2<f32>,
};

struct Uniforms {
    transform: mat4x4<f32>,
    color: vec4<f32>,
    thickness: f32,
};

@group(0) @binding(0) var<uniform> u: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) color: vec4<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
    var pos = in.position + in.distortion;
    let perp = vec2<f32>(-in.tangent.y, in.tangent.x);
    pos = pos + in.side * u.thickness * perp;

    var out: VertexOutput;
    out.position = u.transform * vec4<f32>(pos, 0.0, 1.0);
    out.color = u.color;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    return in.color;
}
)WGSL";

// ── Syntype ──────────────────────────────────────────────────────

Syntype::Syntype(wgpu::Device device) : device_(device) {}

void Syntype::ensureBufferCapacity(size_t vertexCount, size_t indexCount) {
    size_t needVBytes = vertexCount * sizeof(Vertex);
    size_t needIBytes = indexCount * sizeof(uint16_t);

    if (!vertexBuffer_ || vertexBufferCapacity_ < needVBytes) {
        size_t newCap = needVBytes * 2;
        if (newCap < 65536) newCap = 65536;
        wgpu::BufferDescriptor bd{};
        bd.size = newCap;
        bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        vertexBuffer_ = device_.CreateBuffer(&bd);
        vertexBufferCapacity_ = newCap;
    }

    if (!indexBuffer_ || indexBufferCapacity_ < needIBytes) {
        size_t newCap = needIBytes * 2;
        if (newCap < 65536) newCap = 65536;
        wgpu::BufferDescriptor bd{};
        bd.size = newCap;
        bd.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        indexBuffer_ = device_.CreateBuffer(&bd);
        indexBufferCapacity_ = newCap;
    }
}

void Syntype::uploadBatch(const std::vector<Vertex>& verts,
                          const std::vector<uint16_t>& indices) {
    ensureBufferCapacity(verts.size(), indices.size());
    device_.GetQueue().WriteBuffer(vertexBuffer_, 0, verts.data(),
                                    verts.size() * sizeof(Vertex));
    device_.GetQueue().WriteBuffer(indexBuffer_, 0, indices.data(),
                                    indices.size() * sizeof(uint16_t));
}

wgpu::RenderPipeline Syntype::ensurePipeline(wgpu::TextureFormat fmt) {
    for (auto& [f, p] : pipelineCache_) {
        if (f == fmt) return p;
    }

    wgpu::ShaderSourceWGSL wgslDesc{};
    wgslDesc.code = kSyntypeWGSL;
    wgpu::ShaderModuleDescriptor smd{};
    smd.nextInChain = &wgslDesc;
    smd.label = "syntype-line";
    wgpu::ShaderModule module = device_.CreateShaderModule(&smd);

    wgpu::VertexAttribute attrs[4] = {
        {.format = wgpu::VertexFormat::Float32x2,
         .offset = offsetof(Vertex, position),
         .shaderLocation = 0},
        {.format = wgpu::VertexFormat::Float32,
         .offset = offsetof(Vertex, side),
         .shaderLocation = 1},
        {.format = wgpu::VertexFormat::Float32x2,
         .offset = offsetof(Vertex, tangent),
         .shaderLocation = 2},
        {.format = wgpu::VertexFormat::Float32x2,
         .offset = offsetof(Vertex, distortion),
         .shaderLocation = 3},
    };
    wgpu::VertexBufferLayout vbl{};
    vbl.arrayStride = sizeof(Vertex);
    vbl.stepMode = wgpu::VertexStepMode::Vertex;
    vbl.attributeCount = 4;
    vbl.attributes = attrs;

    if (!bindGroupLayout_) {
        wgpu::BindGroupLayoutEntry e{};
        e.binding = 0;
        e.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        e.buffer.type = wgpu::BufferBindingType::Uniform;
        e.buffer.minBindingSize = sizeof(Uniforms);
        wgpu::BindGroupLayoutDescriptor bgld{};
        bgld.entryCount = 1;
        bgld.entries = &e;
        bindGroupLayout_ = device_.CreateBindGroupLayout(&bgld);
    }

    wgpu::PipelineLayoutDescriptor pld{};
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &bindGroupLayout_;
    wgpu::PipelineLayout pl = device_.CreatePipelineLayout(&pld);

    wgpu::ColorTargetState target{};
    target.format = fmt;
    target.blend = &Blend::Alpha;
    target.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag{};
    frag.module = module;
    frag.entryPoint = "fs_main";
    frag.targetCount = 1;
    frag.targets = &target;

    wgpu::RenderPipelineDescriptor desc{};
    desc.layout = pl;
    desc.vertex.module = module;
    desc.vertex.entryPoint = "vs_main";
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &vbl;
    desc.fragment = &frag;
    desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFF;

    wgpu::RenderPipeline pipe = device_.CreateRenderPipeline(&desc);
    pipelineCache_.push_back({fmt, pipe});
    return pipe;
}

void Syntype::drawInternal(Surface& target,
                           const SyntypeFont& font,
                           const std::string& text,
                           glm::vec2 pos,
                           float size,
                           glm::vec4 color,
                           const std::vector<glm::vec2>* distortionOffsets) {
    if (!device_) return;

    float scale = size / font.baseScale;
    float x = pos.x;
    float y = pos.y;

    std::vector<Vertex> verts;
    std::vector<uint16_t> indices;
    uint16_t baseIndex = 0;
    size_t strokeIndex = 0;

    for (char c : text) {
        if (c == ' ') {
            if (auto* g = font.glyph(' ')) {
                x += g->advance * scale;
            } else {
                x += size * 0.5f;
            }
            continue;
        }

        const Glyph* g = font.glyph(c);
        if (!g) {
            x += size * 0.5f;
            continue;
        }

        for (const auto& stroke : g->strokes) {
            if (stroke.size() < 2) continue;

            glm::vec2 distortion{0, 0};
            if (distortionOffsets) {
                distortion = (*distortionOffsets)[strokeIndex % distortionOffsets->size()];
            }

            for (size_t i = 0; i < stroke.size(); ++i) {
                glm::vec2 p = glm::vec2(x, y) + stroke[i] * scale;

                glm::vec2 tan;
                if (stroke.size() == 1) {
                    tan = glm::vec2(1, 0);
                } else if (i == 0) {
                    tan = glm::normalize(stroke[1] - stroke[0]);
                } else if (i == stroke.size() - 1) {
                    tan = glm::normalize(stroke[i] - stroke[i - 1]);
                } else {
                    tan = glm::normalize(stroke[i + 1] - stroke[i - 1]);
                }
                if (glm::length(tan) < 1e-5f) tan = glm::vec2(1, 0);

                verts.push_back({p, -1.0f, tan, distortion});
                verts.push_back({p, +1.0f, tan, distortion});

                if (i > 0) {
                    uint16_t pL = baseIndex + static_cast<uint16_t>((i - 1) * 2);
                    uint16_t pR = pL + 1;
                    uint16_t cL = baseIndex + static_cast<uint16_t>(i * 2);
                    uint16_t cR = cL + 1;
                    indices.insert(indices.end(), {pL, pR, cL, pR, cR, cL});
                }
            }
            baseIndex += static_cast<uint16_t>(stroke.size() * 2);
            ++strokeIndex;
        }
        x += g->advance * scale;
    }

    if (verts.empty()) return;

    uploadBatch(verts, indices);

    Uniforms u;
    u.transform =
        glm::ortho(0.0f, static_cast<float>(target.width()),
                   static_cast<float>(target.height()), 0.0f, -1.0f, 1.0f);
    u.color = color;
    u.thickness = size * 0.08f;

    wgpu::Buffer uniformBuf;
    {
        wgpu::BufferDescriptor bd{};
        bd.size = sizeof(Uniforms);
        bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        uniformBuf = device_.CreateBuffer(&bd);
    }
    device_.GetQueue().WriteBuffer(uniformBuf, 0, &u, sizeof(u));

    wgpu::BindGroupEntry be{};
    be.binding = 0;
    be.buffer = uniformBuf;
    be.offset = 0;
    be.size = sizeof(Uniforms);

    wgpu::TextureFormat fmt = target.format();
    wgpu::RenderPipeline pipe = ensurePipeline(fmt);

    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = bindGroupLayout_;
    bgd.entryCount = 1;
    bgd.entries = &be;
    wgpu::BindGroup bg = device_.CreateBindGroup(&bgd);

    if (detail::ctx().currentPass) {
        detail::ctx().currentPass->SetPipeline(pipe);
        detail::ctx().currentPass->SetVertexBuffer(
            0, vertexBuffer_, 0, verts.size() * sizeof(Vertex));
        detail::ctx().currentPass->SetIndexBuffer(
            indexBuffer_, wgpu::IndexFormat::Uint16, 0,
            indices.size() * sizeof(uint16_t));
        detail::ctx().currentPass->SetBindGroup(0, bg);
        detail::ctx().currentPass->DrawIndexed(
            static_cast<uint32_t>(indices.size()));
    } else {
        wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
        wgpu::RenderPassEncoder pass = target.beginLoad(enc);
        pass.SetPipeline(pipe);
        pass.SetVertexBuffer(0, vertexBuffer_, 0, verts.size() * sizeof(Vertex));
        pass.SetIndexBuffer(indexBuffer_, wgpu::IndexFormat::Uint16, 0,
                             indices.size() * sizeof(uint16_t));
        pass.SetBindGroup(0, bg);
        pass.DrawIndexed(static_cast<uint32_t>(indices.size()));
        target.end(pass);
        wgpu::CommandBuffer cmd = enc.Finish();
        device_.GetQueue().Submit(1, &cmd);
    }
}

void Syntype::draw(Surface& target,
                   const SyntypeFont& font,
                   const std::string& text,
                   glm::vec2 pos,
                   float size,
                   glm::vec4 color) {
    drawInternal(target, font, text, pos, size, color, nullptr);
}

void Syntype::drawDistorted(Surface& target,
                            const SyntypeFont& font,
                            const std::string& text,
                            glm::vec2 pos,
                            float size,
                            glm::vec4 color,
                            const std::vector<glm::vec2>& distortionOffsets) {
    drawInternal(target, font, text, pos, size, color, &distortionOffsets);
}

glm::vec2 Syntype::measure(const SyntypeFont& font,
                           const std::string& text,
                           float size) const {
    float scale = size / font.baseScale;
    float width = 0.0f;
    float maxHeight = 0.0f;

    for (char c : text) {
        const Glyph* g = font.glyph(c);
        if (!g) {
            width += size * 0.5f;
            continue;
        }
        width += g->advance * scale;

        for (const auto& stroke : g->strokes) {
            for (const auto& p : stroke) {
                maxHeight = std::max(maxHeight, std::abs(p.y) * scale);
            }
        }
    }
    return {width, maxHeight};
}

void Syntype::drawCentered(Surface& target,
                           const SyntypeFont& font,
                           const std::string& text,
                           glm::vec2 center,
                           float size,
                           glm::vec4 color) {
    glm::vec2 dim = measure(font, text, size);
    glm::vec2 pos{center.x - dim.x * 0.5f, center.y + dim.y * 0.5f};
    draw(target, font, text, pos, size, color);
}

}  // namespace braid
