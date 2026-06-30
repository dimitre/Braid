// syntype_audio.cpp — audio-reactive distorted text using braid-syntype
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "braid.h"
#include "braid_syntype.h"

class SyntypeAudio : public braid::SketchApp {
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
        background(0.0f, 0.0f, 0.0f);

        // Mock 8 FFT bands
        std::vector<glm::vec2> distortion(8);
        float t = elapsedTime();
        for (int i = 0; i < 8; ++i) {
            float amp = (0.5f + 0.5f * std::sin(t * 3.0f + i * 0.8f)) * 15.0f;
            distortion[i] = {amp * std::cos(t * 2.0f + i),
                             amp * std::sin(t * 2.0f + i)};
        }

        syntype->drawDistorted(surface(), font, "FREQUENCY",
                               {width() * 0.5f - 200, height() * 0.5f + 20},
                               32.0f, {1, 0.3f, 0.5f, 1}, distortion);
    }
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — syntype audio";
    s.width = 900;
    s.height = 600;
    SyntypeAudio app(s);
    if (auto r = app.run(); !r) {
        std::fprintf(stderr, "braid: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}
