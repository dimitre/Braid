// braid_detail.h — internal shared seam (NOT part of the public API).
// Lets addon translation units (braid_image.cpp, future braid_audio.cpp, …)
// reach the one process-wide WebGPU instance+device that braid-core owns,
// without re-plumbing it through the public Surface/App surface.
//
// Public code includes braid.h. Only core + addon .cpp files include this.
#pragma once

#include <webgpu/webgpu_cpp.h>

namespace braid {
namespace detail {

// The single instance+device, set once by App during init. Addons that build
// Surfaces off the main thread (image decode, etc.) borrow it from here.
struct Context {
    wgpu::Instance instance;
    wgpu::Device device;
    wgpu::RenderPassEncoder* currentPass = nullptr;  // active pass during SketchApp draw()
};

void setContext(wgpu::Instance i, wgpu::Device d);
Context& ctx();

}  // namespace detail
}  // namespace braid
