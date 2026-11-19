/*
 * libhakiosmali.dylib: tools/mesa/air/air_metal.h's contract (rwa_*) on the Mali-G52, with this library writing the job
 * descriptors itself -- the same calls libhakiosair.dylib serves through Gallium's panfrost, so that the Metal plug-in
 * (src/hakiosmetal.m, userland/src/metal) draws the window server's frames through either, by boot-arg mali_native=1.
 *
 * What a pass is here: Metal's render pass reaches this library as draws (rwa_render_draw), each naming its targets.
 * Draws into the same targets with no clear between them are one BATCH -- one tiler context, one frame descriptor, one
 * fragment job -- as Panfrost batches draws per framebuffer (pan_job.c). A batch is submitted when the targets change, a
 * clear asks for a new pass, the processor needs what it touched, or the caller flushes (rwa_flush_seq, rwa_finish):
 * its vertex and tiler jobs on job slot 1, then its fragment job on slot 0, in the kernel's in-order queue.
 *
 * A target that a batch does not clear is LOADED into the tile buffer first by a frame shader (pan_fb_preload.c: a
 * pre-frame DCD, INTERSECT, that samples the target), or a pass that draws over a part of it would write the rest back
 * as the clear colour.
 *
 * Every refusal is a sentence, and the first of each kind goes to the host's kept lines (kern.hakios.say, "!!rwn"); the
 * draws done and refused are counted there every five seconds.
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/sysctl.h>
#include <dlfcn.h>

#include "genxml/gen_macros.h"
#include "pan_encoder.h"
#include "pan_format.h"
#include "pan_props.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/half_float.h"

#include "rwa_priv.h"

void rwa_preload_blend(const struct rwa_device *device, enum pipe_format format, unsigned rt, void *out, bool format_comps);

/* --- the lock, the kept lines -------------------------------------------------------------------------------------- */

static pthread_mutex_t rwa_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static inline void
unlock_at_return(int *held)
{
	(void)held;
	pthread_mutex_unlock(&rwa_lock);
}
#define GUARD()                                                                                                        \
	pthread_mutex_lock(&rwa_lock);                                                                                 \
	int rwa_guard_ __attribute__((cleanup(unlock_at_return), unused)) = 1

static void
say(char *why, size_t why_size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void
say(char *why, size_t why_size, const char *fmt, ...)
{
	va_list ap;

	if (why == NULL || why_size == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(why, why_size, fmt, ap);
	va_end(ap);
}

static void
keep(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void
keep(const char *fmt, ...)
{
	char line[400];
	va_list ap;

	va_start(ap, fmt);
	int n = snprintf(line, sizeof(line), "!!rwn %s: ", getprogname());
	vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
#ifndef RWA_HOST_FAKE
	sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
#endif
	fprintf(stderr, "%s\n", line);
}

/* the first refusal of each sentence (by its first 40 characters), into the kept lines */
static void
note_refusal(const char *what, const char *why)
{
	static char seen[24][40];
	static unsigned nseen;

	for (unsigned i = 0; i < nseen; i++)
		if (strncmp(seen[i], why, sizeof(seen[i]) - 1) == 0)
			return;
	if (nseen >= 24)
		return;
	snprintf(seen[nseen], sizeof(seen[nseen]), "%s", why);
	nseen++;
	keep("%.60s refused: %.200s", what, why);
}

static uint64_t
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static struct {
	uint64_t since, draws, refused, batches, preloads, clears, cpu_syncs, draw_ns, flush_ns, pipelines, pipeline_ns;
	uint64_t refused_pipelines, imports, multiload_cleared;
	uint64_t att_loaded, att_cleared, att_dontcare_load, att_stored, att_discarded;
	uint64_t vt_parts;   /* rwn_vtsplit: vertex and tiler parts sent before their pass's submit */
} stats;

static void report(void);
/* a draw's time by part: checks, batch_for (with the flush of the batch before), index range and viewport, vertex
 * stage, fragment stage, attributes, varyings and jobs */
static uint64_t phase_ns[7];
static uint64_t lib_up_ns;  /* when the device was opened */
static void sec_pipe_note(void);
/* boot-arg rwn_profile=2: each draw of a full-screen pass of several targets, its pipeline and where the GPU wrote its
 * vertices' screen positions -- the watcher thread reads them once the pass's vertex job is done, and the 30-second
 * lines give each pipeline's draws and the pixels their bounding boxes cover (scissor and frame clipped) */
struct area_rec {
	const uint8_t *pos;     /* the GPU's copy (pool_real) of the position buffer: x, y, z, 1/w a vertex */
	unsigned count;         /* vertices an instance (at most 64) */
	unsigned stride;        /* vertices between instances (the padded count when instanced) */
	unsigned instances;     /* read (at most 256) */
	unsigned all_instances;
	bool say;               /* a small offscreen pass's draw: its box said by itself */
	uint16_t target[2];
	int16_t clip[4];        /* the scissor: x0, y0, x1, y1 */
	char name[24];
};
static struct { char name[24]; uint64_t draws, instances; double pixels; } area_tab[32];
static unsigned narea_tab;
static float area_sample[4];

/* the pipelines of full-screen passes of several targets (run 541: one such pass a frame, ~70 ms of the GPU's ~80),
 * by draws, and whether they blend by shader */
static struct { struct rwa_render *r; uint64_t draws; char name[24]; bool bs, rd; } heavy[48];
static unsigned nheavy;

static void
heavy_note(struct rwa_render *r)
{
	for (unsigned i = 0; i < nheavy; i++)
		if (heavy[i].r == r) {
			heavy[i].draws++;
			return;
		}
	if (nheavy < ARRAY_SIZE(heavy)) {
		heavy[nheavy].r = r;
		snprintf(heavy[nheavy].name, sizeof(heavy[nheavy].name), "%s", r->fs.name);
		heavy[nheavy].bs = r->blend_shader_mask != 0;
		heavy[nheavy].rd = r->reads_dest_mask != 0;
		heavy[nheavy++].draws = 1;
	}
}
/* a stage's time by part, vertex [0] and fragment [1]: buffers, textures, samplers, system values, pushed words */
static uint64_t stage_ns[2][5];
static uint64_t surface_frames;

static bool
boot_arg(const char *text)
{
#ifdef RWA_HOST_FAKE
	const char *args = getenv("RWA_BOOTARGS");   /* the host's stand-in for kern.bootargs */
	return args != NULL && strstr(args, text) != NULL;
#else
	static char args[1024];
	static int read = 0;

	if (!read) {
		size_t n = sizeof(args) - 1;
		if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) != 0)
			args[0] = 0;
		read = 1;
	}
	return strstr(args, text) != NULL;
#endif
}

/* boot-arg NAME=N (NAME with its '='): N, or DEFAULT without it */
static unsigned
boot_arg_uint(const char *name, unsigned dflt)
{
	char args[1024] = "";
#ifdef RWA_HOST_FAKE
	const char *env = getenv("RWA_BOOTARGS");
	if (env)
		snprintf(args, sizeof(args), "%s", env);
#else
	size_t n = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) != 0)
		args[0] = 0;
#endif
	const char *at = strstr(args, name);
	return at ? (unsigned)strtoul(at + strlen(name), NULL, 10) : dflt;
}

/* --- objects ------------------------------------------------------------------------------------------------------- */

struct batch;

/* memory one or more resources share: a buffer, a texture and its views, an imported surface */
struct store {
	/* the last pass that drew into it, for the texture probe: its first pipeline, draws, cleared or loaded, level */
	char writer[64];
	unsigned writes;
	struct km_bo *bo;
	unsigned refs;
	uint64_t last_seq;        /* the last job that used it */
	struct batch *pending;    /* the open batch that uses it, not submitted yet */
	uint32_t surface_id;
};

struct rwa_buffer {
	struct rwa_device *device;
	struct store *store;
	size_t size;
};

struct rwa_texture {
	struct rwa_device *device;
	struct store *store;
	struct rwa_texture_shape shape;
	enum pipe_format format;
	unsigned blocksize, bw, bh;
	uint64_t offset;                  /* of level 0, layer 0, in the store */
	uint64_t level_offset[16];
	uint32_t row_bytes[16];
	uint64_t layer_stride;
	unsigned layers;
	unsigned dim;                     /* MALI_TEXTURE_DIMENSION_* */
	unsigned char swizzle[4];
	bool is_view;
	/* the texture probe (tex_probe): when it was last looked at, whether it ever held more than one colour */
	uint64_t probe_ns;
	bool probe_varied, probe_said;
};

struct rwa_sampler {
	struct rwa_sampler_state state;
	uint32_t desc[8], compare[8];
};

struct rwa_depth_stencil {
	struct rwa_depth_stencil_state state;
};

static struct store *
store_new(uint64_t size, char *why, size_t why_size)
{
	struct store *s = calloc(1, sizeof(*s));

	if (s == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	s->bo = km_bo_new(size, why, why_size);
	if (s->bo == NULL) {
		free(s);
		return NULL;
	}
	s->refs = 1;
	return s;
}

static void flush_batch(const char *reason);

static void
store_unref(struct store *s)
{
	if (s == NULL || --s->refs > 0)
		return;
	if (s->pending)
		flush_batch("a resource it uses is let go");
	km_bo_release(s->bo, s->last_seq);
	free(s);
}

/* the processor is about to read or write S: what the GPU has of it, done */
static void
store_sync(struct store *s)
{
	if (s->pending)
		flush_batch("the processor wants what it draws");
	if (s->last_seq) {
		stats.cpu_syncs++;
		km_wait(s->last_seq, 2000000000);
	}
}

/* --- the device ---------------------------------------------------------------------------------------------------- */

static struct rwa_device *the_device;
static unsigned thread_tls_alloc, core_id_range = 1, tiler_levels = 8;

#define HEAP_BYTES (16u << 20)

/* what a GPU buffer's memory costs the processor against its own, once at the start (ns a KB): byte stores (as the
 * descriptors' pack functions write), 4-byte loads, and a memcpy each way */
static void
memory_bench(uint8_t *wc)
{
	static uint8_t cached[4096];
	uint8_t *mem[2] = { wc, cached };
	double ns[2][4];
	for (int m = 0; m < 2; m++) {
		volatile uint8_t *v = mem[m];
		uint64_t t = now_ns();
		for (int rep = 0; rep < 4; rep++)
			for (int i = 0; i < 4096; i++)
				v[i] = (uint8_t)(i + rep);
		ns[m][0] = (now_ns() - t) / 16.0;
		volatile const uint32_t *w = (volatile const uint32_t *)mem[m];
		uint32_t sum = 0;
		t = now_ns();
		for (int rep = 0; rep < 4; rep++)
			for (int i = 0; i < 1024; i++)
				sum += w[i];
		ns[m][1] = (now_ns() - t) / 16.0;
		static uint8_t tmp[4096];
		t = now_ns();
		for (int rep = 0; rep < 4; rep++)
			memcpy(tmp, mem[m], 4096);
		ns[m][2] = (now_ns() - t) / 16.0;
		t = now_ns();
		for (int rep = 0; rep < 4; rep++)
			memcpy(mem[m], tmp, 4096);
		ns[m][3] = (now_ns() - t) / 16.0;
		(void)sum;
	}
	memset(wc, 0, 4096);   /* the dummy's zeros, as they were */
	keep("memory, ns a KB, GPU buffer / cached: byte stores %.0f / %.0f, word loads %.0f / %.0f, memcpy out %.0f / %.0f, "
	     "memcpy in %.0f / %.0f", ns[0][0], ns[1][0], ns[0][1], ns[1][1], ns[0][2], ns[1][2], ns[0][3], ns[1][3]);
}

struct rwa_device *
rwa_device_open(char *why, size_t why_size)
{
	GUARD();
	if (the_device)
		return the_device;
	if (km_open(why, why_size) != 0)
		return NULL;
	struct rwa_device *d = calloc(1, sizeof(*d));
	if (d == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	d->gpu_id = km_gpu_id();
	/* which Helio and which Mali (rwa_helio.c): said at the start, and in the refusal of a GPU not served */
	const char *soc = rwa_board_soc();
	const struct rwa_helio *h = rwa_helio_find(soc[0] ? soc : NULL, d->gpu_id);
	const struct panfrost_model *model = panfrost_get_model(d->gpu_id, 0);
	if (pan_arch(d->gpu_id) != 7) {
		say(why, why_size, "%s%s%s: the GPU is product %#x (%s), %s", h ? h->name : (soc[0] ? soc : "this board"),
		    h ? ", " : "", h ? h->gpu : "", d->gpu_id, model ? model->name : "not in Panfrost's table",
		    rwa_arch_status(pan_arch(d->gpu_id), d->gpu_id));
		free(d);
		return NULL;
	}
	uint64_t v = 0;
	if (km_param(10, &v) == 0 && ((v >> 8) & 0xf) >= 2)   /* TILER_FEATURES: hierarchy levels */
		tiler_levels = (v >> 8) & 0xf;
	if (km_param(2, &v) == 0 && v != 0)                   /* SHADER_PRESENT */
		core_id_range = 64 - __builtin_clzll(v);
	if (km_param(39, &v) == 0)                            /* THREAD_TLS_ALLOC */
		thread_tls_alloc = (unsigned)v;
	snprintf(d->name, sizeof(d->name), "Mali-G52 r1 (hakios-mali, native)");
	/* the model's tile buffer, halved (panfrost_query_optimal_tib_size): the G52's and G76's 16 KB */
	d->tile_budget = model && model->tilebuffer_size ? model->tilebuffer_size / 2 : 8192;
	d->tiler_heap = km_bo_new(HEAP_BYTES, why, why_size);
	d->sample_positions = d->tiler_heap ? km_bo_new(4096, why, why_size) : NULL;
	d->dummy = d->sample_positions ? km_bo_new(65536, why, why_size) : NULL;
	if (d->dummy == NULL) {
		free(d);
		return NULL;
	}
	memset(d->dummy->cpu, 0, 65536);
	/* pan_samples.c: positions of (x, y) in 1/256 of a pixel, (x + 8) * 16; single sampled at the centre, and the
	 * rotated 4x grid at pattern 2 (offset 512), the origin at index 32 of each */
	{
		uint16_t *p = (uint16_t *)d->sample_positions->cpu;
		static const int rotated[4][2] = { { -1, -3 }, { 3, -1 }, { -3, 1 }, { 1, 3 } };
		memset(p, 0, 768);
		p[0] = p[1] = 8 * 16;
		p[64] = p[65] = 8 * 16;
		uint16_t *q = p + 2 * 128;
		for (int i = 0; i < 4; i++) {
			q[2 * i] = (uint16_t)((2 * rotated[i][0] + 8) * 16);
			q[2 * i + 1] = (uint16_t)((2 * rotated[i][1] + 8) * 16);
		}
		q[64] = q[65] = 8 * 16;
	}
	if (rwa_shader_init(d, why, why_size) != 0) {
		free(d);
		return NULL;
	}
	stats.since = now_ns();
	lib_up_ns = stats.since;
	the_device = d;
	Dl_info dl = { 0 };
	dladdr((const void *)rwa_device_open, &dl);
	keep("native Mali library up at %p: gpu %#x (%s), %u tiler levels, %u core ids, TLS %u, tile budget %u; %s%s%s%s",
	     dl.dli_fbase, d->gpu_id, model ? model->name : "?", tiler_levels, core_id_range, thread_tls_alloc,
	     d->tile_budget, soc[0] ? soc : "no hw.board.soc", h ? " = " : "", h ? h->name : "",
	     h && !h->run ? " (not run on a phone before)" : "");
	memory_bench(d->dummy->cpu + 32768);
	return d;
}

void
rwa_device_close(struct rwa_device *device)
{
	(void)device;   /* one per process, for as long as the process lives */
}

const char *
rwa_device_name(struct rwa_device *device)
{
	return device ? device->name : "Mali-G52 (hakios-mali, native)";
}

/* --- buffers ------------------------------------------------------------------------------------------------------- */

struct rwa_buffer *
rwa_buffer_create(struct rwa_device *device, size_t size, char *why, size_t why_size)
{
	GUARD();
	struct rwa_buffer *b = calloc(1, sizeof(*b));

	if (b == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	b->store = store_new(size ? size : 16, why, why_size);
	if (b->store == NULL) {
		free(b);
		return NULL;
	}
	b->device = device;
	b->size = size;
	return b;
}

void *
rwa_buffer_map(struct rwa_buffer *buffer)
{
	return buffer ? buffer->store->bo->cpu : NULL;
}

size_t
rwa_buffer_size(struct rwa_buffer *buffer)
{
	return buffer ? buffer->size : 0;
}

void
rwa_buffer_destroy(struct rwa_buffer *buffer)
{
	GUARD();
	if (buffer == NULL)
		return;
	store_unref(buffer->store);
	free(buffer);
}

/* --- textures ------------------------------------------------------------------------------------------------------ */

static unsigned
mali_dim(unsigned type, bool *array, bool *ok)
{
	/* MTLTextureType: 1D 0, 1DArray 1, 2D 2, 2DArray 3, 2DMultisample 4, Cube 5, CubeArray 6, 3D 7, 2DMSArray 8,
	 * TextureBuffer 9 */
	*array = false;
	*ok = true;
	switch (type) {
	case 0: return MALI_TEXTURE_DIMENSION_1D;
	case 1: *array = true; return MALI_TEXTURE_DIMENSION_1D;
	case 2: case 4: return MALI_TEXTURE_DIMENSION_2D;
	case 3: case 8: *array = true; return MALI_TEXTURE_DIMENSION_2D;
	case 5: return MALI_TEXTURE_DIMENSION_CUBE;
	case 6: *array = true; return MALI_TEXTURE_DIMENSION_CUBE;
	case 7: return MALI_TEXTURE_DIMENSION_3D;
	default: *ok = false; return MALI_TEXTURE_DIMENSION_2D;
	}
}

/* the layout of a linear image: levels after one another, rows 64-byte aligned, each level 64-byte aligned, layers
 * (and 3D slices) a whole chain apart (pan_layout.c's linear rule) */
static int
texture_layout(struct rwa_texture *t, const struct rwa_texture_shape *s, char *why, size_t why_size)
{
	bool array, ok;

	t->format = rwa_pipe_format(s->pixel_format);
	if (t->format == PIPE_FORMAT_NONE) {
		say(why, why_size, "pixel format %u is not one this library knows", s->pixel_format);
		return -1;
	}
	if (s->pixel_format == 260) {
		say(why, why_size, "Depth32Float_Stencil8 (two planes) is not served yet");
		return -1;
	}
	t->dim = mali_dim(s->type, &array, &ok);
	if (!ok) {
		say(why, why_size, "texture type %u is not served", s->type);
		return -1;
	}
	t->blocksize = util_format_get_blocksize(t->format);
	t->bw = util_format_get_blockwidth(t->format);
	t->bh = util_format_get_blockheight(t->format);
	unsigned levels = s->mip_levels ? s->mip_levels : 1;
	if (levels > 16) {
		say(why, why_size, "%u mip levels", levels);
		return -1;
	}
	unsigned faces = t->dim == MALI_TEXTURE_DIMENSION_CUBE ? 6 : 1;
	t->layers = (array ? MAX2(s->array_length, 1) : 1) * faces;
	uint64_t at = 0;
	for (unsigned l = 0; l < levels; l++) {
		unsigned w = u_minify(s->width ? s->width : 1, l), h = u_minify(s->height ? s->height : 1, l);
		unsigned d = t->dim == MALI_TEXTURE_DIMENSION_3D ? u_minify(MAX2(s->depth, 1), l) : 1;
		unsigned bx = DIV_ROUND_UP(w, t->bw), by = DIV_ROUND_UP(h, t->bh);
		t->row_bytes[l] = ALIGN_POT(bx * t->blocksize, 64);
		t->level_offset[l] = at;
		at += ALIGN_POT((uint64_t)t->row_bytes[l] * by * d, 64);
	}
	t->layer_stride = at;
	return 0;
}

static struct rwa_texture *
texture_new(struct rwa_device *device, const struct rwa_texture_shape *shape)
{
	struct rwa_texture *t = calloc(1, sizeof(*t));

	if (t == NULL)
		return NULL;
	t->device = device;
	t->shape = *shape;
	t->swizzle[0] = PIPE_SWIZZLE_X;
	t->swizzle[1] = PIPE_SWIZZLE_Y;
	t->swizzle[2] = PIPE_SWIZZLE_Z;
	t->swizzle[3] = PIPE_SWIZZLE_W;
	return t;
}

struct rwa_texture *
rwa_texture_create(struct rwa_device *device, const struct rwa_texture_shape *shape, char *why, size_t why_size)
{
	GUARD();
	struct rwa_texture *t = texture_new(device, shape);

	if (t == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	if (texture_layout(t, shape, why, why_size) != 0) {
		free(t);
		return NULL;
	}
	if (shape->sample_count > 1) {
		say(why, why_size, "a multisample texture is not served yet");
		free(t);
		return NULL;
	}
	t->store = store_new(t->layer_stride * t->layers, why, why_size);
	if (t->store == NULL) {
		free(t);
		return NULL;
	}
	return t;
}

struct rwa_texture *
rwa_texture_view_create(struct rwa_texture *parent, unsigned pixel_format, char *why, size_t why_size)
{
	GUARD();
	enum pipe_format f = rwa_pipe_format(pixel_format);

	if (parent == NULL || f == PIPE_FORMAT_NONE ||
	    util_format_get_blocksize(f) != parent->blocksize) {
		say(why, why_size, "a view as pixel format %u: not of the same texel size", pixel_format);
		return NULL;
	}
	struct rwa_texture *t = malloc(sizeof(*t));
	if (t == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	*t = *parent;
	t->format = f;
	t->shape.pixel_format = pixel_format;
	t->is_view = true;
	t->store->refs++;
	return t;
}

struct rwa_texture *
rwa_texture_from_surface(struct rwa_device *device, const struct rwa_texture_shape *shape, int surface_id,
			 unsigned offset, unsigned row_bytes, char *why, size_t why_size)
{
	GUARD();
	if (shape->type != 2 || (shape->mip_levels > 1) || shape->sample_count > 1 || shape->array_length > 1) {
		say(why, why_size, "a drawable is a 2D texture of one level, one sample, one slice");
		return NULL;
	}
	struct rwa_texture *t = texture_new(device, shape);
	if (t == NULL || texture_layout(t, shape, why, why_size) != 0) {
		free(t);
		return NULL;
	}
	struct km_surface_info info;
	struct km_bo *bo = km_surface((uint32_t)surface_id, &info, why, why_size);
	if (bo == NULL) {
		free(t);
		return NULL;
	}
	t->store = calloc(1, sizeof(*t->store));
	if (t->store == NULL) {
		km_bo_release(bo, 0);
		free(t);
		say(why, why_size, "out of memory");
		return NULL;
	}
	t->store->bo = bo;
	t->store->refs = 1;
	t->store->surface_id = (uint32_t)surface_id;
	t->offset = offset ? offset : info.offset;
	t->row_bytes[0] = row_bytes ? row_bytes : info.row_bytes;
	t->level_offset[0] = 0;
	t->layer_stride = (uint64_t)t->row_bytes[0] * shape->height;
	if (t->offset + t->layer_stride > bo->size || (t->row_bytes[0] & 63) != 0 || ((bo->gpu + t->offset) & 63) != 0) {
		say(why, why_size, "surface %d: %u bytes a row at %llu is not 64-byte aligned rows inside its %llu bytes",
		    surface_id, t->row_bytes[0], (unsigned long long)t->offset, (unsigned long long)bo->size);
		store_unref(t->store);
		free(t);
		return NULL;
	}
	stats.imports++;
	return t;
}

int
rwa_texture_set_swizzle(struct rwa_texture *texture, const unsigned char channels[4])
{
	/* MTLTextureSwizzle: Zero 0, One 1, Red 2 .. Alpha 5 -> PIPE_SWIZZLE_0 4, 1 5, X 0 .. W 3 */
	for (unsigned i = 0; i < 4; i++) {
		unsigned c = channels[i];
		if (c > 5)
			return -1;
		texture->swizzle[i] = c == 0 ? PIPE_SWIZZLE_0 : c == 1 ? PIPE_SWIZZLE_1 : (unsigned char)(c - 2);
	}
	return 0;
}

void
rwa_texture_destroy(struct rwa_texture *texture)
{
	GUARD();
	if (texture == NULL)
		return;
	store_unref(texture->store);
	free(texture);
}

static uint8_t *
texel_at(const struct rwa_texture *t, unsigned level, unsigned layer, unsigned x, unsigned y, unsigned z)
{
	return t->store->bo->cpu + t->offset + layer * t->layer_stride + t->level_offset[level] +
	       (uint64_t)z * t->row_bytes[level] * DIV_ROUND_UP(u_minify(t->shape.height, level), t->bh) +
	       (uint64_t)(y / t->bh) * t->row_bytes[level] + (uint64_t)(x / t->bw) * t->blocksize;
}

static int
region_ok(const struct rwa_texture *t, unsigned level, unsigned layer, const unsigned origin[3], const unsigned size[3],
	  char *why, size_t why_size)
{
	unsigned levels = t->shape.mip_levels ? t->shape.mip_levels : 1;
	if (level >= levels || layer >= t->layers || origin[0] + size[0] > u_minify(t->shape.width, level) ||
	    origin[1] + size[1] > u_minify(t->shape.height, level)) {
		say(why, why_size, "a region outside level %u, slice %u of the texture", level, layer);
		return -1;
	}
	return 0;
}

int
rwa_texture_write(struct rwa_texture *t, unsigned level, unsigned layer, const unsigned origin[3],
		  const unsigned size[3], const void *bytes, size_t stride, char *why, size_t why_size)
{
	GUARD();
	if (region_ok(t, level, layer, origin, size, why, why_size) != 0)
		return -1;
	t->store->writes++;
	snprintf(t->store->writer, sizeof(t->store->writer), "the processor (%ux%u at L%u)", size[0], size[1], level);
	store_sync(t->store);
	unsigned bx = DIV_ROUND_UP(size[0], t->bw), by = DIV_ROUND_UP(size[1], t->bh);
	size_t row = (size_t)bx * t->blocksize;
	unsigned depth = MAX2(size[2], 1);
	if (t->shape.pixel_format == 115 && t->format == PIPE_FORMAT_R8G8B8A8_UNORM) {
		/* rwn_f16as8=1: the caller's RGBA16Float, 8 bytes a texel, into RGBA8 */
		if (stride == 0)
			stride = (size_t)bx * 8;
		for (unsigned z = 0; z < depth; z++)
			for (unsigned y = 0; y < by; y++) {
				const uint16_t *from = (const uint16_t *)((const uint8_t *)bytes + ((size_t)z * by + y) * stride);
				uint8_t *to = texel_at(t, level, layer, origin[0], origin[1] + y, origin[2] + z);
				for (unsigned k = 0; k < bx * 4; k++)
					to[k] = (uint8_t)(CLAMP(_mesa_half_to_float(from[k]), 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		return 0;
	}
	if (stride == 0)
		stride = row;
	for (unsigned z = 0; z < depth; z++)
		for (unsigned y = 0; y < by; y++)
			memcpy(texel_at(t, level, layer, origin[0], origin[1] + y * t->bh, origin[2] + z),
			       (const uint8_t *)bytes + ((size_t)z * by + y) * stride, row);
	return 0;
}

int
rwa_texture_read(struct rwa_texture *t, unsigned level, unsigned layer, const unsigned origin[3],
		 const unsigned size[3], void *bytes, size_t stride, char *why, size_t why_size)
{
	GUARD();
	if (region_ok(t, level, layer, origin, size, why, why_size) != 0)
		return -1;
	store_sync(t->store);
	unsigned bx = DIV_ROUND_UP(size[0], t->bw), by = DIV_ROUND_UP(size[1], t->bh);
	size_t row = (size_t)bx * t->blocksize;
	unsigned depth = MAX2(size[2], 1);
	if (t->shape.pixel_format == 115 && t->format == PIPE_FORMAT_R8G8B8A8_UNORM) {
		/* rwn_f16as8=1: RGBA8 back to the caller's RGBA16Float */
		if (stride == 0)
			stride = (size_t)bx * 8;
		for (unsigned z = 0; z < depth; z++)
			for (unsigned y = 0; y < by; y++) {
				uint16_t *to = (uint16_t *)((uint8_t *)bytes + ((size_t)z * by + y) * stride);
				const uint8_t *from = texel_at(t, level, layer, origin[0], origin[1] + y, origin[2] + z);
				for (unsigned k = 0; k < bx * 4; k++)
					to[k] = _mesa_float_to_half(from[k] / 255.0f);
			}
		return 0;
	}
	if (stride == 0)
		stride = row;
	for (unsigned z = 0; z < depth; z++)
		for (unsigned y = 0; y < by; y++)
			memcpy((uint8_t *)bytes + ((size_t)z * by + y) * stride,
			       texel_at(t, level, layer, origin[0], origin[1] + y * t->bh, origin[2] + z), row);
	return 0;
}

int
rwa_texture_copy(struct rwa_texture *dst, unsigned dst_level, unsigned dst_slice, const unsigned dst_origin[3],
		 struct rwa_texture *src, unsigned src_level, unsigned src_slice, const unsigned src_origin[3],
		 const unsigned size[3], char *why, size_t why_size)
{
	GUARD();
	if (dst->blocksize != src->blocksize || dst->bw != src->bw) {
		say(why, why_size, "a copy between formats of different texel sizes");
		return -1;
	}
	dst->store->writes++;
	snprintf(dst->store->writer, sizeof(dst->store->writer), "a copy (%ux%u from %s %ux%u, last by %.20s)", size[0],
		 size[1], util_format_short_name(src->format), src->shape.width, src->shape.height,
		 src->store->writes ? src->store->writer : "nothing");
	if (region_ok(dst, dst_level, dst_slice, dst_origin, size, why, why_size) != 0 ||
	    region_ok(src, src_level, src_slice, src_origin, size, why, why_size) != 0)
		return -1;
	store_sync(src->store);
	store_sync(dst->store);
	unsigned bx = DIV_ROUND_UP(size[0], dst->bw), by = DIV_ROUND_UP(size[1], dst->bh);
	for (unsigned z = 0; z < MAX2(size[2], 1); z++)
		for (unsigned y = 0; y < by; y++)
			memmove(texel_at(dst, dst_level, dst_slice, dst_origin[0], dst_origin[1] + y * dst->bh, dst_origin[2] + z),
				texel_at(src, src_level, src_slice, src_origin[0], src_origin[1] + y * src->bh, src_origin[2] + z),
				(size_t)bx * dst->blocksize);
	return 0;
}

int
rwa_buffer_copy(struct rwa_buffer *dst, size_t dst_offset, struct rwa_buffer *src, size_t src_offset, size_t size,
		char *why, size_t why_size)
{
	GUARD();
	if (dst_offset + size > dst->size || src_offset + size > src->size) {
		say(why, why_size, "a copy past the end of a buffer");
		return -1;
	}
	store_sync(src->store);
	store_sync(dst->store);
	memmove(dst->store->bo->cpu + dst_offset, src->store->bo->cpu + src_offset, size);
	return 0;
}

int
rwa_buffer_fill(struct rwa_buffer *buffer, size_t offset, size_t size, uint8_t value, char *why, size_t why_size)
{
	GUARD();
	if (offset + size > buffer->size) {
		say(why, why_size, "a fill past the end of the buffer");
		return -1;
	}
	store_sync(buffer->store);
	memset(buffer->store->bo->cpu + offset, value, size);
	return 0;
}

/* -generateMipmapsForTexture: on the processor, a box filter of 8-bit-per-channel unsigned formats */
int
rwa_texture_mipmaps(struct rwa_texture *t, char *why, size_t why_size)
{
	GUARD();
	const struct util_format_description *d = util_format_description(t->format);
	unsigned levels = t->shape.mip_levels ? t->shape.mip_levels : 1;

	if (d->block.width != 1 || d->channel[0].size != 8 || d->channel[0].type != UTIL_FORMAT_TYPE_UNSIGNED) {
		say(why, why_size, "mipmaps of %s are not made here yet", util_format_short_name(t->format));
		return -1;
	}
	store_sync(t->store);
	for (unsigned layer = 0; layer < t->layers; layer++)
		for (unsigned l = 1; l < levels; l++) {
			unsigned w = u_minify(t->shape.width, l), h = u_minify(t->shape.height, l);
			unsigned pw = u_minify(t->shape.width, l - 1), ph = u_minify(t->shape.height, l - 1);
			for (unsigned y = 0; y < h; y++)
				for (unsigned x = 0; x < w; x++) {
					uint8_t *o = texel_at(t, l, layer, x, y, 0);
					for (unsigned c = 0; c < t->blocksize; c++) {
						unsigned x0 = MIN2(2 * x, pw - 1), x1 = MIN2(2 * x + 1, pw - 1);
						unsigned y0 = MIN2(2 * y, ph - 1), y1 = MIN2(2 * y + 1, ph - 1);
						unsigned s = texel_at(t, l - 1, layer, x0, y0, 0)[c] + texel_at(t, l - 1, layer, x1, y0, 0)[c] +
							     texel_at(t, l - 1, layer, x0, y1, 0)[c] + texel_at(t, l - 1, layer, x1, y1, 0)[c];
						o[c] = (uint8_t)((s + 2) / 4);
					}
				}
		}
	return 0;
}

/* --- samplers, depth states ---------------------------------------------------------------------------------------- */

struct rwa_sampler *
rwa_sampler_create(struct rwa_device *device, const struct rwa_sampler_state *state, char *why, size_t why_size)
{
	(void)device;
	struct rwa_sampler *s = calloc(1, sizeof(*s));

	if (s == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	s->state = *state;
	if (rwa_sampler_desc(s->desc, state, false, why, why_size) != 0 ||
	    rwa_sampler_desc(s->compare, state, true, why, why_size) != 0) {
		free(s);
		return NULL;
	}
	return s;
}

void
rwa_sampler_destroy(struct rwa_sampler *sampler)
{
	free(sampler);
}

struct rwa_depth_stencil *
rwa_depth_stencil_create(struct rwa_device *device, const struct rwa_depth_stencil_state *state, char *why,
			 size_t why_size)
{
	(void)device;
	struct rwa_depth_stencil *d = calloc(1, sizeof(*d));

	if (d == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	d->state = *state;
	return d;
}

void
rwa_depth_stencil_destroy(struct rwa_depth_stencil *state)
{
	free(state);
}

/* --- pipelines ----------------------------------------------------------------------------------------------------- */

#define RWA_MAX_CONSTANTS 256
static _Thread_local struct rwa_constant next_vconst[RWA_MAX_CONSTANTS], next_fconst[RWA_MAX_CONSTANTS];
static _Thread_local unsigned next_nvconst, next_nfconst;
static _Thread_local uint8_t next_vconfig[256], next_fconfig[256];
static _Thread_local unsigned next_vconfig_size, next_fconfig_size;

void
rwa_render_constants(const struct rwa_constant *vertex, unsigned vertex_count, const struct rwa_constant *fragment,
		     unsigned fragment_count)
{
	next_nvconst = MIN2(vertex_count, RWA_MAX_CONSTANTS);
	next_nfconst = MIN2(fragment_count, RWA_MAX_CONSTANTS);
	if (vertex)
		memcpy(next_vconst, vertex, next_nvconst * sizeof(*vertex));
	if (fragment)
		memcpy(next_fconst, fragment, next_nfconst * sizeof(*fragment));
}

void
rwa_render_config(const void *vertex, unsigned vertex_size, const void *fragment, unsigned fragment_size)
{
	next_vconfig_size = vertex ? MIN2(vertex_size, 256) : 0;
	next_fconfig_size = fragment ? MIN2(fragment_size, 256) : 0;
	if (next_vconfig_size)
		memcpy(next_vconfig, vertex, next_vconfig_size);
	if (next_fconfig_size)
		memcpy(next_fconfig, fragment, next_fconfig_size);
}

static void
count_stage(const struct rwa_stage *st, unsigned *buffers, unsigned *textures, unsigned *samplers, unsigned *argbufs)
{
	for (unsigned i = 0; i < st->air.num_resources; i++) {
		const struct air_resource *r = &st->air.resources[i];
		if (r->unused || r->argbuf_set != ~0u) {
			if (r->argbuf_set != ~0u && argbufs)
				*argbufs |= 1u << (r->argbuf_set & 31);
			continue;
		}
		if ((r->kind == AIR_RESOURCE_CONSTANT_BUFFER || r->kind == AIR_RESOURCE_DEVICE_BUFFER) && buffers)
			*buffers = MAX2(*buffers, r->index + 1);
		if (r->kind == AIR_RESOURCE_TEXTURE && textures)
			*textures = MAX2(*textures, r->index + 1);
		if (r->kind == AIR_RESOURCE_SAMPLER && !r->inline_sampler && samplers)
			*samplers = MAX2(*samplers, r->index + 1);
	}
}

struct rwa_render *
rwa_render_create(struct rwa_device *device, const void *vertex_bitcode, size_t vertex_size, const char *vertex_entry,
		  const void *fragment_bitcode, size_t fragment_size, const char *fragment_entry,
		  const struct rwa_render_shape *shape, struct rwa_render_info *info, char *why, size_t why_size)
{
	/* the constants and buffer 0 are this thread's, for this one call */
	const unsigned nv = next_nvconst, nf = next_nfconst, vcs = next_vconfig_size, fcs = next_fconfig_size;
	next_nvconst = next_nfconst = next_vconfig_size = next_fconfig_size = 0;
	if (shape == NULL || fragment_bitcode == NULL) {
		say(why, why_size, "a render pipeline needs an attachment description and a fragment function");
		return NULL;
	}
	if (vertex_bitcode == NULL) {
		say(why, why_size, "a pipeline with no vertex function (the generated full-screen stage) is not served here");
		return NULL;
	}
	uint64_t began = now_ns();
	struct rwa_render *r = calloc(1, sizeof(*r));
	if (r == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	r->device = device;
	r->shape = *shape;
	/* compiling is not under the lock: the window server builds pipelines on other threads while it draws */
	if (rwa_stage_compile(device, &r->fs, fragment_bitcode, fragment_size, fragment_entry, MESA_SHADER_FRAGMENT,
			      next_fconst, nf, fcs ? next_fconfig : NULL, fcs, shape, why, why_size) != 0 ||
	    (rwa_next_noperspective = r->fs.noperspective, false) ||
	    rwa_stage_compile(device, &r->vs, vertex_bitcode, vertex_size, vertex_entry, MESA_SHADER_VERTEX, next_vconst,
			      nv, vcs ? next_vconfig : NULL, vcs, shape, why, why_size) != 0 ||
	    rwa_render_prepare(r, why, why_size) != 0) {
		rwa_stage_free(&r->vs);
		rwa_stage_free(&r->fs);
		free(r);
		GUARD();
		stats.refused_pipelines++;
		note_refusal("a pipeline", why);
		return NULL;
	}
	struct rwa_render_info out = { 0 };
	count_stage(&r->vs, &out.vertex_buffers, NULL, NULL, &out.vertex_argbuf_mask);
	count_stage(&r->fs, &out.fragment_buffers, &out.fragment_textures, &out.fragment_samplers,
		    &out.fragment_argbuf_mask);
	out.fragment_writes_depth = r->fs_writes_depth;
	unsigned n = 0;
	for (unsigned i = 0; i < r->attr_count && n < 16; i++)
		out.vertex_attribute_index[n++] = r->vs.vs_attr[i];
	out.vertex_attribute_count = n;
	for (unsigned i = 0; i < r->attr_count; i++)
		if (r->attr[i].buffer != RWA_ATTR_ZERO)
			out.vertex_stream_mask |= 1u << r->attr[i].buffer;
	/* the slots rwa_draw.vertex_streams must cover: the highest buffer index fetched from, plus one (air_metal.h) */
	out.vertex_streams = util_last_bit(out.vertex_stream_mask);
	for (unsigned i = 0; i < r->rt_count && i < r->shape.color_blend_count; i++) {
		if (r->shape.color_blend[i].blending_enabled)
			out.blend_enabled_mask |= 1u << i;
		unsigned m = r->shape.color_blend[i].write_mask;
		unsigned g = ((m >> 3) & 1) | ((m >> 1) & 2) | ((m << 1) & 4) | ((m << 3) & 8);
		out.color_write_masks |= g << (4 * i);
	}
	r->info = out;
	if (info)
		*info = out;
	GUARD();
	stats.pipelines++;
	stats.pipeline_ns += now_ns() - began;
	sec_pipe_note();
	return r;
}

void rwa_render_destroy(struct rwa_render *render);

void
rwa_render_outputs(const struct rwa_render *render, unsigned *writes, unsigned *fetches)
{
	if (writes)
		*writes = render ? render->fs_writes : 0;
	if (fetches)
		*fetches = render ? render->fs_fetches : 0;
}

/* --- batches ------------------------------------------------------------------------------------------------------- */

#define CHUNK (256u << 10)

struct ptr {
	uint8_t *cpu;
	uint64_t gpu;
};

struct region {
	const char *name;
	uint64_t gpu;
	uint32_t size;
};

struct batch {
	/* what the frame's descriptors are, by name: a refused fragment job's fault pointer is told against them */
	struct region region[24];
	unsigned nregion;
	unsigned discard;   /* attachments whose store action is DontCare: not written back */
	unsigned prof_group;   /* boot-arg rwn_profile=1: 1 + which draws a full pass of several targets skips */
	struct area_rec *areas;   /* rwn_profile=2: where each draw's positions land, read once its vertex job is done */
	unsigned nareas;
	bool chunks_given;        /* its chunks handed to the GPU thread, which gives them back once it has read them */
	unsigned dontcare;  /* attachments whose load action is DontCare: the tile starts at zero, and a tile nothing is
			     * drawn into is not written (no clean writes), as a tiler leaves it */
	struct km_bo *chunks[512];
	/* each chunk's descriptors written in the processor's cached memory first and copied to the chunk whole at the
	 * submit (pool_upload): the buffers are write-combined (HakiosMali.cpp createBO), and a draw packed byte by byte
	 * into them, and read back from them, took ~250 us (run 536). NULL: a big one, written in place */
	uint8_t *shadow[512];
	uint32_t fill[512];
	uint32_t up[512];   /* how much of each chunk is already in the GPU's memory (pool_upload, by parts: vt_flush) */
	unsigned nchunks;
	uint64_t chunk_used;
	/* rwn_vtsplit: the vertex and tiler jobs already sent (vt_flush), and the thread storage they were sent with */
	unsigned vt_parts;
	bool tls_frozen;
	unsigned tls_frozen_size;
	/* what only the GPU writes -- the vertex jobs' positions, varyings and point sizes -- in chunks of its own with
	 * no shadow (pool_gpu), not copied at the submit */
	uint8_t *gpu_cpu;
	uint64_t gpu_gpu, gpu_used;
	struct km_bo **bos;
	unsigned nbos, bos_room;
	uint32_t mark;
	struct store **stores;
	unsigned nstores, stores_room;
	struct rwa_render **renders;
	unsigned nrenders, renders_room;

	/* the targets */
	struct rwa_texture *color[8];
	unsigned color_count;
	struct rwa_texture *depth;
	unsigned width, height, samples;
	unsigned level[8];          /* the mip level each colour target is drawn into (rwa_draw.color_level) */
	bool clear[8], clear_z, clear_s;
	uint32_t clear_words[8][4];
	float z_clear;
	uint8_t s_clear;

	/* the job chain */
	uint64_t first_job;
	uint8_t *last_header;       /* to link the next job to */
	unsigned job_index, last_tiler;
	unsigned draws;
	struct ptr tiler_ctx, tls;
	unsigned tls_size;
	unsigned minx, miny, maxx, maxy;   /* what the draws touched, inclusive pixels */
	char sampled[200];                 /* the textures the draws sample, for a refused pass's line */
	char samplers[120];
	unsigned tex_flags;
	unsigned raw_variant;        /* a pass loading several targets: 0 one frame shader, 1 two (8-bit, raw), 2 raw cleared */
	unsigned drawn_load;         /* targets loaded by the batch's first draw instead of a frame shader */
	unsigned nsampled;
};

static struct batch *open_batch;
static uint32_t batch_marks;

/* every fragment job in a ring of its own, so its status can be read once it is done and a pass the GPU refused can
 * be named: what its targets were, which were loaded, its size */
#define RING 128
static struct km_bo *ring;
static struct {
	uint64_t seq;
	char what[400];
	unsigned flags;
	struct region region[24];
	unsigned nregion;
} ring_slot[RING];

static void
note(struct batch *b, const char *name, uint64_t gpu, uint64_t size)
{
	if (b->nregion < ARRAY_SIZE(b->region) && gpu != 0)
		b->region[b->nregion++] = (struct region){ name, gpu, (uint32_t)size };
}
/* what a pass has, for counting which of them the GPU refuses */
enum { F_LOAD_RAW = 1, F_LOAD_BLEND = 2, F_TEX_MIP = 4, F_TEX_F16 = 8, F_CLEAR_ONLY = 16, F_RAW_TARGET = 32,
       F_TEX_SURFACE = 64, F_RAW_A = 128, F_RAW_B = 256, F_RAW_C = 512, NFLAGS = 10 };
static const char *const flag_names[NFLAGS] = { "load-raw", "load-8bit", "tex-mip", "tex-f16", "clear-only",
						"raw-target", "tex-surface", "multi-frame-shader", "v1-comps", "v2-rawclear" };
static uint64_t flag_total[NFLAGS], flag_failed[NFLAGS];
static unsigned ring_next;
static uint64_t frag_failed, frag_checked;

static const char *
job_status_name(uint32_t s)
{
	switch (s & 0xff) {
	case 0x01: return "DONE";
	case 0x40: return "JOB_CONFIG_FAULT";
	case 0x42: return "JOB_READ_FAULT";
	case 0x43: return "JOB_WRITE_FAULT";
	case 0x58: return "DATA_INVALID_FAULT";
	case 0x59: return "TILE_RANGE_FAULT";
	case 0x5a: return "ADDR_RANGE_FAULT";
	case 0x60: return "OOM";
	case 0x00: return "NOT_STARTED";
	default: return "fault";
	}
}

/* the status of slot I's job, once done; its slot free for the next */
static void
ring_check(unsigned i, bool wait)
{
	if (ring_slot[i].seq == 0)
		return;
	if (km_wait(ring_slot[i].seq, wait ? 1000000000 : 0) != 0)
		return;
	uint32_t status = 0;
	memcpy(&status, ring->cpu + i * 256, 4);
	frag_checked++;
	if ((status & 0xff) == 1 && strstr(ring_slot[i].what, " r16g16") != NULL) {
		static unsigned good;
		if (good++ < 6)
			keep("fragment job DONE: %.330s", ring_slot[i].what);
	}
	for (unsigned f = 0; f < NFLAGS; f++)
		if (ring_slot[i].flags & (1u << f)) {
			flag_total[f]++;
			if ((status & 0xff) != 1)
				flag_failed[f]++;
		}
	if ((status & 0xff) != 1) {
		frag_failed++;
		static unsigned said;
		if (said++ < 12) {
			/* the header: exception status, first incomplete task, fault pointer */
			uint32_t task = 0;
			uint64_t fault = 0;
			memcpy(&task, ring->cpu + i * 256 + 4, 4);
			memcpy(&fault, ring->cpu + i * 256 + 8, 8);
			char at[96] = "in nothing named";
			for (unsigned r = 0; r < ring_slot[i].nregion; r++) {
				const struct region *g = &ring_slot[i].region[r];
				if (fault >= g->gpu && fault < g->gpu + g->size) {
					snprintf(at, sizeof(at), "%s+%#llx of %#x", g->name, (unsigned long long)(fault - g->gpu),
						 g->size);
					break;
				}
			}
			keep("fragment job %s (%#x) task %#x fault %#llx %s", job_status_name(status), status, task,
			     (unsigned long long)fault, at);
			char list[300];
			int n = 0;
			list[0] = 0;
			for (unsigned r = 0; r < ring_slot[i].nregion && n < (int)sizeof(list) - 40; r++)
				n += snprintf(list + n, sizeof(list) - n, " %s=%llx/%x", ring_slot[i].region[r].name,
					      (unsigned long long)ring_slot[i].region[r].gpu, ring_slot[i].region[r].size);
			keep("  regions:%s", list);
			const char *z = strstr(ring_slot[i].what, " z");
			keep("  targets%.170s", z ? z : "?");
			keep("  %.180s", ring_slot[i].what);
		}
	}
	ring_slot[i].seq = 0;
}

static uint8_t *shadow_free[64];
static unsigned shadow_nfree;
static uint64_t upload_ns, upload_bytes;
static uint64_t commits;   /* command buffers committed (rwa_flush_seq) */

/* the GPU's time: a thread of its own waits for each pass's fragment job in turn and counts the time from its submit (or
 * the pass before's end, if later) to its end as busy -- how much of the 30 seconds the GPU worked, and a pass's
 * submit-to-done time */
#define GQ 1024
static struct {
	uint64_t vseq, seq, submitted;
	unsigned kind, draws;
	struct area_rec *areas;
	unsigned nareas;
	struct km_bo **chunks;   /* given back by the GPU thread, once it has read the areas */
	unsigned nchunks;
} gq[GQ];
static unsigned gq_head, gq_tail;
/* each second of the 30 (from sec_base, seconds of the library): command buffers committed and pipelines made -- an
 * application's opening, which CoreAnimation animates by the clock, seen second by second (under gq_lock) */
static uint16_t sec_frames[32], sec_pipes[32];
static uint64_t sec_base;
static pthread_mutex_t gq_lock = PTHREAD_MUTEX_INITIALIZER;

/* a pipeline made, counted in its second (sec_pipes) */
static void
sec_pipe_note(void)
{
	uint64_t sec = (now_ns() - lib_up_ns) / 1000000000ull;
	pthread_mutex_lock(&gq_lock);
	if (sec >= sec_base && sec - sec_base < 32 && sec_pipes[sec - sec_base] < 0xffff)
		sec_pipes[sec - sec_base]++;
	pthread_mutex_unlock(&gq_lock);
}
static pthread_cond_t gq_cond = PTHREAD_COND_INITIALIZER;
static uint64_t gpu_busy_ns, gpu_latency_ns, gpu_passes, gpu_last_done, gpu_latency_max;
/* by kind (width >= 700, >= 300, >= 100, smaller; several targets; loaded): passes, draws, vertex+tiler and fragment
 * time */
static uint64_t kind_passes[16], kind_draws[16], kind_vt_ns[16], kind_frag_ns[16];
/* the passes that clear nothing and so are bounded by their draws (submit_batch): how many, their tiles, of how many */
static uint64_t bound_passes, bound_tiles, bound_of;
static uint64_t prof_passes[4], prof_draws[4], prof_ns[4];   /* rwn_profile=1: by group skipped */
/*
 * Whether the window server draws a frame while the GPU is still at the one before, or only once it is done: the
 * first draw after a frame handed to the display (under gq_lock), and over the frames in motion (under 80 ms after
 * the one before) how many began with passes still on the GPU, how many after it went idle and how long after, and
 * the time from the first draw to the display
 */
static uint64_t tl_begin, tl_begin_idle;   /* the first draw's time; 0 or how long the GPU had been idle then */
static bool tl_begin_busy;
static uint64_t tl_frames, tl_busy, tl_idle, tl_idle_ns, tl_span_ns;
/* the time between one command buffer committed and the next, in 60 Hz refreshes: 1, 2, 3, 4, 5, 6 and more */
static uint64_t frame_gap[7];
static bool gq_running;

static void *
gpu_watch(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&gq_lock);
		while (gq_head == gq_tail)
			pthread_cond_wait(&gq_cond, &gq_lock);
		uint64_t seq = gq[gq_tail % GQ].seq, submitted = gq[gq_tail % GQ].submitted, vseq = gq[gq_tail % GQ].vseq;
		unsigned kind = gq[gq_tail % GQ].kind & 15, draws = gq[gq_tail % GQ].draws;
		unsigned group = (gq[gq_tail % GQ].kind >> 4) & 7;
		struct area_rec *areas = gq[gq_tail % GQ].areas;
		unsigned nareas = gq[gq_tail % GQ].nareas;
		struct km_bo **chunks = gq[gq_tail % GQ].chunks;
		unsigned nchunks = gq[gq_tail % GQ].nchunks;
		gq[gq_tail % GQ].areas = NULL;
		gq[gq_tail % GQ].chunks = NULL;
		pthread_mutex_unlock(&gq_lock);
		uint64_t from0 = MAX2(submitted, gpu_last_done), vdone = from0;
		if (vseq) {
			km_wait(vseq, 2000000000);
			vdone = now_ns();
		}
		if (areas) {
			/* the positions are there once the vertex job is done, and the buffers are not reused before
			 * the fragment job is */
			static float v[1024 * 4];
			for (unsigned i = 0; i < nareas; i++) {
				const struct area_rec *a = &areas[i];
				/* each instance's own box (run 545 boxed several instances' rectangles together) */
				size_t n = MIN2((size_t)a->stride * a->instances, (size_t)1024);
				memcpy(v, a->pos, n * 16);
				double px = 0;
				for (unsigned in = 0; in < a->instances; in++) {
					float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
					for (unsigned k = 0; k < a->count && (size_t)in * a->stride + k < n; k++) {
						const float *q = &v[4 * ((size_t)in * a->stride + k)];
						x0 = MIN2(x0, q[0]);
						x1 = MAX2(x1, q[0]);
						y0 = MIN2(y0, q[1]);
						y1 = MAX2(y1, q[1]);
					}
					x0 = MAX2(x0, a->clip[0]);
					y0 = MAX2(y0, a->clip[1]);
					x1 = MIN2(x1, a->clip[2] + 1.0f);
					y1 = MIN2(y1, a->clip[3] + 1.0f);
					if (x1 > x0 && y1 > y0)
						px += (double)(x1 - x0) * (y1 - y0);
				}
				/* the instances not read counted as the read ones' mean */
				if (a->all_instances > a->instances && a->instances)
					px *= (double)a->all_instances / a->instances;
				if (i == 0)
					memcpy(area_sample, v, sizeof(area_sample));
				if (a->say) {
					static unsigned said;
					if (said++ < 40) {
						float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
						for (size_t k = 0; k < n; k++) {
							bx0 = MIN2(bx0, v[4 * k]);
							bx1 = MAX2(bx1, v[4 * k]);
							by0 = MIN2(by0, v[4 * k + 1]);
							by1 = MAX2(by1, v[4 * k + 1]);
						}
						keep("small pass positions %ux%u %.20s: %zu vertices in %.0f,%.0f..%.0f,%.0f; first %g %g %g %g, "
						     "%g %g %g %g", a->target[0], a->target[1], a->name, n, bx0, by0, bx1, by1, v[0], v[1], v[2],
						     v[3], v[4], v[5], v[6], v[7]);
					}
				}
				pthread_mutex_lock(&gq_lock);
				unsigned t = 0;
				while (t < narea_tab && strcmp(area_tab[t].name, a->name) != 0)
					t++;
				if (t == narea_tab && narea_tab < ARRAY_SIZE(area_tab))
					snprintf(area_tab[narea_tab++].name, sizeof(area_tab[0].name), "%s", a->name);
				if (t == narea_tab - 1 && area_tab[t].draws == 0) {
					area_tab[t].pixels = 0;
					area_tab[t].instances = 0;
				}
				if (t < narea_tab) {
					area_tab[t].draws++;
					area_tab[t].instances += a->all_instances;
					area_tab[t].pixels += px;
				}
				pthread_mutex_unlock(&gq_lock);
			}
			free(areas);
		}
		for (unsigned i = 0; chunks && i < nchunks; i++)
			km_bo_release(chunks[i], seq);
		free(chunks);
		km_wait(seq, 2000000000);
		uint64_t done = now_ns();
		pthread_mutex_lock(&gq_lock);
		kind_passes[kind]++;
		kind_draws[kind] += draws;
		if (group >= 1 && group <= 4) {
			prof_passes[group - 1]++;
			prof_draws[group - 1] += draws;
			if (done > MAX2(vdone, from0))
				prof_ns[group - 1] += done - from0;
		}
		if (vdone > from0)
			kind_vt_ns[kind] += vdone - from0;
		if (done > MAX2(vdone, from0))
			kind_frag_ns[kind] += done - MAX2(vdone, from0);
		uint64_t from = MAX2(submitted, gpu_last_done);
		if (done > from)
			gpu_busy_ns += done - from;
		gpu_latency_ns += done - submitted;
		gpu_latency_max = MAX2(gpu_latency_max, done - submitted);
		gpu_passes++;
		gpu_last_done = done;
		gq_tail++;
		pthread_mutex_unlock(&gq_lock);
	}
	return NULL;
}

static void
gpu_watch_push(uint64_t vseq, uint64_t seq, uint64_t submitted, unsigned kind, unsigned draws, struct area_rec *areas,
	       unsigned nareas, struct km_bo **chunks, unsigned nchunks)
{
	pthread_mutex_lock(&gq_lock);
	if (!gq_running) {
		pthread_t t;
		gq_running = pthread_create(&t, NULL, gpu_watch, NULL) == 0;
		if (gq_running)
			pthread_detach(t);
	}
	if (gq_running && gq_head - gq_tail < GQ) {
		gq[gq_head % GQ].seq = seq;
		gq[gq_head % GQ].vseq = vseq;
		gq[gq_head % GQ].submitted = submitted;
		gq[gq_head % GQ].kind = kind;
		gq[gq_head % GQ].draws = draws;
		gq[gq_head % GQ].areas = areas;
		gq[gq_head % GQ].nareas = nareas;
		gq[gq_head % GQ].chunks = chunks;
		gq[gq_head % GQ].nchunks = nchunks;
		areas = NULL;
		chunks = NULL;
		gq_head++;
		pthread_cond_signal(&gq_cond);
	}
	pthread_mutex_unlock(&gq_lock);
	free(areas);   /* not queued */
	for (unsigned i = 0; chunks && i < nchunks; i++)
		km_bo_release(chunks[i], seq);
	free(chunks);
}
static uint64_t ubo_copied, ubo_copied_bytes, ubo_given;   /* uniform buffers off 16 bytes: copied, or given as they are */
static uint64_t surface_passes;   /* passes into an IOSurface (the window server's frames), for the 30-second lines */

static uint8_t *
shadow_get(void)
{
	if (shadow_nfree)
		return shadow_free[--shadow_nfree];
	return malloc(CHUNK);   /* NULL: the chunk is written in place */
}

static void
shadow_put(uint8_t *sh)
{
	if (sh == NULL)
		return;
	if (shadow_nfree < ARRAY_SIZE(shadow_free))
		shadow_free[shadow_nfree++] = sh;
	else
		free(sh);
}

/* a buffer of its own for SIZE bytes, written in place (no shadow, not copied at the submit): what is bigger than a
 * chunk, and the threads' stack, which only the GPU writes */
static struct ptr
pool_big(struct batch *b, size_t size, char *why, size_t why_size)
{
	struct ptr p = { NULL, 0 };

	if (b->nchunks >= ARRAY_SIZE(b->chunks)) {
		say(why, why_size, "a batch's descriptors outgrew %u chunks", (unsigned)ARRAY_SIZE(b->chunks));
		return p;
	}
	struct km_bo *big = km_bo_new(size, why, why_size);
	if (big == NULL)
		return p;
	/* the big one before the current chunk, which stays the one allocated from */
	if (b->nchunks > 0) {
		b->chunks[b->nchunks] = b->chunks[b->nchunks - 1];
		b->shadow[b->nchunks] = b->shadow[b->nchunks - 1];
		b->fill[b->nchunks] = b->fill[b->nchunks - 1];
		b->up[b->nchunks] = b->up[b->nchunks - 1];
		b->chunks[b->nchunks - 1] = big;
		b->shadow[b->nchunks - 1] = NULL;
		b->fill[b->nchunks - 1] = 0;
		b->up[b->nchunks - 1] = 0;
	} else {
		b->chunks[0] = big;
		b->shadow[0] = NULL;
		b->fill[0] = 0;
		b->up[0] = 0;
		b->chunk_used = CHUNK;   /* no small chunk yet */
	}
	b->nchunks++;
	p.cpu = big->cpu;
	p.gpu = big->gpu;
	return p;
}

/*
 * Memory only the GPU writes: the vertex jobs' positions, varyings and point sizes. Until run 576 they came from the
 * shadowed chunks and were copied, garbage as they are, to the GPU's memory at every submit -- with the path
 * rasterizer's ~19000 instances of 4 vertices a frame, most of the 500 to 800 KB copied a pass (run 573). Chunks of
 * their own now (pool_big's, written in place). Boot-arg rwn_gpupool=0: from the shadowed chunks as before.
 */
static struct ptr pool_alloc(struct batch *b, size_t size, size_t align, char *why, size_t why_size);

static struct ptr
pool_gpu(struct batch *b, size_t size, size_t align, char *why, size_t why_size)
{
	static int off = -1;
	if (off < 0)
		off = boot_arg("rwn_gpupool=0");
	if (off)
		return pool_alloc(b, size, align, why, why_size);
	if (size > CHUNK)
		return pool_big(b, size, why, why_size);
	uint64_t at = ALIGN_POT(b->gpu_used, align);
	if (b->gpu_cpu == NULL || at + size > CHUNK) {
		struct ptr c = pool_big(b, CHUNK, why, why_size);
		if (c.cpu == NULL)
			return c;
		b->gpu_cpu = c.cpu;
		b->gpu_gpu = c.gpu;
		at = 0;
	}
	b->gpu_used = at + size;
	struct ptr p = { b->gpu_cpu + at, b->gpu_gpu + at };
	return p;
}

static struct ptr
pool_alloc(struct batch *b, size_t size, size_t align, char *why, size_t why_size)
{
	struct ptr p = { NULL, 0 };
	uint64_t at = ALIGN_POT(b->chunk_used, align);

	if (size > CHUNK)
		return pool_big(b, size, why, why_size);
	if (b->nchunks == 0 || at + size > CHUNK || b->chunks[b->nchunks - 1]->size < CHUNK) {
		if (b->nchunks >= ARRAY_SIZE(b->chunks)) {
			say(why, why_size, "a batch's descriptors outgrew %u chunks", (unsigned)ARRAY_SIZE(b->chunks));
			return p;
		}
		struct km_bo *c = km_bo_new(CHUNK, why, why_size);
		if (c == NULL)
			return p;
		b->shadow[b->nchunks] = shadow_get();
		b->fill[b->nchunks] = 0;
		b->up[b->nchunks] = 0;
		b->chunks[b->nchunks++] = c;
		at = 0;
	}
	struct km_bo *c = b->chunks[b->nchunks - 1];
	uint8_t *sh = b->shadow[b->nchunks - 1];
	p.cpu = (sh ? sh : c->cpu) + at;
	p.gpu = c->gpu + at;
	b->chunk_used = at + size;
	b->fill[b->nchunks - 1] = (uint32_t)(at + size);
	return p;
}

/* the batch's descriptors to the GPU's memory, each chunk as far as it was filled, in one copy each -- from where the
 * last upload stopped: a pass whose vertex and tiler jobs went in parts (vt_flush) is uploaded in parts, and what went
 * already is not copied again (the tiler context among it, which the GPU writes to) */
static void
pool_upload(struct batch *b)
{
	uint64_t t = now_ns();
	for (unsigned i = 0; i < b->nchunks; i++)
		if (b->shadow[i] && b->fill[i] > b->up[i]) {
			memcpy(b->chunks[i]->cpu + b->up[i], b->shadow[i] + b->up[i], b->fill[i] - b->up[i]);
			upload_bytes += b->fill[i] - b->up[i];
			b->up[i] = b->fill[i];
		}
	upload_ns += now_ns() - t;
}

/* where the GPU's own copy of pool memory CPU is, to read what the GPU wrote there */
static uint8_t *
pool_real(struct batch *b, uint8_t *cpu)
{
	for (unsigned i = 0; i < b->nchunks; i++)
		if (b->shadow[i] && cpu >= b->shadow[i] && cpu < b->shadow[i] + CHUNK)
			return b->chunks[i]->cpu + (cpu - b->shadow[i]);
	return cpu;
}

static void
use_bo(struct batch *b, struct km_bo *bo)
{
	if (bo == NULL || bo->mark == b->mark)
		return;
	if (b->nbos == b->bos_room) {
		b->bos_room = b->bos_room ? 2 * b->bos_room : 64;
		b->bos = realloc(b->bos, b->bos_room * sizeof(*b->bos));
	}
	bo->mark = b->mark;
	b->bos[b->nbos++] = bo;
}

static void
use_store(struct batch *b, struct store *s)
{
	if (s == NULL)
		return;
	use_bo(b, s->bo);
	if (s->pending == b)
		return;
	s->pending = b;
	s->refs++;      /* held until the batch is submitted */
	if (b->nstores == b->stores_room) {
		b->stores_room = b->stores_room ? 2 * b->stores_room : 32;
		b->stores = realloc(b->stores, b->stores_room * sizeof(*b->stores));
	}
	b->stores[b->nstores++] = s;
}

static void
use_render(struct batch *b, struct rwa_render *r)
{
	for (unsigned i = 0; i < b->nrenders; i++)
		if (b->renders[i] == r)
			return;
	if (b->nrenders == b->renders_room) {
		b->renders_room = b->renders_room ? 2 * b->renders_room : 16;
		b->renders = realloc(b->renders, b->renders_room * sizeof(*b->renders));
	}
	b->renders[b->nrenders++] = r;
	use_bo(b, r->vs.code);
	use_bo(b, r->fs.code);
}

/*
 * The tiler's levels (bins of 16 << n pixels) for a frame. pan_select_tiler_hierarchy_mask takes the coarsest ones that
 * cover the frame when the GPU has fewer active levels than that, and this G52 has two (TILER_FEATURES 0x2xx): 512 and
 * 1024 pixels for the window server's 750x1624, every tile then walking every primitive of its 512-pixel bin. Run 548:
 * the full pass of the swiped home screen took 40-44 ms so, 21-22 with the 32-pixel level beside the frame-covering one
 * (16: 22, 64: 21-22), the path rasterizer's ~23000 small primitives a frame binned where they are. So: the level that
 * covers the frame, the 32-pixel one, and any more the GPU has from the coarse end down. Boot-arg rwn_hier=panfrost:
 * Panfrost's.
 */
static unsigned
hierarchy_mask(unsigned width, unsigned height)
{
	unsigned max_wh = MAX2(width, height);
	unsigned last = util_last_bit((max_wh + 15) / 16);
	unsigned mask = BITFIELD_MASK(tiler_levels);

	if (last <= tiler_levels)
		return mask;
	if (boot_arg("rwn_hier=panfrost"))
		return mask << (last - tiler_levels);
	unsigned top = last - 1;
	mask = 1u << top;
	unsigned left = tiler_levels - 1;
	if (left && top > 1) {
		mask |= 1u << 1;
		left--;
	}
	for (unsigned l = top - 1; left && l > 1; l--, left--)
		mask |= 1u << l;
	return mask;
}

static struct batch *
batch_new(struct rwa_texture *const *color, const unsigned *level, unsigned color_count, struct rwa_texture *depth,
	  char *why, size_t why_size)
{
	struct batch *b = calloc(1, sizeof(*b));

	if (b == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	b->mark = ++batch_marks;
	b->color_count = color_count;
	unsigned w = 0, h = 0;
	for (unsigned i = 0; i < color_count; i++) {
		b->color[i] = color[i];
		b->level[i] = level ? level[i] : 0;
		if (color[i]) {
			/* the pass is the level's size, the smallest attachment's when they differ (Metal's render area) */
			unsigned lw = u_minify(color[i]->shape.width, b->level[i]), lh = u_minify(color[i]->shape.height, b->level[i]);
			w = w ? MIN2(w, lw) : lw;
			h = h ? MIN2(h, lh) : lh;
		}
	}
	b->depth = depth;
	if (depth) {
		w = w ? MIN2(w, depth->shape.width) : depth->shape.width;
		h = h ? MIN2(h, depth->shape.height) : depth->shape.height;
	}
	b->width = MAX2(w, 1);
	b->height = MAX2(h, 1);
	b->samples = 1;
	b->minx = b->miny = ~0u;
	b->maxx = b->maxy = 0;
	struct rwa_device *d = the_device;
	/* the tiler context and its heap (jm_emit_tiler_desc), the thread storage filled at the submit */
	b->tiler_ctx = pool_alloc(b, pan_size(TILER_CONTEXT) + pan_size(TILER_HEAP), 64, why, why_size);
	b->tls = pool_alloc(b, pan_size(LOCAL_STORAGE), 64, why, why_size);
	if (b->tiler_ctx.cpu == NULL || b->tls.cpu == NULL) {
		free(b);
		return NULL;
	}
	uint64_t heap_desc = b->tiler_ctx.gpu + pan_size(TILER_CONTEXT);
	pan_cast_and_pack((b->tiler_ctx.cpu + pan_size(TILER_CONTEXT)), TILER_HEAP, heap) {
		heap.size = d->tiler_heap->size;
		heap.base = d->tiler_heap->gpu;
		heap.bottom = d->tiler_heap->gpu;
		heap.top = d->tiler_heap->gpu + d->tiler_heap->size;
	}
	/* boot-arg rwn_profile=3: from 150 to 300 s after the library is up, full-screen passes take, frame by frame in turn,
	 * Panfrost's hierarchy (the two coarsest levels that cover the frame: this G52 has two active levels,
	 * TILER_FEATURES 0x2xx) or the frame-covering one with a fine one, 16, 32 or 64 pixels (run 547: the path
	 * rasterizer's ~23000 small primitives a frame are ~25 ms of a ~41 ms pass) */
	unsigned hmask = hierarchy_mask(b->width, b->height);
	if (b->width >= 700 && boot_arg("rwn_profile=3") && now_ns() - lib_up_ns > 150000000000ull &&
	    now_ns() - lib_up_ns < 300000000000ull && tiler_levels == 2) {
		b->prof_group = 1 + (unsigned)(commits % 4);
		unsigned top = util_last_bit(hmask) - 1;
		if (b->prof_group >= 2)
			hmask = (1u << top) | (1u << (b->prof_group - 2));
	}
	pan_cast_and_pack((b->tiler_ctx.cpu), TILER_CONTEXT, tiler) {
		tiler.hierarchy_mask = hmask;
		tiler.fb_width = b->width;
		tiler.fb_height = b->height;
		tiler.heap = heap_desc;
		tiler.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
	}
	use_bo(b, d->tiler_heap);
	use_bo(b, d->sample_positions);
	use_bo(b, d->dummy);
	for (unsigned i = 0; i < color_count; i++)
		if (color[i])
			use_store(b, color[i]->store);
	if (depth)
		use_store(b, depth->store);
	return b;
}

static bool
same_targets(const struct batch *b, struct rwa_texture *const *color, const unsigned *level, unsigned color_count,
	     struct rwa_texture *depth)
{
	if (b->color_count != color_count || b->depth != depth)
		return false;
	for (unsigned i = 0; i < color_count; i++) {
		struct rwa_texture *a = b->color[i], *c = color[i];
		if (b->level[i] != (level ? level[i] : 0))
			return false;
		if (a == c)
			continue;
		/* two texture objects over the same memory and layout (a view, or the same surface made twice) */
		if (a == NULL || c == NULL || a->store != c->store || a->offset != c->offset || a->format != c->format)
			return false;
	}
	return true;
}

static void
batch_free(struct batch *b, uint64_t seq)
{
	for (unsigned i = 0; i < b->nchunks; i++) {
		if (!b->chunks_given)
			km_bo_release(b->chunks[i], seq);
		shadow_put(b->shadow[i]);
	}
	for (unsigned i = 0; i < b->nstores; i++) {
		struct store *s = b->stores[i];
		if (s->pending == b)
			s->pending = NULL;
		if (seq > s->last_seq)
			s->last_seq = seq;
		store_unref(s);
	}
	free(b->stores);
	free(b->bos);
	free(b->renders);
	free(b->areas);
	free(b);
}

/* --- the frame: frame descriptor, render targets, the frame shader, the fragment job ------------------------------- */

/* runs 528..: a pass that loads a raw target tries one of three ways in turn (the kept lines count each) */
static unsigned raw_variant_next;

static int
emit_preload_dcd(struct batch *b, void *dcd, unsigned rt_mask, char *why, size_t why_size)
{
	struct rwa_device *d = the_device;
	struct rwa_stage *ps = &d->preload[rt_mask & 0xff];

	/* the frame shader of these targets, made the first time a pass loads them */
	if (ps->code == NULL && rwa_preload_shader(d, ps, rt_mask & 0xff, why, why_size) != 0)
		return -1;
	unsigned n = util_bitcount(rt_mask);
	unsigned nblend = MAX2(b->color_count, 1);
	struct ptr rsd = pool_alloc(b, pan_size(RENDERER_STATE) + nblend * pan_size(BLEND), 64, why, why_size);
	/* the descriptors, then each target's surface on 64 bytes of its own (as every texture payload is) */
	struct ptr tex = pool_alloc(b, ALIGN_POT(n * pan_size(TEXTURE), 64) + n * 64, 64, why, why_size);
	struct ptr smp = pool_alloc(b, pan_size(SAMPLER), 64, why, why_size);
	struct ptr var = pool_alloc(b, pan_size(ATTRIBUTE) + 2 * pan_size(ATTRIBUTE_BUFFER), 64, why, why_size);
	struct ptr crd = pool_alloc(b, 64, 64, why, why_size);   /* an attribute buffer's pointer is 64-byte aligned */
	struct ptr vp = pool_alloc(b, pan_size(VIEWPORT), 64, why, why_size);
	if (!rsd.cpu || !tex.cpu || !smp.cpu || !var.cpu || !vp.cpu || !crd.cpu)
		return -1;
	note(b, "fs.rsd", rsd.gpu, pan_size(RENDERER_STATE) + nblend * pan_size(BLEND));
	note(b, "fs.tex", tex.gpu, ALIGN_POT(n * pan_size(TEXTURE), 64) + n * 64);
	note(b, "fs.smp", smp.gpu, pan_size(SAMPLER));
	note(b, "fs.var", var.gpu, pan_size(ATTRIBUTE) + 2 * pan_size(ATTRIBUTE_BUFFER));
	note(b, "fs.crd", crd.gpu, 64);
	note(b, "fs.vp", vp.gpu, pan_size(VIEWPORT));
	note(b, "fs.code", ps->code->gpu, ps->code->size);
	memcpy(rsd.cpu, ps->rsd, 64);
	for (unsigned i = 0; i < nblend; i++) {
		void *bd = rsd.cpu + pan_size(RENDERER_STATE) + i * pan_size(BLEND);
		if (rt_mask & (1u << i))
			rwa_preload_blend(d, b->color[i]->format, i, bd,
					  b->raw_variant == 1 && !rwa_rt_blendable(b->color[i]->format));
		else
			pan_cast_and_pack((bd), BLEND, cfg) {
				cfg.enable = false;
				cfg.internal.mode = MALI_BLEND_MODE_OFF;
			}
	}
	/* the targets as textures of their level, in target order: the coordinates are pixels (an unnormalized sampler) */
	const unsigned char id[4] = { PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W };
	unsigned k = 0;
	for (unsigned rt = 0; rt < 8; rt++) {
		if (!(rt_mask & (1u << rt)))
			continue;
		struct rwa_texture *t = b->color[rt];
		uint8_t *desc = tex.cpu + k * pan_size(TEXTURE);
		uint8_t *surf = tex.cpu + ALIGN_POT(n * pan_size(TEXTURE), 64) + k * 64;
		uint64_t surf_gpu = tex.gpu + ALIGN_POT(n * pan_size(TEXTURE), 64) + k * 64;
		unsigned l = b->level[rt];
		rwa_surface_desc(surf, t->store->bo->gpu + t->offset + t->level_offset[l], (int32_t)t->row_bytes[l], 0);
		rwa_texture_desc(desc, t->format, id, MALI_TEXTURE_DIMENSION_2D, u_minify(t->shape.width, l),
				 u_minify(t->shape.height, l), 1, 1, 1, 1, surf_gpu);
		k++;
	}
	pan_cast_and_pack((smp.cpu), SAMPLER, cfg) {
		cfg.seamless_cube_map = false;
		cfg.normalized_coordinates = false;
		cfg.minify_nearest = true;
		cfg.magnify_nearest = true;
	}
	/* the rectangle, the frame's extent, as positions and as the varying (pan_preload_fb_part) */
	unsigned minx = 0, miny = 0, maxx = b->width - 1, maxy = b->height - 1;
	float *coords = (float *)crd.cpu;
	uint64_t coords_gpu = crd.gpu;
	const float rect[16] = { minx, miny, 0, 1, maxx + 1, miny, 0, 1, minx, maxy + 1, 0, 1, maxx + 1, maxy + 1, 0, 1 };
	memcpy(coords, rect, sizeof(rect));
	/* the two attribute buffers first: an Attribute Buffer is 32-byte aligned (v7.xml), and at var + 8, after the
	 * Attribute, every frame shader's draw descriptor was refused -- DATA_INVALID at dcds+0, every load of runs 526
	 * to 534 */
	uint8_t *abuf_cpu = var.cpu;
	uint64_t abuf_gpu = var.gpu;
	uint8_t *attr_cpu = var.cpu + 2 * pan_size(ATTRIBUTE_BUFFER);
	uint64_t attr_gpu = var.gpu + 2 * pan_size(ATTRIBUTE_BUFFER);
	pan_cast_and_pack((attr_cpu), ATTRIBUTE, cfg) {
		cfg.buffer_index = 0;
		cfg.offset_enable = false;
		cfg.format = GENX(panfrost_format_from_pipe_format)(PIPE_FORMAT_R32G32B32_FLOAT)->hw;
	}
	pan_cast_and_pack((abuf_cpu), ATTRIBUTE_BUFFER, cfg) {
		cfg.pointer = coords_gpu;
		cfg.stride = 4 * sizeof(float);
		cfg.size = cfg.stride * 4;
	}
	pan_cast_and_pack((abuf_cpu + pan_size(ATTRIBUTE_BUFFER)), ATTRIBUTE_BUFFER, cfg);
	pan_cast_and_pack((vp.cpu), VIEWPORT, cfg) {
		cfg.scissor_minimum_x = minx;
		cfg.scissor_minimum_y = miny;
		cfg.scissor_maximum_x = maxx;
		cfg.scissor_maximum_y = maxy;
		cfg.minimum_z = 0.0f;
		cfg.maximum_z = 1.0f;
	}
	pan_cast_and_pack((dcd), DRAW, cfg) {
		cfg.thread_storage = b->tls.gpu;
		cfg.state = rsd.gpu;
		cfg.position = coords_gpu;
		cfg.viewport = vp.gpu;
		cfg.varyings = attr_gpu;
		cfg.varying_buffers = abuf_gpu;
		cfg.textures = tex.gpu;
		cfg.samplers = smp.gpu;
		cfg.clean_fragment_write = true;
	}
	use_bo(b, ps->code);
	stats.preloads++;
	return 0;
}

static uint64_t
emit_framebuffer(struct batch *b, char *why, size_t why_size)
{
	struct rwa_device *d = the_device;
	unsigned rt_count = MAX2(b->color_count, 1);
	bool zs = b->depth != NULL;
	uint32_t internal[8], writeback[8], swz[8];
	unsigned bpp[8], sum = 0;

	for (unsigned i = 0; i < b->color_count; i++) {
		bpp[i] = 0;
		if (b->color[i] == NULL)
			continue;
		if (!rwa_rt_format(b->color[i]->format, &internal[i], &writeback[i], &swz[i], &bpp[i])) {
			say(why, why_size, "target %u: %s is not a format rendered into here", i,
			    util_format_short_name(b->color[i]->format));
			return 0;
		}
		sum += bpp[i];
	}
	/* GENX(pan_select_tile_size) */
	unsigned tile_size = d->tile_budget >> util_logbase2_ceil(MAX2(sum, 1));
	tile_size = MIN2(tile_size, panfrost_max_effective_tile_size(7));
	unsigned cbuf_allocation = ALIGN_POT(MAX2(sum, 1) * tile_size, 1024);

	unsigned load_mask = 0;
	for (unsigned i = 0; i < b->color_count; i++)
		if (b->color[i] && !b->clear[i])
			load_mask |= 1u << i;
	if (zs && !b->clear_z) {
		say(why, why_size, "a depth attachment that is loaded, not cleared, is not served yet");
		return 0;
	}
	/* run 529: a pass that loads several targets with a raw one among them takes one of three ways in turn */
	unsigned raw_mask = 0;
	for (unsigned i = 0; i < b->color_count; i++)
		if ((load_mask & (1u << i)) && !rwa_rt_blendable(b->color[i]->format))
			raw_mask |= 1u << i;
	/* runs 528 to 534 tried other ways for raw targets (b->raw_variant 1: the format's channels, 2: cleared); what
	 * was refused was the frame shader's attribute buffers' alignment, whatever it loaded */
	b->raw_variant = 0;
	(void)raw_variant_next;
	load_mask &= ~b->drawn_load;   /* those the first draw loaded */
	if (b->raw_variant == 2) {
		/* C: the raw targets cleared to zero instead of loaded */
		for (unsigned i = 0; i < b->color_count; i++)
			if (raw_mask & (1u << i)) {
				static const float zero[4] = { 0, 0, 0, 0 };
				b->clear[i] = true;
				rwa_pack_clear(b->color[i]->format, zero, b->clear_words[i]);
			}
		load_mask &= ~raw_mask;
	}
	unsigned mode1 = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
	struct ptr dcds = { NULL, 0 };
	if (load_mask) {
		dcds = pool_alloc(b, 3 * pan_size(DRAW), 64, why, why_size);
		if (dcds.cpu == NULL)
			return 0;
		memset(dcds.cpu, 0, 3 * pan_size(DRAW));
		unsigned first = load_mask;
		unsigned second = 0;
		if (first && emit_preload_dcd(b, dcds.cpu, first, why, why_size) != 0)
			return 0;
		if (second) {
			/* B: the raw targets by a second frame shader, in the pre-frame 1 slot */
			if (emit_preload_dcd(b, dcds.cpu + pan_size(DRAW), second, why, why_size) != 0)
				return 0;
			mode1 = MALI_PRE_POST_FRAME_SHADER_MODE_INTERSECT;
		}
		if (!first)
			load_mask = 0;   /* (only the second) */
	}
	size_t size = pan_size(FRAMEBUFFER) + (zs ? pan_size(ZS_CRC_EXTENSION) : 0) + rt_count * pan_size(RENDER_TARGET);
	struct ptr fb = pool_alloc(b, size, 64, why, why_size);
	if (fb.cpu == NULL)
		return 0;
	note(b, "fbd", fb.gpu, size);
	if (dcds.gpu)
		note(b, "dcds", dcds.gpu, 3 * pan_size(DRAW));
	note(b, "tiler", b->tiler_ctx.gpu, pan_size(TILER_CONTEXT) + pan_size(TILER_HEAP));
	note(b, "tls", b->tls.gpu, pan_size(LOCAL_STORAGE));
	note(b, "heap", d->tiler_heap->gpu, d->tiler_heap->size);
	note(b, "samplepos", d->sample_positions->gpu, d->sample_positions->size);
	for (unsigned i = 0; i < b->color_count; i++)
		if (b->color[i]) {
			static const char *const rtn[8] = { "rt0", "rt1", "rt2", "rt3", "rt4", "rt5", "rt6", "rt7" };
			struct rwa_texture *t = b->color[i];
			note(b, rtn[i], t->store->bo->gpu + t->offset, t->store->bo->size - t->offset);
		}
	memset(fb.cpu, 0, size);
	pan_section_pack(fb.cpu, FRAMEBUFFER, PARAMETERS, cfg) {
		cfg.sample_locations = d->sample_positions->gpu;
		/* run 533: every other frame of several targets that loads runs its frame shader ALWAYS (TSIX-2033: a
		 * forced clean write breaks INTERSECT readbacks), to tell the two apart */
		cfg.pre_frame_0 = !load_mask ? MALI_PRE_POST_FRAME_SHADER_MODE_NEVER : MALI_PRE_POST_FRAME_SHADER_MODE_INTERSECT;
		cfg.pre_frame_1 = mode1;
		cfg.post_frame = MALI_PRE_POST_FRAME_SHADER_MODE_NEVER;
		cfg.frame_shader_dcds = dcds.gpu;
		cfg.tiler = b->tiler_ctx.gpu;
		cfg.width = b->width;
		cfg.height = b->height;
		cfg.bound_min_x = 0;
		cfg.bound_min_y = 0;
		cfg.bound_max_x = b->width - 1;
		cfg.bound_max_y = b->height - 1;
		cfg.effective_tile_size = tile_size;
		cfg.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
		cfg.render_target_count = rt_count;
		cfg.z_internal_format = MALI_Z_INTERNAL_FORMAT_D24;
		if (zs) {
			switch (b->depth->format) {
			case PIPE_FORMAT_Z16_UNORM: cfg.z_internal_format = MALI_Z_INTERNAL_FORMAT_D16; break;
			case PIPE_FORMAT_Z32_FLOAT: cfg.z_internal_format = MALI_Z_INTERNAL_FORMAT_D32; break;
			default: break;
			}
			cfg.z_write_enable = util_format_has_depth(util_format_description(b->depth->format));
			cfg.s_write_enable = util_format_has_stencil(util_format_description(b->depth->format));
		}
		cfg.z_clear = b->z_clear;
		cfg.s_clear = b->s_clear;
		cfg.color_buffer_allocation = cbuf_allocation;
		cfg.sample_count = 1;
		cfg.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
		cfg.has_zs_crc_extension = zs;
	}
	pan_section_pack(fb.cpu, FRAMEBUFFER, PADDING, padding) {
		(void)padding;
	}
	uint8_t *rts = fb.cpu + pan_size(FRAMEBUFFER);
	if (zs) {
		struct rwa_texture *t = b->depth;
		pan_cast_and_pack((rts), ZS_CRC_EXTENSION, cfg) {
			cfg.zs_clean_pixel_write_enable = b->clear_z;
			cfg.zs_block_format = MALI_BLOCK_FORMAT_LINEAR;
			cfg.zs_writeback_base = t->store->bo->gpu + t->offset;
			cfg.zs_writeback_row_stride = t->row_bytes[0];
			cfg.zs_writeback_surface_stride = 0;
			switch (t->format) {
			case PIPE_FORMAT_Z16_UNORM: cfg.zs_write_format = MALI_ZS_FORMAT_D16; break;
			case PIPE_FORMAT_Z32_FLOAT: cfg.zs_write_format = MALI_ZS_FORMAT_D32; break;
			case PIPE_FORMAT_S8_UINT:
				cfg.s_write_format = MALI_S_FORMAT_S8;
				cfg.s_block_format = MALI_BLOCK_FORMAT_LINEAR;
				cfg.s_writeback_base = t->store->bo->gpu + t->offset;
				cfg.s_writeback_row_stride = t->row_bytes[0];
				break;
			default: break;
			}
		}
		rts += pan_size(ZS_CRC_EXTENSION);
	}
	unsigned cbuf_offset = 0;
	for (unsigned i = 0; i < rt_count; i++) {
		struct rwa_texture *t = i < b->color_count ? b->color[i] : NULL;
		pan_cast_and_pack((rts + i * pan_size(RENDER_TARGET)), RENDER_TARGET, cfg) {
			cfg.internal_buffer_offset = cbuf_offset;
			if (t == NULL) {
				cfg.internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
				cfg.writeback_block_format = MALI_BLOCK_FORMAT_TILED_U_INTERLEAVED;
				cfg.dithering_enable = true;
				continue;
			}
			/* run 540: a DontCare attachment cleared whole, every tile written, lost what the window server
			 * meant to keep outside what it drew (the Weather widget's blue): such tiles are left alone */
			cfg.clean_pixel_write_enable = b->clear[i] && !(b->dontcare & (1u << i));
			if (b->clear[i]) {
				cfg.clear.color_0 = b->clear_words[i][0];
				cfg.clear.color_1 = b->clear_words[i][1];
				cfg.clear.color_2 = b->clear_words[i][2];
				cfg.clear.color_3 = b->clear_words[i][3];
			}
			/* MTLStoreActionDontCare: the tile buffer is not written back (its contents are undefined after the
			 * pass, and nothing reads them) */
			cfg.write_enable = !(b->discard & (1u << i));
			cfg.dithering_enable = true;
			cfg.writeback_msaa = MALI_MSAA_SINGLE;
			cfg.srgb = util_format_is_srgb(t->format);
			cfg.internal_format = internal[i];
			cfg.writeback_format = writeback[i];
			cfg.swizzle = swz[i];
			cfg.writeback_block_format = MALI_BLOCK_FORMAT_LINEAR;
			cfg.rgb.base = t->store->bo->gpu + t->offset + t->level_offset[b->level[i]];
			cfg.rgb.row_stride = t->row_bytes[b->level[i]];
			cfg.rgb.surface_stride = 0;
		}
		if (t)
			cbuf_offset += bpp[i] * tile_size;
	}
	struct mali_framebuffer_pointer_packed tag;
	pan_pack(&tag, FRAMEBUFFER_POINTER, cfg) {
		cfg.zs_crc_extension_present = zs;
		cfg.render_target_count = rt_count;
	}
	return fb.gpu | tag.opaque[0];
}

/* the thread storage: the draws' stack (panfrost_batch_get_scratchpad, pan_emit_tls), as big as the draws so far ask */
static int
tls_pack(struct batch *b, char *why, size_t why_size)
{
	struct ptr scratch = { NULL, 0 };
	if (b->tls_size) {
		unsigned total = panfrost_get_total_stack_size(b->tls_size, thread_tls_alloc ? thread_tls_alloc : 256,
							       core_id_range);
		scratch = pool_big(b, total, why, why_size);   /* only the GPU writes it: not copied */
		if (scratch.cpu == NULL)
			return -1;
	}
	pan_cast_and_pack((b->tls.cpu), LOCAL_STORAGE, cfg) {
		if (b->tls_size) {
			cfg.tls_size = panfrost_get_stack_shift(b->tls_size);
			cfg.tls_base_pointer = scratch.gpu;
		}
		cfg.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
	}
	return 0;
}

/*
 * Boot-arg rwn_vtsplit=N: a full-screen pass of several targets sends its vertex and tiler jobs every N draws, while
 * the rest is still being recorded, instead of all at its submit. The window server's frame is recorded at -commit
 * (6.6 ms, run 575) and its full pass is the last, so its ~3 ms of vertex and tiler work came after all of that; run
 * 575 also showed every frame in motion begins only once the GPU is done with the one before, so what the GPU does
 * after the commit is the frame's time. The parts go one after another on the kernel's one queue, the tiler context
 * and its heap carry on in memory from one part to the next, and the thread storage goes with the first part as big
 * as the draws so far ask (a later draw that asks more takes a descriptor of its own: draw).
 */
static int
vt_flush(struct batch *b, char *why, size_t why_size)
{
	if (b->first_job == 0)
		return 0;
	if (!b->tls_frozen) {
		if (tls_pack(b, why, why_size) != 0)
			return -1;
		b->tls_frozen = true;
		b->tls_frozen_size = b->tls_size;
	}
	for (unsigned i = 0; i < b->nchunks; i++)
		use_bo(b, b->chunks[i]);
	pool_upload(b);
	uint64_t seq = 0;
	int e = km_submit(b->first_job, false, b->bos, b->nbos, &seq);
	if (e != 0) {
		say(why, why_size, "a part of the vertex and tiler jobs was not taken: %s", strerror(e));
		return -1;
	}
	b->first_job = 0;
	b->last_header = NULL;
	b->job_index = 0;
	b->last_tiler = 0;
	b->vt_parts++;
	stats.vt_parts++;
	return 0;
}

static int
submit_batch(struct batch *b, uint64_t *seq_out, char *why, size_t why_size)
{
	struct rwa_device *d = the_device;
	uint64_t began = now_ns();

	if (!b->tls_frozen && tls_pack(b, why, why_size) != 0)
		return -1;
	uint64_t fbd = emit_framebuffer(b, why, why_size);
	if (fbd == 0)
		return -1;
	if (ring == NULL) {
		ring = km_bo_new(RING * 256, why, why_size);
		if (ring == NULL)
			return -1;
	}
	unsigned slot = ring_next++ % RING;
	ring_check(slot, true);
	struct ptr frag = { ring->cpu + slot * 256, ring->gpu + slot * 256 };
	memset(frag.cpu, 0, 256);
	{
		unsigned fl = 0;
		bool any_clear = false;
		for (unsigned i = 0; i < b->color_count; i++) {
			if (b->color[i] == NULL)
				continue;
			bool raw = !rwa_rt_blendable(b->color[i]->format);
			if (raw)
				fl |= F_RAW_TARGET;
			if (!b->clear[i])
				fl |= raw ? F_LOAD_RAW : F_LOAD_BLEND;
			else
				any_clear = true;
		}
		if (any_clear && !(fl & (F_LOAD_RAW | F_LOAD_BLEND)) && b->draws == 0)
			fl |= F_CLEAR_ONLY;
		fl |= b->tex_flags;
		if (b->raw_variant == 1)
			fl |= F_RAW_B;
		if (b->raw_variant == 2 && (fl & F_LOAD_BLEND))
			fl |= F_RAW_C;

		else if (util_bitcount(fl & (F_LOAD_RAW | F_LOAD_BLEND)) && b->color_count > 1)
			fl |= F_RAW_A;
		ring_slot[slot].flags = fl;
		memcpy(ring_slot[slot].region, b->region, sizeof(b->region));
		for (unsigned i = 0; i < b->color_count; i++)
			if (b->color[i]) {
				struct store *ts = b->color[i]->store;
				ts->writes++;
				snprintf(ts->writer, sizeof(ts->writer), "%.28s %ud %s L%u%s", b->nrenders ? b->renders[0]->fs.name :
					 "(no draw)", b->draws, b->clear[i] ? "clr" : "ld", b->level[i],
					 (b->discard >> i) & 1 ? " nostore" : "");
			}
		for (unsigned i = 0; i < b->color_count; i++)
			if (b->color[i]) {
				if (b->discard & (1u << i))
					stats.att_discarded++;
				else
					stats.att_stored++;
			}
		ring_slot[slot].nregion = b->nregion;
	}
	{
		char *w = ring_slot[slot].what;
		size_t room = sizeof(ring_slot[slot].what);
		int n = snprintf(w, room, "tex%s; smp%s; %ux%u %ud z%s:", b->sampled, b->samplers, b->width, b->height,
				 b->draws, b->depth ? util_format_short_name(b->depth->format) : "-");
		for (unsigned i = 0; i < b->color_count && n < (int)room - 24; i++)
			n += snprintf(w + n, room - n, " %.14s%s@%u/%ux%u", b->color[i] ? util_format_short_name(b->color[i]->format) : "none",
				      b->color[i] && !b->clear[i] ? "(L)" : "", b->level[i],
				      b->color[i] ? b->color[i]->shape.width : 0, b->color[i] ? b->color[i]->shape.height : 0);
		for (unsigned i = 0; i < b->nrenders && i < 3 && n < (int)room - 40; i++)
			n += snprintf(w + n, room - n, "%s %.24s%s", i ? "," : ";", b->renders[i]->fs.name,
				      b->renders[i]->blend_shader_mask ? "[bs]" : "");
	}
	/*
	 * The tiles the fragment job goes through. A pass that clears nothing -- every target loaded, no depth -- leaves
	 * what its draws do not touch as it was, so only the tiles under the union of its draws' scissors are gone
	 * through (Panfrost's batch extent: panfrost_batch_union_scissor per draw, the whole frame for a clear); the
	 * window server's passes that load its targets redraw what changed. Off unless boot-arg rwn_bound=1: run 574
	 * bounded 600 to 760 passes in 30 s and went through 90 to 96% of their tiles all the same, and it is the only
	 * run with a two-minute stall in the swipes, compute jobs faulting near address 0 and a strip of noise at the
	 * left edge of the second home page -- not tied to this, but nothing here is worth that doubt.
	 */
	unsigned tx0 = 0, ty0 = 0, tx1 = (b->width - 1) >> MALI_TILE_SHIFT, ty1 = (b->height - 1) >> MALI_TILE_SHIFT;
	{
		/* a target whose load action is DontCare is cleared where the job goes, and what it holds elsewhere is
		 * undefined anyway (run 572: with that counted as a clear no pass was bounded) */
		bool cleared = b->depth != NULL;
		for (unsigned i = 0; i < b->color_count; i++)
			cleared |= b->color[i] && b->clear[i] && !(b->dontcare & (1u << i));
		static int full = -1;
		if (full < 0)
			full = !boot_arg("rwn_bound=1");
		if (!cleared && !full && b->draws && b->minx <= b->maxx && b->miny <= b->maxy) {
			unsigned of = (tx1 + 1) * (ty1 + 1);
			tx0 = b->minx >> MALI_TILE_SHIFT;
			ty0 = b->miny >> MALI_TILE_SHIFT;
			tx1 = MIN2(tx1, b->maxx >> MALI_TILE_SHIFT);
			ty1 = MIN2(ty1, b->maxy >> MALI_TILE_SHIFT);
			__atomic_fetch_add(&bound_passes, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&bound_tiles, (tx1 - tx0 + 1) * (ty1 - ty0 + 1), __ATOMIC_RELAXED);
			__atomic_fetch_add(&bound_of, of, __ATOMIC_RELAXED);
		}
	}
	pan_section_pack(frag.cpu, FRAGMENT_JOB, PAYLOAD, payload) {
		payload.bound_min_x = tx0;
		payload.bound_min_y = ty0;
		payload.bound_max_x = tx1;
		payload.bound_max_y = ty1;
		payload.framebuffer = fbd;
	}
	pan_section_pack(frag.cpu, FRAGMENT_JOB, HEADER, header) {
		header.type = MALI_JOB_TYPE_FRAGMENT;
		header.index = 1;
	}
	for (unsigned i = 0; i < b->nchunks; i++)
		use_bo(b, b->chunks[i]);
	use_bo(b, ring);
	pool_upload(b);
	if (b->color_count && b->color[0] && b->color[0]->store->surface_id)
		surface_passes++;
	uint64_t seq = 0, vseq = 0, submitted = now_ns();
	int e = 0;
	if (b->first_job) {
		e = km_submit(b->first_job, false, b->bos, b->nbos, &seq);
		vseq = seq;
		if (e != 0) {
			say(why, why_size, "the vertex and tiler jobs were not taken: %s", strerror(e));
			return -1;
		}
	}
	e = km_submit(frag.gpu, true, b->bos, b->nbos, &seq);
	if (e != 0) {
		say(why, why_size, "the fragment job was not taken: %s", strerror(e));
		return -1;
	}
	*seq_out = seq;
	{
		/* the pass's kind for the GPU's time by kind: its size, its targets, its draws */
		unsigned bytes = 0;
		for (unsigned i = 0; i < b->color_count; i++)
			if (b->color[i])
				bytes += util_format_get_blocksize(b->color[i]->format);
		unsigned kind = (b->width >= 700 ? 0 : b->width >= 300 ? 1 : b->width >= 100 ? 2 : 3) * 4 +
				(b->color_count > 1 ? 2 : 0) + ((ring_slot[slot].flags & (F_LOAD_RAW | F_LOAD_BLEND)) ? 1 : 0);
		(void)bytes;
		/* run 544: the GPU thread read a position buffer the cache had already closed (it closes the oldest past
		 * 48 MB, done or not): a batch whose positions it reads gives it its chunks to give back afterwards */
		struct km_bo **given = NULL;
		if (b->areas && (given = malloc(b->nchunks * sizeof(*given))) != NULL) {
			memcpy(given, b->chunks, b->nchunks * sizeof(*given));
			b->chunks_given = true;
		} else if (b->areas) {
			free(b->areas);
			b->areas = NULL;
			b->nareas = 0;
		}
		gpu_watch_push(vseq, seq, submitted, kind | (b->prof_group << 4), b->draws, b->areas, b->nareas, given,
			       b->chunks_given ? b->nchunks : 0);
		b->areas = NULL;   /* the watcher's now */
		b->nareas = 0;
	}
	ring_slot[slot].seq = seq;
	use_bo(b, ring);
	for (unsigned i = 0; i < RING; i++)
		if (i != slot)
			ring_check(i, false);
	stats.batches++;
	stats.flush_ns += now_ns() - began;
	if (boot_arg("mali_native_sync=1"))
		km_wait(seq, 2000000000);
	return 0;
}

static void
flush_batch(const char *reason)
{
	struct batch *b = open_batch;
	char why[300];
	uint64_t seq = 0;

	(void)reason;
	if (b == NULL)
		return;
	open_batch = NULL;
	bool any_clear = b->clear_z;
	for (unsigned i = 0; i < b->color_count; i++)
		any_clear |= b->clear[i];
	if (b->draws == 0 && !any_clear) {
		batch_free(b, 0);
		return;
	}
	if (submit_batch(b, &seq, why, sizeof(why)) != 0) {
		note_refusal("a pass", why);
		seq = km_last_seq();
	}
	batch_free(b, seq);
	report();
}

static void pack_invocation(void *out, unsigned vertex_count, unsigned instance_count);
static void link_job(struct batch *b, uint8_t *header_cpu, uint64_t gpu);

/* the targets in MASK loaded by a draw of their own, the batch's first: a tiler job whose draw descriptor is the frame
 * shader's (its positions are already the pass's pixels, so no vertex job), drawn as a strip over the whole pass.
 * Run 529: a pre-frame DCD in a frame of several targets was refused whatever it loaded; draws into such frames
 * finish. Sampling the targets here reads their memory as it was: the tiles are written back at the pass's end. */
static int
emit_preload_draw(struct batch *b, unsigned mask, char *why, size_t why_size)
{
	struct ptr job = pool_alloc(b, pan_size(TILER_JOB), 64, why, why_size);
	if (job.cpu == NULL)
		return -1;
	memset(job.cpu, 0, pan_size(TILER_JOB));
	if (emit_preload_dcd(b, pan_section_ptr(job.cpu, TILER_JOB, DRAW), mask, why, why_size) != 0)
		return -1;
	pack_invocation(pan_section_ptr(job.cpu, TILER_JOB, INVOCATION), 4, 1);
	pan_section_pack(job.cpu, TILER_JOB, PRIMITIVE, cfg) {
		cfg.draw_mode = MALI_DRAW_MODE_TRIANGLE_STRIP;
		cfg.first_provoking_vertex = true;
		cfg.job_task_split = 6;
		cfg.index_count = 4;
		cfg.index_type = MALI_INDEX_TYPE_NONE;
	}
	pan_section_pack(job.cpu, TILER_JOB, PRIMITIVE_SIZE, cfg) {
		cfg.constant = 1.0f;
	}
	pan_section_pack(job.cpu, TILER_JOB, TILER, cfg) {
		cfg.address = b->tiler_ctx.gpu;
	}
	pan_section_pack(job.cpu, TILER_JOB, PADDING, cfg) {
		(void)cfg;
	}
	unsigned index = ++b->job_index;
	pan_section_pack(job.cpu, TILER_JOB, HEADER, cfg) {
		cfg.type = MALI_JOB_TYPE_TILER;
		cfg.index = index;
		cfg.dependency_2 = b->last_tiler;
	}
	link_job(b, job.cpu, job.gpu);
	b->last_tiler = index;
	b->drawn_load |= mask;
	return 0;
}

static struct batch *
batch_for(struct rwa_texture *const *color, const unsigned *level, unsigned color_count, struct rwa_texture *depth,
	  const float *clear_color,
	  const float *clear_depth, const unsigned *clear_stencil, const struct rwa_draw *dr, char *why, size_t why_size)
{
	/* each attachment's own load and store actions (rwa_draw.color_actions; boot-arg rwn_actions=0: clear_color
	 * alone, as before) */
	const bool actions = dr && dr->color_actions && !boot_arg("rwn_actions=0");
	bool begins = clear_color != NULL;
	if (actions) {
		begins = false;
		for (unsigned i = 0; i < color_count && i < 8; i++)
			if (color[i] && (dr->color_load[i] == 2 || (dr->color_load[i] == 0 && !boot_arg("rwn_dontcare=load"))))
				begins = true;
	}
	if (open_batch && (!same_targets(open_batch, color, level, color_count, depth) || begins || clear_depth ||
			   clear_stencil))
		flush_batch("another pass");
	if (open_batch == NULL) {
		open_batch = batch_new(color, level, color_count, depth, why, why_size);
		if (open_batch == NULL)
			return NULL;
		struct batch *b = open_batch;
		for (unsigned i = 0; i < color_count; i++) {
			if (color[i] == NULL)
				continue;
			if (actions && i < 8) {
				/* Clear: its own colour; DontCare: whatever is cheapest, and a clear costs the tile nothing
				 * where a load reads the whole attachment */
				/* DontCare is cleared (its tiles start at zero, untouched ones not written): runs 540 and 541
				 * lost the Weather widget's blue doing so, but what lost it was the generic vertex stage's
				 * sliding varyings (c26f18f) -- run 569 cleared DontCare with the widgets whole, ~8% less
				 * fragment work idle, 16.4 frames a second swiping. Boot-arg rwn_dontcare=load: loaded */
				if (dr->color_load[i] != 2 && !(dr->color_load[i] == 0 && !boot_arg("rwn_dontcare=load"))) {
					if (dr->color_load[i] == 1)
						stats.att_loaded++;
					else
						stats.att_dontcare_load++;
					continue;
				}
				static const float zero[4] = { 0, 0, 0, 0 };
				b->clear[i] = true;
				rwa_pack_clear(color[i]->format, dr->color_load[i] == 2 ? dr->color_clear[i] : zero,
					       b->clear_words[i]);
				if (dr->color_load[i] == 2) {
					stats.att_cleared++;
				} else {
					stats.att_dontcare_load++;
					b->dontcare |= 1u << i;
				}
				continue;
			}
			if (clear_color == NULL)
				continue;
			b->clear[i] = true;
			rwa_pack_clear(color[i]->format, clear_color, b->clear_words[i]);
		}
		if (depth) {
			b->clear_z = clear_depth != NULL || clear_stencil != NULL;
			b->z_clear = clear_depth ? *clear_depth : 1.0f;
			b->s_clear = clear_stencil ? (uint8_t)*clear_stencil : 0;
		}
		if (clear_color || begins)
			stats.clears++;
		/* A frame of several targets loads them by its frame shader, as a frame of one does. Runs 526 to 530 had every
		 * such frame refused: the frame shader's renderer state named as many samplers as textures, and there is one
		 * (fixed in rwa_preload_shader). Boot-arg rwn_multiload=1 loads them by a first draw instead
		 * (emit_preload_draw); rwn_multiclear=1 clears them to transparent black, as runs 531.. did. */
		unsigned load = 0;
		for (unsigned i = 0; i < color_count; i++)
			if (color[i] && !b->clear[i])
				load |= 1u << i;
		if (color_count > 1 && load && boot_arg("rwn_multiload=1")) {
			if (emit_preload_draw(b, load, why, why_size) != 0) {
				batch_free(b, 0);
				open_batch = NULL;
				return NULL;
			}
		} else if (color_count > 1 && load && boot_arg("rwn_multiclear=1")) {
			static const float zero[4] = { 0, 0, 0, 0 };
			for (unsigned i = 0; i < color_count; i++)
				if (load & (1u << i)) {
					b->clear[i] = true;
					rwa_pack_clear(color[i]->format, zero, b->clear_words[i]);
				}
			stats.multiload_cleared++;
		}
	}
	/* the store actions: the pass's last draw's (one Metal pass, or passes merged into one batch, the last says) */
	if (open_batch && actions)
		for (unsigned i = 0; i < color_count && i < 8; i++) {
			if (dr->color_store[i] == 0)
				open_batch->discard |= 1u << i;
			else
				open_batch->discard &= ~(1u << i);
		}
	return open_batch;
}

int
rwa_render_clear(struct rwa_device *device, struct rwa_texture *const *color, unsigned color_count,
		 const float *clear_color, struct rwa_texture *depth, const float *clear_depth,
		 const unsigned *clear_stencil, char *why, size_t why_size)
{
	GUARD();
	(void)device;
	if (color_count > 8) {
		say(why, why_size, "%u colour attachments", color_count);
		return -1;
	}
	static const float black[4] = { 0, 0, 0, 0 };
	struct batch *b = batch_for(color, NULL, color_count, depth, clear_color ? clear_color : black, clear_depth,
				    clear_stencil, NULL, why, why_size);
	return b ? 0 : -1;
}

/*
 * The texture probe (on unless boot-arg rwn_texprobe=0): a texture a draw samples, 16x16 or bigger, is looked at every
 * 5 s at most -- 16 texels of level 0 on a 4x4 grid, once the GPU is done with it. All of them one colour is said: white
 * (an icon or the glass drawn as a white square), zero (nothing drawn there), or another colour; and LOST when a texture
 * that once held more than one colour holds one. Each texture is said once, 40 lines a process at most; the 30-second
 * lines count them. ~3 us a probe (16 uncached reads).
 */
static uint64_t probe_count, probe_white, probe_zero, probe_flat, probe_lost;

static void
tex_probe(struct batch *b, const struct rwa_stage *st, struct rwa_texture *t)
{
	static int on = -1;
	if (on < 0)
		on = !boot_arg("rwn_texprobe=0");
	uint64_t now = now_ns();
	if (!on || t->shape.width < 16 || t->shape.height < 16 || t->bw != 1 || t->bh != 1 || t->blocksize > 16 ||
	    now - t->probe_ns < 5000000000ull)
		return;
	struct store *s = t->store;
	if (s->pending != NULL || (s->last_seq && km_wait(s->last_seq, 0) != 0))
		return;   /* still being drawn: another time */
	t->probe_ns = now;
	probe_count++;
	uint8_t first[16], texel[16];
	bool flat = true;
	const uint8_t *base = s->bo->cpu + t->offset;
	for (unsigned i = 0; i < 16; i++) {
		unsigned x = (2 * (i % 4) + 1) * t->shape.width / 8, y = (2 * (i / 4) + 1) * t->shape.height / 8;
		memcpy(i ? texel : first, base + (size_t)y * t->row_bytes[0] + (size_t)x * t->blocksize, t->blocksize);
		if (i && memcmp(texel, first, t->blocksize) != 0)
			flat = false;
	}
	if (!flat) {
		t->probe_varied = true;
		return;
	}
	bool white = true, zero = true;
	for (unsigned k = 0; k < t->blocksize; k++) {
		zero &= first[k] == 0;
		white &= first[k] == 0xff;
	}
	if (t->format == PIPE_FORMAT_R16G16B16A16_FLOAT)
		white = first[1] == 0x3c && first[3] == 0x3c && first[5] == 0x3c && first[7] == 0x3c && !first[0] &&
			!first[2] && !first[4] && !first[6];   /* 1.0 in all four */
	const char *what = t->probe_varied ? "LOST" : white ? "WHITE" : zero ? "ZERO" : "FLAT";
	if (t->probe_varied)
		probe_lost++;
	else if (white)
		probe_white++;
	else if (zero)
		probe_zero++;
	else
		probe_flat++;
	static unsigned said;
	if ((t->probe_said && !t->probe_varied) || said >= 40)
		return;
	t->probe_said = true;
	said++;
	char hex[40] = "";
	for (unsigned k = 0; k < MIN2(t->blocksize, 8u); k++)
		snprintf(hex + 2 * k, sizeof(hex) - 2 * k, "%02x", first[k]);
	keep("texprobe %s: %s %ux%u/%u%s%s = %s, sampled by %.30s into %s %ux%u; written %u times, last by %s", what,
	     util_format_short_name(t->format), t->shape.width, t->shape.height, MAX2(t->shape.mip_levels, 1),
	     s->surface_id ? " surface" : "", t->is_view ? " view" : "", hex, st->name,
	     b->color_count && b->color[0] ? util_format_short_name(b->color[0]->format) : "-", b->width, b->height,
	     s->writes, s->writes ? s->writer : "nothing (uploaded or never drawn)");
}

/* --- a stage's resources at a draw --------------------------------------------------------------------------------- */

struct bound {
	uint64_t ubos, push, textures, samplers;
};

/* the CPU's view of uniform buffer N as bound, for the pushed words */
struct ubo_view {
	const uint8_t *cpu;
	uint32_t size;
};

static const struct rwa_buffer_bind *
table_entry(const struct rwa_buffer_bind *table, unsigned count, unsigned index)
{
	return table && index < count && table[index].buffer ? &table[index] : NULL;
}

/* what the system values of a draw or a dispatch are made of */
struct sv_input {
	float viewport[6];          /* scale x y z, offset x y z */
	unsigned offsets[3];        /* offset_start, base vertex, base instance */
	unsigned vertex_count;
	unsigned groups[3], local[3];
};

static int
emit_stage(struct batch *b, const struct rwa_stage *st, const struct rwa_bindings *bind,
	   const struct rwa_buffer_bind *table, unsigned table_count, const struct sv_input *in, struct bound *out,
	   char *why, size_t why_size)
{
	const float *viewport = in->viewport;
	const unsigned *offsets = in->offsets;
	const unsigned vertex_count = in->vertex_count;
	struct rwa_device *d = the_device;
	struct ubo_view views[RWA_MAX_UBOS];
	uint64_t ssbo_addr[RWA_MAX_UBOS] = { 0 };
	uint32_t ssbo_size[RWA_MAX_UBOS] = { 0 };
	uint8_t misalign[RWA_MAX_UBOS] = { 0 };

	const unsigned es_kind = st->info.stage == MESA_SHADER_FRAGMENT;
	uint64_t es_t = now_ns(), es_t1;
#define ES_PHASE(n) (es_t1 = now_ns(), stage_ns[es_kind][n] += es_t1 - es_t, es_t = es_t1)
	memset(out, 0, sizeof(*out));
	memset(views, 0, sizeof(views));
	struct ptr ubos = { NULL, 0 };
	if (st->ubo_count) {
		ubos = pool_alloc(b, st->ubo_count * pan_size(UNIFORM_BUFFER), 64, why, why_size);
		if (ubos.cpu == NULL)
			return -1;
		/* an entry the shader declares and nothing is bound to: 256 bytes of zeros */
		for (unsigned i = 0; i < st->ubo_count; i++) {
			pan_cast_and_pack((ubos.cpu + i * pan_size(UNIFORM_BUFFER)), UNIFORM_BUFFER, cfg) {
				cfg.pointer = d->dummy->gpu;
				cfg.entries = 16;
			}
			views[i].cpu = d->dummy->cpu;
			views[i].size = 256;
		}
		out->ubos = ubos.gpu;
	}
	unsigned ntex = 0, nsamp = 0;
	const struct rwa_texture *tex[RWA_MAX_SLOTS] = { 0 };
	const uint32_t *samp[RWA_MAX_SLOTS] = { 0 };
	for (unsigned i = 0; i < st->air.num_resources; i++) {
		const struct air_resource *r = &st->air.resources[i];
		if (r->unused)
			continue;
		if (r->argbuf_set != ~0u) {
			say(why, why_size, "%s reads an argument buffer (not served yet by the native library)", st->name);
			return -1;
		}
		switch (r->kind) {
		case AIR_RESOURCE_CONSTANT_BUFFER:
		case AIR_RESOURCE_DEVICE_BUFFER: {
			struct rwa_buffer *buf = r->index < bind->buffer_count ? bind->buffers[r->index] : NULL;
			const struct rwa_buffer_bind *e = table_entry(table, table_count, r->index);
			size_t off = e ? e->offset : 0;
			if (buf == NULL && e)
				buf = e->buffer;
			if (buf == NULL)
				break;
			if (off > buf->size) {
				say(why, why_size, "%s: buffer %u bound at offset %zu past its %zu bytes", st->name, r->index, off,
				    buf->size);
				return -1;
			}
			use_store(b, buf->store);
			size_t left = buf->size - off;
			if (r->kind == AIR_RESOURCE_DEVICE_BUFFER) {
				if (r->index < RWA_MAX_UBOS) {
					ssbo_addr[r->index] = buf->store->bo->gpu + off;
					ssbo_size[r->index] = (uint32_t)MIN2(left, UINT32_MAX);
				}
				break;
			}
			if (r->index >= st->ubo_count)
				break;     /* declared, never read after all */
			size_t want = MIN2(left, 65536);
			if (r->size && r->size < want)
				want = r->size;
			if (r->reads && r->reads < want)
				want = r->reads;
			bool dynamic = (st->ubo_dynamic[r->index / 64] >> (r->index % 64)) & 1;
			if (!dynamic && st->ubo_extent_known && st->ubo_extent[r->index] < want)
				want = st->ubo_extent[r->index];   /* what the shader's constant offsets reach (0: none) */
			want = MAX2(want, 4);
			uint64_t gpu = buf->store->bo->gpu + off;
			const uint8_t *cpu = buf->store->bo->cpu + off;
			if ((gpu & 15) != 0 && dynamic && (gpu & 3) == 0) {
				/* read at computed offsets: the descriptor on the 16 bytes before, the shader adds the rest
				 * (RWA_SV_UBO_MISALIGN) -- no copy out of write-combined memory */
				misalign[r->index] = gpu & 15;
				pan_cast_and_pack((ubos.cpu + r->index * pan_size(UNIFORM_BUFFER)), UNIFORM_BUFFER, cfg) {
					cfg.pointer = gpu & ~15ull;
					cfg.entries = DIV_ROUND_UP(want + (gpu & 15), 16);
				}
				views[r->index].cpu = cpu;
				views[r->index].size = (uint32_t)want;
				ubo_given++;
				break;
			}
			if ((gpu & 15) != 0) {
				ubo_copied++;
				ubo_copied_bytes += want;
				/* the hardware's uniform buffers start on 16 bytes: a copy (Gallium's user_buffer upload) */
				struct ptr c = pool_alloc(b, ALIGN_POT(want, 16), 16, why, why_size);
				if (c.cpu == NULL)
					return -1;
				memcpy(c.cpu, cpu, MIN2(want, left));
				gpu = c.gpu;
				cpu = c.cpu;
			}
			pan_cast_and_pack((ubos.cpu + r->index * pan_size(UNIFORM_BUFFER)), UNIFORM_BUFFER, cfg) {
				cfg.pointer = gpu;
				cfg.entries = DIV_ROUND_UP(want, 16);
			}
			views[r->index].cpu = cpu;
			views[r->index].size = (uint32_t)want;
			break;
		}
		case AIR_RESOURCE_TEXTURE: {
			const struct rwa_texture *t = r->index < bind->texture_count ? bind->textures[r->index] : NULL;
			tex[r->index] = t;
			ntex = MAX2(ntex, r->index + 1);
			break;
		}
		case AIR_RESOURCE_SAMPLER:
			if (r->inline_sampler) {
				samp[r->index] = st->inline_sampler[r->index];
				size_t l = strlen(b->samplers);
				if (l < sizeof(b->samplers) - 16)
					snprintf(b->samplers + l, sizeof(b->samplers) - l, " i%08x%08x", st->inline_sampler[r->index][0],
						 st->inline_sampler[r->index][1]);
			} else if (r->index < bind->sampler_count && bind->samplers[r->index]) {
				samp[r->index] = bind->samplers[r->index]->desc;
				const struct rwa_sampler_state *ss = &bind->samplers[r->index]->state;
				size_t l = strlen(b->samplers);
				if (l < sizeof(b->samplers) - 16)
					snprintf(b->samplers + l, sizeof(b->samplers) - l, " a%u%u%u f%u%u%u n%u x%u", ss->address[0],
						 ss->address[1], ss->address[2], ss->min_filter, ss->mag_filter, ss->mip_filter,
						 ss->normalized_coordinates, ss->max_anisotropy);
			}
			nsamp = MAX2(nsamp, r->index + 1);
			break;
		default:
			break;
		}
	}
	ntex = MAX2(ntex, st->texture_count);
	nsamp = MAX2(nsamp, st->sampler_count);
	ES_PHASE(0);
	if (ntex) {
		/* the descriptors, then each one's surfaces (GENX(panfrost_new_texture): levels x layers of them) */
		/* each texture's surfaces on 64 bytes of their own, as panfrost_create_sampler_view_bo allocates a payload */
		size_t surfaces = 0;
		for (unsigned i = 0; i < ntex; i++)
			surfaces += ALIGN_POT((tex[i] ? (size_t)MAX2(tex[i]->shape.mip_levels, 1) * tex[i]->layers : 1) *
					      pan_size(SURFACE_WITH_STRIDE), 64);
		struct ptr tp = pool_alloc(b, ALIGN_POT(ntex * pan_size(TEXTURE), 64) + surfaces, 64, why, why_size);
		if (tp.cpu == NULL)
			return -1;
		uint8_t *sc = tp.cpu + ALIGN_POT(ntex * pan_size(TEXTURE), 64);
		uint64_t sg = tp.gpu + ALIGN_POT(ntex * pan_size(TEXTURE), 64);
		for (unsigned i = 0; i < ntex; i++) {
			const struct rwa_texture *t = tex[i];
			void *desc = tp.cpu + i * pan_size(TEXTURE);
			if (t == NULL) {
				/* nothing bound: a 1x1 of zeros (air_metal.c blank_view) */
				rwa_surface_desc(sc, d->dummy->gpu, 64, 0);
				const unsigned char id[4] = { PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W };
				rwa_texture_desc(desc, PIPE_FORMAT_R8G8B8A8_UNORM, id, MALI_TEXTURE_DIMENSION_2D, 1, 1, 1, 1, 1, 1,
						 sg);
				sc += 64;
				sg += 64;
				continue;
			}
			if (!rwa_samplable(t->format)) {
				say(why, why_size, "%s samples texture %u of %s, which this GPU does not sample", st->name, i,
				    util_format_short_name(t->format));
				return -1;
			}
			tex_probe(b, st, (struct rwa_texture *)t);
			use_store(b, t->store);
			if (MAX2(t->shape.mip_levels, 1) > 1)
				b->tex_flags |= F_TEX_MIP;
			if (t->format == PIPE_FORMAT_R16G16B16A16_FLOAT)
				b->tex_flags |= F_TEX_F16;
			if (t->store->surface_id)
				b->tex_flags |= F_TEX_SURFACE;
			if (b->nsampled < 8) {
				size_t l = strlen(b->sampled);
				snprintf(b->sampled + l, sizeof(b->sampled) - l, " %.10s %ux%u/%u r%u%s%s%s",
					 util_format_short_name(t->format) + (strncmp(util_format_short_name(t->format), "r16g16b16a16", 12) == 0 ? 9 : 0),
					 t->shape.width, t->shape.height, MAX2(t->shape.mip_levels, 1), t->row_bytes[0],
					 t->store->surface_id ? "S" : "", t->is_view ? "V" : "",
					 t->store == (b->color[0] ? b->color[0]->store : NULL) ? "!T" : "");
				b->nsampled++;
			}
			unsigned levels = MAX2(t->shape.mip_levels, 1);
			uint64_t first = sg;
			/* GENX(panfrost_emit_texture_payload): layer-major, then level */
			for (unsigned layer = 0; layer < t->layers; layer++)
				for (unsigned l = 0; l < levels; l++) {
					uint64_t at = t->store->bo->gpu + t->offset + layer * t->layer_stride + t->level_offset[l];
					unsigned rows = DIV_ROUND_UP(u_minify(t->shape.height, l), t->bh);
					rwa_surface_desc(sc, at, (int32_t)t->row_bytes[l], (int32_t)(t->row_bytes[l] * rows));
					sc += pan_size(SURFACE_WITH_STRIDE);
					sg += pan_size(SURFACE_WITH_STRIDE);
				}
			rwa_texture_desc(desc, t->format, t->swizzle, t->dim, t->shape.width, t->shape.height,
					 MAX2(t->shape.depth, 1), levels,
					 t->dim == MALI_TEXTURE_DIMENSION_CUBE ? t->layers / 6 : t->layers, 1, first);
			/* the next texture's surfaces on 64 bytes */
			uint64_t pad = ALIGN_POT(sg, 64) - sg;
			sc += pad;
			sg += pad;
		}
		out->textures = tp.gpu;
	}
	ES_PHASE(1);
	if (nsamp) {
		struct ptr sp = pool_alloc(b, nsamp * pan_size(SAMPLER), 64, why, why_size);
		if (sp.cpu == NULL)
			return -1;
		for (unsigned i = 0; i < nsamp; i++) {
			if (samp[i]) {
				memcpy(sp.cpu + i * pan_size(SAMPLER), samp[i], pan_size(SAMPLER));
				continue;
			}
			/* nothing bound: nearest, clamped (air_metal.c default_sampler) */
			pan_cast_and_pack((sp.cpu + i * pan_size(SAMPLER)), SAMPLER, cfg) {
				cfg.magnify_nearest = true;
				cfg.minify_nearest = true;
				cfg.normalized_coordinates = true;
				cfg.wrap_mode_s = MALI_WRAP_MODE_CLAMP_TO_EDGE;
				cfg.wrap_mode_t = MALI_WRAP_MODE_CLAMP_TO_EDGE;
				cfg.wrap_mode_r = MALI_WRAP_MODE_CLAMP_TO_EDGE;
			}
		}
		out->samplers = sp.gpu;
	}
	ES_PHASE(2);
	/* the system values (panfrost_upload_sysvals) */
	if (st->sysval_count) {
		struct ptr sv = pool_alloc(b, st->sysval_count * 16, 16, why, why_size);
		if (sv.cpu == NULL)
			return -1;
		memset(sv.cpu, 0, st->sysval_count * 16);
		for (unsigned i = 0; i < st->sysval_count; i++) {
			uint32_t *u = (uint32_t *)(sv.cpu + 16 * i);
			float *f = (float *)u;
			unsigned arg = RWA_SV_ARG(st->sysvals[i]);
			switch (RWA_SV_KIND(st->sysvals[i])) {
			case RWA_SV_VIEWPORT_SCALE: f[0] = viewport[0]; f[1] = viewport[1]; f[2] = viewport[2]; break;
			case RWA_SV_VIEWPORT_OFFSET: f[0] = viewport[3]; f[1] = viewport[4]; f[2] = viewport[5]; break;
			case RWA_SV_VERTEX_OFFSETS: u[0] = offsets[0]; u[1] = offsets[1]; u[2] = offsets[2]; break;
			case RWA_SV_NUM_VERTICES: u[0] = vertex_count; break;
			case RWA_SV_SSBO:
				if (arg < RWA_MAX_UBOS) {
					uint64_t a = ssbo_addr[arg] ? ssbo_addr[arg] : d->dummy->gpu;
					memcpy(u, &a, 8);
					u[2] = ssbo_addr[arg] ? ssbo_size[arg] : 0;
				}
				break;
			case RWA_SV_TEXTURE_SIZE: {
				unsigned ti = arg & 0xff;
				const struct rwa_texture *t = ti < RWA_MAX_SLOTS ? tex[ti] : NULL;
				if (t) {
					u[0] = t->shape.width;
					u[1] = t->shape.height;
					u[2] = t->dim == MALI_TEXTURE_DIMENSION_3D ? MAX2(t->shape.depth, 1) : t->layers;
					if ((arg >> 12) & 1)
						u[(arg >> 8) & 0xf] = t->dim == MALI_TEXTURE_DIMENSION_CUBE ? t->layers / 6 : t->layers;
				} else {
					u[0] = u[1] = u[2] = 1;
				}
				break;
			}
			case RWA_SV_TEXTURE_LEVELS: {
				const struct rwa_texture *t = arg < RWA_MAX_SLOTS ? tex[arg] : NULL;
				u[0] = t ? MAX2(t->shape.mip_levels, 1) : 1;
				break;
			}
			case RWA_SV_SAMPLE_POSITIONS: {
				uint64_t a = d->sample_positions->gpu;
				memcpy(u, &a, 8);
				break;
			}
			case RWA_SV_MULTISAMPLED: u[0] = 0; break;
			case RWA_SV_UBO_MISALIGN: u[0] = arg < RWA_MAX_UBOS ? misalign[arg] : 0; break;
			case RWA_SV_NUM_WORKGROUPS: u[0] = in->groups[0]; u[1] = in->groups[1]; u[2] = in->groups[2]; break;
			case RWA_SV_LOCAL_SIZE: u[0] = in->local[0]; u[1] = in->local[1]; u[2] = in->local[2]; break;
			case RWA_SV_RT_CONVERSION: {
				unsigned rt = arg & 0xf, size = arg >> 4;
				if (rt < b->color_count && b->color[rt])
					u[0] = (uint32_t)rwa_rt_conversion(b->color[rt]->format, rt, size);
				break;
			}
			default: break;
			}
		}
		if (st->sysval_ubo < st->ubo_count) {
			pan_cast_and_pack((ubos.cpu + st->sysval_ubo * pan_size(UNIFORM_BUFFER)), UNIFORM_BUFFER, cfg) {
				cfg.pointer = sv.gpu;
				cfg.entries = st->sysval_count;
			}
			views[st->sysval_ubo].cpu = sv.cpu;
			views[st->sysval_ubo].size = st->sysval_count * 16;
		}
	}
	/* the pushed words (panfrost_emit_const_buf) */
	ES_PHASE(3);
	if (st->info.push.count) {
		struct ptr pp = pool_alloc(b, st->info.push.count * 4, 16, why, why_size);
		if (pp.cpu == NULL)
			return -1;
		/* each buffer's words read in one copy, not one by one: a bound buffer is write-combined memory, and a
		 * read of it is the memory's, uncached */
		uint32_t lo[RWA_MAX_UBOS], hi[RWA_MAX_UBOS];
		static _Thread_local uint8_t local[RWA_MAX_UBOS][1024];
		for (unsigned u = 0; u < st->ubo_count && u < RWA_MAX_UBOS; u++) {
			lo[u] = UINT32_MAX;
			hi[u] = 0;
		}
		for (unsigned i = 0; i < st->info.push.count; i++) {
			unsigned u = st->info.push.words[i].ubo, o = st->info.push.words[i].offset;
			if (u < st->ubo_count && u < RWA_MAX_UBOS && views[u].cpu && o + 4 <= views[u].size) {
				lo[u] = MIN2(lo[u], o);
				hi[u] = MAX2(hi[u], o + 4);
			}
		}
		const uint8_t *base[RWA_MAX_UBOS];
		for (unsigned u = 0; u < st->ubo_count && u < RWA_MAX_UBOS; u++) {
			base[u] = NULL;
			if (lo[u] < hi[u] && hi[u] - lo[u] <= sizeof(local[u])) {
				memcpy(local[u], views[u].cpu + lo[u], hi[u] - lo[u]);
				base[u] = local[u] - lo[u];
			}
		}
		uint32_t *words = (uint32_t *)pp.cpu;
		for (unsigned i = 0; i < st->info.push.count; i++) {
			unsigned u = st->info.push.words[i].ubo, o = st->info.push.words[i].offset;
			uint32_t w = 0;
			if (u < st->ubo_count && u < RWA_MAX_UBOS && views[u].cpu && o + 4 <= views[u].size)
				memcpy(&w, (base[u] ? base[u] : views[u].cpu) + o, 4);
			words[i] = w;
		}
		out->push = pp.gpu;
	}
	ES_PHASE(4);
	return 0;
}

/* --- a draw -------------------------------------------------------------------------------------------------------- */

static enum mali_draw_mode
draw_mode(unsigned primitive, bool *ok)
{
	*ok = true;
	switch (primitive) {   /* MTLPrimitiveType */
	case 0: return MALI_DRAW_MODE_POINTS;
	case 1: return MALI_DRAW_MODE_LINES;
	case 2: return MALI_DRAW_MODE_LINE_STRIP;
	case 3: return MALI_DRAW_MODE_TRIANGLES;
	case 4: return MALI_DRAW_MODE_TRIANGLE_STRIP;
	default: *ok = false; return MALI_DRAW_MODE_TRIANGLES;
	}
}

static void
pack_invocation(void *out, unsigned vertex_count, unsigned instance_count)
{
	if (instance_count > 1) {
		panfrost_pack_work_groups_compute(out, 1, vertex_count, instance_count, 1, 1, 1, true, false);
		return;
	}
	pan_cast_and_pack((out), INVOCATION, cfg) {
		cfg.invocations = vertex_count - 1;
		cfg.size_y_shift = 0;
		cfg.size_z_shift = 0;
		cfg.workgroups_x_shift = 0;
		cfg.workgroups_y_shift = 0;
		cfg.workgroups_z_shift = 32;
		cfg.thread_group_split = MALI_SPLIT_MIN_EFFICIENT;
	}
}

static void
link_job(struct batch *b, uint8_t *header_cpu, uint64_t gpu)
{
	if (b->last_header) {
		/* the Job Header's next pointer, words 6..7 (pan_jc_add_job) */
		memcpy(b->last_header + 24, &gpu, 8);
	} else {
		b->first_job = gpu;
	}
	b->last_header = header_cpu;
}

static int
draw(struct rwa_render *r, const struct rwa_draw *dr, char *why, size_t why_size)
{
	struct rwa_device *d = the_device;
	uint64_t t = now_ns(), t1;
#define PHASE(n) (t1 = now_ns(), phase_ns[n] += t1 - t, t = t1)
	bool ok;
	enum mali_draw_mode mode = draw_mode(dr->primitive, &ok);

	if (!ok) {
		say(why, why_size, "primitive type %u", dr->primitive);
		return -1;
	}
	if (dr->color_count != r->rt_count) {
		say(why, why_size, "%u colour attachments for a pipeline made for %u", dr->color_count, r->rt_count);
		return -1;
	}
	for (unsigned i = 0; i < dr->color_count; i++)
		if (dr->color[i] && r->rt_format[i] != PIPE_FORMAT_NONE &&
		    util_format_get_blocksize(dr->color[i]->format) != util_format_get_blocksize(r->rt_format[i])) {
			say(why, why_size, "attachment %u is %s and the pipeline renders %s", i,
			    util_format_short_name(dr->color[i]->format), util_format_short_name(r->rt_format[i]));
			return -1;
		}
	if (dr->depth && !r->zs_format) {
		/* a pass with depth and a pipeline without: Metal draws with no depth test */
	}
	for (unsigned i = 0; dr->color_level && i < dr->color_count; i++)
		if (dr->color[i] && dr->color_level[i] >= MAX2(dr->color[i]->shape.mip_levels, 1)) {
			say(why, why_size, "attachment %u drawn into level %u of a texture of %u", i, dr->color_level[i],
			    MAX2(dr->color[i]->shape.mip_levels, 1));
			return -1;
		}
	PHASE(0);
	struct batch *b = batch_for(dr->color, dr->color_level, dr->color_count, dr->depth, dr->clear_color, dr->clear_depth,
				    dr->clear_stencil, dr, why, why_size);
	if (b == NULL)
		return -1;
	PHASE(1);
	/* boot-arg rwn_profile=1: the full-screen passes of several targets skip, frame by frame in turn, none of their
	 * draws, CoreAnimation's path rasterizer's (path_*), the plain layers that read their destination, and those that
	 * do not -- the GPU's time of each kind of frame told apart (what each group costs) */
	if (b->width >= 700 && b->color_count > 1 && boot_arg("rwn_profile=1") && now_ns() - lib_up_ns > 150000000000ull &&
	    now_ns() - lib_up_ns < 300000000000ull) {
		b->prof_group = 1 + (unsigned)(commits % 4);
		/* run 546 showed the path draws are three quarters: now which of them -- the clears, the blits (framebuffer
		 * fetch of the R32Float coverage), or the coverage itself (rect, cubic, stroke; additive R32Float) */
		/* run 571 (the tiler's levels fixed, DontCare cleared, 950 MHz): the path rasterizer whole, the glass, or the
		 * plain layers (CoreAnimation's uber shader and the rest) */
		bool path = strncmp(r->fs.name, "path_", 5) == 0, glass = strncmp(r->fs.name, "glass_", 6) == 0;
		bool skip = (b->prof_group == 2 && path) || (b->prof_group == 3 && glass) ||
			    (b->prof_group == 4 && !path && !glass);
		if (skip)
			return 0;
	}
	unsigned instances = MAX2(dr->instance_count, 1);
	if (dr->base_instance && instances > 1) {
		say(why, why_size, "a base instance of %u (not served yet)", dr->base_instance);
		return -1;
	}

	/* the vertices the vertex job shades, and where the tiler finds them */
	unsigned vertex_count, offset_start, index_count = 0;
	uint64_t indices = 0;
	int32_t base_vertex_offset = 0;
	unsigned index_size = 0;
	if (dr->index_buffer) {
		index_size = dr->index_type == 1 ? 4 : 2;
		struct rwa_buffer *ib = dr->index_buffer;
		size_t end = dr->index_buffer_offset + (size_t)dr->vertex_count * index_size;
		if (end > ib->size || (dr->index_buffer_offset % index_size) != 0) {
			say(why, why_size, "%u indices at offset %zu of a %zu-byte index buffer", dr->vertex_count,
			    dr->index_buffer_offset, ib->size);
			return -1;
		}
		if (ib->store->pending && ib->store->pending != b)
			store_sync(ib->store);
		/* the range they span (u_vbuf's min/max scan; the restart index skipped, as Metal always restarts strips) */
		uint32_t lo = UINT32_MAX, hi = 0;
		const uint8_t *p = ib->store->bo->cpu + dr->index_buffer_offset;
		uint32_t restart = index_size == 4 ? 0xffffffffu : 0xffffu;
		for (unsigned i = 0; i < dr->vertex_count; i++) {
			uint32_t v = index_size == 4 ? ((const uint32_t *)p)[i] : ((const uint16_t *)p)[i];
			if (v == restart)
				continue;
			lo = MIN2(lo, v);
			hi = MAX2(hi, v);
		}
		if (lo > hi)
			return 0;   /* no vertex at all */
		vertex_count = hi - lo + 1;
		offset_start = (unsigned)((int64_t)lo + dr->base_vertex);
		base_vertex_offset = dr->base_vertex - (int32_t)offset_start;
		index_count = dr->vertex_count;
		indices = ib->store->bo->gpu + dr->index_buffer_offset;
		use_store(b, ib->store);
	} else {
		vertex_count = dr->vertex_count;
		offset_start = dr->vertex_start;
	}
	if (vertex_count == 0)
		return 0;
	unsigned padded = instances > 1 ? panfrost_padded_vertex_count(vertex_count) : vertex_count;
	unsigned alloc = instances > 1 ? padded * instances : vertex_count;

	/* the viewport transform (Metal's: y down) and the scissor */
	float vx = dr->viewport ? dr->viewport[0] : 0.0f, vy = dr->viewport ? dr->viewport[1] : 0.0f;
	float vw = dr->viewport ? dr->viewport[2] : (float)b->width, vh = dr->viewport ? dr->viewport[3] : (float)b->height;
	float zn = dr->depth_range ? dr->depth_range[0] : 0.0f, zf = dr->depth_range ? dr->depth_range[1] : 1.0f;
	const float vp[6] = { vw / 2.0f, -(vh / 2.0f), zf - zn, vx + vw / 2.0f, vy + vh / 2.0f, zn };
	int64_t sx0 = (int64_t)floorf(MIN2(vx, vx + vw)), sy0 = (int64_t)floorf(MIN2(vy, vy + vh));
	int64_t sx1 = (int64_t)ceilf(MAX2(vx, vx + vw)) - 1, sy1 = (int64_t)ceilf(MAX2(vy, vy + vh)) - 1;
	if (dr->scissor) {
		sx0 = MAX2(sx0, (int64_t)dr->scissor[0]);
		sy0 = MAX2(sy0, (int64_t)dr->scissor[1]);
		sx1 = MIN2(sx1, (int64_t)dr->scissor[0] + dr->scissor[2] - 1);
		sy1 = MIN2(sy1, (int64_t)dr->scissor[1] + dr->scissor[3] - 1);
	}
	sx0 = MAX2(sx0, 0);
	sy0 = MAX2(sy0, 0);
	sx1 = MIN2(sx1, (int64_t)b->width - 1);
	sy1 = MIN2(sy1, (int64_t)b->height - 1);
	/* the draws of small offscreen passes that clear (run 556: such passes of fixed_frag_lph_cpf, the glass's backdrops
	 * among them, came out holding only their clear): what each draw is, and the textures it samples, 60 lines */
	if (b->width <= 800 && b->height <= 800 && b->color_count && b->color[0] && b->clear[0] &&
	    !b->color[0]->store->surface_id && !boot_arg("rwn_smalllog=0")) {
		static unsigned said;
		if (said < 60) {
			said++;
			char tx[200] = "";
			int n = 0;
			for (unsigned i = 0; i < dr->fragment.texture_count && i < 4 && n < (int)sizeof(tx) - 50; i++) {
				const struct rwa_texture *t = dr->fragment.textures ? dr->fragment.textures[i] : NULL;
				if (t)
					n += snprintf(tx + n, sizeof(tx) - n, " t%u=%.8s %ux%u by %.14s", i,
						      util_format_short_name(t->format), t->shape.width, t->shape.height,
						      t->store->writes ? t->store->writer : (t->store->surface_id ? "surface" : "-"));
			}
			keep("small pass %ux%u %s L%u: %.24s v%u i%u vp %.0f,%.0f %.0fx%.0f sc %s -> %lld,%lld..%lld,%lld%s;%s",
			     b->width, b->height, util_format_short_name(b->color[0]->format), b->level[0], r->fs.name, vertex_count,
			     instances, vx, vy, vw, vh, dr->scissor ? "set" : "-", (long long)sx0, (long long)sy0, (long long)sx1,
			     (long long)sy1, (sx0 > sx1 || sy0 > sy1) ? " CULLED" : "", tx);
		}
	}
	if (sx0 > sx1 || sy0 > sy1)
		return 0;     /* a scissor that leaves no pixel: nothing to draw */
	/* the depth range the clipper keeps: [0, 1] with a depth attachment (clip_halfz), [-1, 1] otherwise, as the
	 * Gallium library's rasterizer state has it */
	float minz = r->zs_format ? MIN2(zn, zf) : zn - (zf - zn), maxz = MAX2(zn, zf);

	use_render(b, r);
	struct sv_input svi = { .offsets = { offset_start, (unsigned)dr->base_vertex, dr->base_instance },
				.vertex_count = vertex_count };
	memcpy(svi.viewport, vp, sizeof(vp));
	struct bound vb, fb;
	PHASE(2);
	if (emit_stage(b, &r->vs, &dr->vertex, dr->vertex_buffer_table, dr->vertex_buffer_table_count, &svi, &vb, why,
		       why_size) != 0)
		return -1;
	PHASE(3);
	if (emit_stage(b, &r->fs, &dr->fragment, dr->fragment_buffer_table, dr->fragment_buffer_table_count, &svi, &fb,
		       why, why_size) != 0)
		return -1;
	PHASE(4);

	/* the vertex attributes, a buffer each (panfrost_emit_vertex_data, 64-byte aligned bases) */
	struct ptr attrs = { NULL, 0 }, abufs = { NULL, 0 };
	if (r->attr_count) {
		attrs = pool_alloc(b, r->attr_count * pan_size(ATTRIBUTE), 64, why, why_size);
		abufs = pool_alloc(b, (2 * r->attr_count + 1) * pan_size(ATTRIBUTE_BUFFER), 64, why, why_size);
		if (!attrs.cpu || !abufs.cpu)
			return -1;
		unsigned k = 0;
		for (unsigned i = 0; i < r->attr_count; i++) {
			const struct rwa_attr_rec *a = &r->attr[i];
			if (a->buffer == RWA_ATTR_ZERO) {
				pan_cast_and_pack((abufs.cpu + k * pan_size(ATTRIBUTE_BUFFER)), ATTRIBUTE_BUFFER, cfg) {
					cfg.pointer = d->dummy->gpu;
					cfg.stride = 0;
					cfg.size = 64;
				}
				pan_cast_and_pack((attrs.cpu + i * pan_size(ATTRIBUTE)), ATTRIBUTE, cfg) {
					cfg.buffer_index = k;
					cfg.format = a->format;
					cfg.offset = 0;
				}
				k++;
				continue;
			}
			const struct rwa_vertex_stream *s =
			    a->buffer < dr->vertex_stream_count ? &dr->vertex_streams[a->buffer] : NULL;
			const struct rwa_vertex_layout *l = &r->shape.vertex_layout[a->buffer];
			if (s == NULL || s->buffer == NULL) {
				say(why, why_size, "%s fetches from buffer %u and nothing is bound there", r->vs.name, a->buffer);
				return -1;
			}
			use_store(b, s->buffer->store);
			uint64_t addr = s->buffer->store->bo->gpu + s->offset;
			uint32_t size = (uint32_t)MIN2(s->buffer->size - MIN2(s->offset, s->buffer->size), UINT32_MAX);
			int32_t off = (int32_t)(a->offset + (addr & 63));
			uint64_t base = addr & ~63ull;
			size += (uint32_t)(addr & 63);
			unsigned stride = a->step == 0 ? 0 : l->stride;
			bool per_instance = a->step == 2;
			if (per_instance && instances > 1 && offset_start)
				off -= (int32_t)(stride * offset_start) / (int32_t)a->step_rate;
			if (per_instance && dr->base_instance)
				off += (int32_t)(stride * dr->base_instance / a->step_rate);
			if (per_instance && instances > 1) {
				unsigned divisor = a->step_rate * padded;
				if (util_is_power_of_two_or_zero(divisor)) {
					pan_cast_and_pack((abufs.cpu + k * pan_size(ATTRIBUTE_BUFFER)), ATTRIBUTE_BUFFER, cfg) {
						cfg.type = MALI_ATTRIBUTE_TYPE_1D_POT_DIVISOR;
						cfg.pointer = base;
						cfg.stride = stride;
						cfg.size = size;
						cfg.divisor_r = __builtin_ctz(divisor);
					}
					pan_cast_and_pack((attrs.cpu + i * pan_size(ATTRIBUTE)), ATTRIBUTE, cfg) {
						cfg.buffer_index = k;
						cfg.format = a->format;
						cfg.offset = off;
					}
					k++;
				} else {
					k = (k + 1) & ~1u;    /* the NPOT pair at an even record */
					unsigned shift = 0, extra = 0;
					unsigned magic = panfrost_compute_magic_divisor(divisor, &shift, &extra);
					pan_cast_and_pack((abufs.cpu + k * pan_size(ATTRIBUTE_BUFFER)), ATTRIBUTE_BUFFER, cfg) {
						cfg.type = MALI_ATTRIBUTE_TYPE_1D_NPOT_DIVISOR;
						cfg.pointer = base;
						cfg.stride = stride;
						cfg.size = size;
						cfg.divisor_r = shift;
						cfg.divisor_e = extra;
					}
					pan_cast_and_pack((abufs.cpu + (k + 1) * pan_size(ATTRIBUTE_BUFFER)),
							  ATTRIBUTE_BUFFER_CONTINUATION_NPOT, cfg) {
						cfg.divisor_numerator = magic;
						cfg.divisor = a->step_rate;
					}
					pan_cast_and_pack((attrs.cpu + i * pan_size(ATTRIBUTE)), ATTRIBUTE, cfg) {
						cfg.buffer_index = k;
						cfg.format = a->format;
						cfg.offset = off;
					}
					k += 2;
				}
				continue;
			}
			pan_cast_and_pack((abufs.cpu + k * pan_size(ATTRIBUTE_BUFFER)), ATTRIBUTE_BUFFER, cfg) {
				cfg.type = instances > 1 && !per_instance ? MALI_ATTRIBUTE_TYPE_1D_MODULUS : MALI_ATTRIBUTE_TYPE_1D;
				cfg.pointer = base;
				cfg.stride = stride;
				cfg.size = size;
				if (instances > 1 && !per_instance)
					cfg.divisor = padded;
			}
			pan_cast_and_pack((attrs.cpu + i * pan_size(ATTRIBUTE)), ATTRIBUTE, cfg) {
				cfg.buffer_index = k;
				cfg.format = a->format;
				cfg.offset = off;
			}
			k++;
		}
		/* "Bifrost needs an empty desc to mark end of prefetching" */
		pan_cast_and_pack((abufs.cpu + k * pan_size(ATTRIBUTE_BUFFER)), ATTRIBUTE_BUFFER, cfg);
	}

	PHASE(5);
	/* the varyings: records of both stages and their buffers -- general, position, point size, the end */
	struct ptr vrec = pool_alloc(b, (r->vs_out_count + r->fs_in_count + 1) * pan_size(ATTRIBUTE), 64, why, why_size);
	struct ptr vbuf = pool_alloc(b, 4 * pan_size(ATTRIBUTE_BUFFER), 64, why, why_size);
	struct ptr general = { NULL, 0 }, pos = pool_gpu(b, 16 * alloc, 64, why, why_size), psiz = { NULL, 0 };
	const bool small_say = b->width <= 800 && b->height <= 800 && b->color_count && b->color[0] && b->clear[0] &&
			       !b->color[0]->store->surface_id && !boot_arg("rwn_smalllog=0");
	if (pos.cpu && ((b->width >= 700 && b->color_count > 1 && boot_arg("rwn_profile=2")) || small_say) &&
	    b->nareas < 256) {
		if (b->areas == NULL)
			b->areas = calloc(256, sizeof(*b->areas));
		if (b->areas) {
			struct area_rec *a = &b->areas[b->nareas++];
			a->pos = pool_real(b, pos.cpu);
			a->count = MIN2(vertex_count, 64);
			a->stride = instances > 1 ? padded : vertex_count;
			a->all_instances = instances;
			a->instances = MIN2(instances, MAX2(1024 / MAX2(a->stride, 1), 1));
			a->clip[0] = (int16_t)sx0;
			a->clip[1] = (int16_t)sy0;
			a->clip[2] = (int16_t)sx1;
			a->clip[3] = (int16_t)sy1;
			snprintf(a->name, sizeof(a->name), "%s", r->fs.name);
			a->say = small_say;
			a->target[0] = (uint16_t)b->width;
			a->target[1] = (uint16_t)b->height;
		}
	}
	if (r->general_stride)
		general = pool_gpu(b, (size_t)r->general_stride * alloc, 64, why, why_size);
	if (r->writes_psiz)
		psiz = pool_gpu(b, 2 * alloc, 64, why, why_size);
	if (!vrec.cpu || !vbuf.cpu || !pos.cpu || (r->general_stride && !general.cpu) || (r->writes_psiz && !psiz.cpu))
		return -1;
	for (unsigned i = 0; i < r->vs_out_count + r->fs_in_count; i++) {
		const struct rwa_varying_rec *v = i < r->vs_out_count ? &r->vs_out[i] : &r->fs_in[i - r->vs_out_count];
		pan_cast_and_pack((vrec.cpu + i * pan_size(ATTRIBUTE)), ATTRIBUTE, cfg) {
			cfg.buffer_index = v->buffer;
			cfg.offset_enable = false;
			cfg.format = v->format;
			cfg.offset = v->offset;
		}
	}
	pan_cast_and_pack((vbuf.cpu), ATTRIBUTE_BUFFER, cfg) {
		cfg.pointer = general.gpu;
		cfg.stride = r->general_stride;
		cfg.size = r->general_stride * alloc;
	}
	pan_cast_and_pack((vbuf.cpu + pan_size(ATTRIBUTE_BUFFER)), ATTRIBUTE_BUFFER, cfg) {
		cfg.pointer = pos.gpu;
		cfg.stride = 16;
		cfg.size = 16 * alloc;
	}
	pan_cast_and_pack((vbuf.cpu + 2 * pan_size(ATTRIBUTE_BUFFER)), ATTRIBUTE_BUFFER, cfg) {
		cfg.pointer = psiz.gpu;
		cfg.stride = r->writes_psiz ? 2 : 0;
		cfg.size = r->writes_psiz ? 2 * alloc : 0;
	}
	pan_cast_and_pack((vbuf.cpu + 3 * pan_size(ATTRIBUTE_BUFFER)), ATTRIBUTE_BUFFER, cfg);

	/* the fixed state: renderer states (the fragment one followed by the targets' blend descriptors), viewport */
	unsigned nblend = MAX2(r->rt_count, 1);
	struct ptr vsr = pool_alloc(b, pan_size(RENDERER_STATE), 64, why, why_size);
	struct ptr fsr = pool_alloc(b, pan_size(RENDERER_STATE) + nblend * pan_size(BLEND), 64, why, why_size);
	struct ptr vpd = pool_alloc(b, pan_size(VIEWPORT), 32, why, why_size);
	struct ptr jobs = pool_alloc(b, pan_size(COMPUTE_JOB) + pan_size(TILER_JOB), 64, why, why_size);
	if (!vsr.cpu || !fsr.cpu || !vpd.cpu || !jobs.cpu)
		return -1;
	memcpy(vsr.cpu, r->vs.rsd, 64);
	rwa_render_fs_rsd(r, dr->depth_stencil ? &dr->depth_stencil->state : NULL, dr->depth != NULL, 1, fsr.cpu);
	memcpy(fsr.cpu + pan_size(RENDERER_STATE), r->blend, nblend * pan_size(BLEND));
	pan_cast_and_pack((vpd.cpu), VIEWPORT, cfg) {
		cfg.scissor_minimum_x = (unsigned)sx0;
		cfg.scissor_minimum_y = (unsigned)sy0;
		cfg.scissor_maximum_x = (unsigned)sx1;
		cfg.scissor_maximum_y = (unsigned)sy1;
		cfg.minimum_z = minz;
		cfg.maximum_z = maxz;
	}
	if (b->tls_frozen && MAX2(r->vs.info.tls_size, r->fs.info.tls_size) > b->tls_frozen_size) {
		/* the thread storage went with a part already sent (vt_flush), smaller than this draw asks: a descriptor of
		 * its own for this draw and those after, filled at the submit (or the next part) */
		b->tls = pool_alloc(b, pan_size(LOCAL_STORAGE), 64, why, why_size);
		if (b->tls.cpu == NULL)
			return -1;
		b->tls_frozen = false;
	}
	b->tls_size = MAX3(b->tls_size, r->vs.info.tls_size, r->fs.info.tls_size);
	b->minx = MIN2(b->minx, (unsigned)sx0);
	b->miny = MIN2(b->miny, (unsigned)sy0);
	b->maxx = MAX2(b->maxx, (unsigned)sx1);
	b->maxy = MAX2(b->maxy, (unsigned)sy1);

	/* the vertex job (jm_emit_vertex_job) */
	uint8_t *vj = jobs.cpu, *tj = jobs.cpu + pan_size(COMPUTE_JOB);
	uint64_t vjg = jobs.gpu, tjg = jobs.gpu + pan_size(COMPUTE_JOB);
	if (b->job_index + 2 > 0xfff0) {
		say(why, why_size, "more than %u draws in one pass", 0xfff0 / 2);
		return -1;
	}
	unsigned vindex = ++b->job_index, tindex = ++b->job_index;
	pack_invocation(pan_section_ptr(vj, COMPUTE_JOB, INVOCATION), vertex_count, instances);
	pan_section_pack(vj, COMPUTE_JOB, PARAMETERS, cfg) {
		cfg.job_task_split = 5;
	}
	pan_section_pack(vj, COMPUTE_JOB, DRAW, cfg) {
		cfg.state = vsr.gpu;
		cfg.attributes = attrs.gpu;
		cfg.attribute_buffers = abufs.gpu;
		cfg.varyings = vrec.gpu;
		cfg.varying_buffers = vbuf.gpu;
		cfg.thread_storage = b->tls.gpu;
		cfg.offset_start = offset_start;
		cfg.instance_size = instances > 1 ? padded : 1;
		cfg.uniform_buffers = vb.ubos;
		cfg.push_uniforms = vb.push;
		cfg.textures = vb.textures;
		cfg.samplers = vb.samplers;
	}
	pan_section_pack(vj, COMPUTE_JOB, HEADER, cfg) {
		cfg.type = MALI_JOB_TYPE_VERTEX;
		cfg.index = vindex;
	}
	link_job(b, vj, vjg);

	/* the tiler job (jm_emit_tiler_job): it waits for its vertex job and for the tiler job before it */
	pack_invocation(pan_section_ptr(tj, TILER_JOB, INVOCATION), vertex_count, instances);
	bool lines = mode == MALI_DRAW_MODE_LINES || mode == MALI_DRAW_MODE_LINE_STRIP;
	pan_section_pack(tj, TILER_JOB, PRIMITIVE, cfg) {
		cfg.draw_mode = mode;
		cfg.first_provoking_vertex = true;
		cfg.job_task_split = 6;
		cfg.low_depth_cull = true;
		cfg.high_depth_cull = true;
		cfg.index_count = indices ? index_count : vertex_count;
		cfg.index_type = !indices ? MALI_INDEX_TYPE_NONE
			       : index_size == 4 ? MALI_INDEX_TYPE_UINT32 : MALI_INDEX_TYPE_UINT16;
		if (indices) {
			cfg.indices = indices;
			cfg.base_vertex_offset = base_vertex_offset;
			if (mode == MALI_DRAW_MODE_TRIANGLE_STRIP || mode == MALI_DRAW_MODE_LINE_STRIP)
				cfg.primitive_restart = MALI_PRIMITIVE_RESTART_IMPLICIT;
		}
		cfg.point_size_array_format = (mode == MALI_DRAW_MODE_POINTS && r->writes_psiz)
						  ? MALI_POINT_SIZE_ARRAY_FORMAT_FP16 : MALI_POINT_SIZE_ARRAY_FORMAT_NONE;
	}
	pan_section_pack(tj, TILER_JOB, PRIMITIVE_SIZE, cfg) {
		if (mode == MALI_DRAW_MODE_POINTS && r->writes_psiz)
			cfg.size_array = psiz.gpu;
		else
			cfg.constant = 1.0f;
	}
	pan_section_pack(tj, TILER_JOB, TILER, cfg) {
		cfg.address = b->tiler_ctx.gpu;
	}
	pan_section_pack(tj, TILER_JOB, PADDING, cfg) {
		(void)cfg;
	}
	pan_section_pack(tj, TILER_JOB, DRAW, cfg) {
		cfg.cull_front_face = false;
		cfg.cull_back_face = false;
		cfg.front_face_ccw = false;
		cfg.position = pos.gpu;
		cfg.state = fsr.gpu;
		cfg.viewport = vpd.gpu;
		cfg.varyings = r->fs_in_count ? vrec.gpu + r->vs_out_count * pan_size(ATTRIBUTE) : 0;
		cfg.varying_buffers = r->fs_in_count ? vbuf.gpu : 0;
		cfg.thread_storage = b->tls.gpu;
		cfg.offset_start = offset_start;
		cfg.instance_size = instances > 1 ? padded : 1;
		cfg.uniform_buffers = fb.ubos;
		cfg.push_uniforms = fb.push;
		cfg.textures = fb.textures;
		cfg.samplers = fb.samplers;
		if (lines)
			cfg.flat_shading_vertex = 0;
	}
	pan_section_pack(tj, TILER_JOB, HEADER, cfg) {
		cfg.type = MALI_JOB_TYPE_TILER;
		cfg.index = tindex;
		cfg.dependency_1 = vindex;
		cfg.dependency_2 = b->last_tiler;
	}
	link_job(b, tj, tjg);
	b->last_tiler = tindex;
	b->draws++;
	if (b->width >= 700 && b->color_count > 1)
		heavy_note(r);
	{
		static unsigned every = ~0u;
		if (every == ~0u)
			every = boot_arg_uint("rwn_vtsplit=", 0);
		if (every && b->width >= 700 && b->color_count > 1 && b->job_index >= 2 * every &&
		    vt_flush(b, why, why_size) != 0)
			return -1;
	}
	(void)d;
	PHASE(6);
#undef PHASE
	return 0;
}

int
rwa_render_draw(struct rwa_render *render, const struct rwa_draw *dr, char *why, size_t why_size)
{
	GUARD();
	uint64_t began = now_ns();
	if (tl_begin == 0) {
		pthread_mutex_lock(&gq_lock);
		if (tl_begin == 0) {
			tl_begin = began;
			tl_begin_busy = gq_head != gq_tail;
			tl_begin_idle = tl_begin_busy || began < gpu_last_done ? 0 : began - gpu_last_done;
		}
		pthread_mutex_unlock(&gq_lock);
	}
	if (render == NULL || dr == NULL) {
		say(why, why_size, "a draw with no pipeline");
		return -1;
	}
	int e = draw(render, dr, why, why_size);
	stats.draw_ns += now_ns() - began;
	if (e != 0) {
		stats.refused++;
		note_refusal(render->fs.name, why);
		return -1;
	}
	stats.draws++;
	return 0;
}

void
rwa_render_destroy(struct rwa_render *render)
{
	GUARD();
	if (render == NULL)
		return;
	if (open_batch) {
		for (unsigned i = 0; i < open_batch->nrenders; i++)
			if (open_batch->renders[i] == render) {
				flush_batch("a pipeline it uses is let go");
				break;
			}
	}
	rwa_stage_free(&render->vs);
	rwa_stage_free(&render->fs);
	free(render);
}

/* --- compute: one dispatch, waited for -------------------------------------------------------------------------------- */

struct rwa_compute *
rwa_compute_create(struct rwa_device *device, const void *bitcode, size_t size, const char *entry,
		   struct rwa_compute_info *info, char *why, size_t why_size)
{
	struct rwa_compute *c = calloc(1, sizeof(*c));

	if (c == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	c->device = device;
	if (rwa_stage_compile(device, &c->cs, bitcode, size, entry, MESA_SHADER_COMPUTE, NULL, 0, NULL, 0, NULL, why,
			      why_size) != 0) {
		free(c);
		return NULL;
	}
	if (info) {
		memset(info, 0, sizeof(*info));
		info->max_threads_per_group = 256;
		info->thread_execution_width = 4;
		info->static_shared_bytes = c->cs.air.threadgroup_bytes;
		count_stage(&c->cs, &info->buffers, &info->textures, &info->samplers, &info->argbuf_mask);
	}
	return c;
}

void
rwa_compute_destroy(struct rwa_compute *compute)
{
	GUARD();
	if (compute == NULL)
		return;
	rwa_stage_free(&compute->cs);
	free(compute);
}

int
rwa_dispatch_buffers(struct rwa_compute *c, const struct rwa_bindings *bindings, const struct rwa_buffer_bind *table,
		     unsigned table_count, const unsigned groups[3], const unsigned group_size[3], char *why,
		     size_t why_size)
{
	GUARD();
	flush_batch("a dispatch");
	if (groups[0] == 0 || groups[1] == 0 || groups[2] == 0)
		return 0;
	if (c->cs.info.wls_size || c->cs.air.threadgroup_bytes) {
		say(why, why_size, "%s uses threadgroup memory (not served yet by the native library)", c->cs.name);
		return -1;
	}
	struct batch *b = calloc(1, sizeof(*b));
	if (b == NULL) {
		say(why, why_size, "out of memory");
		return -1;
	}
	b->mark = ++batch_marks;
	b->tls = pool_alloc(b, pan_size(LOCAL_STORAGE), 64, why, why_size);
	struct ptr job = pool_alloc(b, pan_size(COMPUTE_JOB), 64, why, why_size);
	struct ptr rsd = pool_alloc(b, pan_size(RENDERER_STATE), 64, why, why_size);
	if (!b->tls.cpu || !job.cpu || !rsd.cpu) {
		batch_free(b, 0);
		return -1;
	}
	use_bo(b, the_device->dummy);
	use_bo(b, c->cs.code);
	memcpy(rsd.cpu, c->cs.rsd, 64);
	struct bound cb;
	struct sv_input svi = { .groups = { groups[0], groups[1], groups[2] },
				.local = { group_size[0], group_size[1], group_size[2] } };
	if (emit_stage(b, &c->cs, bindings, table, table_count, &svi, &cb, why, why_size) != 0) {
		batch_free(b, 0);
		return -1;
	}
	unsigned scratch_size = c->cs.info.tls_size;
	struct ptr scratch = { NULL, 0 };
	if (scratch_size) {
		unsigned total = panfrost_get_total_stack_size(scratch_size, thread_tls_alloc ? thread_tls_alloc : 256,
							       core_id_range);
		scratch = pool_big(b, total, why, why_size);   /* only the GPU writes it: not copied */
		if (scratch.cpu == NULL) {
			batch_free(b, 0);
			return -1;
		}
	}
	pan_cast_and_pack((b->tls.cpu), LOCAL_STORAGE, cfg) {
		if (scratch_size) {
			cfg.tls_size = panfrost_get_stack_shift(scratch_size);
			cfg.tls_base_pointer = scratch.gpu;
		}
		cfg.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
	}
	panfrost_pack_work_groups_compute(pan_section_ptr(job.cpu, COMPUTE_JOB, INVOCATION), groups[0], groups[1],
					  groups[2], group_size[0], group_size[1], group_size[2], false, false);
	pan_section_pack(job.cpu, COMPUTE_JOB, PARAMETERS, cfg) {
		cfg.job_task_split = util_logbase2_ceil(group_size[0] + 1) + util_logbase2_ceil(group_size[1] + 1) +
				     util_logbase2_ceil(group_size[2] + 1);
	}
	pan_section_pack(job.cpu, COMPUTE_JOB, DRAW, cfg) {
		cfg.state = rsd.gpu;
		cfg.thread_storage = b->tls.gpu;
		cfg.uniform_buffers = cb.ubos;
		cfg.push_uniforms = cb.push;
		cfg.textures = cb.textures;
		cfg.samplers = cb.samplers;
	}
	pan_section_pack(job.cpu, COMPUTE_JOB, HEADER, cfg) {
		cfg.type = MALI_JOB_TYPE_COMPUTE;
		cfg.index = 1;
		cfg.barrier = true;
	}
	for (unsigned i = 0; i < b->nchunks; i++)
		use_bo(b, b->chunks[i]);
	pool_upload(b);
	uint64_t seq = 0;
	int e = km_submit(job.gpu, false, b->bos, b->nbos, &seq);
	if (e == 0)
		e = km_wait(seq, 2000000000);
	uint32_t status = 0;
	memcpy(&status, pool_real(b, job.cpu), 4);
	batch_free(b, seq);
	if (e != 0 || (status & 0xff) != 1) {
		say(why, why_size, "%s: the compute job %s (status %#x)", c->cs.name, e ? strerror(e) : "did not finish DONE",
		    status);
		return -1;
	}
	return 0;
}

int
rwa_dispatch(struct rwa_compute *compute, const struct rwa_bindings *bindings, const unsigned groups[3],
	     const unsigned group_size[3], char *why, size_t why_size)
{
	return rwa_dispatch_buffers(compute, bindings, NULL, 0, groups, group_size, why, why_size);
}

/* --- submission and waiting ---------------------------------------------------------------------------------------- */

uint64_t
rwa_flush_seq(struct rwa_device *device)
{
	GUARD();
	(void)device;
	commits++;
	flush_batch("the command buffer is committed");
	/* a command buffer is a frame of the window server's (one a frame, run at -commit on its own thread without
	 * rwm_async=1, and rwa_surface_seq not called then): the time since the one before, and its first draw's */
	{
		static uint64_t last;
		uint64_t now = now_ns(), was = last;
		last = now;
		pthread_mutex_lock(&gq_lock);
		if (was) {
			uint64_t gap = (now - was + 8333333) / 16666667;   /* refreshes, rounded */
			frame_gap[MIN2(MAX2(gap, 1), 7) - 1]++;
		}
		{
			uint64_t sec = (now - lib_up_ns) / 1000000000ull;
			if (sec >= sec_base && sec - sec_base < 32 && sec_frames[sec - sec_base] < 0xffff)
				sec_frames[sec - sec_base]++;
		}
		if (was && now - was < 80000000ull && tl_begin && tl_begin < now) {
			tl_frames++;
			tl_span_ns += now - tl_begin;
			if (tl_begin_busy) {
				tl_busy++;
			} else {
				tl_idle++;
				tl_idle_ns += tl_begin_idle;
			}
		}
		tl_begin = 0;
		pthread_mutex_unlock(&gq_lock);
	}
	return km_last_seq();
}

int
rwa_wait_seq(uint64_t seq, int64_t timeout_ns)
{
	return km_wait(seq, timeout_ns);
}



int
rwa_surface_seq(uint32_t surface, uint64_t seq)
{
	if (seq != ~0ull)
		surface_frames++;
	return km_surface_seq(surface, seq);
}

int
rwa_finish(struct rwa_device *device, char *why, size_t why_size)
{
	GUARD();
	(void)device;
	flush_batch("finish");
	uint64_t seq = km_last_seq();
	int e = km_wait(seq, 5000000000ll);
	if (e != 0) {
		say(why, why_size, "the GPU did not finish job %llu: %s", (unsigned long long)seq, strerror(e));
		return -1;
	}
	return 0;
}

void
rwa_draw_profile(uint64_t out[7])
{
	memset(out, 0, 7 * sizeof(uint64_t));
	out[4] = stats.draw_ns;
	out[6] = stats.draws;
}

void
rwa_submit_profile(uint64_t out[5])
{
	memset(out, 0, 5 * sizeof(uint64_t));
	out[0] = stats.batches;
	out[1] = stats.flush_ns;
}

static void
report(void)
{
	uint64_t now = now_ns();

	if (now - stats.since < 30000000000ull)
		return;
	uint64_t k[4];
	km_counts(k);
	{
		char line[300];
		int n = snprintf(line, sizeof(line), "30s: fragment jobs %llu checked, %llu failed;",
				 (unsigned long long)frag_checked, (unsigned long long)frag_failed);
		for (unsigned f = 0; f < NFLAGS; f++) {
			n += snprintf(line + n, sizeof(line) - n, " %s %llu/%llu", flag_names[f],
				      (unsigned long long)flag_failed[f], (unsigned long long)flag_total[f]);
			flag_failed[f] = flag_total[f] = 0;
		}
		keep("%s", line);
	}
	frag_checked = frag_failed = 0;
	{
		/* microseconds a draw, by part (phase_ns) */
		uint64_t n = MAX2(stats.draws + stats.refused, 1);
		keep("30s: us a draw: checks %.1f, batch %.1f, index+viewport %.1f, vs %.1f, fs %.1f, attrs %.1f, "
		     "varyings+jobs %.1f; %llu cleared instead of loaded; %llu surface frames (%.1f a second), %llu passes into "
		     "surfaces (%.1f a second)",
		     phase_ns[0] / 1e3 / n, phase_ns[1] / 1e3 / n, phase_ns[2] / 1e3 / n, phase_ns[3] / 1e3 / n,
		     phase_ns[4] / 1e3 / n, phase_ns[5] / 1e3 / n, phase_ns[6] / 1e3 / n,
		     (unsigned long long)stats.multiload_cleared, (unsigned long long)surface_frames,
		     surface_frames * 1e9 / (double)MAX2(now - stats.since, 1), (unsigned long long)surface_passes,
		     surface_passes * 1e9 / (double)MAX2(now - stats.since, 1));
		surface_passes = 0;
		memset(phase_ns, 0, sizeof(phase_ns));
		surface_frames = 0;
		keep("30s: us a draw, vs/fs: buffers %.1f/%.1f, textures %.1f/%.1f, samplers %.1f/%.1f, sysvals %.1f/%.1f, "
		     "pushed %.1f/%.1f; upload %.1f us a pass, %.1f KB a pass; buffers off 16 bytes %llu copied (%.1f KB "
		     "each), %llu given as they are; %llu vertex and tiler parts sent ahead",
		     stage_ns[0][0] / 1e3 / n, stage_ns[1][0] / 1e3 / n, stage_ns[0][1] / 1e3 / n, stage_ns[1][1] / 1e3 / n,
		     stage_ns[0][2] / 1e3 / n, stage_ns[1][2] / 1e3 / n, stage_ns[0][3] / 1e3 / n, stage_ns[1][3] / 1e3 / n,
		     stage_ns[0][4] / 1e3 / n, stage_ns[1][4] / 1e3 / n, upload_ns / 1e3 / MAX2(stats.batches, 1),
		     upload_bytes / 1024.0 / MAX2(stats.batches, 1), (unsigned long long)ubo_copied,
		     ubo_copied_bytes / 1024.0 / MAX2(ubo_copied, 1), (unsigned long long)ubo_given,
		     (unsigned long long)stats.vt_parts);
		ubo_copied = ubo_copied_bytes = ubo_given = 0;
		stats.vt_parts = 0;
		keep("30s: texture probe: %llu looked at, one colour: %llu white, %llu zero, %llu other, %llu LOST (held more before)",
		     (unsigned long long)probe_count, (unsigned long long)probe_white, (unsigned long long)probe_zero,
		     (unsigned long long)probe_flat, (unsigned long long)probe_lost);
		probe_count = probe_white = probe_zero = probe_flat = probe_lost = 0;
		keep("30s: attachments at a pass's start %llu loaded, %llu cleared, %llu DontCare; at its end %llu "
		     "stored, %llu DontCare (not written back)",
		     (unsigned long long)stats.att_loaded, (unsigned long long)stats.att_cleared,
		     (unsigned long long)stats.att_dontcare_load, (unsigned long long)stats.att_stored,
		     (unsigned long long)stats.att_discarded);
		{
			char line[460];
			int n = snprintf(line, sizeof(line), "30s: full passes' pipelines by draws:");
			for (unsigned round = 0; round < 8 && n < (int)sizeof(line) - 50; round++) {
				unsigned best = ~0u;
				for (unsigned i = 0; i < nheavy; i++)
					if (heavy[i].draws && (best == ~0u || heavy[i].draws > heavy[best].draws))
						best = i;
				if (best == ~0u)
					break;
				n += snprintf(line + n, sizeof(line) - n, " %s%s%s %llu;", heavy[best].name,
					      heavy[best].bs ? " bs" : "", heavy[best].rd ? " rd" : "",
					      (unsigned long long)heavy[best].draws);
				heavy[best].draws = 0;
			}
			keep("%s", line);
			nheavy = 0;
		}
		pthread_mutex_lock(&gq_lock);
		double span = (double)MAX2(now - stats.since, 1);
		keep("30s: GPU busy %.0f%%, a pass submit to done %.1f ms (at most %.1f), %llu passes waited, %llu queued; "
		     "%llu command buffers committed (%.1f a second)",
		     100.0 * gpu_busy_ns / span, gpu_latency_ns / 1e6 / MAX2(gpu_passes, 1), gpu_latency_max / 1e6,
		     (unsigned long long)gpu_passes, (unsigned long long)(gq_head - gq_tail), (unsigned long long)commits,
		     commits * 1e9 / span);
		{
			static const char *const wname[4] = { "full", "mid", "small", "tiny" };
			char line[400];
			int n = snprintf(line, sizeof(line), "30s: GPU by pass kind (passes, draws, ms vertex+tiler / fragment):");
			for (unsigned k = 0; k < 16 && n < (int)sizeof(line) - 60; k++)
				if (kind_passes[k])
					n += snprintf(line + n, sizeof(line) - n, " %s%s%s %llu %llu %.0f/%.0f;", wname[k / 4],
						      (k & 2) ? "-multi" : "", (k & 1) ? "-load" : "",
						      (unsigned long long)kind_passes[k], (unsigned long long)kind_draws[k],
						      kind_vt_ns[k] / 1e6, kind_frag_ns[k] / 1e6);
			keep("%s", line);
			{
				uint64_t g[7];
				for (unsigned i = 0; i < 7; i++) {   /* (gq_lock held) */
					g[i] = frame_gap[i];
					frame_gap[i] = 0;
				}
				/* swipes: how many frames came after 2 refreshes (30 a second), 3 (20), ... -- the idle 10 a second
				 * is 6 */
				keep("30s: command buffers by the refreshes since the one before: 1 %llu, 2 %llu, 3 %llu, "
				     "4 %llu, 5 %llu, 6 %llu, more %llu", (unsigned long long)g[0], (unsigned long long)g[1],
				     (unsigned long long)g[2], (unsigned long long)g[3], (unsigned long long)g[4],
				     (unsigned long long)g[5], (unsigned long long)g[6]);
			}
			/* (gq_lock held) */
			{
				uint64_t now_sec = (now_ns() - lib_up_ns) / 1000000000ull;
				unsigned span = (unsigned)MIN2(now_sec - MIN2(sec_base, now_sec), 32);
				char fl[160] = "", pl[160] = "";
				int fn = 0, pn = 0;
				bool any_pipe = false;
				for (unsigned i = 0; i < span; i++) {
					fn += snprintf(fl + fn, sizeof(fl) - fn, "%s%u", i ? " " : "", sec_frames[i]);
					pn += snprintf(pl + pn, sizeof(pl) - pn, "%s%u", i ? " " : "", sec_pipes[i]);
					any_pipe |= sec_pipes[i] != 0;
				}
				keep("30s: from %llu s, command buffers each second: %s%s%s", (unsigned long long)sec_base, fl,
				     any_pipe ? "; pipelines made: " : "", any_pipe ? pl : "");
				memset(sec_frames, 0, sizeof(sec_frames));
				memset(sec_pipes, 0, sizeof(sec_pipes));
				sec_base = now_sec;
			}
			if (tl_frames)
				keep("30s: %llu frames in motion: the first draw with the GPU at the frame before %llu, after it went "
				     "idle %llu (%.1f ms after); first draw to commit %.1f ms", (unsigned long long)tl_frames,
				     (unsigned long long)tl_busy, (unsigned long long)tl_idle, tl_idle_ns / 1e6 / MAX2(tl_idle, 1),
				     tl_span_ns / 1e6 / tl_frames);
			tl_frames = tl_busy = tl_idle = tl_idle_ns = tl_span_ns = 0;
			uint64_t bp = __atomic_exchange_n(&bound_passes, 0, __ATOMIC_RELAXED);
			uint64_t bt = __atomic_exchange_n(&bound_tiles, 0, __ATOMIC_RELAXED);
			uint64_t bo = __atomic_exchange_n(&bound_of, 0, __ATOMIC_RELAXED);
			if (bp)
				keep("30s: %llu passes that clear nothing bounded by their draws: %.0f%% of their tiles gone through",
				     (unsigned long long)bp, 100.0 * bt / MAX2(bo, 1));
			if (prof_passes[0] || prof_passes[1] || prof_passes[2] || prof_passes[3]) {
				static const char *const gname[4] = { "all", "no path_*", "no glass_*", "no plain layers" };
				n = snprintf(line, sizeof(line), "30s: full passes, ms a pass (draws) with:");
				for (unsigned g = 0; g < 4; g++)
					n += snprintf(line + n, sizeof(line) - n, " %s %.1f (%.0f);", gname[g],
						      prof_ns[g] / 1e6 / MAX2(prof_passes[g], 1),
						      (double)prof_draws[g] / MAX2(prof_passes[g], 1));
				keep("%s", line);
				memset(prof_passes, 0, sizeof(prof_passes));
				memset(prof_draws, 0, sizeof(prof_draws));
				memset(prof_ns, 0, sizeof(prof_ns));
			}
			if (narea_tab) {
				n = snprintf(line, sizeof(line), "30s: full passes, pipelines' draws, instances, Mpx (first vertex %.0f,%.0f):",
					     area_sample[0], area_sample[1]);
				for (unsigned round = 0; round < 10 && n < (int)sizeof(line) - 50; round++) {
					unsigned best = ~0u;
					for (unsigned t = 0; t < narea_tab; t++)
						if (area_tab[t].draws && (best == ~0u || area_tab[t].pixels > area_tab[best].pixels))
							best = t;
					if (best == ~0u)
						break;
					n += snprintf(line + n, sizeof(line) - n, " %.18s %llu %llu %.0f;", area_tab[best].name,
						      (unsigned long long)area_tab[best].draws,
						      (unsigned long long)area_tab[best].instances, area_tab[best].pixels / 1e6);
					area_tab[best].draws = 0;
					area_tab[best].pixels = 0;
				}
				keep("%s", line);
				memset(area_tab, 0, sizeof(area_tab));
				narea_tab = 0;
			}
			memset(kind_passes, 0, sizeof(kind_passes));
			memset(kind_draws, 0, sizeof(kind_draws));
			memset(kind_vt_ns, 0, sizeof(kind_vt_ns));
			memset(kind_frag_ns, 0, sizeof(kind_frag_ns));
		}
		gpu_busy_ns = gpu_latency_ns = gpu_passes = gpu_latency_max = 0;
		commits = 0;
		pthread_mutex_unlock(&gq_lock);
		memset(stage_ns, 0, sizeof(stage_ns));
		upload_ns = upload_bytes = 0;
	}
	keep("30s: pipeline compiling, ms: AIR to NIR %llu, lowering %llu, Bifrost compiler %llu",
	     (unsigned long long)(rwa_compile_ns[0] / 1000000), (unsigned long long)(rwa_compile_ns[1] / 1000000),
	     (unsigned long long)(rwa_compile_ns[2] / 1000000));
	memset(rwa_compile_ns, 0, sizeof(rwa_compile_ns));
	keep("30s: at %llu s of the library", (unsigned long long)((now - lib_up_ns) / 1000000000ull));
	keep("30s: %llu draws (%llu refused), %llu passes, %llu loaded, %llu cleared, %llu waits by the processor; "
	     "draws %llu us, submits %llu us; %llu pipelines in %llu ms (%llu refused), %llu surfaces; buffers %llu made, "
	     "%llu reused, %llu KB kept",
	     (unsigned long long)stats.draws, (unsigned long long)stats.refused, (unsigned long long)stats.batches,
	     (unsigned long long)stats.preloads, (unsigned long long)stats.clears, (unsigned long long)stats.cpu_syncs,
	     (unsigned long long)(stats.draw_ns / 1000), (unsigned long long)(stats.flush_ns / 1000),
	     (unsigned long long)stats.pipelines, (unsigned long long)(stats.pipeline_ns / 1000000),
	     (unsigned long long)stats.refused_pipelines, (unsigned long long)stats.imports, (unsigned long long)k[0],
	     (unsigned long long)k[1], (unsigned long long)(k[3] >> 10));
	memset(&stats, 0, sizeof(stats));
	stats.since = now;
}
