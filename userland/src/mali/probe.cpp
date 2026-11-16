// mali-probe: hakios-mali on the phone's Mali-G52, stage by stage, each with its verdict and, when it fails, why.
//
//   mali-probe            every stage against the kernel's HakiosMali (waits up to 60 s for the service)
//   mali-probe --fake     the same stages against the fake GPU, in this process (any machine)
//   mali-probe --dump     also print every frame's descriptors as Mesa's printers say them
//   mali-probe --until T  nothing but wait until the system has been up T seconds (a job list's clock: pictures
//                         taken at the run's end, which is a time since boot, wherever launchd started the list)
//   mali-probe --show N   only the triangle, then it on the panel for N seconds, sixteen times as big (a picture of
//                         the test for fbgrab: put again five times a second, as the window server draws over it)
//
// Each verdict is a line "mali-probe: <stage> PASS|FAIL|MEASURE ..." on standard output (the console, when launchd's
// job com.hakios.demo runs it from etc/rc.jobs) and, on the phone, the same line with "!!" through kern.hakios.say,
// which the kernel keeps for the host however long the run (tools/xnu-cycle.sh appends them to the log).
//
// The expected pixels are computed here from the colour, independently of the library (round(c * 255), to even, as
// pan_clear.c rounds), and every byte of a target's buffer outside the pixels the frame covers must keep the canary.
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#include <sys/types.h>
#include <sys/sysctl.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#endif
#include "fake_gpu.hpp"
#include "mali.hpp"
#if defined(HAKIOS_MALI_COMPILER)
#include "compiler.h"
#endif
#include "blit.hpp"
#include "draw.hpp"
#include "metal.hpp"
#include "pass.hpp"
#include "shaders_v7.h"

using namespace hakios::mali;

namespace {

bool gDump = false, gFake = false;
int gShow = 0;
unsigned gPassed = 0, gFailed = 0;

void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
void say(const char* format, ...) {
    char line[400];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    printf("mali-probe: %s\n", line);
#if defined(__APPLE__)
    if (!gFake) {
        // the kernel keeps 200 characters of a "!!" line (hakios_host_keep_add)
        char kept[190];
        snprintf(kept, sizeof(kept), "!!mali-probe: %s", line);
        sysctlbyname("kern.hakios.say", nullptr, nullptr, kept, strlen(kept));
    }
#endif
}

std::vector<std::string> gPassedStages;

bool verdict(const char* stage, bool ok, const std::string& detail) {
    say("%s %s %s", stage, ok ? "PASS" : "FAIL", detail.c_str());
    (ok ? gPassed : gFailed)++;
    if (ok) gPassedStages.push_back(stage);
    return ok;
}

uint8_t unorm8(float f) {
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    return static_cast<uint8_t>(std::nearbyint(f * 255.0f));      // the default rounding: to nearest, ties to even
}

std::string hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "%#llx", static_cast<unsigned long long>(v));
    return b;
}

std::string statusText(uint32_t status) {
    return hex(status) + " " + exceptionName(status);
}

// --- one cleared frame, measured ---------------------------------------------------------------------------------

struct ClearCase {
    const char* stage;
    uint32_t width, height, rowBytes;
    uint64_t offset;            // of the first pixel in the buffer
    Format format;
    float rgba[4];
    ClearOptions options;
    bool expectWritten = true;  // false: the frame should leave every byte as it was
};

constexpr uint8_t kCanary = 0xa5;

bool runClear(Device& device, const ClearCase& c) {
    std::string why;
    uint64_t bytes = c.offset + static_cast<uint64_t>(c.rowBytes) * c.height + 4096;   // and a page of canary after
    auto bo = device.gpu().newBo(bytes, &why);
    if (!bo) return verdict(c.stage, false, "target buffer: " + why);
    memset(bo->cpu(), kCanary, bo->size());

    Target t;
    t.bo = bo.get();
    t.offset = c.offset;
    t.width = c.width;
    t.height = c.height;
    t.rowBytes = c.rowBytes;
    t.format = c.format;
    auto pass = ClearPass::make(device, t, c.rgba, c.options, &why);
    if (!pass) return verdict(c.stage, false, "refused: " + why);

    // the pixel, from the colour alone
    uint8_t r = unorm8(c.rgba[0]), g = unorm8(c.rgba[1]), b = unorm8(c.rgba[2]), a = unorm8(c.rgba[3]);
    uint8_t want[4] = {r, g, b, a};
    if (c.format == Format::BGRA8Unorm) {
        want[0] = b, want[2] = r;
    }
    // the pixels the frame covers: the framebuffer's bounds inside the frame. Not the job's tiles: on the phone a job
    // over some of the frame's tiles wrote all of it (runs 501, 502), while the framebuffer's bounds held to the pixel
    uint32_t x0 = c.options.boundMinX, y0 = c.options.boundMinY;
    uint32_t x1 = c.options.boundMaxX == ~0u ? c.width - 1 : c.options.boundMaxX;
    uint32_t y1 = c.options.boundMaxY == ~0u ? c.height - 1 : c.options.boundMaxY;

    Transport& tr = device.gpu().transport();
    Submission s;
    uint64_t t0 = tr.nowUs();
    int error = pass->submit(&s);
    if (error != 0) return verdict(c.stage, false, std::string("SUBMIT: ") + strerror(error));
    error = device.gpu().wait(s, 2000000);
    uint64_t us = tr.nowUs() - t0;
    if (error != 0) return verdict(c.stage, false, "job " + std::to_string(s.seq) + " not done in 2 s: " + strerror(error));
    uint32_t status = pass->status();

    uint64_t covered = 0, right = 0, canaryKept = 0, canaryTotal = 0, firstWrong = ~0ull;
    const uint8_t* m = bo->cpu();
    for (uint64_t i = 0; i < bo->size(); i++) {
        bool inPixel = false;
        if (i >= c.offset && (i - c.offset) / c.rowBytes < c.height) {
            uint64_t y = (i - c.offset) / c.rowBytes, x = ((i - c.offset) % c.rowBytes) / 4;
            inPixel = x < c.width && y >= y0 && y <= y1 && x >= x0 && x <= x1 && c.expectWritten;
            if (inPixel && ((i - c.offset) % c.rowBytes) % 4 == 0) {
                covered++;
                if (memcmp(m + i, want, 4) == 0) {
                    right++;
                } else if (firstWrong == ~0ull) {
                    firstWrong = i;
                }
            }
        }
        if (!inPixel) {
            canaryTotal++;
            canaryKept += m[i] == kCanary;
            if (m[i] != kCanary && firstWrong == ~0ull) firstWrong = i;
        }
    }
    // what the GPU wrote, whatever was expected: the pixels of the target holding the colour and their bounding box
    uint64_t holding = 0;
    uint32_t bx0 = ~0u, by0 = ~0u, bx1 = 0, by1 = 0;
    for (uint32_t y = 0; y < c.height; y++) {
        for (uint32_t x = 0; x < c.width; x++) {
            if (memcmp(m + c.offset + static_cast<uint64_t>(y) * c.rowBytes + 4ull * x, want, 4) == 0) {
                holding++;
                bx0 = x < bx0 ? x : bx0, by0 = y < by0 ? y : by0, bx1 = x > bx1 ? x : bx1, by1 = y > by1 ? y : by1;
            }
        }
    }
    char region[80];
    if (holding == 0) snprintf(region, sizeof(region), "none written");
    else snprintf(region, sizeof(region), "written (%u,%u)-(%u,%u), %llu", bx0, by0, bx1, by1, (unsigned long long)holding);
    char detail[320];
    snprintf(detail, sizeof(detail), "%ux%u %s, job %s, %llu of %llu pixels %02x%02x%02x%02x, canary %llu of %llu "
             "bytes kept, %s, %llu us", c.width, c.height, c.format == Format::BGRA8Unorm ? "BGRA8" : "RGBA8",
             statusText(status).c_str(), static_cast<unsigned long long>(right), static_cast<unsigned long long>(covered),
             want[0], want[1], want[2], want[3], static_cast<unsigned long long>(canaryKept),
             static_cast<unsigned long long>(canaryTotal), region, static_cast<unsigned long long>(us));
    bool ok = (status & 0xff) == 1 && right == covered && canaryKept == canaryTotal;
    std::string text = detail;
    if (!ok && firstWrong != ~0ull) {
        uint64_t i = firstWrong & ~3ull;
        char w[96];
        snprintf(w, sizeof(w), "; first difference at byte %llu: %02x %02x %02x %02x",
                 static_cast<unsigned long long>(firstWrong), m[i], m[i + 1], m[i + 2], m[i + 3]);
        text += w;
    }
    verdict(c.stage, ok, text);
    if (!ok || gDump) {
        pass->dump(stdout);
    }
    return ok;
}

// One DrawPass of the color program (a varying from the vertices, a uniform tint pushed from buffer 0), judged pixel by
// pixel: more than a pixel inside the triangle the colour interpolated in screen space (w is 1) times the tint, through
// the blend when there is one; more than a pixel outside the clear; within 2/255 a channel (8-bit rounding, dithering)
bool runColorDraw(Device& device, const char* stage, const float colors[12], const float tint[4], const float clear[4],
                  bool over, uint32_t W = 64, uint32_t H = 64, bool bgra = false) {
    std::string why;
    const uint32_t R = (4 * W + 63) & ~63u;
    auto bo = device.gpu().newBo(static_cast<uint64_t>(R) * H + 4096, &why);
    if (!bo) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    Target t;
    t.bo = bo.get();
    t.width = W;
    t.height = H;
    t.rowBytes = R;
    t.format = bgra ? Format::BGRA8Unorm : Format::RGBA8Unorm;
    const float positions[12] = {-0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f};
    DrawInput in;
    in.attributes[0] = positions;
    in.attributes[1] = colors;
    in.uniforms = tint;
    in.uniformSize = 16;
    memcpy(in.clear, clear, sizeof(in.clear));
    in.blendOver = over;
    auto pass = DrawPass::make(device, t, hm_color, in, &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission ds, fs;
    uint64_t t0 = device.gpu().transport().nowUs();
    int e = pass->submit(&ds, &fs);
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    uint64_t us = device.gpu().transport().nowUs() - t0;
    // the viewport: NDC (x, y) at pixel (W/2 + W/2 x, H/2 + H/2 y)
    const float vx[3] = {W * 0.25f, W * 0.75f, W * 0.5f}, vy[3] = {H * 0.25f, H * 0.25f, H * 0.75f};
    float area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vx[2] - vx[0]) * (vy[1] - vy[0]);
    unsigned inRight = 0, inTotal = 0, outRight = 0, outTotal = 0, worst = 0;
    uint8_t clearPx[4];
    for (int c = 0; c < 4; c++) clearPx[c] = unorm8(clear[c]);
    char firstBad[120] = "";
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            float px = x + 0.5f, py = y + 0.5f, b[3], d = 1e9f;
            for (int k = 0; k < 3; k++) {
                int i = (k + 1) % 3, j = (k + 2) % 3;
                b[k] = ((vx[j] - vx[i]) * (py - vy[i]) - (px - vx[i]) * (vy[j] - vy[i])) / area;
                float ex = vx[j] - vx[i], ey = vy[j] - vy[i];
                float dist = std::fabs(b[k] * area) / std::sqrt(ex * ex + ey * ey);
                d = dist < d ? dist : d;
            }
            bool inside = b[0] > 0 && b[1] > 0 && b[2] > 0;
            if (d <= 1.0f) continue;
            const uint8_t* q = bo->cpu() + y * R + 4 * x;
            uint8_t want[4];
            if (inside) {
                float src[4];
                for (int c = 0; c < 4; c++) src[c] = (b[0] * colors[c] + b[1] * colors[4 + c] + b[2] * colors[8 + c]) * tint[c];
                for (int c = 0; c < 4; c++) {
                    float v = over ? src[c] + clear[c] * (1.0f - src[3]) : src[c];
                    want[c] = unorm8(v);
                }
            } else {
                memcpy(want, clearPx, 4);
            }
            if (bgra) {
                uint8_t r = want[0];
                want[0] = want[2], want[2] = r;
            }
            unsigned diff = 0;
            for (int c = 0; c < 4; c++) {
                unsigned dd = q[c] > want[c] ? q[c] - want[c] : want[c] - q[c];
                diff = dd > diff ? dd : diff;
            }
            bool good = diff <= 2;
            worst = inside && diff > worst ? diff : worst;
            (inside ? inTotal : outTotal)++;
            (inside ? inRight : outRight) += good;
            if (!good && firstBad[0] == 0)
                snprintf(firstBad, sizeof(firstBad), "; first wrong (%u,%u): %02x%02x%02x%02x, want %02x%02x%02x%02x", x, y,
                         q[0], q[1], q[2], q[3], want[0], want[1], want[2], want[3]);
        }
    }
    const uint8_t* mid = bo->cpu() + (H / 2) * R + 4 * (W / 2);
    char detail[360];
    snprintf(detail, sizeof(detail), "vertex %s tiler %s fragment %s%s%s; inside %u of %u right (largest difference %u), "
             "outside %u of %u the clear; pixel (32,32) %02x%02x%02x%02x; %llu us%s",
             statusText(pass->vertexStatus()).c_str(), statusText(pass->tilerStatus()).c_str(),
             statusText(pass->fragmentStatus()).c_str(), e ? ", " : "", e ? strerror(e) : "", inRight, inTotal, worst,
             outRight, outTotal, mid[0], mid[1], mid[2], mid[3], (unsigned long long)us, firstBad);
    bool jobs = e == 0 && (pass->vertexStatus() & 0xff) == 1 && (pass->tilerStatus() & 0xff) == 1 &&
                (pass->fragmentStatus() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU checks the jobs, does not rasterize: %s) %s", stage,
            jobs ? "jobs accepted" : "jobs REFUSED", detail);
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && inRight == inTotal && outRight == outTotal;
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// The tex program: a 4x4 RGBA8 texture of sixteen colours sampled with nearest filtering across a quad covering
// the 64x64 target (two triangles, uv 0..1): pixel (x, y) must be texel (x / 16, y / 16) exactly, judged away from
// the texel edges by a pixel
bool runTexture(Device& device) {
    const char* stage = "texture";
    std::string why;
    auto bo = device.gpu().newBo(256 * 64 + 4096, &why);
    auto tex = device.gpu().newBo(4096, &why);
    if (!bo || !tex) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    uint8_t texel[16][4];
    for (int i = 0; i < 16; i++) {
        texel[i][0] = static_cast<uint8_t>(17 * i), texel[i][1] = static_cast<uint8_t>(255 - 16 * i);
        texel[i][2] = static_cast<uint8_t>((i * 97) & 255), texel[i][3] = 255;
    }
    for (int y = 0; y < 4; y++) memcpy(tex->cpu() + y * 64, texel[4 * y], 16);
    Target t;
    t.bo = bo.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    // two triangles over the whole viewport: NDC -1..1, uv 0..1 (row y of the target is uv.y = y / 64)
    const float pos[24] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
    const float uv[24] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0};
    DrawInput in;
    in.attributes[0] = pos;
    in.attributes[1] = uv;
    in.vertexCount = 6;
    in.texture = tex.get();
    in.textureWidth = in.textureHeight = 4;
    in.textureRowBytes = 64;
    in.nearest = true;
    auto pass = DrawPass::make(device, t, hm_tex, in, &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission ds, fs;
    int e = pass->submit(&ds, &fs);
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    unsigned right = 0, judged = 0;
    char firstBad[120] = "";
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            if (x % 16 == 0 || x % 16 == 15 || y % 16 == 0 || y % 16 == 15) continue;
            judged++;
            const uint8_t* q = bo->cpu() + y * 256 + 4 * x;
            const uint8_t* w = texel[(y / 16) * 4 + x / 16];
            if (memcmp(q, w, 4) == 0) right++;
            else if (firstBad[0] == 0)
                snprintf(firstBad, sizeof(firstBad), "; first wrong (%u,%u): %02x%02x%02x%02x, want %02x%02x%02x%02x", x, y,
                         q[0], q[1], q[2], q[3], w[0], w[1], w[2], w[3]);
        }
    }
    char detail[300];
    snprintf(detail, sizeof(detail), "vertex %s tiler %s fragment %s; %u of %u pixels the texel their uv names%s",
             statusText(pass->vertexStatus()).c_str(), statusText(pass->tilerStatus()).c_str(),
             statusText(pass->fragmentStatus()).c_str(), right, judged, firstBad);
    bool jobs = e == 0 && (pass->vertexStatus() & 0xff) == 1 && (pass->tilerStatus() & 0xff) == 1 &&
                (pass->fragmentStatus() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU checks the jobs, does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED", detail);
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && right == judged;
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// The fill kernel: 4 workgroups of 16 write out[i] = i * 3 + k for i < 64 into a buffer of canaries; every word right
// and the canary after them kept
bool runCompute(Device& device) {
    const char* stage = "compute";
    std::string why;
    auto bo = device.gpu().newBo(4096, &why);
    if (!bo) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    uint32_t uniforms[3] = {static_cast<uint32_t>(bo->gpu()), static_cast<uint32_t>(bo->gpu() >> 32), 1000};
    const unsigned groups[3] = {4, 1, 1};
    auto pass = ComputePass::make(device, hm_fill, groups, uniforms, sizeof(uniforms), &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission s;
    uint64_t t0 = device.gpu().transport().nowUs();
    int e = pass->submit({bo.get()}, &s);
    if (e == 0) e = device.gpu().wait(s, 2000000);
    uint64_t us = device.gpu().transport().nowUs() - t0;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(bo->cpu());
    unsigned right = 0, kept = 0;
    for (unsigned i = 0; i < 64; i++) right += w[i] == i * 3 + 1000;
    for (unsigned i = 64; i < bo->size() / 4; i++) kept += w[i] == 0xa5a5a5a5u;
    char detail[200];
    snprintf(detail, sizeof(detail), "job %s%s%s; %u of 64 words i*3+1000 (word 0 %u, word 63 %u), canary %u of %llu kept; %llu us",
             statusText(pass->status()).c_str(), e ? ", " : "", e ? strerror(e) : "", right, w[0], w[63], kept,
             (unsigned long long)(bo->size() / 4 - 64), (unsigned long long)us);
    bool job = e == 0 && (pass->status() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU checks the job, runs no shader: %s) %s", stage, job ? "job accepted" : "job REFUSED", detail);
        if (!job) gFailed++;
        return job;
    }
    bool ok = job && right == 64 && kept == bo->size() / 4 - 64;
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// The copy kernel as a blit (MTLBlitCommandEncoder's copyFromBuffer): 200 words of a pattern from one buffer into
// another of canaries, 4 workgroups of 64 so the last is cut short by the count; every word copied, every other kept
bool runBlit(Device& device) {
    const char* stage = "blit";
    std::string why;
    auto src = device.gpu().newBo(4096, &why);
    auto dst = src ? device.gpu().newBo(4096, &why) : nullptr;
    if (!src || !dst) return verdict(stage, false, why);
    uint32_t* in = reinterpret_cast<uint32_t*>(src->cpu());
    for (unsigned i = 0; i < src->size() / 4; i++) in[i] = 0x9e3779b9u * (i + 1);
    memset(dst->cpu(), kCanary, dst->size());
    const unsigned count = 200;
    uint32_t uniforms[8] = {static_cast<uint32_t>(src->gpu()), static_cast<uint32_t>(src->gpu() >> 32),
                            static_cast<uint32_t>(dst->gpu()), static_cast<uint32_t>(dst->gpu() >> 32), count, 0, 0, 0};
    const unsigned groups[3] = {(count + 63) / 64, 1, 1};
    auto pass = ComputePass::make(device, hm_copy, groups, uniforms, sizeof(uniforms), &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission s;
    uint64_t t0 = device.gpu().transport().nowUs();
    int e = pass->submit({dst.get()}, &s);
    if (e == 0) e = device.gpu().wait(s, 2000000);
    uint64_t us = device.gpu().transport().nowUs() - t0;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(dst->cpu());
    unsigned right = 0, kept = 0;
    for (unsigned i = 0; i < count; i++) right += w[i] == in[i];
    for (unsigned i = count; i < dst->size() / 4; i++) kept += w[i] == 0xa5a5a5a5u;
    char detail[200];
    snprintf(detail, sizeof(detail), "job %s%s%s; %u of %u words copied (word 0 %08x, word %u %08x), canary %u of %llu kept; %llu us",
             statusText(pass->status()).c_str(), e ? ", " : "", e ? strerror(e) : "", right, count, w[0], count - 1,
             w[count - 1], kept, (unsigned long long)(dst->size() / 4 - count), (unsigned long long)us);
    bool job = e == 0 && (pass->status() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU checks the job, runs no shader: %s) %s", stage, job ? "job accepted" : "job REFUSED", detail);
        if (!job) gFailed++;
        return job;
    }
    bool ok = job && right == count && kept == dst->size() / 4 - count;
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// Lines and points with the color program into 64x64 on black. Lines: a red one along row 32 from x 16.5 to 48.5 and a
// green one down column 10 from y 16.5 to 48.5 (width 1); points: four of size 1 at pixel centres. Judged: the
// pixels well inside each line, every point's own pixel, and every pixel more than one away from all of them black
bool runPrimitive(Device& device, bool lines) {
    const char* stage = lines ? "lines" : "points";
    std::string why;
    auto bo = device.gpu().newBo(256 * 64 + 4096, &why);
    if (!bo) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    Target t;
    t.bo = bo.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    auto ndc = [](float p) { return (p - 32.0f) / 32.0f; };
    // pixel-space vertices, then their colours (red, green, blue, white for the points)
    const float lx[4] = {16.5f, 48.5f, 10.5f, 10.5f}, ly[4] = {32.5f, 32.5f, 16.5f, 48.5f};
    const float px[4] = {8.5f, 40.5f, 20.5f, 55.5f}, py[4] = {8.5f, 20.5f, 50.5f, 55.5f};
    const float* vx = lines ? lx : px;
    const float* vy = lines ? ly : py;
    float pos[16], col[16];
    const float lineCol[16] = {1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    const float pointCol[16] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1};
    for (int i = 0; i < 4; i++) {
        pos[4 * i] = ndc(vx[i]), pos[4 * i + 1] = ndc(vy[i]), pos[4 * i + 2] = 0, pos[4 * i + 3] = 1;
    }
    memcpy(col, lines ? lineCol : pointCol, sizeof(col));
    const float one[4] = {1, 1, 1, 1};
    DrawInput in;
    in.attributes[0] = pos;
    in.attributes[1] = col;
    in.vertexCount = 4;
    in.uniforms = one;
    in.uniformSize = 16;
    in.primitive = lines ? DrawInput::Primitive::Lines : DrawInput::Primitive::Points;
    auto pass = DrawPass::make(device, t, hm_color, in, &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission ds, fs;
    uint64_t t0 = device.gpu().transport().nowUs();
    int e = pass->submit(&ds, &fs);
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    uint64_t us = device.gpu().transport().nowUs() - t0;
    auto px8 = [&](unsigned x, unsigned y) { return bo->cpu() + y * 256 + 4 * x; };
    auto is = [&](unsigned x, unsigned y, const float* c) {
        const uint8_t* q = px8(x, y);
        for (int k = 0; k < 4; k++) if (q[k] != unorm8(c[k])) return false;
        return true;
    };
    unsigned onRight = 0, onTotal = 0, offRight = 0, offTotal = 0, drawn = 0;
    if (lines) {
        for (unsigned x = 18; x <= 46; x++) onTotal++, onRight += is(x, 32, lineCol);
        for (unsigned y = 18; y <= 46; y++) onTotal++, onRight += is(10, y, lineCol + 8);
    } else {
        for (int i = 0; i < 4; i++) onTotal++, onRight += is(static_cast<unsigned>(px[i]), static_cast<unsigned>(py[i]), pointCol + 4 * i);
    }
    const float black[4] = {0, 0, 0, 1};
    for (unsigned y = 0; y < 64; y++) {
        for (unsigned x = 0; x < 64; x++) {
            float cx = x + 0.5f, cy = y + 0.5f;
            bool near = false;
            if (lines) {
                near |= cy >= 31.0f && cy <= 34.0f && cx >= 15.0f && cx <= 50.0f;
                near |= cx >= 9.0f && cx <= 12.0f && cy >= 15.0f && cy <= 50.0f;
            } else {
                for (int i = 0; i < 4; i++) near |= std::fabs(cx - px[i]) <= 1.0f && std::fabs(cy - py[i]) <= 1.0f;
            }
            drawn += !is(x, y, black);
            if (near) continue;
            offTotal++, offRight += is(x, y, black);
        }
    }
    char detail[300];
    snprintf(detail, sizeof(detail), "vertex %s tiler %s fragment %s%s%s; on the %s %u of %u right, elsewhere %u of %u black, "
             "%u pixels drawn; %llu us", statusText(pass->vertexStatus()).c_str(), statusText(pass->tilerStatus()).c_str(),
             statusText(pass->fragmentStatus()).c_str(), e ? ", " : "", e ? strerror(e) : "", stage, onRight, onTotal,
             offRight, offTotal, drawn, (unsigned long long)us);
    bool jobs = e == 0 && (pass->vertexStatus() & 0xff) == 1 && (pass->tilerStatus() & 0xff) == 1 &&
                (pass->fragmentStatus() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED", detail);
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && onRight == onTotal && offRight == offTotal;
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// --- one frame of draws into a 64x64 RGBA8 target, judged by a rule per pixel ----------------------------------

struct Frame {
    std::unique_ptr<Bo> bo;
    std::unique_ptr<DrawPass> pass;
    int e = 0;
    bool jobs = false;
    uint64_t us = 0;
    std::string why;
    const uint8_t* px(uint32_t x, uint32_t y) const { return bo->cpu() + y * 256 + 4 * x; }
};

Frame drawFrame(Device& device, const std::vector<std::pair<const hm_program*, DrawInput>>& draws) {
    Frame f;
    f.bo = device.gpu().newBo(256 * 64 + 4096, &f.why);
    if (!f.bo) return f;
    memset(f.bo->cpu(), kCanary, f.bo->size());
    Target t;
    t.bo = f.bo.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    f.pass = DrawPass::makeMany(device, t, draws, &f.why);
    if (!f.pass) {
        f.why = "refused: " + f.why;
        return f;
    }
    Submission ds, fs;
    uint64_t t0 = device.gpu().transport().nowUs();
    f.e = f.pass->submit(&ds, &fs);
    if (f.e == 0) f.e = device.gpu().wait(fs, 2000000);
    f.us = device.gpu().transport().nowUs() - t0;
    f.jobs = f.e == 0 && (f.pass->vertexStatus() & 0xff) == 1 && (f.pass->tilerStatus() & 0xff) == 1 &&
             (f.pass->fragmentStatus() & 0xff) == 1;
    return f;
}

std::string jobsText(const Frame& f) {
    if (!f.pass) return f.why;
    return "vertex " + statusText(f.pass->vertexStatus()) + " tiler " + statusText(f.pass->tilerStatus()) + " fragment " +
           statusText(f.pass->fragmentStatus()) + (f.e ? std::string(", ") + strerror(f.e) : std::string());
}

// a stage's end: on the fake GPU only the jobs are measured
bool finish(const char* stage, const std::vector<const Frame*>& frames, bool right, const std::string& detail) {
    bool jobs = true;
    for (const Frame* f : frames) jobs &= f->pass != nullptr && f->jobs;
    if (gFake) {
        say("%s MEASURE (the fake GPU does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED", detail.c_str());
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && right;
    verdict(stage, ok, detail);
    if (!ok || gDump) {
        for (const Frame* f : frames) if (f->pass) f->pass->dump(stdout);
    }
    return ok;
}

// +1 more than MARGIN pixels inside the triangle (pixel space), -1 more than MARGIN outside, 0 near an edge
int sideOf(float px, float py, const float vx[3], const float vy[3], float margin) {
    float area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vx[2] - vx[0]) * (vy[1] - vy[0]);
    float d = 1e9f;
    bool inside = true;
    for (int k = 0; k < 3; k++) {
        int i = (k + 1) % 3, j = (k + 2) % 3;
        float b = ((vx[j] - vx[i]) * (py - vy[i]) - (px - vx[i]) * (vy[j] - vy[i])) / area;
        float ex = vx[j] - vx[i], ey = vy[j] - vy[i];
        float dist = std::fabs(b * area) / std::sqrt(ex * ex + ey * ey);
        inside &= b > 0;
        d = dist < d ? dist : d;
    }
    if (d <= margin) return 0;
    return inside ? 1 : -1;
}

bool samePixel(const uint8_t* q, const uint8_t want[4], unsigned tolerance = 0) {
    for (int c = 0; c < 4; c++) {
        unsigned d = q[c] > want[c] ? q[c] - want[c] : want[c] - q[c];
        if (d > tolerance) return false;
    }
    return true;
}

const float kWhite12[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
const float kOne4[4] = {1, 1, 1, 1};
const uint8_t kWhitePx[4] = {255, 255, 255, 255}, kBlackPx[4] = {0, 0, 0, 255};

DrawInput colorDraw(const float* pos, const float* col, uint32_t vertices) {
    DrawInput in;
    in.attributes[0] = pos;
    in.attributes[1] = col;
    in.vertexCount = vertices;
    in.uniforms = kOne4;
    in.uniformSize = 16;
    return in;
}

// Linear filtering: a 2x2 texture (R along x, G along y) stretched over the 64x64 target: pixel x's red is
// 255 * clamp(2 (x + 0.5) / 64 - 0.5, 0, 1), as bilinear filtering with clamp to edge gives, within 3/255
bool runTextureLinear(Device& device) {
    const char* stage = "texture-linear";
    std::string why;
    auto tex = device.gpu().newBo(4096, &why);
    if (!tex) return verdict(stage, false, why);
    const uint8_t texels[2][2][4] = {{{0, 0, 0, 255}, {255, 0, 0, 255}}, {{0, 255, 0, 255}, {255, 255, 0, 255}}};
    for (int y = 0; y < 2; y++) memcpy(tex->cpu() + y * 64, texels[y], 8);
    const float pos[24] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
    const float uv[24] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0};
    DrawInput in;
    in.attributes[0] = pos;
    in.attributes[1] = uv;
    in.vertexCount = 6;
    in.texture = tex.get();
    in.textureWidth = in.textureHeight = 2;
    in.textureRowBytes = 64;
    in.nearest = false;
    Frame f = drawFrame(device, {{&hm_tex, in}});
    if (!f.bo) return verdict(stage, false, f.why);
    unsigned right = 0, worst = 0;
    char firstBad[120] = "";
    for (uint32_t y = 0; f.pass && y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            auto ramp = [](uint32_t i) {
                float s = 2.0f * (i + 0.5f) / 64.0f - 0.5f;
                return unorm8(s < 0 ? 0 : s > 1 ? 1 : s);
            };
            uint8_t want[4] = {ramp(x), ramp(y), 0, 255};
            const uint8_t* q = f.px(x, y);
            for (int c = 0; c < 4; c++) {
                unsigned d = q[c] > want[c] ? q[c] - want[c] : want[c] - q[c];
                worst = d > worst ? d : worst;
            }
            if (samePixel(q, want, 3)) right++;
            else if (firstBad[0] == 0)
                snprintf(firstBad, sizeof(firstBad), "; first wrong (%u,%u): %02x%02x%02x%02x, want %02x%02x%02x%02x", x, y,
                         q[0], q[1], q[2], q[3], want[0], want[1], want[2], want[3]);
        }
    }
    const uint8_t* mid = f.pass ? f.px(24, 40) : kBlackPx;
    char d[400];
    snprintf(d, sizeof(d), "%s; %u of 4096 pixels the bilinear value within 3 (largest difference %u), (24,40) %02x%02x%02x%02x; %llu us%s",
             jobsText(f).c_str(), right, worst, mid[0], mid[1], mid[2], mid[3], (unsigned long long)f.us, firstBad);
    return finish(stage, {&f}, right == 4096, d);
}

// Render to a texture, then sample it: pass 1 draws the varyings triangle into a 64x64 texture, pass 2 samples that
// texture with nearest filtering over a quad into another target, both committed before either is waited for (the
// queue in order); the second target must be the first, every pixel
bool runRenderToTexture(Device& device) {
    const char* stage = "render-to-texture";
    std::string why;
    auto tex = metal::Texture::make(device, 64, 64, Format::RGBA8Unorm, &why);
    if (!tex) return verdict(stage, false, why);
    const float tri[12] = {-0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0, 0.5f, 0, 1};
    const float rgb[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    DrawInput first = colorDraw(tri, rgb, 3);
    first.clear[0] = 0.2f, first.clear[1] = 0.1f, first.clear[2] = 0.3f;
    std::string w1;
    auto p1 = DrawPass::make(device, tex->target(), hm_color, first, &w1);
    if (!p1) return verdict(stage, false, "refused: " + w1);
    const float pos[24] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
    const float uv[24] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0};
    DrawInput second;
    second.attributes[0] = pos;
    second.attributes[1] = uv;
    second.vertexCount = 6;
    second.texture = &tex->bo();
    second.textureWidth = second.textureHeight = 64;
    second.textureRowBytes = tex->rowBytes();
    second.nearest = true;
    auto out = device.gpu().newBo(256 * 64 + 4096, &why);
    if (!out) return verdict(stage, false, why);
    memset(out->cpu(), kCanary, out->size());
    Target t;
    t.bo = out.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    auto p2 = DrawPass::make(device, t, hm_tex, second, &why);
    if (!p2) return verdict(stage, false, "refused: " + why);
    Submission a, b, c, d;
    uint64_t t0 = device.gpu().transport().nowUs();
    int e = p1->submit(&a, &b);
    if (e == 0) e = p2->submit(&c, &d);
    if (e == 0) e = device.gpu().wait(d, 2000000);
    uint64_t us = device.gpu().transport().nowUs() - t0;
    unsigned same = 0, drawn = 0;
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            const uint8_t* q = out->cpu() + y * 256 + 4 * x;
            const uint8_t* r = tex->bo().cpu() + y * tex->rowBytes() + 4 * x;
            same += memcmp(q, r, 4) == 0;
            drawn += !(r[0] == unorm8(0.2f) && r[1] == unorm8(0.1f) && r[2] == unorm8(0.3f));
        }
    }
    auto ok1 = [](const DrawPass& p) {
        return (p.vertexStatus() & 0xff) == 1 && (p.tilerStatus() & 0xff) == 1 && (p.fragmentStatus() & 0xff) == 1;
    };
    bool jobs = e == 0 && ok1(*p1) && ok1(*p2);
    char detail[300];
    snprintf(detail, sizeof(detail), "pass 1 fragment %s, pass 2 fragment %s%s%s; %u of 4096 pixels of the second target "
             "the texture's, %u pixels of the texture the triangle; %llu us", statusText(p1->fragmentStatus()).c_str(),
             statusText(p2->fragmentStatus()).c_str(), e ? ", " : "", e ? strerror(e) : "", same, drawn, (unsigned long long)us);
    if (gFake) {
        say("%s MEASURE (the fake GPU does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED", detail);
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && same == 4096 && drawn > 300 && drawn < 700;
    verdict(stage, ok, detail);
    if (!ok || gDump) p1->dump(stdout), p2->dump(stdout);
    return ok;
}

// The viewport and the scissor from the API. viewport: a triangle NDC (-1,-1) (1,-1) (-1,1) with the viewport
// (8, 16) 32x24 must cover pixels (8,16) (40,16) (8,40); scissor: a quad over the whole target with the scissor
// (10, 20) 30x16 writes exactly that rectangle
bool runViewportScissor(Device& device) {
    const char* stage = "viewport-scissor";
    const float tri[12] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1};
    DrawInput a = colorDraw(tri, kWhite12, 3);
    a.viewport.set = true;
    a.viewport.x = 8, a.viewport.y = 16, a.viewport.width = 32, a.viewport.height = 24;
    Frame fa = drawFrame(device, {{&hm_color, a}});
    const float quad[24] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
    const float white24[24] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    DrawInput b = colorDraw(quad, white24, 6);
    b.scissor.set = true;
    b.scissor.x = 10, b.scissor.y = 20, b.scissor.width = 30, b.scissor.height = 16;
    Frame fb = drawFrame(device, {{&hm_color, b}});
    if (!fa.bo || !fb.bo) return verdict(stage, false, fa.why + fb.why);
    unsigned inA = 0, inAok = 0, outA = 0, outAok = 0, rectOk = 0;
    const float vx[3] = {8, 40, 8}, vy[3] = {16, 16, 40};
    for (uint32_t y = 0; fa.pass && fb.pass && y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            int side = sideOf(x + 0.5f, y + 0.5f, vx, vy, 1.0f);
            if (side > 0) inA++, inAok += samePixel(fa.px(x, y), kWhitePx);
            if (side < 0) outA++, outAok += samePixel(fa.px(x, y), kBlackPx);
            bool inRect = x >= 10 && x < 40 && y >= 20 && y < 36;
            rectOk += samePixel(fb.px(x, y), inRect ? kWhitePx : kBlackPx);
        }
    }
    char d[400];
    snprintf(d, sizeof(d), "viewport: %s; inside %u of %u white, outside %u of %u black; scissor: %s; %u of 4096 pixels "
             "white exactly in (10,20)-(39,35), black elsewhere", jobsText(fa).c_str(), inAok, inA, outAok, outA,
             jobsText(fb).c_str(), rectOk);
    return finish(stage, {&fa, &fb}, inAok == inA && outAok == outA && inA > 250 && rectOk == 4096, d);
}

// Culling and winding: triangle A counter-clockwise in NDC (y up), B clockwise, side by side, in four frames:
// cull none (both), back with CCW front (A), back with CW front (B), front with CCW front (B) -- GL's and Metal's
// rule, the winding of the vertices as they land in a target whose row 0 is NDC y -1
bool runCull(Device& device) {
    const char* stage = "cull";
    const float pos[24] = {-0.9f, -0.5f, 0, 1, -0.1f, -0.5f, 0, 1, -0.5f, 0.5f, 0, 1,
                           0.1f, -0.5f, 0, 1, 0.5f, 0.5f, 0, 1, 0.9f, -0.5f, 0, 1};
    const float white24[24] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    struct Case { DrawInput::Cull cull; bool ccw; const char* want; } cases[] = {
        {DrawInput::Cull::None, false, "AB"}, {DrawInput::Cull::Back, true, "A-"},
        {DrawInput::Cull::Back, false, "-B"}, {DrawInput::Cull::Front, true, "-B"},
    };
    std::string got, want;
    std::vector<Frame> frames;
    bool right = true;
    for (const Case& c : cases) {
        DrawInput in = colorDraw(pos, white24, 6);
        in.cull = c.cull;
        in.frontCCW = c.ccw;
        frames.push_back(drawFrame(device, {{&hm_color, in}}));
        const Frame& f = frames.back();
        if (!f.bo) return verdict(stage, false, f.why);
        std::string g = "??";
        if (f.pass) {
            g[0] = samePixel(f.px(16, 26), kWhitePx) ? 'A' : samePixel(f.px(16, 26), kBlackPx) ? '-' : '?';
            g[1] = samePixel(f.px(48, 26), kWhitePx) ? 'B' : samePixel(f.px(48, 26), kBlackPx) ? '-' : '?';
        }
        right &= g == c.want;
        got += (got.empty() ? "" : " ") + g;
        want += (want.empty() ? "" : " ") + std::string(c.want);
    }
    std::vector<const Frame*> fp;
    for (const Frame& f : frames) fp.push_back(&f);
    char d[300];
    snprintf(d, sizeof(d), "%s; drawn %s, want %s (none, back+CCW, back+CW, front+CCW)", jobsText(frames[0]).c_str(),
             got.c_str(), want.c_str());
    return finish(stage, fp, right, d);
}

// Stencil, in the tile buffer (cleared to 0): draw 1 the triangle (16,16) (48,16) (32,48) in red, stencil Always,
// Replace with reference 1; draw 2 a quad over the whole target in green, stencil Equal 1, Keep. Inside the triangle
// green, outside the black clear
bool runStencil(Device& device) {
    const char* stage = "stencil";
    const float tri[12] = {-0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0, 0.5f, 0, 1};
    const float red[12] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    const float quad[24] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
    const float green[24] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    hm_v7_zs mark = {}, test = {};
    mark.depth_compare = test.depth_compare = HM_COMPARE_ALWAYS;
    mark.stencil_enable = test.stencil_enable = true;
    mark.reference = test.reference = 1;
    mark.front = {HM_COMPARE_ALWAYS, HM_STENCIL_KEEP, HM_STENCIL_KEEP, HM_STENCIL_REPLACE, 0xff, 0xff};
    test.front = {HM_COMPARE_EQUAL, HM_STENCIL_KEEP, HM_STENCIL_KEEP, HM_STENCIL_KEEP, 0xff, 0x00};
    mark.back = mark.front;
    test.back = test.front;
    DrawInput a = colorDraw(tri, red, 3);
    a.zs = &mark;
    DrawInput b = colorDraw(quad, green, 6);
    b.zs = &test;
    Frame f = drawFrame(device, {{&hm_color, a}, {&hm_color, b}});
    if (!f.bo) return verdict(stage, false, f.why);
    const uint8_t greenPx[4] = {0, 255, 0, 255};
    const float vx[3] = {16, 48, 32}, vy[3] = {16, 16, 48};
    unsigned in = 0, inOk = 0, out = 0, outOk = 0, redLeft = 0;
    for (uint32_t y = 0; f.pass && y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            int side = sideOf(x + 0.5f, y + 0.5f, vx, vy, 1.0f);
            if (side > 0) in++, inOk += samePixel(f.px(x, y), greenPx);
            if (side < 0) out++, outOk += samePixel(f.px(x, y), kBlackPx);
            redLeft += f.px(x, y)[0] == 255 && f.px(x, y)[1] == 0;
        }
    }
    char d[300];
    snprintf(d, sizeof(d), "%s; inside %u of %u green, outside %u of %u black, %u pixels left red; %llu us",
             jobsText(f).c_str(), inOk, in, outOk, out, redLeft, (unsigned long long)f.us);
    return finish(stage, {&f}, in > 300 && inOk == in && outOk == out && redLeft == 0, d);
}

// Instancing with the inst program (a position per vertex, an offset and a colour per instance). Frame 1: a triangle
// of 3 vertices, 4 instances to the four quadrants in red, green, blue, white (padded count 3: the NPOT divisor and its
// continuation record); frame 2: a line of 2 vertices, 3 instances 20 rows apart (padded count 2: a POT divisor).
// Every instance where its offset puts it, nothing where none is
bool runInstanced(Device& device) {
    const char* stage = "instanced";
    const float tri[12] = {-0.2f, -0.2f, 0, 1, 0.2f, -0.2f, 0, 1, 0, 0.2f, 0, 1};
    const float off4[16] = {-0.5f, -0.5f, 0, 0, 0.5f, -0.5f, 0, 0, -0.5f, 0.5f, 0, 0, 0.5f, 0.5f, 0, 0};
    const float col4[16] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1};
    DrawInput a;
    a.attributes[0] = tri;
    a.attributes[1] = off4;
    a.attributes[2] = col4;
    a.vertexCount = 3;
    a.instanceCount = 4;
    a.perInstance = 0x6;
    Frame fa = drawFrame(device, {{&hm_inst, a}});
    auto ndcx = [](float px) { return (px - 32.0f) / 32.0f; };
    const float line[8] = {ndcx(16.5f), ndcx(10.5f), 0, 1, ndcx(48.5f), ndcx(10.5f), 0, 1};
    const float off3[12] = {0, 0, 0, 0, 0, 0.625f, 0, 0, 0, 1.25f, 0, 0};
    const float col3[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    DrawInput b;
    b.attributes[0] = line;
    b.attributes[1] = off3;
    b.attributes[2] = col3;
    b.vertexCount = 2;
    b.instanceCount = 3;
    b.perInstance = 0x6;
    b.primitive = DrawInput::Primitive::Lines;
    Frame fb = drawFrame(device, {{&hm_inst, b}});
    if (!fa.bo || !fb.bo) return verdict(stage, false, fa.why + fb.why);
    unsigned in = 0, inOk = 0, out = 0, outOk = 0;
    for (uint32_t y = 0; fa.pass && y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            int where = -1;
            for (int i = 0; i < 4; i++) {
                float dx = off4[4 * i] * 32, dy = off4[4 * i + 1] * 32;
                const float vx[3] = {25.6f + dx, 38.4f + dx, 32 + dx}, vy[3] = {25.6f + dy, 25.6f + dy, 38.4f + dy};
                int side = sideOf(x + 0.5f, y + 0.5f, vx, vy, 1.0f);
                if (side > 0) where = i;
                if (side == 0 && where < 0) where = -2;
            }
            if (where >= 0) {
                uint8_t want[4];
                for (int c = 0; c < 4; c++) want[c] = unorm8(col4[4 * where + c]);
                in++, inOk += samePixel(fa.px(x, y), want);
            } else if (where == -1) {
                out++, outOk += samePixel(fa.px(x, y), kBlackPx);
            }
        }
    }
    unsigned onLine = 0, onLineOk = 0, drawnB = 0;
    for (uint32_t y = 0; fb.pass && y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            drawnB += !samePixel(fb.px(x, y), kBlackPx);
            for (int i = 0; i < 3; i++) {
                if (y == 10u + 20u * i && x >= 18 && x <= 46) {
                    uint8_t want[4];
                    for (int c = 0; c < 4; c++) want[c] = unorm8(col3[4 * i + c]);
                    onLine++, onLineOk += samePixel(fb.px(x, y), want);
                }
            }
        }
    }
    char d[400];
    snprintf(d, sizeof(d), "triangles: %s; inside the 4 instances %u of %u their colour, elsewhere %u of %u black; lines: %s; "
             "%u of %u on the 3 instances' rows their colour, %u pixels drawn (96 for three lines of 32)",
             jobsText(fa).c_str(), inOk, in, outOk, out, jobsText(fb).c_str(), onLineOk, onLine, drawnB);
    return finish(stage, {&fa, &fb}, in >= 120 && inOk == in && outOk == out && onLineOk == onLine && drawnB == 96, d);
}

// More pixel formats, R8Unorm and RG8Unorm (pan_format.c: R8G8B8A8 in the tile buffer, written back as R8 or R8G8):
// a clear of R8 (one byte a pixel, the rest of each 128-byte row kept); the varyings triangle into RG8 (two bytes a
// pixel, red and green interpolated within 2/255, the clear outside); a 4x4 R8 texture and a 4x4 RG8 texture sampled
// nearest into RGBA8: (r, 0, 0, 1) and (r, g, 0, 1), as Metal reads them
bool runFormats(Device& device) {
    const char* stage = "formats";
    std::string why;
    std::string detail;
    bool right = true;
    std::vector<std::unique_ptr<DrawPass>> passes;
    bool jobs = true;
    // 1. a clear of R8
    {
        auto bo = device.gpu().newBo(128 * 64 + 4096, &why);
        if (!bo) return verdict(stage, false, why);
        memset(bo->cpu(), kCanary, bo->size());
        Target t;
        t.bo = bo.get();
        t.width = t.height = 64;
        t.rowBytes = 128;
        t.format = Format::R8Unorm;
        const float c[4] = {0.2f, 0.4f, 0.6f, 0.8f};
        auto pass = ClearPass::make(device, t, c, ClearOptions(), &why);
        if (!pass) return verdict(stage, false, "R8 clear refused: " + why);
        Submission sub;
        int e = pass->submit(&sub);
        if (e == 0) e = device.gpu().wait(sub, 2000000);
        jobs &= e == 0 && (pass->status() & 0xff) == 1;
        unsigned px = 0, kept = 0;
        for (uint32_t y = 0; y < 64; y++) {
            for (uint32_t x = 0; x < 128; x++) {
                uint8_t b = bo->cpu()[y * 128 + x];
                if (x < 64) px += b == 0x33;
                else kept += b == kCanary;
            }
        }
        right &= px == 4096 && kept == 4096;
        detail += "R8 clear " + statusText(pass->status()) + ": " + std::to_string(px) + " of 4096 bytes 33, " +
                  std::to_string(kept) + " of 4096 padding kept";
    }
    // 2. the varyings triangle into RG8
    {
        auto bo = device.gpu().newBo(256 * 64 + 4096, &why);
        if (!bo) return verdict(stage, false, why);
        memset(bo->cpu(), kCanary, bo->size());
        Target t;
        t.bo = bo.get();
        t.width = t.height = 64;
        t.rowBytes = 256;
        t.format = Format::RG8Unorm;
        const float tri[12] = {-0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0, 0.5f, 0, 1};
        const float rgb[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
        DrawInput in = colorDraw(tri, rgb, 3);
        in.clear[0] = 0.2f, in.clear[1] = 0.4f;
        auto pass = DrawPass::make(device, t, hm_color, in, &why);
        if (!pass) return verdict(stage, false, "RG8 draw refused: " + why);
        Submission ds, fs;
        int e = pass->submit(&ds, &fs);
        if (e == 0) e = device.gpu().wait(fs, 2000000);
        jobs &= e == 0 && (pass->fragmentStatus() & 0xff) == 1 && (pass->tilerStatus() & 0xff) == 1;
        const float vx[3] = {16, 48, 32}, vy[3] = {16, 16, 48};
        float area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vx[2] - vx[0]) * (vy[1] - vy[0]);
        unsigned in1 = 0, in1ok = 0, out1 = 0, out1ok = 0, kept = 0;
        for (uint32_t y = 0; y < 64; y++) {
            for (uint32_t x = 0; x < 64; x++) {
                const uint8_t* q = bo->cpu() + y * 256 + 2 * x;
                int side = sideOf(x + 0.5f, y + 0.5f, vx, vy, 1.0f);
                if (side > 0) {
                    float b[3];
                    for (int k = 0; k < 3; k++) {
                        int i = (k + 1) % 3, j = (k + 2) % 3;
                        b[k] = ((vx[j] - vx[i]) * (y + 0.5f - vy[i]) - (x + 0.5f - vx[i]) * (vy[j] - vy[i])) / area;
                    }
                    uint8_t r = unorm8(b[0]), g = unorm8(b[1]);
                    in1++, in1ok += std::abs(q[0] - r) <= 2 && std::abs(q[1] - g) <= 2;
                } else if (side < 0) {
                    out1++, out1ok += q[0] == 0x33 && q[1] == 0x66;
                }
            }
            for (uint32_t x = 128; x < 256; x++) kept += bo->cpu()[y * 256 + x] == kCanary;
        }
        right &= in1 > 300 && in1ok == in1 && out1ok == out1 && kept == 64 * 128;
        detail += "; RG8 draw " + statusText(pass->fragmentStatus()) + ": inside " + std::to_string(in1ok) + " of " +
                  std::to_string(in1) + " right, outside " + std::to_string(out1ok) + " of " + std::to_string(out1) +
                  " the clear, padding " + std::to_string(kept) + " of 8192 kept";
        passes.push_back(std::move(pass));
    }
    // 3. R8 and RG8 textures sampled
    for (Format tf : {Format::R8Unorm, Format::RG8Unorm}) {
        const uint32_t bpp = bytesPerPixel(tf);
        auto tex = device.gpu().newBo(4096, &why);
        if (!tex) return verdict(stage, false, why);
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                uint8_t* q = tex->cpu() + y * 64 + bpp * x;
                q[0] = static_cast<uint8_t>(17 * (4 * y + x));
                if (bpp == 2) q[1] = static_cast<uint8_t>(255 - 16 * (4 * y + x));
            }
        }
        const float pos[24] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
        const float uv[24] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0};
        DrawInput in;
        in.attributes[0] = pos;
        in.attributes[1] = uv;
        in.vertexCount = 6;
        in.texture = tex.get();
        in.textureFormat = tf;
        in.textureWidth = in.textureHeight = 4;
        in.textureRowBytes = 64;
        Frame f = drawFrame(device, {{&hm_tex, in}});
        if (!f.bo) return verdict(stage, false, f.why);
        jobs &= f.jobs;
        unsigned ok = 0, judged = 0;
        char bad[100] = "";
        for (uint32_t y = 0; f.pass && y < 64; y++) {
            for (uint32_t x = 0; x < 64; x++) {
                if (x % 16 == 0 || x % 16 == 15 || y % 16 == 0 || y % 16 == 15) continue;
                int i = (y / 16) * 4 + x / 16;
                uint8_t want[4] = {static_cast<uint8_t>(17 * i), static_cast<uint8_t>(bpp == 2 ? 255 - 16 * i : 0), 0, 255};
                judged++;
                if (samePixel(f.px(x, y), want)) ok++;
                else if (!bad[0]) {
                    const uint8_t* q = f.px(x, y);
                    snprintf(bad, sizeof(bad), " (first wrong (%u,%u) %02x%02x%02x%02x, want %02x%02x%02x%02x)", x, y, q[0],
                             q[1], q[2], q[3], want[0], want[1], want[2], want[3]);
                }
            }
        }
        right &= ok == judged;
        detail += std::string("; ") + (bpp == 1 ? "R8" : "RG8") + " texture " + jobsText(f).substr(jobsText(f).find("fragment")) +
                  ": " + std::to_string(ok) + " of " + std::to_string(judged) + " right" + bad;
        passes.push_back(std::move(f.pass));
    }
    if (gFake) {
        say("%s MEASURE (the fake GPU clears, does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED",
            detail.c_str());
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && right;
    verdict(stage, ok, detail);
    if (!ok || gDump) {
        for (auto& p : passes) if (p) p->dump(stdout);
    }
    return ok;
}

// Mipmaps: a 16x16 RGBA8 texture of five levels (16, 8, 4, 2, 1 texels a side), each a colour of its own, sampled
// with Nearest mip filtering on quads drawn through viewports of 16, 8, 4 and 2 pixels -- LOD 0, 1, 2 and 3 exactly --
// and a fifth of 16 pixels with the LOD clamped to 4..4; each quad must be its level's colour, the rest the clear
bool runMipmaps(Device& device) {
    const char* stage = "mipmaps";
    std::string why;
    auto tex = device.gpu().newBo(4096, &why);
    if (!tex) return verdict(stage, false, why);
    const uint8_t colours[5][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {255, 0, 255, 255}};
    const uint64_t offsets[5] = {0, 1024, 1536, 1792, 1920};
    for (int l = 0; l < 5; l++) {
        uint32_t side = 16u >> l;
        for (uint32_t y = 0; y < side; y++) {
            for (uint32_t x = 0; x < side; x++) memcpy(tex->cpu() + offsets[l] + y * 64 + 4 * x, colours[l], 4);
        }
    }
    const float pos[24] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
    const float uv[24] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0};
    struct Quad { uint32_t x, y, side; float minLod, maxLod; int level; } quads[] = {
        {0, 0, 16, 0, 1000, 0}, {20, 0, 8, 0, 1000, 1}, {32, 0, 4, 0, 1000, 2}, {40, 0, 2, 0, 1000, 3}, {0, 32, 16, 4, 4, 4},
    };
    std::vector<std::pair<const hm_program*, DrawInput>> draws;
    for (const Quad& q : quads) {
        DrawInput in;
        in.attributes[0] = pos;
        in.attributes[1] = uv;
        in.vertexCount = 6;
        in.texture = tex.get();
        in.textureWidth = in.textureHeight = 16;
        in.textureRowBytes = 64;
        in.textureLevels = 5;
        for (int l = 1; l < 5; l++) in.textureLevelOffset[l] = offsets[l], in.textureLevelRowBytes[l] = 64;
        in.mip = HM_MIP_NEAREST;
        in.minLod = q.minLod, in.maxLod = q.maxLod;
        in.viewport.set = true;
        in.viewport.x = static_cast<float>(q.x), in.viewport.y = static_cast<float>(q.y);
        in.viewport.width = in.viewport.height = static_cast<float>(q.side);
        draws.emplace_back(&hm_tex, in);
    }
    Frame f = drawFrame(device, draws);
    if (!f.bo) return verdict(stage, false, f.why);
    unsigned right[5] = {}, total[5] = {}, rest = 0, restOk = 0;
    for (uint32_t y = 0; f.pass && y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            int which = -1;
            for (int i = 0; i < 5; i++) {
                if (x >= quads[i].x && x < quads[i].x + quads[i].side && y >= quads[i].y && y < quads[i].y + quads[i].side) which = i;
            }
            if (which >= 0) total[which]++, right[which] += samePixel(f.px(x, y), colours[quads[which].level]);
            else rest++, restOk += samePixel(f.px(x, y), kBlackPx);
        }
    }
    std::string d = jobsText(f);
    bool ok = true;
    for (int i = 0; i < 5; i++) {
        const uint8_t* c = f.pass ? f.px(quads[i].x, quads[i].y) : kBlackPx;
        char b[96];
        snprintf(b, sizeof(b), "; %ux%u%s: %u of %u level %d (%02x%02x%02x)", quads[i].side, quads[i].side,
                 i == 4 ? " LOD 4..4" : "", right[i], total[i], quads[i].level, c[0], c[1], c[2]);
        d += b;
        ok &= right[i] == total[i];
    }
    d += "; elsewhere " + std::to_string(restOk) + " of " + std::to_string(rest) + " black";
    return finish(stage, {&f}, ok && restOk == rest, d);
}

// The blit encoder's texture work (blit.hpp). blit-texture: a 37x21 region of a 64x64 RGBA8 texture at (8,4) copied to
// (20,30) of another, by the copy2d kernel: the region the source's, every other byte of the destination kept.
// generate-mipmaps: a 32x32 RGBA8 texture's five smaller levels drawn one from the other with a linear sampler: every
// texel of level l the average of its four texels in level l - 1 (as the GPU wrote that level) within 1
bool runBlitTexture(Device& device) {
    const char* stage = "blit-texture";
    std::string why;
    auto a = device.gpu().newBo(256 * 64, &why);
    auto b = a ? device.gpu().newBo(256 * 64, &why) : nullptr;
    if (!a || !b) return verdict(stage, false, why);
    for (uint32_t i = 0; i < 64 * 64; i++) {
        uint32_t v = 0x9e3779b9u * (i + 7);
        memcpy(a->cpu() + 4 * i, &v, 4);
    }
    memset(b->cpu(), kCanary, b->size());
    Image src{a.get(), 0, 64, 64, 256, Format::RGBA8Unorm}, dst{b.get(), 0, 64, 64, 256, Format::RGBA8Unorm};
    auto pass = makeTextureCopy(device, src, 8, 4, 37, 21, dst, 20, 30, &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission sub;
    uint64_t t0 = device.gpu().transport().nowUs();
    int e = pass->submit({b.get()}, &sub);
    if (e == 0) e = device.gpu().wait(sub, 2000000);
    uint64_t us = device.gpu().transport().nowUs() - t0;
    unsigned copied = 0, kept = 0;
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            const uint8_t* q = b->cpu() + y * 256 + 4 * x;
            if (x >= 20 && x < 57 && y >= 30 && y < 51) {
                copied += memcmp(q, a->cpu() + (y - 30 + 4) * 256 + 4 * (x - 20 + 8), 4) == 0;
            } else {
                kept += q[0] == kCanary && q[1] == kCanary && q[2] == kCanary && q[3] == kCanary;
            }
        }
    }
    char d[300];
    snprintf(d, sizeof(d), "job %s%s%s; %u of 777 texels the source's, %u of 3319 outside kept; %llu us",
             statusText(pass->status()).c_str(), e ? ", " : "", e ? strerror(e) : "", copied, kept, (unsigned long long)us);
    bool job = e == 0 && (pass->status() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU runs no shader: %s) %s", stage, job ? "job accepted" : "job REFUSED", d);
        if (!job) gFailed++;
        return job;
    }
    bool ok = job && copied == 777 && kept == 3319;
    verdict(stage, ok, d);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

bool runGenerateMipmaps(Device& device) {
    const char* stage = "generate-mipmaps";
    std::string why;
    auto bo = device.gpu().newBo(6 * 4096, &why);
    if (!bo) return verdict(stage, false, why);
    memset(bo->cpu(), 0, bo->size());
    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            uint8_t* q = bo->cpu() + y * 128 + 4 * x;
            q[0] = static_cast<uint8_t>(x * 8), q[1] = static_cast<uint8_t>(y * 8);
            q[2] = static_cast<uint8_t>((x * 37 + y * 91) & 255), q[3] = 255;
        }
    }
    std::vector<Image> levels;
    for (uint32_t l = 0; l < 6; l++) levels.push_back({bo.get(), l * 4096ull, 32u >> l, 32u >> l, 128, Format::RGBA8Unorm});
    auto passes = makeMipmaps(device, levels, &why);
    if (passes.empty()) return verdict(stage, false, "refused: " + why);
    Submission ds, fs;
    int e = 0;
    uint64_t t0 = device.gpu().transport().nowUs();
    for (auto& p : passes) {
        if (e == 0) e = p->submit(&ds, &fs);
    }
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    uint64_t us = device.gpu().transport().nowUs() - t0;
    bool jobs = e == 0;
    for (auto& p : passes) jobs &= (p->fragmentStatus() & 0xff) == 1 && (p->tilerStatus() & 0xff) == 1;
    std::string d = std::string("fragment jobs ") + (jobs ? "DONE" : "NOT all DONE") + (e ? std::string(", ") + strerror(e) : "");
    bool right = true;
    for (uint32_t l = 1; l < 6; l++) {
        uint32_t side = 32u >> l;
        unsigned ok = 0, worst = 0;
        for (uint32_t y = 0; y < side; y++) {
            for (uint32_t x = 0; x < side; x++) {
                const uint8_t* q = bo->cpu() + l * 4096 + y * 128 + 4 * x;
                const uint8_t* p = bo->cpu() + (l - 1) * 4096 + (2 * y) * 128 + 4 * (2 * x);
                bool good = true;
                for (int c = 0; c < 4; c++) {
                    float avg = (p[c] + p[4 + c] + p[128 + c] + p[132 + c]) / 4.0f;
                    float dd = std::fabs(q[c] - avg);
                    good &= dd <= 1.0f;
                    worst = static_cast<unsigned>(dd) > worst ? static_cast<unsigned>(dd) : worst;
                }
                ok += good;
            }
        }
        right &= ok == side * side;
        d += "; level " + std::to_string(l) + " " + std::to_string(ok) + "/" + std::to_string(side * side) +
             (worst > 1 ? " (off by " + std::to_string(worst) + ")" : "");
    }
    const uint8_t* last = bo->cpu() + 5 * 4096;
    char t[80];
    snprintf(t, sizeof(t), "; level 5 %02x%02x%02x%02x; %llu us", last[0], last[1], last[2], last[3], (unsigned long long)us);
    d += t;
    if (gFake) {
        say("%s MEASURE (the fake GPU does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED", d.c_str());
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && right;
    verdict(stage, ok, d);
    if (!ok || gDump) {
        for (auto& p : passes) p->dump(stdout);
    }
    return ok;
}

// Apple's own render pipeline: ARKitCore's resample_v (a position and a uv per vertex read out of device buffers 0 and
// 1 by [[vertex_id]]) and resample_f (texture 0 sampled at the uv through its constexpr sampler), compiled on the phone
// by the AIR frontend and Mesa's Bifrost compiler (hm_compile_air_program), drawn over a 4x4 texture of sixteen
// colours on a quad covering the 64x64 target: pixel (x, y) the texel (x / 16, y / 16), within 1/255 (the fragment
// function's output is half4), judged away from the texel edges
bool runAppleRender(Device& device) {
    const char* stage = "apple-render";
#if !defined(HAKIOS_MALI_COMPILER)
    (void)device;
    (void)stage;
    say("apple-render MEASURE not built in (needs the compiler)");
    return true;
#else
    const char* path = "/hakios/share/ARKitCore.metallib";
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) return verdict(stage, false, std::string("no ") + path);
    std::vector<uint8_t> lib;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) lib.insert(lib.end(), chunk, chunk + n);
    fclose(fp);
    hm_program prog;
    uint32_t dev = 0;
    bool linear = false;
    char why[300];
    uint64_t c0 = device.gpu().transport().nowUs();
    if (hm_compile_air_program(lib.data(), lib.size(), "resample_v", "resample_f", static_cast<unsigned>(device.productId()),
                               &prog, &dev, &linear, why, sizeof(why)) != 0)
        return verdict(stage, false, why);
    uint64_t compileUs = device.gpu().transport().nowUs() - c0;
    std::string w;
    auto tex = device.gpu().newBo(4096, &w);
    auto buf = tex ? device.gpu().newBo(4096, &w) : nullptr;
    if (!tex || !buf) return verdict(stage, false, w);
    uint8_t texel[16][4];
    for (int i = 0; i < 16; i++) {
        texel[i][0] = static_cast<uint8_t>(17 * i), texel[i][1] = static_cast<uint8_t>(255 - 16 * i);
        texel[i][2] = static_cast<uint8_t>((i * 97) & 255), texel[i][3] = 255;
    }
    for (int y = 0; y < 4; y++) memcpy(tex->cpu() + y * 64, texel[4 * y], 16);
    // device buffer 0: float2 positions, buffer 1 (at 256): float2 uvs, six vertices of two triangles
    const float pos[12] = {-1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1};
    const float uv[12] = {0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1};
    memcpy(buf->cpu(), pos, sizeof(pos));
    memcpy(buf->cpu() + 256, uv, sizeof(uv));
    DrawInput in;
    in.vertexCount = 6;
    in.texture = tex.get();
    in.textureWidth = in.textureHeight = 4;
    in.textureRowBytes = 64;
    in.nearest = !linear;
    in.deviceBuffers[0] = buf.get(), in.deviceOffsets[0] = 0;
    in.deviceBuffers[1] = buf.get(), in.deviceOffsets[1] = 256;
    Frame f = drawFrame(device, {{&prog, in}});
    if (!f.bo) {
        hm_program_free(&prog);
        return verdict(stage, false, f.why);
    }
    unsigned right = 0, judged = 0;
    char bad[120] = "";
    for (uint32_t y = 0; f.pass && y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            if (x % 16 == 0 || x % 16 == 15 || y % 16 == 0 || y % 16 == 15) continue;
            judged++;
            const uint8_t* want = texel[(y / 16) * 4 + x / 16];
            if (samePixel(f.px(x, y), want, 1)) right++;
            else if (!bad[0]) {
                const uint8_t* q = f.px(x, y);
                snprintf(bad, sizeof(bad), "; first wrong (%u,%u): %02x%02x%02x%02x, want %02x%02x%02x%02x", x, y, q[0], q[1],
                         q[2], q[3], want[0], want[1], want[2], want[3]);
            }
        }
    }
    char d[420];
    snprintf(d, sizeof(d), "%s compiled on the phone in %llu us (vs %u, fs %u bytes, device buffers %#x, %s sampler); %s; "
             "%u of %u pixels the texel their uv names%s", prog.name, (unsigned long long)compileUs, prog.vs_size, prog.fs_size,
             dev, linear ? "linear" : "nearest", jobsText(f).c_str(), right, judged, bad);
    bool ok = finish(stage, {&f}, judged > 0 && right == judged, d);
    hm_program_free(&prog);
    return ok;
#endif
}

// A vertex descriptor (MTLVertexDescriptor): the varyings triangle's position as Float2 at offset 0 and its colour as
// UChar4Normalized at offset 8 of one interleaved buffer, 12 bytes a vertex; the hardware widens both (z 0, w 1 for
// the position): the same picture as the varyings stage, every pixel within 2/255
bool runVertexDescriptor(Device& device) {
    const char* stage = "vertex-descriptor";
    struct V { float x, y; uint8_t c[4]; } verts[3] = {
        {-0.5f, -0.5f, {255, 0, 0, 255}}, {0.5f, -0.5f, {0, 255, 0, 255}}, {0.0f, 0.5f, {0, 0, 255, 255}}};
    static_assert(sizeof(V) == 12, "12 bytes a vertex");
    DrawInput in;
    in.vertexCount = 3;
    in.uniforms = kOne4;
    in.uniformSize = 16;
    in.vertexLayout.set = true;
    in.vertexLayout.data = verts;
    in.vertexLayout.stride = 12;
    in.vertexLayout.format[0] = 0, in.vertexLayout.offset[0] = 0;      // Float2
    in.vertexLayout.format[1] = 3, in.vertexLayout.offset[1] = 8;      // UChar4Normalized
    Frame f = drawFrame(device, {{&hm_color, in}});
    if (!f.bo) return verdict(stage, false, f.why);
    const float vx[3] = {16, 48, 32}, vy[3] = {16, 16, 48};
    float area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vx[2] - vx[0]) * (vy[1] - vy[0]);
    unsigned in1 = 0, ok1 = 0, out1 = 0, ok2 = 0;
    for (uint32_t y = 0; f.pass && y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            int side = sideOf(x + 0.5f, y + 0.5f, vx, vy, 1.0f);
            if (side > 0) {
                float b[3];
                for (int k = 0; k < 3; k++) {
                    int i = (k + 1) % 3, j = (k + 2) % 3;
                    b[k] = ((vx[j] - vx[i]) * (y + 0.5f - vy[i]) - (x + 0.5f - vx[i]) * (vy[j] - vy[i])) / area;
                }
                uint8_t want[4] = {unorm8(b[0]), unorm8(b[1]), unorm8(b[2]), 255};
                in1++, ok1 += samePixel(f.px(x, y), want, 2);
            } else if (side < 0) {
                out1++, ok2 += samePixel(f.px(x, y), kBlackPx);
            }
        }
    }
    const uint8_t* mid = f.pass ? f.px(32, 26) : kBlackPx;
    char d[300];
    snprintf(d, sizeof(d), "%s; inside %u of %u the interpolated colour, outside %u of %u black; (32,26) %02x%02x%02x%02x",
             jobsText(f).c_str(), ok1, in1, ok2, out1, mid[0], mid[1], mid[2], mid[3]);
    return finish(stage, {&f}, in1 > 300 && ok1 == in1 && ok2 == out1, d);
}

// An argument buffer as Metal 3 has them: a buffer holding the GPU addresses of two other buffers (gpuAddress) and two
// values, which the argbuf kernel reads and follows -- dst[i] = src[i] + add for i < count; 200 words, the canary after
bool runArgumentBuffer(Device& device) {
    const char* stage = "argument-buffer";
    std::string why;
    auto args = device.gpu().newBo(4096, &why);
    auto src = args ? device.gpu().newBo(4096, &why) : nullptr;
    auto dst = src ? device.gpu().newBo(4096, &why) : nullptr;
    if (!dst) return verdict(stage, false, why);
    uint32_t* in = reinterpret_cast<uint32_t*>(src->cpu());
    for (unsigned i = 0; i < 1024; i++) in[i] = 0x01000193u * (i + 3);
    memset(dst->cpu(), kCanary, dst->size());
    const unsigned count = 200;
    const uint32_t add = 0x1234;
    uint64_t a = src->gpu(), b = dst->gpu();
    memcpy(args->cpu(), &a, 8);
    memcpy(args->cpu() + 8, &b, 8);
    memcpy(args->cpu() + 16, &count, 4);
    memcpy(args->cpu() + 20, &add, 4);
    uint32_t u[4] = {static_cast<uint32_t>(args->gpu()), static_cast<uint32_t>(args->gpu() >> 32), 0, 0};
    const unsigned groups[3] = {(count + 63) / 64, 1, 1};
    auto pass = ComputePass::make(device, hm_argbuf, groups, u, sizeof(u), &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission sub;
    int e = pass->submit({dst.get(), args.get(), src.get()}, &sub);
    if (e == 0) e = device.gpu().wait(sub, 2000000);
    const uint32_t* w = reinterpret_cast<const uint32_t*>(dst->cpu());
    unsigned right = 0, kept = 0;
    for (unsigned i = 0; i < count; i++) right += w[i] == in[i] + add;
    for (unsigned i = count; i < dst->size() / 4; i++) kept += w[i] == 0xa5a5a5a5u;
    char d[200];
    snprintf(d, sizeof(d), "job %s%s%s; %u of %u words src + 0x1234 through the argument buffer's addresses, canary %u of %llu kept",
             statusText(pass->status()).c_str(), e ? ", " : "", e ? strerror(e) : "", right, count, kept,
             (unsigned long long)(dst->size() / 4 - count));
    bool job = e == 0 && (pass->status() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU runs no shader: %s) %s", stage, job ? "job accepted" : "job REFUSED", d);
        if (!job) gFailed++;
        return job;
    }
    bool ok = job && right == count && kept == dst->size() / 4 - count;
    verdict(stage, ok, d);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// Multisampling: the triangle (16,16) (48,16) (32,48) in white on black with four samples a pixel in the tile buffer,
// averaged as the frame is written back (storeAction MultisampleResolve), and the same with one sample. With four,
// the pixels its edges cross are between black and white, in quarters (within 2 of 0, 64, 128, 191, 255 -- 255 / 4
// rounded either way); inside white and outside black exactly. With one, every pixel black or white
bool runMsaa(Device& device) {
    const char* stage = "msaa";
    const float tri[12] = {-0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0, 0.5f, 0, 1};
    DrawInput in = colorDraw(tri, kWhite12, 3);
    in.sampleCount = 4;
    Frame f4 = drawFrame(device, {{&hm_color, in}});
    in.sampleCount = 1;
    Frame f1 = drawFrame(device, {{&hm_color, in}});
    if (!f4.bo || !f1.bo) return verdict(stage, false, f4.why + f1.why);
    const float vx[3] = {16, 48, 32}, vy[3] = {16, 16, 48};
    unsigned inside = 0, insideOk = 0, outside = 0, outsideOk = 0, partial = 0, partialQuarters = 0, partial1 = 0;
    unsigned levels[5] = {};
    for (uint32_t y = 0; f4.pass && f1.pass && y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            const uint8_t* q = f4.px(x, y);
            int side = sideOf(x + 0.5f, y + 0.5f, vx, vy, 1.0f);
            if (side > 0) inside++, insideOk += samePixel(q, kWhitePx);
            if (side < 0) outside++, outsideOk += samePixel(q, kBlackPx);
            if (q[0] != 0 && q[0] != 255) {
                partial++;
                for (int l = 1; l < 4; l++) {
                    if (std::abs(q[0] - 255 * l / 4) <= 2 || std::abs(q[0] - (255 * l + 3) / 4) <= 2) {
                        partialQuarters++, levels[l]++;
                        break;
                    }
                }
            }
            const uint8_t* p1 = f1.px(x, y);
            partial1 += p1[0] != 0 && p1[0] != 255;
        }
    }
    char d[400];
    snprintf(d, sizeof(d), "4x: %s; inside %u of %u white, outside %u of %u black, %u edge pixels between (%u in quarters: "
             "%u at 1/4, %u at 2/4, %u at 3/4); 1x: %s, %u pixels between", jobsText(f4).c_str(), insideOk, inside, outsideOk,
             outside, partial, partialQuarters, levels[1], levels[2], levels[3], jobsText(f1).c_str(), partial1);
    return finish(stage, {&f4, &f1}, insideOk == inside && outsideOk == outside && inside > 300 && partial >= 40 &&
                                         partialQuarters == partial && partial1 == 0, d);
}

// The fixed list of what a Metal backend for this GPU has to do (README "Capabilities"): each one counted only when
// every stage named for it said PASS in this run; those with no stage are not done yet
struct Capability {
    const char* name;
    std::vector<const char*> stages;
};

void capabilities() {
    const Capability list[] = {
        {"the GPU opened and identified", {"open", "params", "device"}},
        {"GPU buffers: create, map, address", {"buffer"}},
        {"job submission and completion", {"write-value"}},
        {"render pass: load Clear, store", {"clear64", "clear64-tiler", "clear64-noclean"}},
        {"targets of any size, to 750x1624", {"clear-48x20", "clear-750x1624", "varyings-160x96", "varyings-750x1624"}},
        {"BGRA8 targets", {"clear64-bgra", "varyings-bgra"}},
        {"vertex and fragment shaders: a draw", {"triangle"}},
        {"vertex attributes and varyings", {"varyings"}},
        {"uniform buffers (setVertexBytes)", {"uniform"}},
        {"blending, source over", {"blend"}},
        {"indexed draws, 16-bit", {"indexed"}},
        {"several draws in a pass", {"multidraw"}},
        {"textures sampled, nearest", {"texture"}},
        {"compute dispatch", {"compute"}},
        {"threadgroup memory and barriers", {"shared"}},
        {"depth test and write", {"depth", "depth-off"}},
        {"the shader compiler on the device", {"compiler", "compiler-draw"}},
        {"command buffers, encoders, statuses", {"metal-api"}},
        {"an IOSurface as a drawable", {"iosurface"}},
        {"Apple metallib compute kernels", {"apple-image_calcminmax0", "apple-MetalFaceMask_InitMinMax", "apple-MetalFaceMask_InitSpanTable"}},
        {"blit: buffer to buffer copy", {"blit"}},
        {"primitive: lines", {"lines"}},
        {"primitive: points", {"points"}},
        {"textures sampled, linear", {"texture-linear"}},
        {"mipmaps", {"mipmaps"}},
        {"more pixel formats: R8, RG8", {"formats"}},
        {"render to a texture, then sample it", {"render-to-texture"}},
        {"multisampling", {"msaa"}},
        {"stencil", {"stencil"}},
        {"scissor and viewport from the API", {"viewport-scissor"}},
        {"culling and winding", {"cull"}},
        {"instanced draws", {"instanced"}},
        {"vertex descriptors and argument buffers", {"vertex-descriptor", "argument-buffer"}},
        {"Apple vertex and fragment functions (AIR)", {"apple-render"}},
        {"blit: textures, generateMipmaps", {"blit-texture", "generate-mipmaps"}},
        {"presenting to the panel (CAMetalLayer)", {}},
        {"Metal.framework's MTLDevice on this backend", {}},
    };
    const unsigned total = sizeof(list) / sizeof(list[0]);
    unsigned measured = 0;
    std::string missing;
    for (unsigned i = 0; i < total; i++) {
        bool ok = !list[i].stages.empty();
        for (const char* st : list[i].stages) {
            bool found = false;
            for (const std::string& p : gPassedStages) found |= p == st;
            ok &= found;
        }
        measured += ok;
        printf("mali-probe: capability %2u %s: %s\n", i + 1, list[i].name, ok ? "measured" : list[i].stages.empty() ? "not done" : "FAILED");
        if (!ok && !list[i].stages.empty()) missing += std::string(missing.empty() ? "" : ", ") + list[i].name;
    }
    say("capabilities %u of %u measured (%u%%)%s%s", measured, total, measured * 100 / total,
        missing.empty() ? "" : "; failed: ", missing.c_str());
}

// Depth: one draw of two triangles, the near one (z -0.5, green) first and the far one (z +0.5, red) after it,
// overlapping; with the depth test where both cover a pixel it must be green, without it red
bool runDepth(Device& device, bool test) {
    const char* stage = test ? "depth" : "depth-off";
    std::string why;
    auto bo = device.gpu().newBo(256 * 64 + 4096, &why);
    if (!bo) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    Target t;
    t.bo = bo.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    // near: pixels (8,8) (56,8) (8,56), so x + y < 64; far: (56,56) (8,40) (40,8), so x + y > 48: both in 48..64
    const float pos[24] = {-0.75f, -0.75f, -0.5f, 1, 0.75f, -0.75f, -0.5f, 1, -0.75f, 0.75f, -0.5f, 1,
                           0.75f, 0.75f, 0.5f, 1, -0.75f, 0.25f, 0.5f, 1, 0.25f, -0.75f, 0.5f, 1};
    const float col[24] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    const float one[4] = {1, 1, 1, 1};
    DrawInput in;
    in.attributes[0] = pos;
    in.attributes[1] = col;
    in.vertexCount = 6;
    in.uniforms = one;
    in.uniformSize = 16;
    in.depthTest = test;
    auto pass = DrawPass::make(device, t, hm_color, in, &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission ds, fs;
    int e = pass->submit(&ds, &fs);
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    // pixels well inside both triangles: x + y in pixel space between the two hypotenuses, away from the edges
    unsigned green = 0, red = 0, judged = 0;
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            // inside both triangles by more than a pixel: near (8,8) (56,8) (8,56), far (56,56) (8,40) (40,8)
            auto inside = [&](const float* tx, const float* ty) {
                for (int k = 0; k < 3; k++) {
                    float ax = tx[k], ay = ty[k], bx = tx[(k + 1) % 3], by = ty[(k + 1) % 3];
                    float cx = tx[(k + 2) % 3], cy = ty[(k + 2) % 3];
                    float ex = bx - ax, ey = by - ay, len = std::sqrt(ex * ex + ey * ey);
                    float side = ((cx - ax) * ey - (cy - ay) * ex) > 0 ? 1.0f : -1.0f;
                    if (side * ((px - ax) * ey - (py - ay) * ex) / len <= 1.0f) return false;
                }
                return true;
            };
            const float nx[3] = {8, 56, 8}, ny[3] = {8, 8, 56}, fx[3] = {56, 8, 40}, fy[3] = {56, 40, 8};
            bool both = inside(nx, ny) && inside(fx, fy);
            if (!both) continue;
            judged++;
            const uint8_t* q = bo->cpu() + y * 256 + 4 * x;
            green += q[0] == 0 && q[1] == 0xff && q[2] == 0;
            red += q[0] == 0xff && q[1] == 0 && q[2] == 0;
        }
    }
    char detail[220];
    snprintf(detail, sizeof(detail), "vertex %s tiler %s fragment %s; where both cover: %u green (near), %u red (far) of %u",
             statusText(pass->vertexStatus()).c_str(), statusText(pass->tilerStatus()).c_str(),
             statusText(pass->fragmentStatus()).c_str(), green, red, judged);
    bool jobs = e == 0 && (pass->fragmentStatus() & 0xff) == 1 && (pass->tilerStatus() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED", detail);
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && judged > 0 && (test ? green == judged : red == judged);
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// The compiler inside the driver: the programs compiled here, in this process, by Mesa's Bifrost compiler, held word for
// word against shaders_v7.h (compiled on the Mac by the same compiler.c), then drawn and dispatched with
bool runCompiler(Device& device) {
#if !defined(HAKIOS_MALI_COMPILER)
    (void)device;
    say("compiler MEASURE not built in (mali27.sh links Mesa's compiler with HAKIOS_MALI_COMPILER)");
    return true;
#else
    char why[200] = "";
    uint64_t t0 = device.gpu().transport().nowUs();
    hm_compile_init();
    struct hm_program color;
    if (hm_compile_program("color", static_cast<unsigned>(device.productId()), &color, why, sizeof(why)) != 0)
        return verdict("compiler", false, why);
    uint64_t compiled = device.gpu().transport().nowUs() - t0;
    auto same = [](const void* a, const void* b, size_t n) { return memcmp(a, b, n) == 0; };
    bool code = color.vs_size == hm_color.vs_size && color.fs_size == hm_color.fs_size &&
                same(color.vs_code, hm_color.vs_code, color.vs_size) && same(color.fs_code, hm_color.fs_code, color.fs_size);
    bool states = same(color.vs_rsd, hm_color.vs_rsd, 64) && same(color.fs_rsd, hm_color.fs_rsd, 64) &&
                  same(color.fs_rsd_depth, hm_color.fs_rsd_depth, 64) && same(color.blend_off, hm_color.blend_off, 16) &&
                  same(color.blend_over, hm_color.blend_over, 16);
    bool tables = color.fs_push_count == hm_color.fs_push_count && color.vs_out_count == hm_color.vs_out_count &&
                  color.fs_in_count == hm_color.fs_in_count &&
                  same(color.fs_push, hm_color.fs_push, 4 * color.fs_push_count) &&
                  same(color.vs_out, hm_color.vs_out, sizeof(hm_varying) * color.vs_out_count) &&
                  same(color.fs_in, hm_color.fs_in, sizeof(hm_varying) * color.fs_in_count);
    struct hm_kernel fill;
    bool kernel = hm_compile_kernel("fill", static_cast<unsigned>(device.productId()), &fill, why, sizeof(why)) == 0 &&
                  fill.size == hm_fill.size && same(fill.code, hm_fill.code, fill.size) && same(fill.rsd, hm_fill.rsd, 64);
    char detail[260];
    snprintf(detail, sizeof(detail), "color compiled on the phone in %llu us: vertex %u bytes, fragment %u bytes; code %s, "
             "states %s, tables %s the Mac's; fill %s", (unsigned long long)compiled, color.vs_size, color.fs_size,
             code ? "=" : "DIFFERENT from", states ? "=" : "DIFFERENT from", tables ? "=" : "DIFFERENT from",
             kernel ? "= the Mac's" : why[0] ? why : "DIFFERENT from the Mac's");
    bool ok = verdict("compiler", code && states && tables && kernel, detail);
    // and drawn with what was compiled here: the varyings stage again, with the phone's own program
    {
        std::string w;
        auto bo = device.gpu().newBo(256 * 64 + 4096, &w);
        Target t;
        t.bo = bo.get();
        t.width = t.height = 64;
        t.rowBytes = 256;
        const float positions[12] = {-0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f};
        const float rgb[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
        const float one[4] = {1, 1, 1, 1};
        DrawInput in;
        in.attributes[0] = positions;
        in.attributes[1] = rgb;
        in.uniforms = one;
        in.uniformSize = 16;
        auto pass = bo ? DrawPass::make(device, t, color, in, &w) : nullptr;
        Submission ds, fs;
        int e = pass ? pass->submit(&ds, &fs) : -1;
        if (e == 0) e = device.gpu().wait(fs, 2000000);
        const uint8_t* mid = bo ? bo->cpu() + 32 * 256 + 4 * 32 : nullptr;
        bool drawn = e == 0 && pass && (pass->fragmentStatus() & 0xff) == 1 && mid[3] == 0xff && (mid[0] | mid[1] | mid[2]);
        char d2[160];
        snprintf(d2, sizeof(d2), "the phone's color program drawn: fragment %s, pixel (32,32) %02x%02x%02x%02x",
                 pass ? statusText(pass->fragmentStatus()).c_str() : w.c_str(), mid ? mid[0] : 0, mid ? mid[1] : 0,
                 mid ? mid[2] : 0, mid ? mid[3] : 0);
        ok = verdict("compiler-draw", drawn, d2) && ok;
    }
    hm_program_free(&color);
    if (fill.code) hm_kernel_free(&fill);
    return ok;
#endif
}

// An indexed draw: a quad of four vertices as two triangles of six 16-bit indices (0 1 2, 2 1 3), every vertex green:
// the quad's inside green, the rest the clear
bool runIndexed(Device& device) {
    const char* stage = "indexed";
    std::string why;
    auto bo = device.gpu().newBo(256 * 64 + 4096, &why);
    if (!bo) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    Target t;
    t.bo = bo.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    // the quad (-0.5..0.5)^2: pixels 16..48
    const float pos[16] = {-0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, -0.5f, 0.5f, 0, 1, 0.5f, 0.5f, 0, 1};
    const float col[16] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    const uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
    const float one[4] = {1, 1, 1, 1};
    DrawInput in;
    in.attributes[0] = pos;
    in.attributes[1] = col;
    in.vertexCount = 4;
    in.indices = idx;
    in.indexCount = 6;
    in.uniforms = one;
    in.uniformSize = 16;
    auto pass = DrawPass::make(device, t, hm_color, in, &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission ds, fs;
    int e = pass->submit(&ds, &fs);
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    unsigned inRight = 0, inTotal = 0, outRight = 0, outTotal = 0;
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            const uint8_t* q = bo->cpu() + y * 256 + 4 * x;
            if (x >= 17 && x <= 46 && y >= 17 && y <= 46) {
                inTotal++;
                inRight += q[0] == 0 && q[1] == 0xff && q[2] == 0 && q[3] == 0xff;
            } else if (x < 15 || x > 48 || y < 15 || y > 48) {
                outTotal++;
                outRight += q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0xff;
            }
        }
    }
    char detail[220];
    snprintf(detail, sizeof(detail), "vertex %s tiler %s fragment %s; inside %u of %u green, outside %u of %u the clear",
             statusText(pass->vertexStatus()).c_str(), statusText(pass->tilerStatus()).c_str(),
             statusText(pass->fragmentStatus()).c_str(), inRight, inTotal, outRight, outTotal);
    bool jobs = e == 0 && (pass->fragmentStatus() & 0xff) == 1 && (pass->tilerStatus() & 0xff) == 1 &&
                (pass->vertexStatus() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED", detail);
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && inRight == inTotal && outRight == outTotal;
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// Several draws in one pass (DrawPass::makeMany): a green quad on the left, a blue triangle on the right, and, over the middle, a near yellow and a far red quad in two more draws with the depth test, the far one
// last: the left green, the right blue, the middle yellow -- the tile buffer's depth kept across draws of one frame
bool runMultiDraw(Device& device) {
    const char* stage = "multidraw";
    std::string why;
    auto bo = device.gpu().newBo(256 * 64 + 4096, &why);
    if (!bo) return verdict(stage, false, why);
    memset(bo->cpu(), kCanary, bo->size());
    Target t;
    t.bo = bo.get();
    t.width = t.height = 64;
    t.rowBytes = 256;
    const float one[4] = {1, 1, 1, 1};
    // left quad: x -1..-0.5 (pixels 0..16), right triangle x 0.5..1, middle quads x -0.25..0.25 (pixels 24..40)
    const float leftPos[24] = {-1, -1, 0, 1, -0.5f, -1, 0, 1, -1, 1, 0, 1, -1, 1, 0, 1, -0.5f, -1, 0, 1, -0.5f, 1, 0, 1};
    const float green[24] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    const float rightPos[12] = {0.5f, -1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1};
    const float blue[12] = {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1};
    const float nearPos[24] = {-0.25f, -1, -0.5f, 1, 0.25f, -1, -0.5f, 1, -0.25f, 1, -0.5f, 1,
                               -0.25f, 1, -0.5f, 1, 0.25f, -1, -0.5f, 1, 0.25f, 1, -0.5f, 1};
    const float yellow[24] = {1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1};
    float farPos[24];
    for (int i = 0; i < 24; i++) farPos[i] = (i % 4 == 2) ? 0.5f : nearPos[i];
    const float red[24] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    auto draw = [&](const float* pos, const float* col, uint32_t n) {
        DrawInput in;
        in.attributes[0] = pos;
        in.attributes[1] = col;
        in.vertexCount = n;
        in.uniforms = one;
        in.uniformSize = 16;
        in.depthTest = true;
        return std::make_pair(&hm_color, in);
    };
    auto pass = DrawPass::makeMany(device, t,
        {draw(leftPos, green, 6), draw(rightPos, blue, 3), draw(nearPos, yellow, 6), draw(farPos, red, 6)}, &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission ds, fs;
    int e = pass->submit(&ds, &fs);
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    auto at = [&](int x, int y) { return bo->cpu() + y * 256 + 4 * x; };
    auto is = [](const uint8_t* q, uint8_t r, uint8_t g, uint8_t b) { return q[0] == r && q[1] == g && q[2] == b && q[3] == 0xff; };
    unsigned left = 0, right = 0, mid = 0, n = 0;
    for (int y = 4; y < 60; y++) {
        left += is(at(6, y), 0, 0xff, 0);
        // the right triangle (0.5,-1) (1,-1) (1,1) covers column 61 down to row 53 (y = -1 + 4 (x - 0.5))
        right += y <= 50 ? is(at(61, y), 0, 0, 0xff) : 1;
        mid += is(at(32, y), 0xff, 0xff, 0);
        n++;
    }
    char detail[220];
    snprintf(detail, sizeof(detail), "4 draws, fragment %s; column 6 green %u of %u, column 61 blue %u of %u, column 32 "
             "yellow (near beat far) %u of %u", statusText(pass->fragmentStatus()).c_str(), left, n, right, n, mid, n);
    bool jobs = e == 0 && (pass->fragmentStatus() & 0xff) == 1;
    if (gFake) {
        say("%s MEASURE (the fake GPU does not rasterize: %s) %s", stage, jobs ? "jobs accepted" : "jobs REFUSED", detail);
        if (!jobs) gFailed++;
        return jobs;
    }
    bool ok = jobs && left == n && right == n && mid == n;
    verdict(stage, ok, detail);
    if (!ok || gDump) pass->dump(stdout);
    return ok;
}

// Metal's command model (metal.hpp), nothing below it used directly: one command buffer with a render pass (a cleared
// BGRA8 texture, a textured quad sampling a 2x2 texture and a source-over half-transparent triangle over it), a
// second pass with no draw (its clear alone), and a compute dispatch, committed and waited for; the status Completed
// and the results right
bool runMetal(Device& device) {
    using namespace hakios::mali::metal;
    const char* stage = "metal-api";
    std::string why;
    auto target = Texture::make(device, 64, 64, Format::BGRA8Unorm, &why);
    auto image = Texture::make(device, 2, 2, Format::RGBA8Unorm, &why);
    auto empty = Texture::make(device, 16, 16, Format::RGBA8Unorm, &why);
    auto out = device.gpu().newBo(4096, &why);
    if (!target || !image || !empty || !out) return verdict(stage, false, why);
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    image->replace(texels, 8);
    memset(out->cpu(), kCanary, out->size());

    CommandBuffer cb(device);
    const float black[4] = {0, 0, 0, 1}, gray[4] = {0.5f, 0.5f, 0.5f, 1};
    RenderEncoder* enc = cb.renderCommandEncoder(*target, black);
    RenderPipeline textured{&hm_tex, false, false}, over{&hm_color, true, false};
    const float quad[24] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1};
    const float uv[24] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0};
    enc->setPipeline(textured);
    enc->setVertexBytes(0, quad, 24);
    enc->setVertexBytes(1, uv, 24);
    enc->setFragmentTexture(image.get(), true);
    enc->drawTriangles(6);
    const float tri[12] = {-0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, 0, 0.5f, 0, 1};
    const float white[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    const float half[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    enc->setPipeline(over);
    enc->setVertexBytes(0, tri, 12);
    enc->setVertexBytes(1, white, 12);
    enc->setUniformBytes(half, 16);
    enc->drawTriangles(3);
    enc->endEncoding();
    cb.renderCommandEncoder(*empty, gray)->endEncoding();
    uint32_t uniforms[3] = {static_cast<uint32_t>(out->gpu()), static_cast<uint32_t>(out->gpu() >> 32), 7};
    const unsigned groups[3] = {2, 1, 1};
    cb.dispatch(ComputePipeline{&hm_fill}, groups, uniforms, sizeof(uniforms), {out.get()});
    cb.commit();
    cb.waitUntilCompleted();

    // the quad's four quarters the four texels (B G R A in memory), the triangle's middle the texel blended half white
    std::vector<uint8_t> px(64 * 64 * 4), e(16 * 16 * 4);
    target->read(px.data(), 256);
    empty->read(e.data(), 64);
    auto at = [&](int x, int y) { return &px[(y * 64 + x) * 4]; };
    bool corners = at(4, 4)[2] == 255 && at(4, 4)[1] == 0 && at(60, 4)[1] == 255 && at(60, 4)[2] == 0 &&
                   at(4, 60)[0] == 255 && at(60, 60)[0] == 255 && at(60, 60)[1] == 255;
    const uint8_t* m = at(26, 22);   // inside the triangle, in the red texel's quarter: (255,0,0)*0.5 + 0.5 white
    bool blended = m[2] >= 254 && m[1] >= 126 && m[1] <= 129 && m[0] >= 126 && m[0] <= 129;
    unsigned grayOk = 0;
    for (int i = 0; i < 256; i++) grayOk += e[4 * i] == 128 && e[4 * i + 3] == 255;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(out->cpu());
    unsigned words = 0;
    for (unsigned i = 0; i < 32; i++) words += w[i] == i * 3 + 7;
    const char* st = cb.status() == CommandBuffer::Status::Completed ? "Completed" :
                     cb.status() == CommandBuffer::Status::Error ? "Error" : "not done";
    char detail[300];
    snprintf(detail, sizeof(detail), "command buffer %s%s%s in %llu us; texels in the quarters %s, the triangle blended "
             "over them %02x%02x%02x, the empty pass's clear %u of 256, the dispatch %u of 32 words", st,
             cb.error().empty() ? "" : ": ", cb.error().c_str(), (unsigned long long)cb.gpuMicroseconds(),
             corners ? "right" : "WRONG", m[2], m[1], m[0], grayOk, words);
    if (gFake) {
        bool ok = cb.status() == CommandBuffer::Status::Completed && grayOk == 256;
        say("%s MEASURE (the fake GPU: the clear carried out, draws and kernels not run: %s) %s", stage,
            ok ? "accepted" : "REFUSED", detail);
        if (!ok) gFailed++;
        return ok;
    }
    return verdict(stage, cb.status() == CommandBuffer::Status::Completed && corners && blended && grayOk == 256 &&
                   words == 32, detail);
}

// A draw into an IOSurface, the shape of a CAMetalLayer drawable: a 64x64 BGRA surface made here (IOSurfaceCreate), its
// memory a buffer of the GPU through HakiosMali (request 0x7e), the varyings triangle drawn into it, then read back
// through IOSurface's own lock and base address
bool runSurface(Device& device) {
    const char* stage = "iosurface";
#if !defined(__APPLE__)
    (void)device;
    say("%s MEASURE no IOSurface here", stage);
    return true;
#else
    if (gFake) {
        say("%s MEASURE the fake GPU has no IOSurfaces", stage);
        return true;
    }
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    auto num = [](int v) { return CFNumberCreate(nullptr, kCFNumberIntType, &v); };
    CFNumberRef n;
    n = num(64); CFDictionarySetValue(props, kIOSurfaceWidth, n); CFRelease(n);
    n = num(64); CFDictionarySetValue(props, kIOSurfaceHeight, n); CFRelease(n);
    n = num(4); CFDictionarySetValue(props, kIOSurfaceBytesPerElement, n); CFRelease(n);
    n = num(256); CFDictionarySetValue(props, kIOSurfaceBytesPerRow, n); CFRelease(n);
    n = num(0x42475241); CFDictionarySetValue(props, kIOSurfacePixelFormat, n); CFRelease(n);   // 'BGRA'
    IOSurfaceRef surface = IOSurfaceCreate(props);
    CFRelease(props);
    if (surface == nullptr) return verdict(stage, false, "IOSurfaceCreate gave none");
    std::string why;
    HakiosSurface info = {};
    auto bo = device.gpu().surfaceBo(IOSurfaceGetID(surface), &info, &why);
    if (!bo) {
        CFRelease(surface);
        return verdict(stage, false, why);
    }
    Target t;
    t.bo = bo.get();
    t.offset = info.offset;
    t.width = info.width;
    t.height = info.height;
    t.rowBytes = info.rowBytes;
    t.format = Format::BGRA8Unorm;
    const float positions[12] = {-0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f};
    const float rgb[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    const float one[4] = {1, 1, 1, 1};
    DrawInput in;
    in.attributes[0] = positions;
    in.attributes[1] = rgb;
    in.uniforms = one;
    in.uniformSize = 16;
    in.clear[0] = 0.0f, in.clear[1] = 0.0f, in.clear[2] = 0.5f, in.clear[3] = 1.0f;
    auto pass = DrawPass::make(device, t, hm_color, in, &why);
    if (!pass) {
        CFRelease(surface);
        return verdict(stage, false, "refused: " + why);
    }
    Submission ds, fs;
    int e = pass->submit(&ds, &fs);
    if (e == 0) e = device.gpu().wait(fs, 2000000);
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
    const uint8_t* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
    size_t row = IOSurfaceGetBytesPerRow(surface);
    const uint8_t* corner = base + 2 * row + 4 * 2;
    const uint8_t* mid = base + 32 * row + 4 * 32;
    bool clearOk = corner[0] == 128 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255;   // B G R A
    bool drawn = mid[3] == 255 && mid[0] > 40 && mid[1] > 40 && mid[2] > 40 && mid[0] < 150;
    char detail[240];
    snprintf(detail, sizeof(detail), "surface %u %ux%u row %u offset %u; fragment %s; corner %02x%02x%02x%02x (the clear), "
             "middle %02x%02x%02x%02x (the three colours mixed), through IOSurface's own mapping",
             IOSurfaceGetID(surface), info.width, info.height, info.rowBytes, info.offset,
             statusText(pass->fragmentStatus()).c_str(), corner[2], corner[1], corner[0], corner[3], mid[2], mid[1], mid[0],
             mid[3]);
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    pass.reset();
    bo.reset();
    CFRelease(surface);
    return verdict(stage, e == 0 && clearOk && drawn, detail);
#endif
}

// Apple's own kernels, compiled on the phone from a .metallib of the 27 release (PortraitFilters', in the small image as
// /hakios/share/PortraitFilters.metallib) through the AIR frontend, the device buffers made global memory and Mesa's
// Bifrost compiler, dispatched by ComputePass: one thread each, and what each must write, read off its NIR
// (tools/airkernels with HM_PRINT_NIR): image_calcminmax0 words 0 and 1 = 0x7fffffff 0x80000000;
// MetalFaceMask_InitMinMax words 0..2 = 0x7fffffff and 3..5 = 0x80000000; MetalFaceMask_InitSpanTable words 2 and 3 = 0
bool runAppleKernels(Device& device) {
#if !defined(HAKIOS_MALI_COMPILER)
    (void)device;
    say("apple-kernels MEASURE not built in (needs the compiler)");
    return true;
#else
    const char* path = "/hakios/share/PortraitFilters.metallib";
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return verdict("apple-kernels", false, std::string("no ") + path);
    std::vector<uint8_t> lib;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) lib.insert(lib.end(), chunk, chunk + n);
    fclose(f);
    static char names[16384];
    int count = hm_air_kernels(lib.data(), lib.size(), names, sizeof(names));
    struct Case {
        const char* name;
        std::vector<std::pair<unsigned, uint32_t>> want;    // word, value
    } cases[] = {
        {"image_calcminmax0", {{0, 0x7fffffffu}, {1, 0x80000000u}}},
        {"MetalFaceMask_InitMinMax", {{0, 0x7fffffffu}, {1, 0x7fffffffu}, {2, 0x7fffffffu}, {3, 0x80000000u},
                                      {4, 0x80000000u}, {5, 0x80000000u}}},
        {"MetalFaceMask_InitSpanTable", {{2, 0}, {3, 0}}},
    };
    bool all = true;
    unsigned compiled = 0;
    for (const char* name = names; count > 0 && *name; name += strlen(name) + 1) {
        hm_kernel k;
        uint32_t dev = 0, con = 0;
        char why[300];
        if (hm_compile_air_kernel(lib.data(), lib.size(), name, static_cast<unsigned>(device.productId()), &k, &dev, &con,
                                  why, sizeof(why)) == 0) {
            compiled++;
            hm_kernel_free(&k);
        }
    }
    say("apple-kernels MEASURE %u of %d compute functions of PortraitFilters compiled on the phone", compiled, count);
    for (const Case& c : cases) {
        std::string stage = std::string("apple-") + c.name;
        hm_kernel k;
        uint32_t dev = 0, con = 0;
        char why[300];
        uint64_t t0 = device.gpu().transport().nowUs();
        if (hm_compile_air_kernel(lib.data(), lib.size(), c.name, static_cast<unsigned>(device.productId()), &k, &dev,
                                  &con, why, sizeof(why)) != 0) {
            all = verdict(stage.c_str(), false, why) && all;
            continue;
        }
        uint64_t compileUs = device.gpu().transport().nowUs() - t0;
        std::string w;
        auto bo = device.gpu().newBo(4096, &w);
        memset(bo->cpu(), kCanary, bo->size());
        // the system values: buffer 0's address at byte 0, one threadgroup of one thread
        std::vector<uint8_t> sys(HM_AIR_SYSVAL_SIZE, 0);
        uint64_t addr = bo->gpu();
        memcpy(&sys[0], &addr, 8);
        uint32_t one[3] = {1, 1, 1};
        memcpy(&sys[HM_AIR_SYSVAL_GRID], one, 12);
        memcpy(&sys[HM_AIR_SYSVAL_LOCAL], one, 12);
        const unsigned groups[3] = {1, 1, 1}, local[3] = {1, 1, 1};
        auto pass = ComputePass::makeWith(device, k, groups, local, {{HM_AIR_SYSVAL_UBO, sys}}, &w);
        if (!pass) {
            all = verdict(stage.c_str(), false, "refused: " + w) && all;
            hm_kernel_free(&k);
            continue;
        }
        Submission s;
        int e = pass->submit({bo.get()}, &s);
        if (e == 0) e = device.gpu().wait(s, 2000000);
        const uint32_t* words = reinterpret_cast<const uint32_t*>(bo->cpu());
        unsigned right = 0;
        std::string got;
        for (const auto& wv : c.want) {
            right += words[wv.first] == wv.second;
            char b[24];
            snprintf(b, sizeof(b), " %#x", words[wv.first]);
            got += b;
        }
        char detail[300];
        snprintf(detail, sizeof(detail), "compiled on the phone in %llu us (%u bytes, device buffers %#x); job %s; %u of %zu "
                 "words as the NIR says:%s", (unsigned long long)compileUs, k.size, dev, statusText(pass->status()).c_str(),
                 right, c.want.size(), got.c_str());
        all = verdict(stage.c_str(), e == 0 && (pass->status() & 0xff) == 1 && right == c.want.size(), detail) && all;
        hm_kernel_free(&k);
    }
    return all;
#endif
}

// Threadgroup memory and a barrier: the share kernel, 4 threadgroups of 64; out[64 wg + lid] must be what thread 63 - lid
// of the same group wrote before the barrier, (63 - lid) * 2 + 1, plus 1000 wg
bool runShared(Device& device) {
    const char* stage = "shared";
    std::string why;
    auto out = device.gpu().newBo(4096, &why);
    if (!out) return verdict(stage, false, why);
    memset(out->cpu(), kCanary, out->size());
    uint32_t uniforms[2] = {static_cast<uint32_t>(out->gpu()), static_cast<uint32_t>(out->gpu() >> 32)};
    const unsigned groups[3] = {4, 1, 1};
    auto pass = ComputePass::make(device, hm_share, groups, uniforms, sizeof(uniforms), &why);
    if (!pass) return verdict(stage, false, "refused: " + why);
    Submission s;
    int e = pass->submit({out.get()}, &s);
    if (e == 0) e = device.gpu().wait(s, 2000000);
    const uint32_t* w = reinterpret_cast<const uint32_t*>(out->cpu());
    unsigned right = 0;
    for (unsigned g = 0; g < 4; g++)
        for (unsigned l = 0; l < 64; l++) right += w[64 * g + l] == (63 - l) * 2 + 1 + 1000 * g;
    char detail[200];
    snprintf(detail, sizeof(detail), "job %s; %u of 256 words read across the barrier right (word 0 %u, word 255 %u), "
             "%u bytes of threadgroup memory", statusText(pass->status()).c_str(), right, w[0], w[255], hm_share.wls_size);
    if (gFake) {
        bool job = e == 0 && (pass->status() & 0xff) == 1;
        say("%s MEASURE (the fake GPU runs no shader: %s) %s", stage, job ? "job accepted" : "job REFUSED", detail);
        if (!job) gFailed++;
        return job;
    }
    return verdict(stage, e == 0 && (pass->status() & 0xff) == 1 && right == 256, detail);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);       // every line out as it is printed: a crash must not take them along
    if (argc == 3 && strcmp(argv[1], "--until") == 0) {
        struct timeval boot = {}, now = {};
        size_t n = sizeof(boot);
        int mib[2] = {CTL_KERN, KERN_BOOTTIME};
        if (sysctl(mib, 2, &boot, &n, nullptr, 0) != 0) return 1;
        gettimeofday(&now, nullptr);
        long up = now.tv_sec - boot.tv_sec, until = atol(argv[2]);
        printf("mali-probe: up %ld s, waiting until %ld s\n", up, until);
        if (until > up) sleep(static_cast<unsigned>(until - up));
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fake") == 0) gFake = true;
        else if (strcmp(argv[i], "--dump") == 0) gDump = true;
        else if (strcmp(argv[i], "--show") == 0 && i + 1 < argc) gShow = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: mali-probe [--fake] [--dump] [--show SECONDS] | mali-probe --until SECONDS\n");
            return 2;
        }
    }
    say("started (%s)", gFake ? "the fake GPU" : "HakiosMali");

    // 1. the service
    std::string why;
    std::unique_ptr<Transport> transport;
    if (gFake) {
        transport.reset(new FakeGpu());
    } else {
        transport = openKernelMali(60, &why);
    }
    if (!verdict("open", transport != nullptr, transport ? "" : why)) {
        say("done: %u passed, %u failed; milestone clear64 FAIL", gPassed, gFailed);
        return 1;
    }
    FakeGpu* fake = gFake ? static_cast<FakeGpu*>(transport.get()) : nullptr;
    Gpu gpu(std::move(transport));

    // 2. what the GPU is
    {
        uint64_t v[5] = {};
        uint32_t which[5] = {kParamGpuProdId, kParamGpuRevision, kParamShaderPresent, kParamTilerFeatures,
                             kParamThreadTlsAlloc};
        std::string detail;
        bool ok = true;
        for (int i = 0; i < 5; i++) {
            int e = gpu.param(which[i], &v[i]);
            ok = ok && e == 0;
        }
        const Model* model = modelOf(v[0]);
        char b[200];
        snprintf(b, sizeof(b), "product %#llx (%s) revision %#llx shaders %#llx tiler features %#llx (levels %llu) "
                 "TLS %#llx", (unsigned long long)v[0], model ? model->name : "not a v7 GPU", (unsigned long long)v[1],
                 (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)((v[3] >> 8) & 0xf),
                 (unsigned long long)v[4]);
        verdict("params", ok && model != nullptr, b);
    }
    Device device(gpu);
    if (!verdict("device", device.init(&why) == 0, why)) {
        say("done: %u passed, %u failed; milestone clear64 FAIL", gPassed, gFailed);
        return 1;
    }

    // 3. a buffer: made, at a GPU address, mapped, written and read back
    if (gShow == 0) {
        auto bo = gpu.newBo(65536, &why);
        bool ok = bo != nullptr;
        uint64_t wrong = 0;
        if (ok) {
            uint32_t* w = reinterpret_cast<uint32_t*>(bo->cpu());
            for (uint32_t i = 0; i < bo->size() / 4; i++) w[i] = i * 2654435761u;
            for (uint32_t i = 0; i < bo->size() / 4; i++) wrong += w[i] != i * 2654435761u;
            ok = wrong == 0 && bo->gpu() != 0 && bo->gpu() < (1ull << 32);
            why = "handle " + std::to_string(bo->handle()) + " at GPU " + hex(bo->gpu()) + ", " +
                  std::to_string(bo->size()) + " bytes, " + std::to_string(wrong) + " words read back wrong";
        }
        verdict("buffer", ok, why);
    }

    // 4. the smallest job: Write Value
    if (gShow == 0) {
        auto target = gpu.newBo(4096, &why);
        std::unique_ptr<WriteValue> job;
        if (target) {
            memset(target->cpu(), kCanary, 64);
            job = WriteValue::make(device, *target, 16, 0x6d616c69u, &why);
        }
        if (!job) {
            verdict("write-value", false, why);
        } else {
            Submission s;
            int e = job->submit(&s);
            if (e == 0) e = gpu.wait(s, 2000000);
            const uint32_t* w = reinterpret_cast<const uint32_t*>(target->cpu());
            bool neighbours = w[3] == 0xa5a5a5a5u && w[5] == 0xa5a5a5a5u;
            char b[160];
            snprintf(b, sizeof(b), "job %s, word %#x (want 0x6d616c69), neighbours %s%s%s", statusText(job->status()).c_str(),
                     w[4], neighbours ? "kept" : "CHANGED", e ? ", " : "", e ? strerror(e) : "");
            bool ok = e == 0 && (job->status() & 0xff) == 1 && w[4] == 0x6d616c69u && neighbours;
            verdict("write-value", ok, b);
            if (!ok || gDump) job->dump(stdout);
        }
    }

    // 5. THE MILESTONE: one Fragment job clears a 64x64 RGBA8 target to a known colour, read back right
    ClearCase clear64 = {"clear64", 64, 64, 256, 0, Format::RGBA8Unorm, {0.2f, 0.4f, 0.6f, 0.8f}, {}, true};
    bool milestone = runClear(device, clear64);
    if (gShow == 0) {

    // 6.. what narrows it down or goes further, each on its own
    ClearCase tiler = {"clear64-tiler", 64, 64, 256, 0, Format::RGBA8Unorm, {1.0f, 0.0f, 0.5f, 1.0f}, {}, true};
    tiler.options.withTiler = true;
    runClear(device, tiler);

    ClearCase odd = {"clear-48x20", 48, 20, 256, 256, Format::RGBA8Unorm, {0.0f, 1.0f, 0.0f, 1.0f}, {}, true};
    runClear(device, odd);

    ClearCase bgra = {"clear64-bgra", 64, 64, 256, 0, Format::BGRA8Unorm, {0.2f, 0.4f, 0.6f, 0.8f}, {}, true};
    runClear(device, bgra);

    // a job over tiles (1,1)-(2,2) alone: the whole frame is written all the same (runs 501, 502)
    ClearCase tiles = {"clear64-tiles", 64, 64, 256, 0, Format::RGBA8Unorm, {0.0f, 0.0f, 1.0f, 1.0f}, {}, true};
    tiles.options.minTx = tiles.options.minTy = 1;
    tiles.options.maxTx = tiles.options.maxTy = 2;
    runClear(device, tiles);

    // run 501: the job's tiles alone did not keep the GPU inside (1,1)-(2,2) -- it wrote the whole 64x64. Which bounds
    // does it keep? The framebuffer's alone, then both
    ClearCase fbBounds = {"clear64-fbbounds", 64, 64, 256, 0, Format::RGBA8Unorm, {1.0f, 0.0f, 1.0f, 1.0f}, {}, true};
    fbBounds.options.boundMinX = fbBounds.options.boundMinY = 16;
    fbBounds.options.boundMaxX = fbBounds.options.boundMaxY = 47;
    runClear(device, fbBounds);
    ClearCase both = {"clear64-both", 64, 64, 256, 0, Format::RGBA8Unorm, {0.0f, 1.0f, 1.0f, 1.0f}, {}, true};
    both.options.minTx = both.options.minTy = 1;
    both.options.maxTx = both.options.maxTy = 2;
    both.options.boundMinX = both.options.boundMinY = 16;
    both.options.boundMaxX = both.options.boundMaxY = 47;
    runClear(device, both);
    // and a frame bounded off the tile grid: pixels (5,7)-(40,50)
    ClearCase odd2 = {"clear64-fbbounds-odd", 64, 64, 256, 0, Format::RGBA8Unorm, {1.0f, 1.0f, 0.0f, 1.0f}, {}, true};
    odd2.options.boundMinX = 5, odd2.options.boundMinY = 7, odd2.options.boundMaxX = 40, odd2.options.boundMaxY = 50;
    runClear(device, odd2);

    ClearCase big = {"clear-750x1624", 750, 1624, 3008, 0, Format::BGRA8Unorm, {0.1f, 0.2f, 0.3f, 1.0f}, {}, true};
    runClear(device, big);

    // the fake's model of a frame without primitives whose target does not write clean tiles: nothing is written
    ClearCase noclean = {"clear64-noclean", 64, 64, 256, 0, Format::RGBA8Unorm, {1.0f, 1.0f, 1.0f, 1.0f}, {}, false};
    noclean.options.cleanPixelWrite = false;
    runClear(device, noclean);

    }   // (not with --show)

    // the first triangle: shaders compiled by Mesa's Bifrost compiler (shaders_v7.h), a vertex job, a tiler job and the
    // Fragment job, all written here. NDC (-0.5,-0.5) (0.5,-0.5) (0,0.5) land on pixels (16,16) (48,16) (32,48);
    // a pixel whose centre is more than a pixel inside must be red, more than a pixel outside the clear colour
    // (black), and the edges are not judged. The fake GPU checks the jobs and does not rasterize.
    {
        auto bo = gpu.newBo(256 * 64 + 4096, &why);
        Target t;
        t.bo = bo.get();
        t.width = t.height = 64;
        t.rowBytes = 256;
        const float positions[12] = {-0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f};
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        std::unique_ptr<DrawPass> tri;
        if (bo) {
            memset(bo->cpu(), kCanary, bo->size());
            DrawInput din;
            din.attributes[0] = positions;
            memcpy(din.clear, black, sizeof(din.clear));
            tri = DrawPass::make(device, t, hm_tri, din, &why);
        }
        if (!tri) {
            verdict("triangle", false, "refused: " + why);
        } else {
            Submission ds, fs;
            uint64_t t0 = gpu.transport().nowUs();
            int e = tri->submit(&ds, &fs);
            if (e == 0) e = gpu.wait(fs, 2000000);
            uint64_t us = gpu.transport().nowUs() - t0;
            const float vx[3] = {16, 48, 32}, vy[3] = {16, 16, 48};
            unsigned inRight = 0, inTotal = 0, outRight = 0, outTotal = 0, red = 0;
            uint32_t bx0 = ~0u, by0 = ~0u, bx1 = 0, by1 = 0, row20 = 0, row44 = 0;
            const uint8_t redPx[4] = {0xff, 0, 0, 0xff}, blackPx[4] = {0, 0, 0, 0xff};
            for (uint32_t y = 0; y < 64; y++) {
                for (uint32_t x = 0; x < 64; x++) {
                    float px = x + 0.5f, py = y + 0.5f, d = 1e9f;
                    bool inside = true;
                    for (int k = 0; k < 3; k++) {
                        float ax = vx[k], ay = vy[k], bx = vx[(k + 1) % 3], by = vy[(k + 1) % 3];
                        float ex = bx - ax, ey = by - ay, len = std::sqrt(ex * ex + ey * ey);
                        // the signed distance to the edge, positive inside (the vertices are counter-clockwise
                        // with y down... either winding: compared against the third vertex's side)
                        float cx = vx[(k + 2) % 3], cy = vy[(k + 2) % 3];
                        float side = ((cx - ax) * ey - (cy - ay) * ex) > 0 ? 1.0f : -1.0f;
                        float dist = side * ((px - ax) * ey - (py - ay) * ex) / len;
                        inside = inside && dist > 0;
                        d = std::fabs(dist) < d ? std::fabs(dist) : d;
                    }
                    const uint8_t* p = bo->cpu() + y * 256 + 4 * x;
                    bool isRed = memcmp(p, redPx, 4) == 0;
                    if (isRed) {
                        red++;
                        bx0 = x < bx0 ? x : bx0, by0 = y < by0 ? y : by0, bx1 = x > bx1 ? x : bx1, by1 = y > by1 ? y : by1;
                        row20 += y == 20, row44 += y == 44;
                    }
                    if (d <= 1.0f) continue;
                    if (inside) {
                        inTotal++;
                        inRight += isRed;
                    } else {
                        outTotal++;
                        outRight += memcmp(p, blackPx, 4) == 0;
                    }
                }
            }
            unsigned canaryKept = 0;
            for (uint64_t i = 256 * 64; i < bo->size(); i++) canaryKept += bo->cpu()[i] == kCanary;
            const float* w = tri->positionsWritten();
            char b[400];
            snprintf(b, sizeof(b), "vertex %s tiler %s fragment %s%s%s; inside %u of %u red, outside %u of %u black; "
                     "red %u pixels in (%u,%u)-(%u,%u), rows 20/44 %u/%u wide; vertex 0 written at (%g, %g, %g, %g); "
                     "canary %u of %llu kept; %llu us", statusText(tri->vertexStatus()).c_str(),
                     statusText(tri->tilerStatus()).c_str(), statusText(tri->fragmentStatus()).c_str(), e ? ", " : "",
                     e ? strerror(e) : "", inRight, inTotal, outRight, outTotal, red, red ? bx0 : 0, red ? by0 : 0, bx1,
                     by1, row20, row44, w[0], w[1], w[2], w[3], canaryKept,
                     (unsigned long long)(bo->size() - 256 * 64), (unsigned long long)us);
            // the target itself, for the host (tools/probe-picture.py LOG OUT.png triangle): RGB, base64, 96 bytes a line
            {
                static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::vector<uint8_t> rgb;
                for (uint32_t i = 0; i < 64 * 64; i++) {
                    rgb.insert(rgb.end(), bo->cpu() + 4 * i, bo->cpu() + 4 * i + 3);
                }
                const unsigned per = 72, lines = (rgb.size() + per - 1) / per;
                for (unsigned l = 0; l < lines; l++) {
                    std::string text;
                    for (size_t i = l * per; i < rgb.size() && i < (l + 1) * per; i += 3) {
                        uint32_t v = rgb[i] << 16 | rgb[i + 1] << 8 | rgb[i + 2];
                        for (int k = 18; k >= 0; k -= 6) text += b64[(v >> k) & 63];
                    }
                    printf("mali-picture triangle 64x64 %u/%u: %s\n", l + 1, lines, text.c_str());
                }
            }
            // --show: the target sixteen times as big on the panel (B G R A there), on dark grey, with the clear64
            // colour as a band under it, for the window server's frames to be drawn over and put back
            if (gShow > 0 && !gFake) {
                const uint32_t W = 1080, H = 1200, S = 16, X0 = (W - 64 * S) / 2, Y0 = 40;
                std::vector<uint8_t> pic(static_cast<size_t>(W) * H * 4, 0x20);
                for (uint32_t y = 0; y < H; y++) {
                    for (uint32_t x = 0; x < W; x++) {
                        uint8_t* q = &pic[(static_cast<size_t>(y) * W + x) * 4];
                        q[3] = 0xff;
                        if (x >= X0 && x < X0 + 64 * S && y >= Y0 && y < Y0 + 64 * S) {
                            const uint8_t* p = bo->cpu() + ((y - Y0) / S) * 256 + 4 * ((x - X0) / S);
                            q[0] = p[2], q[1] = p[1], q[2] = p[0];
                        } else if (y >= Y0 + 64 * S + 40 && y < Y0 + 64 * S + 120 && x >= X0 && x < X0 + 64 * S) {
                            q[0] = 0x99, q[1] = 0x66, q[2] = 0x33;      // clear64's colour, 33 66 99, as B G R
                        }
                    }
                }
                int shown = 0, first = gpu.show(pic.data(), W, H, W * 4);
                say("show: the triangle sixteen times as big on the panel for %d s (%s)", gShow,
                    first == 0 ? "shown" : strerror(first));
                for (int i = 1; first == 0 && i < gShow * 5; i++) {
                    usleep(200000);
                    shown += gpu.show(pic.data(), W, H, W * 4) == 0;
                }
                (void)shown;
            }
            bool jobs = e == 0 && (tri->vertexStatus() & 0xff) == 1 && (tri->tilerStatus() & 0xff) == 1 &&
                        (tri->fragmentStatus() & 0xff) == 1;
            if (fake != nullptr) {
                say("triangle MEASURE (the fake GPU checks the jobs, does not rasterize: %s) %s",
                    jobs ? "jobs accepted" : "jobs REFUSED", b);
                if (!jobs) gFailed++;
            } else {
                bool ok = jobs && inRight == inTotal && outRight == outTotal && canaryKept == bo->size() - 256 * 64;
                verdict("triangle", ok, b);
                if (!ok || gDump) tri->dump(stdout);
            }
        }
    }

    // the color program (draw.hpp, shaders_v7.h): a varying, a pushed uniform, blending
    if (gShow == 0) {
        const float rgb[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
        const float white[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        const float one[4] = {1, 1, 1, 1}, tint[4] = {0.5f, 1.0f, 0.25f, 1.0f}, half[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        const float black[4] = {0, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
        runColorDraw(device, "varyings", rgb, one, black, false);
        runColorDraw(device, "uniform", rgb, tint, black, false);
        runColorDraw(device, "blend", white, half, blue, true);
        runColorDraw(device, "varyings-160x96", rgb, one, black, false, 160, 96);
        runColorDraw(device, "varyings-750x1624", rgb, one, black, false, 750, 1624);
        runColorDraw(device, "varyings-bgra", rgb, one, black, false, 64, 64, true);
        runIndexed(device);
        runMultiDraw(device);
        runMetal(device);
        runSurface(device);
        runAppleKernels(device);
        runTexture(device);
        runCompute(device);
        runShared(device);
        runBlit(device);
        runPrimitive(device, true);
        runPrimitive(device, false);
        runTextureLinear(device);
        runRenderToTexture(device);
        runViewportScissor(device);
        runCull(device);
        runStencil(device);
        runInstanced(device);
        runFormats(device);
        runMipmaps(device);
        runBlitTexture(device);
        runGenerateMipmaps(device);
        runAppleRender(device);
        runVertexDescriptor(device);
        runArgumentBuffer(device);
        runMsaa(device);
        runDepth(device, false);
        runDepth(device, true);
        runCompiler(device);
    }

    // the time of a submission and its wait, twenty clears of 64x64
    {
        auto bo = gpu.newBo(65536, &why);
        Target t;
        t.bo = bo.get();
        t.width = t.height = 64;
        t.rowBytes = 256;
        float c[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        uint64_t total = 0, worst = 0;
        unsigned done = 0;
        for (int i = 0; bo && i < 20; i++) {
            auto pass = ClearPass::make(device, t, c, ClearOptions(), &why);
            Submission s;
            uint64_t t0 = gpu.transport().nowUs();
            if (!pass || pass->submit(&s) != 0 || gpu.wait(s, 2000000) != 0 || (pass->status() & 0xff) != 1) break;
            uint64_t us = gpu.transport().nowUs() - t0;
            total += us;
            worst = us > worst ? us : worst;
            done++;
        }
        say("timing MEASURE %u of 20 clears done, %llu us each on average, %llu us the longest", done,
            (unsigned long long)(done ? total / done : 0), (unsigned long long)worst);
    }

    if (fake != nullptr) {
        say("fake: %llu jobs run, %zu faults, %llu pixels written", (unsigned long long)fake->jobsRun(),
            fake->faults().size(), (unsigned long long)fake->pixelsWritten());
        for (const auto& f : fake->faults()) say("fake fault: %s", f.why.c_str());
    }
    if (gShow == 0 && !gFake) capabilities();
    say("done: %u passed, %u failed; milestone clear64 %s", gPassed, gFailed, milestone ? "PASS" : "FAIL");
    return milestone ? 0 : 1;
}
