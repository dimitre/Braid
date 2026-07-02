// ui_demo.cpp — TinyUI M1 smoke test: blind widgets (rect-only, no text yet).
// Layout: examples/ui_demo.txt — a label, a checkbox ("enabled"), and two
// sliders ("radius", "count") driving a ring of circles. Since M1 has no text,
// tell the widgets apart by their top-to-bottom order in the layout file.
//
//   v — toggle UI visibility (values/presets keep working while hidden)
//   r — reset all widgets to their layout-file defaults
//   s — save current values to a preset file
//   l — load that preset back
#include <cmath>
#include <cstdio>

#include "braid.h"
#include "braid_ui.h"

class UIDemo : public braid::SketchApp {
    braid::TinyUI ui{"../examples/ui_demo.txt"};

public:
    using braid::SketchApp::SketchApp;

    void setup() override {
        addAddon(ui);  // Window::addAddon, because App is a Window
    }

    void keyPressed(braid::KeyEvent e) override {
        if (e.key == braid::Key::V) ui.visible = !ui.visible;
        if (e.key == braid::Key::R) ui.resetAll();
        if (e.key == braid::Key::S) ui.savePreset("ui_demo.preset.txt");
        if (e.key == braid::Key::L) ui.loadPreset("ui_demo.preset.txt");
    }

    void draw() override {
        background(0.05f, 0.05f, 0.08f);

        if (ui.get<bool>("enabled")) {
            float radius = ui.get<float>("radius");
            int count = ui.get<int>("count");

            noStroke();
            for (int i = 0; i < count; ++i) {
                float a = i / float(count) * 6.2831853f;
                fill(1.0f, 0.35f, 0.15f + 0.5f * (i / float(count)));
                circle(width() * 0.5f + std::cos(a) * radius, height() * 0.5f + std::sin(a) * radius, 10);
            }
        }

        if (frameCount() % 15 == 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "ui_demo · %d fps · wantsMouse=%d · visible=%d", currentFps(),
                         ui.wantsMouse(), ui.visible);
            setWindowTitle(buf);
        }
    }
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — TinyUI demo (M1: blind widgets)";
    s.width = 900;
    s.height = 700;
    UIDemo app(s);
    return app.run() ? 0 : 1;
}
