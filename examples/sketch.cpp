// sketch.cpp — the Processing-style Tier 2 face. background() clears each frame;
// fill/stroke/transform + primitives draw into the main Surface.
#include <cmath>
#include <cstdio>

#include "braid.h"

class Sketch : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;

    void draw() override {
        // background(0.08f, 0.09f, 0.11f);  // clears (omit it to accumulate)
        // background(0.08f, 0.09f, 0.11f, 0.01f);  // clears (omit it to accumulate)
        surface().feedback(0.97f, [](braid::Surface& s) {
        });

        float t = elapsedTime();
        // a row of pulsing circles
        for (int i = 0; i < 8; ++i) {
            float x = 100.0f + i * 90.0f;
            float y = height() * 0.5f + std::sin(t * 2.0f + i * 0.6f) * 120.0f;
            float r = 30.0f + std::sin(t * 3.0f + i) * 12.0f;
            fill(0.2f + 0.1f * i, 0.6f, 1.0f - 0.1f * i);
            circle(x, y, r);
        }

        // a spinning square via the transform stack
        pushMatrix();
        translate(width() * 0.5f, 120.0f);
        rotate(t);
        fill(1.0f, 0.7f, 0.2f);
        rect(-30, -30, 60, 60);
        popMatrix();


    }
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — sketch";
    s.width = 900;
    s.height = 600;
    Sketch app(s);
    if (auto r = app.run(); !r) {
        std::fprintf(stderr, "braid: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}
