// hakios-mali's tests on this machine, against the fake GPU.
//
// 1. The words of the 64x64 RGBA8 frame against literals worked out by hand from v7.xml, field by field (each comment
//    says which): the packers are Mesa's, so this holds the library's choice of field VALUES against an independent
//    reading of what Panfrost writes for the same frame (pan_desc.c, pan_jm.c).
// 2. The frame carried out by the fake GPU, and every refusal of the library by its sentence, nothing submitted.
// 3. Mutation twins: a frame the fake accepts, changed in one place in GPU memory, must be refused with its sentence
//    and the job's Exception Status set to the code the GPU would write.
#include <cmath>
#include <csignal>
#include <cstdio>
#include <execinfo.h>
#include <unistd.h>
#include <cstring>
#include <functional>
#include <string>

#include "fake_gpu.hpp"
#include "mali.hpp"
#include "pass.hpp"
#include "shaders_v7.h"
#include "v7.h"

using namespace hakios::mali;

static int failures = 0, checks = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);        \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static uint32_t word(const uint8_t* p, unsigned i) {
    uint32_t w;
    memcpy(&w, p + 4 * i, 4);
    return w;
}
static void setWord(uint8_t* p, unsigned i, uint32_t w) { memcpy(p + 4 * i, &w, 4); }

struct World {
    FakeGpu* fake;
    Gpu gpu;
    Device device;
    World() : fake(new FakeGpu()), gpu(std::unique_ptr<Transport>(fake)), device(gpu) {
        std::string why;
        if (device.init(&why) != 0) printf("device: %s\n", why.c_str());
    }
};

static const float kColour[4] = {0.2f, 0.4f, 0.6f, 0.8f};     // 51 102 153 204: 0xcc996633

static void testLiterals() {
    World w;
    std::string why;
    auto target = w.gpu.newBo(65536, &why);
    Target t;
    t.bo = target.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    auto pass = ClearPass::make(w.device, t, kColour, ClearOptions(), &why);
    CHECK(pass != nullptr, "%s", why.c_str());
    if (!pass) return;
    const uint8_t* job = w.fake->memoryAt(pass->jobAddress(), 0x100);
    const uint8_t* fbd = job + 0x40;
    const uint8_t* rt = fbd + 128;
    uint64_t fbdGpu = pass->jobAddress() + 0x40;

    // Job Header word 4: Is 64b (bit 0) 1, Type (bits 1..7) Fragment = 9, Index (bits 16..31) 1
    CHECK(word(job, 4) == 0x00010013u, "job word 4 %#x", word(job, 4));
    CHECK(word(job, 0) == 0 && word(job, 5) == 0 && word(job, 6) == 0 && word(job, 7) == 0, "job words 0, 5..7");
    // Fragment Job Payload (job + 32) word 0: Bound Min X/Y 0; word 1: Bound Max X (0..11) 3, Bound Max Y (16..27) 3 --
    // tiles of 16 pixels, (64 - 1) >> 4; words 2..3: the framebuffer pointer, tagged with Type (bit 0) true, ZS CRC
    // extension (bit 1) false, Render target count minus(1) (bits 2..5) 0
    CHECK(word(job, 8) == 0, "payload word 0 %#x", word(job, 8));
    CHECK(word(job, 9) == 0x00030003u, "payload word 1 %#x", word(job, 9));
    CHECK(word(job, 10) == static_cast<uint32_t>(fbdGpu | 1) && word(job, 11) == static_cast<uint32_t>(fbdGpu >> 32),
          "payload framebuffer %#x", word(job, 10));
    // Framebuffer Parameters word 0: the three frame shader modes Never (0)
    CHECK(word(fbd, 0) == 0, "fbd word 0 %#x", word(fbd, 0));
    // words 4..5: Sample Locations, the device's buffer; 6..7 Frame Shader DCDs 0
    CHECK(word(fbd, 4) == static_cast<uint32_t>(w.device.samplePositions().gpu()), "sample locations");
    CHECK(word(fbd, 6) == 0 && word(fbd, 7) == 0, "frame shader DCDs");
    // word 8: Width minus(1) (0..15) 63, Height minus(1) (16..31) 63
    CHECK(word(fbd, 8) == 0x003f003fu, "fbd word 8 %#x", word(fbd, 8));
    // word 9: Bound Min 0; word 10: Bound Max X 63, Y 63
    CHECK(word(fbd, 9) == 0 && word(fbd, 10) == 0x003f003fu, "fbd words 9, 10 %#x %#x", word(fbd, 9), word(fbd, 10));
    // word 11: Sample Count log2 (0..2) 0, Sample Pattern (3..5) 0, Tie-Break Rule (6..8) MINUS_180_IN_0_OUT = 2,
    // Effective Tile Size log2 (9..12) log2(256) = 8, Render Target Count minus(1) (19..22) 0, Color Buffer Allocation
    // shr(10) (24..31) 1024 >> 10 = 1: 0x80 | 0x1000 | 0x01000000
    CHECK(word(fbd, 11) == 0x01001080u, "fbd word 11 %#x", word(fbd, 11));
    // word 12: Z Internal Format (16..17) D24 = 1, nothing written, no ZS CRC extension
    CHECK(word(fbd, 12) == 0x00010000u, "fbd word 12 %#x", word(fbd, 12));
    // word 13: Z Clear 0.0; words 14..15: Tiler 0 (a frame without draws has no Tiler Context: pan_jm.c)
    CHECK(word(fbd, 13) == 0 && word(fbd, 14) == 0 && word(fbd, 15) == 0, "fbd words 13..15");
    for (unsigned i = 16; i < 32; i++) CHECK(word(fbd, i) == 0, "padding word %u", i);
    // Render Target word 0: Internal Buffer Offset 0, Internal Format (26..31) R8G8B8A8 = 1
    CHECK(word(rt, 0) == 0x04000000u, "rt word 0 %#x", word(rt, 0));
    // word 1: Write Enable (0) 1, Writeback Format (3..7) R8G8B8A8 = 19, Writeback Block Format (8..11) Linear = 2,
    // MSAA (12..13) Single, sRGB (14) 0, Dithering Enable (15) 1, Swizzle (16..27) R | G << 3 | B << 6 | A << 9 = 0x688,
    // Clean Pixel Write Enable (31) 1
    CHECK(word(rt, 1) == 0x86888299u, "rt word 1 %#x", word(rt, 1));
    // words 8..9 RGB Base, 10 Row Stride 256, 11 Surface Stride 0, 12..15 Clear Color 0..3: R | G << 8 | B << 16 | A << 24
    CHECK(word(rt, 8) == static_cast<uint32_t>(target->gpu()) && word(rt, 9) == static_cast<uint32_t>(target->gpu() >> 32),
          "rt base");
    CHECK(word(rt, 10) == 256 && word(rt, 11) == 0, "rt strides %u %u", word(rt, 10), word(rt, 11));
    for (unsigned i = 12; i < 16; i++) CHECK(word(rt, i) == 0xcc996633u, "rt clear word %u %#x", i, word(rt, i));
    for (unsigned i = 2; i < 8; i++) CHECK(word(rt, i) == 0, "rt word %u %#x", i, word(rt, i));

    // the sample positions: pan_samples.c's single-sampled entry, 128/256 of a pixel in x and y, at 0 and at 32
    const uint16_t* sp = reinterpret_cast<const uint16_t*>(w.device.samplePositions().cpu());
    CHECK(sp[0] == 128 && sp[1] == 128 && sp[64] == 128 && sp[65] == 128 && sp[2] == 0, "sample positions");

    // a BGRA8 target differs in the swizzle alone: B | G << 3 | R << 6 | A << 9 = 0x60a
    t.format = Format::BGRA8Unorm;
    auto bgra = ClearPass::make(w.device, t, kColour, ClearOptions(), &why);
    const uint8_t* rt2 = w.fake->memoryAt(bgra->jobAddress() + 0x40 + 128, 64);
    CHECK(word(rt2, 1) == 0x860a8299u, "bgra rt word 1 %#x", word(rt2, 1));

    // the Tiler Context and heap of a frame with them: Hierarchy Mask for 64 pixels with the phone's 2 levels
    // (pan_select_tiler_hierarchy_mask: the last bit of 64 / 16 = 4 is bit 3, past 2 levels, so 0b11 << 1 = 6),
    // FB Width/Height minus(1) 63
    ClearOptions o;
    o.withTiler = true;
    t.format = Format::RGBA8Unorm;
    auto tiled = ClearPass::make(w.device, t, kColour, o, &why);
    const uint8_t* tj = w.fake->memoryAt(tiled->jobAddress(), 0x200);
    CHECK(word(tj + 0x40, 14) == static_cast<uint32_t>(tiled->jobAddress() + 0x140), "fbd tiler pointer");
    CHECK(word(tj + 0x140, 2) == 0x6, "tiler word 2 %#x", word(tj + 0x140, 2));
    CHECK(word(tj + 0x140, 3) == 0x003f003fu, "tiler word 3 %#x", word(tj + 0x140, 3));
    CHECK(word(tj + 0x140, 6) == static_cast<uint32_t>(tiled->jobAddress() + 0x100), "tiler heap pointer");
    CHECK(word(tj + 0x100, 1) == (2u << 20), "heap size %#x", word(tj + 0x100, 1));
    CHECK(hm_v7_hierarchy_mask(4096, 4096, 8) == 0xff << 1, "hierarchy mask of 4096: %#x",
          hm_v7_hierarchy_mask(4096, 4096, 8));
    CHECK(hm_v7_hierarchy_mask(64, 64, 8) == 0xff && hm_v7_hierarchy_mask(16, 16, 2) == 0x3, "hierarchy masks");

    // the tile size for one RGBA8 target in the G52's 8 KB budget: 8192 >> log2(4) = 2048, clamped to 256; 1 KB
    uint32_t tile, cbuf;
    hm_v7_tile_size(4, kG52TileBufferBudget, &tile, &cbuf);
    CHECK(tile == 256 && cbuf == 1024, "tile %u cbuf %u", tile, cbuf);
    // the clear colour: round to even (0.5 * 255 = 127.5 -> 128, 1.5 / 255 * 255 ...)
    uint32_t packed[4];
    float half[4] = {0.5f, 0.0f, 1.0f, 2.0f};
    hm_v7_pack_clear_rgba8(half, packed);
    CHECK(packed[0] == 0xffff0080u && packed[3] == packed[0], "packed %#x", packed[0]);
}

static void testCarriedOut() {
    World w;
    std::string why;
    auto target = w.gpu.newBo(65536, &why);
    memset(target->cpu(), 0xa5, target->size());
    Target t;
    t.bo = target.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    auto pass = ClearPass::make(w.device, t, kColour, ClearOptions(), &why);
    Submission s;
    CHECK(pass->submit(&s) == 0 && s.seq == 1 && s.fragment, "submit");
    CHECK(w.gpu.wait(s, 1000) == 0, "wait");
    CHECK(pass->status() == 1, "status %#x", pass->status());
    unsigned right = 0;
    for (unsigned i = 0; i < 4096; i++) right += word(target->cpu(), i) == 0xcc996633u;
    CHECK(right == 4096, "%u pixels", right);
    CHECK(target->cpu()[16384] == 0xa5, "the byte after the target");
    CHECK(w.fake->faults().empty(), "faults");

    // the library refuses, by name, and builds nothing
    struct Bad {
        Target t;
        const char* says;
    } bad[] = {
        {{target.get(), 0, 0, 64, 256, Format::RGBA8Unorm}, "1 to 4096"},
        {{target.get(), 0, 64, 64, 200, Format::RGBA8Unorm}, "for 64 pixels"},
        {{target.get(), 0, 64, 64, 288, Format::RGBA8Unorm}, "not a multiple of 64"},
        {{target.get(), 16, 64, 64, 256, Format::RGBA8Unorm}, "not 64-byte aligned"},
        {{target.get(), 0, 64, 300, 256, Format::RGBA8Unorm}, "do not fit"},
        {{nullptr, 0, 64, 64, 256, Format::RGBA8Unorm}, "no buffer"},
    };
    for (const Bad& b : bad) {
        why.clear();
        CHECK(ClearPass::make(w.device, b.t, kColour, ClearOptions(), &why) == nullptr && contains(why, b.says),
              "expected \"%s\", got \"%s\"", b.says, why.c_str());
    }
    float nan[4] = {NAN, 0, 0, 1};
    CHECK(ClearPass::make(w.device, t, nan, ClearOptions(), &why) == nullptr && contains(why, "finite"), "%s", why.c_str());
    ClearOptions past;
    past.maxTx = 4;
    CHECK(ClearPass::make(w.device, t, kColour, past, &why) == nullptr && contains(why, "TILE_RANGE"), "%s", why.c_str());

    // a GPU that is not a v7 one is refused before anything is made; the G52 of pan_props.c's first entry is taken
    FakeGpu* other = new FakeGpu();
    other->params[kParamGpuProdId] = 0x9091;
    Gpu og((std::unique_ptr<Transport>(other)));
    Device od(og);
    CHECK(od.init(&why) != 0 && contains(why, "not a Bifrost v7"), "%s", why.c_str());
    FakeGpu* g52 = new FakeGpu();
    g52->params[kParamGpuProdId] = 0x7212;
    Gpu gg((std::unique_ptr<Transport>(g52)));
    Device gd(gg);
    CHECK(gd.init(&why) == 0 && std::string(gd.model()->name) == "Mali-G52" && gd.tileBufferBudget() == 8192, "G52");
    CHECK(w.device.model() != nullptr && std::string(w.device.model()->name) == "Mali-G52 r1", "the phone's model");

    // Write Value
    auto wv = WriteValue::make(w.device, *target, 20000, 0x12345678u, &why);
    CHECK(wv && wv->submit(&s) == 0 && w.gpu.wait(s, 1000) == 0 && wv->status() == 1 &&
          word(target->cpu(), 5000) == 0x12345678u, "write value");
    CHECK(WriteValue::make(w.device, *target, 2, 1, &why) == nullptr, "an unaligned write value");
}

// a frame the fake accepts, changed by MUTATE, then submitted (on the fragment slot unless VERTEX)
static void twin(const char* name, const std::function<void(World&, uint8_t* job, const Bo& target)>& mutate,
                 const char* says, uint32_t status, bool vertexSlot = false) {
    World w;
    std::string why;
    auto target = w.gpu.newBo(32768, &why);
    memset(target->cpu(), 0xa5, target->size());
    Target t;
    t.bo = target.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    auto pass = ClearPass::make(w.device, t, kColour, ClearOptions(), &why);
    uint8_t* job = w.fake->memoryAt(pass->jobAddress(), 0x200);
    mutate(w, job, *target);
    Submission s;
    int e = vertexSlot ? w.gpu.submit(pass->jobAddress(), false, {target.get()}, &s) : pass->submit(&s);
    CHECK(e == 0 && w.gpu.wait(s, 1000) == 0, "%s: submit/wait", name);
    const auto& f = w.fake->faults();
    CHECK(f.size() == 1 && contains(f[0].why, says), "%s: expected \"%s\", got \"%s\"", name, says,
          f.empty() ? "(no fault)" : f[0].why.c_str());
    CHECK(pass->status() == status, "%s: status %#x, expected %#x", name, pass->status(), status);
    CHECK(target->cpu()[0] == 0xa5, "%s: the target was written", name);
}

static void testTwins() {
    // the pointer's render target count (bits 2..5 of the tagged pointer) says 2
    twin("rt count", [](World&, uint8_t* job, const Bo&) { setWord(job, 10, word(job, 10) | (1u << 2)); },
         "pointer says 2 render targets, the descriptor 1", 0x40);
    // the pointer's Type bit cleared
    twin("type bit", [](World&, uint8_t* job, const Bo&) { setWord(job, 10, word(job, 10) & ~1u); },
         "Type bit is 0", 0x40);
    // a bit in the Fragment Job Payload's word 7, which v7.xml leaves empty
    twin("payload word 7", [](World&, uint8_t* job, const Bo&) { setWord(job, 15, 1); },
         "Invalid field of Fragment Job Payload unpacked at word 7", 0x40);
    // a bit in the Job Header's word 4 where v7.xml has none (bit 10)
    twin("header bit 10", [](World&, uint8_t* job, const Bo&) { setWord(job, 4, word(job, 4) | (1u << 10)); },
         "Invalid field of Job Header unpacked at word 4", 0x40);
    // the sample locations pointer to nowhere
    twin("sample locations", [](World&, uint8_t* job, const Bo&) { setWord(job + 0x40, 4, 0x1000); setWord(job + 0x40, 5, 0); },
         "sample locations at 0x1000 are outside every buffer", 0xc1 | (1u << 8));
    // the job's tiles past the frame: Bound Max X 4 (64 pixels are tiles 0..3)
    twin("tile range", [](World&, uint8_t* job, const Bo&) { setWord(job, 9, 0x00030004u); },
         "not inside the frame's (0,0)-(3,3)", 0x59);
    // the render target's base 16 bytes on
    twin("rt base", [](World&, uint8_t* job, const Bo&) { setWord(job + 0x40 + 128, 8, word(job + 0x40 + 128, 8) + 16); },
         "not 64-byte aligned", 0x58);
    // the render target's rows past its buffer: a row stride of 1024 (64 rows of it are 64 KB, the buffer 32 KB)
    twin("rt rows", [](World&, uint8_t* job, const Bo&) { setWord(job + 0x40 + 128, 10, 1024); },
         "are not inside one buffer", 0xc1 | (3u << 8));
    // an effective tile size of 512 pixels (log2 9 in bits 9..12 of word 11)
    twin("tile size", [](World&, uint8_t* job, const Bo&) {
        setWord(job + 0x40, 11, (word(job + 0x40, 11) & ~(0xfu << 9)) | (9u << 9));
    }, "effective tile size of 512", 0x58);
    // a colour buffer allocation of 0 KB (bits 24..31 of word 11), less than the tile's 1 KB
    twin("cbuf", [](World&, uint8_t* job, const Bo&) { setWord(job + 0x40, 11, word(job + 0x40, 11) & 0x00ffffffu); },
         "past the colour buffer allocation of 0", 0x58);
    // a frame shader mode Always (pre frame 0) with no DCDs
    twin("frame shader", [](World&, uint8_t* job, const Bo&) { setWord(job + 0x40, 0, 1); },
         "with no Frame Shader DCDs", 0xc1 | (1u << 8));
    // a fragment chain sent to the other slot
    twin("slot", [](World&, uint8_t*, const Bo&) {}, "without PANFROST_JD_REQ_FS", 0x40, true);
    // a Tiler Context whose frame is not the framebuffer's
    twin("tiler size", [](World& w, uint8_t* job, const Bo&) {
        std::string why;
        const Bo* heap = w.device.tilerHeap(&why);
        hm_v7_tiler_heap(job + 0x100, heap->gpu(), static_cast<uint32_t>(heap->size()));
        uint64_t jobGpu = 0;
        // the job's own GPU address: the framebuffer pointer less its offset and tag
        jobGpu = (static_cast<uint64_t>(word(job, 11)) << 32 | word(job, 10)) - 0x40 - 1;
        hm_v7_tiler_context(job + 0x140, jobGpu + 0x100, 32, 64, 0xff);
        setWord(job + 0x40, 14, static_cast<uint32_t>(jobGpu + 0x140));
        setWord(job + 0x40, 15, static_cast<uint32_t>((jobGpu + 0x140) >> 32));
    }, "Tiler Context's frame is 32x64", 0x40);
    // a job that was run before (Exception Status already DONE)
    twin("run twice", [](World&, uint8_t* job, const Bo&) { setWord(job, 0, 1); }, "it was run before", 0x40);

    // and one the fake carries out but writes nothing for: Clean Pixel Write Enable off on a frame without primitives
    World w;
    std::string why;
    auto target = w.gpu.newBo(32768, &why);
    memset(target->cpu(), 0xa5, target->size());
    Target t;
    t.bo = target.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    ClearOptions o;
    o.cleanPixelWrite = false;
    auto pass = ClearPass::make(w.device, t, kColour, o, &why);
    Submission s;
    CHECK(pass->submit(&s) == 0 && w.gpu.wait(s, 1000) == 0 && pass->status() == 1 && w.fake->faults().empty() &&
          target->cpu()[0] == 0xa5 && w.fake->pixelsWritten() == 0, "no clean pixel write: done, nothing written");
}

// what the phone's GPU did with a cleared frame's bounds (runs 501, 502), and the fake GPU now does: the job's tiles do
// not limit what is written, the framebuffer's Bound Min/Max does, to the pixel
static void testBounds() {
    World w;
    std::string why;
    auto target = w.gpu.newBo(32768, &why);
    Target t;
    t.bo = target.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    auto written = [&](const ClearOptions& o, unsigned* x0, unsigned* y0, unsigned* x1, unsigned* y1) {
        memset(target->cpu(), 0xa5, target->size());
        auto pass = ClearPass::make(w.device, t, kColour, o, &why);
        Submission s;
        pass->submit(&s);
        w.gpu.wait(s, 1000);
        unsigned n = 0;
        *x0 = *y0 = 99, *x1 = *y1 = 0;
        for (unsigned y = 0; y < 64; y++) {
            for (unsigned x = 0; x < 64; x++) {
                if (word(target->cpu(), y * 64 + x) == 0xcc996633u) {
                    n++;
                    *x0 = x < *x0 ? x : *x0, *y0 = y < *y0 ? y : *y0, *x1 = x > *x1 ? x : *x1, *y1 = y > *y1 ? y : *y1;
                }
            }
        }
        return n;
    };
    unsigned x0, y0, x1, y1;
    ClearOptions tiles;
    tiles.minTx = tiles.minTy = 1;
    tiles.maxTx = tiles.maxTy = 2;
    CHECK(written(tiles, &x0, &y0, &x1, &y1) == 4096, "a job over tiles (1,1)-(2,2) writes the whole frame");
    ClearOptions bounds;
    bounds.boundMinX = 5, bounds.boundMinY = 7, bounds.boundMaxX = 40, bounds.boundMaxY = 50;
    CHECK(written(bounds, &x0, &y0, &x1, &y1) == 36 * 44 && x0 == 5 && y0 == 7 && x1 == 40 && y1 == 50,
          "the framebuffer's bounds (5,7)-(40,50): (%u,%u)-(%u,%u)", x0, y0, x1, y1);
}

static void testBuffers() {
    World w;
    std::string why;
    size_t before = w.fake->liveBuffers();
    {
        auto bo = w.gpu.newBo(100, &why);
        CHECK(bo && bo->size() == 16384 && bo->gpu() >= 0x01000000 && (bo->gpu() & 0xffff) == 0, "a buffer");
        CHECK(w.fake->mappedBuffers() >= 1, "mapped");
    }
    CHECK(w.fake->liveBuffers() == before, "given back: %zu of %zu", w.fake->liveBuffers(), before);
    CHECK(!w.gpu.newBo(0, &why) && contains(why, "1 byte to 256 MB"), "%s", why.c_str());
    CHECK(!w.gpu.newBo(300ull << 20, &why), "300 MB");
    Submission s;
    CHECK(w.gpu.submit(0, false, {}, &s) == EIO, "a chain at 0 is not queued");
    s.seq = 99;
    CHECK(w.gpu.wait(s, 10) == ETIMEDOUT, "a job never sent");
    CHECK(std::string(exceptionName(1)) == "DONE" && std::string(exceptionName(0x159)) == "TILE_RANGE_FAULT" &&
          std::string(exceptionName(0x3c3)) == "TRANSLATION_FAULT", "exception names");
}

// The depth and stencil state written over a compiled Renderer State: Less with writes over the depth state gives the
// compiler's own fs_rsd_depth word for word; Always without writes gives fs_rsd's words 8 to 11; a stencil state
// reads back as written
static void testZs() {
    for (const hm_program* p : {&hm_color, &hm_tri, &hm_tex}) {
        uint32_t rsd[16];
        memcpy(rsd, p->fs_rsd_depth, sizeof(rsd));
        hm_v7_zs zs = {};
        zs.depth_compare = HM_COMPARE_LESS;
        zs.depth_write = true;
        const char* why = nullptr;
        CHECK(hm_v7_renderer_state_zs(rsd, &zs, &why) == 0, "%s: %s", p->name, why ? why : "");
        CHECK(memcmp(rsd, p->fs_rsd_depth, sizeof(rsd)) == 0, "%s: Less, write is not fs_rsd_depth", p->name);
        zs.depth_compare = HM_COMPARE_ALWAYS;
        zs.depth_write = false;
        hm_v7_renderer_state_zs(rsd, &zs, &why);
        CHECK(memcmp(rsd + 8, p->fs_rsd + 8, 16) == 0, "%s: Always is not fs_rsd's words 8..11: %08x %08x %08x %08x",
              p->name, rsd[8], rsd[9], rsd[10], rsd[11]);
    }
    uint32_t rsd[16];
    memcpy(rsd, hm_color.fs_rsd_depth, sizeof(rsd));
    hm_v7_zs zs = {};
    zs.depth_compare = HM_COMPARE_ALWAYS;
    zs.stencil_enable = true;
    zs.reference = 0x5a;
    zs.front = {HM_COMPARE_EQUAL, HM_STENCIL_KEEP, HM_STENCIL_ZERO, HM_STENCIL_REPLACE, 0xff, 0x0f};
    zs.back = zs.front;
    CHECK(hm_v7_renderer_state_zs(rsd, &zs, nullptr) == 0, "stencil state");
    // STENCIL: reference 0:7, mask 8:15, compare 16:18, stencil fail 19:21, depth fail 22:24, depth pass 25:27 (v7.xml)
    uint32_t want = 0x5au | 0xffu << 8 | 2u << 16 | 0u << 19 | 2u << 22 | 1u << 25;
    CHECK(rsd[10] == want && rsd[11] == want, "stencil words %08x %08x, want %08x", rsd[10], rsd[11], want);
    CHECK((rsd[9] & 0xffff) == 0x0f0f, "stencil write masks %04x", rsd[9] & 0xffff);
    CHECK(memcmp(rsd, hm_color.fs_rsd_depth, 32) == 0 && memcmp(rsd + 12, hm_color.fs_rsd_depth + 12, 16) == 0,
          "words other than 8..11 changed");
}

// The render target formats of v7.c against Panfrost's own (hm_formats: pan_format.c's blendable table and
// pan_prepare_rt's inverted swizzle, computed by Mesa in bishaders), and a clear into R8 and RG8 carried out by the
// fake GPU byte for byte, nothing past each row's pixels
static void testFormats() {
    const enum hm_rt_format rts[4] = {HM_RT_RGBA8_UNORM, HM_RT_BGRA8_UNORM, HM_RT_R8_UNORM, HM_RT_RG8_UNORM};
    for (int i = 0; i < 4; i++) {
        CHECK(hm_formats[i].rt_internal == 1, "%s: internal %u", hm_formats[i].name, hm_formats[i].rt_internal);
        CHECK(hm_v7_rt_writeback(rts[i]) == hm_formats[i].rt_writeback, "%s: writeback %u, Mesa's %u",
              hm_formats[i].name, hm_v7_rt_writeback(rts[i]), hm_formats[i].rt_writeback);
        CHECK(hm_v7_rt_swizzle(rts[i]) == hm_formats[i].rt_swizzle, "%s: swizzle %#x, Mesa's %#x", hm_formats[i].name,
              hm_v7_rt_swizzle(rts[i]), hm_formats[i].rt_swizzle);
        CHECK(hm_formats[i].blend_memory_format == hm_formats[0].blend_memory_format,
              "%s: a blend memory format of its own (%#x): the compiled blend descriptors would not do",
              hm_formats[i].name, hm_formats[i].blend_memory_format);
    }
    for (Format f : {Format::R8Unorm, Format::RG8Unorm}) {
        World w;
        Gpu& gpu = w.gpu;
        Device& device = w.device;
        std::string why;
        auto bo = gpu.newBo(128 * 64 + 4096, &why);
        memset(bo->cpu(), 0xa5, bo->size());
        Target t;
        t.bo = bo.get();
        t.width = t.height = 64;
        t.rowBytes = 128;
        t.format = f;
        const float c[4] = {0.2f, 0.4f, 0.6f, 0.8f};
        auto pass = ClearPass::make(device, t, c, ClearOptions(), &why);
        CHECK(pass != nullptr, "%s", why.c_str());
        if (!pass) continue;
        Submission s;
        CHECK(pass->submit(&s) == 0 && gpu.wait(s, 1000000) == 0 && (pass->status() & 0xff) == 1, "R8/RG8 clear not done");
        const uint32_t bpp = bytesPerPixel(f);
        unsigned right = 0, kept = 0;
        for (uint32_t y = 0; y < 64; y++) {
            for (uint32_t x = 0; x < 64; x++) {
                const uint8_t* q = bo->cpu() + y * 128 + bpp * x;
                right += q[0] == 0x33 && (bpp == 1 || q[1] == 0x66);
            }
            for (uint32_t x = bpp * 64; x < 128; x++) kept += bo->cpu()[y * 128 + x] == 0xa5;
        }
        CHECK(right == 4096, "%u bytes-per-pixel %u: %u of 4096 pixels right", bpp, bpp, right);
        CHECK(kept == 64 * (128 - bpp * 64), "row padding written: %u of %u bytes kept", kept, 64 * (128 - bpp * 64));
    }
}

// an assertion or a fault: the backtrace, and out at once (no crash report: this Mac's reporter has been seen to hold a
// crashed process for minutes)
static void fatal(int sig) {
    void* frames[48];
    int n = backtrace(frames, 48);
    fprintf(stderr, "host_test: signal %d\n", sig);
    backtrace_symbols_fd(frames, n, 2);
    _exit(70);
}

int main() {
    for (int sig : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE}) signal(sig, fatal);
    testLiterals();
    testCarriedOut();
    testTwins();
    testBounds();
    testBuffers();
    testZs();
    testFormats();
    {
        // pan_samples.c by hand: the single-sampled entry's position 0 and origin (128, 128); ROTATED_4X_GRID at
        // pattern 2's 512 bytes, SAMPLE8(-1,-3) = SAMPLE16(-2,-6) = (96, 32), SAMPLE8(3,-1) = (224, 96), SAMPLE8(-3,1) = (32, 160),
        // SAMPLE8(1,3) = (160, 224), origin (128, 128)
        uint16_t t[HM_V7_SAMPLE_POSITIONS_SIZE / 2];
        hm_v7_sample_positions_table(t);
        CHECK(t[0] == 128 && t[1] == 128 && t[64] == 128 && t[65] == 128, "single-sampled entry");
        CHECK(hm_v7_sample_pattern_offset(4) == 512 && hm_v7_sample_pattern_offset(1) == 0, "pattern offsets");
        const uint16_t* r = t + 256;
        CHECK(r[0] == 96 && r[1] == 32 && r[2] == 224 && r[3] == 96 && r[4] == 32 && r[5] == 160 && r[6] == 160 &&
              r[7] == 224 && r[64] == 128 && r[65] == 128 && r[8] == 0, "rotated 4x entry %u,%u %u,%u %u,%u %u,%u",
              r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    }
    printf("host_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
