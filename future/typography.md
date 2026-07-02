# Braid — Basic Typography Design

**Status:** design draft — stashed in `future/` until core is ready  
**Date:** 2026-07-02  
**Scope:** a built-in bitmap-string helper for Braid core. TrueType / SDF typography is explicitly out of scope for this first pass and belongs in a future `braid-text` addon.

> **Note:** The converter, source BDF, license, and generated C++ header are all
> in `future/` so they can be dropped into core once the current core work is
> finished. Do not edit the generated `braid_font_cherry13.h` by hand; rerun
> `bdf_to_braid_font.py` instead.

---

## Goal

Give sketches a one-line way to put text on screen:

```cpp
text(10, 20, "fps: " + std::to_string(currentFps()));
```

No font loading, no asset files, no dependency weight. The first version is a
single baked bitmap font, modeled after openFrameworks' `ofDrawBitmapString`,
but with a clean, intentionally-chosen glyph set instead of OF's murky
freeglut/XFree86 font data.

---

## Principles

1. **Core owns the helper font.** A single 1-bit bitmap font has zero heavy
dependencies, so it belongs in `braid-core`. TTF/SDF/variable-font support
stays in the future `braid-text` addon.
2. **Surface-first.** The primitive draws into the current sketch Surface, just
like `rect()`, `circle()`, and `line()`.
3. **Batch like everything else.** Text quads are appended to the same
SketchApp batch as colored primitives, with a second textured pipeline.
4. **No runtime allocation.** Glyph data is a static C array; the atlas texture
is created once on first use.
5. **Public-domain-equivalent font data.** Avoid the legal ambiguity of OF's
built-in font. Use a font with a clear 0BSD/MIT/public-domain license.

---

## Recommended default font: Cherry 13-r

The default embedded font is **Cherry 13-r** from
[turquoise-hexagon/cherry](https://github.com/turquoise-hexagon/cherry).

Why Cherry:

- **0BSD license** — effectively public domain, no attribution ceremony.
- **7×13 cell** — close to OF's 8×13 footprint, so the API feels familiar.
- **192 glyphs** — covers ASCII + Latin-1 Supplement, enough for debug/HUD text
and simple UI labels.
- **Actually drawn as a bitmap font** — it looks intentional and friendly, not
like an extracted system font.
- **BDF source** — plain text, trivial to convert to a C array once and embed.

A bold variant (`cherry-13-b`) can be added later as a separate atlas, and
users can eventually load their own BDF/PCF files through an addon seam.

**License note (to be included above the embedded glyph table):**

```text
Cherry bitmap font
Copyright (C) 2023 by camille
Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.
THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES ...
```

### Alternative: Dina 10

If a sketch needs a denser, more "terminal/programmer" look, **Dina** is the
runner-up. It is similarly free and readable, but its aesthetic is more
IDE than creative tool. Cherry is the better default for Braid's personality.

---

## API

Add to `SketchApp`:

```cpp
// Draw a string at (x, y) using the current fill color.
void text(float x, float y, std::string_view s);

// Width and height of the string in pixels, using the same metrics as text().
glm::vec2 textSize(std::string_view s) const;
```

Behavior:

- Top-left origin: `(x, y)` is the top-left corner of the first glyph.
- Color comes from the current `fill()`; `noFill()` makes text invisible.
- Stroke does not apply to bitmap text.
- Supports `\n` and `\t` (tab stop every 8 character cells).
- Control characters `< 32` other than `\n` and `\t` are skipped.
- Respects the existing `pushMatrix()` / `popMatrix()` / `scale()` stack.

Later additions (not in v0):

```cpp
void textAlign(TextAlign h);   // Left | Center | Right
void textLeading(float px);    // line spacing
```

---

## Metrics

For Cherry 13-r:

| Metric | Value |
|---|---|
| Cell width | 7 px |
| Cell height | 13 px |
| Line height | 13 px (ascent 11 + descent 2) |
| Atlas | 128×208 `R8Unorm` |
| Tab stop | 8 cells (56 px) |

`textSize()` computes bounds on the CPU without touching the GPU.

---

## Implementation Plan

### 1. Font data

- Cherry 13-r is already converted to `future/braid_font_cherry13.h`.
- The converter lives at `future/bdf_to_braid_font.py` and can regenerate the
header from any fixed-cell BDF:
  ```bash
  python3 future/bdf_to_braid_font.py future/cherry-13-r.bdf future/braid_font_cherry13.h --name Cherry13
  ```
- The header contains a 128×208 `R8Unorm` atlas plus a `Glyph kGlyphs[256]`
table, ready to upload to WebGPU.

### 2. Atlas texture

- Create a single `wgpu::Texture` on first `text()` call.
- Format: `R8Unorm` (one channel, sampled as alpha).
- Sampler: `Nearest` magnification/minification — keeps bitmap text crisp,
especially on Retina where a 7×13 glyph maps to 14×26 physical pixels.
- Layout: 16×16 grid of 7×13 glyphs, with empty padding where a glyph is
missing.

### 3. Batching

- Extend the `DrawCmd` struct in `SketchApp` with a `textured` flag.
- Textured quads use a second fragment shader that samples the font atlas and
multiplies by the per-vertex fill color.
- `emitBatch()` groups consecutive commands that share the same pipeline;
switching between colored and textured geometry flushes the current group.
- The text vertex data reuses the existing `Vertex` layout (`position`,
`texCoord`, `normal`, `color`), with `texCoord` populated and `color` set to
the current fill.

### 4. Shader

Text fragment shader (conceptual):

```wgsl
@group(0) @binding(0) var<uniform> u : Uniforms; // MVP + tint
@group(1) @binding(0) var fontSampler : sampler;
@group(1) @binding(1) var fontTexture : texture_2d<f32>;

@fragment
fn fsMain(v : VertexOut) -> @location(0) vec4<f32> {
    let a = textureSample(fontTexture, fontSampler, v.uv).r;
    return vec4<f32>(v.color.rgb, v.color.a * a);
}
```

Blend: `Blend::Alpha`.

### 5. Lifecycle

- The `BitmapFont` object is lazy-initialized inside `SketchApp::ensureReady()`.
- It lives on the `SketchApp` instance and is destroyed with it.
- No global font state.

---

## Why not OF's font data?

openFrameworks' `ofBitmapFont` implementation is a good reference for the
*API* (`ofDrawBitmapString`, fixed-cell atlas, `\n` / `\t` handling), but its
glyph table carries a vague license note:

> "The legal status of this file is a bit vague. The font glyphs themselves
> come from XFree86 v4.3.0 ... believed to be licensed under the XFree86
> license."

For a built-in helper that is compiled into every Braid binary, we want a font
whose license we can state in one sentence. Cherry's 0BSD is that.

---

## Why not a TTF/SDF font in core?

- FreeType + harfbuzz is a meaningful dependency stack.
- SDF is more code and tuning than a v0 text primitive needs.
- The "Surface is the one primitive" rule says heavy text rendering belongs in
an addon that reaches toward Surface, not inside the core.

The core bitmap font is for debug overlays, frame counters, and simple labels.
Pretty typography comes later via `braid-text`.

---

## Open Questions

1. Do we want a `textFont()` hook so a sketch can switch between Cherry and a
user-provided BDF, or is one default enough for v0?
2. Should `text()` accept `const char*`, `std::string`, `std::string_view`, or
all three? `std::string_view` is the cheapest common denominator.
3. Should we expose the atlas `Surface` directly for effects (e.g. feedback on
text), or keep it internal?

---

## Summary

- Add `SketchApp::text(x, y, s)` and `textSize(s)`.
- Embed **Cherry 13-r** as the default bitmap font (0BSD, 7×13, Latin-1).
- Render through the existing SketchApp batch with a new textured pipeline.
- Keep TTF/SDF out of core; reserve that for a future `braid-text` addon.
