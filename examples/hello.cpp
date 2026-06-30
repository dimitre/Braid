// hello.cpp — Braid v0.1.0: clear the screen and draw a red triangle at 60fps.
// Uses the explicit Tier-1 API (App + Shader + Mesh).
#include <cstdio>

#include "braid.h"

static const char* kTriangleWGSL = R"WGSL(
@vertex
fn vs(@location(0) position : vec3<f32>) -> @builtin(position) vec4<f32> {
    return vec4<f32>(position, 1.0);
}

@fragment
fn fs() -> @location(0) vec4<f32> {
    return vec4<f32>(1.0, 0.0, 0.0, 1.0);  // red
}
)WGSL";

class Hello : public braid::App {
public:
    using braid::App::App;

    void setup() override {
        braid::Shader::LoadOptions opts{};
        opts.wgsl = kTriangleWGSL;
        opts.label = "triangle";
        if (auto r = braid::Shader::load(device(), opts)) {
            shader_ = std::move(*r);
        } else {
            std::fprintf(stderr, "shader load failed: %s\n", r.error.c_str());
            close();
            return;
        }

        if (auto r = braid::Mesh::triangle(device(), {0.0f, 0.5f, 0.0f}, {-0.5f, -0.5f, 0.0f},
                                           {0.5f, -0.5f, 0.0f})) {
            mesh_ = std::move(*r);
        } else {
            std::fprintf(stderr, "mesh build failed: %s\n", r.error.c_str());
            close();
        }
    }

    void draw() override {
        if (!shader_ || !mesh_) return;
        auto pass = screen().begin(encoder(), {0.1f, 0.1f, 0.12f, 1.0f});  // dark clear

        auto pipeline = shader_->getPipeline(braid::Mesh::vertexLayout(), screen().format(),
                                             braid::Blend::None);
        pass.SetPipeline(pipeline);
        mesh_->draw(pass);

        screen().end(pass);
    }

private:
    std::optional<braid::Shader> shader_;
    std::optional<braid::Mesh> mesh_;
};

int main() {
    braid::App::Settings settings{};
    settings.title = "Braid — hello";
    settings.width = 1280;
    settings.height = 720;
    settings.targetFps = 60;

    Hello app(settings);
    if (auto r = app.run(); !r) {
        std::fprintf(stderr, "braid: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}
