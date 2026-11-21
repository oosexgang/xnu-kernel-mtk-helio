/*
 * HakiosMali's user client for the native Metal library (libhakiosmali.dylib): the panfrost requests the kernel serves
 * (iokit/Kernel/HakiosMali.cpp, the same numbers as mali.hpp's Request) in C, and a cache of buffers given back -- a
 * buffer made by the kernel is ~2.5 ms (hakios_drm.c's note, run 141), and a frame of the window server needs dozens.
 *
 * A buffer is handed out again only once the job that last used it is done (km_bo_release takes that job's number).
 * Imported surfaces are never cached: their memory is CoreAnimation's.
 */
#ifndef KMALI_H
#define KMALI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct km_surface_info {
	uint32_t offset, row_bytes, width, height, pixel_format, planes;
	uint64_t size;
};

struct km_bo {
	uint32_t handle;
	uint64_t size;          /* the kernel's pages */
	uint64_t gpu;
	uint8_t *cpu;           /* write-combined */
	bool imported;          /* an IOSurface's memory */
	uint64_t busy_seq;      /* the last job that uses it (cache: not handed out before it is done) */
	uint32_t mark;          /* a batch's stamp, so it is named once in that batch's list */
	uint32_t surface_id;    /* an imported one's IOSurface, and how many hold it (the kernel's handle is one a surface) */
	unsigned surface_refs;
	struct km_surface_info surface;
	uint64_t linger_since;  /* an imported one let go by its last texture, kept a while (kmali.c lingering) */
	struct km_bo *next;     /* the cache's list */
};



/* the service, once per process: 0, or -1 and a sentence */
int km_open(char *why, size_t why_size);
/* DRM_PANFROST_PARAM_*: 0 and *VALUE, or an errno */
int km_param(uint32_t which, uint64_t *value);
/* DRM_PANFROST_PARAM_GPU_PROD_ID (0x7212 on the G52), as Panfrost's gpu_id */
unsigned km_gpu_id(void);
/* a buffer of at least SIZE bytes, from the cache when one is free; NULL and a sentence */
struct km_bo *km_bo_new(uint64_t size, char *why, size_t why_size);
/* the two halves of km_bo_new: one from the cache (NULL when none is free), and a new one the kernel makes (slow) */
struct km_bo *km_bo_reuse(uint64_t size);
struct km_bo *km_bo_make(uint64_t size, char *why, size_t why_size);
/* given back: kept for the next ask once job SEQ is done (0: now); an imported one is closed */
void km_bo_release(struct km_bo *bo, uint64_t seq);
/* IOSurface SURFACE_ID's memory as a buffer, and what the kernel says of it */
struct km_bo *km_surface(uint32_t surface_id, struct km_surface_info *info, char *why, size_t why_size);
/* the job chain at JC, the fragment slot or the other, naming BOS (none freed under it): 0 and *SEQ, or an errno */
int km_submit(uint64_t jc, bool fragment, struct km_bo *const *bos, unsigned count, uint64_t *seq);
/* until job SEQ is done, at most TIMEOUT_NS (0: only ask): 0, ETIMEDOUT, or another errno */
int km_wait(uint64_t seq, int64_t timeout_ns);
/* the last job submitted by this process */
uint64_t km_last_seq(void);
/* the kernel's display: surface SURFACE's last job (~0: drawn and not yet sent) */
int km_surface_seq(uint32_t surface, uint64_t seq);
/* counters for the five-second lines: buffers made by the kernel, taken from the cache, submits */
void km_counts(uint64_t out[4]);
void km_made_time(uint64_t out[8]);
/* the buffers let go under the library's lock, given back to the kernel now (called as the lock is let go) */
void km_close_pending(void);

#ifdef __cplusplus
}
#endif

#endif
