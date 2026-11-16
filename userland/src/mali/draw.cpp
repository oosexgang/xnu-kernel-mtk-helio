// DrawPass (draw.hpp).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "draw.hpp"
#include "shaders_v7.h"
#include "v7.h"

namespace hakios::mali {

// One draw's descriptors in a block of the pass's buffer (CPU and GPU addresses CPU, GPU), its vertex job numbered VINDEX
// and its tiler job VINDEX + 1, depending on the vertex job and (pan_jc_add_job: tiler jobs are chained) on the tiler
// job PREV_TILER before it, NEXT after it in the chain. 0, or the reason the draw cannot be written.
static std::string writeDraw(Device& device, const Target& t, const hm_program& p, const DrawInput& in, uint8_t* cpu,
                             uint64_t gpu, uint64_t tilerCtx, unsigned vIndex, unsigned prevTiler, uint64_t next) {
    auto no = [&p](const std::string& s) { return std::string(p.name) + ": " + s; };
    const struct hm_v7_sizes* sz = hm_v7_sizes();
    const bool bgra = t.format == Format::BGRA8Unorm;
    using D = DrawPass;
    constexpr uint32_t kVertexJob = D::kVertexJob, kTilerJob = D::kTilerJob, kVsRsd = D::kVsRsd, kFsRsd = D::kFsRsd,
        kAttributes = D::kAttributes, kAttributeBuffers = D::kAttributeBuffers, kVaryings = D::kVaryings,
        kVaryingBuffers = D::kVaryingBuffers, kViewport = D::kViewport, kLocalStorage = D::kLocalStorage,
        kUbos = D::kUbos, kVsPush = D::kVsPush, kFsPush = D::kFsPush, kVertices = D::kVertices, kGeneral = D::kGeneral,
        kPositions = D::kPositions, kVsCode = D::kVsCode, kFsCode = D::kFsCode, kUniforms = D::kUniforms,
        kTexture = D::kTexture, kSurface = D::kSurface, kSampler = D::kSampler, kSysvals = D::kSysvals,
        kSysvalSize = D::kSysvalSize, kIndices = D::kIndices, kSize = D::kSize;
    (void)device;
    const unsigned per = in.primitive == DrawInput::Primitive::Triangles ? 3 :
                         in.primitive == DrawInput::Primitive::Lines ? 2 : 1;
    if (in.primitive != DrawInput::Primitive::Triangles && in.indices != nullptr)
        return no("indices with lines or points (not served yet)");
    if (in.indices == nullptr && (in.vertexCount < per || in.vertexCount % per != 0 || in.vertexCount > 48))
        return no("a vertex count that is not a whole number of primitives, up to 48");
    if (in.indices != nullptr) {
        if (in.vertexCount < 1 || in.vertexCount > 48 || in.indexCount < 3 || in.indexCount % 3 != 0 ||
            in.indexCount * 2 > D::kAirSysvals - kIndices)
            return no("an indexed draw of 1 to 48 vertices and a whole number of triangles of indices");
        for (uint32_t i = 0; i < in.indexCount; i++) {
            if (in.indices[i] >= in.vertexCount) return no("an index past the vertices");
        }
    }
    if (p.attribute_count > 4) return no("more than four vertex attributes");
    const bool instanced = in.instanceCount > 1;
    if (in.instanceCount < 1 || in.instanceCount > 16) return no("an instance count of 1 to 16");
    if (instanced && in.indices != nullptr) return no("indices with instances (not served yet)");
    if ((in.perInstance >> p.attribute_count) != 0) return no("a per-instance attribute the program does not have");
    // the vertices an instance takes in the varyings (panfrost_padded_vertex_count, for instance_count > 1)
    const uint32_t padded = instanced ? hm_v7_padded_vertex_count(in.vertexCount) : in.vertexCount;
    const uint32_t shaded = padded * in.instanceCount;
    auto elements = [&](uint32_t a) { return (in.perInstance >> a & 1) ? in.instanceCount : in.vertexCount; };
    uint32_t vertexBytes = 0;
    for (uint32_t a = 0; a < p.attribute_count; a++) {
        if (in.attributes[a] == nullptr) return no("attribute " + std::to_string(a) + " has no data");
        vertexBytes += (p.attribute_sizes[a] * elements(a) + 63) & ~63u;
        for (uint32_t i = 0; i < p.attribute_sizes[a] / 4 * elements(a); i++) {
            if (!std::isfinite(in.attributes[a][i])) return no("an attribute value that is not a finite number");
        }
    }
    if (vertexBytes > kGeneral - kVertices) return no("the vertices do not fit");
    if (p.texture_count > 1 || p.sampler_count > 1) return no("more than one texture or sampler");
    static_assert(D::kSurface + DrawInput::kMaxLevels * 16 <= D::kSampler && D::kSampler + 32 <= D::kSysvals &&
                  D::kSysvals + 32 <= D::kUbos && D::kUbos + 16 * 8 <= D::kIndices && D::kAirSysvals + 288 <= D::kSize, "layout");
    if (p.texture_count != 0) {
        const Bo* tb = in.texture;
        if (tb == nullptr) return no("the program samples a texture and none is given");
        uint64_t tbase = tb->gpu() + in.textureOffset;
        const uint32_t tbpp = bytesPerPixel(in.textureFormat);
        if (in.textureFormat == Format::BGRA8Unorm) return no("a BGRA8 texture (not served yet)");
        if (in.textureWidth == 0 || in.textureHeight == 0 || in.textureWidth > 4096 || in.textureHeight > 4096 ||
            in.textureRowBytes < tbpp * in.textureWidth || (tbase & 63) != 0 || (in.textureRowBytes & 63) != 0 ||
            in.textureOffset + static_cast<uint64_t>(in.textureRowBytes) * (in.textureHeight - 1) +
                    static_cast<uint64_t>(tbpp) * in.textureWidth > tb->size())
            return no("the texture is not 64-byte aligned rows of its format inside its buffer (pan_layout.c, v7)");
        if (in.textureLevels < 1 || in.textureLevels > DrawInput::kMaxLevels ||
            (in.textureLevels > 1 && ((in.textureWidth >> (in.textureLevels - 1)) == 0 && (in.textureHeight >> (in.textureLevels - 1)) == 0)))
            return no("mip levels past 1x1, or more than eight");
        for (uint32_t l = 1; l < in.textureLevels; l++) {
            uint32_t lw = std::max(1u, in.textureWidth >> l), lh = std::max(1u, in.textureHeight >> l);
            uint64_t lb = tb->gpu() + in.textureLevelOffset[l];
            uint32_t rb = in.textureLevelRowBytes[l];
            if ((lb & 63) != 0 || (rb & 63) != 0 || rb < tbpp * lw ||
                in.textureLevelOffset[l] + static_cast<uint64_t>(rb) * (lh - 1) + static_cast<uint64_t>(tbpp) * lw > tb->size())
                return no("mip level " + std::to_string(l) + " is not 64-byte aligned rows inside the texture's buffer");
        }
        if (!(in.minLod >= 0) || !(in.maxLod >= in.minLod)) return no("LOD clamps that are not 0 <= min <= max");
    }
    if (in.blendOver && !p.can_blend) return no("the program has no blended state");
    // uniform buffer 0 is the caller's, 1 the system values (compiler.c: the viewport's scale and offset)
    // and 15 the device buffers' addresses (an Apple vertex function's: compiler.h HM_AIR_SYSVAL_UBO)
    auto pushable = [&](const hm_push_word& w) {
        return (w.ubo == 0 && w.offset + 4u <= in.uniformSize) || (w.ubo == 1 && w.offset + 4u <= kSysvalSize) ||
               (w.ubo == 15 && w.offset + 4u <= 8 * DrawInput::kMaxDeviceBuffers);
    };
    for (uint32_t i = 0; i < p.vs_push_count; i++) {
        if (!pushable(p.vs_push[i])) return no("the vertex shader pushes a word the uniform data does not hold");
    }
    for (uint32_t i = 0; i < p.fs_push_count; i++) {
        if (!pushable(p.fs_push[i])) return no("the fragment shader pushes a word the uniform data does not hold");
    }
    if (in.uniformSize > kTexture - kUniforms) return no("the uniform data do not fit");
    if (p.vs_size > kFsCode - kVsCode || p.fs_size > kUniforms - kFsCode - 128) return no("the shaders do not fit");

    // the varyings, linked (pan_assign_varyings, panfrost_emit_varying_descs): general buffer offsets by the vertex
    // shader's output order, for the outputs the fragment shader reads, in the reader's format and size
    std::vector<int32_t> offset(p.vs_out_count, -1);
    std::vector<uint32_t> format(p.vs_out_count, 0);
    uint32_t stride = 0;
    for (uint32_t i = 0; i < p.vs_out_count; i++) {
        for (uint32_t j = 0; j < p.fs_in_count; j++) {
            if (p.fs_in[j].location == p.vs_out[i].location && p.vs_out[i].location != HM_VARYING_SLOT_POS) {
                offset[i] = static_cast<int32_t>(stride);
                format[i] = p.fs_in[j].format;
                stride += p.fs_in[j].size;
            }
        }
    }
    for (uint32_t j = 0; j < p.fs_in_count; j++) {
        bool found = false;
        for (uint32_t i = 0; i < p.vs_out_count; i++) found |= p.vs_out[i].location == p.fs_in[j].location;
        if (!found) return no("the fragment shader reads a varying the vertex shader does not write");
    }
    if (stride * shaded > kPositions - kGeneral || 16 * shaded > kVsCode - kPositions) return no("the varyings do not fit");
    if (p.vs_out_count + p.fs_in_count > 16) return no("more than sixteen varying records");

    memset(cpu, 0, kSize);
    // the shaders, their Renderer State with their addresses merged in, and the target's Blend descriptor
    memcpy(cpu + kVsCode, p.vs_code, p.vs_size);
    memcpy(cpu + kFsCode, p.fs_code, p.fs_size);
    uint32_t rsd[16];
    memcpy(rsd, p.vs_rsd, sizeof(rsd));
    rsd[0] |= static_cast<uint32_t>(gpu + kVsCode);
    rsd[1] |= static_cast<uint32_t>((gpu + kVsCode) >> 32);
    memcpy(cpu + kVsRsd, rsd, sizeof(rsd));
    const bool zs = in.depthTest || in.zs != nullptr;
    if (zs && in.blendOver) return no("a depth or stencil test with blending (not served yet)");
    // a depth or stencil state: the Renderer State compiled for a test that writes (pan_earlyzs_get with writes_zs),
    // its words 8 to 11 the state's
    memcpy(rsd, zs ? p.fs_rsd_depth : in.blendOver ? p.fs_rsd_blended : p.fs_rsd, sizeof(rsd));
    if (in.zs != nullptr) {
        const char* bad = nullptr;
        if (hm_v7_renderer_state_zs(rsd, in.zs, &bad) != 0) return no(std::string("the depth and stencil state: ") + bad);
    }
    rsd[0] |= static_cast<uint32_t>(gpu + kFsCode);
    rsd[1] |= static_cast<uint32_t>((gpu + kFsCode) >> 32);
    memcpy(cpu + kFsRsd, rsd, sizeof(rsd));
    memcpy(cpu + kFsRsd + sz->renderer_state,
           in.blendOver ? (bgra ? p.blend_over_bgra : p.blend_over) : (bgra ? p.blend_off_bgra : p.blend_off), sz->blend);

    // the vertex attributes, one buffer each, and the tables' ends
    // (instanced: per vertex 1D_MODULUS by the padded count, per instance a divisor of it; an NPOT divisor takes a
    // continuation record, and such a pair starts at an even record -- panfrost_emit_vertex_data)
    uint64_t at = kVertices;
    unsigned k = 0;
    for (uint32_t a = 0; a < p.attribute_count; a++) {
        uint32_t bytes = p.attribute_sizes[a] * elements(a);
        memcpy(cpu + at, in.attributes[a], bytes);
        uint8_t* rec;
        if (!instanced) {
            rec = cpu + kAttributeBuffers + k * sz->attribute_buffer;
            hm_v7_attribute_buffer(rec, gpu + at, p.attribute_sizes[a], bytes);
            hm_v7_attribute(cpu + kAttributes + a * sz->attribute, k, p.attribute_formats[a], 0, true);
            k++;
        } else {
            bool step = in.perInstance >> a & 1;
            if (step && (padded & (padded - 1)) != 0) k = (k + 1) & ~1u;
            rec = cpu + kAttributeBuffers + k * sz->attribute_buffer;
            hm_v7_attribute(cpu + kAttributes + a * sz->attribute, k, p.attribute_formats[a], 0, true);
            k += hm_v7_attribute_buffer_instanced(rec, gpu + at, p.attribute_sizes[a], bytes, padded, step);
        }
        at = (at + bytes + 63) & ~63ull;
    }
    if (kAttributeBuffers + (k + 1) * sz->attribute_buffer > kVaryings) return no("too many attribute buffer records");
    hm_v7_attribute_buffer(cpu + kAttributeBuffers + k * sz->attribute_buffer, 0, 0, 0);

    // the varying records: the vertex shader's (producer), then the fragment shader's (consumer); buffers GENERAL 0,
    // POSITION 1, then the table's end
    for (uint32_t i = 0; i < p.vs_out_count; i++) {
        uint8_t* rec = cpu + kVaryings + i * sz->attribute;
        if (p.vs_out[i].location == HM_VARYING_SLOT_POS) {
            hm_v7_attribute(rec, 1, hm_v7_position_format(), 0, false);
        } else if (offset[i] < 0) {
            hm_v7_attribute(rec, 0, hm_v7_constant_format(), 0, false);
        } else {
            hm_v7_attribute(rec, 0, format[i], offset[i], false);
        }
    }
    for (uint32_t j = 0; j < p.fs_in_count; j++) {
        int32_t off = 0;
        for (uint32_t i = 0; i < p.vs_out_count; i++) {
            if (p.vs_out[i].location == p.fs_in[j].location) off = offset[i];
        }
        hm_v7_attribute(cpu + kVaryings + (p.vs_out_count + j) * sz->attribute, 0, p.fs_in[j].format, off, false);
    }
    if (stride != 0) {
        hm_v7_attribute_buffer(cpu + kVaryingBuffers, gpu + kGeneral, stride, stride * shaded);
    }
    hm_v7_attribute_buffer(cpu + kVaryingBuffers + sz->attribute_buffer, gpu + kPositions, 16, 16 * shaded);
    hm_v7_attribute_buffer(cpu + kVaryingBuffers + 2 * sz->attribute_buffer, 0, 0, 0);

    // the uniforms: buffer 0 the caller's, buffer 1 the system values -- the viewport transform's scale and offset
    // (w/2, h/2, 0.5), as panfrost_upload_sysvals gives PAN_SYSVAL_VIEWPORT_SCALE and _OFFSET for a viewport of the whole
    // target with depth 0..1 -- the table naming both, and each stage's pushed words (panfrost_emit_const_buf)
    if (in.uniformSize != 0) {
        memcpy(cpu + kUniforms, in.uniforms, in.uniformSize);
        hm_v7_uniform_buffer(cpu + kUbos, gpu + kUniforms, in.uniformSize);
    }
    float vpX = 0, vpY = 0, vpW = static_cast<float>(t.width), vpH = static_cast<float>(t.height);
    if (in.viewport.set) {
        vpX = in.viewport.x, vpY = in.viewport.y, vpW = in.viewport.width, vpH = in.viewport.height;
        if (!std::isfinite(vpX) || !std::isfinite(vpY) || !(vpW > 0) || !(vpH > 0) || vpW > 8192 || vpH > 8192)
            return no("a viewport that is not a finite rectangle of positive size");
    }
    float sysvals[8] = {vpW / 2.0f, vpH / 2.0f, 0.5f, 0.0f, vpX + vpW / 2.0f, vpY + vpH / 2.0f, 0.5f, 0.0f};
    // the Viewport descriptor's scissor, inclusive: the viewport's pixels, within the scissor rectangle and the target
    int64_t sMinX = static_cast<int64_t>(std::floor(vpX)), sMinY = static_cast<int64_t>(std::floor(vpY));
    int64_t sMaxX = static_cast<int64_t>(std::ceil(vpX + vpW)) - 1, sMaxY = static_cast<int64_t>(std::ceil(vpY + vpH)) - 1;
    if (in.scissor.set) {
        sMinX = std::max<int64_t>(sMinX, in.scissor.x), sMinY = std::max<int64_t>(sMinY, in.scissor.y);
        sMaxX = std::min<int64_t>(sMaxX, static_cast<int64_t>(in.scissor.x) + in.scissor.width - 1);
        sMaxY = std::min<int64_t>(sMaxY, static_cast<int64_t>(in.scissor.y) + in.scissor.height - 1);
    }
    sMinX = std::max<int64_t>(sMinX, 0), sMinY = std::max<int64_t>(sMinY, 0);
    sMaxX = std::min<int64_t>(sMaxX, t.width - 1), sMaxY = std::min<int64_t>(sMaxY, t.height - 1);
    if (sMinX > sMaxX || sMinY > sMaxY) return no("a scissor that leaves no pixel (not served: skip the draw)");
    memcpy(cpu + kSysvals, sysvals, sizeof(sysvals));
    hm_v7_uniform_buffer(cpu + kUbos + sz->uniform_buffer, gpu + kSysvals, sizeof(sysvals));
    bool anyDevice = false;
    for (uint32_t i = 0; i < DrawInput::kMaxDeviceBuffers; i++) {
        if (in.deviceBuffers[i] == nullptr) continue;
        if (in.deviceOffsets[i] > in.deviceBuffers[i]->size()) return no("a device buffer's offset past its end");
        uint64_t a = in.deviceBuffers[i]->gpu() + in.deviceOffsets[i];
        memcpy(cpu + D::kAirSysvals + 8 * i, &a, 8);
        anyDevice = true;
    }
    if (anyDevice) hm_v7_uniform_buffer(cpu + kUbos + 15 * sz->uniform_buffer, gpu + D::kAirSysvals, 288);
    auto word = [&](const hm_push_word& w) {
        return w.ubo == 0 ? static_cast<const uint8_t*>(in.uniforms) + w.offset
             : w.ubo == 15 ? cpu + D::kAirSysvals + w.offset : cpu + kSysvals + w.offset;
    };
    for (uint32_t i = 0; i < p.vs_push_count; i++) memcpy(cpu + kVsPush + 4 * i, word(p.vs_push[i]), 4);
    for (uint32_t i = 0; i < p.fs_push_count; i++) memcpy(cpu + kFsPush + 4 * i, word(p.fs_push[i]), 4);

    // texture 0 and sampler 0 (GENX(panfrost_new_texture), panfrost_create_sampler_state)
    if (p.texture_count != 0) {
        hm_v7_surface_with_stride(cpu + kSurface, in.texture->gpu() + in.textureOffset, static_cast<int32_t>(in.textureRowBytes), 0);
        for (uint32_t l = 1; l < in.textureLevels; l++) {
            hm_v7_surface_with_stride(cpu + kSurface + l * sz->surface_with_stride, in.texture->gpu() + in.textureLevelOffset[l],
                                      static_cast<int32_t>(in.textureLevelRowBytes[l]), 0);
        }
        const uint32_t texFormat = in.textureFormat == Format::R8Unorm ? hm_formats[2].texture_format
                                 : in.textureFormat == Format::RG8Unorm ? hm_formats[3].texture_format
                                 : hm_texture_format_rgba8;
        if (in.textureLevels == 1 && in.mip == HM_MIP_NONE) {
            hm_v7_texture_2d(cpu + kTexture, texFormat, hm_texture_swizzle_identity, in.textureWidth,
                             in.textureHeight, gpu + kSurface);
            hm_v7_sampler(cpu + kSampler, in.nearest);
        } else {
            hm_v7_texture_2d_levels(cpu + kTexture, texFormat, hm_texture_swizzle_identity, in.textureWidth,
                                    in.textureHeight, in.textureLevels, gpu + kSurface);
            hm_v7_sampler_full(cpu + kSampler, in.nearest, in.nearest, in.mip, in.minLod,
                               std::min(in.maxLod, static_cast<float>(in.textureLevels - 1)));
        }
    }
    hm_v7_viewport(cpu + kViewport, static_cast<unsigned>(sMinX), static_cast<unsigned>(sMinY),
                   static_cast<unsigned>(sMaxX), static_cast<unsigned>(sMaxY), 0.0f, 1.0f);
    hm_v7_local_storage(cpu + kLocalStorage);
    struct hm_v7_draw vertex = {};
    vertex.state = gpu + kVsRsd;
    vertex.attributes = gpu + kAttributes;
    vertex.attribute_buffers = gpu + kAttributeBuffers;
    vertex.varyings = gpu + kVaryings;
    vertex.varying_buffers = gpu + kVaryingBuffers;
    vertex.thread_storage = gpu + kLocalStorage;
    vertex.push_uniforms = p.vs_push_count ? gpu + kVsPush : 0;
    vertex.uniform_buffers = gpu + kUbos;
    vertex.instance_count = in.instanceCount;
    vertex.padded_count = padded;
    hm_v7_vertex_job(cpu + kVertexJob, vIndex, gpu + kTilerJob, in.vertexCount, &vertex);
    struct hm_v7_draw fragment = {};
    fragment.state = gpu + kFsRsd;
    fragment.position = gpu + kPositions;
    fragment.viewport = gpu + kViewport;
    fragment.varyings = p.fs_in_count ? gpu + kVaryings + p.vs_out_count * sz->attribute : 0;
    fragment.varying_buffers = gpu + kVaryingBuffers;
    fragment.thread_storage = gpu + kLocalStorage;
    fragment.push_uniforms = p.fs_push_count ? gpu + kFsPush : 0;
    fragment.uniform_buffers = gpu + kUbos;
    fragment.textures = p.texture_count ? gpu + kTexture : 0;
    fragment.samplers = p.sampler_count ? gpu + kSampler : 0;
    fragment.cull_front = in.cull == DrawInput::Cull::Front;
    fragment.cull_back = in.cull == DrawInput::Cull::Back;
    // the hardware's winding is the target's with row 0 at the top (run 515: CCW front kept the triangle that is
    // clockwise in NDC); here row 0 is NDC y -1, so the API's winding, which is NDC's, is the other one -- as the state
    // tracker turns rast->front_ccw over for a framebuffer whose y is the other way (st_update_rasterizer)
    fragment.front_ccw = !in.frontCCW;
    fragment.instance_count = in.instanceCount;
    fragment.padded_count = padded;
    if (in.indices != nullptr) {
        memcpy(cpu + kIndices, in.indices, 2 * in.indexCount);
        hm_v7_tiler_job_indexed(cpu + kTilerJob, vIndex + 1, vIndex, next, in.vertexCount, tilerCtx, &fragment, in.indexCount,
                                gpu + kIndices);
    } else {
        hm_v7_tiler_job_mode(cpu + kTilerJob, vIndex + 1, vIndex, next, in.vertexCount, tilerCtx, &fragment, 0, 0,
                             in.primitive == DrawInput::Primitive::Lines ? 2 : in.primitive == DrawInput::Primitive::Points ? 1 : 8);
    }

    if (prevTiler != 0) {
        // dependency 2: the tiler job before it (pan_jc_add_job), in Job Header word 5 bits 16..31
        uint32_t w5;
        memcpy(&w5, cpu + kTilerJob + 20, 4);
        w5 |= prevTiler << 16;
        memcpy(cpu + kTilerJob + 20, &w5, 4);
    }
    return std::string();
}

std::unique_ptr<DrawPass> DrawPass::make(Device& device, const Target& t, const hm_program& p, const DrawInput& in,
                                         std::string* why) {
    return makeMany(device, t, {{&p, in}}, why);
}

std::unique_ptr<DrawPass> DrawPass::makeMany(Device& device, const Target& t,
                                             const std::vector<std::pair<const hm_program*, DrawInput>>& draws,
                                             std::string* why) {
    auto no = [why](const std::string& s) {
        if (why) *why = s;
        return std::unique_ptr<DrawPass>();
    };
    if (draws.empty() || draws.size() > kMaxDraws) return no("a pass of 1 to " + std::to_string(kMaxDraws) + " draws");
    if (t.bo == nullptr || t.width == 0 || t.height == 0 || t.width > 4096 || t.height > 4096)
        return no("a draw goes into a target of 1 to 4096 pixels a side");
    uint64_t base = t.bo->gpu() + t.offset;
    const uint32_t bpp = bytesPerPixel(t.format);
    if ((base & 63) != 0 || (t.rowBytes & 63) != 0 || t.rowBytes < bpp * t.width ||
        t.offset + static_cast<uint64_t>(t.rowBytes) * (t.height - 1) + static_cast<uint64_t>(bpp) * t.width > t.bo->size())
        return no("the target is not 64-byte aligned rows inside its buffer (pan_layout.c, v7)");
    std::unique_ptr<DrawPass> pass(new DrawPass(device));
    pass->target_ = t;
    pass->draws_ = static_cast<uint32_t>(draws.size());
    for (const auto& d : draws) {
        if (d.first->texture_count && d.second.texture) pass->textures_.push_back(d.second.texture);
        for (const Bo* b : d.second.deviceBuffers) {
            if (b) pass->textures_.push_back(b);
        }
    }
    pass->cmd_ = device.gpu().newBo(kSize * draws.size(), why);
    if (!pass->cmd_) return nullptr;
    pass->heap_ = device.tilerHeap(why);
    if (pass->heap_ == nullptr) return nullptr;
    uint8_t* cpu = pass->cmd_->cpu();
    uint64_t gpu = pass->cmd_->gpu();
    // the draws, each in a block of its own, chained vertex, tiler, vertex, tiler...; the frame's own descriptors in
    // the first block's room for them
    for (size_t i = 0; i < draws.size(); i++) {
        uint64_t next = i + 1 < draws.size() ? gpu + (i + 1) * kSize + kVertexJob : 0;
        unsigned vIndex = static_cast<unsigned>(2 * i + 1);
        std::string e = writeDraw(device, t, *draws[i].first, draws[i].second, cpu + i * kSize, gpu + i * kSize,
                                  gpu + kTilerCtx, vIndex, i ? vIndex - 1 : 0, next);
        if (!e.empty()) return no("draw " + std::to_string(i) + ", " + e);
    }
    const DrawInput& in = draws[0].second;
    hm_v7_tiler_heap(cpu + kHeapDesc, pass->heap_->gpu(), static_cast<uint32_t>(pass->heap_->size()));
    hm_v7_tiler_context(cpu + kTilerCtx, gpu + kHeapDesc, t.width, t.height,
                        hm_v7_hierarchy_mask(t.width, t.height, device.tilerLevels()));

    struct hm_v7_fb fb = {};
    fb.width = t.width;
    fb.height = t.height;
    fb.bound_max_x = t.width - 1;
    fb.bound_max_y = t.height - 1;
    fb.sample_locations = device.samplePositions().gpu();
    fb.tiler = gpu + kTilerCtx;
    hm_v7_tile_size(4, device.tileBufferBudget(), &fb.tile_size, &fb.cbuf_allocation);
    fb.rt_count = 1;
    fb.z_clear = 1.0f;
    fb.s_clear = in.stencilClear;
    struct hm_v7_rt rt = {};
    rt.format = rtFormat(t.format);
    rt.base = base;
    rt.row_stride = t.rowBytes;
    rt.write_enable = true;
    rt.clear = true;
    rt.clean_pixel_write = true;
    rt.dithering = true;
    hm_v7_pack_clear_rgba8(in.clear, rt.clear_words);
    const char* refused = nullptr;
    uint64_t fbd = hm_v7_framebuffer(cpu + kFbd, gpu + kFbd, &fb, &rt, &refused);
    if (fbd == 0) return no(refused);
    hm_v7_fragment_job(cpu + kFragmentJob, 1, 0, fbd, 0, 0, (t.width - 1) >> 4, (t.height - 1) >> 4);
    return pass;
}

const float* DrawPass::positionsWritten() const { return reinterpret_cast<const float*>(cmd_->cpu() + kPositions); }

int DrawPass::submit(Submission* draw, Submission* frame) {
    std::vector<const Bo*> uses = {cmd_.get(), target_.bo, &device_.samplePositions(), heap_};
    for (const Bo* tex : textures_) uses.push_back(tex);
    int error = device_.gpu().submit(cmd_->gpu() + kVertexJob, false, uses, draw);
    if (error == 0) error = device_.gpu().submit(cmd_->gpu() + kFragmentJob, true, uses, frame);
    return error;
}

static uint32_t statusOf(const uint8_t* job) {
    struct hm_v7_job h;
    hm_v7_read_job(job, &h, nullptr);
    return h.exception_status;
}
uint32_t DrawPass::vertexStatus() const { return statusOf(cmd_->cpu() + kVertexJob); }
uint32_t DrawPass::tilerStatus() const { return statusOf(cmd_->cpu() + kTilerJob); }
uint32_t DrawPass::fragmentStatus() const { return statusOf(cmd_->cpu() + kFragmentJob); }

void DrawPass::dump(FILE* fp) const {
    fprintf(fp, "Vertex job:\n");
    hm_v7_print_job(fp, cmd_->cpu() + kVertexJob);
    fprintf(fp, "Tiler job:\n");
    hm_v7_print_job(fp, cmd_->cpu() + kTilerJob);
    fprintf(fp, "Fragment job:\n");
    hm_v7_print_job(fp, cmd_->cpu() + kFragmentJob);
    const float* g = reinterpret_cast<const float*>(cmd_->cpu() + kGeneral);
    const float* q = reinterpret_cast<const float*>(cmd_->cpu() + kPositions);
    fprintf(fp, "position 0 (%g %g %g %g), general varyings of vertex 0: %g %g %g %g\n", q[0], q[1], q[2], q[3], g[0],
            g[1], g[2], g[3]);
}

std::unique_ptr<ComputePass> ComputePass::make(Device& device, const hm_kernel& k, const unsigned groups[3],
                                               const void* uniforms, uint32_t uniformSize, std::string* why) {
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ubos;
    if (uniformSize != 0) {
        ubos.emplace_back(0, std::vector<uint8_t>(static_cast<const uint8_t*>(uniforms),
                                                  static_cast<const uint8_t*>(uniforms) + uniformSize));
    }
    return makeWith(device, k, groups, k.local, ubos, why);
}

std::unique_ptr<ComputePass> ComputePass::makeWith(Device& device, const hm_kernel& k, const unsigned groups[3],
                                                   const unsigned local[3],
                                                   const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& ubos,
                                                   std::string* why) {
    auto no = [why, &k](const std::string& s) {
        if (why) *why = std::string(k.name) + ": " + s;
        return std::unique_ptr<ComputePass>();
    };
    const struct hm_v7_sizes* sz = hm_v7_sizes();
    for (int i = 0; i < 3; i++) {
        if (groups[i] == 0 || groups[i] > 65535) return no("a workgroup count outside 1..65535");
        if (local[i] == 0 || local[i] > 256) return no("a threadgroup size outside 1..256");
    }
    if (local[0] * local[1] * local[2] > 256) return no("more than 256 threads a threadgroup");
    if (k.tls_size != 0) return no("a kernel that spills to thread storage (not served yet)");
    if (k.size > kUniforms - kCode) return no("the kernel does not fit");
    uint32_t tableEntries = k.ubo_count ? k.ubo_count : 1;
    if (tableEntries * sz->uniform_buffer > kPush - kUbos) return no("too many uniform buffers");
    if (k.push_count * 4 > kCode - kPush) return no("too many pushed words");
    // where each buffer's bytes go, 16-aligned, and every pushed word inside one of them
    std::vector<std::pair<uint32_t, uint32_t>> at;   // (ubo index, offset in the command buffer)
    uint32_t next = kUniforms;
    for (const auto& u : ubos) {
        if (u.first >= tableEntries) return no("uniform buffer " + std::to_string(u.first) + " is past the kernel's table");
        if (next + u.second.size() > kSize) return no("the uniform data do not fit");
        at.emplace_back(u.first, next);
        next = (next + static_cast<uint32_t>(u.second.size()) + 15) & ~15u;
    }
    auto source = [&](const hm_push_word& w) -> const uint8_t* {
        for (size_t i = 0; i < ubos.size(); i++) {
            if (ubos[i].first == w.ubo && w.offset + 4u <= ubos[i].second.size()) return ubos[i].second.data() + w.offset;
        }
        return nullptr;
    };
    for (uint32_t i = 0; i < k.push_count; i++) {
        if (source(k.push[i]) == nullptr)
            return no("the kernel pushes word " + std::to_string(k.push[i].offset) + " of uniform buffer " +
                      std::to_string(k.push[i].ubo) + ", which the data do not hold");
    }
    std::unique_ptr<ComputePass> pass(new ComputePass(device));
    pass->cmd_ = device.gpu().newBo(kSize, why);
    if (!pass->cmd_) return nullptr;
    uint8_t* cpu = pass->cmd_->cpu();
    uint64_t gpu = pass->cmd_->gpu();
    memset(cpu, 0, kSize);
    memcpy(cpu + kCode, k.code, k.size);
    uint32_t rsd[16];
    memcpy(rsd, k.rsd, sizeof(rsd));
    rsd[0] |= static_cast<uint32_t>(gpu + kCode);
    rsd[1] |= static_cast<uint32_t>((gpu + kCode) >> 32);
    memcpy(cpu + kRsd, rsd, sizeof(rsd));
    for (size_t i = 0; i < ubos.size(); i++) {
        memcpy(cpu + at[i].second, ubos[i].second.data(), ubos[i].second.size());
        hm_v7_uniform_buffer(cpu + kUbos + at[i].first * sz->uniform_buffer, gpu + at[i].second,
                             static_cast<uint32_t>(ubos[i].second.size()));
    }
    for (uint32_t i = 0; i < k.push_count; i++) memcpy(cpu + kPush + 4 * i, source(k.push[i]), 4);
    if (k.wls_size != 0) {
        // panfrost_emit_shared_memory: pan_wls_adjust_size(size) * the instances (the grid to powers of two,
        // panfrost_choose_wls_instance_count) * the core id range
        auto pow2 = [](unsigned v) { unsigned p = 1; while (p < v) p <<= 1; return p; };
        unsigned size = pow2(k.wls_size < 128 ? 128 : k.wls_size);
        unsigned instances = pow2(groups[0]) * pow2(groups[1]) * pow2(groups[2]);
        uint64_t bytes = static_cast<uint64_t>(size) * instances * device.coreIdRange();
        if (bytes > (64u << 20)) return no("more than 64 MB of workgroup storage");
        pass->wls_ = device.gpu().newBo(bytes, why);
        if (!pass->wls_) return nullptr;
        hm_v7_local_storage_wls(cpu + kLocalStorage, pass->wls_->gpu(), size, instances);
    } else {
        hm_v7_local_storage(cpu + kLocalStorage);
    }
    struct hm_v7_draw d = {};
    d.state = gpu + kRsd;
    d.thread_storage = gpu + kLocalStorage;
    d.uniform_buffers = gpu + kUbos;
    d.push_uniforms = k.push_count ? gpu + kPush : 0;
    hm_v7_compute_job(cpu + kJob, 1, 0, groups, local, &d);
    return pass;
}

int ComputePass::submit(const std::vector<const Bo*>& uses, Submission* out) {
    std::vector<const Bo*> all = uses;
    all.push_back(cmd_.get());
    if (wls_) all.push_back(wls_.get());
    return device_.gpu().submit(cmd_->gpu() + kJob, false, all, out);
}

uint32_t ComputePass::status() const { return statusOf(cmd_->cpu() + kJob); }

void ComputePass::dump(FILE* fp) const {
    fprintf(fp, "Compute job:\n");
    hm_v7_print_job(fp, cmd_->cpu() + kJob);
}

}  // namespace hakios::mali
