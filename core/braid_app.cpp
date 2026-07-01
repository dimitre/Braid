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
// App
// ===========================================================================
App::App() : App(Settings{}) {}
App::App(const Settings& settings) : settings_(settings), timer_(settings.targetFps) {}
App::~App() {
    if (window_) RGFW_window_close(static_cast<RGFW_window*>(window_));
    RGFW_deinit();
}

int App::width() const { return settings_.width; }
int App::height() const { return settings_.height; }

wgpu::CommandEncoder& App::encoder() { return frameEncoder_; }
Surface& App::surface() { return *mainSurface_; }

void App::submitFrame() {
    wgpu::CommandBuffer cmd = frameEncoder_.Finish();
    queue_.Submit(1, &cmd);
    frameEncoder_ = device_.CreateCommandEncoder();
}

void App::setWindowTitle(const char* title) {
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

Result<void> App::initWindow() {
    RGFW_init("braid", (RGFW_initFlags)0);
    RGFW_windowFlags flags = RGFW_windowCenter | RGFW_windowFocus | RGFW_windowFocusOnShow;
    if (!settings_.resizable) flags |= RGFW_windowNoResize;
    RGFW_window* win = RGFW_createWindow(settings_.title, 0, 0, settings_.width, settings_.height,
                                         flags);
    if (!win) return Result<void>::failure("RGFW_createWindow failed");
    window_ = win;
    mousePos_ = {settings_.width * 0.5f, settings_.height * 0.5f};  // neutral until moved
    RGFW_window_setExitKey(win, RGFW_keyEscape);  // Esc quits (Cmd+Q handled in pump)
    RGFW_window_show(win);
    RGFW_window_raise(win);
    RGFW_window_focus(win);
    return Result<void>::success();
}

Result<void> App::initWebGPU() {
    wgpu::InstanceDescriptor instDesc{};
    instance_ = wgpu::CreateInstance(&instDesc);
    if (!instance_) return Result<void>::failure("wgpu::CreateInstance failed");

    // --- Surface from the Metal layer [VERIFY] ---
    void* metalLayer = attachMetalLayer(window_);
    if (!metalLayer) return Result<void>::failure("attachMetalLayer failed");
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
    configureSurface();
    // swapchain wrap (present target, 8-bit) + persistent offscreen main draw
    // target in 16-bit float (smooth feedback, HDR accumulation).
    swapSurface_.emplace(surface_, settings_.width, settings_.height, settings_.format);
    mainSurface_.emplace(device_, settings_.width, settings_.height,
                         wgpu::TextureFormat::RGBA16Float);
    // WebGPU zero-initializes textures (alpha=0). Clear to opaque black so that
    // Surface algebra (invert, pasteSelf) and save() always see alpha=1 from frame 1.
    mainSurface_->clear({0, 0, 0, 1});
    return Result<void>::success();
}

void App::configureSurface() {
    wgpu::SurfaceConfiguration config{};
    config.device = device_;
    config.format = settings_.format;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.width = static_cast<uint32_t>(settings_.width);
    config.height = static_cast<uint32_t>(settings_.height);
    config.presentMode = settings_.vsync ? wgpu::PresentMode::Fifo : wgpu::PresentMode::Immediate;
    config.alphaMode = wgpu::CompositeAlphaMode::Auto;
    surface_.Configure(&config);
}

void App::pumpEvents() {
    auto* win = static_cast<RGFW_window*>(window_);
    RGFW_event ev;
    while (RGFW_window_checkEvent(win, &ev)) {
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
                if (e.super && e.key == Key::Q) running_ = false;  // Cmd+Q quits
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

bool App::beginFrame() {
    wgpu::SurfaceTexture surfTex;
    surface_.GetCurrentTexture(&surfTex);
    if (surfTex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surfTex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        // Lost/outdated surface — reconfigure and skip this frame.
        configureSurface();
        return false;
    }
    frameView_ = surfTex.texture.CreateView();
    frameEncoder_ = device_.CreateCommandEncoder();
    swapSurface_->setCurrentView(frameView_);
    return true;
}

void App::endFrame() {
    // "The screen is the Surface you show": blit the persistent main Surface onto
    // the swapchain. This single copy is what makes screenshot/record/feedback free.
    detail::BlitUniforms u{};
    detail::compositor().blit(frameEncoder_, swapSurface_->asTexture(), settings_.format,
                              mainSurface_->asTexture(), u, Blend::None, wgpu::LoadOp::Clear);
    wgpu::CommandBuffer cmd = frameEncoder_.Finish();
    queue_.Submit(1, &cmd);
    surface_.Present();
    instance_.ProcessEvents();  // service async work (maps, device callbacks)
    frameEncoder_ = nullptr;
    frameView_ = nullptr;
}

Result<void> App::run() {
    if (auto r = initWindow(); !r) return r;
    if (auto r = initWebGPU(); !r) return r;

    setup();
    running_ = true;
    timer_.reset();

    auto* win = static_cast<RGFW_window*>(window_);
    while (running_ && !RGFW_window_shouldClose(win)) {
        timer_.waitNext();
        pumpEvents();
        update();
        if (beginFrame()) {
            beforeDraw();
            draw();
            afterDraw();
            endFrame();
        }
    }

    exit();
    return Result<void>::success();
}

void App::close() { running_ = false; }

}  // namespace braid
