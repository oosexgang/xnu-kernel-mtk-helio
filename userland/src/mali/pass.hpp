// What the library builds on the GPU: the device's shared buffers, a Write Value job, and a render pass of one colour
// target whose load action is Clear and into which nothing is drawn -- one Fragment job over a Framebuffer Descriptor
// and its Render Target, the frame Panfrost submits for a clear-only batch (pan_cmdstream.c submit_batch: no
// vertex or tiler chain, emit_fbd, emit_fragment_job; on v7 no Tiler Context either: jm_emit_tiler_desc runs only for a
// draw). Every refusal is a sentence that names what is wrong, and nothing is submitted after one.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "mali.hpp"
#include "v7.h"

namespace hakios::mali {

enum class Format { RGBA8Unorm, BGRA8Unorm, R8Unorm, RG8Unorm };
// bytes a pixel in memory; the render target format of v7.h (all four kept as R8G8B8A8 in the tile buffer:
// pan_format.c's blendable table)
inline uint32_t bytesPerPixel(Format f) { return f == Format::R8Unorm ? 1 : f == Format::RG8Unorm ? 2 : 4; }
inline enum hm_rt_format rtFormat(Format f) {
    return f == Format::BGRA8Unorm ? HM_RT_BGRA8_UNORM : f == Format::R8Unorm ? HM_RT_R8_UNORM
         : f == Format::RG8Unorm ? HM_RT_RG8_UNORM : HM_RT_RGBA8_UNORM;
}

// the device: the GPU and what every frame on it shares
class Device {
public:
    explicit Device(Gpu& gpu) : gpu_(gpu) {}
    // the GPU's parameters read and checked (a v7 GPU of pan_props.c's table: the phone's is 0x7402, G52 r1), the
    // sample positions uploaded. 0, or -1 with *why
    int init(std::string* why);

    Gpu& gpu() { return gpu_; }
    uint64_t productId() const { return productId_; }
    const Model* model() const { return model_; }
    // half the tile buffer, as Panfrost budgets it (panfrost_query_optimal_tib_size)
    uint32_t tileBufferBudget() const { return model_ ? model_->tileBuffer / 2 : kG52TileBufferBudget; }
    unsigned tilerLevels() const { return tilerLevels_; }
    // the highest shader core index plus one (panfrost_query_core_count: the last bit of SHADER_PRESENT), what workgroup
    // storage is multiplied by
    unsigned coreIdRange() const { return coreIdRange_; }
    const Bo& samplePositions() const { return *samplePositions_; }
    // the tiler heap, made the first time it is asked for (a frame with a Tiler Context names it); nullptr with *why
    const Bo* tilerHeap(std::string* why);

private:
    Gpu& gpu_;
    uint64_t productId_ = 0;
    const Model* model_ = nullptr;
    unsigned tilerLevels_ = 0;
    unsigned coreIdRange_ = 1;
    std::unique_ptr<Bo> samplePositions_, tilerHeap_;
};

// a colour target: WIDTH x HEIGHT pixels of FORMAT in BO from byte OFFSET on, ROW_BYTES apart
struct Target {
    const Bo* bo = nullptr;
    uint64_t offset = 0;
    uint32_t width = 0, height = 0, rowBytes = 0;
    Format format = Format::RGBA8Unorm;
};

struct ClearOptions {
    // a Tiler Context and its heap in the frame, as a frame with draws has them (jm_emit_tiler_desc), instead of none
    bool withTiler = false;
    // Clean Pixel Write Enable on the target, as pan_prepare_rt sets it for a clear; off only to see that the GPU then
    // writes no tile of a frame without primitives
    bool cleanPixelWrite = true;
    // the tiles the Fragment job runs, 16-pixel units, inclusive; ~0u: to the frame's last
    uint32_t minTx = 0, minTy = 0, maxTx = ~0u, maxTy = ~0u;
    // the Framebuffer Parameters' Bound Min/Max, pixels, inclusive; ~0u: to the frame's last (pan_emit_fbd always
    // writes the whole frame here and limits the work with the job's tiles alone)
    uint32_t boundMinX = 0, boundMinY = 0, boundMaxX = ~0u, boundMaxY = ~0u;
};

// One cleared frame, built and ready to submit. Its descriptors live in a command buffer of its own.
class ClearPass {
public:
    // nullptr with *why when the target, the colour or the options cannot make a frame (nothing is written then)
    static std::unique_ptr<ClearPass> make(Device& device, const Target& target, const float rgba[4],
                                           const ClearOptions& options, std::string* why);

    // to the fragment slot
    int submit(Submission* out);
    // the Fragment job's Exception Status, as the GPU left it (0: not run, 1: DONE)
    uint32_t status() const;
    uint64_t jobAddress() const { return cmd_->gpu() + kJob; }
    // the bytes pixel (x, y) must hold after the frame
    const uint8_t* expectedPixel() const { return expected_; }
    // the job, the framebuffer and its target as Mesa's printers say them
    void dump(FILE* fp) const;

private:
    ClearPass(Device& device) : device_(device) {}
    static constexpr uint32_t kJob = 0x000, kFbd = 0x040, kHeapDesc = 0x100, kTilerCtx = 0x140;
    Device& device_;
    Target target_;
    std::unique_ptr<Bo> cmd_;
    const Bo* heap_ = nullptr;
    uint8_t expected_[4] = {0, 0, 0, 0};
};

// One Write Value job: the GPU writes VALUE (32 bits) at byte OFFSET of TARGET. Its job in a buffer of its own; done
// when submit's chain is (wait), status() then 1.
class WriteValue {
public:
    static std::unique_ptr<WriteValue> make(Device& device, const Bo& target, uint64_t offset, uint32_t value,
                                            std::string* why);
    int submit(Submission* out);
    uint32_t status() const;
    void dump(FILE* fp) const;

private:
    WriteValue(Device& device) : device_(device) {}
    Device& device_;
    const Bo* target_ = nullptr;
    std::unique_ptr<Bo> cmd_;
};

}  // namespace hakios::mali
