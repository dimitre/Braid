# Braid Addons — First Impressions

**Status:** draft, 2026-07-01. Written before the addons design session as a starting position, after reviewing `md/ui.md` §4 (the first runtime `Addon` skeleton), the existing link-to-enable TUs, and the ofworks addons tree at `/Users/z/Dmtr/ofworks/addons/`.

This is opinion, not locked design. The next session should argue with it.

---

## 1. "Addon" currently means two different things

Braid already has two addon mechanisms, and they solve different problems:

1. **Link-to-enable TUs** — `braid_image.cpp`, `braid-syntype`. Free functions or plain classes declared in a header, defined in an optional TU, reaching core through `detail::ctx()` (`braid_detail.h`). The problem they solve is **dependency weight**: mango, font stacks, codec libraries stay out of the binary unless linked. Proven: core-only hello ≈10.4 MB, addon-linked ≈14.25 MB (roadmap).
2. **Runtime `Addon` objects** — the base class designed in `ui.md` §4 (`setup`/`update`/`draw`/`afterDraw`/`overlay`), registered per-window via `Window::addAddon`. The problem they solve is **frame lifecycle**: things that need to act every frame without the sketch calling them.

These are **orthogonal axes**, not competing designs. In openFrameworks both collapse into one folder convention (`addons/ofxFoo` + `addon_config.mk`), which is why oF addons are such a mixed bag — a physics engine, a GUI, and an XML parser all pretend to be the same kind of thing. Braid should keep the axes separate and name them:

|  | **plain object / free functions** | **runtime `Addon` (frame hooks)** |
|---|---|---|
| **always linked (core)** | Surface, Mesh, Shader, Channel | — (core has no addons by definition) |
| **link-to-enable** | `braid-image`, `braid-syntype`, future codecs/CV/physics | `TinyUI`, future Syphon/NDI publishers, recorders |

The linkage axis is chalet's job (build targets). The runtime axis is `Window`'s job (the hook loop). A given addon picks a cell; TinyUI is the first occupant of the bottom-right.

### 1.1 User-facing vocabulary: one word — "addon" (direction from Dimitre, 2026-07-01)

An addon is anything you can plug into the engine, like a peripheral into a computer: engine at the center, addons as generalized I/O devices at the edges. For the **user** there is exactly one word and one plug. The 2×2 above is *device classes* — implementer vocabulary that should never appear in user docs or the addon catalog, the same way no one thinks about USB device classes when plugging in a mouse.

The user-facing catalog axis is functional, not structural: an addon either **does something** (image, fonts, CV, physics, UI) or **connects to something** (MIDI, OSC, NDI, Syphon, Artnet, sensors). It cross-cuts the 2×2 completely — "connects" contains both NDI-out (frame-hooked) and MIDI (plain + `Channel`); "does" contains both TinyUI (frame-hooked) and CV (plain). That's the mark of a good user split: it predicts intent, not implementation.

Consequence: the **plug must be uniform** for the metaphor to hold — same folder layout, same one-line declaration, same include-one-header experience for every addon regardless of internal shape. Runtime usage may differ by device class (`addAddon` a UI vs. just constructing a Midi object); the build-time plug may not. §4 defines what that plug is.

---

## 2. First impressions of the `Addon` skeleton

### 2.1 The interface is the right size, but it's designed at n=1

Five phase hooks and deliberately no ordering knob (`priority()` was removed 2026-07-01 — registration order within a phase; "must see the finished frame" is expressed by the `afterDraw` phase, not by out-numbering other addons) — small enough that wrong guesses are cheap to fix. But every hook so far is shaped by TinyUI's needs. **Freeze it this small until a second real addon exists** — the Syphon/NDI publisher (§3) is the best candidate second occupant, because it exercises `draw()` without `overlay()`, the opposite profile from TinyUI. If the interface survives both, it's probably right.

### 2.2 The hook set maps cleanly to real frame phases

- `setup(Window&)` — after device + window exist.
- `update(Window&)` — pre-draw, main thread. The natural place to drain `Channel`s.
- `draw(Window&)` — after the sketch's draw. The natural place for **contributors** that add to `mainSurface_`.
- `afterDraw(Window&)` — after the sketch *and* every addon's draw: the frame is final. The natural place for **readers** of the finished frame (publishers, recorders).
- `overlay()` — present time, swapchain compositing. The natural place for **layers on top** (UI, guides, meters).

One rule the skeleton hasn't locked yet: **setup timing**. If `addAddon` is called before the window/device exists (constructor-time member registration), `setup` must be deferred to window init; if called on a live window, `setup` runs immediately. Lock that rule before the second addon exists, or every addon will hand-roll its own "am I initialized" flag.

### 2.3 The clean-feed consequence is a big deal — write it on the wall

Because the UI overlay never touches `mainSurface_` (`ui.md` §9), any addon reading `window.surface()` in `draw()` gets **UI-free frames automatically**. Concretely: a Syphon or NDI publisher addon sends a clean feed to the projector/media server while the local window shows sliders. That is exactly the setup every installation wants, and in oF it requires deliberate FBO discipline that ofxMicroUI users have to hand-build every time. Here it falls out of the architecture. When evaluating future addon designs, protect this property.

### 2.4 Ownership and lifetime: adequate, slightly trusting

Two `addAddon` overloads (non-owning ref for sketch members, `shared_ptr` for heap). The teardown rule from `ui.md` §4 (Window never calls addons during destruction) makes the non-owning path safe for the common case. It's still trusting — there's no `removeAddon`, no handle. Fine at n=1; revisit when an addon needs to unregister mid-run (the `Subscription`-handle idea is the known escape hatch).

### 2.5 Not everything optional is an Addon

This is the opinion most likely to matter in the ofworks review: **services without frame needs should be plain objects + `Channel<T>`, not Addons.** Braid's `Channel` was explicitly designed so an I/O thread can produce events without touching GPU state off-main (`braid.h` header comment). A MIDI listener, OSC receiver, Artnet node, GPS/lidar reader — none of them need `draw()` or a window. Forcing them through the Addon interface would recreate oF's category confusion. Their braid shape is: link-to-enable TU, plain class, owns a thread, publishes into a `Channel` the sketch (or a TinyUI-MIDI bridge) drains in `update`.

The test: **does it need the frame loop or a window surface?** No → plain object. Yes → Addon.

### 2.6 Window-scoped is right for the Addon interface precisely *because* of 2.5

"Addons are window-scoped" (roadmap §1) looked over-broad to me until the taxonomy above: once services are excluded from Addon-hood, everything left genuinely needs a surface and mouse/window context. The claim holds — but only because the Addon category is narrow. If someone proposes an app-scoped Addon, the answer is probably "that's a plain object."

---

## 3. Mapping ofworks addons into braid shapes

Discussion seeds for the next session, from `/Users/z/Dmtr/ofworks/addons/`:

| ofworks addon | braid shape | notes |
|---|---|---|
| ofxMicroUI, ofxTinyUI, ofxImGui | runtime Addon | done: `ui.md` TinyUI |
| ofxSyphon, ofxNDI (×7 variants!) **out** | runtime Addon, `afterDraw()` hook | best second Addon; gets clean feed free (§2.3) |
| ofxNDI **in**, ofxHapPlayer, video players | link-to-enable, plain object → produces a `Surface`/texture; maybe `update()` for frame upload | receiver ≠ publisher; may not need Addon-hood at all |
| ofxFFmpegRecorder / VideoRecorder / VideoWriter | runtime Addon (`afterDraw()` reads `mainSurface_`) + heavy deps → link-to-enable | same clean-feed property as publishers |
| ofxMidi, ofxMicroUIMidiController | plain object + `Channel<MidiEvent>` | the MicroUIMidiController pattern becomes a tiny bridge: drain channel → `ui.set<T>()` |
| ofxOsc / ofxNetwork / ofxArtnet | plain object + `Channel` | classic §2.5 services |
| ofxGPS, ofxRPlidar, ofxUVC, ofxKinect | plain object + `Channel` (or texture producer) | sensor I/O threads |
| ofxFontStash2, ofxSmartFont | link-to-enable, plain | `braid-syntype` already exists |
| ofxCv, ofxAssimp, ofxBox2d(_dimitre), ofxVoronoi | link-to-enable, plain algorithm/data libs | no frame hooks; Box2d maybe wants `update()`, resist it — sketch can step the world |
| ofxXmlSettings | n/a | presets already text-based |

Pattern worth noticing: almost nothing in the list needs the Addon interface. The runtime-Addon category is small — UI, publishers, recorders, overlay meters — and that's healthy. The bulk of the ecosystem is plain libraries plus `Channel`-fed services, which need **chalet conventions**, not runtime machinery.

---

## 4. The plug: addon manifests + a generator, not hand-written chalet (direction from Dimitre, 2026-07-01)

Addon integration should not be *based on* chalet — chalet.yaml becomes a **generated artifact**, re-created from project + addon properties by a braid equivalent of **ofGen** (`/Users/z/Dmtr/ofworks/ofGen/`). This is proven tech, not speculation: ofGen already reads a project manifest (`of.yml`: addons, defines, frameworks, templates) and generates chalet.yaml + Zed/Xcode/VSCode config for ofWorks, with local-first addon search and recursive addon-dependency resolution. A "braidGen" is a port with a smaller job — one platform (macOS), one build system — so v1 = chalet.yaml + `compile_flags.txt` + `.zed/`. In ofGen's own architecture chalet is just one *template*; the manifests are the source of truth, and generated files are never hand-edited.

Motivation is already visible in braid's current chalet.yaml: the 7-archive mango block is copy-pasted into four targets (feedback, feed, bloom, image), and `addons/braid-syntype` + `addons/braid-ui` includeDirs leak into the global abstract shared by every target. That knowledge belongs to each addon, stated once.

### 4.1 Per-addon manifest: `build` + `plugs`

Each addon carries a manifest (braid's `addon_config.mk` analog). Two sections matching the two axes of §1, plus the §1.1 catalog kind:

```yaml
# addons/braid-image/addon.yml (filename TBD)
name: braid-image
kind: does            # does | connects (§1.1)
build:                # consumed by braidGen → chalet.yaml
  sources: [braid_image.cpp]
  staticLinks: [libmango.a, libdeflate.a, libz.a, liblcms2.a,
                libzstd.a, libbz2_static.a, liblz4.a]
plugs: []             # frame sockets it asks of braid; braid-ui: [update, overlay]
```

`plugs:` is Dimitre's node-based idea: the addon declares which internal sockets it plugs into (update / draw / overlay / channels it publishes) — "one can plug to draw or not." Honest scope: the C++ virtual overrides remain the *truth* for hooks; `plugs:` is the USB device descriptor — catalog metadata, patchbay tooling (a generator can print which addons plug into which frame phases), and a forcing function for authors to state their device class. Far-future horizon, noted not designed: wiring declared in data (generated `addAddon` glue), the sketch's patch as a node graph.

### 4.2 The project is an addon (direction from Dimitre, 2026-07-01)

ofGen's smartest move, inherited wholesale: the project manifest carries **the same schema** as an addon manifest — sources, staticLinks, frameworks, defines, everything — plus the few root-only fields (version, the `addons:` list, braid path). One schema, one parser; the project is simply the root node of the dependency graph, the one that has `main()`. Consequence: anything an addon can carry, a project can carry ad hoc (a one-off framework link, a define) with no special case in the framework. Recursive dependency resolution (§4.5) then treats project → addons → addon-deps as one graph.

### 4.3 Project layout: mirror ofWorks' apps relation, but discover the path (don't assume `../../..`)

Per-project folders are the real target shape, like oF's `apps/`; the monorepo `examples/` + hand-kept chalet.yaml stays as the framework-development playground (both shapes are cheap once the addon manifest is identical in either). What gets rethought is oF's depth-locked `ofpath: ../../..`. braidGen resolves the braid root by a chain:

1. `braidpath:` in the project manifest — explicit pin; doubles as **version pinning** so an installation remounted years later builds against the braid checkout it shipped with;
2. `BRAID_PATH` environment variable;
3. **upward marker search**: walk up from the project folder for a `.braidroot` marker (the `.ofroot` analog) — preserves oF's zero-config nested experience but at *any* depth (`braid/apps/2026/client/show/` works, moving a project within the tree never breaks it);
4. interactive prompt, like ofGen's "edit ofpath? (y)es, (n)o, (q)uit".

Net: nested projects zero-config, external projects (client folder, external drive) first-class via one line, nothing counts `../`s.

### 4.4 Sequencing: convention before tool

Don't write braidGen until the manifest schema has survived retrofitting the three real addons (braid-image, braid-syntype, braid-ui). The convention is the design deliverable; the generator is its automation, with ofGen as the reference implementation to port. Until then, hand-write chalet.yaml *against the convention*. Housekeeping the uniform plug demands: `braid_image.cpp` lives in `core/` today — it belongs in `addons/braid-image/`.

### 4.5 Policies to inherit from ofGen

Local-project-first addon search, recursive addon dependency resolution, CLI overrides of manifest fields, an `import` migration path, and the golden rule: commit the manifest, regenerate the rest.

### 4.6 Events interoperability with ofWorks (future)

The cheap, real version is **shared event vocabulary + thin adapters**, not framework linking. `Channel<T>` is thread-safe with main-thread pop, so an ofx addon's listener thread can push into a braid `Channel` from a callback (~10-line bridge). Design rule when braid-midi/braid-osc arrive: mirror ofxMidi/ofxOsc message struct fields so existing mapping code and muscle memory port 1:1.

---

## 5. Open questions for the session

1. **The second Addon should be Syphon or NDI out** — it stress-tests `draw()`, the clean-feed property, and link-to-enable packaging at once. Which one first? (Syphon is macOS-local and tiny; NDI is the show-critical one given seven forks.)
2. **Addon folder layout + manifest schema** — direction set by §4 (manifest is truth, project is an addon, per-project folders + discovered braid path per §4.2–4.3); remaining details: exact folder layout (header, TU, shaders/, fonts/), manifest filename, and the concrete field set.
3. **setup-timing rule** (§2.2) — lock before implementing TinyUI.
4. **Does anything ever need an app-scoped Addon**, or does §2.5 hold everywhere? (Candidate counterexample: a recorder capturing *all* windows?)
5. **Naming — resolved direction (2026-07-01):** "addon" is the single user-facing word for everything pluggable (peripheral model, §1.1); the linkage/runtime split stays internal. Remaining sub-question: the runtime interface class in `ui.md` §4 is currently named `Addon`, which overloads the umbrella word ("braid-midi is an addon with no `Addon` in it"). Proposal: rename the class **`WindowAddon`** — `window.addAddon(ui)` still reads naturally, and docs can say "addons that hook the frame loop implement `WindowAddon`." Cheap to rename now, before implementation starts.
