// braid_mesh.cpp — Mesh: GPU vertex (+ optional index) buffers, the canonical
// interleaved vertex layout, and the CPU primitive generators (triangle, plane,
// cube, fullscreen quad, polyline). CPU copies are retained so clone() is a pure
// re-upload. No GPU device state beyond the buffers themselves.
#include "braid.h"

#include <array>
#include <vector>

namespace braid {

Mesh::Mesh(wgpu::Device device) : device_(device) {}

wgpu::VertexBufferLayout Mesh::vertexLayout() {
    static const std::array<wgpu::VertexAttribute, 4> attrs = {{
        {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, position), .shaderLocation = 0},
        {.format = wgpu::VertexFormat::Float32x2, .offset = offsetof(Vertex, texCoord), .shaderLocation = 1},
        {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, normal), .shaderLocation = 2},
        {.format = wgpu::VertexFormat::Float32x4, .offset = offsetof(Vertex, color), .shaderLocation = 3},
    }};
    wgpu::VertexBufferLayout layout{};
    layout.arrayStride = sizeof(Vertex);
    layout.stepMode = wgpu::VertexStepMode::Vertex;
    layout.attributeCount = attrs.size();
    layout.attributes = attrs.data();
    return layout;
}

Result<void> Mesh::setVertices(std::span<const Vertex> vertices) {
    if (vertices.empty()) return Result<void>::failure("Mesh::setVertices: empty span");
    cpuVertices_.assign(vertices.begin(), vertices.end());
    const size_t bytes = vertices.size_bytes();

    wgpu::BufferDescriptor bd{};
    bd.size = bytes;
    bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    vertexBuffer_ = device_.CreateBuffer(&bd);
    if (!vertexBuffer_) return Result<void>::failure("Mesh::setVertices: buffer creation failed");

    device_.GetQueue().WriteBuffer(vertexBuffer_, 0, vertices.data(), bytes);
    vertexCount_ = vertices.size();
    return Result<void>::success();
}

Result<void> Mesh::setIndices(std::span<const uint32_t> indices) {
    if (indices.empty()) return Result<void>::failure("Mesh::setIndices: empty span");
    cpuIndices_.assign(indices.begin(), indices.end());
    const size_t bytes = indices.size_bytes();

    wgpu::BufferDescriptor bd{};
    bd.size = bytes;
    bd.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
    indexBuffer_ = device_.CreateBuffer(&bd);
    if (!indexBuffer_) return Result<void>::failure("Mesh::setIndices: buffer creation failed");

    device_.GetQueue().WriteBuffer(indexBuffer_, 0, indices.data(), bytes);
    indexCount_ = indices.size();
    return Result<void>::success();
}

void Mesh::draw(wgpu::RenderPassEncoder& pass, uint32_t instanceCount) {
    if (!vertexBuffer_) return;
    pass.SetVertexBuffer(0, vertexBuffer_, 0, cpuVertices_.size() * sizeof(Vertex));
    if (indexCount_ > 0) {
        pass.SetIndexBuffer(indexBuffer_, wgpu::IndexFormat::Uint32, 0,
                            cpuIndices_.size() * sizeof(uint32_t));
        pass.DrawIndexed(static_cast<uint32_t>(indexCount_), instanceCount);
    } else {
        pass.Draw(static_cast<uint32_t>(vertexCount_), instanceCount);
    }
}

Result<Mesh> Mesh::clone() const {
    Mesh out(device_);
    if (!cpuVertices_.empty()) {
        auto r = out.setVertices(cpuVertices_);
        if (!r) return Result<Mesh>::failure(r.error);
    }
    if (!cpuIndices_.empty()) {
        auto r = out.setIndices(cpuIndices_);
        if (!r) return Result<Mesh>::failure(r.error);
    }
    return Result<Mesh>::success(std::move(out));
}

Result<Mesh> Mesh::triangle(wgpu::Device device, glm::vec3 a, glm::vec3 b, glm::vec3 c) {
    Mesh m(device);
    Vertex va{a, {0, 0}, {0, 0, 1}, {1, 1, 1, 1}};
    Vertex vb{b, {1, 0}, {0, 0, 1}, {1, 1, 1, 1}};
    Vertex vc{c, {0.5f, 1}, {0, 0, 1}, {1, 1, 1, 1}};
    std::array<Vertex, 3> v{va, vb, vc};
    auto r = m.setVertices(v);
    if (!r) return Result<Mesh>::failure(r.error);
    return Result<Mesh>::success(std::move(m));
}

Result<Mesh> Mesh::plane(wgpu::Device device, float w, float h, int cols, int rows) {
    Mesh m(device);
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    const float x0 = -w * 0.5f, y0 = -h * 0.5f;
    for (int r = 0; r <= rows; ++r) {
        for (int c = 0; c <= cols; ++c) {
            float u = static_cast<float>(c) / cols, v = static_cast<float>(r) / rows;
            verts.push_back({{x0 + u * w, y0 + v * h, 0}, {u, v}, {0, 0, 1}, {1, 1, 1, 1}});
        }
    }
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            uint32_t i = r * (cols + 1) + c;
            uint32_t j = i + cols + 1;
            idx.insert(idx.end(), {i, j, i + 1, i + 1, j, j + 1});
        }
    }
    if (auto rr = m.setVertices(verts); !rr) return Result<Mesh>::failure(rr.error);
    if (auto rr = m.setIndices(idx); !rr) return Result<Mesh>::failure(rr.error);
    return Result<Mesh>::success(std::move(m));
}

Result<Mesh> Mesh::cube(wgpu::Device device, float size) {
    Mesh m(device);
    const float s = size * 0.5f;
    const glm::vec3 p[8] = {{-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s},
                            {-s, -s, s},  {s, -s, s},  {s, s, s},  {-s, s, s}};
    const int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
                             {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
    const glm::vec3 nrm[6] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    for (int f = 0; f < 6; ++f) {
        uint32_t base = static_cast<uint32_t>(verts.size());
        const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int k = 0; k < 4; ++k)
            verts.push_back({p[faces[f][k]], uv[k], nrm[f], {1, 1, 1, 1}});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    if (auto rr = m.setVertices(verts); !rr) return Result<Mesh>::failure(rr.error);
    if (auto rr = m.setIndices(idx); !rr) return Result<Mesh>::failure(rr.error);
    return Result<Mesh>::success(std::move(m));
}

Result<Mesh> Mesh::fullscreenQuad(wgpu::Device device) {
    Mesh m(device);
    std::array<Vertex, 6> v{{
        {{-1, -1, 0}, {0, 1}, {0, 0, 1}, {1, 1, 1, 1}},
        {{1, -1, 0}, {1, 1}, {0, 0, 1}, {1, 1, 1, 1}},
        {{1, 1, 0}, {1, 0}, {0, 0, 1}, {1, 1, 1, 1}},
        {{-1, -1, 0}, {0, 1}, {0, 0, 1}, {1, 1, 1, 1}},
        {{1, 1, 0}, {1, 0}, {0, 0, 1}, {1, 1, 1, 1}},
        {{-1, 1, 0}, {0, 0}, {0, 0, 1}, {1, 1, 1, 1}},
    }};
    if (auto rr = m.setVertices(v); !rr) return Result<Mesh>::failure(rr.error);
    return Result<Mesh>::success(std::move(m));
}

Result<Mesh> Mesh::line(wgpu::Device device, std::span<const glm::vec3> points) {
    Mesh m(device);
    std::vector<Vertex> verts;
    verts.reserve(points.size());
    for (auto& p : points) verts.push_back({p, {0, 0}, {0, 0, 1}, {1, 1, 1, 1}});
    if (auto rr = m.setVertices(verts); !rr) return Result<Mesh>::failure(rr.error);
    return Result<Mesh>::success(std::move(m));
}

Result<Mesh> Mesh::line(wgpu::Device device, std::span<const glm::vec2> points) {
    std::vector<glm::vec3> p3;
    p3.reserve(points.size());
    for (auto& p : points) p3.emplace_back(p.x, p.y, 0.0f);
    return line(device, std::span<const glm::vec3>(p3));
}

}  // namespace braid
