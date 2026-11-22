// hakios-mali: the Mali-G52 (Bifrost, v7) driven from user space by job descriptors this library writes itself, through
// the kernel's HakiosMali user client (drivers/iokit/HakiosMali.cpp), with no Gallium and no Panfrost
// driver in between. The requests are Linux's panfrost DRM ones (include/uapi/drm/panfrost_drm.h), each a call of the
// user client whose selector is the request's number and whose structure goes in and comes back; what Linux does with
// mmap() on the device is the client memory of the type that is the buffer's handle.
//
// Under the Gpu is a Transport, the only part that knows IOKit, so that everything above it runs on any machine against
// the fake GPU (fake_gpu.hpp), which answers the same requests and runs the job chains it is given.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hakios::mali {

// --- the requests, as HakiosMali numbers and lays them out ----------------------------------------------------------

enum Request : uint32_t {
    kGemClose = 0x09,
    kSubmit = 0x40,
    kWaitBo = 0x41,
    kCreateBo = 0x42,
    kMmapBo = 0x43,
    kGetParam = 0x44,
    kGetBoOffset = 0x45,
    kMadvise = 0x48,
    kHakiosWait = 0x7d,      // HakiosMali's own: wait for a job chain by the number kSubmit gave back in jc
    kHakiosSurface = 0x7e,   // HakiosMali's own: an IOSurface's memory as a buffer, by the surface's ID
    kHakiosShow = 0x7f,      // HakiosMali's own: a test's pixels onto the panel, as they are (no scaling), BGRA
};

struct DrmGemClose { uint32_t handle, pad; };
struct PanfrostSubmit {
    uint64_t jc, in_syncs;
    uint32_t in_sync_count, out_sync;
    uint64_t bo_handles;
    uint32_t bo_handle_count, requirements;
};
struct PanfrostWaitBo { uint32_t handle, pad; int64_t timeout_ns; };
struct PanfrostCreateBo { uint32_t size, flags, handle, pad; uint64_t offset; };
struct PanfrostMmapBo { uint32_t handle, flags; uint64_t offset; };
struct PanfrostGetParam { uint32_t param, pad; uint64_t value; };
struct PanfrostGetBoOffset { uint32_t handle, pad; uint64_t offset; };
struct HakiosWait { uint64_t seq; int64_t timeout_ns; };
struct HakiosShow { uint64_t pixels; uint32_t width, height, rowBytes, pad; };
// struct hakios_surface (HakiosMali.cpp surfaceBuffer): SURFACE the IOSurface's ID in; HANDLE back (the same while this
// client holds the surface's buffer); SIZE the buffer's bytes, OFFSET where the pixels start, ROW_BYTES, the surface's
// WIDTH, HEIGHT, PIXEL_FORMAT (a FourCC) and PLANES
struct HakiosSurface { uint32_t surface, handle; uint64_t size; uint32_t offset, rowBytes, width, height, pixelFormat, planes; };
static_assert(sizeof(HakiosSurface) == 40, "HakiosMali.cpp layout");

static_assert(sizeof(PanfrostSubmit) == 40 && sizeof(PanfrostCreateBo) == 24 && sizeof(PanfrostGetParam) == 16,
              "panfrost_drm.h layout");
static_assert(sizeof(HakiosShow) == 24, "HakiosMali.cpp layout");
static_assert(sizeof(PanfrostWaitBo) == 16 && sizeof(PanfrostGetBoOffset) == 16 && sizeof(HakiosWait) == 16,
              "HakiosMali.cpp layout");

constexpr uint32_t kJdReqFs = 1u << 0;    // PANFROST_JD_REQ_FS: a fragment chain (job slot 0); otherwise slot 1

// DRM_PANFROST_PARAM_*, the ones used here (mtk_mali_parameter answers them from the GPU's registers)
enum : uint32_t {
    kParamGpuProdId = 0,      // GPU_ID[31:16]: 0x7212 for the G52
    kParamGpuRevision = 1,
    kParamShaderPresent = 2,
    kParamTilerPresent = 3,
    kParamL2Present = 4,
    kParamTilerFeatures = 10, // [4:0] log2 bin size, [11:8] max hierarchy levels (panfrost_query_tiler_features)
    kParamThreadTlsAlloc = 39,
};

// The Bifrost v7 GPUs as Panfrost knows them (pan_props.c, panfrost_model_list): product id, name, tile buffer bytes.
// The X6826B's answers 0x7402, "G52 r1" (TGOx, r1p0: revision 0x1000), measured in run 500; Panfrost budgets half of
// the tile buffer (panfrost_query_optimal_tib_size). nullptr: not a v7 GPU these descriptors are for.
struct Model {
    uint32_t productId;
    const char* name;
    uint32_t tileBuffer;
};
const Model* modelOf(uint64_t productId);
constexpr uint32_t kPhoneProductId = 0x7402;
constexpr uint32_t kG52TileBufferBudget = 16384 / 2;

// What the GPU writes into a job's Exception Status when the job ends: the low byte, by Linux's panfrost
// (drivers/gpu/drm/panfrost/panfrost_device.c, panfrost_exception_name) and Mesa's pandecode_abort_on_fault (1: done).
const char* exceptionName(uint32_t status);

// --- under it: the one part that talks to the kernel --------------------------------------------------------------------

class Transport {
public:
    virtual ~Transport() = default;
    // the request with its structure in and out: 0, or an errno (ENOMEM, ENOENT, EINVAL, ETIMEDOUT, ...)
    virtual int call(uint32_t request, void* data, size_t size) = 0;
    // the buffer's memory in this process, at least *SIZE bytes, and *SIZE then what was mapped; nullptr if not
    virtual void* map(uint32_t handle, uint64_t* size) = 0;
    virtual void unmap(uint32_t handle, void* address, uint64_t size) = 0;
    // now, in microseconds of a monotonic clock
    virtual uint64_t nowUs() = 0;
};

// HakiosMali's user client (Darwin: iokit_transport.cpp). nullptr when there is no such service within WAIT_SECONDS
// (no boot-arg mali=1, the GPU not up to step 6, or not the phone), with *why saying which.
std::unique_ptr<Transport> openKernelMali(unsigned waitSeconds, std::string* why);

// --- the GPU --------------------------------------------------------------------------------------------------------

class Gpu;

// One buffer (a GEM object). The GPU's for as long as it lives; the destructor gives it back.
class Bo {
public:
    ~Bo();
    Bo(const Bo&) = delete;
    Bo& operator=(const Bo&) = delete;

    uint32_t handle() const { return handle_; }
    uint64_t size() const { return size_; }
    uint64_t gpu() const { return gpu_; }
    uint8_t* cpu() const { return cpu_; }     // write-combined: write it in order, read it rarely

private:
    friend class Gpu;
    Bo(Gpu& gpu, uint32_t handle, uint64_t size, uint64_t address, uint8_t* cpu)
        : owner_(gpu), handle_(handle), size_(size), gpu_(address), cpu_(cpu) {}
    Gpu& owner_;
    uint32_t handle_;
    uint64_t size_;
    uint64_t gpu_;
    uint8_t* cpu_;
};

// One job chain sent: its number (the kernel's queue is in order) and the slot it went to.
struct Submission {
    uint64_t seq = 0;
    bool fragment = false;
};

class Gpu {
public:
    explicit Gpu(std::unique_ptr<Transport> transport) : t_(std::move(transport)) {}

    // DRM_PANFROST_PARAM_*: 0 and *value, or an errno
    int param(uint32_t which, uint64_t* value);

    // SIZE bytes (rounded up to a page by the kernel), its GPU address and its mapping; nullptr with *why
    std::unique_ptr<Bo> newBo(uint64_t size, std::string* why);

    // the chain from GPU address JC, on the fragment slot or the other, naming the buffers it uses (each remembers the
    // job, and none is freed under it): 0 and *out, or an errno
    int submit(uint64_t jc, bool fragment, const std::vector<const Bo*>& uses, Submission* out);

    // until the chain is done, or TIMEOUT_US: 0, ETIMEDOUT, or another errno. Done is not "succeeded": the job
    // headers say that (their Exception Status), and the kernel resets the GPU after a chain that failed.
    int wait(const Submission& s, uint64_t timeoutUs);

    // the memory of IOSurface SURFACE_ID as a buffer (the kernel holds the surface while the buffer lives) and what the
    // kernel says of it in *info (offset, rowBytes, width, height, pixelFormat in the struct); nullptr with *why
    std::unique_ptr<Bo> surfaceBo(uint32_t surfaceId, HakiosSurface* info, std::string* why);

    // PIXELS (B G R A, ROW_BYTES apart) onto the panel's top left corner, until the window server's next frame
    int show(const void* pixels, uint32_t width, uint32_t height, uint32_t rowBytes);

    Transport& transport() { return *t_; }

private:
    friend class Bo;
    void release(Bo& bo);
    std::unique_ptr<Transport> t_;
};

}  // namespace hakios::mali
