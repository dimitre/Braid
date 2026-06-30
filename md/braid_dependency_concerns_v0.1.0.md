# Braid — Dependency Concerns (live issues)

*Compressed from the original 2026-06-28 discussion brief. Resolved items and completed
discussion threads removed. What remains is either unresolved, still structurally important,
or forward-looking.*

---

## Joint isolation map

How much each dependency leaks into the public API and how hard the joint is to keep loose.

| Dependency | Source coupling | Leak risk | Joint difficulty | Swap fallback |
|---|---|---|---|---|
| **Chalet** | None (build meta-tool) | None | Trivial | CMake (Chalet emits it) |
| **mango** | Behind `Surface::load/save` only | Low | Easy | stb_image |
| **RGFW** | Window + surface + input + keycodes | **High** | **Hard** | GLFW / SDL3 |
| **Dawn** | Pervasive (it IS the GPU API) | N/A (the bet) | N/A | wgpu-native (C API) |
| **glm** | Pervasive (public vector types) | N/A (the bet) | N/A | — |

**Takeaway:** the windowing library needs the most seam discipline. Budget goes to RGFW.

---

## RGFW — joint seam discipline (still in progress)

`braid::Key` enum is done (mapped from RGFW at the pump; sketches never see raw RGFW codes).
The remaining joint work: a proper `platform::` internal namespace so RGFW types are confined.

**Live principles:**
- `braid::Key` / `braid::MouseButton` → done ✅. No RGFW type in any public signature.
- `platform::` namespace — the *only* place RGFW types are allowed to exist. Not yet done.
  Wraps window + surface + input pump; everything above speaks Braid types.
- The macOS Metal-surface glue (`[VERIFY]` in `braid.cpp:909`) is the hardest platform code;
  it lives at the RGFW↔Dawn boundary and must be proven across resize + HiDPI before it
  can be called stable.
- **Why this matters:** RGFW bus factor is one maintainer. A real `platform::` seam makes the
  GLFW/SDL3 fallback a contained namespace rewrite, not a core rewrite.

**Open:** macOS HiDPI — `RGFW` reports framebuffer vs window size; needs verification that
`mainSurface_` dimensions match the Metal drawable size and that `mousePos()` uses the same
coordinate space. See also unmapped_todos #5.

---

## mango — color management policy (unresolved)

mango is in use (`braid-image` addon, `Surface::load/save` working). The color policy for
image loading is not yet implemented:

- **sRGB or no-profile** → upload to `RGBA8UnormSrgb` format so the GPU sampler linearizes
  in hardware, free. lcms2 NOT invoked.
- **Non-sRGB ICC profile present** → mango's bundled **lcms2** transform during decode →
  upload as linear. Expose the embedded profile to branch, not always/never convert.
- **No mango type in any public signature** — the seam is proven in ofWorks' `ofImage.cpp`;
  copy that pattern exactly.

Async load (`Surface::loadAsync`) is also not yet implemented (current `load` is sync +
`while (!done) ProcessEvents()` spin on readback).

---

## Dawn — validation default

`Settings::enableValidation` exists but is not consumed by `App::initWebGPU()`. Wire it to
Dawn's device toggles: on in debug (`#ifndef NDEBUG`), off in release. As-is, the promise
in the settings struct is misleading.

Dawn is a prebuilt static artifact (`libwebgpu_dawn.a`). Treat it as such — it is not a
swappable joint, it is the strategic bet. The only realistic swap is wgpu-native (same API
shape). No wrapper layer over WebGPU; that would be an abstraction over an abstraction.

---

## High-bit-depth & Virtual Production output (future)

Internal Surface is already `RGBA16Float` ✅ — smooth feedback, HDR headroom. The unbuilt
delivery layer:

**Two regimes:**
- **Festival / installation (direct-to-eye):** 8–10-bit display chain. fp16 internal +
  dither to 8-bit on present is sufficient. Banding from 8-bit math is gone; quantization
  is dithered at the final stage.
- **Virtual production (LED wall → camera → post):** footage gets graded downstream;
  banding invisible to the eye becomes visible when a colorist lifts shadows. Keep the bits
  all the way **out** — do not dither to 8-bit.

**Output paths not yet built:**
1. Request `RGB10A2Unorm` or `RGBA16Float` swapchain via `surface.GetCapabilities()` — this
   is Dawn's call, not RGFW's (windowing lib is irrelevant to bit depth; both are passthrough).
2. **`Playout` target** distinct from window present: `Surface → AJA NTV2 / Blackmagic DeckLink`.
   - Simple path: `copyTextureToBuffer → mapAsync → card SDK` (one frame latency, always works).
   - Fast path: GPUDirect DMA via Dawn native-interop (D3D12 shared handle / Metal resource).
     Open question whether the latency win justifies the Dawn-interop complexity for v0.x.
3. Let the LED processor (Brompton/Megapixel) handle final PWM/dither from a clean 10/12-bit
   signal. Do not dither yourself unless stuck at 8-bit.

**Note on "three bandings":** only tonal/bit-depth banding (1) is Braid's to solve. Roll-bar
banding (2, wall refresh vs shutter sync) and moiré (3, panel pitch vs sensor) are hardware
and not in scope for the framework.

---

## One-line position per dependency

- **Chalet** — safest joint; zero code coupling; worst case, emit CMake and move on.
- **RGFW** — riskiest joint on the hardest seam; `braid::Key` done; `platform::` namespace next.
- **mango** — seam proven in ofWorks; single-copy upload done; color policy (sRGB/-srgb,
  lcms2 for non-sRGB ICC) is the remaining work. Never let a mango type into a public signature.
- **Dawn** — the bet, not a joint; prebuilt + linked; wire `enableValidation` to debug toggle.
- **glm** — uncontroversial; no concerns.
- **Output** — `RGBA16Float` internal ✅; `Playout` (AJA/DeckLink SDI) and 10/12-bit swapchain
  are the unbuilt pieces; festivals dither to 8-bit, VP keeps the bits all the way out.

*"Keep the love at the edges and the seams clean."*
