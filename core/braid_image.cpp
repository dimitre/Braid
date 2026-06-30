// braid_image.cpp — the image addon (braid-image target).
//
// This is the first proof of Braid's architecture: anything that flows *into* or
// *out of* a Surface but that the Surface itself doesn't depend on is an addon.
// File I/O is exactly that — so the mango decode/encode stack and all its codec
// archives (png/jpeg/zlib/zstd/…) link here, not in braid-core. A sketch that
// only does feedback and shapes never pays for them.
//
// Surface::load / Surface::save are *declared* in braid.h (so the public API still
// reads "an image is just a Surface") but *defined* here. Link braid-image to turn
// them on; leave it out and they're simply unresolved — a link-to-enable feature.
#include "braid.h"
#include "braid_detail.h"

#include <cstring>
#include <string>
#include <vector>

#include <mango/image/image.hpp>  // SIMD decode + extension-driven encode

namespace braid {

namespace {
// Decode an IEEE binary16 (half) to float — needed to tonemap RGBA16Float
// surfaces down to 8-bit on export.
float halfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu, f;
    if (exp == 0) {
        if (mant == 0) { f = sign << 31; }
        else {  // subnormal
            int e = -1;
            do { mant <<= 1; ++e; } while (!(mant & 0x400u));
            mant &= 0x3FFu;
            f = (sign << 31) | (static_cast<uint32_t>(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | (0xFFu << 23) | (mant << 13);
    } else {
        f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}
}  // namespace

// --- Image load: mango SIMD decode → RGBA8 Surface (single-copy upload) -------
Result<Surface> Surface::load(const char* path) {
    using namespace mango::image;
    try {
        // Decode straight to RGBA8 (mango does RGB→RGBA + SIMD in one pass).
        Bitmap bmp(std::string(path), Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8));
        if (!bmp.image || bmp.width <= 0 || bmp.height <= 0)
            return Result<Surface>::failure(std::string("Surface::load: decode failed: ") + path);

        Surface s(detail::ctx().device, bmp.width, bmp.height, wgpu::TextureFormat::RGBA8Unorm);

        // WriteTexture accepts arbitrary bytesPerRow (no 256-alignment rule), so
        // mango's stride uploads directly — one copy, CPU bitmap → texture.
        wgpu::TexelCopyTextureInfo dst{};
        dst.texture = s.texture_;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = static_cast<uint32_t>(bmp.stride);
        layout.rowsPerImage = static_cast<uint32_t>(bmp.height);
        wgpu::Extent3D ext{static_cast<uint32_t>(bmp.width), static_cast<uint32_t>(bmp.height), 1};
        detail::ctx().device.GetQueue().WriteTexture(&dst, bmp.image, bmp.stride * bmp.height,
                                                     &layout, &ext);
        return Result<Surface>::success(std::move(s));
    } catch (const std::exception& e) {
        return Result<Surface>::failure(std::string("Surface::load: ") + e.what());
    } catch (...) {
        return Result<Surface>::failure(std::string("Surface::load: failed: ") + path);
    }
}

// --- Export: readback → mango encode. Format is chosen from the file extension
//     (.png/.jpg/.bmp/…). RGBA16Float surfaces are decoded + clamped to [0,1]. ---
Result<void> Surface::save(const char* path) const {
    if (swapchain_ || !texture_)
        return Result<void>::failure("Surface::save: not an offscreen Surface");

    const bool half = (format_ == wgpu::TextureFormat::RGBA16Float);
    const uint32_t bpp = half ? 8 : 4;  // bytes per pixel in the readback
    const uint32_t rowBytes = width_ * bpp;
    const uint32_t aligned = (rowBytes + 255u) & ~255u;  // 256-byte row alignment
    const uint64_t bufSize = static_cast<uint64_t>(aligned) * height_;

    wgpu::BufferDescriptor bd{};
    bd.size = bufSize;
    bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer readback = device_.CreateBuffer(&bd);

    wgpu::TexelCopyTextureInfo srcT{};
    srcT.texture = texture_;
    wgpu::TexelCopyBufferInfo dstB{};
    dstB.buffer = readback;
    dstB.layout.bytesPerRow = aligned;
    dstB.layout.rowsPerImage = height_;
    wgpu::Extent3D ext{static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};

    wgpu::CommandEncoder enc = device_.CreateCommandEncoder();
    enc.CopyTextureToBuffer(&srcT, &dstB, &ext);
    wgpu::CommandBuffer cmd = enc.Finish();
    device_.GetQueue().Submit(1, &cmd);

    bool done = false;
    wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::Error;
    readback.MapAsync(wgpu::MapMode::Read, 0, bufSize, wgpu::CallbackMode::AllowProcessEvents,
                      [&](wgpu::MapAsyncStatus s, wgpu::StringView) { status = s; done = true; });
    while (!done) detail::ctx().instance.ProcessEvents();
    if (status != wgpu::MapAsyncStatus::Success)
        return Result<void>::failure("Surface::save: buffer map failed");

    const uint8_t* src = static_cast<const uint8_t*>(readback.GetConstMappedRange(0, bufSize));

    // Pack the readback into a tight RGBA8 buffer, then hand it to mango. mango
    // picks PNG/JPG/etc. from the path's extension and does the encoding.
    auto toByte = [](float v) -> uint8_t {
        v = v < 0 ? 0 : (v > 1 ? 1 : v);
        return static_cast<uint8_t>(v * 255.0f + 0.5f);
    };
    std::vector<uint8_t> rgba(static_cast<size_t>(width_) * height_ * 4);
    for (int y = 0; y < height_; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * aligned;
        uint8_t* o = rgba.data() + static_cast<size_t>(y) * width_ * 4;
        for (int x = 0; x < width_; ++x) {
            if (half) {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(s + x * 8);
                o[x * 4 + 0] = toByte(halfToFloat(p[0]));
                o[x * 4 + 1] = toByte(halfToFloat(p[1]));
                o[x * 4 + 2] = toByte(halfToFloat(p[2]));
                o[x * 4 + 3] = toByte(halfToFloat(p[3]));
            } else {
                o[x * 4 + 0] = s[x * 4 + 0];
                o[x * 4 + 1] = s[x * 4 + 1];
                o[x * 4 + 2] = s[x * 4 + 2];
                o[x * 4 + 3] = s[x * 4 + 3];
            }
        }
    }
    readback.Unmap();

    using namespace mango::image;
    mango::image::Surface view(width_, height_,
                               Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8),
                               static_cast<size_t>(width_) * 4, rgba.data());
    ImageEncodeStatus st = view.save(std::string(path));
    if (!st.success)
        return Result<void>::failure(std::string("Surface::save: encode failed: ") +
                                     (st.info.empty() ? path : st.info));
    return Result<void>::success();
}

}  // namespace braid
