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

#include <string>
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

Result<void> DeviceContext::init(void* metalLayer, const AppSettings& settings) {
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

        RGFW_init("braid", (RGFW_initFlags)0);
        rgfwInit_ = true;

        if (auto r = primary_->create(); !r) return r;

        void* metalLayer = attachMetalLayer(primary_->nativeWindow());
        if (!metalLayer) return Result<void>::failure("attachMetalLayer failed");

        auto ctx = DeviceContext::create(metalLayer, primary_->settings());
        if (!ctx) return Result<void>::failure(ctx.error);
        ctx_ = std::move(*ctx.value);

        primary_->initSurface(ctx_->takeSurface());
        primary_->configureSurface();
        primary_->createSwapAndMainSurfaces();

        timer_.setFps(primary_->settings().targetFps);
        initialized_ = true;
        return Result<void>::success();
    }

    void registerSecondary(std::unique_ptr<Window> w) {
        if (auto r = ensureInitialized(); !r) {
            fmt::print(stderr, "[braid] createWindow failed: {}\n", r.error);
            std::abort();
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
        secondaries_.push_back(std::move(w));
    }

    Result<void> run() {
        if (auto r = ensureInitialized(); !r) return r;

        running_ = true;
        timer_.reset();
        if (primary_ && !primary_->setupDone_) {
            primary_->setup();
            primary_->setupDone_ = true;
        }

        while (running_) {
            timer_.waitNext();
            RGFW_pollEvents();

            if (primary_) primary_->pumpQueued();
            for (auto& w : secondaries_) w->pumpQueued();

            if (primary_ && primary_->shouldClose()) running_ = false;
            for (auto it = secondaries_.begin(); it != secondaries_.end(); ) {
                if ((*it)->shouldClose()) it = secondaries_.erase(it);
                else ++it;
            }
            if (!primary_ && secondaries_.empty()) running_ = false;

            if (running_) {
                if (primary_) {
                    primary_->update();
                    drawWindow(*primary_);
                }
                for (auto& w : secondaries_) {
                    w->update();
                    drawWindow(*w);
                }
            }

            if (ctx_) ctx_->instance().ProcessEvents();
        }

        if (primary_) primary_->exit();
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
            w.setupDone_ = true;
        }
        if (w.beginFrame()) {
            w.beforeDraw();
            w.draw();
            w.afterDraw();
            w.endFrame();
        }
    }

    bool rgfwInit_ = false;
    bool initialized_ = false;
    bool running_ = false;
    Window* primary_ = nullptr;
    std::vector<std::unique_ptr<Window>> secondaries_;
    std::unique_ptr<DeviceContext> ctx_;
    Timer timer_;
};

// ===========================================================================
// Window method implementations
// ===========================================================================
Window::Window(Application& app, const AppSettings& settings)
    : app_(app), settings_(settings) {}

Window::~Window() {
    if (window_) {
        RGFW_window_close(static_cast<RGFW_window*>(window_));
    }
}

Result<void> Window::create() {
    RGFW_windowFlags flags = RGFW_windowCenter | RGFW_windowFocus | RGFW_windowFocusOnShow;
    if (!settings_.resizable) flags |= RGFW_windowNoResize;
    RGFW_window* win = RGFW_createWindow(settings_.title, 0, 0, settings_.width, settings_.height,
                                         flags);
    if (!win) return Result<void>::failure("RGFW_createWindow failed");
    window_ = win;
    mousePos_ = {settings_.width * 0.5f, settings_.height * 0.5f};
    RGFW_window_setExitKey(win, RGFW_keyEscape);
    RGFW_window_show(win);
    RGFW_window_raise(win);
    RGFW_window_focus(win);
    running_ = true;
    return Result<void>::success();
}

void Window::initSurface(wgpu::Surface surface) { surface_ = std::move(surface); }

void Window::configureSurface() {
    wgpu::SurfaceConfiguration config{};
    config.device = device();
    config.format = settings_.format;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.width = static_cast<uint32_t>(settings_.width);
    config.height = static_cast<uint32_t>(settings_.height);
    config.presentMode = settings_.vsync ? wgpu::PresentMode::Fifo : wgpu::PresentMode::Immediate;
    config.alphaMode = wgpu::CompositeAlphaMode::Auto;
    surface_.Configure(&config);
}

void Window::createSwapAndMainSurfaces() {
    swapSurface_.emplace(surface_, settings_.width, settings_.height, settings_.format);
    mainSurface_.emplace(device(), settings_.width, settings_.height,
                         wgpu::TextureFormat::RGBA16Float);
    // WebGPU zero-initializes textures (alpha=0). Clear to opaque black so that
    // Surface algebra (invert, pasteSelf) and save() always see alpha=1 from frame 1.
    mainSurface_->clear({0, 0, 0, 1});
}

void Window::pumpQueued() {
    auto* win = static_cast<RGFW_window*>(window_);
    RGFW_event ev;
    while (RGFW_window_checkQueuedEvent(win, &ev)) {
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
                int w = ev.update.w, h = ev.update.h;
                settings_.width = w;
                settings_.height = h;
                configureSurface();
                if (swapSurface_) swapSurface_->resize(w, h);
                if (mainSurface_) mainSurface_->resize(w, h);
                WindowEvent we{WindowEvent::Resized, {static_cast<float>(w), static_cast<float>(h)}};
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
    : Window(*app, settings), app_(std::move(app)) {}

App::App(Application& app, const Settings& settings) : Window(app, settings) {}

App::~App() {
    // Close our RGFW window *before* the Application member is destroyed,
    // because ~Application calls RGFW_deinit().
    if (window_) {
        RGFW_window_close(static_cast<RGFW_window*>(window_));
        window_ = nullptr;
    }
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
