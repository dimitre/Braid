// braid_app.cpp — App: window + device + event pump + run loop. This is the one
// TU that compiles the RGFW single-header implementation and the macOS Cocoa/Metal
// glue, so the platform specifics stay behind a single seam (the swappable joint
// for a future Windows/Linux/iOS backend).
//
// Two spots below are sensitive to the exact prebuilt Dawn / RGFW versions and
// are marked [VERIFY]:
//   1. Adapter/device request (Dawn moved this to a future+callback API).
//   2. The macOS Metal surface (CAMetalLayer attach + surface descriptor).
// Both are isolated so they can be reconciled without touching the rest.
#include "braid.h"
#include "braid_compositor.h"
#include "braid_detail.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

#include <fmt/core.h>
#include <fmt/format.h>

// RGFW: single-header windowing. Implementation compiled here, macOS/Cocoa.
#define RGFW_IMPLEMENTATION
#include <RGFW.h>

#include <objc/message.h>
#include <objc/runtime.h>

#include <webgpu/webgpu_cpp.h>

namespace braid {

// ===========================================================================
// Helpers
// ===========================================================================
// WebGPU StringViews use a WGPU_STRLEN sentinel for null-terminated strings.
static std::string sv(wgpu::StringView s) {
    if (s.data == nullptr) return {};
    if (s.length == WGPU_STRLEN) return std::string(s.data);
    return std::string(s.data, s.length);
}

// Creates a CAMetalLayer and attaches it to the RGFW window's view using RGFW's
// own Cocoa helpers (no hand-rolled objc_msgSend needed).
static void* attachMetalLayer(void* rgfwWindow) {
    auto* win = static_cast<RGFW_window*>(rgfwWindow);
    void* layer = RGFW_getLayer_OSX();  // [CAMetalLayer layer]
    if (!layer) return nullptr;
    RGFW_window_setLayer_OSX(win, layer);
    return layer;
}

// Authoritative backing scale (md/hidpi.md §4-5): NSWindow.backingScaleFactor,
// falling back to the monitor's pixelRatio. Re-queried fresh on every scale
// event — never accumulated, never taken from the RGFW_scaleUpdated payload
// (which carries a PPI-derived value, not the backing scale).
static float queryPixelRatio(void* rgfwWindow) {
    auto* win = static_cast<RGFW_window*>(rgfwWindow);
    if (id nswin = reinterpret_cast<id>(RGFW_window_getWindow_OSX(win))) {
        using MsgF = double (*)(id, SEL);
        double s = reinterpret_cast<MsgF>(objc_msgSend)(nswin, sel_registerName("backingScaleFactor"));
        if (s > 0.0) return static_cast<float>(s);
    }
    RGFW_monitor* mon = RGFW_window_getMonitor(win);
    return (mon && mon->pixelRatio > 0.0f) ? mon->pixelRatio : 1.0f;
}

// Dawn drives the layer's drawableSize from the surface configuration, but
// contentsScale should still match the backing scale so CA compositing hints
// stay honest (md/hidpi.md §4).
static void setLayerContentsScale(void* layer, float scale) {
    using MsgD = void (*)(id, SEL, double);
    reinterpret_cast<MsgD>(objc_msgSend)(reinterpret_cast<id>(layer),
                                         sel_registerName("setContentsScale:"),
                                         static_cast<double>(scale));
}

// RGFW mouse button order (Left, Middle, Right) -> braid (Left, Right, Middle).
static std::optional<MouseButton> mapButton(RGFW_mouseButton b) {
    switch (b) {
        case RGFW_mouseLeft: return MouseButton::Left;
        case RGFW_mouseMiddle: return MouseButton::Middle;
        case RGFW_mouseRight: return MouseButton::Right;
        default: return std::nullopt;
    }
}

// RGFW keycode -> braid::Key (the windowing seam: nothing above sees an RGFW value).
// RGFW already gives printable keys their ASCII value, and braid::Key matches that,
// so printables pass straight through; only control/navigation keys are remapped.
static Key mapKey(RGFW_key v) {
    switch (v) {
        case RGFW_keyEscape:    return Key::Escape;
        case RGFW_keyReturn:    return Key::Enter;     // == RGFW_keyEnter
        case RGFW_keyTab:       return Key::Tab;
        case RGFW_keyBackSpace: return Key::Backspace;
        case RGFW_keyDelete:    return Key::Delete;
        case RGFW_keyInsert:    return Key::Insert;
        case RGFW_keyMenu:      return Key::Menu;
        case RGFW_keyLeft:      return Key::Left;
        case RGFW_keyRight:     return Key::Right;
        case RGFW_keyUp:        return Key::Up;
        case RGFW_keyDown:      return Key::Down;
        case RGFW_keyHome:      return Key::Home;
        case RGFW_keyEnd:       return Key::End;
        case RGFW_keyPageUp:    return Key::PageUp;
        case RGFW_keyPageDown:  return Key::PageDown;
        case RGFW_keyCapsLock:  return Key::CapsLock;
        case RGFW_keyNumLock:   return Key::NumLock;
        case RGFW_keyShiftL:    case RGFW_keyShiftR:   return Key::Shift;
        case RGFW_keyControlL:  case RGFW_keyControlR: return Key::Control;
        case RGFW_keyAltL:      case RGFW_keyAltR:     return Key::Alt;
        case RGFW_keySuperL:    case RGFW_keySuperR:   return Key::Super;
        case RGFW_keyF1:  return Key::F1;   case RGFW_keyF2:  return Key::F2;
        case RGFW_keyF3:  return Key::F3;   case RGFW_keyF4:  return Key::F4;
        case RGFW_keyF5:  return Key::F5;   case RGFW_keyF6:  return Key::F6;
        case RGFW_keyF7:  return Key::F7;   case RGFW_keyF8:  return Key::F8;
        case RGFW_keyF9:  return Key::F9;   case RGFW_keyF10: return Key::F10;
        case RGFW_keyF11: return Key::F11;  case RGFW_keyF12: return Key::F12;
        default: break;
    }
    if (v >= 32 && v < 127) return static_cast<Key>(v);  // printable ASCII passthrough
    return Key::Unknown;
}

// The printable character a key produces ('s', ' ', …), or 0 for control keys.
static char printableChar(RGFW_key v) {
    return (v >= 32 && v < 127) ? static_cast<char>(v) : 0;
}

// RGFW is a process-wide singleton (see braid_multiwindow.md §1): init is safe
// to call more than once as long as every call is idempotent. Application::
// ensureInitialized() is the "real" owner (it pairs this with RGFW_deinit()),
// but Monitors::list() must also work *before* any window exists — e.g. to
// decide AppSettings::monitors for the very first window being created — so
// both funnel through this shared, idempotent guard instead of each calling
// RGFW_init() directly.
static void ensureRGFWReady() {
    static bool inited = false;
    if (!inited) {
        RGFW_init("braid", (RGFW_initFlags)0);
        inited = true;
    }
}

// ===========================================================================
// Monitors — see braid.h. RGFW_getMonitors returns pointers into RGFW's own
// monitor list (only the outer array we get back is ours to free).
// ===========================================================================
namespace Monitors {

std::vector<MonitorRect> list() {
    ensureRGFWReady();
    std::vector<MonitorRect> out;
    size_t count = 0;
    RGFW_monitor** mons = RGFW_getMonitors(&count);
    if (!mons) return out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        RGFW_monitor* m = mons[i];
        out.push_back({m->x, m->y, m->mode.w, m->mode.h});
    }
    RGFW_FREE(mons);
    return out;
}

MonitorRect unionOf(std::span<const int> indices) {
    std::vector<MonitorRect> mons = list();
    MonitorRect r{};
    bool first = true;
    for (int i : indices) {
        if (i < 0 || static_cast<size_t>(i) >= mons.size()) continue;
        const MonitorRect& m = mons[static_cast<size_t>(i)];
        if (first) {
            r = m;
            first = false;
            continue;
        }
        int left = std::min(r.x, m.x);
        int top = std::min(r.y, m.y);
        int right = std::max(r.x + r.width, m.x + m.width);
        int bottom = std::max(r.y + r.height, m.y + m.height);
        r = {left, top, right - left, bottom - top};
    }
    return r;
}

}  // namespace Monitors

// ===========================================================================
// Forward declarations (defined later in this TU)
// ===========================================================================
class Application;

// ===========================================================================
// DeviceContext — one instance + adapter + device + queue for the process.
// ===========================================================================
class DeviceContext {
public:
    static Result<std::unique_ptr<DeviceContext>> create(void* metalLayer,
                                                         const AppSettings& settings);

    wgpu::Instance instance() const { return instance_; }
    wgpu::Device device() const { return device_; }
    wgpu::Queue queue() const { return queue_; }

    // The surface that was used to request the adapter. Handed to the primary window.
    wgpu::Surface takeSurface() { return std::move(surface_); }

private:
    DeviceContext() = default;
    Result<void> init(void* metalLayer, const AppSettings& settings);

    wgpu::Instance instance_;
    wgpu::Adapter adapter_;
    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::Surface surface_;
};

Result<std::unique_ptr<DeviceContext>> DeviceContext::create(void* metalLayer,
                                                             const AppSettings& settings) {
    auto ctx = std::unique_ptr<DeviceContext>(new DeviceContext());
    if (auto r = ctx->init(metalLayer, settings); !r)
        return Result<std::unique_ptr<DeviceContext>>::failure(r.error);
    return Result<std::unique_ptr<DeviceContext>>::success(std::move(ctx));
}

Result<void> DeviceContext::init(void* metalLayer, [[maybe_unused]] const AppSettings& settings) {
    wgpu::InstanceDescriptor instDesc{};
    instance_ = wgpu::CreateInstance(&instDesc);
    if (!instance_) return Result<void>::failure("wgpu::CreateInstance failed");

    // --- Surface from the Metal layer [VERIFY] ---
    wgpu::SurfaceSourceMetalLayer metalSrc{};
    metalSrc.layer = metalLayer;
    wgpu::SurfaceDescriptor surfDesc{};
    surfDesc.nextInChain = &metalSrc;
    surface_ = instance_.CreateSurface(&surfDesc);
    if (!surface_) return Result<void>::failure("CreateSurface failed");

    // --- Adapter request (future + ProcessEvents pump) ---
    wgpu::RequestAdapterOptions adapterOpts{};
    adapterOpts.compatibleSurface = surface_;
    adapterOpts.powerPreference = wgpu::PowerPreference::HighPerformance;

    std::string requestError;
    bool adapterDone = false;
    instance_.RequestAdapter(
        &adapterOpts, wgpu::CallbackMode::AllowProcessEvents,
        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView message) {
            if (status == wgpu::RequestAdapterStatus::Success) adapter_ = std::move(a);
            else requestError = sv(message);
            adapterDone = true;
        });
    while (!adapterDone) instance_.ProcessEvents();
    if (!adapter_) return Result<void>::failure("RequestAdapter: " + requestError);

    // --- Device request ---
    wgpu::DeviceDescriptor devDesc{};
    devDesc.label = "braid-device";
    devDesc.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
            fmt::print(stderr, "[braid] WebGPU error ({}): {}\n", static_cast<int>(type), sv(msg));
        });
    devDesc.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowProcessEvents,
        [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
            if (reason != wgpu::DeviceLostReason::Destroyed)
                fmt::print(stderr, "[braid] device lost: {}\n", sv(msg));
        });

    bool deviceDone = false;
    adapter_.RequestDevice(
        &devDesc, wgpu::CallbackMode::AllowProcessEvents,
        [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView message) {
            if (status == wgpu::RequestDeviceStatus::Success) device_ = std::move(d);
            else requestError = sv(message);
            deviceDone = true;
        });
    while (!deviceDone) instance_.ProcessEvents();
    if (!device_) return Result<void>::failure("RequestDevice: " + requestError);

    queue_ = device_.GetQueue();
    detail::setContext(instance_, device_);
    return Result<void>::success();
}

// ===========================================================================
// Application — process-wide: RGFW init/deinit, shared device, global run loop.
// ===========================================================================
class Application {
public:
    Application() = default;
    ~Application() {
        windowMap_.clear();
        // Destroy secondary windows (and their surfaces) before the device.
        secondaries_.clear();
        ctx_.reset();
        if (rgfwInit_) RGFW_deinit();
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void setPrimary(Window* primary) { primary_ = primary; }

    Result<void> ensureInitialized() {
        if (initialized_) return Result<void>::success();
        if (!primary_) return Result<void>::failure("no primary window");

        ensureRGFWReady();
        rgfwInit_ = true;

        if (auto r = primary_->create(); !r) return r;

        void* metalLayer = attachMetalLayer(primary_->nativeWindow());
        if (!metalLayer) return Result<void>::failure("attachMetalLayer failed");
        primary_->adoptMetalLayer(metalLayer);

        auto ctx = DeviceContext::create(metalLayer, primary_->settings());
        if (!ctx) return Result<void>::failure(ctx.error);
        ctx_ = std::move(*ctx.value);

        primary_->initSurface(ctx_->takeSurface());
        primary_->configureSurface();
        primary_->createSwapAndMainSurfaces();
        windowMap_[static_cast<RGFW_window*>(primary_->nativeWindow())] = primary_;

        timer_.setFps(primary_->settings().targetFps);
        initialized_ = true;
        return Result<void>::success();
    }

    void registerSecondary(std::unique_ptr<Window> w) {
        if (auto r = ensureInitialized(); !r) {
            fmt::print(stderr, "[braid] createWindow failed: {}\n", r.error);
            std::abort();
        }

        // A secondary with no explicit position/monitors would otherwise land
        // centered on the same screen as the primary, directly overlapping it.
        // Default it onto the next connected monitor (typical rig: primary on
        // the laptop panel, secondaries on external displays); fall back to a
        // simple cascade if there isn't one.
        if (w->settings_.monitors.empty() && !w->settings_.position) {
            std::vector<MonitorRect> mons = Monitors::list();
            size_t idx = secondaries_.size() + 1;  // monitor 0 is presumed the primary's
            if (idx < mons.size()) {
                const MonitorRect& m = mons[idx];
                w->settings_.position = glm::ivec2{
                    m.x + (m.width - w->settings_.width) / 2,
                    m.y + (m.height - w->settings_.height) / 2};
            } else {
                int n = static_cast<int>(secondaries_.size());
                w->settings_.position = glm::ivec2{120 + n * 48, 120 + n * 48};
            }
        }

        if (auto r = w->create(); !r) {
            fmt::print(stderr, "[braid] createWindow failed: {}\n", r.error);
            std::abort();
        }
        void* layer = attachMetalLayer(w->nativeWindow());
        if (!layer) {
            fmt::print(stderr, "[braid] createWindow: attachMetalLayer failed\n");
            std::abort();
        }
        w->adoptMetalLayer(layer);
        wgpu::SurfaceSourceMetalLayer metalSrc{};
        metalSrc.layer = layer;
        wgpu::SurfaceDescriptor surfDesc{};
        surfDesc.nextInChain = &metalSrc;
        wgpu::Surface surface = ctx_->instance().CreateSurface(&surfDesc);
        if (!surface) {
            fmt::print(stderr, "[braid] createWindow: CreateSurface failed\n");
            std::abort();
        }
        w->initSurface(std::move(surface));
        w->configureSurface();
        w->createSwapAndMainSurfaces();
        Window* ptr = w.get();
        secondaries_.push_back(std::move(w));
        windowMap_[static_cast<RGFW_window*>(ptr->nativeWindow())] = ptr;
    }

    Result<void> run() {
        if (auto r = ensureInitialized(); !r) return r;

        running_ = true;
        timer_.reset();
        if (primary_ && !primary_->setupDone_) {
            primary_->setup();
            for (Addon* a : primary_->addons_) a->setup(*primary_);
            primary_->setupDone_ = true;
        }

        while (running_) {
            timer_.waitNext();
            RGFW_pollEvents();

            // Drain the shared RGFW queue once, then dispatch each event to the
            // correct window by its RGFW_window*. This avoids the event-loss bug in
            // RGFW_window_checkQueuedEvent when it scans past non-matching events.
            std::vector<RGFW_event> events;
            RGFW_event ev;
            while (RGFW_checkQueuedEvent(&ev)) {
                events.push_back(ev);
            }
            for (const auto& e : events) {
                if (e.common.win == nullptr) continue;
                auto it = windowMap_.find(e.common.win);
                if (it != windowMap_.end()) it->second->processEvent(&e);
            }

            if (primary_ && !primaryClosed_) primary_->drainChannels();
            for (auto& w : secondaries_) w->drainChannels();

            // Any window — primary or secondary — can close independently; the
            // app only ends once none are left. The primary's C++ object is
            // caller-owned (typically the App on main()'s stack), so it can't be
            // erased like a secondary: release its native window/surfaces via
            // closeNative() and just stop touching it.
            if (primary_ && !primaryClosed_ && primary_->shouldClose()) {
                windowMap_.erase(static_cast<RGFW_window*>(primary_->nativeWindow()));
                primary_->exit();
                primary_->closeNative();
                primaryClosed_ = true;
            }
            for (auto it = secondaries_.begin(); it != secondaries_.end(); ) {
                if ((*it)->shouldClose()) {
                    windowMap_.erase(static_cast<RGFW_window*>((*it)->nativeWindow()));
                    (*it)->exit();
                    it = secondaries_.erase(it);
                } else ++it;
            }

            bool anyOpen = (primary_ && !primaryClosed_) || !secondaries_.empty();
            if (!anyOpen) running_ = false;

            if (running_) {
                if (primary_ && !primaryClosed_) {
                    primary_->update();
                    for (Addon* a : primary_->addons_) a->update(*primary_);
                    drawWindow(*primary_);
                }
                for (auto& w : secondaries_) {
                    w->update();
                    for (Addon* a : w->addons_) a->update(*w);
                    drawWindow(*w);
                }
            }

            if (ctx_) ctx_->instance().ProcessEvents();
        }

        if (primary_ && !primaryClosed_) primary_->exit();
        return Result<void>::success();
    }

    void stop() { running_ = false; }

    wgpu::Instance instance() const { return ctx_ ? ctx_->instance() : nullptr; }
    wgpu::Device device() const { return ctx_ ? ctx_->device() : nullptr; }
    wgpu::Queue queue() const { return ctx_ ? ctx_->queue() : nullptr; }
    Timer& timer() { return timer_; }

private:
    void drawWindow(Window& w) {
        if (w.shouldClose()) return;
        if (!w.setupDone_) {
            w.setup();
            for (Addon* a : w.addons_) a->setup(w);
            w.setupDone_ = true;
        }
        if (w.beginFrame()) {
            w.beforeDraw();
            w.draw();
            w.afterDraw();
            for (Addon* a : w.addons_) a->draw(w);
            for (Addon* a : w.addons_) a->afterDraw(w);
            w.endFrame();
        }
    }

    bool rgfwInit_ = false;
    bool initialized_ = false;
    bool running_ = false;
    bool primaryClosed_ = false;
    Window* primary_ = nullptr;
    std::vector<std::unique_ptr<Window>> secondaries_;
    std::unordered_map<RGFW_window*, Window*> windowMap_;
    std::unique_ptr<DeviceContext> ctx_;
    Timer timer_;
};

// ===========================================================================
// Window method implementations
// ===========================================================================
Window::Window(Application& app, const AppSettings& settings)
    : app_(app), settings_(settings) {}

Window::~Window() { closeNative(); }

Result<void> Window::create() {
    RGFW_windowFlags flags = RGFW_windowFocus | RGFW_windowFocusOnShow;
    if (!settings_.resizable) flags |= RGFW_windowNoResize;

    int x = 0, y = 0;
    int w = settings_.width, h = settings_.height;

    if (!settings_.monitors.empty()) {
        // Span the union of the requested monitors as one borderless window —
        // the openFrameworks `fullscreenDisplays` pattern.
        MonitorRect r = Monitors::unionOf(settings_.monitors);
        if (r.width > 0 && r.height > 0) {
            x = r.x;
            y = r.y;
            w = r.width;
            h = r.height;
            settings_.width = w;
            settings_.height = h;
            flags |= RGFW_windowNoBorder;
        } else {
            flags |= RGFW_windowCenter;  // requested monitors don't exist; fall back
        }
    } else if (settings_.position) {
        x = settings_.position->x;
        y = settings_.position->y;
    } else {
        flags |= RGFW_windowCenter;
    }

    RGFW_window* win = RGFW_createWindow(settings_.title, x, y, w, h, flags);
    if (!win) return Result<void>::failure("RGFW_createWindow failed");
    window_ = win;
    // The OS can silently clamp the actual window (e.g. a requested height taller
    // than the screen's usable area) without RGFW telling us via a resize event —
    // win->w/win->h already reflect the real, possibly-clamped result right after
    // creation. Reconcile settings_ to match *before* the swapchain is configured
    // from it, otherwise the swapchain/mainSurface_ get sized for the requested
    // (wrong) canvas while the real Metal layer is smaller, and the whole frame
    // renders squeezed/distorted to fit.
    settings_.width = win->w;
    settings_.height = win->h;
    // Backing scale next: everything physical (drawable, mainSurface_) sizes off
    // it, while settings_/mousePos_ stay in logical points (md/hidpi.md §1).
    pixelRatio_ = queryPixelRatio(win);
    fmt::print(stderr, "[braid] window {}x{} pt @ {:g}x -> drawable {}x{} px\n",
               settings_.width, settings_.height, pixelRatio_, pixelWidth(), pixelHeight());
    mousePos_ = {settings_.width * 0.5f, settings_.height * 0.5f};
    RGFW_window_setExitKey(win, RGFW_keyEscape);
    RGFW_window_show(win);
    RGFW_window_raise(win);
    RGFW_window_focus(win);
    running_ = true;
    return Result<void>::success();
}

void Window::closeNative() {
    if (window_) {
        RGFW_window_close(static_cast<RGFW_window*>(window_));
        window_ = nullptr;
    }
    // Release GPU-side surfaces here (rather than letting them fall out as base-class
    // members, which — for the primary window, whose Application is a *derived*
    // member of App — would otherwise be destroyed after the device itself).
    swapSurface_.reset();
    mainSurface_.reset();
    surface_ = nullptr;
    running_ = false;
}

void Window::initSurface(wgpu::Surface surface) { surface_ = std::move(surface); }

void Window::adoptMetalLayer(void* layer) {
    metalLayer_ = layer;
    setLayerContentsScale(layer, pixelRatio_);
}

void Window::resizeSurfacesToWindow() {
    configureSurface();
    if (swapSurface_) swapSurface_->resize(pixelWidth(), pixelHeight());
    if (mainSurface_) mainSurface_->resize(settings_.hidpi ? pixelWidth() : settings_.width,
                                           settings_.hidpi ? pixelHeight() : settings_.height);
}

void Window::configureSurface() {
    wgpu::SurfaceConfiguration config{};
    config.device = device();
    config.format = settings_.format;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    // The drawable is always physical pixels (md/hidpi.md §2): hidpi=false only
    // shrinks mainSurface_, so the upscale is endFrame()'s blit — never CA.
    config.width = static_cast<uint32_t>(pixelWidth());
    config.height = static_cast<uint32_t>(pixelHeight());
    config.presentMode = settings_.vsync ? wgpu::PresentMode::Fifo : wgpu::PresentMode::Immediate;
    config.alphaMode = wgpu::CompositeAlphaMode::Auto;
    surface_.Configure(&config);
}

void Window::createSwapAndMainSurfaces() {
    swapSurface_.emplace(surface_, pixelWidth(), pixelHeight(), settings_.format);
    const int mw = settings_.hidpi ? pixelWidth() : settings_.width;
    const int mh = settings_.hidpi ? pixelHeight() : settings_.height;
    mainSurface_.emplace(device(), mw, mh, wgpu::TextureFormat::RGBA16Float);
    // WebGPU zero-initializes textures (alpha=0). Clear to opaque black so that
    // Surface algebra (invert, pasteSelf) and save() always see alpha=1 from frame 1.
    mainSurface_->clear({0, 0, 0, 1});
}

void Window::processEvent(const void* rgfwEvent) {
    const RGFW_event& ev = *static_cast<const RGFW_event*>(rgfwEvent);
    switch (ev.type) {
        case RGFW_keyPressed: {
            KeyEvent e{};
            e.key = mapKey(ev.key.value);
            e.ch = printableChar(ev.key.value);
            e.pressed = true;
            e.repeat = ev.key.repeat != 0;
            e.shift = (ev.key.mod & RGFW_modShift) != 0;
            e.ctrl = (ev.key.mod & RGFW_modControl) != 0;
            e.alt = (ev.key.mod & RGFW_modAlt) != 0;
            e.super = (ev.key.mod & RGFW_modSuper) != 0;
            if (e.super && e.key == Key::Q) running_ = false;
            keyEvents.push(e);
            keyPressed(e);
            break;
        }
        case RGFW_keyReleased: {
            KeyEvent e{};
            e.key = mapKey(ev.key.value);
            e.ch = printableChar(ev.key.value);
            e.shift = (ev.key.mod & RGFW_modShift) != 0;
            e.ctrl = (ev.key.mod & RGFW_modControl) != 0;
            e.alt = (ev.key.mod & RGFW_modAlt) != 0;
            e.super = (ev.key.mod & RGFW_modSuper) != 0;
            keyEvents.push(e);
            keyReleased(e);
            break;
        }
        case RGFW_mouseButtonPressed: {
            MouseEvent e{};
            e.button = mapButton(ev.button.value);
            e.pos = mousePos_;  // button events carry no position; use latest
            e.pressed = true;
            mouseEvents.push(e);
            mousePressed(e);
            break;
        }
        case RGFW_mouseButtonReleased: {
            MouseEvent e{};
            e.button = mapButton(ev.button.value);
            e.pos = mousePos_;
            e.pressed = false;
            mouseEvents.push(e);
            mouseReleased(e);
            break;
        }
        case RGFW_mouseMotion: {
            MouseEvent e{};
            e.pos = {static_cast<float>(ev.mouse.x), static_cast<float>(ev.mouse.y)};
            e.delta = e.pos - mousePos_;
            mousePos_ = e.pos;
            mouseEvents.push(e);
            mouseMoved(e);
            break;
        }
        case RGFW_mouseScroll: {
            ScrollEvent e{{ev.delta.x, ev.delta.y}};
            scrollEvents.push(e);
            break;
        }
        case RGFW_windowResized: {
            settings_.width = ev.update.w;   // points — the Window API unit (md/hidpi.md §1)
            settings_.height = ev.update.h;
            resizeSurfacesToWindow();
            WindowEvent we{WindowEvent::Resized,
                           {static_cast<float>(settings_.width), static_cast<float>(settings_.height)}};
            windowEvents.push(we);
            windowResized(we);
            break;
        }
        case RGFW_scaleUpdated: {
            // Signal only — the payload is PPI-derived, not the backing scale
            // (md/hidpi.md §4). Re-query the authoritative source instead.
            float pr = queryPixelRatio(window_);
            if (pr == pixelRatio_) break;
            pixelRatio_ = pr;
            if (metalLayer_) setLayerContentsScale(metalLayer_, pr);
            resizeSurfacesToWindow();
            // Point size is unchanged but every backing surface reallocated;
            // sketches and addons already react to Resized, so reuse it.
            WindowEvent we{WindowEvent::Resized,
                           {static_cast<float>(settings_.width), static_cast<float>(settings_.height)}};
            windowEvents.push(we);
            windowResized(we);
            break;
        }
        case RGFW_windowClose:
            running_ = false;
            break;
        default:
            break;
    }
}

void Window::drainChannels() {
    // Drain channels so subscribed callbacks fire on this (main) thread.
    while (keyEvents.pop()) {}
    while (mouseEvents.pop()) {}
    while (scrollEvents.pop()) {}
    while (windowEvents.pop()) {}
    while (dropEvents.pop()) {}
}

bool Window::beginFrame() {
    wgpu::SurfaceTexture surfTex;
    surface_.GetCurrentTexture(&surfTex);
    if (surfTex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surfTex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        // Lost/outdated surface — reconfigure and skip this frame.
        configureSurface();
        return false;
    }
    frameView_ = surfTex.texture.CreateView();
    frameEncoder_ = device().CreateCommandEncoder();
    swapSurface_->setCurrentView(frameView_);
    return true;
}

void Window::endFrame() {
    // "The screen is the Surface you show": blit the persistent main Surface onto
    // the swapchain. This single copy is what makes screenshot/record/feedback free.
    detail::BlitUniforms u{};
    detail::compositor().blit(frameEncoder_, swapSurface_->asTexture(), settings_.format,
                              mainSurface_->asTexture(), u, Blend::None, wgpu::LoadOp::Clear);
    // Addon overlays (e.g. TinyUI) land on top, alpha-blended, after the single
    // mainSurface_ copy — mainSurface_ itself never contains them, so screenshot/
    // record/feedback stay clean unless a sketch explicitly composites one in.
    for (Addon* addon : addons_) {
        if (Surface* ov = addon->overlay()) {
            detail::compositor().blit(frameEncoder_, swapSurface_->asTexture(), settings_.format,
                                      ov->asTexture(), u, Blend::Alpha, wgpu::LoadOp::Load);
        }
    }
    wgpu::CommandBuffer cmd = frameEncoder_.Finish();
    queue().Submit(1, &cmd);
    surface_.Present();
    frameEncoder_ = nullptr;
    frameView_ = nullptr;
}

void Window::submitFrame() {
    wgpu::CommandBuffer cmd = frameEncoder_.Finish();
    queue().Submit(1, &cmd);
    frameEncoder_ = device().CreateCommandEncoder();
}

void Window::addAddon(Addon& addon) {
    addons_.push_back(&addon);
}

void Window::addAddon(std::shared_ptr<Addon> addon) {
    addons_.push_back(addon.get());
    ownedAddons_.push_back(std::move(addon));
}

void Window::setWindowTitle(const char* title) {
    if (!window_) return;
    // Set the NSWindow title under our own autorelease pool. RGFW_window_setName
    // autoreleases the NSString into RGFW's event pool, and calling it from draw()
    // (outside that pool's scope) corrupts the balance and trips shouldClose.
    auto* win = static_cast<RGFW_window*>(window_);
    using Msg = id (*)(id, SEL);
    using MsgStr = id (*)(id, SEL, const char*);
    using MsgId = void (*)(id, SEL, id);
    id pool = reinterpret_cast<Msg>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSAutoreleasePool")), sel_registerName("alloc"));
    pool = reinterpret_cast<Msg>(objc_msgSend)(pool, sel_registerName("init"));
    id str = reinterpret_cast<MsgStr>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSString")), sel_registerName("stringWithUTF8String:"),
        title);
    reinterpret_cast<MsgId>(objc_msgSend)(reinterpret_cast<id>(win->src.window),
                                          sel_registerName("setTitle:"), str);
    reinterpret_cast<Msg>(objc_msgSend)(pool, sel_registerName("drain"));
}

bool Window::shouldClose() const {
    return !running_ || (window_ && RGFW_window_shouldClose(static_cast<RGFW_window*>(window_)));
}

void Window::close() { running_ = false; }

wgpu::Device Window::device() const { return app_.device(); }
wgpu::Queue Window::queue() const { return app_.queue(); }
Timer& Window::timer() const { return app_.timer(); }
int Window::width() const { return settings_.width; }
int Window::height() const { return settings_.height; }
float Window::pixelRatio() const { return pixelRatio_; }
int Window::pixelWidth() const { return static_cast<int>(std::lround(settings_.width * pixelRatio_)); }
int Window::pixelHeight() const { return static_cast<int>(std::lround(settings_.height * pixelRatio_)); }
glm::vec2 Window::mousePos() const { return mousePos_; }
float Window::mouseX() const { return mousePos_.x; }
float Window::mouseY() const { return mousePos_.y; }
float Window::deltaTime() const { return timer().deltaTime(); }
float Window::elapsedTime() const { return timer().elapsedTime(); }
int Window::frameCount() const { return timer().frameCount(); }
int Window::currentFps() const { return timer().currentFps(); }
wgpu::CommandEncoder& Window::encoder() { return frameEncoder_; }
Surface& Window::surface() { return *mainSurface_; }

void* Window::nativeWindow() const { return window_; }
const AppSettings& Window::settings() const { return settings_; }
bool Window::setupDone() const { return setupDone_; }
void Window::markSetupDone() { setupDone_ = true; }

// ===========================================================================
// App — backward-compatible façade: the first window + owner of the Application.
// ===========================================================================
App::App() : App(Settings{}) {}

App::App(const Settings& settings) : App(settings, std::make_unique<Application>()) {}

App::App(const Settings& settings, std::unique_ptr<Application> app)
    : Window(*app, settings), app_(std::move(app)) {
    // Register as the primary window now so createWindow() can be called before run().
    application().setPrimary(this);
}

App::App(Application& app, const Settings& settings) : Window(app, settings) {}

App::~App() {
    // Close the window and release its surfaces *before* the app_ member is
    // destroyed: app_ (Application) is a *derived*-class member, so it would
    // otherwise be torn down (device included, via ~Application) before this
    // base Window's own surface members get destroyed. closeNative() is
    // idempotent, so this is a no-op if Application::run() already closed us.
    closeNative();
}

Application& App::application() { return *app_; }

Result<void> App::run() {
    application().setPrimary(this);
    return application().run();
}

void App::adoptSecondary(std::unique_ptr<Window> w) {
    application().registerSecondary(std::move(w));
}

}  // namespace braid
