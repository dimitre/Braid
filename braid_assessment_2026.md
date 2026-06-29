# Braid — Impressions & Relevancy in 2026

*An honest assessment, not a pitch. Written after building v0.1.0 to a working
triangle and crystallizing the Surface design with the author.*

---

## Short version

The **timing is genuinely good**, the **design has a real thesis** (not "OF but
newer"), and the **risks are the ordinary solo-framework ones** plus one strategic
question (C++ vs. the web audience). It probably won't become "the next
openFrameworks" — network effects are brutal and that's the wrong goal anyway. As a
**sharp, opinionated, personal tool that's genuinely co-developable with an LLM**, it's
well justified and arrives at the right moment. Its highest relevancy is as a *scalpel
for a specific aesthetic* (GPU feedback / Surface algebra / installations), not as a
mass framework.

**Relevancy in 2026: high for its niche, modest as a general framework.** That's a
compliment, not a hedge — niche tools with a point of view age better than broad ones
without.

---

## Why the timing is right (2026 specifically)

1. **WebGPU has actually arrived.** It shipped in browsers (2023) and by 2026 the
   native stacks (Dawn, wgpu) are stable and shippable. A portable, modern GPU API that
   is *the same shape* on native and web is finally real. Building a creative framework
   on WebGPU today is not early-adopter risk — it's the correct substrate. OpenGL is
   legacy; Metal/Vulkan/D3D directly is too much. WebGPU is the sweet spot, and it
   barely existed when OF/Cinder were designed.

2. **openFrameworks' weight is a real opening.** OF is a ~2005-lineage OpenGL codebase:
   beloved, vast, and *heavy*. Cinder is largely dormant. Nannou (Rust) is the modern
   systems-language attempt but stays niche. There is no small, fast, **WebGPU-native,
   LLM-legible** C++ creative framework. That gap is exactly where Braid sits.

3. **LLM-assisted coding rewards small, legible frameworks.** In 2026 you don't build a
   framework alone — you build it with a model in the loop. The design choices that make
   Braid "LLM-friendly" (single-include, explicit lifetimes, `Result` returns, no macro
   magic, named structs) are the *same* choices that make it human-debuggable and
   fast-to-iterate. This isn't marketing; it showed up live — we took it from spec to a
   working triangle in one session, and the model could hold the whole interface in
   view. A 2-million-line engine can't be co-developed this way; a 2,000-line one can.

---

## What makes it more than "a newer OF"

Most "modern OF replacement" projects are just a re-skin. Braid has an actual **design
thesis**, and that's rarer than a new renderer:

- **Surface as the single primitive.** "You can only draw via a Surface; the screen is
  just the Surface you show." This collapses Screen / image-export / video-out /
  feedback into one elastic object. It's a real architectural stance, and it makes
  whole feature categories *fall out for free* instead of being bolted on.
- **Total algebra over guarded operations.** `surface += surface.transformed()` that
  *just works* — uninitialized = additive identity, size mismatch = defined composite,
  format mismatch = promote. Replacing defensive `if`s with definitions is a genuine
  design maturity most frameworks never reach.
- **The artist's tacit knowledge, encoded.** The feedback model comes from real
  analog-video-feedback practice (invert as snake-eating-tail, recursive 105% zoom as
  tunneling, **gain** as the hand on the loop). "Expose the ouroboros, hide the
  ping-pong" is the kind of insight you only get from having *done* it, not from reading
  a graphics paper. That authenticity is the moat a committee framework structurally
  cannot have.

A framework with a point of view ages better than a feature-complete one without.

---

## The honest headwinds

1. **C++ vs. where the audience is.** Most new creative coders in 2026 start in
   p5.js / Three.js / TouchDesigner. C++ is a deliberate narrowing — justified for
   installations, performance, and native control, but it caps the addressable
   audience. The counter: that audience (installation/performance artists who've
   outgrown the browser) is underserved and *sticky*.

2. **"If the magic is WebGPU, why not ship to the browser?"** The strongest strategic
   tension. Braid's substrate is literally the browser's GPU API. A web/Emscripten
   target could put sketches in a tab where the audience already is — that might matter
   more than its current "Later" placement suggests.

3. **Maintenance tax is real and ongoing.** We hit Dawn API churn *in this very
   session* (StringView, surface descriptors, callback shapes). RGFW's API had drifted
   hard from the spec's assumptions. A solo WebGPU framework pays a continuous
   reconciliation cost as Dawn evolves. Pinning versions helps; it doesn't eliminate it.

4. **The solo-framework bus factor.** Your own OF story is the cautionary tale in both
   directions: a committee killed a beautiful PR with "what-ifs," but a committee also
   *sustains* a project past any one person. Solo means Braid lives or dies with your
   attention. That's freedom (the `+=` dream is *allowed to exist* here) and fragility
   (no one else carries it).

5. **Frameworks live on examples, docs, and community — not code.** The hardest,
   least-fun 80%. A microframework can punt on this longer than a big one, but adoption
   beyond yourself is gated by it. The good news: it doesn't *need* adoption to be worth
   building — see below.

---

## Who it's actually for

- Installation / performance / VJ artists who've outgrown p5/Processing and want native
  performance + explicit GPU control without OF's weight.
- People drawn to the OF/Cinder lineage who find those projects dated or heavy.
- The GPU-feedback / generative / shader-art crowd — for whom Surface algebra and
  one-knob feedback are *exactly* the native grammar they want.
- Increasingly: anyone who codes with an LLM and wants a framework the model can fully
  hold in context.

It is **not** for: beginners (use p5.js), web-first projects (use Three.js), or
node-graph VJs happy in TouchDesigner.

---

## Verdict

Braid is **the right idea on the right substrate at the right moment** — with the
ordinary caveat that solo creative frameworks are hard to grow and easy to under-
maintain. What pushes it above the median "modern OF" attempt is that it isn't chasing
OF; it has a *thesis* (Surface as the one elastic primitive; algebra and feedback as
native grammar) drawn from real artistic practice and sharpened into clean
architecture.

The most honest framing: **don't measure it by adoption, measure it by fit.** If it
becomes the tool that lets *you* (and a handful of kindred artists) express GPU feedback
and Surface algebra with less friction than anything else — running fast, native, and
co-built with an LLM — then it has already succeeded, regardless of star counts. The
2026 conditions (WebGPU mature, OF aging, LLM-assisted development normal) mean a
small, opinionated, beautiful framework is *more* viable now than at any point in the
last decade, not less.

The dream is allowed to exist. In 2026, it's also allowed to be small, fast, and
entirely yours — and that's the version most likely to last.
