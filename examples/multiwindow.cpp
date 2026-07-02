// multiwindow.cpp — one big offscreen canvas, drawn once per frame and shown
// in two windows off one shared WebGPU device: a small aspect-correct preview
// on the primary (control) window, and a second window that spans monitors
// {1, 2} side by side as one wide, borderless, native-resolution output once
// that rig is connected — falling back to a plain window at the same aspect
// ratio so the example still runs meaningfully on a single-monitor machine.
#include <cstdio>
#include <vector>

#include "braid.h"

namespace {

// Two 1920x1080 outputs side by side. Independent of whatever the real
// monitors turn out to be — Surface::compositeFrom() scales to fit either way.
constexpr int kCanvasWidth = 3840;
constexpr int kCanvasHeight = 1080;
constexpr int kPreviewScale = 4;    // preview window = canvas / this, same aspect
constexpr int kFallbackScale = 3;   // output window size when {1,2} aren't connected

// Owns the shared canvas and advances its one animation step per frame. Built
// in main() once the shared device exists (after the first createWindow()).
struct BigScene {
    braid::Surface canvas;
    explicit BigScene(wgpu::Device device) : canvas(device, kCanvasWidth, kCanvasHeight) {
        canvas.clear({1, 1, 1, 1});
    }

    void step() {
        canvas.feedback(1.0f, [](braid::Surface& s) {
            s.zoom(1.03f);
            s.rotate(0.010f);
        });
    }
};

}  // namespace

// The control window: small, aspect-correct preview of the shared canvas.
class PreviewWindow : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;
    BigScene* scene = nullptr;  // set in main(), once the shared device exists

    void update() override {
        if (scene) scene->step();  // the canvas' one animation step per frame
    }

    void draw() override {
        if (!scene) return;
        background(0.0f, 0.0f, 0.0f);
        surface().compositeFrom(scene->canvas, braid::Blend::Alpha, {1.0f, 0.0f, 0.0f, 1.0f});

        if (frameCount() % 15 == 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "preview · %d fps", currentFps());
            setWindowTitle(buf);
        }
    }
};

// The output: spans monitors {1, 2} as one borderless canvas when connected;
// otherwise a plain window at a smaller size, same aspect ratio.
class OutputWindow : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;
    BigScene* scene = nullptr;

    void draw() override {
        if (!scene) return;
        background(0.0f, 0.0f, 0.0f);
        surface().compositeFrom(scene->canvas, braid::Blend::Alpha, {0.0f, 1.0f, 0.0f, 1.0f});
    }
};

int main() {
    braid::App::Settings previewSettings{};
    previewSettings.title = "Braid — multiwindow (preview)";
    previewSettings.width = kCanvasWidth / kPreviewScale;
    previewSettings.height = kCanvasHeight / kPreviewScale;
    PreviewWindow app(previewSettings);

    braid::App::Settings outputSettings{};
    outputSettings.title = "Braid — multiwindow (output)";
    outputSettings.vsync = false;

    // Log what RGFW actually enumerates, so index assignment (which monitor is
    // "1", which is "2") can be confirmed by eye before trusting it below —
    // enumeration order isn't guaranteed stable across reconnects/reboots.
    std::vector<braid::MonitorRect> mons = braid::Monitors::list();
    std::printf("[multiwindow] %zu monitor(s) detected:\n", mons.size());
    for (size_t i = 0; i < mons.size(); ++i) {
        const braid::MonitorRect& m = mons[i];
        std::printf("  [%zu] x=%d y=%d w=%d h=%d\n", i, m.x, m.y, m.width, m.height);
    }

    braid::MonitorRect span = braid::Monitors::unionOf(std::vector<int>{1, 2});
    if (span.width > 0 && span.height > 0) {
        std::printf("[multiwindow] spanning monitors {1,2}: x=%d y=%d w=%d h=%d\n", span.x,
                    span.y, span.width, span.height);
        outputSettings.monitors = {1, 2};  // real rig: span them, borderless
        outputSettings.width = span.width;
        outputSettings.height = span.height;
    } else {
        std::printf("[multiwindow] monitors {1,2} not both present — falling back to a plain window\n");
        outputSettings.width = kCanvasWidth / kFallbackScale;
        outputSettings.height = kCanvasHeight / kFallbackScale;
    }
    std::fflush(stdout);  // stdout is fully buffered when piped to a file/log — don't lose this
    auto& output = app.createWindow<OutputWindow>(outputSettings);

    // createWindow() above brought the shared device up — build the canvas
    // now and hand both windows a pointer to it. Declared after `app` so it's
    // destroyed before `app` (and its device) when main() returns.
    BigScene scene(app.device());
    app.scene = &scene;
    output.scene = &scene;

    return app.run() ? 0 : 1;
}
