// The blit encoder's texture work (blit.hpp).
#include "blit.hpp"

#include "shaders_v7.h"

namespace hakios::mali {

static bool inside(const Image& m) {
    uint32_t bpp = bytesPerPixel(m.format);
    return m.bo != nullptr && m.width && m.height && m.rowBytes >= bpp * m.width &&
           m.offset + static_cast<uint64_t>(m.rowBytes) * (m.height - 1) + static_cast<uint64_t>(bpp) * m.width <= m.bo->size();
}

std::unique_ptr<ComputePass> makeTextureCopy(Device& device, const Image& src, uint32_t sx, uint32_t sy, uint32_t width,
                                             uint32_t height, const Image& dst, uint32_t dx, uint32_t dy, std::string* why) {
    auto no = [why](const std::string& s) {
        if (why) *why = s;
        return std::unique_ptr<ComputePass>();
    };
    if (!inside(src) || !inside(dst)) return no("an image outside its buffer");
    if (bytesPerPixel(src.format) != 4 || src.format != dst.format) return no("a copy between images of one 4-byte format");
    if (width == 0 || height == 0 || sx + width > src.width || sy + height > src.height || dx + width > dst.width ||
        dy + height > dst.height)
        return no("a region outside an image");
    if ((src.rowBytes & 3) != 0 || (dst.rowBytes & 3) != 0) return no("rows that are not whole words");
    uint64_t a = src.bo->gpu() + src.offset + static_cast<uint64_t>(sy) * src.rowBytes + 4ull * sx;
    uint64_t b = dst.bo->gpu() + dst.offset + static_cast<uint64_t>(dy) * dst.rowBytes + 4ull * dx;
    uint32_t u[8] = {static_cast<uint32_t>(a), static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(b),
                     static_cast<uint32_t>(b >> 32), width, height, src.rowBytes / 4, dst.rowBytes / 4};
    const unsigned groups[3] = {(width + 63) / 64, height, 1};
    return ComputePass::make(device, hm_copy2d, groups, u, sizeof(u), why);
}

std::vector<std::unique_ptr<DrawPass>> makeMipmaps(Device& device, const std::vector<Image>& levels, std::string* why) {
    std::vector<std::unique_ptr<DrawPass>> passes;
    auto no = [&](const std::string& s) {
        if (why) *why = s;
        passes.clear();
        return std::move(passes);
    };
    if (levels.size() < 2) return no("fewer than two levels");
    const Image& base = levels[0];
    if (base.format != Format::RGBA8Unorm && base.format != Format::BGRA8Unorm) return no("mipmaps of RGBA8 or BGRA8");
    if ((base.width & (base.width - 1)) || (base.height & (base.height - 1))) return no("a side that is not a power of two");
    for (size_t l = 0; l < levels.size(); l++) {
        const Image& m = levels[l];
        if (!inside(m) || m.format != base.format || m.width != std::max(1u, base.width >> l) ||
            m.height != std::max(1u, base.height >> l))
            return no("level " + std::to_string(l) + " is not half the one before, inside its buffer");
    }
    static const float pos[24] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
    static const float uv[24] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0};
    for (size_t l = 1; l < levels.size(); l++) {
        const Image& from = levels[l - 1];
        const Image& to = levels[l];
        Target t;
        t.bo = to.bo;
        t.offset = to.offset;
        t.width = to.width;
        t.height = to.height;
        t.rowBytes = to.rowBytes;
        t.format = to.format;
        DrawInput in;
        in.attributes[0] = pos;
        in.attributes[1] = uv;
        in.vertexCount = 6;
        in.texture = from.bo;
        in.textureOffset = from.offset;
        in.textureWidth = from.width;
        in.textureHeight = from.height;
        in.textureRowBytes = from.rowBytes;
        in.nearest = false;
        // the tex program's target is RGBA as sampled: a BGRA8 level is read and written in the same order
        auto pass = DrawPass::make(device, t, hm_tex, in, why);
        if (!pass) return no("level " + std::to_string(l) + ": " + (why ? *why : std::string()));
        passes.push_back(std::move(pass));
    }
    return passes;
}

}  // namespace hakios::mali
