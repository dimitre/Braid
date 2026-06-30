/*

 ▓▓▓▓▓▓▓▓▓
 ▓▓▓    ▓▓
 ▓▓▓     ▓▓              ██
 ▓▓▓     ▓▓  ▓█ ▓▓▓      ██     ▓▓
 ▓▓▓    ██   ███▓               ▓▓
 ▓▓▓▓▓▓▓▓    ███         ██     ▓▓
 ▓▓▓     ▓▓  ███   ▓▓▓▓▓ ██     ██
 ▓▓▓      █▓ ███  ▓▓  ▓▓ ▓▓     ██
 ▓██      █▓ ███ ▓▓   ▓▓ ▓▓  █████
 ▓██      ▓▓ ▓▓▓ ▓▓   ▓▓ ▓▓ ██  ▓█
 ▓▓▓▓▓▓▓▓▓▓  ▓▓▓ ▓▓▓▓▓▓▓ ▓▓ ▓▓  ▓█
  █▓▓▓▓▓▓    ▓▓▓ ▓▓   ▓▓ ▓▓  ▓▓▓▓

 */

// feed.cpp — "shatter" feedback. Each frame: grab the WHOLE frame and paste it
// back as a single rotated + scaled quad (pasteSelf). Because the quad doesn't
// cover the screen corners, every generation leaves shards behind — so the same
// content piles up at many orientations and scales: a kaleidoscope of itself.
//
// This is the difference from feedback.cpp's tunnel: there the transform is a
// *fullscreen* UV warp (always fills the screen); here the frame is pasted as a
// discrete, placed quad, so it shatters instead of tunneling.
//
// Inverted, full-power, no fade: the negative just feeds itself.
//   mouseX → per-frame rotation (screen center = no spin)
//   mouseY → quad scale (top = grow/cover, bottom = shrink → more shards)
//   i      → toggle invert (compare against the non-negative look)
//   c      → clear
#include <cstdio>
#include "braid.h"

class Feed : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;

    void draw() override {
        float rot   = braid::remap(mouseX(), 0, width(), -0.4f, 0.4f);    // center = still
        float scale = braid::remap(mouseY(), 0, height(), 1.06f, 0.86f);  // top cover → bottom shatter

        // Grab the frame, paste it back as one rotated/scaled shard over itself,
        // inverted, at full power. Blend::None ⇒ the quad replaces inside; the
        // uncovered corners keep older shards. No fade — the negative feeds itself.
        surface().pasteSelf({width() * 0.5f, height() * 0.5f},
                            {width() * scale, height() * scale},
                            rot, braid::Blend::None,
                            {1.0f, 1.0f, 1.0f, 1.0f}, invert_);

        if (frameCount() % 15 == 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "feed · %d fps", currentFps());
            setWindowTitle(buf);
        }
    }

    void keyPressed(braid::KeyEvent e) override {
        if (e.ch == 's') surface().save("f.png");  // press S to save a frame
        if (e.ch == 'i') invert_ = !invert_;
        if (e.ch == 'c') surface().clear({0, 0, 0, 1});
        // if (e.ch == 's') surface().save("feedback.png");  // press S to save a frame
    }

private:
    bool invert_ = true;  // negative feedback by default (press i to compare)
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — feed (shatter)";
    s.width = 900;
    s.height = 900;
    Feed app(s);
    return app.run() ? 0 : 1;
}
