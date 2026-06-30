// feedback.cpp — the ouroboros. One Surface feeding itself: each frame the
// previous contents are zoomed + rotated and decayed by gain, then a bright
// moving dot is added on top. No background() ⇒ the Surface accumulates.
// This is `surface += surface.transformed()`, the analog-video-feedback tunnel.
#include <cmath>
#include <cstdio>

#include "braid.h"

class Feedback : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;

    void draw() override {
        // The whole idea, one line: transform the previous frame, decay by gain.
        surface().feedback(0.97f, [](braid::Surface& s) {
            s.zoom(1.03f);     // recursive magnify → tunnel
            s.rotate(0.010f);  // slow twist
        });

        // New input on top (no background() this frame ⇒ it accumulates).
        float t = elapsedTime();
        float x = width() * 0.5f + std::cos(t * 1.7f) * 220.0f;
        float y = height() * 0.5f + std::sin(t * 2.3f) * 160.0f;
        noStroke();
        fill(1.0f, 0.55f, 0.12f);
        ellipse(x, y, 46, 46);

    }

    void keyPressed(braid::KeyEvent e) override {
        if (e.ch == 's') surface().save("feedback.png");  // press S to save a frame
    }
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — feedback";
    s.width = 900;
    s.height = 900;
    Feedback app(s);
    if (auto r = app.run(); !r) {
        std::fprintf(stderr, "braid: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}
