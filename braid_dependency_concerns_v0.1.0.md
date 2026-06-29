# Braid v0.1.0 — Dependency Concerns & Discussion Brief
## "Flexible Joints" — Doubts, Solutions, and Questions for Each Party

**Date:** 2026-06-28
**Status:** Discussion draft — for orchestrating conversation with dependency authors & team
**Companion to:** `braid_deployment_spec_v0.1.0.md`

---

## 0. Framing: Why This Document Exists

Braid bets on three deliberately niche dependencies — **Chalet** (build), **RGFW** (windowing), **mango** (image codec) — alongside three conservative ones — **Dawn/WebGPU** (GPU), **glm** (math), **C++20**.

The niche three are chosen on conviction: they are exceptionally well-designed, maintained by responsive authors, and worth pushing on the world. The architectural bet that makes this safe is that **each is a "flexible joint"** — isolated at the edge of Braid, swappable without a rewrite if it ever stalls.

This document exists to:
1. Make the doubts explicit and on the record, per party.
2. Pair every doubt with a proposed solution or seam.
3. Give each upstream author a clear, scoped set of questions.

**Core principle:** Affection for these tools is a reason to *adopt and evangelize* them, not a reason to *couple* to them. Keep the love at the edges; keep the seams clean.

---

## 1. Joint Isolation Map

How much API surface each dependency touches, and therefore how hard the joint is to keep loose.

| Dependency | Source coupling | Leak risk | Joint difficulty | Swap fallback |
|---|---|---|---|---|
| **Chalet** | None (build meta-tool) | None | Trivial | CMake (Chalet emits it) |
| **mango** | Behind `Texture::load()` only | Low | Easy | stb_image |
| **RGFW** | Window + surface + input + keycodes | **High** | **Hard** | GLFW / SDL3 |
| **Dawn** | Pervasive (it IS the GPU API) | N/A (the bet) | N/A | wgpu-native (C API) |
| **glm** | Pervasive (public vector types) | N/A (the bet) | N/A | — |

**Takeaway:** The build system I worried about most is the safest joint; the windowing I worried about is the one that needs a real abstraction seam. Discipline budget goes to RGFW.

---

## 2. Chalet (Build System)

**Author:** Andrew King (@rewrking)
**Repo:** https://github.com/chalet-org/chalet
**Role in Braid:** Build orchestration, dependency fetching, IDE project generation.

### Why we chose it
- Bootstraps cleanly on all platforms; generates Xcode, Visual Studio, JetBrains, and CMake projects "for free."
- Genuinely well-designed; nothing else occupies this niche.
- Zero source coupling — it never appears in a line of `braid.h`.
- Want to push it on the world.

### Doubts
- **D1.** Does Chalet emit `compile_commands.json`? Braid's `.zed` clangd setup (and any clangd/LSP IDE) is dead without it.
- **D2.** Dawn is a GN/CMake-based build, not a tidy library. Can Chalet's git-dependency model (`dependencies: dawn: { git, commit }`) actually fetch + build Dawn, or does Dawn need to be prebuilt and linked as an external artifact?
- **D3.** mango and RGFW as git-tag dependencies — do those tags exist and build cleanly under Chalet's model, or do they need build shims?
- **D4.** Cross-compilation / platform matrix: is the "works great on all platforms" claim validated specifically for a WebGPU + native-surface project, or for simpler targets?

### Solutions / Seams
- **S1.** Chalet is a build meta-tool with zero code coupling → worst case, generate CMake and continue. This is the lowest-lock-in dependency in the stack. Treat it as the *reference* joint.
- **S2.** For D2: assume Dawn is **prebuilt and linked** (system or vendored binary), not source-built through Chalet, until proven otherwise. Document this assumption in `chalet.yaml`.
- **S3.** For D1: verify `compile_commands.json` emission as a five-minute pre-flight check before any core code is written.

### Questions for the Chalet author
1. Does Chalet emit `compile_commands.json` for clangd-based editors? If so, which command/flag?
2. What is the recommended pattern for a heavyweight GN-based dependency like Dawn — fetch-and-build, or link a prebuilt artifact? Any reference projects?
3. Is there a reference project building a WebGPU/native-surface app across Linux/macOS/Windows with Chalet?
4. Stability of the `dependencies:` git-fetch format across Chalet versions — any planned breaking changes we should pin against?

---

## 3. RGFW (Windowing) — **Primary Concern**

**Author:** Riley (ColleagueRiley)
**Repo:** https://github.com/ColleagueRiley/RGFW
**Role in Braid:** Window creation, input/event pump, native surface for WebGPU.

### Why we chose it
- Single-header (~244KB), minimal, no bloat — matches Braid's aesthetic.
- GLFW feels stalled at times; RGFW has momentum and passion behind it.
- Trust in the maintainer and the morale of the project.

### Doubts (this is where the risk concentrates)
- **D1. Surface creation for WebGPU/Dawn.** The hardest, most platform-specific code in Braid lives at the RGFW↔Dawn boundary. RGFW gives a native window handle; Braid must turn it into a `wgpu::Surface` on:
  - X11 (Linux primary) — expected OK
  - **Wayland** — RGFW Wayland is experimental (flagged in spec §10.2)
  - **macOS Cocoa → Metal `CAMetalLayer`** — untested with Dawn (flagged in spec §10.1)
  - Win32 — expected OK
- **D2. Keycode leak.** Spec originally documented `KeyEvent.key` as "RGFW keycode." If a raw RGFW keycode is the value sketches compare against (`if (e.key == ...)`), RGFW has stopped being a joint and welded itself into the event system. Swapping windowing later breaks every sketch.
- **D3. Event model fit.** RGFW's event pump must map cleanly onto Braid's pull-first `Channel<T>` model (push events into channels each frame). Need to confirm RGFW exposes a poll-based event API, not a callback-only one.
- **D4. HiDPI / display scaling.** Mouse coordinates, framebuffer size vs. window size — does RGFW report these consistently across platforms? Affects `mousePos()` and surface sizing.
- **D5. Window resize → surface reconfigure.** On resize, the swapchain/surface must be reconfigured. Does RGFW deliver resize events reliably and early enough on all platforms?
- **D6. Bus factor.** Single maintainer, younger than GLFW. Acceptable *only if* the joint is real.

### Solutions / Seams (make the joint genuinely loose)
- **S1. `braid::Key` enum.** Map RGFW keycodes → a Braid-owned `Key` enum at the event pump. Sketches never see an RGFW number. This alone makes windowing swappable.
- **S2. Internal `platform::` namespace.** The *only* place RGFW types are allowed to exist. Wraps window + surface + input pump. Everything above speaks Braid types. This also isolates the macOS Metal-surface glue — the hardest platform code — behind one seam.
- **S3. Born-jointed bootstrap.** The first deliverable (window + surface + device + cleared frame) lands *with the `platform::` seam already in place*, even if it's a one-file forwarder to RGFW today. Avoids retrofitting an abstraction after 2,000 lines reach past it.
- **S4. macOS-first spike.** Prove the RGFW→Dawn Metal surface on macOS *specifically* before building primitives — that's where the stack is most likely to first say no.
- **S5. Fallback contract.** Because of S1–S2, the GLFW/SDL3 fallback is a contained `platform::` rewrite, not a core rewrite.

### Questions for the RGFW author
1. **WebGPU/Dawn surface:** Is there a known-good path to get a `wgpu::Surface` from an RGFW window on macOS (Cocoa/Metal `CAMetalLayer`)? Any existing example or user doing this?
2. **Wayland maturity:** Current state of the Wayland backend — production-usable for a 60fps interactive app, or X11-only recommended for now?
3. **Event polling:** Does RGFW expose a non-blocking poll-all-events API suitable for draining into our own event queue each frame?
4. **HiDPI:** How does RGFW report framebuffer size vs. logical window size, and mouse coords, under display scaling on each platform?
5. **Resize timing:** Are resize events delivered synchronously/early enough to reconfigure a swapchain before the next present?
6. **Native handles:** What's the canonical way to extract the raw native handle (NSWindow/Wayland surface/Xlib window/HWND) per platform for surface creation?
7. **Roadmap:** Where are macOS+Metal and Wayland on your priority list? Would a creative-coding/WebGPU use case help drive them?

---

## 4. mango (Image Codec)

**Author:** t0rakka (Finland)
**Repo:** https://github.com/t0rakka/mango
**Role in Braid:** SIMD-accelerated image decode → direct GPU upload.

### Why we chose it
- SIMD decode, claimed ~18× faster than libpng.
- Exceptionally well-made; author is responsive at any hour.
- Worth putting in front of the world.

**Decision:** mango is **first**, from v0.1.0 — not a later optimization. It's adopted on conviction and to put it in front of the world. The doubts below are recorded to keep the joint honest, not to defer the dependency.

### Cross-check against ofWorks (real, shipping mango integration)

ofWorks already replaced FreeImage with mango in `libs/openFrameworks/graphics/ofImage.cpp`. The old FreeImage code is commented out (lines ~18–180); mango is live (`#include <mango/image/image.hpp>`, `using namespace mango;`). This gives us *evidence*, not speculation, for several doubts:

**Already solved / proven in ofWorks ✅**
- **The seam is real.** Every `mango::*` type lives only in `ofImage.cpp`. The public header `ofImage.h` is pure OF API — `ofPixels`, `ofImageLoadSettings`, zero mango types. Our §4-S1 rule ("no mango type in a public signature") is their shipping practice. Copy this pattern into `Texture::load`.
- **Decode is a one-liner.** `mango::image::Bitmap bitmap(path);` (ofImage.cpp:265) replaced ~150 lines of FreeImage gymnastics. Conviction validated.
- **Exception→value wrapping pattern exists.** mango *throws*; ofWorks wraps load in `try/catch(const std::exception&)` → returns `bool` (ofImage.cpp:259, 390–395). This is exactly the boundary Braid needs to honor "error returns, not exceptions" (spec doubt X2): `Texture::load` → `LoadResult` wraps mango's throws at the seam.

**NOT solved even in ofWorks ❌ (our doubts stand)**
- **Direct-to-GPU is absent.** ofWorks decodes mango → CPU `ofPixels` via `std::memcpy` (ofImage.cpp:313, 376); the GPU upload is a *separate* later step. There is no direct path. The Braid spec's "direct upload to GPU, no intermediate CPU buffer" (§3.4) is aspirational — unproven in their own working code. See D1 below for whether it's even achievable.
- **sRGB→linear during decode is absent.** No color-management in the load path; raw bytes copied as-is. But: **mango bundles liblcms2** — the library is far more capable than ofWorks exercises. The capability exists; it's just not wired up. See D2.
- **Bit-depth/channel handling is manual and rough.** Lines 272–388 hand-derive channel count from `format.bits`, do 16↔8-bit conversion by hand, and have a "may look wrong" unsupported-conversion fallback (line 362) plus `// FIXME: unused param settings` (line 245). `Texture::load` inherits this work; it's not "mango decodes, done."

### Doubts
- **D1. Is a true direct-to-GPU path even possible?** ofWorks proves it isn't *done*; the question is whether WebGPU even allows it. **Answer: yes, at one specific level — "single-copy," not "zero-copy."** Four meanings, and WebGPU permits only some:

  | Level | Meaning | WebGPU? |
  |---|---|---|
  | decode → CPU pixels → copy → staging → upload (ofWorks today) | several CPU copies | yes (slow status quo) |
  | **decode straight into GPU staging memory, one DMA to VRAM** | no redundant CPU copy | **yes — the path to build** |
  | no copy to VRAM at all | pixels never move | only on unified memory (Apple Silicon); never discrete |
  | GPU-side decode (JPEG/PNG on GPU) | CPU never decodes | no — core WebGPU has no image decoders |

  mango decodes on the **CPU**, and on a discrete GPU the bytes must physically cross to VRAM — so "zero-copy to GPU" is impossible by definition and that wording must die. But the redundant *CPU* copies ofWorks does (mango → `ofPixels` → texture-internal staging) are fully removable. That's the real win.

- **D2. Color management — lcms2 is the right tool for the wrong-by-default case.** For plain sRGB images you should **not** run lcms2 at all: upload raw bytes to an `-srgb` texture format (`rgba8unorm-srgb`) and let the GPU sampler linearize in hardware, free, on every fetch. lcms2 earns its place only for **non-sRGB embedded ICC profiles** (Display P3, Adobe RGB, camera profiles, CMYK) where there is no GPU shortcut. Policy in S3 below.
- **D3. Build weight / packaging / licensing.** mango + bundled lcms2 is a less-common dependency; confirm minimal-codec build config and license compatibility for vendoring into a framework shipped to others.
- **D4. Joint hygiene.** Because mango is adopted early and deeply, seam discipline matters *more*, not less — early adoption is when a dependency tends to leak into public signatures. (ofWorks shows it can be held — see cross-check.)

### Solutions / Seams
- **S1. Keep the seam exactly as ofWorks does.** mango lives entirely behind `Texture::load()` returning Braid types. **No mango type in a public signature.** stb_image is the contained fallback behind the same seam — not the starting point.

- **S2. The single-copy direct path (the one to build).** WebGPU won't map a *texture*, but it will map a *buffer*. So:
  1. **Main thread:** create staging `Buffer` (`MapWrite | CopySrc`), size `height × align256(width×4)`.
  2. **Main thread:** `mapAsync(MapWrite)`, poll until ready, `getMappedRange()` → raw writable pointer.
  3. **Any thread (incl. I/O):** wrap that pointer in a `mango::Surface` with **stride = `align256(width×4)`**, format `RGBA8`, and decode *directly into it* — mango also does the RGB→RGBA expansion in the same pass, folding away the manual juggling at ofImage.cpp:313–388. lcms2 transform applied here if a non-sRGB profile is present.
  4. **Main thread:** `unmap()`, then `encoder.copyBufferToTexture(...)`.

  Result: **one decode-write, one DMA** — as close to "direct" as portable WebGPU allows. On Apple Silicon unified memory the staging buffer may *be* the final memory (near-zero). On discrete GPUs the one DMA is unavoidable (physics).

  Two gotchas:
  - **256-byte row alignment.** `copyBufferToTexture` requires `bytesPerRow % 256 == 0` — hence the padded stride handed to mango.
  - **No RGB8 texture in WebGPU.** 24-bit RGB is not a WebGPU format; must expand to RGBA8 (mango does this during decode).

  **Threading resolves cleanly:** map/unmap/submit happen on the device-owning (main) thread, but the raw memory write (mango decode into the mapped pointer) can run on the I/O thread. Main maps → hands pointer to worker → worker decodes (+lcms2) → worker signals → main unmaps + copies. This is the `loadAsync` path.

- **S2b. Two-tier load.** Synchronous `Texture::load()` uses `Queue::writeTexture` (no 256 alignment requirement — Dawn repacks internally), decoding into a packed buffer in one line. The mapped-buffer Path (S2) is the async/optimized route for when it matters. Simple default, fast path on demand.

- **S3. Color-management policy.**
  - **No profile, or sRGB** → upload as-is to an `-srgb` format; GPU sampler linearizes. lcms2 *not* invoked.
  - **Non-sRGB ICC profile present** → lcms2 transform during decode-into-staging → upload to linear/`-srgb` target per working space.
  This is strictly more capable than ofWorks *and* skips a pointless CPU pass on the ~95% sRGB case.

- **S4. Drop "zero-copy" everywhere.** Respec §3.4 as "single-copy: decode directly into mapped GPU staging, one `copyBufferToTexture`." Honest and still the fast path.

- **S5.** Keep mango to the decode/CMS path; do mipmap/resize as explicit GPU passes (spec doubt D for §4 original — mipmap gen is a GPU job, not a codec job) unless mango demonstrably wins. Minimal-codec build config to keep the joint light.

### Questions for the mango author (sharpened by the above)
1. **Decode into caller memory at arbitrary stride:** Can mango's decoder write **directly into a caller-supplied buffer at a 256-aligned stride**, with **RGB→RGBA expansion**, in a single pass? (This is what makes the single-copy GPU path work.)
2. **ICC profile access:** Does mango expose the **embedded ICC profile** so we can branch — "sRGB/none → GPU `-srgb` format" vs. "non-sRGB → run the bundled lcms2 transform" — rather than always/never converting? What's the idiomatic lcms2 entry point within mango for that transform?
3. **Threading:** Is decode safe to run on an arbitrary I/O thread, writing into a pre-mapped pointer the main thread owns, with map/unmap/submit kept on main?
4. **Footprint:** Minimal build config for just the codecs we ship (PNG/JPG/…) — can unused codecs and lcms2 be compiled out when not needed, to keep the joint light?
5. **Licensing:** Confirm mango + lcms2 license compatibility for vendoring into a framework distributed to others.

---

## 5. Dawn / WebGPU (the GPU bet — not a joint)

**Maintainer:** Google (Dawn) / W3C (WebGPU spec)
**Role:** The GPU API itself. Pervasive by design — this is the strategic bet, not a swappable joint.

### Doubts
- **D1. Build & size.** Dawn dominates compile time and binary size. The spec's "~3s compile, ~1MB binary" targets are only true for *Braid's own code against a prebuilt Dawn* — Dawn itself is a GN-based monster.
- **D2. Surface integration.** Tightly coupled to RGFW concern §3-D1 — getting a surface from a native window per platform.
- **D3. Validation default.** `Settings.enableValidation = true` by default contradicts "ignored in release" (spec §8). Validation in release hurts perf.

### Solutions
- **S1.** Treat Dawn as a **prebuilt/linked artifact**, not a from-source dependency in the hot build path. State the compile/size targets as "Braid code vs. prebuilt Dawn" explicitly.
- **S2.** Default `enableValidation` to debug-only.
- **S3.** wgpu-native (Rust impl, C API) is a theoretical alternative, but it would be a major migration, not a joint swap. Not in scope; noted for completeness.

### Questions for the team (internal)
1. Vendor a prebuilt Dawn per platform, or require a system Dawn? Version pinning strategy?
2. Confirm the surface-from-native-handle path for each platform against the current Dawn API.

---

## 6. High-Bit-Depth & Virtual Production Output

Braid targets two delivery regimes with opposite precision needs. The framework must serve both, and the difference is an **output-architecture** decision, not a windowing one.

### Two regimes
- **Festival / installation (direct-to-eye).** The display chain caps at 8-bit (sometimes 10-bit). The eye forgives quantization. fp16 internal render + dither-down to 8-bit on final present is plenty — gradients look "deeper than 8-bit" because the banding source (8-bit math) is gone and the final quantization is dithered.
- **Virtual production (LED wall → camera → post).** Output precision genuinely matters. Footage gets **exposed, white-balanced, and graded** downstream, and grading *stretches* the signal — banding invisible to the eye becomes visible the moment a colorist lifts shadows. Keep the bits all the way **out**; do not dither down to 8-bit.

### Render policy (serves both)
- **Default internal render target = `RGBA16Float` Screen.** All compositing/blending happens in 16-bit float, linear space, regardless of what the output can show. Braid's "everything is a Screen/texture" model makes this the natural default — banding never enters the pipeline.
- Only the final stage quantizes, and only to what the *output* supports (8 / 10 / 12 / 16-bit).

### Output paths
1. **Window swapchain (present).** Request `RGB10A2Unorm` (10-bit) or `RGBA16Float` (fp16/HDR) surface formats, gated by `surface.GetCapabilities()`. This is **Dawn's** job, not the windowing lib's — RGFW vs GLFW is irrelevant to output bit depth (both are passthrough window providers; neither configures the swapchain). *Reinforces §3: do not re-litigate windowing choice over bit depth.*
2. **Direct SDI playout via AJA / Blackmagic — the pro VP path.** Render into a `Screen`, then hand the **texture directly to the card SDK** (AJA NTV2; Blackmagic DeckLink). 10/12-bit out. This path **bypasses the window, swapchain, and `present()` entirely** — you never present to a screen. This is exactly where Braid's `Screen`-as-routable-texture model shines: a Screen can route to a window *or* a card.

### Architectural note for Braid
Braid needs a **playout target abstraction distinct from swapchain present** — `Screen → window` OR `Screen → AJA/DeckLink card`. A present-to-window-only framework is unusable for serious VP; a routable-`Screen` framework is ideal. Worth a spec section: `Playout` (or `Output`) as a sibling to the window-surface Screen.

### The AJA/* SDI-out texture path (detail)
- **Simple / portable path:** `copyTextureToBuffer` → `mapAsync` → hand the mapped 10/12-bit bytes to the AJA NTV2 / DeckLink SDK frame queue. Works on every platform; costs ~one frame of latency. Good default.
- **Low-latency path:** GPU-direct DMA from GPU memory straight to the card (NVIDIA **GPUDirect for Video**, which AJA Kona/Corvid and DeckLink support). Avoids the CPU round-trip. **But:** WebGPU/Dawn's portability abstraction does not hand you the native GPU resource handle easily — GPUDirect needs the underlying D3D12 shared handle / Vulkan external-memory / Metal resource, reached via Dawn's native-interop surface. This is the same shape of problem as the mango single-copy path (§4): the portable API hides the handle you need for true zero-copy. **Open question whether the latency win justifies the Dawn-interop complexity for v0.x.**
- **Pixel format:** SDI typically wants 10-bit YUV (`v210`) or 10/12-bit RGB. Pack/convert from the `RGBA16Float` Screen in a final shader pass or on readback — keep the conversion in the float→output stage, never in 8-bit.

### Color management & sync (outside the framework, but must be respected)
- **Output transform.** fp16 linear → the wall's calibrated working space (ACES / Rec.2020 / P3 depending on the volume). The LED processor (Brompton / Megapixel-class) runs a high-bit-depth pipeline and does the final PWM/dither — **feed it a clean 10/12-bit signal and stay out of its way.**
- **Temporal dithering caveat (VP-specific).** Unlike festivals, app-side temporal dither can be caught by the camera shutter as flicker. Let the *processor* handle dithering from a clean high-bit feed; don't dither yourself unless stuck at 8-bit out.
- **The three "bandings" are different problems:** (1) tonal/bit-depth banding → fp16 + 10/12-bit out; (2) horizontal roll-bar / scan-line banding → wall refresh vs camera shutter (processor + sync, not content); (3) moiré → panel pitch vs sensor (lens/distance). Only (1) is Braid's to solve.
- **Genlock/sync and wall calibration** live outside any creative-coding framework (pro GPU sync boards / the processor). Note as a hard limit; do not promise it.

### Doubts
- **D1.** SDI playout via AJA/DeckLink: simple readback path (one-frame latency) vs GPUDirect low-latency path (needs Dawn native-resource interop). Which for v0.x?
- **D2.** `RGBA16Float` as the *default* Screen format doubles VRAM per target vs RGBA8 — confirm acceptable for the install/VP targets (it is, for these use cases) and that the spec sets it as default with an 8-bit opt-out for memory-bound sketches.
- **D3.** Does Dawn expose enough native-interop on each platform to reach a card's GPUDirect path, or is CPU readback the only portable option?

### Questions for the team (internal)
1. Add a `Playout`/`Output` abstraction (Screen → AJA/DeckLink) to the roadmap — which version? (Likely post-v0.2, but design the `Screen` texture-access API now so it isn't precluded.)
2. Default internal Screen format = `RGBA16Float`? Confirm and document the 8-bit opt-out.
3. Evaluate Dawn native-interop (D3D12 shared handle / Vulkan external memory / Metal) for GPUDirect-to-card feasibility — or commit to `copyTextureToBuffer` readback for v0.x.

---

## 7. Cross-Cutting Spec Issues Surfaced During Review

Not dependency-specific, but blockers an implementer can't paper over. Tracked here so they're in the same discussion.

- **X1. Inline enum won't compile** — `std::optional<enum Button {...}>` (spec §4, line ~333). Declare `enum class Button` separately.
- **X2. Unified error type** — three conventions coexist (`Error` hierarchy, `LoadResult` struct, void-returning `load()` that prose says "returns Error"). Pick one `Result<T>` and thread it everywhere.
- **X3. Channel cross-thread contract** — the v0.1.0 thread-safety table was dropped while I/O-thread → main-thread channel pushes remain. Restate which channels are cross-thread safe.
- **X4. subscribe() callback timing** — fire on `pop()` (main thread), not on `push()` (possibly I/O thread). Off-main GPU calls = UB.
- **X5. `Mesh::line(span<glm::vec2>)`** missing `wgpu::Device` param.
- **X6. clone() signatures** disagree with the ownership table (Texture clone takes an encoder).
- **X7. Sketch tier internals** undefined (default shader, transform stack, state struct, batching/flush).

---

## 8. Suggested Discussion Orchestration

| Thread | Parties | Goal |
|---|---|---|
| **A. RGFW surface bootstrap** | RGFW author + Braid team | Resolve §3 D1–D6; greenlight macOS/Wayland path or confirm X11-first |
| **B. Chalet + Dawn build** | Chalet author + Braid team | Resolve §2 D1–D4; lock prebuilt-Dawn assumption + `compile_commands.json` |
| **C. mango decode path** | mango author + Braid team | Confirm single-copy decode-into-mapped-staging + ICC/lcms2 policy (§4 S2–S3); mango-first, v0.1.0 |
| **D. Spec corrections** | Braid team (internal) | Resolve §7 X1–X7 before core code generation |
| **E. Output architecture** | Braid team (internal) | `RGBA16Float` default Screen + `Playout` (AJA/DeckLink SDI) abstraction per §6; decide readback vs GPUDirect |

**Sequencing:** D and the `platform::` seam (§3 S2–S3) come first — they're prerequisites for a born-jointed bootstrap. A is the highest-risk external thread and should start in parallel. B unblocks the build. C runs alongside (mango is first, in v0.1.0).

---

## 9. One-Line Position per Dependency

- **Chalet** — Safest joint. Verify `compile_commands.json`, assume prebuilt Dawn, otherwise full confidence.
- **RGFW** — Riskiest joint, on the hardest seam. Make it real with `braid::Key` + `platform::`. Spike macOS first.
- **mango** — Adopted first, on conviction; seam proven in ofWorks. Build the single-copy decode-into-mapped-staging path (true zero-copy is impossible; single-copy is achievable). lcms2 only for non-sRGB ICC profiles; plain sRGB rides a GPU `-srgb` format. Never let a mango type into a public signature.
- **Dawn** — The bet, not a joint. Prebuilt + linked. Default validation to debug.
- **glm** — Uncontroversial. No concerns.
- **Output (not a dependency, an architecture)** — `RGBA16Float` Screen by default; route a Screen to a window swapchain *or* straight to an AJA/DeckLink SDI card. Festivals dither down to 8-bit; VP keeps 10/12-bit all the way out. Windowing lib is irrelevant to bit depth.

---

*"Keep the love at the edges and the seams clean, and the flexible-joints thesis holds."*
