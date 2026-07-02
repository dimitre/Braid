// contour.cpp — Surface::contour() in action (ported from ofworks contour2.frag).
// Binary isoline at a luma threshold, not a flat mask like threshold()/bloom().
// A random speckle field is seeded once; after that, contour() alone runs every
// frame with nothing drawn on top of it, so its own output *is* next frame's
// input. (Primitives are batched and only actually render in afterDraw(), after
// draw() returns — so a fresh circle drawn "after" contour() in source order
// still paints over contour's result on the GPU; the fix is to not draw
// anything past the initial seed, not to reorder the calls.)
// Iterating contour() on its own raw binary output collapses to all-black within
// a couple of frames: it's an edge-highlighter (white only where a pixel has
// *some but not most* neighbors below threshold), tuned for smooth grayscale
// input, not a balanced birth/survival rule like Conway's — once the input is
// already pure black/white there's no mechanism to regenerate gray. So a small
// blur runs before each contour pass to keep reintroducing gradients (the same
// blur→threshold→repeat trick behind reaction-diffusion/Lenia-style effects).
// Keys: 1/2/3/4 — sample pattern (mode 0..3) · Up/Down — level · [ / ] — radius
// Z/X — blur amount · R — reseed with a fresh random speckle field
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "braid.h"

class Contour : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;

    void draw() override {
        // background(0.0f);
        if (!seeded_ || reseedRequested_) {
            seed();
            seeded_ = true;
            reseedRequested_ = false;
        }

        // if (blur_ > 0.0f) surface().blur(blur_);
        surface().contour(level_, radius_, mode_);

        if (frameCount() % 15 == 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "contour · mode %d · level %.2f · radius %.2f · blur %.2f · %d fps",
                          mode_, level_, radius_, blur_, currentFps());
            setWindowTitle(buf);
        }
    }

    void seed() {
        noStroke();
        for (int i = 0; i < 4000; i++) {
            float g = float(std::rand()) / float(RAND_MAX);
            fill(g, g, g);
            float x = float(std::rand() % width());
            float y = float(std::rand() % height());
            circle(x, y, 11.5f);
        }
    }

    void keyPressed(braid::KeyEvent e) override {
        if (!e.pressed) return;
        if (e.ch == '1') mode_ = 0;
        if (e.ch == '2') mode_ = 1;
        if (e.ch == '3') mode_ = 2;
        if (e.ch == '4') mode_ = 3;
        if (e.ch == 'r') reseedRequested_ = true;
        if (e.key == braid::Key::Up)   level_ = std::min(level_ + 0.02f, 1.0f);
        if (e.key == braid::Key::Down) level_ = std::max(level_ - 0.02f, 0.0f);
        if (e.ch == ']') radius_ += 0.25f;
        if (e.ch == '[') radius_ = std::max(radius_ - 0.25f, 0.25f);
        if (e.ch == 'x') blur_ += 0.1f;
        if (e.ch == 'z') blur_ = std::max(blur_ - 0.1f, 0.0f);
    }

private:
    int mode_ = 0;
    float level_ = 0.5f;
    float radius_ = 1.5f;
    float blur_ = 0.6f;
    bool seeded_ = false;
    bool reseedRequested_ = false;
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — contour (1-4=pattern, up/down=level, []=radius, z/x=blur, r=reseed)";
    s.width = 900;
    s.height = 900;
    Contour app(s);
    if (auto r = app.run(); !r) {
        std::fprintf(stderr, "braid: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}
