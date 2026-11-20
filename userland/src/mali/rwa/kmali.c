/*
 * HakiosMali's user client, in C, for libhakiosmali.dylib (kmali.h). The requests and structures are mali.hpp's (and so
 * panfrost_drm.h's and HakiosMali.cpp's); the transport is iokit_transport.cpp's: the request's number is the user
 * client's selector, its structure goes in and comes back.
 */
#include "kmali.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

enum {
	KM_GEM_CLOSE = 0x09,
	KM_SUBMIT = 0x40,
	KM_CREATE_BO = 0x42,
	KM_MMAP_BO = 0x43,
	KM_GET_PARAM = 0x44,
	KM_GET_BO_OFFSET = 0x45,
	KM_SURFACE_SEQ = 0x7c,  /* hakios_drm.c hakios_drm_surface_seq */
	KM_WAIT = 0x7d,
	KM_SURFACE = 0x7e,
};

struct km_submit_req {
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements;
};
struct km_create_req { uint32_t size, flags, handle, pad; uint64_t offset; };
struct km_mmap_req { uint32_t handle, flags; uint64_t offset; };
struct km_param_req { uint32_t param, pad; uint64_t value; };
struct km_offset_req { uint32_t handle, pad; uint64_t offset; };
struct km_wait_req { uint64_t seq; int64_t timeout_ns; };
struct km_surface_req { uint32_t surface, handle; uint64_t size; uint32_t offset, row_bytes, width, height, pixel_format, planes; };
_Static_assert(sizeof(struct km_submit_req) == 40 && sizeof(struct km_create_req) == 24, "panfrost_drm.h");
_Static_assert(sizeof(struct km_surface_req) == 40, "HakiosMali.cpp");

static io_connect_t conn;
static unsigned gpu_id;
static uint64_t last_seq;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t made, reused, submits, cached_bytes;
static struct km_bo *cache;           /* given back, newest first */
static struct km_bo *surfaces;        /* imported, by surface */
static uint64_t made_ns, made_max_ns, made_bytes;   /* km_made_time */
static struct km_bo *closing;         /* imported, let go while the GPU still had them: closed once done (reap) */
/* Not more: run 618 kept 128 MB here (with the closing on a queue of its own) and the phone ran out of memory -- 30 MB
 * free, buffers made and given back in up to 3 and 6 s, surfaces imported in up to 7 s, no frame for ten seconds */
#define KM_CACHE_MAX (48ull << 20)

static int
errno_of(kern_return_t kr)
{
	switch (kr) {
	case KERN_SUCCESS: return 0;
	case kIOReturnNoMemory: return ENOMEM;
	case kIOReturnBadArgument: return EINVAL;
	case kIOReturnTimeout: return ETIMEDOUT;
	case kIOReturnUnsupported: return ENOTSUP;
	case kIOReturnNotReady: return EAGAIN;
	case kIOReturnVMError: return EFAULT;
	case kIOReturnNotFound: return ENOENT;
	default: return EIO;
	}
}

static int
call(uint32_t request, void *data, size_t size)
{
	size_t out = size;

	if (conn == 0)
		return ENODEV;
	return errno_of(IOConnectCallStructMethod(conn, request, data, size, data, &out));
}

/* RWA_HOST=1 (a build on the Mac with no HakiosMali): buffers are this process's memory at made-up addresses, a
 * submit is taken and done at once -- for compiling and laying out, not for pictures */
static bool fake;
static uint64_t fake_gpu = 0x100000000ull;

#ifdef RWA_PANDECODE
/* the host build with RWA_PANDECODE=1: every buffer known to Panfrost's decoder, every chain submitted decoded to
 * stderr -- the descriptors as the GPU would read them, and the decoder's complaints */
#include "wrap.h"
static struct pandecode_context *decode;
#endif

int
km_open(char *why, size_t why_size)
{
	if (getenv("RWA_HOST") != NULL) {
		fake = true;
		gpu_id = 0x7402;   /* the phone's (Mali-G52 r1): the stage cache is compiled here for it */
#ifdef RWA_PANDECODE
		if (getenv("RWA_PANDECODE") != NULL)
			decode = pandecode_create_context(true);
#endif
		return 0;
	}
	pthread_mutex_lock(&lock);
	if (conn != 0) {
		pthread_mutex_unlock(&lock);
		return 0;
	}
	io_service_t service = 0;
	for (unsigned waited = 0; (service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosMali"))) == 0 &&
	    waited < 8; waited++)
		usleep(250000);
	if (service == 0) {
		snprintf(why, why_size, "no HakiosMali service (boot-arg mali=1? the GPU up to step 6 of 6?)");
		pthread_mutex_unlock(&lock);
		return -1;
	}
	kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS) {
		conn = 0;
		snprintf(why, why_size, "IOServiceOpen of HakiosMali: 0x%x", kr);
		pthread_mutex_unlock(&lock);
		return -1;
	}
	struct km_param_req p = { 0, 0, 0 };   /* DRM_PANFROST_PARAM_GPU_PROD_ID */
	if (call(KM_GET_PARAM, &p, sizeof(p)) != 0 || p.value == 0) {
		snprintf(why, why_size, "HakiosMali does not say which GPU it is");
		IOServiceClose(conn);
		conn = 0;
		pthread_mutex_unlock(&lock);
		return -1;
	}
	gpu_id = (unsigned)p.value;
	pthread_mutex_unlock(&lock);
	return 0;
}

int
km_param(uint32_t which, uint64_t *value)
{
	struct km_param_req p = { which, 0, 0 };
	int e = call(KM_GET_PARAM, &p, sizeof(p));

	if (e == 0)
		*value = p.value;
	return e;
}

unsigned
km_gpu_id(void)
{
	return gpu_id;
}

static void *
map(uint32_t handle, uint64_t *size)
{
	mach_vm_address_t where = 0;
	mach_vm_size_t got = 0;

	if (IOConnectMapMemory64(conn, handle, mach_task_self(), &where, &got, kIOMapAnywhere) != KERN_SUCCESS ||
	    got < *size) {
		if (where != 0)
			IOConnectUnmapMemory64(conn, handle, mach_task_self(), where);
		return NULL;
	}
	*size = got;
	return (void *)where;
}

static void
close_bo(struct km_bo *bo)
{
	if (bo->cpu != NULL)
		IOConnectUnmapMemory64(conn, bo->handle, mach_task_self(), (mach_vm_address_t)bo->cpu);
	struct { uint32_t handle, pad; } c = { bo->handle, 0 };
	(void)call(KM_GEM_CLOSE, &c, sizeof(c));
	free(bo);
}

/* A buffer the GPU is done with given back to the kernel, timed (km_made_time). */
static uint64_t closed_n, closed_ns, closed_max_ns;

static void
close_bo_now(struct km_bo *context)
{
	const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	close_bo(context);
	const uint64_t took = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began;
	__atomic_fetch_add(&closed_n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&closed_ns, took, __ATOMIC_RELAXED);
	uint64_t was = __atomic_load_n(&closed_max_ns, __ATOMIC_RELAXED);
	while (took > was && !__atomic_compare_exchange_n(&closed_max_ns, &was, took, false, __ATOMIC_RELAXED,
							  __ATOMIC_RELAXED))
		;
}

/*
 * Given back by km_close_pending(), which the library calls the moment its lock is let go (rwa_mali.c GUARD), on the
 * same thread: no longer inside the lock the window server's three drawing threads share (run 616:
 * rwa_texture_destroy over 5 ms 63 to 251 times in 30 s, 10 to 29 ms each), and no later than that. (Run 618 gave
 * them back on a queue of its own: late, under load, and with a bigger cache the phone ran out of memory.)
 */
static struct km_bo *to_close;

static void
close_later(struct km_bo *bo)
{
	pthread_mutex_lock(&lock);
	bo->next = to_close;
	to_close = bo;
	pthread_mutex_unlock(&lock);
}

void
km_close_pending(void)
{
	if (__atomic_load_n(&to_close, __ATOMIC_RELAXED) == NULL)
		return;
	pthread_mutex_lock(&lock);
	struct km_bo *all = to_close;
	to_close = NULL;
	pthread_mutex_unlock(&lock);
	while (all != NULL) {
		struct km_bo *n = all->next;
		close_bo_now(all);
		all = n;
	}
}

/*
 * Whether job SEQ is done. The kernel runs every process's jobs one after another in the order given (mtk_mali.c's
 * one queue), so a job done means every job before it done: the highest seen done is kept and what is at or below it
 * answered here, not by a KM_WAIT each -- km_bo_new asked that of every busy buffer in its cache before the free one
 * it found, a call each (run 577: the path rasterizer's varyings in chunks of their own, ~4 us more a draw).
 */
static uint64_t known_done;

static bool
done(uint64_t seq)
{
	if (seq == 0 || seq <= __atomic_load_n(&known_done, __ATOMIC_RELAXED))
		return true;
	struct km_wait_req w = { seq, 0 };
	if (call(KM_WAIT, &w, sizeof(w)) != 0)
		return false;
	uint64_t was = __atomic_load_n(&known_done, __ATOMIC_RELAXED);
	while (was < seq && !__atomic_compare_exchange_n(&known_done, &was, seq, false, __ATOMIC_RELAXED,
							 __ATOMIC_RELAXED))
		;
	return true;
}

struct km_bo *
km_bo_new(uint64_t size, char *why, size_t why_size)
{
	if (size == 0 || size > (256ull << 20)) {
		snprintf(why, why_size, "a buffer of %llu bytes (HakiosMali takes 1 byte to 256 MB)", (unsigned long long)size);
		return NULL;
	}
	size = (size + 16383) & ~16383ull;
	if (fake) {
		struct km_bo *bo = calloc(1, sizeof(*bo));
		bo->cpu = calloc(1, size);
		bo->size = size;
		bo->gpu = fake_gpu;
		fake_gpu += size;
		bo->handle = 1;
#ifdef RWA_PANDECODE
		if (decode)
			pandecode_inject_mmap(decode, bo->gpu, bo->cpu, (unsigned)size, NULL);
#endif
		return bo;
	}
	pthread_mutex_lock(&lock);
	/* the smallest free one that is big enough and not more than twice the size; one found busy, every one of its
	 * job or later is busy too (done) */
	struct km_bo **best = NULL;
	uint64_t busy_from = UINT64_MAX;
	for (struct km_bo **p = &cache; *p != NULL; p = &(*p)->next) {
		struct km_bo *b = *p;
		if (b->size < size || b->size > 2 * size + 65536)
			continue;
		if (best != NULL && (*best)->size <= b->size)
			continue;
		if (b->busy_seq >= busy_from)
			continue;
		if (!done(b->busy_seq)) {
			busy_from = b->busy_seq;
			continue;
		}
		best = p;
	}
	if (best != NULL) {
		struct km_bo *b = *best;
		*best = b->next;
		b->next = NULL;
		b->busy_seq = 0;
		cached_bytes -= b->size;
		reused++;
		pthread_mutex_unlock(&lock);
		return b;
	}
	pthread_mutex_unlock(&lock);

	const uint64_t create_began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	struct km_create_req create = { (uint32_t)size, 0, 0, 0, 0 };
	int error = call(KM_CREATE_BO, &create, sizeof(create));
	if (error != 0) {
		/* out of memory: the cache's buffers back to the kernel, and once more */
		pthread_mutex_lock(&lock);
		struct km_bo *all = cache;
		cache = NULL;
		cached_bytes = 0;
		pthread_mutex_unlock(&lock);
		while (all != NULL) {
			struct km_bo *n = all->next;
			if (all->busy_seq != 0) {
				struct km_wait_req w = { all->busy_seq, 1000000000 };
				(void)call(KM_WAIT, &w, sizeof(w));
			}
			close_bo(all);
			all = n;
		}
		error = call(KM_CREATE_BO, &create, sizeof(create));
	}
	if (error != 0) {
		snprintf(why, why_size, "CREATE_BO of %llu bytes: %s", (unsigned long long)size, strerror(error));
		return NULL;
	}
	struct km_offset_req off = { create.handle, 0, 0 };
	struct km_mmap_req mm = { create.handle, 0, 0 };
	uint64_t mapped = size;
	void *cpu = NULL;
	if (call(KM_GET_BO_OFFSET, &off, sizeof(off)) == 0 && off.offset == create.offset && off.offset != 0 &&
	    call(KM_MMAP_BO, &mm, sizeof(mm)) == 0)
		cpu = map(create.handle, &mapped);
	if (cpu == NULL) {
		struct { uint32_t handle, pad; } c = { create.handle, 0 };
		(void)call(KM_GEM_CLOSE, &c, sizeof(c));
		snprintf(why, why_size, "a buffer of %llu bytes cannot be mapped", (unsigned long long)size);
		return NULL;
	}
	struct km_bo *bo = calloc(1, sizeof(*bo));
	if (bo == NULL) {
		IOConnectUnmapMemory64(conn, create.handle, mach_task_self(), (mach_vm_address_t)cpu);
		struct { uint32_t handle, pad; } c = { create.handle, 0 };
		(void)call(KM_GEM_CLOSE, &c, sizeof(c));
		snprintf(why, why_size, "out of memory");
		return NULL;
	}
	bo->handle = create.handle;
	bo->size = mapped;
	bo->gpu = create.offset;
	bo->cpu = cpu;
	const uint64_t took = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - create_began;
	pthread_mutex_lock(&lock);
	made++;
	made_ns += took;
	made_bytes += mapped;
	if (took > made_max_ns)
		made_max_ns = took;
	pthread_mutex_unlock(&lock);
	return bo;
}

/*
 * The imported buffers let go while a job still used them, closed now that it is done. Until run 617 their last
 * release waited for that job (KM_WAIT, up to 2 s) -- under the library's lock, which the window server's three
 * drawing threads share: run 616 counted rwa_texture_destroy holding it over 5 ms 63 to 251 times in 30 s (up to
 * 29 ms each, 2.4 s in all), and draws that freed a batch waited the same way.
 */
static void
reap(void)
{
	if (__atomic_load_n(&closing, __ATOMIC_RELAXED) == NULL)
		return;
	struct km_bo *gone = NULL;
	pthread_mutex_lock(&lock);
	for (struct km_bo **p = &closing; *p != NULL;) {
		struct km_bo *b = *p;
		if (done(b->busy_seq)) {
			*p = b->next;
			b->next = gone;
			gone = b;
		} else {
			p = &b->next;
		}
	}
	pthread_mutex_unlock(&lock);
	while (gone != NULL) {
		struct km_bo *n = gone->next;
		close_later(gone);
		gone = n;
	}
}

void
km_bo_release(struct km_bo *bo, uint64_t seq)
{
	if (bo == NULL)
		return;
	if (fake) {
#ifdef RWA_PANDECODE
		if (decode)
			pandecode_inject_free(decode, bo->gpu, (unsigned)bo->size);
#endif
		free(bo->cpu);
		free(bo);
		return;
	}
	if (bo->imported) {
		pthread_mutex_lock(&lock);
		bool last = --bo->surface_refs == 0;
		if (seq > bo->busy_seq)
			bo->busy_seq = seq;
		if (last) {
			for (struct km_bo **p = &surfaces; *p != NULL; p = &(*p)->next)
				if (*p == bo) {
					*p = bo->next;
					break;
				}
		}
		if (last && !done(bo->busy_seq)) {
			bo->next = closing;     /* closed by reap() once its last job is done, not waited for here */
			closing = bo;
			last = false;
		}
		pthread_mutex_unlock(&lock);
		reap();
		if (!last)
			return;
		close_later(bo);
		return;
	}
	reap();
	pthread_mutex_lock(&lock);
	if (seq > bo->busy_seq)
		bo->busy_seq = seq;
	bo->next = cache;
	cache = bo;
	cached_bytes += bo->size;
	/* past the budget: the oldest done ones go back to the kernel */
	struct km_bo *drop = NULL;
	while (cached_bytes > KM_CACHE_MAX) {
		struct km_bo **p = &cache, **last = NULL;
		for (; *p != NULL; p = &(*p)->next)
			if (done((*p)->busy_seq))
				last = p;
		if (last == NULL)
			break;
		struct km_bo *b = *last;
		*last = b->next;
		cached_bytes -= b->size;
		b->next = drop;
		drop = b;
	}
	pthread_mutex_unlock(&lock);
	while (drop != NULL) {
		struct km_bo *n = drop->next;
		close_later(drop);
		drop = n;
	}
}

struct km_bo *
km_surface(uint32_t surface_id, struct km_surface_info *info, char *why, size_t why_size)
{
	struct km_surface_req s = { 0 };

	/* one buffer a surface, however many textures are made over it: the kernel hands this client the same handle
	 * while it holds the surface, so closing it for one would take it from the others */
	pthread_mutex_lock(&lock);
	for (struct km_bo *b = surfaces; b != NULL; b = b->next)
		if (b->surface_id == surface_id) {
			b->surface_refs++;
			*info = b->surface;
			pthread_mutex_unlock(&lock);
			return b;
		}
	pthread_mutex_unlock(&lock);

	s.surface = surface_id;
	int error = call(KM_SURFACE, &s, sizeof(s));
	if (error != 0 || s.handle == 0) {
		snprintf(why, why_size, "surface %u's memory as a buffer: %s", surface_id, strerror(error ? error : ENOENT));
		return NULL;
	}
	struct km_offset_req off = { s.handle, 0, 0 };
	uint64_t mapped = s.size;
	void *cpu = call(KM_GET_BO_OFFSET, &off, sizeof(off)) == 0 ? map(s.handle, &mapped) : NULL;
	if (cpu == NULL) {
		struct { uint32_t handle, pad; } c = { s.handle, 0 };
		(void)call(KM_GEM_CLOSE, &c, sizeof(c));
		snprintf(why, why_size, "surface %u's buffer cannot be mapped", surface_id);
		return NULL;
	}
	struct km_bo *bo = calloc(1, sizeof(*bo));
	if (bo == NULL) {
		snprintf(why, why_size, "out of memory");
		return NULL;
	}
	bo->handle = s.handle;
	bo->size = mapped;
	bo->gpu = off.offset;
	bo->cpu = cpu;
	bo->imported = true;
	info->offset = s.offset;
	info->row_bytes = s.row_bytes;
	info->width = s.width;
	info->height = s.height;
	info->pixel_format = s.pixel_format;
	info->planes = s.planes;
	info->size = s.size;
	bo->surface_id = surface_id;
	bo->surface_refs = 1;
	bo->surface = *info;
	pthread_mutex_lock(&lock);
	bo->next = surfaces;
	surfaces = bo;
	pthread_mutex_unlock(&lock);
	return bo;
}

int
km_submit(uint64_t jc, bool fragment, struct km_bo *const *bos, unsigned count, uint64_t *seq)
{
	if (fake) {
#ifdef RWA_PANDECODE
		if (decode) {
			fprintf(stderr, "=== %s chain %#llx\n", fragment ? "fragment" : "vertex/tiler", (unsigned long long)jc);
			pandecode_jc(decode, jc, gpu_id);
		}
#endif
		*seq = ++last_seq;
		return 0;
	}
	uint32_t stack[256];
	uint32_t *handles = count <= 256 ? stack : malloc(count * sizeof(uint32_t));

	if (handles == NULL)
		return ENOMEM;
	for (unsigned i = 0; i < count; i++)
		handles[i] = bos[i]->handle;
#if defined(__aarch64__)
	__asm__ volatile("dsb sy" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
	struct km_submit_req s = { 0 };
	s.jc = jc;
	s.bo_handles = (uint64_t)(uintptr_t)handles;
	s.bo_handle_count = count;
	s.requirements = fragment ? 1 : 0;   /* PANFROST_JD_REQ_FS */
	int error = call(KM_SUBMIT, &s, sizeof(s));
	if (handles != stack)
		free(handles);
	if (error == 0 && s.jc == 0)
		error = EIO;
	if (error == 0) {
		*seq = s.jc;
		pthread_mutex_lock(&lock);
		if (s.jc > last_seq)
			last_seq = s.jc;
		submits++;
		pthread_mutex_unlock(&lock);
	}
	return error;
}

int
km_wait(uint64_t seq, int64_t timeout_ns)
{
	if (seq == 0 || fake || seq <= __atomic_load_n(&known_done, __ATOMIC_RELAXED))
		return 0;
	struct km_wait_req w = { seq, timeout_ns };
	int e = call(KM_WAIT, &w, sizeof(w));
	if (e == 0) {
		/* done, and so is every job before it (done) */
		uint64_t was = __atomic_load_n(&known_done, __ATOMIC_RELAXED);
		while (was < seq && !__atomic_compare_exchange_n(&known_done, &was, seq, false, __ATOMIC_RELAXED,
								 __ATOMIC_RELAXED))
			;
	}
	return e;
}

uint64_t
km_last_seq(void)
{
	pthread_mutex_lock(&lock);
	uint64_t s = last_seq;
	pthread_mutex_unlock(&lock);
	return s;
}

int
km_surface_seq(uint32_t surface, uint64_t seq)
{
	struct { uint32_t surface, pad; uint64_t seq; } r = { surface, 0, seq };

	return call(KM_SURFACE_SEQ, &r, sizeof(r));
}

/* the buffers made by the kernel (not from the cache) since the last call: time in all, the longest, bytes; and those
 * given back on the closing queue: how many, time in all, the longest */
void
km_made_time(uint64_t out[6])
{
	pthread_mutex_lock(&lock);
	out[0] = made_ns;
	out[1] = made_max_ns;
	out[2] = made_bytes;
	made_ns = made_max_ns = made_bytes = 0;
	pthread_mutex_unlock(&lock);
	out[3] = __atomic_exchange_n(&closed_n, 0, __ATOMIC_RELAXED);
	out[4] = __atomic_exchange_n(&closed_ns, 0, __ATOMIC_RELAXED);
	out[5] = __atomic_exchange_n(&closed_max_ns, 0, __ATOMIC_RELAXED);
}

void
km_counts(uint64_t out[4])
{
	pthread_mutex_lock(&lock);
	out[0] = made;
	out[1] = reused;
	out[2] = submits;
	out[3] = cached_bytes;
	made = reused = submits = 0;
	pthread_mutex_unlock(&lock);
}
