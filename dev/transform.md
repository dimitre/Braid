# Braid Transform — closing the placed-quad / point-space gap

**Status:** design brainstorm, 2026-07-02. Not decided, not implemented. Motivated by a
real bug (§1) that got a numeric patch, not a structural fix — this doc is where to land
before more code reaches for `paste`/`pasteSelf` and inherits the same foot-gun.

---

## 1. The bug that started this

`examples/feed.cpp` called:

```cpp
surface().pasteSelf({width() * 0.5f, height() * 0.5f}, {width() * scale, height() * scale}, ...);
```

`width()`/`height()` here are `SketchApp`'s (point space, per `dev/hidpi.md` §1). But
`Surface::pasteSelf`'s `center`/`size` are pixel space — `Surface` always speaks pixels,
never points. On a Retina display with `hidpi=true` (default), `mainSurface_` is 2×
the point dimensions, so the pasted quad landed at half the intended size, off-center.

**Patched locally:** `examples/feed.cpp:48-52` now uses `surface().width()/height()`
instead of the SketchApp accessors; the stale doc comment on `Surface::paste`/`pasteSelf`
in `braid.h:361-368` ("center & size are in pixels ... like SketchApp 2D" — true pre-HiDPI,
false now) was corrected to say explicitly which accessor to use.

That fixes the one call site. It does not fix why it was easy to get wrong.

## 2. Root cause: two disconnected drawing systems

Every vertex primitive (`rect`, `circle`, `line`, `text`, `box`, …) flows through one
system: `pushMatrix`/`translate`/`rotate`/`scale` build a `glm::mat4` in
`transform_.current` (point space), and `mvp = proj_ * view_ * transform_.current` is
what actually places vertices (`braid_sketch.cpp:120,153-163,185-196`). A sketch author's
mental model — point space, composable transform stack — is consistent everywhere in
this system.

`Surface::paste`/`pasteSelf` are a **second, disconnected system**: a `Surface` method
(not `SketchApp`), taking raw pixel `center`/`size`/`rot`, hand-computing NDC directly in
`makeQuadUniforms` (`braid_surface.cpp:229`) with zero connection to `transform_`. Two
consequences:

- `pushMatrix()`/`rotate()`/`scale()` state a sketch has already set up does nothing to a
  `pasteSelf` call — the systems don't compose.
- Every placed-paste call site must manually know and convert into `Surface`'s pixel
  space by hand. That's exactly the bug in §1, and it can recur anywhere else
  `paste`/`pasteSelf` is used — including future addons that don't know this history.

The GPU side (`QuadUniforms` — `braid_compositor.h:35-42`; `kQuadWGSL` —
`braid_compositor.cpp:237-265`) already supports translate + rotate + non-uniform scale
in NDC (no shear) — it's the CPU-side entry point that's the problem, not the shader.

## 3. Options

### Option A — thin point-space `Transform` + one correct conversion point

A small value type — `translate`/`rotate`/`scale` fields, or a `glm::mat3` for full 2D
affine (shear included, though the current shader doesn't consume shear yet) — built and
composed in **point space**, matching every other sketch-facing coordinate.

`SketchApp::pasteSelf(Transform t, ...)` converts once, using the *dynamic* ratio
`surface().width() / width()` — not a static `pixelRatio()`, which would be wrong when
`AppSettings::hidpi = false` (then `mainSurface_` is point-sized but `pixelRatio()` still
reports 2.0; per `dev/hidpi.md` §2 the ratio that matters is surface-pixels-per-point, not
the display's raw scale) — then calls down into `Surface`'s existing pixel-space API.
`Surface` stays pixel-pure; nothing about fortress rule 5 (`dev/hidpi.md` §10) changes.

Sugar: overload `operator*` so `t * surface()` applies the transform — transform on the
left, mirroring glm's `mat4 * vec4` operand order — as shorthand for the
`pasteSelf(t, ...)` call above. Not a different fix, just a nicer spelling of it.

**Pros:** small, contained, fast to ship — an afternoon, not a redesign.
**Cons:** still "a second place to remember." `Surface::pasteSelf`'s raw pixel overload
stays public and reachable; a future call site can still bypass the point-space entry
point and reintroduce §1's bug. Mitigated (corrected doc comment) but not structurally
prevented.

### Option B — unify placed-paste into the real vertex/MVP pipeline

Make placed-paste a textured quad drawn through `proj_ * view_ * transform_.current`,
the same path as `rect`/`circle`/`text`. Then `pushMatrix`/`rotate`/`scale` apply to a
pasted quad *for free*, because it's no longer a separate system — the point/pixel gap
closes structurally instead of growing a second bridge to remember.

Needs:
- A textured-quad draw path that samples a snapshot of the very surface it draws into.
  `pasteSelf` already solves the self-sampling constraint via `ensureScratch()`/scratch
  ping-pong (`braid_surface.cpp:252-273`) — that part ports over.
- A new pipeline variant alongside the existing `(format, lines, textured)` cache
  (`sketchPipeline`, `braid_sketch.cpp:211-241`) — text already proves textured pipelines
  fit this cache shape.
- Reconciling `SketchUniforms{mvp, tint}` (vertex path) with `QuadUniforms`'s `invert`
  flag — either add `invert` to the sketch uniform block or handle it as a pipeline/blend
  variant.

**Pros:** the only option that actually eliminates the two-systems problem, rather than
adding a third known place (`dev/hidpi.md` rule 2 currently claims exactly two: the
`braid_app.cpp` seam and SketchApp's projection setup — Option A would quietly make that
three).
**Cons:** real scope. Touches batching, the pipeline cache, and self-sampling snapshot
semantics inside the shared path — not a quick patch.

## 4. Recommendation (loose — not decided)

Option B is the structural fix and the one that matches the project's own fortress
rules (`dev/hidpi.md` §10: "scale conversion lives in two known places" — B keeps that
true, A breaks it a little). Option A is the pragmatic fallback if the batching/pipeline
work in B is more than the moment calls for; if A ships, `pasteSelf(Transform, ...)`
should be the sketch-facing shape either way, so a later migration to B doesn't change
call sites — only what's underneath them.

## 5. Open questions

- **Representation:** `glm::mat3` (general affine, shear included) vs. a plain
  translate/rotate/scale struct (matches what `kQuadWGSL` actually consumes today, no
  shear support without a shader change)?
- **Operator direction:** `t * surface()` vs `surface() * t`? Leaning transform-on-left
  (glm convention: matrix operates on its right-hand operand).
- **Scope:** does `Transform` stay scoped to `paste`/`pasteSelf`, or become a general
  value-type alternative to the imperative `pushMatrix`/`translate`/`rotate`/`scale`
  stack for other primitives too? Out of scope for closing §1's bug — worth asking
  before Option A locks in a shape.
- **Visibility:** once a point-space entry point exists, should `Surface::paste`/
  `pasteSelf`'s raw pixel overload become addon/internal-only, to remove the bypass
  that caused §1 rather than just discourage it?

## 6. Next steps

- [ ] Decide A vs. B (or A now as an interim, B later once batching work is scheduled
      anyway).
- [ ] Prototype the chosen option against `feed.cpp` (the shape that broke) as the proof.
- [ ] If A ships: note in `dev/hidpi.md` rule 2 that the "two known places" grew a third,
      or fold the conversion into one of the existing two instead of adding a new one.
- [ ] If B ships: fold `paste`/`pasteSelf` into `braid_sketch.cpp`'s batching pass and
      retire the standalone `Surface`-level pixel API from sketch-facing use (addons that
      genuinely need raw pixel placement — LED-wall-style — can still reach `Surface`
      directly per `dev/hidpi.md` §1's "user Surfaces are sacred pixels" rule).
