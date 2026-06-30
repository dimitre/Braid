# Braid Addon: `braid-syntype`
## Stick-Letter Text Rendering for Debug UI & Creative Typography

---

### 1. Goal

Provide a minimal, dependency-free text renderer for Braid that draws stick/skeleton fonts as GPU line strips. Think `ofDrawBitmapString` or `ofTrueTypeFont` but for vector stroke fonts. Supports debug labels, UI chrome, and audio-reactive distortion out of the box.

---

### 2. File Layout

```
braid-syntype/
├── braid_syntype.h          // public API
├── braid_syntype.cpp        // implementation
├── shaders/
│   └── syntype_line.wgsl    // vertex + fragment
└── fonts/
    └── arame/               // default bundled font (your .txt format)
        ├── A.txt
        ├── B.txt
        ├── ...
        └── space.txt
```

---

### 3. Public API

```cpp
#pragma once
#include "braid.h"

namespace braid {

// ── Glyph Data ───────────────────────────────────────────────────

struct Glyph {
    std::vector<std::vector<glm::vec2>> strokes;  // each stroke = polyline of vec2 points
    float advance = 0.0f;                          // horizontal spacing width
};

// ── Font Loader ──────────────────────────────────────────────────

class SyntypeFont {
public:
    float baseScale = 22.0f;  // matches your OF default; loaded glyphs are pre-scaled

    // Load all .txt files from a directory. Each file named like "A.txt", "space.txt"
    static Result<SyntypeFont> load(const std::string& directoryPath);

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
              glm::vec4 color = {1,1,1,1});

    // Audio-reactive variant — each stroke gets a distortion vector
    // fftBins: per-stroke offset (size >= number of strokes in text, or reused cyclically)
    // distortionScale: max pixel displacement
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
                      glm::vec4 color = {1,1,1,1});

private:
    wgpu::Device device_;
    wgpu::RenderPipeline pipeline_;
    wgpu::BindGroupLayout bindGroupLayout_;
    wgpu::Buffer uniformBuffer_;

    // Reusable GPU buffers for batching
    wgpu::Buffer vertexBuffer_;
    wgpu::Buffer indexBuffer_;
    size_t vertexBufferCapacity_ = 0;
    size_t indexBufferCapacity_ = 0;

    struct alignas(16) Uniforms {
        glm::mat4 transform;
        glm::vec4 color;
        float thickness;
        float _pad[3];
    };

    void ensureBufferCapacity(size_t vertexCount, size_t indexCount);
    void uploadBatch(const std::vector<glm::vec2>& verts, const std::vector<uint16_t>& indices);
};

} // namespace braid
```

---

### 4. `.txt` Font Format (unchanged from your OF version)

Each file is one glyph. Filename = character (e.g. `A.txt`, `space.txt`).

```
# Comment line — ignored
# Stroke format:  x0,y0 x1,y1 x2,y2 | x3,y3 x4,y4
# | separates strokes
# Coordinates are in an arbitrary design space; scaled by baseScale on load

A: 0,0 10,20 20,0 | 10,20 10,30
```

- Lines starting with `#` are ignored.
- Empty lines are ignored.
- Format per line: `GLYPHCHAR: stroke0 | stroke1 | stroke2 ...`
- Each stroke: space-separated `x,y` points.
- `space.txt` (or ` .txt`) defines the space advance.

---

### 5. Implementation Notes

#### 5.1 Font Loading (`SyntypeFont::load`)

```cpp
Result<SyntypeFont> SyntypeFont::load(const std::string& dir) {
    SyntypeFont font;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".txt") continue;

        std::string stem = entry.path().stem().string();
        char glyphChar = (stem == "space") ? ' ' : stem[0];

        auto content = readFile(entry.path());
        auto lines = splitLines(content);
        Glyph g;
        float maxX = 0.0f;

        for (auto& line : lines) {
            trim(line);
            if (line.empty() || line[0] == '#') continue;

            auto colonPos = line.find(':');
            if (colonPos == std::string::npos) continue;

            std::string strokesStr = line.substr(colonPos + 1);
            auto strokeTokens = split(strokesStr, '|');

            for (auto& strokeToken : strokeTokens) {
                trim(strokeToken);
                if (strokeToken.empty()) continue;

                std::vector<glm::vec2> points;
                auto coordTokens = split(strokeToken, ' ');
                for (auto& coord : coordTokens) {
                    trim(coord);
                    if (coord.empty()) continue;
                    auto commaPos = coord.find(',');
                    if (commaPos == std::string::npos) continue;
                    float x = std::stof(coord.substr(0, commaPos)) * font.baseScale;
                    float y = -std::stof(coord.substr(commaPos + 1)) * font.baseScale;  // flip Y
                    points.push_back({x, y});
                    maxX = std::max(maxX, x);
                }
                if (points.size() >= 2) {
                    g.strokes.push_back(std::move(points));
                }
            }
        }
        g.advance = maxX + (5.0f * font.baseScale);  // your OF spacing logic
        font.glyphs_[glyphChar] = std::move(g);
    }
    // Ensure space exists
    if (!font.hasGlyph(' ')) {
        Glyph space; space.advance = 5.0f * font.baseScale;
        font.glyphs_[' '] = std::move(space);
    }
    return font;
}
```

#### 5.2 GPU Pipeline Setup

- **Primitive**: `LineStrip` (or `TriangleStrip` if extruding in geometry shader)
- **For MVP**: Use `LineStrip` with `lineWidth` set via `RenderPipelineDescriptor.primitive.topology = WGPUPrimitiveTopology_LineStrip`. Note: WebGPU `lineWidth` is 1.0 only on some backends (Dawn may support >1 via native extension). For thick lines, use a **screen-space quad extrusion** in vertex shader (see shader section).
- **Vertex format**: `vec2 position` per point. No UVs, no normals.
- **Uniforms per draw call**: `mat4 transform`, `vec4 color`, `float thickness`.
- **Batching**: For a single `draw()` call, accumulate all stroke points from all glyphs into one vertex buffer, upload once, draw with `drawIndexed`.

#### 5.3 Vertex Shader (Screen-Space Quad Extrusion for Thick Lines)

Instead of relying on `lineWidth` (limited in WebGPU), extrude each stroke into a quad in the vertex shader:

```wgsl
struct VertexInput {
    @location(0) position: vec2<f32>,   // stroke point centerline
    @location(1) side: f32,           // -1.0 or +1.0 (extrusion direction)
    @location(2) distortion: vec2<f32>, // optional audio offset
};

struct Uniforms {
    transform: mat4x4<f32>,
    color: vec4<f32>,
    thickness: f32,
};

@group(0) @binding(0) var<uniform> u: Uniforms;

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
    // Compute tangent from neighboring vertices (passed as instance data or precomputed)
    // Simplification: for MVP, use a geometry shader approach or precompute in CPU
    // CPU precomputation: for each stroke point, store (position, tangent, side)

    var pos = in.position + in.distortion;
    var offset = in.side * u.thickness * vec2(-tangent.y, tangent.x);  // perpendicular
    pos += offset;

    var out: VertexOutput;
    out.position = u.transform * vec4(pos, 0.0, 1.0);
    out.color = u.color;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    return in.color;
}
```

**CPU-side per-point expansion**: For each stroke point, generate TWO vertices (side = -1 and +1). Store tangent as an additional vertex attribute. For stroke caps, add a circle approximation or just let the line strip end.

#### 5.4 `draw()` Implementation Sketch

```cpp
void Syntype::draw(Surface& target, const SyntypeFont& font,
                   const std::string& text, glm::vec2 pos,
                   float size, glm::vec4 color) {
    float scale = size / font.baseScale;
    float x = pos.x;
    float y = pos.y;

    std::vector<Vertex> verts;
    std::vector<uint16_t> indices;
    uint16_t baseIndex = 0;

    for (char c : text) {
        const Glyph* g = font.glyph(c);
        if (!g) { x += size * 0.5f; continue; }  // fallback spacing

        for (const auto& stroke : g->strokes) {
            if (stroke.size() < 2) continue;

            // Compute tangents for each segment
            for (size_t i = 0; i < stroke.size(); ++i) {
                glm::vec2 p = glm::vec2(x, y) + stroke[i] * scale;
                glm::vec2 tan = computeTangent(stroke, i);  // average of adjacent segments

                // Two vertices per point (left and right of stroke)
                verts.push_back({p, -1.0f, tan, {0,0}});
                verts.push_back({p, +1.0f, tan, {0,0}});

                if (i > 0) {
                    // Two triangles per segment: (prevL, prevR, curL), (prevR, curR, curL)
                    uint16_t pL = baseIndex + (i-1)*2;
                    uint16_t pR = pL + 1;
                    uint16_t cL = baseIndex + i*2;
                    uint16_t cR = cL + 1;
                    indices.insert(indices.end(), {pL, pR, cL, pR, cR, cL});
                }
            }
            baseIndex += stroke.size() * 2;
        }
        x += g->advance * scale * 0.5f;  // tune spacing multiplier
    }

    // Upload and draw
    ensureBufferCapacity(verts.size(), indices.size());
    uploadBatch(verts, indices);

    Uniforms u;
    u.transform = target.projectionMatrix();  // or ortho(0, width, height, 0)
    u.color = color;
    u.thickness = size * 0.08f;  // stroke thickness relative to font size
    // ... write uniform buffer, set bind group, drawIndexed
}
```

#### 5.5 `measure()` Implementation

```cpp
glm::vec2 Syntype::measure(const SyntypeFont& font, const std::string& text, float size) const {
    float scale = size / font.baseScale;
    float width = 0.0f;
    float maxHeight = 0.0f;

    for (char c : text) {
        const Glyph* g = font.glyph(c);
        if (!g) { width += size * 0.5f; continue; }
        width += g->advance * scale * 0.5f;

        // Height estimate: max Y excursion across all strokes
        for (const auto& stroke : g->strokes) {
            for (const auto& p : stroke) {
                maxHeight = std::max(maxHeight, std::abs(p.y) * scale);
            }
        }
    }
    return {width, maxHeight};
}
```

---

### 6. Usage Examples

#### 6.1 Basic Debug Text (SketchApp tier)

```cpp
#include "braid.h"
#include "braid_syntype.h"

class MySketch : public braid::SketchApp {
    braid::SyntypeFont font;
    braid::Syntype* syntype = nullptr;

public:
    using braid::SketchApp::SketchApp;

    void setup() override {
        font = braid::SyntypeFont::load("fonts/arame").value();
        syntype = new braid::Syntype(device());  // device() from braid::App
    }

    void draw() override {
        background(0.02f, 0.02f, 0.03f);

        // FPS counter — top-left
        std::string fps = fmt::format("FPS: {:.1f}", 1.0f / deltaTime());
        syntype->draw(surface(), font, fps, {10, 20}, 14.0f, {0,1,0,1});

        // Centered title
        syntype->drawCentered(surface(), font, "BRAID", 
                              {width()*0.5f, height()*0.5f}, 
                              48.0f, {1,0.5,0,1});
    }
};
```

#### 6.2 UI Slider with Label

```cpp
void drawSlider(braid::Surface& s, braid::Syntype& st, const braid::SyntypeFont& font,
                float x, float y, float w, float value, const std::string& label) {
    // Track (using braid primitives)
    fill(0.15f, 0.15f, 0.15f);
    rect(x, y, w, 4);

    // Knob
    fill(0.8f, 0.9f, 1.0f);
    circle(x + value * w, y + 2, 8);

    // Label above
    st.draw(s, font, label, {x, y - 18}, 12.0f, {0.7, 0.7, 0.7, 1});

    // Value below
    std::string valStr = fmt::format("{:.2f}", value);
    st.draw(s, font, valStr, {x + w + 10, y + 4}, 12.0f, {1,1,1,1});
}
```

#### 6.3 Audio-Reactive Distortion

```cpp
void draw() override {
    background(0.0f);

    // Get 8 FFT bands from audio addon (or mock)
    std::vector<glm::vec2> distortion(8);
    for (int i = 0; i < 8; ++i) {
        float amp = audio.fft(i) * 10.0f;  // mock
        distortion[i] = {amp * random(-1,1), amp * random(-1,1)};
    }

    syntype->drawDistorted(surface(), font, "FREQUENCY", 
                           {100, 200}, 32.0f, {1,0.3,0.5,1},
                           distortion);
}
```

---

### 7. Build Integration

Add to `chalet.yaml`:

```yaml
addons:
  braid-syntype:
    source: addons/braid-syntype
    includes: [addons/braid-syntype]
    links: [braid-core]
```

Or if Braid uses a simpler include model, just:

```cpp
// In your sketch
#include "braid_syntype.h"   // header-only or linked
```

---

### 8. Open Questions for Implementation

1. **Line thickness strategy**: 
   - Option A: `LineStrip` with `primitive.lineWidth` (may be 1.0 only in WebGPU spec, but Dawn on Metal/D3D may support more).
   - Option B: Quad extrusion in vertex shader (recommended — works everywhere, gives rounded caps for free with circle joins).
   - **Decision**: Start with Option B for reliability.

2. **Transform space**: 
   - Braid uses `[0,1]` clip space. The ortho matrix for text should be `ortho(0, width, height, 0)` so `pos` is in pixels.
   - Confirm with existing Braid `Surface` projection setup.

3. **Buffer strategy**:
   - Option A: One big reusable vertex/index buffer, re-upload each frame (simple, fine for debug text).
   - Option B: Persistent buffer with ring allocation (overkill for now).
   - **Decision**: Option A. Re-upload per `draw()` call. Debug text is low vertex count.

4. **Font bundling**:
   - Bundle `arame` as default in `braid-syntype/fonts/arame/`.
   - User can load custom fonts via `SyntypeFont::load("path")`.

5. **Missing glyphs**:
   - If a char isn't in the font, draw a `?` or skip with spacing.
   - Add `SyntypeFont::fallbackGlyph(char c)` that maps unknown to `?`.

---

### 9. Deliverables Checklist

- [ ] `braid_syntype.h` — public API header
- [ ] `braid_syntype.cpp` — implementation
- [ ] `syntype_line.wgsl` — WGSL shader (quad extrusion)
- [ ] `fonts/arame/*.txt` — default bundled font (port your existing OF .txt files)
- [ ] Sample sketch: `examples/syntype_basic` — FPS counter + centered text
- [ ] Sample sketch: `examples/syntype_ui` — sliders, toggles, labels
- [ ] Sample sketch: `examples/syntype_audio` — FFT-distorted text
- [ ] `README.md` — usage + font format spec

---

### 10. Relation to OF Syntype

| Feature | OF `ofxSyntype` | Braid `braid-syntype` |
|---------|-----------------|----------------------|
| Font format | `.txt` stroke files | **Same** — drop-in compatible |
| Render target | `ofVboMesh` + `ofPolyline` | `Surface` + GPU buffers |
| Stroke render | `OF_MESH_TRIANGLE_STRIP` | Quad extrusion in WGSL |
| UI dependency | `ofxDmtrUI` | None — pure Braid |
| Audio distortion | Manual vertex manipulation | Built-in `drawDistorted()` |
| Scale | `float scale = 22.0` | `float baseScale = 22.0` (same default) |

---

*Locked for Braid v0.1 — June 2026*
