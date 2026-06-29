# Braid v0.3 — Image loading (mango → Surface)

**Thesis:** in Braid the "texture object" *is a `Surface`*. There's no separate image
type to learn — you load a file into a Surface, and it immediately composes with the
whole algebra:

```cpp
auto photo = braid::Surface::load("photo.jpg");   // mango SIMD decode → a Surface
surface() += *photo;                               // composite it in
surface().feedback(0.97f, [&](Surface& s){ s.zoom(1.02f); });  // photo eating its tail
photo->save("copy.png");                           // round-trip out
```

Loading returns `Result<Surface>` (it's fallible — file may be missing/corrupt), which
fits the tier rule: **fallible at the door (`load`), total once inside (the algebra).**

---

## The concrete path (mango)
mango decodes straight into the pixel format you ask for, with its SIMD codecs (the
speed win over libpng/libjpeg). `Bitmap(filename, Format)` is the whole decode:

```cpp
#include <mango/image/image.hpp>

Result<Surface> Surface::load(const char* path) {
    using namespace mango::image;
    // RGBA8, decoded in one SIMD pass (RGB→RGBA happens in-decode).
    Bitmap bmp(path, Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8));
    if (!bmp.image) return Result<Surface>::failure(std::string("load failed: ") + path);

    Surface s(detail::ctx().device, bmp.width, bmp.height, wgpu::TextureFormat::RGBA8Unorm);

    // WriteTexture takes arbitrary bytesPerRow — no 256-alignment dance (that rule is
    // only for CopyBufferToTexture). One copy: CPU bitmap → texture.
    wgpu::TexelCopyTextureInfo dst{};
    dst.texture = s.handle();                  // (add a texture accessor)
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow  = static_cast<uint32_t>(bmp.stride);
    layout.rowsPerImage = static_cast<uint32_t>(bmp.height);
    wgpu::Extent3D ext{(uint32_t)bmp.width, (uint32_t)bmp.height, 1};
    detail::ctx().device.GetQueue().WriteTexture(&dst, bmp.image,
                                                 bmp.stride * bmp.height, &layout, &ext);
    return Result<Surface>::success(std::move(s));
}
```

That's it for the sync load. The image Surface is `RGBA8Unorm` (its natural depth); it
still composites/feeds into the default 16-float Surfaces fine because the Compositor
*samples* the source regardless of format and writes whatever the destination is.

**Why `WriteTexture`, not the readback-style buffer path:** `WriteTexture` accepts any
`bytesPerRow`, so mango's stride uploads directly. `CopyBufferToTexture` would force a
256-aligned restride first. Sync load wants `WriteTexture`; the async path (below) wants
the buffer path because it can fill a mapped buffer off-thread.

---

## North stars for this milestone
1. **The texture object is a Surface.** No new primitive; images compose with `+=`,
   `feedback`, transforms, `save`. (One concept, the elastic-mind rule again.)
2. **Keep mango behind the seam.** No `mango::*` type in any public Braid signature —
   `Surface::load`/`save` are the only doors. (ofWorks `ofImage.cpp` proves the pattern.)
3. **One copy, honestly.** Not "zero-copy" (impossible to discrete VRAM). On Apple's
   unified memory it's effectively a single copy; say "single-copy upload," not magic.

---

## Phases (each ends in something you can see)

### 0.3.0a — sync `Surface::load` (the code above)
- `static Result<Surface> Surface::load(const char* path);` + a `wgpu::Texture handle()` accessor.
- **Proof:** load a PNG/JPG → `surface() += *photo` → it shows; `photo->save("out.png")`.

### 0.3.0b — color policy (sRGB done right)
- sRGB / no-profile images → upload to an `RGBA8UnormSrgb` texture so the GPU sampler
  linearizes for free. Non-sRGB ICC profiles → mango's bundled **lcms2** transform in-decode.
- Expose the embedded profile so we *branch* instead of always/never converting.
- **Proof:** an sRGB photo composited over a 16F scene looks right (no double-gamma).

### 0.3.0c — async `loadAsync`
- I/O thread decodes into a mapped `MapWrite|CopySrc` buffer (256-aligned stride);
  main thread does `CopyBufferToTexture`. Result delivered via a `Channel`/`Future`.
- **Proof:** load a big image mid-sketch with no frame hitch.

### 0.3.0d — `image()` in SketchApp
- `void image(Surface& img, float x, float y, float w = 0, float h = 0);` — a textured
  quad through a small textured shader (or reuse the Compositor blit at a placed rect).
- **Proof:** `image(photo, 0, 0)` then draw shapes on top, Processing-style.

### 0.3.0e — mango for `save()` too
- Replace the TGA writer with mango's `ImageEncoder` (PNG/JPEG) so export is real formats
  and round-trips the load path. Tonemap 16F→8 on the way out.
- **Proof:** `surface().save("frame.png")` produces a proper PNG of a bloomed/fed-back frame.

---

## Implementation notes
- **Texture accessor:** `Surface::load` needs to write into the Surface's texture, so add
  `wgpu::Texture handle() const;` (offscreen Surfaces already own `texture_`).
- **Usage flags already fit:** offscreen Surfaces are created `RenderAttachment |
  TextureBinding | CopySrc`; add `CopyDst` so `WriteTexture` is legal (it needs CopyDst).
- **Mip maps (later):** for minified images add a mip chain (the bloom downsample pyramid
  is the same machinery) — defer until something needs it.
- **Totality holds downstream:** once loaded, the Surface obeys the same total algebra;
  only `load` itself is fallible (returns `Result`).

---

## One-line creed (textures edition)
*An image isn't a different kind of object — it's a Surface that arrived from disk. Load
returns `Result`; everything after is the same total algebra.*
