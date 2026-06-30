// image.cpp — load an image file into a Surface (mango decode) and show it.
// The loaded image is just a Surface, so it composes with the whole algebra:
// composite it, feed it back, blend shapes over it, save it back out.
#include <cmath>
#include <cstdio>

#include "braid.h"

class ImageApp : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;

    void setup() override {
        if (auto r = braid::Surface::load("test.png")) {
            img_ = std::move(*r);
            std::fprintf(stderr, "[image] loaded %dx%d\n", img_->width(), img_->height());
        } else {
            std::fprintf(stderr, "[image] %s\n", r.error.c_str());
        }
    }

    void draw() override {
        if (!img_) return;
        // Opaque image → compositeFrom overwrites the frame. (No background() needed;
        // mixing it with self-submitting Surface ops would clear *after* the composite.)
        surface().compositeFrom(*img_);
    }

    void keyPressed(braid::KeyEvent e) override {
        if (e.ch == 's' && img_) surface().save("image_out.png");
    }

private:
    std::optional<braid::Surface> img_;
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — image";
    s.width = 900;
    s.height = 900;
    ImageApp app(s);
    return app.run() ? 0 : 1;
}
