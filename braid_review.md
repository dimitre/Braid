# Braid — Notes from a First Read

*Read on 2026-06-29, the day the project was started. These are first-pass regards on the code, the design opinions, and the risks.*

---

## What it is

Braid is a small, opinionated C++ creative-coding framework built directly on WebGPU (Dawn + RGFW). It is roughly ~1,400 lines of implementation split into a single-include API (`braid.h`) and implementation (`braid.cpp`). It targets macOS today, with Linux/Windows noted as later.

It has a two-tier design:

- **Tier 1 — explicit**: `App`, `Shader`, `Mesh`, `Surface`. You write WGSL, manage pipelines, and draw directly.
- **Tier 2 — sketch**: `SketchApp`, a Processing-style facade with `rect()`, `ellipse()`, `box()`, `pushMatrix()`, etc.

---

## The design opinions that stand out

### 1. Surface as the one primitive

This is the strongest idea. "You can only draw via a Surface; the screen is just the Surface you show." The same object handles the main draw target, offscreen rendering, feedback, save-to-disk, and (eventually) video-out. That collapses a lot of framework surface area into one concept.

### 2. Total algebra over defensive guards

`surface += other` (additive), `surface.over(other)` (alpha), `zoom()`, `rotate()`, `invert()`, `feedback(gain, transform)` — these are defined to be total, with empty/invalid operands treated as identity. That is a real design stance, not just wrapping WebGPU.

### 3. Feedback is a first-class concept

```cpp
surface().feedback(0.97f, [](braid::Surface& s) {
    s.zoom(1.03f);
    s.rotate(0.01f);
});
```

This is genuinely expressive. The hidden ping-pong buffer is the right thing to hide.

### 4. Modern C++ shape

`Result<T>` everywhere instead of exceptions, move-only GPU resources with explicit `clone()`, `std::span`, `std::optional`, `std::function` callbacks. It reads like code written in 2026, not 2006.

### 5. LLM-sized

The whole framework fits in a context window. That is increasingly a feature, not a constraint.

---

## Things that give pause

### The `Surface` name collision

`braid::Surface` wraps and is constructed from `wgpu::Surface`. The code constantly has to qualify `wgpu::Surface` vs `braid::Surface`. The roadmap itself flags this as "a real cognitive tax." If renaming were on the table, `Screen`, `Canvas`, `Layer`, or `Folio` would avoid the collision.

### No batching yet in the Sketch tier

`drawTris()` and `drawLines()` currently create a fresh uniform buffer + bind group per primitive. For the `cubes.cpp` demo with hundreds of wireframe boxes, that is a lot of per-frame churn. A 3-slot ring buffer is fine for one uniform per frame, but not for one uniform per primitive.

### Platform lock-in

macOS-only today. RGFW supports Linux/Windows and Dawn builds exist, but cross-platform will require touching the two `[VERIFY]` blocks (Metal surface attach, adapter/device request callbacks) and validating swapchain capabilities. That is real work.

### Dawn dependency

You are pinned to a ~10 MB static library and an API that is still moving. API churn was hit during the first session. That tax does not go away.

### C++ narrows the audience

That is clearly intentional, but it means Braid competes for a small, performance-sensitive slice of creative coders rather than the p5.js/Three.js mainstream.

---

## Overall regards

Braid is a strong, coherent framework with an actual point of view. Most "modern openFrameworks" attempts are just re-implementations; Braid has a thesis: *the Surface is the universal object, and GPU feedback is a native language primitive*. That gives it a reason to exist beyond "newer OF."

For a project started today, the execution is unusually tight:

- Working examples already: triangle, sketch, feedback, cubes.
- Clean error model.
- Persistent offscreen target with 16-bit float for smooth feedback.
- Strategic notes already captured in `braid_assessment_2026.md`, `braid_roadmap.md`, and `braid_v0.1.1_critical_fixes.md`.

The honest framing in the project's own docs is right: do not measure this by GitHub stars. Measure it by fit. If Braid becomes the fastest way for a small set of like-minded artists to write native GPU feedback and generative sketches, it has already succeeded. WebGPU is the right substrate, the codebase is small enough to co-develop with a model, and the design has a real aesthetic point of view.

**One concrete near-term win:** batching / ring-buffered uniforms in `SketchApp`. The current `freshUniform()` per primitive is fine for demos but will become the bottleneck as sketches grow. After that, the `Surface` naming collision is worth revisiting.

It is a promising start.
