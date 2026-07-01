// cubes.cpp — a Braid port of the MicroFramework sketch: a nested lattice of
// rotating wireframe cubes, perspective camera, mouse drives density + spin.
// (box() here is wireframe, like glutWireCube.)
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "braid.h"

class Cubes : public braid::SketchApp {
public:
    using braid::SketchApp::SketchApp;
    float a = 200.0f;

    void draw() override {
        background(0.02f, 0.02f, 0.03f);

        // gluPerspective(40, 1, .1, 2450) + gluLookAt(0,0,450, 0,0,0, 0,1,0)
        perspective(glm::radians(40.0f), 0.1f, 2450.0f);
        camera({0, 0, 450}, {0, 0, 0}, {0, 1, 0});

        float mx = mousePos().x, my = mousePos().y;

        pushMatrix();
        rotateY((mx / width() - 0.5f) * 6.2831853f);   // mouse spins the lattice
        rotateX((my / height() - 0.5f) * 3.1415926f);

        a += 0.0002f;
        const int maxN = 16;
        const float aresta = 8.0f;
        for (int interval : {8, 4, 2}) {  // nested grids
            float arestaInterval = aresta * interval;
            for (int x = -maxN; x <= maxN; x += interval) {
                float r = braid::remap(x, -maxN, maxN, 0.4f, 1.0f);
                for (int y = -maxN; y <= maxN; y += interval) {
                    pushMatrix();
                    translate(x * aresta, y * aresta, 0);
                    float rot = x * 1.5f + y * 2.2f + a * 300.0f;
                    rotateX(glm::radians(rot));
                    rotateY(glm::radians(rot * 1.3f));
                    rotateZ(glm::radians(rot * 1.7f));
                    fill(r, 1.0f - r, mx / width(), 0.8f);
                    box(arestaInterval * 0.4f);
                    popMatrix();
                }
            }
        }
        popMatrix();

        // The cubes above are only *recorded* into the batch; bloom() submits its
        // own GPU work immediately and would otherwise run before the cubes are
        // actually rasterized. Flush them to the GPU first so bloom sees them.
        submitFrame();

        if (frameCount() % 15 == 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "cubes · %d fps", currentFps());
            setWindowTitle(buf);
        }

        surface().bloom(0.1f, 2.0f, 4);
    }
};

int main() {
    braid::App::Settings s{};
    s.title = "Braid — cubes";
    s.width = 900;
    s.height = 900;
    s.vsync = false;     // free framerate (don't block to display refresh)
    s.targetFps = 0;     // uncapped (Timer just measures)
    Cubes app(s);
    return app.run() ? 0 : 1;
}
