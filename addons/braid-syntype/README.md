# braid-syntype

Stick-letter text renderer for Braid. Draws skeleton/stroke fonts as GPU line strips with quad-extruded thick lines.

## Files

```
braid-syntype/
├── braid_syntype.h       // public API
├── braid_syntype.cpp     // implementation
├── shaders/
│   └── syntype_line.wgsl // WGSL vertex + fragment shader
├── fonts/
│   └── arame.txt         // default bundled stroke font
└── README.md
```

## Usage

```cpp
#include "braid.h"
#include "braid_syntype.h"

class MySketch : public braid::SketchApp {
    braid::SyntypeFont font;
    std::unique_ptr<braid::Syntype> syntype;

public:
    using braid::SketchApp::SketchApp;

    void setup() override {
        font = braid::SyntypeFont::load("addons/braid-syntype/fonts/arame.txt").value();
        syntype = std::make_unique<braid::Syntype>(device());
    }

    void draw() override {
        background(0.02f, 0.02f, 0.03f);
        syntype->draw(surface(), font, "HELLO", {10, 24}, 16.0f, {1,1,1,1});
        syntype->drawCentered(surface(), font, "BRAID",
                              {width()*0.5f, height()*0.5f}, 48.0f,
                              {1, 0.5f, 0, 1});
    }
};
```

## Font Format

The loader reads `.txt` stroke-font files. Each line defines one glyph:

```
A:0,0 2,6 4,0|0.7,2 3.3,2
```

- Lines starting with `#` are comments.
- Empty lines are ignored.
- Format: `GLYPHNAME:stroke0|stroke1|...`
- Each stroke is a space-separated list of `x,y` coordinates.
- An optional `:w,h` scale factor may appear at the end of a line.
- Multiple variants of the same glyph may appear; the first is kept.
- Variant suffixes like `.alt` and `.alt2` are stripped.

Glyph names are mapped to characters:
- Single-character names map directly (e.g. `A` → `'A'`).
- Number names map to digits (`one` → `'1'`, `two` → `'2'`, …).
- Punctuation and symbols have common-name mappings (`period` → `'.'`, `exclam` → `'!'`, …).

## Build

`chalet.yaml` includes the `braid-syntype` static library and three example targets:

```bash
chalet buildrun syntype_basic   # FPS counter + centered title
chalet buildrun syntype_ui      # sliders and labels
chalet buildrun syntype_audio   # FFT-distorted text
```

## API

```cpp
class SyntypeFont {
    static Result<SyntypeFont> load(const std::string& path);
    void insert(char c, Glyph glyph);
    const Glyph* glyph(char c) const;
    bool hasGlyph(char c) const;
    float baseScale = 22.0f;
};

class Syntype {
    explicit Syntype(wgpu::Device device);

    void draw(Surface& target, const SyntypeFont& font, const std::string& text,
              glm::vec2 pos, float size = 16.0f, glm::vec4 color = {1,1,1,1});

    void drawDistorted(Surface& target, const SyntypeFont& font, const std::string& text,
                       glm::vec2 pos, float size, glm::vec4 color,
                       const std::vector<glm::vec2>& distortionOffsets);

    glm::vec2 measure(const SyntypeFont& font, const std::string& text, float size) const;

    void drawCentered(Surface& target, const SyntypeFont& font, const std::string& text,
                      glm::vec2 center, float size = 16.0f, glm::vec4 color = {1,1,1,1});
};
```
