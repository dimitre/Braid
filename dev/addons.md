# Braid Addons

**Status:** mechanism implemented and shipping three addons (`braid-image`, `braid-syntype`,
`braid-ui`). This doc merges the original architecture plan and the post-implementation
"first impressions" review into one current reference. §7 (manifest + generator) is the one
part still unimplemented — hand-written `chalet.yaml` today.

Braid has a small, opinionated core: `Surface` + algebra + the loop. Everything that depends
on the core but that the core does not depend on is an addon.

---

## 1. Core principle

> **If a feature depends on `Surface` but `Surface` does not depend on it, it is an addon.**

WebGPU/Dawn fails this test — the core depends on it — so it is the floor, not an addon.
Images, text, audio, MIDI, video, UI, etc., all pass the test. Full reasoning and the
three-layer (platform / core / addons) breakdown: `braid_roadmap.md` §"Architecture — core
vs addons".

**The test is dependency weight, not feature category** — "typography" or "audio" is not
itself the addon/core boundary; how much a given implementation costs is. `core/braid_text.cpp`
is the live proof: `BitmapFont` is core-owned typography (SketchApp's zero-config debug/HUD
text, `text()` with no linking required) precisely because it's cheap — no external deps, one
baked bitmap atlas — while `braid-syntype` (vector-stroke text, its own shader, its own font
format) is the addon sibling for the same feature category. A blanket "typography is a
feature, so it's an addon" rule would misclassify `BitmapFont` and cost the sketch tier its
zero-config text primitive. `braid_roadmap.md` already states this precisely: "modularize by
dependency weight, not feature category... don't ceremony the cheap stuff."

| Layer | What lives there | Addon? |
|---|---|---|
| Platform | RGFW + Dawn (window + device) | No — floor |
| Core | `Surface`, algebra, feedback, compositor, loop | No — identity |
| Compiled addon | `Surface::load/save` (braid-image), `Syntype` text (braid-syntype) | Yes |
| Runtime addon | TinyUI (braid-ui); future audio/MIDI/OSC/publishers | Yes |

---

## 2. Two addon kinds

### 2.1 Link-to-enable compiled addons

Static libraries that add functionality by defining symbols the core declares but does not
define. Linked only by sketches that need them — the point is **dependency weight**: mango,
font stacks, codec libraries stay out of the binary unless linked. Proven: core-only hello
≈10.4 MB, addon-linked (image/feedback) ≈14.25 MB.

```cpp
// core/braid.h — declared in core, defined nowhere in core
class Surface {
public:
    static Result<Surface> load(const char* path);
    Result<void> save(const char* path) const;
};
```

```cpp
// core/braid_image.cpp — defined in the addon (mango codec stack)
Result<Surface> Surface::load(const char* path) { /* mango decode */ }
Result<void> Surface::save(const char* path) const { /* mango encode */ }
```

`braid-syntype` shows the other valid shape: a compiled addon can also add an entirely new
class (`Syntype`, `SyntypeFont`) rather than filling in a declared method.

Build integration (`chalet.yaml`, current — see §7 for where this is headed):

```yaml
braid-image:
  kind: staticLibrary
  files: { include: [core/braid_image.cpp] }

feedback:
  kind: executable
  settings:Cxx:
    staticLinks: [braid-core, braid-image, libs/macos/lib/libmango.a, ...]
```

Sketches that do not link `braid-image` never pay for mango. Reaching core internals (shared
WebGPU instance/device/queue, the compositor) goes through the narrow seam
`core/braid_detail.h` (`detail::ctx()`) — addons should not depend on RGFW or platform
internals.

### 2.2 Runtime `Addon` (frame-hooked)

Objects the sketch creates and attaches to a `Window`. They receive lifecycle hooks every
frame. This is the shape for anything that needs the **frame loop or a window surface**
(§4.4) — TinyUI is the only occupant today.

```cpp
// core/braid.h — current shape (no priority(); see §4.3)
class Addon {
public:
    virtual ~Addon() = default;
    virtual void setup(Window&) {}
    virtual void update(Window&) {}
    virtual void draw(Window&) {}
    virtual void afterDraw(Window&) {}
    virtual Surface* overlay() { return nullptr; }  // nullptr = no overlay
};

class Window {
public:
    void addAddon(Addon& addon);           // sketch owns lifetime
    void addAddon(std::shared_ptr<Addon>);  // shared ownership
};
```

```cpp
// sketch
class Sketch : public braid::SketchApp {
    braid::TinyUI ui{"layout.txt"};
public:
    using SketchApp::SketchApp;
    void setup() override { addAddon(ui); }
};
```

---

## 3. Frame lifecycle

Hooks map to real frame phases (`core/braid_app.cpp`, `Application::drawWindow` +
the update pass):

- **`setup(Window&)`** — called once, after the window/device exist. Deferred correctly
  regardless of *when* `addAddon` was called: a `setupDone_` flag on `Window` means an addon
  registered at sketch-construction time (before the window exists) and one registered on an
  already-live window both get `setup()` at the right moment. This was an open question in
  the original design ("lock the setup-timing rule before the second addon exists") — it's
  resolved in code now.
- **`update(Window&)`** — pre-draw, main thread. The natural place to drain `Channel`s.
- **`draw(Window&)`** — after the sketch's own `draw()`. The natural place for
  **contributors** that add to `mainSurface_`.
- **`afterDraw(Window&)`** — after the sketch *and* every addon's `draw()`: the frame is
  final. The natural place for **readers** of the finished frame (publishers, recorders).
- **`overlay()`** — present time, blitted over the swapchain with `Blend::Alpha` after the
  main surface blit. The natural place for **layers on top** (UI, guides, meters).

Actual loop (`Application`):

```cpp
for (Addon* a : addons_) a->update(w);        // update pass, before drawWindow
...
w.setup(); for (Addon* a : addons_) a->setup(w);  // first frame only, per-window
w.draw();
for (Addon* a : addons_) a->draw(w);
for (Addon* a : addons_) a->afterDraw(w);
// present: blit mainSurface_, then each addon->overlay() alpha-blended on top
```

**The clean-feed property** falls out of this for free: because the UI overlay never
touches `mainSurface_`, any addon reading `window.surface()` in `draw()`/`afterDraw()` gets
UI-free frames automatically. A Syphon/NDI publisher sends a clean feed to the projector
while the local window shows sliders — no deliberate FBO discipline required, unlike oF.
Protect this property in any future addon design.

---

## 4. Design rules

1. **Core stays small.** If a feature can be an addon, it is.
2. **No global addon registry.** Addons register on a `Window` or are linked explicitly.
3. **No reflection / plugin loading.** Addons are normal C++ libraries linked at build time.
4. **Addons don't see RGFW.** The windowing seam is `braid::Window`; raw RGFW stays inside
   `core/braid_app.cpp`.
5. **Window-scoped by default; app-scoped only when proven necessary.** A UI addon belongs
   to the control window because it needs that window's surface and mouse coordinates;
   output windows stay addon-free. This holds precisely because rule 6 keeps the `Addon`
   category narrow — if it didn't, "window-scoped" would be over-broad.
6. **Not everything optional is an `Addon`.** The test: *does it need the frame loop or a
   window surface?* No → plain object + `Channel<T>` (a MIDI listener, OSC receiver, Artnet
   node, GPS/lidar reader — none need `draw()` or a window; they own a thread and publish
   into a `Channel` the sketch/a bridge drains in `update`). Yes → `Addon`. Forcing
   `Channel`-fed services through the `Addon` interface would recreate oF's
   ofxFoo-is-everything category confusion.

---

## 5. User-facing vocabulary: one word — "addon"

*(direction from Dimitre, 2026-07-01)*

An addon is anything you can plug into the engine, like a peripheral into a computer: engine
at the center, addons as generalized I/O devices at the edges. For the **user** there is
exactly one word and one plug — never expose §2's two-kind split (or the 2×2 below) in user
docs or an addon catalog, the same way no one thinks about USB device classes when plugging
in a mouse.

|  | **plain object / free functions** | **runtime `Addon` (frame hooks)** |
|---|---|---|
| **always linked (core)** | Surface, Mesh, Shader, Channel | — (core has no addons by definition) |
| **link-to-enable** | braid-image, braid-syntype, future codecs/CV/physics | braid-ui (TinyUI), future Syphon/NDI publishers, recorders |

This 2×2 is *device classes* — implementer vocabulary. The user-facing catalog axis is
functional instead: an addon either **does something** (image, fonts, CV, physics, UI) or
**connects to something** (MIDI, OSC, NDI, Syphon, Artnet, sensors). It cross-cuts the 2×2
completely — "connects" contains both NDI-out (frame-hooked) and MIDI (plain + `Channel`);
"does" contains both TinyUI (frame-hooked) and CV (plain).

Consequence: the **plug must be uniform** for the metaphor to hold — same folder layout,
same one-line declaration, same include-one-header experience for every addon regardless of
internal shape. Runtime usage may differ by device class (`addAddon` a UI vs. just
constructing a Midi object); the build-time plug may not. §7 is what that plug becomes.

### 5.1 Two sub-metaphors: I/O expansion vs. interconnect (aspirational)

The peripheral metaphor has two flavors worth naming, though only the first is real today:

- **I/O expansion** — an addon that reaches outside the process: image files (disk),
  MIDI/OSC (network/serial), NDI/Syphon (video bus), audio capture. This is the addon
  mechanism as it exists now — an expansion card that adds a port.
- **Interconnect** — addons wiring directly to *each other* inside the process (an audio
  addon driving `syntype->drawDistorted()` without the sketch as the wire), the way an
  internal bus connects two cards on the same motherboard. This does not exist yet: every
  addon-to-addon link today goes through sketch code (drain a `Channel`, call
  `ui.set<T>()`). It stays aspirational, same tier as the `plugs:` manifest idea in §7.1 —
  a future patch bus, not a current API. Revisit if a real use case needs it; don't build it
  ahead of one.

---

## 6. Current addons

| Addon | Kind | Status | Notes |
|---|---|---|---|
| braid-image | compiled | ✅ done | `Surface::load/save` via mango. Still lives at `core/braid_image.cpp`, not `addons/braid-image/` — the uniform-plug housekeeping move from §7 hasn't happened yet. |
| braid-syntype | compiled | ✅ done | Stick-letter GPU text. Diverged from the original pre-implementation spec (single `.txt` file per font with named glyphs, not one file per glyph) — current docs live in `addons/braid-syntype/README.md`, not this file. Three examples: `syntype_basic`, `syntype_ui`, `syntype_audio`. |
| braid-ui (TinyUI) | compiled + runtime | ✅ M1 done | First and only `Addon` occupant. Rect-only rendering; `drawText` filled in by M2. Full design: `ui.md`. |
| braid-audio | compiled + runtime | future | likely needs a service object for capture |
| braid-midi | compiled + runtime | future | service object + `Channel` of events |
| braid-osc | compiled + runtime | future | `OscIn`/`OscOut` over `Channel<T>` |

---

## 7. Future: manifests + a generator, not hand-written chalet

*(direction from Dimitre, 2026-07-01 — not implemented; still hand-writing `chalet.yaml`)*

Addon integration should not be *based on* chalet — `chalet.yaml` becomes a **generated
artifact**, re-created from project + addon properties by a braid equivalent of **ofGen**
(`/Users/z/Dmtr/ofworks/ofGen/`). This is proven tech, not speculation: ofGen already reads a
project manifest (`of.yml`: addons, defines, frameworks, templates) and generates
`chalet.yaml` + Zed/Xcode/VSCode config for ofWorks, with local-first addon search and
recursive addon-dependency resolution. A "braidGen" is a port with a smaller job — one
platform (macOS), one build system — so v1 = `chalet.yaml` + `compile_flags.txt` + `.zed/`.

Motivation is visible in braid's current `chalet.yaml` today: the 7-archive mango block is
still copy-pasted into `feedback` and `feed` (and any future image-consuming target), and
`addons/braid-syntype` + `addons/braid-ui` includeDirs leak into the global abstract shared
by every target. That knowledge belongs to each addon, stated once.

**Sequencing gate:** "don't write braidGen until the manifest schema has survived
retrofitting the three real addons" — that condition is now met (braid-image, braid-syntype,
braid-ui all exist and build). braidGen is unblocked whenever this becomes a priority; it
just hasn't been started.

### 7.1 Per-addon manifest: `build` + `plugs`

Each addon would carry a manifest (braid's `addon_config.mk` analog):

```yaml
# addons/braid-image/addon.yml (filename TBD)
name: braid-image
kind: does            # does | connects (§5)
build:                # consumed by braidGen → chalet.yaml
  sources: [braid_image.cpp]
  staticLinks: [libmango.a, libdeflate.a, libz.a, liblcms2.a,
                libzstd.a, libbz2_static.a, liblz4.a]
plugs: []             # frame sockets it asks of braid; braid-ui: [update, overlay]
```

`plugs:` is Dimitre's node-based idea: the addon declares which internal sockets it plugs
into (update / draw / overlay / channels it publishes). Honest scope: the C++ virtual
overrides remain the *truth* for hooks; `plugs:` is the USB device descriptor — catalog
metadata, patchbay tooling (a generator can print which addons plug into which frame
phases), and a forcing function for authors to state their device class. Far-future horizon,
noted not designed: wiring declared in data (generated `addAddon` glue), the sketch's patch
as a node graph.

### 7.2 The project is an addon

ofGen's smartest move, inherited wholesale: the project manifest carries **the same schema**
as an addon manifest — sources, staticLinks, frameworks, defines, everything — plus the few
root-only fields (version, the `addons:` list, braid path). One schema, one parser; the
project is simply the root node of the dependency graph, the one that has `main()`.

### 7.3 Project layout: discover the braid path, don't assume `../../..`

Per-project folders are the real target shape, like oF's `apps/`; the monorepo `examples/` +
hand-kept `chalet.yaml` stays as the framework-development playground. What gets rethought is
oF's depth-locked `ofpath: ../../..`. braidGen would resolve the braid root by a chain:

1. `braidpath:` in the project manifest — explicit pin; doubles as version pinning;
2. `BRAID_PATH` environment variable;
3. upward marker search: walk up from the project folder for a `.braidroot` marker (the
   `.ofroot` analog), at any depth;
4. interactive prompt, like ofGen's "edit ofpath? (y)es, (n)o, (q)uit".

### 7.4 Policies to inherit from ofGen

Local-project-first addon search, recursive addon dependency resolution, CLI overrides of
manifest fields, an `import` migration path, and the golden rule: commit the manifest,
regenerate the rest.

### 7.5 Events interoperability with ofWorks (further future)

The cheap, real version is **shared event vocabulary + thin adapters**, not framework
linking. `Channel<T>` is thread-safe with main-thread pop, so an ofx addon's listener thread
can push into a braid `Channel` from a callback (~10-line bridge). Design rule when
braid-midi/braid-osc arrive: mirror ofxMidi/ofxOsc message struct fields so existing mapping
code and muscle memory port 1:1.

---

## 8. Mapping ofworks addons into braid shapes

Seeds for future addon work, from `/Users/z/Dmtr/ofworks/addons/`:

| ofworks addon | braid shape | notes |
|---|---|---|
| ofxMicroUI, ofxTinyUI, ofxImGui | runtime Addon | ✅ done: braid-ui (TinyUI) |
| ofxSyphon, ofxNDI (×7 variants!) **out** | runtime Addon, `afterDraw()` hook | best next Addon — stress-tests `draw()`/clean-feed/link-to-enable at once, opposite hook profile from TinyUI (no `overlay()`) |
| ofxNDI **in**, ofxHapPlayer, video players | link-to-enable, plain object → produces a `Surface`/texture | receiver ≠ publisher; may not need Addon-hood at all |
| ofxFFmpegRecorder / VideoRecorder / VideoWriter | runtime Addon (`afterDraw()` reads `mainSurface_`) + heavy deps → link-to-enable | same clean-feed property as publishers |
| ofxMidi, ofxMicroUIMidiController | plain object + `Channel<MidiEvent>` | bridge pattern: drain channel → `ui.set<T>()` |
| ofxOsc / ofxNetwork / ofxArtnet | plain object + `Channel` | classic §4 rule 6 services |
| ofxGPS, ofxRPlidar, ofxUVC, ofxKinect | plain object + `Channel` (or texture producer) | sensor I/O threads |
| ofxFontStash2, ofxSmartFont | link-to-enable, plain | superseded by braid-syntype (done) |
| ofxCv, ofxAssimp, ofxBox2d(_dimitre), ofxVoronoi | link-to-enable, plain algorithm/data libs | no frame hooks; Box2d maybe wants `update()` — resist it, sketch can step the world |
| ofxXmlSettings | n/a | presets already text-based |

Pattern worth noticing: almost nothing in the list needs the `Addon` interface. The
runtime-Addon category is small — UI, publishers, recorders, overlay meters — and that's
healthy. The bulk of the ecosystem is plain libraries plus `Channel`-fed services, which need
**chalet conventions** (§7), not runtime machinery.

---

## 9. Open questions

1. **The second `Addon` should be Syphon or NDI out** (§8) — still just TinyUI in the tree.
   Would stress-test `draw()` without `overlay()`, the opposite profile from TinyUI, and
   confirm the 5-hook interface generalizes past n=1.
2. **Does any addon ever need app-scoped registration**, or does the window-scoped default
   (§4 rule 5) hold everywhere? Candidate counterexample: a recorder capturing *all* windows.
3. **Rename `Addon` → `WindowAddon`?** The runtime interface class is currently named
   `Addon`, which overloads the umbrella word from §5 ("braid-midi is an addon with no
   `Addon` in it"). `window.addAddon(ui)` still reads naturally either way. Cheap to rename
   now, before a second Addon subclass exists to update.
4. **braidGen** (§7) — unblocked (three addons retrofit the shape), not yet started. Decide
   when it earns priority against other roadmap items.

---

## 10. Relation to other docs

`ui.md` is the first concrete runtime-addon design and the sole current `Addon` consumer;
this doc defines the generic mechanism, `ui.md` defines TinyUI on top of it. `braid_roadmap.md`
has the core-vs-addon architecture argument and the compiled-addon size numbers. Syntype's
implementation-level docs (font format, API, examples) live in
`addons/braid-syntype/README.md`, not here.
