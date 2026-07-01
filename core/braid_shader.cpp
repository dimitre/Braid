// braid_shader.cpp — Shader: WGSL module + a render-pipeline cache keyed on
// (vertex-layout, format, blend, topology) + a 3-frame uniform ring so a bind
// group stays valid for the frames it may be in flight. No WGSL reflection: bind
// slots are stated explicitly in C++.
#include "braid.h"

#include <cstdio>
#include <string>

#include <fmt/core.h>
#include <fmt/format.h>

namespace braid {

wgpu::Buffer Shader::UniformRing::allocate(wgpu::Device device, size_t size) {
    auto& buf = buffers[index];
    auto& cap = capacities[index];
    if (!buf || cap < size) {
        wgpu::BufferDescriptor bd{};
        bd.size = size;
        bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        buf = device.CreateBuffer(&bd);
        cap = size;
    }
    wgpu::Buffer chosen = buf;
    index = (index + 1) % 3;
    return chosen;
}

Result<Shader> Shader::load(wgpu::Device device, const LoadOptions& opts) {
    if (!opts.wgsl) return Result<Shader>::failure("Shader::load: wgsl source is null");

    wgpu::ShaderSourceWGSL wgslDesc{};
    wgslDesc.code = opts.wgsl;

    wgpu::ShaderModuleDescriptor smd{};
    smd.nextInChain = &wgslDesc;
    smd.label = opts.label;

    Shader s;
    s.device_ = device;
    s.module_ = device.CreateShaderModule(&smd);
    s.vertexEntry_ = opts.vertexEntry;
    s.fragmentEntry_ = opts.fragmentEntry;
    if (!s.module_) return Result<Shader>::failure("Shader::load: module creation failed");
    return Result<Shader>::success(std::move(s));
}

Result<Shader> Shader::loadFile(wgpu::Device device, const char* path, bool debug) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return Result<Shader>::failure(fmt::format("Shader::loadFile: cannot open {}", path));
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string src(static_cast<size_t>(n), '\0');
    std::fread(src.data(), 1, static_cast<size_t>(n), f);
    std::fclose(f);
    LoadOptions opts{};
    opts.wgsl = src.c_str();
    opts.label = path;
    opts.debug = debug;
    return load(device, opts);
}

bool Shader::PipelineKey::operator==(const PipelineKey& o) const {
    return layoutHash == o.layoutHash && format == o.format && blend == o.blend &&
           topology == o.topology;
}
size_t Shader::PipelineKeyHash::operator()(const PipelineKey& k) const {
    size_t h = k.layoutHash;
    h ^= static_cast<size_t>(k.format) * 0x9e3779b97f4a7c15ULL;
    h ^= reinterpret_cast<size_t>(k.blend) << 1;
    h ^= static_cast<size_t>(k.topology) << 3;
    return h;
}

static uint64_t hashVertexLayout(const wgpu::VertexBufferLayout& layout) {
    uint64_t h = layout.arrayStride ^ (static_cast<uint64_t>(layout.attributeCount) << 32);
    for (size_t i = 0; i < layout.attributeCount; ++i) {
        const auto& a = layout.attributes[i];
        h = h * 1099511628211ULL + (static_cast<uint64_t>(a.format) << 8) + a.shaderLocation;
        h ^= a.offset;
    }
    return h;
}

wgpu::RenderPipeline Shader::getPipeline(wgpu::VertexBufferLayout vertexLayout,
                                         wgpu::TextureFormat colorFormat,
                                         const wgpu::BlendState& blend,
                                         wgpu::PrimitiveTopology topology) {
    PipelineKey key{hashVertexLayout(vertexLayout), colorFormat, &blend, topology};
    for (auto& [k, p] : pipelineCache_) {
        if (k == key) return p;
    }

    wgpu::ColorTargetState target{};
    target.format = colorFormat;
    target.blend = &blend;
    target.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState frag{};
    frag.module = module_;
    frag.entryPoint = fragmentEntry_.c_str();
    frag.targetCount = 1;
    frag.targets = &target;

    wgpu::RenderPipelineDescriptor desc{};
    desc.layout = nullptr;  // auto pipeline layout (bind groups inferred)
    desc.vertex.module = module_;
    desc.vertex.entryPoint = vertexEntry_.c_str();
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &vertexLayout;
    desc.fragment = &frag;
    desc.primitive.topology = topology;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFF;

    wgpu::RenderPipeline pipeline = device_.CreateRenderPipeline(&desc);
    pipelineCache_.push_back({key, pipeline});
    return pipeline;
}

wgpu::BindGroup Shader::bindUniform(wgpu::RenderPipeline pipeline, int group, int binding,
                                    const void* data, size_t size) {
    wgpu::Buffer buf = uniformRing_.allocate(device_, size);
    device_.GetQueue().WriteBuffer(buf, 0, data, size);

    wgpu::BindGroupEntry entry{};
    entry.binding = static_cast<uint32_t>(binding);
    entry.buffer = buf;
    entry.offset = 0;
    entry.size = size;

    wgpu::BindGroupDescriptor bgd{};
    bgd.layout = pipeline.GetBindGroupLayout(static_cast<uint32_t>(group));
    bgd.entryCount = 1;
    bgd.entries = &entry;
    return device_.CreateBindGroup(&bgd);
}

}  // namespace braid
