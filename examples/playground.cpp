// playground.cpp — your canvas. Edit this and hit Cmd+Shift+R to build & run.
// (Runs until you close the window.)
#include <cmath>
#include <cstdio>

#include "braid.h"

class Playground : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;

    void draw() override {
        // --- feedback (omit background() so the Surface accumulates) ---
        surface().feedback(0.99f, [](braid::Surface& s) {
            s.zoom(1.03f);
            s.rotate(0.0016f);
            s.invert();
            // s.blur(3.0f);
            // s.bloom();
        });

        // --- new input on top ---
        noStroke();
        float t = elapsedTime();
        for (int a=0; a<10; a++) {
            fill(1.0f, 0.6f, a/10.0f);
        float x = width() * 0.5f + std::cos(t * 1.3f + a) * 200.0f;
        float y = height() * 0.5f + std::sin(t * 1.7f + a) * 200.0f;
        circle(x, y, 28 + a*2 );
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
    Playground app(s);
    return app.run() ? 0 : 1;
}
