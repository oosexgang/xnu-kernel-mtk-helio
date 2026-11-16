// A draw of any program of shaders_v7.h (program.h) into an RGBA8Unorm target of any size, as Panfrost submits a batch with
// one draw on v7 without IDVS: the vertex job and the tiler job that depends on it (slot 1), then the Fragment job
// (slot 0) over the frame whose Tiler Context they filled. What the draw needs at draw time is written here through
// Mesa's packers (v7.h):
//   - the vertex attributes, one buffer each, of the program's formats (panfrost_emit_vertex_data);
//   - the varyings linked as panfrost_emit_varying_descs links them: the general buffer laid out by the fragment
//     shader's inputs (pan_assign_varyings, in the vertex shader's order, the consumer's format and size), the position
//     buffer (MALI_SNAP_4), a record of MALI_CONSTANT for an output nobody reads;
//   - the words each stage pushes, copied out of the uniform data by the program's map (panfrost_emit_const_buf), and
//     the uniform buffer table naming the same data (panfrost_emit_ubo);
//   - the Renderer States with the shaders' addresses merged in, the target's Blend descriptor (off, or source-over).
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mali.hpp"
#include "pass.hpp"
#include "program.h"
#include "v7.h"

namespace hakios::mali {

struct DrawInput {
    // per attribute of the program, VERTEX_COUNT of its elements (four floats each for the programs here)
    const float* attributes[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t vertexCount = 3;
    // uniform buffer 0: what the program's pushed words are read from (bytes)
    const void* uniforms = nullptr;
    uint32_t uniformSize = 0;
    float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool blendOver = false;     // Metal's source-over, premultiplied: One, OneMinusSourceAlpha
    // texture 0 and sampler 0 of a program that samples (RGBA8Unorm, linear rows, one level), and its filtering
    const Bo* texture = nullptr;
    Format textureFormat = Format::RGBA8Unorm;      // RGBA8Unorm, R8Unorm or RG8Unorm (hm_formats)
    uint64_t textureOffset = 0;
    uint32_t textureWidth = 0, textureHeight = 0, textureRowBytes = 0;
    bool nearest = true;
    // mip levels of the texture: level 0 at textureOffset, level l at textureLevelOffset[l] with textureLevelRowBytes[l]
    // (each (w >> l) x (h >> l), at least 1); the sampler's MTLSamplerMipFilter and lodMinClamp..lodMaxClamp
    static constexpr uint32_t kMaxLevels = 8;
    uint32_t textureLevels = 1;
    uint64_t textureLevelOffset[kMaxLevels] = {};
    uint32_t textureLevelRowBytes[kMaxLevels] = {};
    hm_mip mip = HM_MIP_NONE;
    float minLod = 0.0f, maxLod = 1000.0f;
    // Metal's depth test Less with writes, in the tile buffer alone (cleared to 1.0, not written back): what a draw of
    // several triangles needs to keep the nearest
    bool depthTest = false;
    // an index buffer: INDEX_COUNT 16-bit indices into the VERTEX_COUNT vertices (triangles of them)
    const uint16_t* indices = nullptr;
    uint32_t indexCount = 0;
    // MTLPrimitiveType: triangles, lines (pairs of vertices) or points
    enum class Primitive { Triangles, Lines, Points } primitive = Primitive::Triangles;
    // MTLViewport: the pixels NDC -1..1 lands on (x, y the corner nearest NDC (-1, -1)), the whole target when unset;
    // MTLScissorRect: the pixels that may be written. The Viewport descriptor's scissor is their intersection with the
    // target (panfrost_emit_viewport)
    struct { bool set = false; float x = 0, y = 0, width = 0, height = 0; } viewport;
    struct { bool set = false; uint32_t x = 0, y = 0, width = 0, height = 0; } scissor;
    // MTLCullMode and MTLWinding (rast->cull_face, rast->front_ccw)
    enum class Cull { None, Front, Back } cull = Cull::None;
    bool frontCCW = false;
    // MTLDepthStencilState in full, with the reference (overrides depthTest); the stencil's clear value is the first
    // draw's
    const hm_v7_zs* zs = nullptr;
    uint8_t stencilClear = 0;
    // -drawPrimitives:...instanceCount: INSTANCE_COUNT instances of the VERTEX_COUNT vertices; bit a of PER_INSTANCE: attribute
    // a steps once an instance (MTLVertexStepFunctionPerInstance, step rate 1), its data INSTANCE_COUNT elements
    uint32_t instanceCount = 1;
    uint32_t perInstance = 0;
    // an Apple vertex function's device buffers (hm_compile_air_program): buffer n at deviceBuffers[n] +
    // deviceOffsets[n], its address in uniform buffer 15 (HM_AIR_SYSVAL_UBO, 8 bytes an index)
    static constexpr uint32_t kMaxDeviceBuffers = 8;
    const Bo* deviceBuffers[kMaxDeviceBuffers] = {};
    uint64_t deviceOffsets[kMaxDeviceBuffers] = {};
};

class DrawPass {
public:
    static std::unique_ptr<DrawPass> make(Device& device, const Target& target, const hm_program& program,
                                          const DrawInput& input, std::string* why);
    // several draws in one pass: one vertex and tiler job pair each, chained (each tiler job depending on its vertex
    // job and on the tiler job before it, pan_jc_add_job), all into the one Tiler Context, then the one Fragment job;
    // the clear colour is the first draw's
    static constexpr size_t kMaxDraws = 16;
    static std::unique_ptr<DrawPass> makeMany(Device& device, const Target& target,
                                              const std::vector<std::pair<const hm_program*, DrawInput>>& draws,
                                              std::string* why);
    int submit(Submission* draw, Submission* frame);
    uint32_t vertexStatus() const;
    uint32_t tilerStatus() const;
    uint32_t fragmentStatus() const;
    void dump(FILE* fp) const;
    // what the vertex job wrote into the position varying buffer: four floats a vertex, in pixels after the viewport
    const float* positionsWritten() const;

    static constexpr uint32_t kVertexJob = 0x000, kTilerJob = 0x100, kFragmentJob = 0x200, kFbd = 0x240,
        kHeapDesc = 0x300, kTilerCtx = 0x340, kVsRsd = 0x400, kFsRsd = 0x440, kAttributes = 0x4c0,
        kAttributeBuffers = 0x500, kVaryings = 0x5c0, kVaryingBuffers = 0x640, kViewport = 0x680,
        kLocalStorage = 0x6c0, kUbos = 0x3180, kVsPush = 0x740, kFsPush = 0x7c0, kVertices = 0x800,
        kGeneral = 0xc00, kPositions = 0xe00, kVsCode = 0x1000, kFsCode = 0x1800, kUniforms = 0x2000, kTexture = 0x3000,
        kSurface = 0x3040, kSampler = 0x30c0, kSysvals = 0x3100, kSysvalSize = 32, kIndices = 0x3200, kAirSysvals = 0x3d00, kSize = 0x4000;

private:
    DrawPass(Device& device) : device_(device) {}
    Device& device_;
    Target target_;
    std::unique_ptr<Bo> cmd_;
    const Bo* heap_ = nullptr;
    std::vector<const Bo*> textures_;     // and the device buffers of Apple vertex functions
    uint32_t draws_ = 0;
};

// One dispatch of a compute kernel of shaders_v7.h (struct hm_kernel) as GENX(jm_launch_grid) submits it on v7: one
// compute job on slot 1, its Renderer State with the code's address merged in, the words it pushes copied out of the
// uniform data by the kernel's map, the uniform buffer table naming the same data, Local Storage without thread or
// workgroup storage.
class ComputePass {
public:
    static std::unique_ptr<ComputePass> make(Device& device, const hm_kernel& kernel, const unsigned groups[3],
                                             const void* uniforms, uint32_t uniformSize, std::string* why);
    // the same with uniform buffers by index (an Apple kernel's constant buffers and its system values, compiler.h) and
    // the threadgroup's size given here (LOCAL; a Metal kernel's is the dispatch's)
    static std::unique_ptr<ComputePass> makeWith(Device& device, const hm_kernel& kernel, const unsigned groups[3],
                                                 const unsigned local[3],
                                                 const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& ubos,
                                                 std::string* why);
    // with the buffers the kernel writes (each remembers the job)
    int submit(const std::vector<const Bo*>& uses, Submission* out);
    uint32_t status() const;
    void dump(FILE* fp) const;

    static constexpr uint32_t kJob = 0x000, kRsd = 0x100, kLocalStorage = 0x140, kUbos = 0x180, kPush = 0x200,
        kCode = 0x400, kUniforms = 0x2000, kSize = 0x4000;

private:
    ComputePass(Device& device) : device_(device) {}
    Device& device_;
    std::unique_ptr<Bo> cmd_;
    std::unique_ptr<Bo> wls_;        // the workgroup storage, when the kernel has threadgroup memory
};

}  // namespace hakios::mali
