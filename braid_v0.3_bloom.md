# Braid v0.3 — Bloom (the natural roadmap)

**Thesis:** bloom is **not** a monolithic post-effect. In Braid it's *Surface algebra*:

```
bloom(s)  =  s  +  blur( brightpass(s) ) · intensity
```

That's it. `brightpass` and `blur` are Surface→Surface transforms; `+` is the additive
composite we already shipped. And because the default Surface is now **16-bit float**, the
"bright" part is literally *the values already above 1.0* — HDR makes the threshold honest
instead of arbitrary. Bloom falls out of verbs we already have plus two new ones.

Spelled out as algebra a user could hand-write (and the convenience just wraps this):

```cpp
Surface glow = surface().clone();   // copy current frame
glow.threshold(1.0f).blur(16.0f);   // keep the bright, blur it wide
surface() += glow;                  // add the glow back  (HDR additive)
```

---

## North stars for this milestone
1. **Bloom is composition, not a black box.** Ship `threshold` and `blur` as first-class
   Surface verbs; `bloom()` is sugar over `s += blur(threshold(s))`.
2. **HDR makes threshold real.** Default threshold = 1.0 — only energy that exceeded white
   blooms. Lower it for stylized glow. (This is why we went 16-bit float first.)
3. **Wide blur must be cheap.** Don't brute-force a huge Gaussian; downsample → blur small →
   upsample (mip pyramid / dual-filter). Wide soft glow at small cost.
4. **It must compose with feedback.** `feedback` then `bloom` each frame = self-glowing
   tunnels. The payoff demo.

---

## Phase 0.3.0a — `blur` (the workhorse)
Separable Gaussian: a horizontal pass then a vertical pass, each a Compositor blit with a
direction + radius uniform doing N weighted taps.
- API: `Surface& blur(float radius);`  (radius in pixels)
- Impl: add a `blur` shader (or a mode in the blit shader) sampling 2·k+1 taps along a
  direction; run H then V via the existing scratch+swap ping-pong.
- **Proof:** `sketch` with `surface().blur(8)` at end of frame → soft, even blur, no banding.

## Phase 0.3.0b — `threshold` (brightpass)
Trivial fragment op, same shape as `invert`.
- API: `Surface& threshold(float level = 1.0f, float knee = 0.1f);`
- Impl: per-pixel `max(0, (luma - level)/knee)`-weighted keep; one self-pass.
- **Proof:** a scene with one over-bright shape → only that shape survives threshold.

## Phase 0.3.0c — downsample pyramid (cheap wide blur)
Wide bloom without giant kernels: halve resolution a few times, blur the small levels,
upsample-add. The modern dual-filter / Kawase approach.
- API (internal, or exposed): `Surface& downsample();  Surface& upsample();`
- Impl: a chain of half-res scratch textures owned by the Surface (lazy); `blur(radius)`
  picks a pyramid depth from `radius`.
- **Proof:** `blur(64)` looks smooth and runs as cheap as `blur(8)`.

## Phase 0.3.0d — `bloom` (the convenience)
- API: `Surface& bloom(float threshold = 1.0f, float intensity = 1.0f, int passes = 5);`
- Impl: `glow = clone(); glow.threshold(threshold); glow.blur(pyramid(passes)); *this += glow*intensity;`
  Needs one temp Surface (the glow buffer) beyond the transform scratch — allocate a
  half-res internal buffer; document the cost (one extra texture, half-res).
- **Proof:** `fill` a bright circle, `bloom()` → it glows; intensity/threshold are knobs.

## Phase 0.3.0e — feedback × bloom (the payoff)
- `examples/bloom.cpp`: each frame `surface().feedback(0.97, …)` then `surface().bloom()`,
  add a bright moving source. Self-glowing feedback — luminous tunnels that the 8-bit path
  could never produce.
- **Proof:** the saved frame shows soft glowing trails, not hard-edged ones.

---

## Implementation notes (so it stays honest)
- **Blur needs multi-tap sampling.** Either extend `kBlitWGSL` with a `mode`/`blurDir`/
  `taps` branch, or add a dedicated separable-blur shader to the Compositor. The pyramid is
  what keeps wide radii cheap; a single huge kernel is the wrong path.
- **Bloom is not purely in-place.** It adds a *blurred copy* back onto the original, so it
  needs a second buffer (the glow), unlike the transforms which reuse one scratch. Give the
  Surface a small lazily-allocated half-res bloom pyramid; reused across frames.
- **Totality holds.** `blur`, `threshold`, `bloom` are total Surface→Surface ops — never
  error; empty/zero in → empty/zero out. Same rule as the rest of the algebra.
- **Tone on export.** `save()` already clamps 16F→[0,1]; bloomed HDR will clip there. A real
  tonemap (Reinhard/ACES) in `save()` and in the present blit is a small v0.3.1 follow-up if
  you want the on-screen look to match the internal HDR.

---

## One-line creed (bloom edition)
*Bloom isn't an effect you bolt on — it's `surface += blur(bright(surface))`. Same algebra,
one new verb (`blur`), made possible by going 16-bit float first.*
