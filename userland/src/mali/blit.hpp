// MTLBlitCommandEncoder's texture work over the passes of draw.hpp:
//   - copyFromTexture:sourceOrigin:sourceSize:toTexture:destinationOrigin: of a 4-byte format, as one dispatch of the
//     copy2d kernel (shaders_v7.h): a texture's rows are memory, and a render pass here only clears before it draws,
//     so a region is copied without one (nothing outside it is touched);
//   - generateMipmapsForTexture:, one DrawPass a level: level l drawn from level l - 1 through a linear sampler, each
//     pixel's centre on the corner its four source texels share, so each is their average (the 2x2 box).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "draw.hpp"
#include "pass.hpp"

namespace hakios::mali {

// one level of a texture: WIDTH x HEIGHT pixels of FORMAT in BO from OFFSET, ROW_BYTES apart
struct Image {
    const Bo* bo = nullptr;
    uint64_t offset = 0;
    uint32_t width = 0, height = 0, rowBytes = 0;
    Format format = Format::RGBA8Unorm;
};

std::unique_ptr<ComputePass> makeTextureCopy(Device& device, const Image& src, uint32_t sx, uint32_t sy, uint32_t width,
                                             uint32_t height, const Image& dst, uint32_t dx, uint32_t dy, std::string* why);

// levels[0] the image the others are made from, each half the one before (at least 1); RGBA8Unorm or BGRA8Unorm, each
// side a power of two. The passes in order (the queue in order: each reads what the one before wrote)
std::vector<std::unique_ptr<DrawPass>> makeMipmaps(Device& device, const std::vector<Image>& levels, std::string* why);

}  // namespace hakios::mali
