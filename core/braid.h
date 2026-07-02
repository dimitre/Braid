// braid.h — Braid v0.1.0 (macOS)
// WebGPU creative coding framework. Single-include API: import this header for
// the full interface; the implementation lives in braid.cpp.
//
// Tier 1 (Explicit): Surface / Shader / Mesh — direct WebGPU control.
// Tier 2 (Sketch):   SketchApp — Processing-like facade (client-side state).
//
// Design rules (see braid_v0.1.1_critical_fixes.md):
//   - Every fallible function returns Result<T> / Result<void>. No exceptions.
//   - GPU resources are move-only; clone() makes an explicit GPU copy.
//   - Channel<T> is immobile; subscribed callbacks fire during pop() on the
//     main thread, never at push() time.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

// WebGPU clip space uses depth [0,1] (like Metal/D3D/Vulkan), not GL's [-1,1].
// Must be set before any glm include so projection matrices match — otherwise the
// near half of a perspective scene gets clipped away.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <webgpu/webgpu_cpp.h>

namespace braid {

// ---------------------------------------------------------------------------
// Fix 1 — Unified result type. valid `value` iff `ok`; valid `error` otherwise.
// ---------------------------------------------------------------------------
// `value` is engaged iff `ok`. Stored in an optional so move-only / non-default
// resource types (Surface, Mesh, …) can flow through it without a sentinel.
template <typename T>
struct Result {
    bool ok = false;
    std::optional<T> value;
    std::string error;

    explicit operator bool() const { return ok; }
    T* operator->() { return &*value; }
    const T* operator->() const { return &*value; }
    T& operator*() { return *value; }
    const T& operator*() const { return *value; }

    static Result success(T v) {
        Result r;
        r.ok = true;
        r.value.emplace(std::move(v));
        return r;
    }
    static Result failure(std::string msg) {
        Result r;
        r.ok = false;
        r.error = std::move(msg);
        return r;
    }
};

template <>
struct Result<void> {
    bool ok = false;
    std::string error;

    explicit operator bool() const { return ok; }

    static Result success() { return Result{true, {}}; }
    static Result failure(std::string msg) { return Result{false, std::move(msg)}; }
};

// ---------------------------------------------------------------------------
// Math helpers (free functions — pure, stateless, usable from either tier).
// ---------------------------------------------------------------------------
// Re-map `v` from the range [inLo, inHi] to [outLo, outHi] (the ofMap / Processing
// map). With clamp=true the result is constrained to the output range. Total: a
// zero-width input range yields outLo instead of a NaN.
inline float remap(float v, float inLo, float inHi, float outLo, float outHi,
                   bool clamp = false) {
    if (inHi == inLo) return outLo;
    float r = outLo + (v - inLo) * (outHi - outLo) / (inHi - inLo);
    if (clamp) {
        const float lo = outLo < outHi ? outLo : outHi;
        const float hi = outLo < outHi ? outHi : outLo;
        r = r < lo ? lo : (r > hi ? hi : r);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Vertex — the canonical interleaved layout for Mesh.
// ---------------------------------------------------------------------------
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec2 texCoord{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec4 color{1.0f};
};

// ---------------------------------------------------------------------------
// Fix 4 — MouseButton declared separately (no inline-enum-in-optional).
// ---------------------------------------------------------------------------
enum class MouseButton { Left, Right, Middle };

// ---------------------------------------------------------------------------
// Key — a Braid-owned keycode (the RGFW seam). Sketches compare against this,
// never a raw RGFW value, so windowing stays a swappable joint. Printable keys
// carry their ASCII value (Key::A == 'a', Key::Space == ' '); control/navigation
// keys get Braid-owned values (256+) independent of RGFW's numbering.
// For text-style input, prefer `KeyEvent::ch` (the printable char) over `key`.
// ---------------------------------------------------------------------------
enum class Key : int {
    Unknown = 0,

    Space = ' ', Apostrophe = '\'', Comma = ',', Minus = '-', Period = '.',
    Slash = '/', Semicolon = ';', Equal = '=', LeftBracket = '[',
    Backslash = '\\', RightBracket = ']', Backtick = '`',
    Num0 = '0', Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,  // contiguous
    A = 'a', B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,                              // contiguous

    Escape = 256, Enter, Tab, Backspace, Delete, Insert, Menu,
    Left, Right, Up, Down, Home, End, PageUp, PageDown,
    CapsLock, NumLock,
    Shift, Control, Alt, Super,  // either side, collapsed
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

struct KeyEvent {
    Key key = Key::Unknown;  // Braid-owned keycode (named, RGFW-independent)
    char ch = 0;             // printable character if any ('s', ' ', …), else 0
    bool pressed = false;
    bool repeat = false;
    bool shift = false, ctrl = false, alt = false, super = false;
};

struct MouseEvent {
    std::optional<MouseButton> button;  // nullopt for pure move events
    glm::vec2 pos{0.0f};
    glm::vec2 delta{0.0f};
    bool pressed = false;  // meaningful only when button.has_value()
    int clickCount = 0;
};

struct ScrollEvent {
    glm::vec2 delta{0.0f};
};

struct WindowEvent {
    enum Type { Resized, Moved, Focused, Unfocused, Closed } type = Resized;
    glm::vec2 size{0.0f};
    glm::vec2 pos{0.0f};
};

struct DropEvent {
    std::vector<std::string> paths;
};

// ---------------------------------------------------------------------------
// Fix 2 — Channel<T>: immobile, thread-safe queue. Callbacks fire during pop()
// on the calling (main) thread, so a producer on an I/O thread never triggers
// GPU work from off-main.
// ---------------------------------------------------------------------------
class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> unsub) : unsub_(std::move(unsub)) {}
    ~Subscription() { if (unsub_) unsub_(); }

    Subscription(Subscription&& o) noexcept : unsub_(std::move(o.unsub_)) { o.unsub_ = nullptr; }
    Subscription& operator=(Subscription&& o) noexcept {
        if (this != &o) { if (unsub_) unsub_(); unsub_ = std::move(o.unsub_); o.unsub_ = nullptr; }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

private:
    std::function<void()> unsub_;
};

template <typename T>
class Channel {
public:
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;       // immobile — keeps Subscription back-refs valid
    Channel& operator=(Channel&&) = delete;

    void push(T event) {
        std::lock_guard lock(mtx_);
        queue_.push(std::move(event));
    }

    // Pops one event (if any) and fires all callbacks with it on this thread.
    std::optional<T> pop() {
        std::optional<T> result;
        {
            std::lock_guard lock(mtx_);
            if (!queue_.empty()) {
                result = std::move(queue_.front());
                queue_.pop();
            }
        }
        if (result) {
            std::lock_guard lock(cbMtx_);
            for (auto& cb : callbacks_) cb.fn(*result);
        }
        return result;
    }

    bool empty() const {
        std::lock_guard lock(mtx_);
        return queue_.empty();
    }

    void clear() {
        std::lock_guard lock(mtx_);
        std::queue<T> empty;
        std::swap(queue_, empty);
    }

    Subscription subscribe(std::function<void(T)> callback) {
        std::lock_guard lock(cbMtx_);
        const uint64_t id = nextId_++;
        callbacks_.push_back({id, std::move(callback)});
        return Subscription([this, id] { removeCallback(id); });
    }

private:
    struct Entry { uint64_t id; std::function<void(T)> fn; };

    void removeCallback(uint64_t id) {
        std::lock_guard lock(cbMtx_);
        for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
            if (it->id == id) { callbacks_.erase(it); return; }
        }
    }

    std::queue<T> queue_;
    mutable std::mutex mtx_;
    std::vector<Entry> callbacks_;
    mutable std::mutex cbMtx_;
    uint64_t nextId_ = 1;
};

// ---------------------------------------------------------------------------
// Timer — direct port of ofTimerFps frame pacing (lazy sleep + tight yield).
// ---------------------------------------------------------------------------
class Timer {
public:
    explicit Timer(int targetFps = 60);

    void setFps(int fps);
    void reset();
    void waitNext();  // sleep until ~3ms before target, then spin-yield

    float deltaTime() const { return delta_; }
    float elapsedTime() const { return elapsed_; }
    int frameCount() const { return frames_; }
    int currentFps() const;

private:
    using clock = std::chrono::steady_clock;
    using nanos = std::chrono::duration<long long, std::nano>;

    nanos interval_;
    clock::time_point wakeTime_;
    clock::time_point lastWakeTime_;
    clock::time_point startTime_;
    int targetFps_;
    int frames_ = 0;
    float delta_ = 0.0f;
    float elapsed_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Blend presets (defined in braid.cpp).
// ---------------------------------------------------------------------------
namespace Blend {
extern const wgpu::BlendState Alpha;
extern const wgpu::BlendState Additive;
extern const wgpu::BlendState None;
}  // namespace Blend

// ---------------------------------------------------------------------------
// Surface — a renderable target. Backed by either an owned offscreen texture or
// the window surface (swapchain). The swapchain Surface cannot clone() (Fix 6).
// ---------------------------------------------------------------------------
class Surface {
public:
    // Offscreen render target. Default is 16-bit float: smooth feedback decay
    // (no 8-bit banding) and HDR headroom for accumulation/bloom.
    Surface(wgpu::Device device, int width, int height,
           wgpu::TextureFormat format = wgpu::TextureFormat::RGBA16Float);
    // Swapchain target (owned by App).
    Surface(wgpu::Surface surface, int width, int height, wgpu::TextureFormat format);

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    Surface(Surface&&) = default;
    Surface& operator=(Surface&&) = default;

    Result<Surface> clone() const;  // offscreen only; fails on swapchain Surface

    // Begins a render pass targeting this Surface. begin() clears; beginLoad()
    // preserves existing contents (needed for feedback / accumulation).
    wgpu::RenderPassEncoder begin(wgpu::CommandEncoder& encoder,
                                  glm::vec4 clearColor = {0, 0, 0, 1});
    wgpu::RenderPassEncoder beginLoad(wgpu::CommandEncoder& encoder);
    void end(wgpu::RenderPassEncoder& pass);

    wgpu::TextureView asTexture() const;  // current view (valid for the frame)

    // === Surface algebra (Tier: total — these never error) ===
    // Draw `src` into this Surface as a fullscreen layer (preserves our contents).
    Surface& compositeFrom(const Surface& src, const wgpu::BlendState& blend = Blend::Alpha,
                           glm::vec4 tint = {1, 1, 1, 1});
    Surface& operator+=(const Surface& src);  // additive composite (energy adds)
    Surface& over(const Surface& src);        // alpha composite (src over self)

    // === In-place transforms (Surface→Surface; the ping-pong is hidden) ===
    Surface& zoom(float factor);     // >1 magnifies toward center (tunneling in)
    Surface& rotate(float radians);  // around center
    Surface& shift(float dx, float dy);  // pixels
    Surface& invert();               // rgb → 1-rgb (snake eating its tail)
    Surface& multiply(glm::vec4 c);  // per-channel gain / tint

    // === Image-processing transforms ===
    // Separable Gaussian blur. radius in pixels; H+V in one submit.
    Surface& blur(float radius);
    // Brightpass: keeps pixels whose luma exceeds `level` (soft knee). Output alpha=1
    // so the result adds cleanly with +=. Default level=1.0 keeps HDR-only energy.
    Surface& threshold(float level = 1.0f, float knee = 0.1f);
    // bloom = clone → threshold → blur(4*passes px) → additive composite back.
    Surface& bloom(float threshold = 1.0f, float intensity = 1.0f, int passes = 5);
    // Isoline/edge detector (ported from ofworks contour2.frag): per pixel, samples
    // `mode`'s neighbor pattern at `radius` px (0: 8-neighbor +/X, 1: + only,
    // 2: X only, 3: 2-point) and draws white where the count of neighbors below
    // luma `level` crosses that pattern's cutoff — a binary contour line at the
    // level set, not a flat mask like threshold(). Replaces contents (like blur/threshold).
    Surface& contour(float level = 0.5f, float radius = 1.0f, int mode = 0);

    Surface& clear(glm::vec4 c = {0, 0, 0, 1});

    // === Self-feedback (the ouroboros) ===
    // Apply `transform` to current contents, then scale by `gain`. One surface,
    // one knob; the double-buffer underneath is invisible.
    Surface& feedback(float gain, const std::function<void(Surface&)>& transform);

    // === Placed paste (grab the frame, paste it back as a transformed quad) ===
    // Draw `src` onto an arbitrary quad of this Surface. center & size are in
    // pixels (top-left origin, like SketchApp 2D); rotation is radians about the
    // quad center. Unlike compositeFrom (always fullscreen), the quad can be
    // smaller / rotated / scaled, so the source lands as a discrete shard.
    Surface& paste(const Surface& src, glm::vec2 center, glm::vec2 size, float rotation = 0.0f,
                   const wgpu::BlendState& blend = Blend::Alpha, glm::vec4 tint = {1, 1, 1, 1},
                   bool invert = false);
    // Snapshot this Surface, then paste the snapshot back as a placed quad over
    // the current contents (preserves them ⇒ accumulates). The snapshot lives in
    // the hidden scratch buffer (no per-frame allocation). Repeat each frame with
    // a small rotation/scale for kaleidoscopic shard feedback.
    Surface& pasteSelf(glm::vec2 center, glm::vec2 size, float rotation = 0.0f,
                       const wgpu::BlendState& blend = Blend::Alpha, glm::vec4 tint = {1, 1, 1, 1},
                       bool invert = false);

    // === Image I/O (braid-image addon) ===
    // Declared here so an image is "just a Surface", but DEFINED in braid_image.cpp.
    // Link the braid-image target to enable them; without it these stay unresolved
    // (link-to-enable) and braid-core never pulls in the mango codec stack.
    //   load: decode PNG/JPG/… (mango SIMD) into a new RGBA8 Surface. Fallible.
    //   save: read back + encode by file extension (.png/.jpg/…). Fallible.
    static Result<Surface> load(const char* path);
    Result<void> save(const char* path) const;

    int width() const { return width_; }
    int height() const { return height_; }
    wgpu::TextureFormat format() const { return format_; }
    wgpu::Texture handle() const { return texture_; }  // offscreen texture
    wgpu::Device device() const { return device_; }
    bool isSwapchain() const { return swapchain_; }
    bool isValid() const;

    // Called by App each frame for the swapchain Surface.
    void setCurrentView(wgpu::TextureView view);
    void resize(int width, int height);

private:
    // Renders src (sampled, optionally transformed) into dest with the given blend.
    // dest==self → composite in place; otherwise a self-transform via scratch+swap.
    void selfTransform(glm::vec2 uvScale, glm::vec2 uvOffset, float rot, glm::vec4 tint,
                       bool invertFlag);
    void ensureScratch();
    void swapScratch();

    wgpu::Device device_;
    wgpu::Surface surface_;     // swapchain only
    wgpu::Texture texture_;     // offscreen only
    wgpu::TextureView view_;    // current frame view
    wgpu::Texture scratch_;     // hidden ping-pong buffer for self-transforms
    wgpu::TextureView scratchView_;
    int width_ = 0, height_ = 0;
    wgpu::TextureFormat format_ = wgpu::TextureFormat::RGBA8Unorm;
    bool swapchain_ = false;
};

// ---------------------------------------------------------------------------
// Shader — WGSL module + cached pipelines + uniform ring buffer (Fix 9).
// No WGSL reflection: bind slots are stated explicitly in C++.
// ---------------------------------------------------------------------------
class Shader {
public:
    struct LoadOptions {
        const char* wgsl = nullptr;
        const char* label = "shader";
        const char* vertexEntry = "vs";
        const char* fragmentEntry = "fs";
        bool debug = false;
    };

    Shader() = default;
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) = default;
    Shader& operator=(Shader&&) = default;

    static Result<Shader> load(wgpu::Device device, const LoadOptions& opts);
    static Result<Shader> loadFile(wgpu::Device device, const char* path, bool debug = false);

    // Returns (and caches) a render pipeline for the given target/vertex config.
    wgpu::RenderPipeline getPipeline(
        wgpu::VertexBufferLayout vertexLayout,
        wgpu::TextureFormat colorFormat = wgpu::TextureFormat::BGRA8Unorm,
        const wgpu::BlendState& blend = Blend::Alpha,
        wgpu::PrimitiveTopology topology = wgpu::PrimitiveTopology::TriangleList);

    // Fix 9 — uniform data is copied into a rotating 3-frame ring buffer, so the
    // returned bind group stays valid for the (max 3) frames it may be in flight.
    wgpu::BindGroup bindUniform(wgpu::RenderPipeline pipeline, int group, int binding,
                                const void* data, size_t size);

    bool isValid() const { return module_ != nullptr; }

private:
    struct UniformRing {
        std::array<wgpu::Buffer, 3> buffers{};
        std::array<size_t, 3> capacities{0, 0, 0};
        int index = 0;
        wgpu::Buffer allocate(wgpu::Device device, size_t size);
    };

    wgpu::Device device_;
    wgpu::ShaderModule module_;
    std::string vertexEntry_ = "vs";
    std::string fragmentEntry_ = "fs";

    struct PipelineKey {
        uint64_t layoutHash;
        wgpu::TextureFormat format;
        const void* blend;  // identity of the BlendState preset
        wgpu::PrimitiveTopology topology;
        bool operator==(const PipelineKey&) const;
    };
    struct PipelineKeyHash { size_t operator()(const PipelineKey&) const; };
    std::vector<std::pair<PipelineKey, wgpu::RenderPipeline>> pipelineCache_;

    UniformRing uniformRing_;
};

// ---------------------------------------------------------------------------
// Mesh — GPU vertex (+ optional index) buffers and primitive generators.
// ---------------------------------------------------------------------------
class Mesh {
public:
    explicit Mesh(wgpu::Device device);

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) = default;
    Mesh& operator=(Mesh&&) = default;

    Result<Mesh> clone() const;

    Result<void> setVertices(std::span<const Vertex> vertices);
    Result<void> setIndices(std::span<const uint32_t> indices);

    void draw(wgpu::RenderPassEncoder& pass, uint32_t instanceCount = 1);

    // Primitive generators (v0.1.0 subset).
    static Result<Mesh> plane(wgpu::Device device, float w, float h, int cols = 1, int rows = 1);
    static Result<Mesh> cube(wgpu::Device device, float size);
    static Result<Mesh> fullscreenQuad(wgpu::Device device);
    static Result<Mesh> triangle(wgpu::Device device, glm::vec3 a, glm::vec3 b, glm::vec3 c);
    static Result<Mesh> line(wgpu::Device device, std::span<const glm::vec3> points);
    static Result<Mesh> line(wgpu::Device device, std::span<const glm::vec2> points);  // Fix 5

    static wgpu::VertexBufferLayout vertexLayout();
    size_t vertexCount() const { return vertexCount_; }
    size_t indexCount() const { return indexCount_; }
    bool hasIndices() const { return indexCount_ > 0; }
    bool isValid() const { return vertexBuffer_ != nullptr; }

private:
    wgpu::Device device_;
    wgpu::Buffer vertexBuffer_;
    wgpu::Buffer indexBuffer_;
    size_t vertexCount_ = 0;
    size_t indexCount_ = 0;
    std::vector<Vertex> cpuVertices_;   // retained for clone()
    std::vector<uint32_t> cpuIndices_;
};

// ---------------------------------------------------------------------------
// Monitors — physical display geometry, in the shared virtual-desktop coordinate
// space (same space RGFW/Cocoa report window positions in). A port of the
// monitor-rect bookkeeping openFrameworks' ofAppGLFWWindow does by hand for
// multi-monitor spanning (ofMonitors::getRectFromMonitors), backed by RGFW's
// monitor API instead of GLFW's.
// ---------------------------------------------------------------------------
struct MonitorRect {
    int x = 0, y = 0, width = 0, height = 0;
};

namespace Monitors {
// All connected monitors, in RGFW's enumeration order (index N here is what
// AppSettings::monitors calls monitor N).
std::vector<MonitorRect> list();
// Bounding rect (union) of the given monitor indices — the rect a single
// borderless window must cover to visually span exactly those screens.
// Out-of-range indices are skipped; an empty/all-invalid input returns {0,0,0,0}.
MonitorRect unionOf(std::span<const int> indices);
}  // namespace Monitors

// ---------------------------------------------------------------------------
// App settings — extracted so Window (the base class) can use them before App
// is fully declared.
// ---------------------------------------------------------------------------
struct AppSettings {
    int width = 1280;
    int height = 720;
    const char* title = "Braid";
    bool resizable = true;
    bool vsync = true;
    // Native-res rendering by default (md/hidpi.md §2). false is a performance
    // hatch, not a mode: the swapchain stays at physical pixels; only mainSurface_
    // shrinks to point size, and endFrame()'s blit does the upscale inside Braid
    // (never Core Animation). Sketches and addons must not branch on this — the
    // point-space API is identical either way.
    bool hidpi = true;
    wgpu::TextureFormat format = wgpu::TextureFormat::BGRA8Unorm;
    // Frame pacing is process-wide (one shared run loop drives every window), so
    // only the primary window's targetFps has any effect in v1 — a secondary's
    // targetFps is not read. Per-window vsync *is* independent (see below).
    int targetFps = 60;
#ifdef NDEBUG
    bool enableValidation = false;  // Fix 8 — off in release
#else
    bool enableValidation = true;   // Fix 8 — on in debug
#endif

    // Explicit top-left position (virtual-desktop coords). Unset = auto-placed:
    // the primary centers on its monitor; secondaries default to the next
    // connected monitor (or cascade if there isn't one) — see Window::create().
    std::optional<glm::ivec2> position;
    // Span these monitor indices (from Monitors::list()) as one borderless
    // window covering their union rect. Overrides width/height/position and
    // implies a frameless window — the openFrameworks `fullscreenDisplays`
    // pattern, e.g. {2, 3, 4, 5} for four adjacent outputs.
    std::vector<int> monitors;
};

// ---------------------------------------------------------------------------
// Addon — window-scoped, stateful extension (UI, MIDI, OSC, ...). Addons need a
// surface and mouse coordinates, so they live on a Window, not on App (see
// braid_roadmap.md §1). Kept deliberately minimal: lifecycle hooks and an
// optional overlay layer. No ordering knob — addons run in registration order,
// and "must see the finished frame" is expressed by the afterDraw phase, not by
// out-numbering the other addons.
// ---------------------------------------------------------------------------
class Window; // forward — Addon hooks take Window&, defined below

class Addon {
public:
    virtual ~Addon() = default;
    virtual void setup(Window&) {}
    virtual void update(Window&) {}
    virtual void draw(Window&) {}

    // Runs after the sketch AND every addon's draw() — the frame is final.
    // Readers of the finished frame (recorders, publishers) belong here.
    virtual void afterDraw(Window&) {}

    // Optional overlay layer, blitted over the swapchain at present time (after
    // the mainSurface_ blit) with Blend::Alpha. nullptr = no overlay.
    virtual Surface* overlay() { return nullptr; }
};

// ---------------------------------------------------------------------------
// Window — one RGFW window + its swapchain + event pump + draw hooks.
//
// App is the backward-compatible public façade over an internal
// Application/Window split. App is itself a Window (the primary one), and it
// owns the process-wide Application that manages the shared device and the
// global run loop. Secondary windows can be spawned with App::createWindow<T>().
// ---------------------------------------------------------------------------
class Application; // internal to braid_app.cpp

class Window {
public:
    virtual ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Lifecycle hooks.
    virtual void setup() {}
    virtual void update() {}
    virtual void draw() {}
    virtual void exit() {}

    // Internal frame hooks (used by SketchApp; user code normally ignores these).
    virtual void beforeDraw() {}
    virtual void afterDraw() {}

    // Direct event callbacks (fired from the main-thread pump). The parallel
    // Channels below carry the same events for pull-style consumers.
    virtual void keyPressed(KeyEvent) {}
    virtual void keyReleased(KeyEvent) {}
    virtual void mousePressed(MouseEvent) {}
    virtual void mouseReleased(MouseEvent) {}
    virtual void mouseMoved(MouseEvent) {}
    virtual void windowResized(WindowEvent) {}

    void close();
    bool shouldClose() const;

    // Valid during draw(): the frame's command encoder and the main Surface you
    // draw into. The main Surface persists across frames (enables feedback) and is
    // blitted to the swapchain at present time — "the screen is the Surface you show."
    wgpu::CommandEncoder& encoder();
    Surface& surface();           // the main offscreen Surface — draw here
    Surface& screen() { return surface(); }  // alias

    // Submits everything recorded into encoder() so far, then starts a fresh
    // encoder for the rest of the frame. Surface algebra (bloom/blur/threshold/
    // paste/...) submits its own command buffers immediately, so it only ever
    // sees GPU work that's already been submitted — not merely recorded. Call
    // this before such a call if it needs to see drawing done earlier in this
    // same draw() (SketchApp overrides it to flush its batched primitives first).
    virtual void submitFrame();

    wgpu::Device device() const;
    wgpu::Queue queue() const;
    Timer& timer() const;

    int width() const;
    int height() const;

    // HiDPI law (md/hidpi.md §1): Window speaks logical points — width()/height(),
    // mousePos(), sketch draw coordinates. Surface speaks real pixels. pixelRatio()
    // is this window's backing scale (1.0 or 2.0 on macOS), re-queried by core on
    // every scale event; pixelWidth()/pixelHeight() = logical size × pixelRatio,
    // the drawable's physical size. These are reads for physical sizing — never
    // units the sketch must convert with.
    float pixelRatio() const;
    int pixelWidth() const;
    int pixelHeight() const;

    glm::vec2 mousePos() const;
    float mouseX() const;
    float mouseY() const;
    float deltaTime() const;
    float elapsedTime() const;
    int frameCount() const;
    int currentFps() const;

    // ofSetWindowTitle-style. Cheap; safe to call every frame (throttle for taste).
    void setWindowTitle(const char* title);

    // Registers an addon on this window. Sketch-owned addons (the common case —
    // a member field alongside the sketch) use the non-owning overload; the
    // sketch must outlive the Window, which holds for the usual "sketch IS the
    // Window" pattern. Heap-owned addons use the shared_ptr overload. Either
    // way, addon hooks run in registration order after the window's own hooks
    // each frame — see Application::run()/drawWindow() and endFrame().
    void addAddon(Addon& addon);
    void addAddon(std::shared_ptr<Addon> addon);

    // Pull-first event streams (Fix 2). Immobile; held by reference.
    Channel<KeyEvent> keyEvents;
    Channel<MouseEvent> mouseEvents;
    Channel<ScrollEvent> scrollEvents;
    Channel<WindowEvent> windowEvents;
    Channel<DropEvent> dropEvents;

protected:
    friend class Application;

    Window(Application& app, const AppSettings& settings);

    Result<void> create();
    void initSurface(wgpu::Surface surface);
    void configureSurface();
    void createSwapAndMainSurfaces();
    void adoptMetalLayer(void* layer);   // store + set contentsScale = pixelRatio_
    void resizeSurfacesToWindow();       // swapchain at pixels; mainSurface_ per hidpi flag
    bool beginFrame();
    void endFrame();

    void processEvent(const void* rgfwEvent);  // opaque RGFW_event*
    void drainChannels();

    // Releases the RGFW window and GPU-side surfaces without destroying this
    // Window object. Idempotent. Used both by ~Window and by Application::run()
    // to let the primary window close early while secondaries keep running.
    void closeNative();

    void* nativeWindow() const;
    const AppSettings& settings() const;
    bool setupDone() const;
    void markSetupDone();

    Application& app_;
    AppSettings settings_;
    glm::vec2 mousePos_{0.0f};
    // Backing scale, re-queried from NSWindow on every RGFW_scaleUpdated (never
    // accumulated, never taken from the event payload — md/hidpi.md §4).
    float pixelRatio_ = 1.0f;
    void* metalLayer_ = nullptr;  // CAMetalLayer*; kept to re-hint contentsScale on scale changes

    void* window_ = nullptr;  // RGFW_window*
    bool running_ = false;
    bool setupDone_ = false;

    wgpu::Surface surface_;
    std::optional<Surface> swapSurface_;   // wraps the swapchain (present target)
    std::optional<Surface> mainSurface_;   // persistent offscreen — draw target

    // Addons, dispatched in registration order. addons_
    // holds every addon (owning or not) for dispatch; ownedAddons_ only keeps
    // the shared_ptr overload's addons alive. Window never calls into addons_
    // during teardown (closeNative()/~Window()), so a sketch-owned addon that is
    // destroyed before this base Window (the common "sketch IS the Window" case)
    // is safe.
    std::vector<Addon*> addons_;
    std::vector<std::shared_ptr<Addon>> ownedAddons_;

    // Per-frame state, valid only between beginFrame/endFrame.
    wgpu::CommandEncoder frameEncoder_;
    wgpu::TextureView frameView_;
};

// ---------------------------------------------------------------------------
// App — backward-compatible façade: the first window + owner of the Application.
// ---------------------------------------------------------------------------
class App : public Window {
public:
    using Settings = AppSettings;

    App();  // uses default Settings
    explicit App(const Settings& settings);
    ~App() override;

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    Result<void> run();   // blocks until the last window closes

    // Spawn a secondary window sharing the same device and run loop.
    // W must derive from Window and expose a constructor (Application&, const Settings&, Args...).
    template <class W, class... Args>
    W& createWindow(const Settings& settings, Args&&... args) {
        static_assert(std::is_base_of_v<Window, W>, "createWindow requires a Window subclass");
        auto w = std::make_unique<W>(application(), settings, std::forward<Args>(args)...);
        W& ref = *w;
        adoptSecondary(std::move(w));
        return ref;
    }

    // For subclasses that are used as secondary windows (they borrow an existing Application).
    App(Application& app, const Settings& settings);

protected:
    void adoptSecondary(std::unique_ptr<Window> w);

private:
    App(const Settings& settings, std::unique_ptr<Application> app);
    Application& application();

    std::unique_ptr<Application> app_;
};

// ---------------------------------------------------------------------------
// Fix 3 — SketchApp (Tier 2). Client-side state + transform stack + a default
// shader for un-textured colored shapes. v0.1.0 ships the state model and the
// flat-2D drawing path; richer primitives/batching land in v0.1.1+.
// ---------------------------------------------------------------------------
class SketchApp : public App {
public:
    SketchApp();
    explicit SketchApp(const Settings& settings);
    // For use as a secondary window (borrows an existing Application).
    SketchApp(Application& app, const Settings& settings);

    // Color / style state (client-side until a primitive is drawn).
    void background(float r, float g, float b, float a = 1.0f);
    void background(glm::vec4 color);
    void fill(float r, float g, float b, float a = 1.0f);
    void fill(glm::vec4 color);
    void noFill();
    void stroke(float r, float g, float b, float a = 1.0f);
    void stroke(glm::vec4 color);
    void noStroke();
    void strokeWeight(float w);

    // Transform stack (client-side; independent of pipeline state).
    void pushMatrix();
    void popMatrix();
    void translate(float x, float y, float z = 0);
    void translate(glm::vec3 t);
    void rotate(float angle);                       // 2D, radians, about +Z
    void rotate(float angle, float x, float y, float z);  // 3D axis-angle
    void rotateX(float angle);                      // radians, about X/Y/Z
    void rotateY(float angle);
    void rotateZ(float angle);
    void scale(float s);
    void scale(float x, float y, float z);

    // Camera / projection.
    void camera(glm::vec3 eye, glm::vec3 center, glm::vec3 up);
    void perspective(float fov, float nearZ, float farZ);
    void ortho(float left, float right, float bottom, float top,
               float nearZ = -1, float farZ = 1);

    // 2D primitives (filled triangles via the default shader).
    void rect(float x, float y, float w, float h);
    void triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c);
    void quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d);
    void ellipse(float x, float y, float w, float h);
    void circle(float x, float y, float r);
    void line(float x1, float y1, float x2, float y2);
    void point(float x, float y);

    // 3D wireframe (lines via the default shader; needs a perspective camera).
    void box(float size);                  // wireframe cube (glutWireCube-style)
    void box(float w, float h, float d);

    // Escape hatch back to Tier 1.
    wgpu::RenderPassEncoder& pass();
    void flush();
    void submitFrame() override;  // also flushes any pending batched primitives first

protected:
    void beforeDraw() override;  // reset per-frame state, lazy init
    void afterDraw() override;   // flush the open pass

private:
    void ensureReady();  // lazy-load default shader + projection (no setup() clash)

    struct State {
        glm::vec4 fill{1, 1, 1, 1};
        glm::vec4 stroke{1, 1, 1, 1};
        bool fillEnabled = true;
        bool strokeEnabled = true;
        float strokeWeight = 1.0f;
    } state_;

    struct TransformStack {
        std::vector<glm::mat4> stack;
        glm::mat4 current{1.0f};
        void push() { stack.push_back(current); }
        void pop() { if (!stack.empty()) { current = stack.back(); stack.pop_back(); } }
    } transform_;

    glm::vec4 clearColor_{0, 0, 0, 1};
    glm::mat4 view_{1.0f};
    glm::mat4 proj_{1.0f};

    wgpu::RenderPassEncoder currentPass_;
    bool passOpen_ = false;
    bool bgRequested_ = false;  // background() called this frame → clear, else load
    bool ready_ = false;

    // --- Batching ---------------------------------------------------------
    // Primitives don't draw immediately; they append geometry to a CPU buffer
    // and record a DrawCmd. At flush() the whole frame uploads in two writes —
    // one growing vertex buffer + one uniform pool addressed by dynamic offset —
    // then replays as draws, switching pipeline only when topology changes. A
    // frame of N shapes allocates nothing in steady state (vs. a buffer + bind
    // group per primitive before).
    struct DrawCmd {
        uint32_t firstVertex = 0, vertexCount = 0;
        glm::mat4 mvp{1.0f};
        glm::vec4 tint{1.0f};
        bool lines = false;  // LineList vs TriangleList
    };
    struct SketchPipeline {
        wgpu::TextureFormat format;
        bool lines;
        wgpu::RenderPipeline pipeline;
    };
    std::vector<Vertex> batchVerts_;
    std::vector<DrawCmd> batchCmds_;
    wgpu::ShaderModule sketchModule_;
    wgpu::BindGroupLayout sketchBGL_;   // binding 0 = uniform w/ dynamic offset
    wgpu::PipelineLayout sketchPL_;
    std::vector<SketchPipeline> sketchPipelines_;
    wgpu::Buffer vbo_;  size_t vboCapacity_ = 0;  // capacity in vertices
    wgpu::Buffer ubo_;  size_t uboCapacity_ = 0;  // capacity in uniform slots
    wgpu::BindGroup uboBindGroup_;

    void drawTris(std::span<const Vertex> verts);   // append a TriangleList cmd
    void drawLines(std::span<const Vertex> verts);  // append a LineList cmd
    // Fill-only quad (no outline) — what public quad() used to be. Internal building
    // block for line()/strokeOutline() so they don't recurse through quad()'s outline.
    void fillQuad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d);
    // Stroke the edges of a polygon (in the same local space as the fill) with the
    // current stroke color/weight, as strokeWeight-thick quads — same technique line()
    // already uses for a single segment. closed=true also strokes the last→first edge.
    void strokeOutline(std::span<const glm::vec2> pts, bool closed);
    void emitBatch();                               // upload + replay into the pass
    wgpu::RenderPipeline sketchPipeline(wgpu::TextureFormat fmt, bool lines);
    void ensurePass();
};

}  // namespace braid
