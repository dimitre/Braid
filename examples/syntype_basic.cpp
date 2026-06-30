// syntype_basic.cpp — FPS counter + centered title using braid-syntype
#include <cstdio>
#include <memory>

#include "braid.h"
#include "braid_syntype.h"

class SyntypeBasic : public braid::SketchApp {
    braid::SyntypeFont font;
    std::unique_ptr<braid::Syntype> syntype;

public:
    using braid::SketchApp::SketchApp;

    void setup() override {
        auto r = braid::SyntypeFont::load("../addons/braid-syntype/fonts/arame.txt");
        if (!r) {
            std::fprintf(stderr, "font load failed: %s\n", r.error.c_str());
            close();
            return;
        }
        font = std::move(*r);
        syntype = std::make_unique<braid::Syntype>(device());
    }

    void draw() override {
        background(0.02f, 0.02f, 0.03f);

        // FPS counter — top-left
        char fps[64];
        std::snprintf(fps, sizeof(fps), "FPS: %d", currentFps());
        syntype->draw(surface(), font, fps, {10, 24}, 16.0f, {0, 1, 0, 1});

        // Centered title
        syntype->drawCentered(surface(), font, "BRAID",
                              {width() * 0.5f, height() * 0.5f}, 48.0f,
                              {1, 0.5f, 0, 1});
    }
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — syntype basic";
    s.width = 900;
    s.height = 600;
    SyntypeBasic app(s);
    if (auto r = app.run(); !r) {
        std::fprintf(stderr, "braid: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}
