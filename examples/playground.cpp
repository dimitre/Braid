// playground.cpp — your canvas. Edit this and hit Cmd+Shift+R to build & run.
// (Runs until you close the window.)
// Feedback/contour params are TinyUI-driven — see examples/playground.txt.
#include <cmath>
#include <cstdio>

#include "braid.h"
#include "braid_ui.h"

class Playground : public braid::SketchApp {
    braid::TinyUI ui{"../examples/playground.txt"};

public:
    using braid::SketchApp::SketchApp;

    void setup() override {
        addAddon(ui);
    }

    void draw() override {
        float gain = ui.get<float>("gain");
        float zoom = ui.get<float>("zoom");
        float rotateAmt = ui.get<float>("rotate");
        bool contourOn = ui.get<bool>("contour");
        float contourLevel = ui.get<float>("contourLevel");
        float contourRadius = ui.get<float>("contourRadius");
        int contourMode = ui.get<int>("contourMode");
        bool invertOn = ui.get<bool>("invert");
        int count = ui.get<int>("count");
        float sizeScale = ui.get<float>("sizeScale");

        // --- feedback (omit background() so the Surface accumulates) ---
        surface().feedback(gain, [&](braid::Surface& s) {
            s.zoom(zoom);
            s.rotate(rotateAmt);
            if (contourOn) s.contour(contourLevel, contourRadius, contourMode);
            if (invertOn) s.invert();
        });

        // --- new input on top ---
        noStroke();
        float t = elapsedTime();
        for (int a = 0; a < count; ++a) {
            fill(1.0f, 0.6f, a / float(count));
            float x = width() * 0.5f + std::cos(t * 0.5f + a) * 200.0f + mouseX() / 4;
            float y = height() * 0.5f + std::sin(t * 0.7f + a) * 200.0f + mouseY() / 4;
            circle(x, y, 3 + a * sizeScale);
        }

        // --- fps in the title bar (throttled ~4x/sec) ---
        if (frameCount() % 15 == 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "playground · %d fps", currentFps());
            setWindowTitle(buf);
        }
    }
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — playground";
    s.width = 900;
    s.height = 900;
    s.width = 2000;
    s.height = 1200;
    Playground app(s);
    return app.run() ? 0 : 1;
}
