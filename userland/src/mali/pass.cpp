// The device, the cleared frame and the Write Value job (pass.hpp).
#include <cerrno>
#include <cmath>
#include <cstring>

#include "pass.hpp"
#include "v7.h"

namespace hakios::mali {

namespace {
constexpr uint32_t kHeapBytes = 2u << 20;       // the tiler heap: Panfrost takes 128 MB growable (pan_device.c); a
                                                // frame without primitives puts nothing in it
}

int Device::init(std::string* why) {
    if (gpu_.param(kParamGpuProdId, &productId_) != 0) {
        if (why) *why = "GET_PARAM GPU_PROD_ID failed";
        return -1;
    }
    model_ = modelOf(productId_);
    if (model_ == nullptr) {
        if (why) {
            char b[120];
            snprintf(b, sizeof(b), "the GPU is product %#llx, not a Bifrost v7 one of pan_props.c's table: these are v7 "
                     "descriptors", static_cast<unsigned long long>(productId_));
            *why = b;
        }
        return -1;
    }
    uint64_t features = 0;
    if (gpu_.param(kParamTilerFeatures, &features) != 0) {
        if (why) *why = "GET_PARAM TILER_FEATURES failed";
        return -1;
    }
    tilerLevels_ = (features >> 8) & 0xf;      // panfrost_query_tiler_features
    if (tilerLevels_ < 2) {
        if (why) *why = "TILER_FEATURES says " + std::to_string(tilerLevels_) + " hierarchy levels (Panfrost asserts >= 2)";
        return -1;
    }
    uint64_t cores = 0;
    if (gpu_.param(kParamShaderPresent, &cores) == 0 && cores != 0) {
        coreIdRange_ = 64 - static_cast<unsigned>(__builtin_clzll(cores));
    }
    samplePositions_ = gpu_.newBo(HM_V7_SAMPLE_POSITIONS_SIZE, why);
    if (!samplePositions_) {
        return -1;
    }
    hm_v7_sample_positions_table(samplePositions_->cpu());
    return 0;
}

const Bo* Device::tilerHeap(std::string* why) {
    if (!tilerHeap_) {
        tilerHeap_ = gpu_.newBo(kHeapBytes, why);
    }
    return tilerHeap_.get();
}

std::unique_ptr<ClearPass> ClearPass::make(Device& device, const Target& t, const float rgba[4],
                                           const ClearOptions& o, std::string* why) {
    auto no = [why](const std::string& s) {
        if (why) *why = s;
        return std::unique_ptr<ClearPass>();
    };
    if (t.bo == nullptr) return no("the target has no buffer");
    if (t.width == 0 || t.height == 0 || t.width > 4096 || t.height > 4096)
        return no("a target of " + std::to_string(t.width) + "x" + std::to_string(t.height) + " (1 to 4096 a side)");
    const uint32_t bpp = bytesPerPixel(t.format);
    if (t.rowBytes < bpp * t.width) return no("a row of " + std::to_string(t.rowBytes) + " bytes for " +
                                              std::to_string(t.width) + " pixels of " + std::to_string(bpp) + " bytes");
    uint64_t base = t.bo->gpu() + t.offset;
    // pan_layout.c: on v7 a linear RGBA8 image's offset and row stride are both 64-byte aligned (format_minimum_alignment)
    if ((base & 63) != 0) return no("the target's first pixel is not 64-byte aligned (pan_layout.c, v7)");
    if ((t.rowBytes & 63) != 0) return no("the target's row of " + std::to_string(t.rowBytes) +
                                          " bytes is not a multiple of 64 (pan_layout.c, v7)");
    uint64_t span = static_cast<uint64_t>(t.rowBytes) * (t.height - 1) + static_cast<uint64_t>(bpp) * t.width;
    if (t.offset > t.bo->size() || span > t.bo->size() - t.offset)
        return no("the target's " + std::to_string(t.height) + " rows do not fit in its buffer");
    for (int i = 0; i < 4; i++) {
        if (!std::isfinite(rgba[i])) return no("a clear colour that is not a finite number");
    }
    uint32_t lastTx = (t.width - 1) >> 4, lastTy = (t.height - 1) >> 4;
    uint32_t maxTx = o.maxTx == ~0u ? lastTx : o.maxTx, maxTy = o.maxTy == ~0u ? lastTy : o.maxTy;
    if (o.minTx > maxTx || o.minTy > maxTy || maxTx > lastTx || maxTy > lastTy)
        return no("tiles outside the frame's (a TILE_RANGE_FAULT: emit_fragment_job clamps them)");

    std::unique_ptr<ClearPass> pass(new ClearPass(device));
    pass->target_ = t;
    pass->cmd_ = device.gpu().newBo(4096, why);
    if (!pass->cmd_) return nullptr;
    if (o.withTiler) {
        pass->heap_ = device.tilerHeap(why);
        if (pass->heap_ == nullptr) return nullptr;
    }
    uint8_t* cpu = pass->cmd_->cpu();
    uint64_t gpu = pass->cmd_->gpu();
    memset(cpu, 0, 0x200);

    uint64_t tiler = 0;
    if (o.withTiler) {
        hm_v7_tiler_heap(cpu + kHeapDesc, pass->heap_->gpu(), static_cast<uint32_t>(pass->heap_->size()));
        hm_v7_tiler_context(cpu + kTilerCtx, gpu + kHeapDesc, t.width, t.height,
                            hm_v7_hierarchy_mask(t.width, t.height, device.tilerLevels()));
        tiler = gpu + kTilerCtx;
    }

    struct hm_v7_fb fb = {};
    fb.width = t.width;
    fb.height = t.height;
    fb.bound_min_x = o.boundMinX;
    fb.bound_min_y = o.boundMinY;
    fb.bound_max_x = o.boundMaxX == ~0u ? t.width - 1 : o.boundMaxX;
    fb.bound_max_y = o.boundMaxY == ~0u ? t.height - 1 : o.boundMaxY;
    fb.sample_locations = device.samplePositions().gpu();
    fb.tiler = tiler;
    hm_v7_tile_size(4, device.tileBufferBudget(), &fb.tile_size, &fb.cbuf_allocation);
    fb.rt_count = 1;
    struct hm_v7_rt rt = {};
    rt.format = rtFormat(t.format);
    rt.base = base;
    rt.row_stride = t.rowBytes;
    rt.internal_buffer_offset = 0;
    rt.write_enable = true;
    rt.clear = true;
    rt.clean_pixel_write = o.cleanPixelWrite;
    rt.dithering = true;        // pan_prepare_rt: "cfg->dithering_enable = true" for every written target
    hm_v7_pack_clear_rgba8(rgba, rt.clear_words);
    const char* refused = nullptr;
    uint64_t fbd = hm_v7_framebuffer(cpu + kFbd, gpu + kFbd, &fb, &rt, &refused);
    if (fbd == 0) return no(refused);
    hm_v7_fragment_job(cpu + kJob, 1, 0, fbd, o.minTx, o.minTy, maxTx, maxTy);

    uint32_t w = rt.clear_words[0];
    uint8_t r = w, g = w >> 8, b = w >> 16, a = w >> 24;
    if (t.format == Format::BGRA8Unorm) {
        pass->expected_[0] = b, pass->expected_[1] = g, pass->expected_[2] = r, pass->expected_[3] = a;
    } else {
        pass->expected_[0] = r, pass->expected_[1] = g, pass->expected_[2] = b, pass->expected_[3] = a;
    }
    return pass;
}

int ClearPass::submit(Submission* out) {
    std::vector<const Bo*> uses = {cmd_.get(), target_.bo, &device_.samplePositions()};
    if (heap_ != nullptr) {
        uses.push_back(heap_);
    }
    return device_.gpu().submit(jobAddress(), true, uses, out);
}

uint32_t ClearPass::status() const {
    struct hm_v7_job h;
    hm_v7_read_job(cmd_->cpu() + kJob, &h, nullptr);
    return h.exception_status;
}

void ClearPass::dump(FILE* fp) const {
    fprintf(fp, "Fragment job at %#llx:\n", static_cast<unsigned long long>(jobAddress()));
    hm_v7_print_job(fp, cmd_->cpu() + kJob);
    fprintf(fp, "Framebuffer at %#llx:\n", static_cast<unsigned long long>(cmd_->gpu() + kFbd));
    hm_v7_print_framebuffer(fp, cmd_->cpu() + kFbd, 1);
}

std::unique_ptr<WriteValue> WriteValue::make(Device& device, const Bo& target, uint64_t offset, uint32_t value,
                                             std::string* why) {
    if ((offset & 3) != 0 || offset + 4 > target.size()) {
        if (why) *why = "a 32-bit write at byte " + std::to_string(offset) + " of a buffer of " +
                        std::to_string(target.size());
        return nullptr;
    }
    std::unique_ptr<WriteValue> job(new WriteValue(device));
    job->target_ = &target;
    job->cmd_ = device.gpu().newBo(4096, why);
    if (!job->cmd_) return nullptr;
    memset(job->cmd_->cpu(), 0, hm_v7_sizes()->write_value_job);
    hm_v7_write_value_job(job->cmd_->cpu(), 1, 0, target.gpu() + offset, value);
    return job;
}

int WriteValue::submit(Submission* out) {
    return device_.gpu().submit(cmd_->gpu(), false, {cmd_.get(), target_}, out);
}

uint32_t WriteValue::status() const {
    struct hm_v7_job h;
    hm_v7_read_job(cmd_->cpu(), &h, nullptr);
    return h.exception_status;
}

void WriteValue::dump(FILE* fp) const {
    fprintf(fp, "Write Value job at %#llx:\n", static_cast<unsigned long long>(cmd_->gpu()));
    hm_v7_print_job(fp, cmd_->cpu());
}

}  // namespace hakios::mali
