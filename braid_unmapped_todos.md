# Braid — Unmapped Todos

*A companion to `braid_roadmap.md`. These are gaps, near-term fixes, and strategic moves that are not yet written down but would be on my go-to list.*

---

## Immediate / next sprint

These are real blockers or oversights that should land before the next feature wave.

### 1. Wire up `Settings::enableValidation`

`App::Settings` has `enableValidation` (debug on, release off), but `App::initWebGPU()` does not appear to consume it. Either:

- pass it through to Dawn's device/request-adapter toggles, or
- remove the flag if Dawn validation is now controlled elsewhere.

Without this, the "debug vs release behavior" promise in the settings is misleading.

### 2. Add depth attachments to `Surface`

The roadmap wants solid `box()` / `sphere()` fill in v0.2.3, but `Surface` currently only carries a color texture. Before filled 3D shapes work correctly, `Surface` needs an optional depth/stencil texture with matching size/lifetime, and `Surface::begin()` needs to attach it.

Design choice to make: is depth part of `Surface` (opinionated, simple) or passed as an optional `DepthSurface` companion (flexible)? Given the "one primitive" north star, depth should probably be internal and enabled on demand.

### 3. Write the SketchApp batching design

The roadmap says "batching (per-primitive vertex+uniform churn, review #4)" but has no spec. This needs a design doc before code. Options:

- **Dynamic vertex buffer per frame:** one large `Vertex` buffer, append primitive geometry, one draw call per material change.
- **Uniform ring for many draws:** replace `freshUniform()` per primitive with a ring-allocated or dynamic uniform buffer that can hold N uniforms, bound at offsets.
- **Draw list sorting:** group by pipeline/blend to minimize state changes.

Given the microframework spirit, a single-frame dynamic buffer + uniform ring is probably the right first step.

### 4. Make `Surface::save()` asynchronous

Current `save()` maps the buffer synchronously with a `while (!done) ProcessEvents()` spin. This hitches the GPU and CPU. For a feedback art tool, saving frames should not stall the tunnel.

Add `Surface::saveAsync(path, callback)` or a `Channel`/`Promise`-style return so the readback completes on the next frame without blocking.

### 5. HiDPI / content-scale reality check

`settings_.width/height` are window client sizes in logical pixels, but the Metal drawable may be larger on Retina. The `configureSurface()` path uses `settings_.width/height` directly. Verify that:

- `RGFW` reports the right framebuffer size vs window size.
- The `mainSurface_` and `swapSurface_` match the drawable size.
- Mouse coordinates from `RGFW_mouseMotion` are in the same coordinate space as drawing.

This is unmentioned but will matter the first time the app runs on a Retina display.

---

## Short-term (after v0.2.3)

### 6. Test harness for non-GPU code

A lot of Braid can be unit-tested without a GPU:

- `Result<T>` construction, success/failure, move-only types.
- `Channel<T>` push/pop/subscribe ordering and thread safety.
- `Timer` delta/elapsed accounting.
- `Mesh` primitive generators (vertex/index counts, layout offsets).
- `Shader::PipelineKey` hash collisions.

Add a tiny test runner (even just a `tests/` executable that asserts and returns nonzero). This pays off quickly once refactoring begins.

### 7. Lint rule: always qualify `wgpu::`

The `Surface` vs `wgpu::Surface` collision is acknowledged in the roadmap but not enforced. Add a `clang-tidy` check or a simple script that fails CI if `wgpu::` is omitted in `braid.h` / `braid.cpp`. Without enforcement, the mitigation will drift.

### 8. Input action map

Mapping `RGFW_key*` to a `braid::Key` enum is on the roadmap, but creative coding usually wants higher-level input:

```cpp
app.input.bind("save", {Key::S, Mod::Super});
app.input.bind("clear", Key::Space);
app.input.pressed("save")  // bool, per-frame
```

This is more useful than raw keycodes and keeps RGFW behind a swappable seam.

### 9. Built-in frame-time overlay

Creative coders need to see frame time, FPS, draw call count, and GPU memory at a glance. A minimal built-in overlay (or an ImGui example) should exist before calling the framework "usable for iteration."

### 10. Asset hot-reload beyond shaders

The roadmap mentions shader hot-reload. Generalize this into a small `AssetWatcher` that can reload:

- WGSL files → recompile `Shader`.
- image files → re-decode into `Surface`.
- mesh files → rebuild `Mesh`.

This is the difference between a framework and a pleasant tool.

---

## Medium-term (v0.3.x and beyond)

### 11. Texture atlas / GPU memory budget

Once `image()` and text (SDF) land, you will want atlasing and a way to reason about VRAM. This is not on the roadmap at all but becomes critical when sketches load dozens of images.

### 12. Syphon / Spout / NDI output

For installation and VJ use, being able to output a `Surface` to Syphon (macOS), Spout (Windows), or NDI is a major differentiator. The "Surface is the one primitive" thesis makes this a natural extension: `surface.publish(" braid-out")`.

### 13. OSC / networking

Installations often speak OSC. A minimal `OscIn`/`OscOut` built on `Channel<T>` would fit the existing event model cleanly.

### 14. Compute pass design

GPU particles are mentioned, but the compute integration needs a design doc before code:

- Where do compute shaders live? `Shader` reused, or a separate `ComputeShader`?
- How is a `StorageBuffer` / `StorageTexture` represented?
- How does a compute dispatch fit into the frame encoder flow?

### 15. Web/Emscripten spike

The roadmap puts this under "Later / if wanted." Given that the substrate is literally WebGPU, a one-weekend Emscripten spike would answer whether Braid sketches can run in a browser with minimal changes. If yes, it changes the audience question significantly.

---

## Strategic / hygiene

### 16. CI builds

A GitHub Actions workflow that builds all examples on macOS on every push is low effort and catches Dawn/RGFW drift early. Add it before the codebase grows.

### 17. Release notes + semver discipline

v0.1.0, v0.2.3, v0.3.0 are used informally. Document what each version means and what breaking changes look like. A one-page `VERSIONING.md` is enough.

### 18. Dependency update policy

Dawn and RGFW will keep moving. Decide now:

- Pin to exact versions and update on a schedule?
- Track a stable branch?
- Vendor patches for `[VERIFY]` blocks?

This avoids panic when a dependency update breaks the Metal surface code.

---

## Summary: my goto order

If I were picking the next three unmapped things to do:

1. **Wire `enableValidation` + add depth to `Surface`.** These are correctness blockers for 3D.
2. **Write and implement the SketchApp batching design.** The current per-primitive allocation will become the first performance wall.
3. **Add a non-GPU test harness.** It will make every refactor faster and safer.

Everything else is valuable, but those three change the foundation.
