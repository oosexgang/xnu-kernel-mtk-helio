/*
 * libhakiosmali.dylib's insides: the contract of tools/mesa/air/air_metal.h (rwa_*) served by writing the Mali-G52's
 * job descriptors here, with no Gallium and no Panfrost driver underneath -- Mesa's AIR front end and Bifrost compiler
 * make the shaders, genxml's v7 packers lay out the descriptors, and HakiosMali (kmali.h) takes the job chains.
 *
 * rwa_shader.c compiles (AIR -> NIR -> Bifrost, with this library's own system values) and prepares the fixed state of a
 * pipeline; rwa_mali.c is the contract: objects, passes as batches of draws, the frame descriptors and the submits.
 */
#ifndef RWA_PRIV_H
#define RWA_PRIV_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "air_metal.h"
#include "kmali.h"
#include "nir_air.h"

#include "compiler/nir/nir.h"
#include "util/format/u_format.h"
#include "pan_earlyzs.h"
#include "panfrost/util/pan_ir.h"

#define RWA_MAX_SYSVALS 64
#define RWA_MAX_SLOTS 64           /* textures, samplers: the flat indices argument buffers allocate go past 31 */
#define RWA_MAX_UBOS 96
#define RWA_MAX_VARYINGS 32
#define RWA_ATTR_ZERO 0xff

/* this library's system values (panfrost_nir_lower_sysvals' kinds, our numbering): one vec4 each in the stage's
 * system value buffer, the last uniform buffer of its table */
enum rwa_sysval {
	RWA_SV_VIEWPORT_SCALE = 1,
	RWA_SV_VIEWPORT_OFFSET,
	RWA_SV_VERTEX_OFFSETS,        /* x offset_start, y base vertex, z base instance (PAN_SYSVAL_VERTEX_INSTANCE_OFFSETS) */
	RWA_SV_SSBO,                  /* arg: the binding; x,y the address, z the size left */
	RWA_SV_NUM_WORKGROUPS,
	RWA_SV_LOCAL_SIZE,
	RWA_SV_TEXTURE_SIZE,          /* arg: texture | dims << 8 | array << 12 */
	RWA_SV_SAMPLE_POSITIONS,
	RWA_SV_MULTISAMPLED,
	RWA_SV_RT_CONVERSION,         /* arg: rt | size << 4 */
	RWA_SV_NUM_VERTICES,
	RWA_SV_DRAW_ID,
	RWA_SV_TEXTURE_LEVELS,        /* arg: texture */
	RWA_SV_UBO_MISALIGN,          /* arg: the buffer; x: how far its binding is past the 16 bytes its descriptor starts on */
};
#define RWA_SV(kind, arg) ((uint32_t)(kind) | ((uint32_t)(arg) << 8))
#define RWA_SV_KIND(sv) ((sv) & 0xff)
#define RWA_SV_ARG(sv) ((sv) >> 8)

/* one compiled stage */
extern uint64_t rwa_compile_ns[3];

/* a MediaTek Helio chip and its GPU (rwa_helio.c) */
struct rwa_helio {
	const char *soc;              /* hw.board.soc, soc/NAME.toml: "mt6768" */
	const char *name;             /* "Helio G85 (MT6768)" */
	const char *gpu;              /* "Mali-G52 MC2" */
	unsigned gpu_id[2];           /* Panfrost's product ids (0: no Mali) */
	bool run;                     /* run on a phone with this library */
};
const struct rwa_helio *rwa_helio_find(const char *soc, unsigned gpu_id);
const char *rwa_board_soc(void);
const char *rwa_arch_status(unsigned arch, unsigned gpu_id);

struct rwa_stage {
	void *mem;                    /* ralloc context: the AIR info's strings and arrays live in it */
	struct air_shader_info air;
	struct pan_shader_info info;
	struct km_bo *code;           /* the binary, in GPU memory */
	unsigned code_size;
	uint32_t sysvals[RWA_MAX_SYSVALS];
	unsigned sysval_count;
	unsigned sysval_ubo;          /* its index in the table (= ubo_count - 1 when there are system values) */
	unsigned ubo_count;           /* entries of the uniform buffer table */
	unsigned texture_count, sampler_count;
	uint32_t inline_sampler[RWA_MAX_SLOTS][8];   /* a constexpr sampler's descriptor, packed at creation */
	uint64_t inline_mask;
	uint32_t rsd[16];             /* the Renderer State words of this stage alone, the shader's address merged in */
	char name[96];
	/* a vertex stage's inputs, compacted (air_metal.c compact_vertex_inputs): driver location -> [[attribute(n)]] */
	uint8_t vs_attr[32];
	unsigned vs_attr_count;
	uint32_t noperspective;       /* a fragment stage's noperspective inputs (pan_nir_collect_noperspective_varyings_fs) */
	/* uniform buffers read at offsets the shader computes: their loads add RWA_SV_UBO_MISALIGN, and a binding off
	 * 16 bytes is given as it is, its descriptor on the 16 bytes before it, instead of copied */
	uint64_t ubo_dynamic[2];
	/* the others: how far into the buffer the shader reads -- what a copy needs; 0 with ubo_extent_known: not read at
	 * all (its loads folded away, or pushed from words it no longer reads) */
	uint32_t ubo_extent[RWA_MAX_UBOS];
	bool ubo_extent_known;        /* every uniform load names its buffer by a constant */
};

struct rwa_varying_rec {
	uint32_t format;              /* the Mali format word */
	int32_t offset;
	uint8_t buffer;               /* 0 general, 1 position, 2 point size */
};

/* the vertex attribute layout, by the shader's driver location (pan_shader_info.attribute_count of them) */
struct rwa_attr_rec {
	uint8_t buffer;               /* Metal's buffer index; RWA_ATTR_ZERO: no buffer, zeros (a function constant's) */
	uint8_t step;                 /* MTLVertexStepFunction */
	uint32_t format;              /* the Mali format word */
	uint32_t offset;
	uint32_t step_rate;
};

struct rwa_device {
	char name[64];
	unsigned gpu_id;
	unsigned tile_budget;         /* the tile buffer's bytes a tile may take (panfrost_query_optimal_tib_size / 2) */
	struct km_bo *sample_positions;   /* the 768-byte table (pan_samples.c) */
	struct km_bo *tiler_heap;
	struct km_bo *dummy;          /* a zeroed page: unbound buffers and textures point here */
	struct rwa_stage preload[256]; /* the frame shaders that load the targets of a mask (preload[mask], made on demand) */
	bool preload_ready;
	uint32_t null_texture[8];
};

struct rwa_render {
	struct rwa_device *device;
	struct rwa_stage vs, fs;
	struct rwa_render_shape shape;
	unsigned rt_count;
	enum pipe_format rt_format[8];
	uint32_t blend[8][4];         /* each target's Blend descriptor */
	unsigned reads_dest_mask;     /* targets whose blend reads the destination */
	uint32_t fs_rsd[16];          /* the fragment Renderer State before the draw's depth, stencil and samples */
	struct pan_earlyzs_lut earlyzs;
	enum pipe_format zs_format;
	unsigned samples;
	unsigned attr_count;
	struct rwa_attr_rec attr[32];
	struct rwa_varying_rec vs_out[RWA_MAX_VARYINGS], fs_in[RWA_MAX_VARYINGS];
	unsigned vs_out_count, fs_in_count;
	unsigned general_stride;      /* bytes a vertex of the linked varyings */
	bool writes_psiz;
	unsigned fs_writes, fs_fetches;
	bool fs_writes_depth, fs_writes_stencil, can_discard;
	struct rwa_render_info info;
	unsigned blend_shader_mask;   /* targets blended by a shader (in the fragment shader's buffer) */
	void *blend_bin[8];
	unsigned blend_bin_size[8], blend_first_tag[8];
};

struct rwa_compute {
	struct rwa_device *device;
	struct rwa_stage cs;
	unsigned local[3];
	unsigned wls_size;
};

/* rwa_shader.c */
/* the fragment stage's noperspective set, for the vertex stage compiled next on this thread */
extern _Thread_local uint32_t rwa_next_noperspective;
int rwa_shader_init(struct rwa_device *device, char *why, size_t why_size);
int rwa_preload_shader(struct rwa_device *device, struct rwa_stage *st, unsigned mask, char *why, size_t why_size);
/* AIR's stage to a compiled stage: CONSTANTS and CONFIG as rwa_render_constants and rwa_render_config gave them */
int rwa_stage_compile(struct rwa_device *device, struct rwa_stage *st, const void *bitcode, size_t size, const char *entry,
		      gl_shader_stage want, const struct rwa_constant *constants, unsigned nconst, const void *config,
		      unsigned config_size, const struct rwa_render_shape *shape, char *why, size_t why_size);
void rwa_stage_free(struct rwa_stage *st);
/* a render pipeline's fixed state, once both stages are compiled */
int rwa_render_prepare(struct rwa_render *r, char *why, size_t why_size);
/* the fragment Renderer State of a draw (64 bytes into OUT): the pipeline's, with depth and stencil state ZS (NULL: no
 * test and no write), whether the pass has a depth/stencil attachment, and its samples */
void rwa_render_fs_rsd(const struct rwa_render *r, const struct rwa_depth_stencil_state *zs, bool has_zs, unsigned samples,
		       void *out);
/* Metal's pixel format: the pipe format, or PIPE_FORMAT_NONE; and its name */
enum pipe_format rwa_pipe_format(unsigned mtl);
const char *rwa_pixel_format_name(unsigned mtl);
/* a texture descriptor for a 2D or 2D array (type) linear image (32 bytes), its surfaces at SURFACES */
void rwa_texture_desc(void *out, enum pipe_format format, const unsigned char user_swizzle[4], unsigned dim,
		      unsigned width, unsigned height, unsigned depth, unsigned levels, unsigned layers, unsigned samples,
		      uint64_t surfaces);
void rwa_surface_desc(void *out, uint64_t pointer, int32_t row_stride, int32_t surface_stride);
/* a sampler descriptor (32 bytes) of Metal's state; COMPARE: the comparing one */
int rwa_sampler_desc(void *out, const struct rwa_sampler_state *s, bool compare, char *why, size_t why_size);
/* what a render target is (pan_rt_init_format): internal and writeback format words, swizzle and the tile buffer's bytes
 * a sample; false: not a target */
bool rwa_rt_blendable(enum pipe_format f);
bool rwa_no_blend_shaders(void);
bool rwa_samplable(enum pipe_format f);
bool rwa_rt_format(enum pipe_format f, uint32_t *internal, uint32_t *writeback, uint32_t *swizzle, unsigned *bpp);
/* the clear colour of a target as the tile buffer holds it (pan_pack_color) */
void rwa_pack_clear(enum pipe_format f, const float rgba[4], uint32_t out[4]);
uint32_t rwa_attribute_format(enum pipe_format f);
enum pipe_format rwa_vertex_format(unsigned mtl_vertex_format, unsigned *size);
uint64_t rwa_rt_conversion(enum pipe_format f, unsigned rt, unsigned size);

#endif
