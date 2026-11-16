/*
 * The Mali-G52's descriptors (Bifrost, architecture v7), written and read through Mesa's own packers.
 *
 * Every word this library hands the GPU is packed by the code Mesa's src/panfrost/lib/genxml/gen_pack.py
 * generates from v7.xml (genxml/v7_pack.h, made at build time from the Mesa tree beside this repository:
 * kernel/deps/build/mesa). No field position is written here by hand: v7.c is the only file that includes
 * the generated header, and what it exposes is plain C, so the rest of the library (C++) never sees the
 * macros. What each function fills in, and why, is the minimal path Panfrost's own driver takes
 * (src/panfrost/lib/pan_desc.c, src/gallium/drivers/panfrost/pan_jm.c), each case cited at the function.
 *
 * The readers (hm_v7_read_*) are for the fake GPU (fake_gpu.cpp) and for the probe's dumps: they unpack
 * with the generated unpackers, and a descriptor with a bit set where v7.xml has no field is refused with
 * the sentence the generated unpacker says ("Invalid field of Fragment Job Payload unpacked at word 6").
 */
#ifndef HAKIOS_MALI_V7_H
#define HAKIOS_MALI_V7_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MALI_JOB_TYPE_* (v7.xml "Job Type"); v7.c checks each against the generated enum */
enum hm_job_type {
	HM_JOB_NOT_STARTED = 0,
	HM_JOB_NULL = 1,
	HM_JOB_WRITE_VALUE = 2,
	HM_JOB_CACHE_FLUSH = 3,
	HM_JOB_COMPUTE = 4,
	HM_JOB_VERTEX = 5,
	HM_JOB_TILER = 7,
	HM_JOB_FRAGMENT = 9,
};

/* the sizes and alignments of what is written, from the generated header (pan_size, pan_alignment) */
struct hm_v7_sizes {
	unsigned job_header, fragment_job, write_value_job, framebuffer, render_target, zs_crc_extension,
	    tiler_context, tiler_heap, sample_positions, job_align, framebuffer_align, tiler_align;
	unsigned vertex_job, tiler_job, renderer_state, blend, attribute, attribute_buffer, viewport, local_storage,
	    uniform_buffer, surface_with_stride, texture, sampler;
};
const struct hm_v7_sizes *hm_v7_sizes(void);

/* the formats a render target can be here: what pan_rt_init_format makes of PIPE_FORMAT_R8G8B8A8_UNORM and
 * PIPE_FORMAT_B8G8R8A8_UNORM (pan_format.c's blendable table: internal R8G8B8A8, writeback R8G8B8A8, and the
 * rendering swizzle, the inverse of the format's) */
enum hm_rt_format {
	HM_RT_RGBA8_UNORM = 1,
	HM_RT_BGRA8_UNORM = 2,
	HM_RT_R8_UNORM = 3,
	HM_RT_RG8_UNORM = 4,
};
/* its writeback format (MALI_COLOR_FORMAT_*: pan_format.c's blendable table, internal R8G8B8A8 for all four) */
unsigned hm_v7_rt_writeback(enum hm_rt_format format);


/* --- writing ---------------------------------------------------------------------------------------------- */

/* A Job Header (the first 32 bytes of every job): TYPE, INDEX (1-based, its place in the chain), NEXT (0: the
 * last job), BARRIER; exception status 0 for the GPU to fill in. Is 64b is v7.xml's default (true). */
void hm_v7_job_header(void *job, enum hm_job_type type, unsigned index, uint64_t next, bool barrier);

/* A whole Write Value Job (header and payload): the GPU writes VALUE, 32 bits, at TARGET ("Immediate 32"). */
void hm_v7_write_value_job(void *job, unsigned index, uint64_t next, uint64_t target, uint32_t value);

/* A Framebuffer Descriptor ("Framebuffer": Parameters and Padding) and its render targets right after it, as
 * GENX(pan_emit_fbd) lays them out (no ZS CRC extension). */
/*
 * Depth and stencil state (panfrost_create_depth_stencil_state, and the reference panfrost_emit_frag_shader merges):
 * Metal's MTLCompareFunction, whose numbers are Mali's (MALI_FUNC_*), and MTLStencilOperation, whose are not
 */
enum hm_compare { HM_COMPARE_NEVER, HM_COMPARE_LESS, HM_COMPARE_EQUAL, HM_COMPARE_LEQUAL, HM_COMPARE_GREATER,
	HM_COMPARE_NOT_EQUAL, HM_COMPARE_GEQUAL, HM_COMPARE_ALWAYS };
enum hm_stencil_op { HM_STENCIL_KEEP, HM_STENCIL_ZERO, HM_STENCIL_REPLACE, HM_STENCIL_INCR_CLAMP, HM_STENCIL_DECR_CLAMP,
	HM_STENCIL_INVERT, HM_STENCIL_INCR_WRAP, HM_STENCIL_DECR_WRAP };
struct hm_v7_stencil {
	enum hm_compare compare;
	enum hm_stencil_op fail, depth_fail, pass;
	uint8_t read_mask, write_mask;
};
struct hm_v7_zs {
	enum hm_compare depth_compare;
	bool depth_write;
	bool stencil_enable;
	struct hm_v7_stencil front, back;
	uint8_t reference;
};
/* ZS into the Renderer State RSD (words 8 to 11, the rest kept): 0, or -1 with the unpacker's sentence in *WHY */
int hm_v7_renderer_state_zs(void *rsd, const struct hm_v7_zs *zs, const char **why);
/* the rasterizer's multisampling into the Renderer State RSD (panfrost_create_rasterizer_state: word 8
 * multisample_enable, word 9 single_sampled_lines = !multisample): 0, or -1 with *WHY */
int hm_v7_renderer_state_multisample(void *rsd, bool multisample, const char **why);
/* the byte offset of SAMPLES' pattern in the sample positions table (panfrost_sample_positions_offset), and that table:
 * pan_samples.c's entries for single sampling and ROTATED_4X_GRID, 256 bytes each at their patterns' offsets */
unsigned hm_v7_sample_pattern_offset(unsigned samples);
#define HM_V7_SAMPLE_POSITIONS_SIZE 768
void hm_v7_sample_positions_table(void *out);
/* a Tiler Context for a frame of SAMPLES (jm_emit_tiler_desc: tiler.sample_pattern) */
void hm_v7_tiler_context_samples(void *out, uint64_t heap, uint32_t fb_width, uint32_t fb_height, uint32_t hierarchy_mask,
    unsigned samples);

struct hm_v7_fb {
	uint32_t width, height;           /* pixels */
	uint32_t bound_min_x, bound_min_y, bound_max_x, bound_max_y;   /* pixels, inclusive (pan_emit_fbd: 0..w-1) */
	uint64_t sample_locations;        /* the single-sampled entry of the sample positions (hm_v7_sample_positions) */
	uint64_t tiler;                   /* a Tiler Context, or 0: a frame without primitives (pan_jm.c builds none) */
	uint32_t tile_size;               /* pixels a tile (pan_select_tile_size): a power of two, 16 to 256 */
	uint32_t cbuf_allocation;         /* bytes of tile buffer for the colour (pan_select_tile_size), 1 KB units */
	uint32_t rt_count;                /* render targets that follow, 1 to 8 */
	float z_clear;                    /* the tile buffer's depth at the frame's start (pan_emit_fbd: zs.clear_value) */
	uint8_t s_clear;                  /* and its stencil */
	unsigned samples;                 /* 0 or 1: single-sampled; 4: ROTATED_4X_GRID (pan_sample_pattern), the
	                                   * sample locations then that pattern's entry (hm_v7_sample_pattern_offset) */
};
struct hm_v7_rt {
	enum hm_rt_format format;
	uint64_t base;                    /* the first pixel */
	uint32_t row_stride;              /* bytes */
	uint32_t internal_buffer_offset;  /* bytes into the tile buffer (pan_emit_fbd's cbuf_offset), 16 B units */
	bool write_enable;
	bool clear;                       /* load action Clear: CLEAR_WORDS are the tile buffer's colour */
	bool clean_pixel_write;           /* write tiles nothing was drawn into (pan_prepare_rt: set by a clear) */
	bool dithering;
	bool msaa_resolve;                /* a multisampled frame written back averaged (MALI_MSAA_AVERAGE: mali_sampling_mode
	                                   * for a single-sampled image under a multisampled framebuffer) */
	uint32_t clear_words[4];
};
/* Writes FB and the RTS at OUT (whose GPU address is GPU, 64-aligned): the tagged pointer a Fragment Job names
 * (the low bits carry the "Framebuffer pointer" struct: type, ZS CRC extension, render target count). 0 if the
 * description cannot be written (the reason in *WHY). */
uint64_t hm_v7_framebuffer(void *out, uint64_t gpu, const struct hm_v7_fb *fb, const struct hm_v7_rt *rts,
    const char **why);

/* A whole Fragment Job: the tiles [MIN, MAX] (16-pixel units, MALI_TILE_SHIFT) of the frame FBD_TAGGED names.
 * Header type Fragment, as GENX(jm_emit_fragment_job) writes it. */
void hm_v7_fragment_job(void *job, unsigned index, uint64_t next, uint64_t fbd_tagged, unsigned min_tx,
    unsigned min_ty, unsigned max_tx, unsigned max_ty);

/* A Tiler Heap over [BASE, BASE + SIZE) and a Tiler Context over it, as jm_emit_tiler_desc writes them. */
void hm_v7_tiler_heap(void *out, uint64_t base, uint32_t size);
void hm_v7_tiler_context(void *out, uint64_t heap, uint32_t fb_width, uint32_t fb_height, uint32_t hierarchy_mask);
/* pan_select_tiler_hierarchy_mask (pan_util.h): MAX_LEVELS from TILER_FEATURES[11:8] */
uint32_t hm_v7_hierarchy_mask(uint32_t width, uint32_t height, unsigned max_levels);

/* The single-sampled entry of pan_samples.c's sample_position_lut (Panfrost's sample_positions buffer at its
 * offset 0): 256 bytes, every position and the origin at the pixel's centre. */
void hm_v7_sample_positions(void *out);

/* pan_select_tile_size for one colour target of BYTES_PER_PIXEL in the tile buffer (4 for RGBA8: blendable
 * formats take 32 bits there) and a tile buffer budget of TIB_BUDGET bytes (pan_props.c: G52's 16384 / 2) */
void hm_v7_tile_size(unsigned bytes_per_pixel, unsigned tib_budget, uint32_t *tile_size, uint32_t *cbuf_allocation);

/* pan_pack_color for an R8G8B8A8 tile buffer (internal format R8G8B8A8, no dithering): R, G, B, A in [0, 1]
 * become 8 bits each, rounded to even, and the word is repeated four times */
void hm_v7_pack_clear_rgba8(const float rgba[4], uint32_t out[4]);

/* --- a draw (jm_launch_draw on v7 without IDVS: a vertex job, then a tiler job that depends on it) ------------ */

/* An Attribute (vertex input or varying record): BUFFER_INDEX into the buffers beside it, FORMAT a Pixel Format
 * word (hm_v7_position_format, or one Mesa's format table gives), OFFSET bytes; Offset enable is v7.xml's default
 * for a vertex input (panfrost_emit_vertex_data), off for a varying (pan_emit_vary: PAN_ARCH <= 5 only) */
void hm_v7_attribute(void *out, unsigned buffer_index, uint32_t format, int32_t offset, bool offset_enable);
/* An Attribute Buffer of type 1D (v7.xml's default): POINTER 64-aligned, STRIDE and SIZE bytes; a zeroed one ends a
 * table (panfrost_emit_vertex_data and _varying_descriptor leave one after the last: "suppress prefetch") */
void hm_v7_attribute_buffer(void *out, uint64_t pointer, uint32_t stride, uint32_t size);
/* the position varying's format, pan_special_format(PAN_VARY_POSITION) on v7: MALI_SNAP_4 << 12, no swizzle */
uint32_t hm_v7_position_format(void);
/* A Viewport (panfrost_emit_viewport, v7): the scissor [MIN, MAX] inclusive in pixels, depth [MIN_Z, MAX_Z]; the
 * x and y limits v7.xml's defaults (-inf, +inf) */
void hm_v7_viewport(void *out, unsigned min_x, unsigned min_y, unsigned max_x, unsigned max_y, float min_z, float max_z);
/* a varying record no consumer reads (panfrost_emit_varying: MALI_CONSTANT << 12, buffer 0) */
uint32_t hm_v7_constant_format(void);
/* A Uniform Buffer table entry (panfrost_emit_ubo): SIZE bytes at POINTER, 16-byte aligned */
void hm_v7_uniform_buffer(void *out, uint64_t pointer, uint32_t size);
/* A 2D texture of one level, one layer, one sample, linear (GENX(panfrost_new_texture) on v7): FORMAT the Mali format
 * word (Mesa's table), SWIZZLE (panfrost_translate_swizzle_4), the surfaces at SURFACES (hm_v7_surface_with_stride) */
void hm_v7_texture_2d(void *out, uint32_t format, uint32_t swizzle, uint32_t width, uint32_t height, uint64_t surfaces);
/* the same with LEVELS mip levels, a Surface With Stride each at SURFACES in order (panfrost_emit_texture_payload); the
 * texture's own LOD range 0..levels-1, for bounds checking only (the API's clamps are the sampler's) */
void hm_v7_texture_2d_levels(void *out, uint32_t format, uint32_t swizzle, uint32_t width, uint32_t height,
    unsigned levels, uint64_t surfaces);
/* The surface a texture names (panfrost_emit_surface_with_stride): the first texel, row and surface strides */
void hm_v7_surface_with_stride(void *out, uint64_t pointer, int32_t row_stride, int32_t surface_stride);
/* A Sampler (panfrost_create_sampler_state on v7): NEAREST or linear filtering both ways, no mipmapping (levels 0..0),
 * every wrap mode Clamp to Edge, normalized coordinates */
void hm_v7_sampler(void *out, bool nearest);
/* panfrost_create_sampler_state in full: MIN_NEAREST / MAG_NEAREST, MIPMAP (pan_pipe_to_mipmode: 0 none, 1 nearest,
 * 2 trilinear -- Metal's NotMipmapped, Nearest, Linear), the LOD clamps MIN_LOD..MAX_LOD */
enum hm_mip { HM_MIP_NONE, HM_MIP_NEAREST, HM_MIP_LINEAR };
void hm_v7_sampler_full(void *out, bool min_nearest, bool mag_nearest, enum hm_mip mip, float min_lod, float max_lod);
/* Local Storage with no thread or workgroup storage (GENX(pan_emit_tls) with both sizes 0) */
void hm_v7_local_storage(void *out);
/* ... with workgroup storage (threadgroup memory): WLS_SIZE bytes a workgroup (pan_wls_adjust_size: a power of two of
 * at least 128), INSTANCES of them (pan_wls_instances: the grid's sizes to powers of two, multiplied), at POINTER
 * (4 KB aligned, inside one 4 GB range) */
void hm_v7_local_storage_wls(void *out, uint64_t pointer, unsigned wls_size, unsigned instances);

struct hm_v7_draw {
	uint64_t state;                   /* the stage's Renderer State (followed by its Blend descriptors: fragment) */
	uint64_t attributes, attribute_buffers;
	uint64_t varyings, varying_buffers;
	uint64_t position;                /* the tiler's: the position varying buffer */
	uint64_t viewport;                /* the tiler's */
	uint64_t thread_storage;
	uint64_t push_uniforms, uniform_buffers, textures, samplers;
	/* the tiler's (jm_emit_tiler_draw: rast->cull_face, rast->front_ccw); zero: nothing culled, clockwise front */
	bool cull_front, cull_back, front_ccw;
	/* instancing (GENX(jm_launch_draw), jm_emit_draw_descs): INSTANCE_COUNT instances (0 or 1: none), each PADDED_COUNT
	 * vertices apart in the varyings (hm_v7_padded_vertex_count of the draw's vertex count) */
	unsigned instance_count, padded_count;
};
/* panfrost_padded_vertex_count (pan_attributes.c): the vertices an instance takes, as the hardware computes it */
unsigned hm_v7_padded_vertex_count(unsigned vertex_count);
/* An attribute buffer of an instanced draw (panfrost_emit_vertex_data): per vertex, 1D_MODULUS by PADDED; per instance,
 * the hardware divisor PADDED (a divisor of 1), 1D_POT_DIVISOR or 1D_NPOT_DIVISOR with its continuation record
 * (panfrost_compute_magic_divisor). Writes 1 or 2 records at OUT: how many */
unsigned hm_v7_attribute_buffer_instanced(void *out, uint64_t pointer, uint32_t stride, uint32_t size, unsigned padded,
    bool per_instance);
/* A vertex job: Compute Job's layout with type Vertex (jm_emit_vertex_job), VERTEX_COUNT vertices of one instance
 * (jm_launch_draw's Invocation for instance_count 1), Job Task Split 5 */
void hm_v7_vertex_job(void *job, unsigned index, uint64_t next, unsigned vertex_count, const struct hm_v7_draw *draw);
/* A tiler job (jm_emit_tiler_job): triangles, VERTEX_COUNT vertices, no index buffer, the Tiler Context TILER,
 * dependent on job DEPENDENCY of the chain; Primitive Size the constant 1.0 (a line width of 1); depth clipped
 * near and far, the last vertex provoking (Gallium's rasterizer defaults); nothing culled */
void hm_v7_tiler_job(void *job, unsigned index, unsigned dependency, uint64_t next, unsigned vertex_count,
    uint64_t tiler, const struct hm_v7_draw *draw);
/* the same with an index buffer: INDEX_COUNT 16-bit indices at INDICES into the VERTEX_COUNT vertices the vertex job
 * shaded (jm_emit_primitive: index type, count and the indices' address; base vertex 0) */
void hm_v7_tiler_job_indexed(void *job, unsigned index, unsigned dependency, uint64_t next, unsigned vertex_count,
    uint64_t tiler, const struct hm_v7_draw *draw, unsigned index_count, uint64_t indices);
/* the same for another primitive: MALI_DRAW_MODE_* (pan_draw_mode: 1 points, 2 lines, 8 triangles); for lines the first
 * vertex provokes and Draw's flat shading vertex says so (jm_emit_primitive, jm_emit_tiler_draw), the primitive size the
 * constant 1.0 -- the line width, and a point's size when the shader writes none */
void hm_v7_tiler_job_mode(void *job, unsigned index, unsigned dependency, uint64_t next, unsigned vertex_count,
    uint64_t tiler, const struct hm_v7_draw *draw, unsigned index_count, uint64_t indices, unsigned draw_mode);

/* A compute job (GENX(jm_launch_grid), v7): GROUPS workgroups of LOCAL invocations each, the Invocation packed as
 * panfrost_pack_work_groups_compute packs it, Job Task Split the sum of log2ceil(local + 1), a barrier in the header
 * (pan_jc_add_job's for a compute job); DRAW's state, thread storage, uniforms and push uniforms */
void hm_v7_compute_job(void *job, unsigned index, uint64_t next, const unsigned groups[3], const unsigned local[3],
    const struct hm_v7_draw *draw);

/* --- reading (the fake GPU, the probe's dumps) ------------------------------------------------------------- */

struct hm_v7_job {
	uint32_t exception_status, first_incomplete_task;
	uint64_t fault_pointer;
	bool is_64b, barrier;
	unsigned type, index, dependency_1, dependency_2;
	uint64_t next;
};
struct hm_v7_fragment {
	unsigned min_tx, min_ty, max_tx, max_ty;
	bool has_tile_enable_map;
	uint64_t framebuffer;             /* as written: tagged */
};
struct hm_v7_fbd_pointer {
	bool type;                        /* the "Framebuffer pointer"'s Type bit (v7.xml default true) */
	bool zs_crc_extension;
	unsigned rt_count;
	uint64_t pointer;
};
struct hm_v7_fb_read {
	struct hm_v7_fb fb;
	unsigned pre_frame_0, pre_frame_1, post_frame;  /* MALI_PRE_POST_FRAME_SHADER_MODE_* (0: Never) */
	uint64_t frame_shader_dcds;
	unsigned sample_count, sample_pattern, effective_tile_size;
	bool has_zs_crc_extension, z_write_enable, s_write_enable, crc_read_enable, crc_write_enable;
};
struct hm_v7_rt_read {
	struct hm_v7_rt rt;
	unsigned internal_format, writeback_format, writeback_block_format, writeback_msaa, swizzle;
	bool yuv_enable, srgb, dithered_clear;
	uint32_t surface_stride;
};
struct hm_v7_tiler_read {
	uint64_t polygon_list, heap;
	uint32_t hierarchy_mask, fb_width, fb_height;
	unsigned sample_pattern;
	uint64_t heap_base, heap_bottom, heap_top;   /* from the Tiler Heap at HEAP, when given */
	uint32_t heap_size;
};
struct hm_v7_write_value {
	uint64_t address;
	unsigned type;                    /* MALI_WRITE_VALUE_TYPE_*: 6 Immediate 32 */
	uint64_t immediate;
};

/* 0, or -1 with *WHY: a bit outside every field (the generated unpacker's sentence) */
int hm_v7_read_job(const void *job, struct hm_v7_job *out, const char **why);
int hm_v7_read_fragment(const void *job, struct hm_v7_fragment *out, const char **why);
int hm_v7_read_write_value(const void *job, struct hm_v7_write_value *out, const char **why);
void hm_v7_read_fbd_pointer(uint64_t tagged, struct hm_v7_fbd_pointer *out);
int hm_v7_read_framebuffer(const void *fbd, struct hm_v7_fb_read *out, const char **why);
int hm_v7_read_rt(const void *rt, struct hm_v7_rt_read *out, const char **why);
int hm_v7_read_tiler_context(const void *ctx, struct hm_v7_tiler_read *out, const char **why);
int hm_v7_read_tiler_heap(const void *heap, struct hm_v7_tiler_read *out, const char **why);

/* a vertex job's or tiler job's draw (TILER: the Tiler Job's layout, else the Compute Job's): its Draw, the
 * invocation count, and for a tiler job the primitive's draw mode and index count and the Tiler Context */
struct hm_v7_draw_read {
	struct hm_v7_draw draw;
	unsigned invocations;             /* vertices less one, as packed */
	unsigned draw_mode, index_count, index_type;
	uint64_t tiler;
};
int hm_v7_read_draw(const void *job, bool tiler, struct hm_v7_draw_read *out, const char **why);

/* the GPU's own write into a job's header when the job ends (what the fake GPU does in its place) */
void hm_v7_set_exception_status(void *job, uint32_t status);

/* Mesa's own printers (MALI_*_print), for the probe: the job, and the framebuffer with its render targets */
void hm_v7_print_job(FILE *fp, const void *job);
void hm_v7_print_framebuffer(FILE *fp, const void *fbd, unsigned rt_count);

/* MALI_CHANNEL swizzle words the formats above take (pan_rt_init_format), for tests */
uint32_t hm_v7_rt_swizzle(enum hm_rt_format format);

#ifdef __cplusplus
}
#endif

#endif
