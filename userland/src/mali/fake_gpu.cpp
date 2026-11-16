// The fake GPU (fake_gpu.hpp).
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>

#include "fake_gpu.hpp"
#include "v7.h"

namespace hakios::mali {

namespace {

constexpr uint64_t kPage = 16384;               // the kernel's pages (round_page_64)
constexpr uint64_t kGranule = 64 * 1024;        // HakiosMali's GPU address granules (mtk_mali.c GPU_GRANULE_SHIFT)
constexpr uint64_t kGpuEnd = 1ull << 32;        // ... all below 4 GB
constexpr uint32_t kTileBuffer = 16384;         // the G52's tile buffer (pan_props.c)

constexpr uint32_t kDone = 0x01;
constexpr uint32_t kJobConfigFault = 0x40;
constexpr uint32_t kDataInvalidFault = 0x58;
constexpr uint32_t kTileRangeFault = 0x59;
constexpr uint32_t kTranslationFault = 0xc1;    // TRANSLATION_FAULT, level 1 (the fake has no levels)
constexpr uint32_t kAccessRead = 1u << 8, kAccessWrite = 3u << 8;   // v7.xml "Exception Access" in bits 8..9

std::string hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "%#llx", static_cast<unsigned long long>(v));
    return b;
}

}  // namespace

FakeGpu::FakeGpu() {
    // what the phone's GPU answered in run 500: a Mali-G52 r1 (0x7402, r1p0), two shader cores, 2^9-byte tiler bins
    // and two hierarchy levels (TILER_FEATURES 0x209)
    params = {
        {kParamGpuProdId, kPhoneProductId},
        {kParamGpuRevision, 0x1000},
        {kParamShaderPresent, 0x3},
        {kParamTilerPresent, 0x1},
        {kParamL2Present, 0x1},
        {kParamTilerFeatures, 0x209},
        {kParamThreadTlsAlloc, 0x400},
    };
}

FakeGpu::~FakeGpu() = default;

size_t FakeGpu::mappedBuffers() const {
    size_t n = 0;
    for (const auto& kv : buffers_) {
        n += kv.second.mapped;
    }
    return n;
}

uint8_t* FakeGpu::at(uint64_t gpu, uint64_t length) {
    for (auto& kv : buffers_) {
        Buffer& b = kv.second;
        if (gpu >= b.gpu && length <= b.size && gpu - b.gpu <= b.size - length) {
            return b.mem + (gpu - b.gpu);
        }
    }
    return nullptr;
}

int FakeGpu::call(uint32_t request, void* data, size_t size) {
    switch (request) {
    case kGetParam: {
        if (size < sizeof(PanfrostGetParam)) return EINVAL;
        auto* p = static_cast<PanfrostGetParam*>(data);
        auto it = params.find(p->param);
        if (it == params.end()) return EINVAL;
        p->value = it->second;
        return 0;
    }
    case kCreateBo: {
        if (size < sizeof(PanfrostCreateBo)) return EINVAL;
        auto* c = static_cast<PanfrostCreateBo*>(data);
        uint64_t bytes = (static_cast<uint64_t>(c->size) + kPage - 1) & ~(kPage - 1);
        if (bytes == 0 || bytes > (256ull << 20)) return EINVAL;
        uint64_t span = (bytes + kGranule - 1) & ~(kGranule - 1);
        if (nextGpu_ + span > kGpuEnd) return ENOMEM;
        Buffer b;
        b.raw.reset(new uint8_t[bytes + kPage]());
        b.mem = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(b.raw.get()) + kPage - 1) & ~(kPage - 1));
        b.gpu = nextGpu_;
        b.size = bytes;
        nextGpu_ += span;
        c->handle = nextHandle_++;
        c->offset = b.gpu;
        buffers_[c->handle] = std::move(b);
        return 0;
    }
    case kGetBoOffset: {
        auto* o = static_cast<PanfrostGetBoOffset*>(data);
        auto it = buffers_.find(o->handle);
        if (it == buffers_.end()) return ENOENT;
        o->offset = it->second.gpu;
        return 0;
    }
    case kMmapBo: {
        auto* m = static_cast<PanfrostMmapBo*>(data);
        if (buffers_.find(m->handle) == buffers_.end()) return ENOENT;
        m->offset = static_cast<uint64_t>(m->handle) << 12;
        return 0;
    }
    case kGemClose: {
        auto* g = static_cast<DrmGemClose*>(data);
        auto it = buffers_.find(g->handle);
        if (it == buffers_.end()) return ENOENT;
        buffers_.erase(it);
        return 0;
    }
    case kSubmit: {
        if (size < sizeof(PanfrostSubmit)) return EINVAL;
        auto* s = static_cast<PanfrostSubmit*>(data);
        if (s->jc == 0) {
            return 0;                     // HakiosMali queues nothing and gives back 0
        }
        uint64_t seq = ++seq_;
        runChain(s->jc, (s->requirements & kJdReqFs) != 0, seq);
        s->jc = seq;
        return 0;
    }
    case kHakiosWait: {
        auto* w = static_cast<HakiosWait*>(data);
        return w->seq <= seq_ ? 0 : ETIMEDOUT;
    }
    case kWaitBo:
        return buffers_.count(static_cast<PanfrostWaitBo*>(data)->handle) ? 0 : ENOENT;
    case kMadvise:
        return 0;
    case kHakiosSurface:
        return ENOENT;                    // the fake has no IOSurfaces
    case kHakiosShow: {
        auto* sh = static_cast<HakiosShow*>(data);
        return sh->width == 0 || sh->width > 4096 || sh->height == 0 || sh->height > 4096 || sh->rowBytes < 4 * sh->width
                   ? EINVAL : 0;
    }
    default:
        return ENOTSUP;
    }
}

void* FakeGpu::map(uint32_t handle, uint64_t* size) {
    auto it = buffers_.find(handle);
    if (it == buffers_.end() || *size > it->second.size) return nullptr;
    it->second.mapped = true;
    *size = it->second.size;
    return it->second.mem;
}

void FakeGpu::unmap(uint32_t handle, void* address, uint64_t) {
    auto it = buffers_.find(handle);
    if (it != buffers_.end() && it->second.mem == address) {
        it->second.mapped = false;
    }
}

void FakeGpu::runChain(uint64_t jc, bool fragment, uint64_t seq) {
    std::set<uint64_t> seen;
    std::set<unsigned> done;      // the job indices run so far (a dependency must name one)
    unsigned n = 0;
    for (uint64_t address = jc; address != 0; n++) {
        Fault f;
        f.seq = seq;
        f.job = address;
        uint8_t* job = (address & 63) == 0 ? at(address, 32) : nullptr;
        if ((address & 63) != 0) {
            f.why = "job " + std::to_string(n + 1) + " at " + hex(address) + " is not 64-byte aligned (Job Header align=64)";
            f.status = 0;
            faults_.push_back(f);
            return;
        }
        if (job == nullptr) {
            f.why = "job " + std::to_string(n + 1) + " at " + hex(address) + " is outside every buffer";
            faults_.push_back(f);
            return;
        }
        if (!seen.insert(address).second || n >= 1024) {
            f.why = "the chain comes back to the job at " + hex(address);
            f.status = kJobConfigFault;
            hm_v7_set_exception_status(job, f.status);
            faults_.push_back(f);
            return;
        }
        struct hm_v7_job h;
        const char* invalid = nullptr;
        uint32_t status = 0;
        std::string why;
        if (hm_v7_read_job(job, &h, &invalid) != 0) {
            status = kJobConfigFault;
            why = invalid;
        } else if (!h.is_64b) {
            status = kJobConfigFault;
            why = "Is 64b is false: this GPU's descriptors are the 64-bit ones";
        } else if (h.exception_status != 0) {
            status = kJobConfigFault;
            why = "the job's Exception Status is " + hex(h.exception_status) + ", not 0: it was run before";
        } else if (h.index == 0) {
            status = kJobConfigFault;
            why = "the job's Index is 0 (pan_jc_add_job numbers jobs from 1)";
        } else if (h.type == HM_JOB_FRAGMENT && !fragment) {
            status = kJobConfigFault;
            why = "a Fragment job in a chain sent without PANFROST_JD_REQ_FS: the kernel puts that chain on slot 1";
        } else if (h.type != HM_JOB_FRAGMENT && fragment) {
            status = kJobConfigFault;
            why = "a job of type " + std::to_string(h.type) + " in a chain sent with PANFROST_JD_REQ_FS (slot 0 takes "
                  "Fragment jobs)";
        } else if (h.type == HM_JOB_WRITE_VALUE) {
            status = runWriteValue(job, &why);
        } else if (h.type == HM_JOB_FRAGMENT) {
            status = runFragment(job, &why);
        } else if (h.type == HM_JOB_COMPUTE) {
            status = runDraw(job, false, &why, true);
        } else if (h.type == HM_JOB_VERTEX || h.type == HM_JOB_TILER) {
            if ((h.dependency_1 != 0 && !done.count(h.dependency_1)) || (h.dependency_2 != 0 && !done.count(h.dependency_2))) {
                status = kJobConfigFault;
                why = "the job depends on a job index the chain has not run before it";
            } else {
                status = runDraw(job, h.type == HM_JOB_TILER, &why);
            }
        } else if (h.type == HM_JOB_NULL) {
            status = 0;
        } else {
            status = kJobConfigFault;
            why = "the fake GPU does not run jobs of type " + std::to_string(h.type);
        }
        if (status != 0) {
            f.status = status;
            f.why = "job " + std::to_string(n + 1) + " at " + hex(address) + ": " + why;
            hm_v7_set_exception_status(job, status);
            faults_.push_back(f);
            return;                       // the kernel resets the GPU; the chain ends here
        }
        hm_v7_set_exception_status(job, kDone);
        jobsRun_++;
        done.insert(h.index);
        address = h.next;
    }
}

// A vertex or tiler job: everything its draw names inside a buffer, its Renderer State's shader too; a tiler job's
// Tiler Context as a fragment job's. Nothing is run: the fragment job of a frame whose Tiler Context got primitives
// writes its clear colour alone, and says so (drawsNotRasterized).
uint32_t FakeGpu::runDraw(uint8_t* job, bool tiler, std::string* why, bool compute) {
    const struct hm_v7_sizes* sz = hm_v7_sizes();
    const char* invalid = nullptr;
    struct hm_v7_draw_read d;
    if (hm_v7_read_draw(job, tiler, &d, &invalid) != 0) {
        *why = invalid;
        return kJobConfigFault;
    }
    const char* kind = tiler ? "the tiler job's" : compute ? "the compute job's" : "the vertex job's";
    struct {
        uint64_t address;
        uint64_t length;
        const char* what;
    } named[] = {
        {d.draw.state, sz->renderer_state + (tiler ? sz->blend : 0), "Renderer State"},
        {d.draw.attributes, sz->attribute, "Attributes"},
        {d.draw.attribute_buffers, sz->attribute_buffer, "Attribute Buffers"},
        {d.draw.varyings, sz->attribute, "Varyings"},
        {d.draw.varying_buffers, sz->attribute_buffer, "Varying Buffers"},
        {d.draw.position, 16ull * (d.invocations + 1), "Position"},
        {d.draw.viewport, sz->viewport, "Viewport"},
        {d.draw.thread_storage, sz->local_storage, "Thread Storage"},
        {d.draw.push_uniforms, 4, "Push Uniforms"},
        {d.draw.uniform_buffers, 8, "Uniform Buffers"},
        {d.draw.textures, 32, "Textures"},
        {d.draw.samplers, 32, "Samplers"},
    };
    for (const auto& n : named) {
        if (n.address != 0 && at(n.address, n.length) == nullptr) {
            *why = std::string(kind) + " " + n.what + " at " + hex(n.address) + " is outside every buffer";
            return kTranslationFault | kAccessRead;
        }
    }
    if (d.draw.state == 0 || (d.draw.state & 63) != 0) {
        *why = std::string(kind) + " Renderer State is missing or not 64-byte aligned";
        return kJobConfigFault;
    }
    const uint8_t* rsd = at(d.draw.state, sz->renderer_state);
    uint64_t shader;
    memcpy(&shader, rsd, 8);
    if (at(shader, 64) == nullptr) {
        *why = std::string(kind) + " shader at " + hex(shader) + " is outside every buffer";
        return kTranslationFault | (2u << 8);
    }
    if (d.draw.thread_storage == 0) {
        *why = std::string(kind) + " draw has no Thread Storage (Panfrost always gives the batch's)";
        return kJobConfigFault;
    }
    if (tiler) {
        if (d.draw.position == 0 || d.draw.viewport == 0) {
            *why = "the tiler job has no Position or no Viewport";
            return kJobConfigFault;
        }
        if ((d.draw_mode != 8 && d.draw_mode != 2 && d.draw_mode != 1) || (d.index_type != 0 && d.index_type != 2)) {
            *why = "a draw mode or index type the fake does not model (triangles, no indices or 16-bit: 8, 0 or 2)";
            return kJobConfigFault;
        }
        uint8_t* ctx = (d.tiler & 63) == 0 ? at(d.tiler, sz->tiler_context) : nullptr;
        if (ctx == nullptr) {
            *why = "the tiler job's Tiler Context at " + hex(d.tiler) + " is not a 64-aligned descriptor inside a buffer";
            return kTranslationFault | kAccessRead;
        }
        drawn_.insert(d.tiler);
    } else if (compute) {
        computesNotRun_++;          // the fake runs no shader
    } else if (d.draw.varyings == 0 || d.draw.varying_buffers == 0) {
        *why = "the vertex job writes no varyings (no position for the tiler)";
        return kJobConfigFault;
    }
    return 0;
}

uint32_t FakeGpu::runWriteValue(uint8_t* job, std::string* why) {
    struct hm_v7_write_value w;
    const char* invalid = nullptr;
    if (hm_v7_read_write_value(job, &w, &invalid) != 0) {
        *why = invalid;
        return kJobConfigFault;
    }
    unsigned bytes;
    switch (w.type) {
    case 3: bytes = 8; w.immediate = 0; break;   // Zero
    case 4: bytes = 1; break;                    // Immediate 8
    case 5: bytes = 2; break;
    case 6: bytes = 4; break;
    case 7: bytes = 8; break;
    default:
        *why = "a Write Value of type " + std::to_string(w.type) + " (the fake writes immediates and zero)";
        return kJobConfigFault;
    }
    if (w.address % bytes != 0) {
        *why = "a Write Value of " + std::to_string(bytes) + " bytes at the unaligned " + hex(w.address);
        return kDataInvalidFault;
    }
    uint8_t* target = at(w.address, bytes);
    if (target == nullptr) {
        *why = "a Write Value to " + hex(w.address) + ", outside every buffer";
        return kTranslationFault | kAccessWrite;
    }
    memcpy(target, &w.immediate, bytes);
    return 0;
}

uint32_t FakeGpu::runFragment(uint8_t* job, std::string* why) {
    const struct hm_v7_sizes* sz = hm_v7_sizes();
    const char* invalid = nullptr;
    struct hm_v7_fragment frag;
    if (hm_v7_read_fragment(job, &frag, &invalid) != 0) {
        *why = invalid;
        return kJobConfigFault;
    }
    if (frag.has_tile_enable_map) {
        *why = "a tile enable map (the fake does not model it)";
        return kJobConfigFault;
    }
    struct hm_v7_fbd_pointer tag;
    hm_v7_read_fbd_pointer(frag.framebuffer, &tag);
    if (!tag.type) {
        *why = "the framebuffer pointer's Type bit is 0 (Midgard's single-target descriptor)";
        return kJobConfigFault;
    }
    uint64_t length = sz->framebuffer + (tag.zs_crc_extension ? sz->zs_crc_extension : 0) +
                      static_cast<uint64_t>(tag.rt_count) * sz->render_target;
    uint8_t* fbd = at(tag.pointer, length);
    if (fbd == nullptr) {
        *why = "the framebuffer descriptor at " + hex(tag.pointer) + " (" + std::to_string(length) +
               " bytes with its targets) is not inside one buffer";
        return kTranslationFault | kAccessRead;
    }
    struct hm_v7_fb_read fb;
    if (hm_v7_read_framebuffer(fbd, &fb, &invalid) != 0) {
        *why = invalid;
        return kJobConfigFault;
    }
    if (fb.fb.rt_count != tag.rt_count) {
        *why = "the framebuffer pointer says " + std::to_string(tag.rt_count) + " render targets, the descriptor " +
               std::to_string(fb.fb.rt_count);
        return kJobConfigFault;
    }
    if (fb.has_zs_crc_extension != tag.zs_crc_extension) {
        *why = "the framebuffer pointer and the descriptor disagree on the ZS CRC extension";
        return kJobConfigFault;
    }
    if (fb.has_zs_crc_extension || fb.z_write_enable || fb.s_write_enable || fb.crc_read_enable || fb.crc_write_enable) {
        *why = "depth, stencil or CRC (the fake does not model them)";
        return kJobConfigFault;
    }
    if (fb.pre_frame_0 != 0 || fb.pre_frame_1 != 0 || fb.post_frame != 0) {
        *why = fb.frame_shader_dcds == 0 ? "a frame shader mode other than Never with no Frame Shader DCDs"
                                         : "a frame shader (the fake runs no shader)";
        return fb.frame_shader_dcds == 0 ? kTranslationFault | kAccessRead : kJobConfigFault;
    }
    if (at(fb.fb.sample_locations, sz->sample_positions) == nullptr) {
        *why = "the sample locations at " + hex(fb.fb.sample_locations) + " are outside every buffer (Bifrost reads "
               "them for every frame)";
        return kTranslationFault | kAccessRead;
    }
    if (!((fb.sample_count == 1 && fb.sample_pattern == 0) || (fb.sample_count == 4 && fb.sample_pattern == 2))) {
        *why = "a sample count and pattern the fake does not model (1 single-sampled, 4 ROTATED_4X_GRID)";
        return kJobConfigFault;
    }
    uint32_t tile = fb.effective_tile_size;
    if (tile < 16 || tile > 256 || (tile & (tile - 1)) != 0) {
        *why = "an effective tile size of " + std::to_string(tile) + " pixels (v7: a power of two up to 16 x 16, "
               "panfrost_max_effective_tile_size)";
        return kDataInvalidFault;
    }
    if (fb.fb.cbuf_allocation > kTileBuffer) {
        *why = "a colour buffer allocation of " + std::to_string(fb.fb.cbuf_allocation) + " bytes, more than the "
               "16 KB tile buffer";
        return kDataInvalidFault;
    }
    if (fb.fb.tiler != 0) {
        uint8_t* ctx = at(fb.fb.tiler, sz->tiler_context);
        if (ctx == nullptr || (fb.fb.tiler & 63) != 0) {
            *why = "the Tiler Context at " + hex(fb.fb.tiler) + " is not a 64-aligned descriptor inside a buffer";
            return kTranslationFault | kAccessRead;
        }
        struct hm_v7_tiler_read t;
        if (hm_v7_read_tiler_context(ctx, &t, &invalid) != 0) {
            *why = invalid;
            return kJobConfigFault;
        }
        if (t.fb_width != fb.fb.width || t.fb_height != fb.fb.height) {
            *why = "the Tiler Context's frame is " + std::to_string(t.fb_width) + "x" + std::to_string(t.fb_height) +
                   ", the framebuffer's " + std::to_string(fb.fb.width) + "x" + std::to_string(fb.fb.height);
            return kJobConfigFault;
        }
        if (t.hierarchy_mask == 0) {
            *why = "the Tiler Context's hierarchy mask is 0";
            return kJobConfigFault;
        }
        uint8_t* heap = (t.heap & 63) == 0 ? at(t.heap, sz->tiler_heap) : nullptr;
        if (heap == nullptr) {
            *why = "the Tiler Context's heap at " + hex(t.heap) + " is not a 64-aligned descriptor inside a buffer";
            return kTranslationFault | kAccessRead;
        }
        if (hm_v7_read_tiler_heap(heap, &t, &invalid) != 0) {
            *why = invalid;
            return kJobConfigFault;
        }
        if (t.heap_bottom < t.heap_base || t.heap_top < t.heap_bottom || t.heap_top - t.heap_base > t.heap_size ||
            at(t.heap_base, t.heap_top - t.heap_base) == nullptr) {
            *why = "the Tiler Heap [" + hex(t.heap_base) + ", " + hex(t.heap_top) + ") is not inside one buffer";
            return kTranslationFault | kAccessWrite;
        }
    }
    uint32_t last_tx = (fb.fb.width - 1) >> 4, last_ty = (fb.fb.height - 1) >> 4;
    if (frag.min_tx > frag.max_tx || frag.min_ty > frag.max_ty || frag.max_tx > last_tx || frag.max_ty > last_ty) {
        *why = "the job's tiles (" + std::to_string(frag.min_tx) + "," + std::to_string(frag.min_ty) + ")-(" +
               std::to_string(frag.max_tx) + "," + std::to_string(frag.max_ty) + ") are not inside the frame's (0,0)-(" +
               std::to_string(last_tx) + "," + std::to_string(last_ty) + ")";
        return kTileRangeFault;
    }

    // the targets: checked first, all of them, then written
    std::vector<struct hm_v7_rt_read> rts(tag.rt_count);
    uint32_t cbuf_needed = 0;
    for (unsigned i = 0; i < tag.rt_count; i++) {
        uint8_t* rtd = fbd + sz->framebuffer + i * sz->render_target;
        struct hm_v7_rt_read& r = rts[i];
        if (hm_v7_read_rt(rtd, &r, &invalid) != 0) {
            *why = "render target " + std::to_string(i) + ": " + invalid;
            return kJobConfigFault;
        }
        std::string which = "render target " + std::to_string(i) + ": ";
        if (r.yuv_enable || r.srgb || r.dithered_clear || r.writeback_msaa > 1 || (r.writeback_msaa == 1) != (fb.sample_count == 4)) {
            *why = which + "YUV, sRGB, a dithered clear, or a writeback that is not single for one sample and averaged "
                           "for four (the fake does not model the rest)";
            return kJobConfigFault;
        }
        if (r.internal_format != 1 || (r.writeback_format != 19 && r.writeback_format != 16 && r.writeback_format != 17)) {
            *why = which + "internal format " + std::to_string(r.internal_format) + ", writeback format " +
                   std::to_string(r.writeback_format) + " (the fake models R8G8B8A8 into R8G8B8A8, R8, R8G8: 1 and 19, 16, 17)";
            return kJobConfigFault;
        }
        if (r.rt.format == 0) {
            *why = which + "a swizzle of " + hex(r.swizzle) + " with writeback format " + std::to_string(r.writeback_format) +
                   " (the fake models RGBA, BGRA, R and RG)";
            return kJobConfigFault;
        }
        const uint32_t bpp = r.writeback_format == 16 ? 1 : r.writeback_format == 17 ? 2 : 4;
        if (r.rt.internal_buffer_offset + 4 * tile > fb.fb.cbuf_allocation) {
            *why = which + "its tile (" + std::to_string(4 * tile) + " bytes at " +
                   std::to_string(r.rt.internal_buffer_offset) + ") is past the colour buffer allocation of " +
                   std::to_string(fb.fb.cbuf_allocation);
            return kDataInvalidFault;
        }
        cbuf_needed += 4 * tile;
        if (!r.rt.write_enable || r.writeback_block_format == 0) {
            continue;
        }
        if (r.writeback_block_format != 2) {
            *why = which + "block format " + std::to_string(r.writeback_block_format) + " (the fake writes Linear: 2)";
            return kJobConfigFault;
        }
        if ((r.rt.base & 63) != 0 || (r.rt.row_stride & 63) != 0) {
            *why = which + "base " + hex(r.rt.base) + " or row stride " + std::to_string(r.rt.row_stride) +
                   " not 64-byte aligned (pan_layout.c, format_minimum_alignment on v7)";
            return kDataInvalidFault;
        }
        if (r.rt.row_stride < bpp * fb.fb.width) {
            *why = which + "a row stride of " + std::to_string(r.rt.row_stride) + " for rows of " +
                   std::to_string(bpp * fb.fb.width) + " bytes";
            return kDataInvalidFault;
        }
        uint64_t span = static_cast<uint64_t>(r.rt.row_stride) * (fb.fb.height - 1) + static_cast<uint64_t>(bpp) * fb.fb.width;
        if (at(r.rt.base, span) == nullptr) {
            *why = which + "its " + std::to_string(fb.fb.height) + " rows at " + hex(r.rt.base) + " (" +
                   std::to_string(span) + " bytes) are not inside one buffer";
            return kTranslationFault | kAccessWrite;
        }
    }
    if (cbuf_needed > fb.fb.cbuf_allocation) {
        *why = "the targets' tiles take " + std::to_string(cbuf_needed) + " bytes of tile buffer, the allocation is " +
               std::to_string(fb.fb.cbuf_allocation);
        return kDataInvalidFault;
    }

    if (fb.fb.tiler != 0 && drawn_.count(fb.fb.tiler)) {
        drawsNotRasterized_++;      // a tiler job put primitives in this frame: the fake does not draw them
    }
    // carried out: no primitive, so every tile is its clear colour, written back only by targets that write clean tiles,
    // inside the framebuffer's Bound Min/Max -- and not the job's tiles: on the phone a job over tiles (1,1)-(2,2) of a
    // 64x64 frame wrote all of it, while the framebuffer's bounds (16..47, and 5,7..40,50 off the tile grid) were kept
    // to the pixel (runs 501 and 502)
    uint32_t x0 = fb.fb.bound_min_x, y0 = fb.fb.bound_min_y;
    uint32_t x1 = fb.fb.bound_max_x, y1 = fb.fb.bound_max_y;     // inclusive
    if (x1 > fb.fb.width - 1) x1 = fb.fb.width - 1;
    if (y1 > fb.fb.height - 1) y1 = fb.fb.height - 1;
    for (const struct hm_v7_rt_read& r : rts) {
        if (!r.rt.write_enable || r.writeback_block_format == 0 || !r.rt.clean_pixel_write || x0 > x1 || y0 > y1) {
            continue;
        }
        // the tile buffer's R8G8B8A8 word (pan_pack_color: R in the low byte), then memory component c takes the
        // channel the swizzle names for it (MALI_CHANNEL: R G B A 0 1)
        uint32_t word = r.rt.clear_words[0];
        uint8_t channel[6] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word >> 16),
                              static_cast<uint8_t>(word >> 24), 0, 255};
        uint8_t pixel[4];
        for (unsigned c = 0; c < 4; c++) {
            unsigned ch = (r.swizzle >> (3 * c)) & 7;
            pixel[c] = ch < 6 ? channel[ch] : 0;
        }
        const uint32_t bpp = r.writeback_format == 16 ? 1 : r.writeback_format == 17 ? 2 : 4;
        uint8_t* base = at(r.rt.base, 1);
        for (uint32_t y = y0; y <= y1; y++) {
            for (uint32_t x = x0; x <= x1; x++) {
                memcpy(base + static_cast<uint64_t>(y) * r.rt.row_stride + static_cast<uint64_t>(bpp) * x, pixel, bpp);
            }
        }
        pixelsWritten_ += static_cast<uint64_t>(x1 - x0 + 1) * (y1 - y0 + 1);
    }
    return 0;
}

}  // namespace hakios::mali
