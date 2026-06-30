// bloom.cpp — blur, threshold, and bloom in action.
// Three modes toggled with 1/2/3:
//   1 — plain feedback tunnel (baseline)
//   2 — blur applied after feedback (soft trails)
//   3 — bloom on the tunnel (HDR glow on bright energy)
// Press S to save a frame.
#include <cmath>
#include <cstdio>

#include "braid.h"

class Bloom : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;

    void draw() override {
        surface().feedback(0.97f, [](braid::Surface& s) {
            s.zoom(1.02f);
            s.rotate(0.008f);
        });

        // Two bright moving sources — HDR values above 1.0
        float t = elapsedTime();
        noStroke();
        fill(2.0f, 1.2f, 0.2f);   // orange, HDR
        ellipse(width()  * 0.5f + std::cos(t * 1.3f) * 200.0f,
                height() * 0.5f + std::sin(t * 1.9f) * 140.0f,
                40, 40);
        fill(0.4f, 1.5f, 2.5f);   // cyan, HDR
        ellipse(width()  * 0.5f + std::cos(t * 2.1f + 1.0f) * 180.0f,
                height() * 0.5f + std::sin(t * 1.4f + 2.0f) * 160.0f,
                32, 32);

        if (mode_ == 2) surface().blur(8.0f);
        if (mode_ == 3) surface().bloom(1.0f, 1.2f, 4);
    }

    void keyPressed(braid::KeyEvent e) override {
        if (e.ch == '1') mode_ = 1;
        if (e.ch == '2') mode_ = 2;
        if (e.ch == '3') mode_ = 3;
        if (e.ch == 's') surface().save("bloom.png");
    }

private:
    int mode_ = 3;
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — bloom (1=plain 2=blur 3=bloom)";
    s.width = 900;
    s.height = 900;
    Bloom app(s);
    if (auto r = app.run(); !r) {
        std::fprintf(stderr, "braid: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}
