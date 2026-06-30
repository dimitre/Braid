// syntype_ui.cpp — sliders, toggles, labels using braid-syntype
#include <cstdio>
#include <memory>
#include <string>

#include "braid.h"
#include "braid_syntype.h"

class SyntypeUI : public braid::SketchApp {
    braid::SyntypeFont font;
    std::unique_ptr<braid::Syntype> syntype;
    float gain = 0.75f;
    float feedback = 0.97f;

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

    void drawSlider(float x, float y, float w, float& value,
                    const std::string& label) {
        // Track
        fill(0.15f, 0.15f, 0.15f);
        rect(x, y, w, 4);

        // Knob
        fill(0.8f, 0.9f, 1.0f);
        circle(x + value * w, y + 2, 8);

        // Label above
        syntype->draw(surface(), font, label, {x, y - 18}, 14.0f,
                      {0.7f, 0.7f, 0.7f, 1});

        // Value below
        char valStr[32];
        std::snprintf(valStr, sizeof(valStr), "%.2f", value);
        syntype->draw(surface(), font, valStr, {x + w + 10, y + 4}, 14.0f,
                      {1, 1, 1, 1});
    }

    void draw() override {
        background(0.02f, 0.02f, 0.03f);

        drawSlider(100, 150, 300, gain, "GAIN");
        drawSlider(100, 220, 300, feedback, "FEEDBACK");

        // Title
        syntype->drawCentered(surface(), font, "SYN TYPE UI",
                              {width() * 0.5f, 80}, 32.0f,
                              {1, 0.6f, 0.2f, 1});
    }
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — syntype ui";
    s.width = 900;
    s.height = 600;
    SyntypeUI app(s);
    if (auto r = app.run(); !r) {
        std::fprintf(stderr, "braid: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}
