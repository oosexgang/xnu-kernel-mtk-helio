/*
 * The shader side of libhakiosmali.dylib (rwa_priv.h): AIR in, a compiled Bifrost stage out, and the fixed state of a
 * render pipeline -- what Gallium's panfrost does between pipe->create_*_state and a draw (pan_shader.c's
 * panfrost_shader_compile, pan_nir_lower_sysvals.c, pan_cmdstream.c's blend and renderer state), done here without it.
 *
 * The path of a stage, in Panfrost's order:
 *   air_to_nir (the AIR front end, with the function constants and the known buffer 0 the caller gave),
 *   nir_lower_system_values, nir_lower_compute_system_values, nir_shader_gather_info  (air_metal.c's finalize),
 *   pan_shader_preprocess (bifrost_preprocess_nir: vertex id, viewport transform, I/O, SSBOs to addresses, ...),
 *   lower_sysvals (this file: what Panfrost's panfrost_nir_lower_sysvals does, into a last uniform buffer),
 *   GENX(pan_shader_compile), every uniform buffer pushable (the Metal driver's choice in the patched pan_shader.c).
 */
#include <stdarg.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"
#include "panfrost/compiler/bifrost_compile.h"
#include "util/format/u_format.h"
#include "util/half_float.h"
#include "util/ralloc.h"
#include "util/u_dynarray.h"

#include "genxml/gen_macros.h"
#include "pan_blend.h"
#include "pan_earlyzs.h"
#include "pan_encoder.h"
#include "pan_format.h"
#include "pan_shader.h"
#include "pan_texture.h"
#include "pan_util.h"

#include "rwa_priv.h"

_Thread_local uint32_t rwa_next_noperspective;

/* the patched pan_blend.c counts the blend shaders it compiles (for the Gallium library's lines); this library compiles
 * none, but links the file */
uint64_t hakios_pan_blend_compiles;
/* and pan_blend.c's blend shader path names Midgard's preprocessing, for v5 and earlier: never reached on a v7 */
void midgard_preprocess_nir(nir_shader *nir, unsigned gpu_id) __attribute__((weak));
void
midgard_preprocess_nir(nir_shader *nir, unsigned gpu_id)
{
	(void)nir;
	(void)gpu_id;
	abort();
}

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

/* --- formats ------------------------------------------------------------------------------------------------------ */

/* MTLPixelFormat -> pipe_format: air_metal.c's table (rwa_pixel_formats), the formats a texture of this port takes */
static const struct {
	unsigned mtl;
	const char *name;
	enum pipe_format pipe;
} pixel_formats[] = {
	{ 1, "A8Unorm", PIPE_FORMAT_A8_UNORM },
	{ 10, "R8Unorm", PIPE_FORMAT_R8_UNORM },
	{ 11, "R8Unorm_sRGB", PIPE_FORMAT_R8_SRGB },
	{ 12, "R8Snorm", PIPE_FORMAT_R8_SNORM },
	{ 13, "R8Uint", PIPE_FORMAT_R8_UINT },
	{ 14, "R8Sint", PIPE_FORMAT_R8_SINT },
	{ 20, "R16Unorm", PIPE_FORMAT_R16_UNORM },
	{ 22, "R16Snorm", PIPE_FORMAT_R16_SNORM },
	{ 23, "R16Uint", PIPE_FORMAT_R16_UINT },
	{ 24, "R16Sint", PIPE_FORMAT_R16_SINT },
	{ 25, "R16Float", PIPE_FORMAT_R16_FLOAT },
	{ 30, "RG8Unorm", PIPE_FORMAT_R8G8_UNORM },
	{ 31, "RG8Unorm_sRGB", PIPE_FORMAT_R8G8_SRGB },
	{ 32, "RG8Snorm", PIPE_FORMAT_R8G8_SNORM },
	{ 33, "RG8Uint", PIPE_FORMAT_R8G8_UINT },
	{ 34, "RG8Sint", PIPE_FORMAT_R8G8_SINT },
	{ 40, "B5G6R5Unorm", PIPE_FORMAT_R5G6B5_UNORM },
	{ 41, "A1BGR5Unorm", PIPE_FORMAT_A1R5G5B5_UNORM },
	{ 42, "ABGR4Unorm", PIPE_FORMAT_A4R4G4B4_UNORM },
	{ 43, "BGR5A1Unorm", PIPE_FORMAT_R5G5B5A1_UNORM },
	{ 53, "R32Uint", PIPE_FORMAT_R32_UINT },
	{ 54, "R32Sint", PIPE_FORMAT_R32_SINT },
	{ 55, "R32Float", PIPE_FORMAT_R32_FLOAT },
	{ 60, "RG16Unorm", PIPE_FORMAT_R16G16_UNORM },
	{ 62, "RG16Snorm", PIPE_FORMAT_R16G16_SNORM },
	{ 63, "RG16Uint", PIPE_FORMAT_R16G16_UINT },
	{ 64, "RG16Sint", PIPE_FORMAT_R16G16_SINT },
	{ 65, "RG16Float", PIPE_FORMAT_R16G16_FLOAT },
	{ 70, "RGBA8Unorm", PIPE_FORMAT_R8G8B8A8_UNORM },
	{ 71, "RGBA8Unorm_sRGB", PIPE_FORMAT_R8G8B8A8_SRGB },
	{ 72, "RGBA8Snorm", PIPE_FORMAT_R8G8B8A8_SNORM },
	{ 73, "RGBA8Uint", PIPE_FORMAT_R8G8B8A8_UINT },
	{ 74, "RGBA8Sint", PIPE_FORMAT_R8G8B8A8_SINT },
	{ 80, "BGRA8Unorm", PIPE_FORMAT_B8G8R8A8_UNORM },
	{ 81, "BGRA8Unorm_sRGB", PIPE_FORMAT_B8G8R8A8_SRGB },
	{ 90, "RGB10A2Unorm", PIPE_FORMAT_R10G10B10A2_UNORM },
	{ 94, "BGR10A2Unorm", PIPE_FORMAT_B10G10R10A2_UNORM },
	{ 103, "RG32Uint", PIPE_FORMAT_R32G32_UINT },
	{ 104, "RG32Sint", PIPE_FORMAT_R32G32_SINT },
	{ 105, "RG32Float", PIPE_FORMAT_R32G32_FLOAT },
	{ 110, "RGBA16Unorm", PIPE_FORMAT_R16G16B16A16_UNORM },
	{ 112, "RGBA16Snorm", PIPE_FORMAT_R16G16B16A16_SNORM },
	{ 113, "RGBA16Uint", PIPE_FORMAT_R16G16B16A16_UINT },
	{ 114, "RGBA16Sint", PIPE_FORMAT_R16G16B16A16_SINT },
	{ 115, "RGBA16Float", PIPE_FORMAT_R16G16B16A16_FLOAT },
	{ 123, "RGBA32Uint", PIPE_FORMAT_R32G32B32A32_UINT },
	{ 124, "RGBA32Sint", PIPE_FORMAT_R32G32B32A32_SINT },
	{ 125, "RGBA32Float", PIPE_FORMAT_R32G32B32A32_FLOAT },
	{ 204, "ASTC_4x4_LDR", PIPE_FORMAT_ASTC_4x4 },
	{ 186, "ASTC_4x4_sRGB", PIPE_FORMAT_ASTC_4x4_SRGB },
	{ 250, "Depth16Unorm", PIPE_FORMAT_Z16_UNORM },
	{ 252, "Depth32Float", PIPE_FORMAT_Z32_FLOAT },
	{ 253, "Stencil8", PIPE_FORMAT_S8_UINT },
	{ 260, "Depth32Float_Stencil8", PIPE_FORMAT_Z32_FLOAT_S8X24_UINT },
};

enum pipe_format
rwa_pipe_format(unsigned mtl)
{
	for (unsigned i = 0; i < ARRAY_SIZE(pixel_formats); i++)
		if (pixel_formats[i].mtl == mtl)
			return pixel_formats[i].pipe;
	return PIPE_FORMAT_NONE;
}

const char *
rwa_pixel_format_name(unsigned mtl)
{
	for (unsigned i = 0; i < ARRAY_SIZE(pixel_formats); i++)
		if (pixel_formats[i].mtl == mtl)
			return pixel_formats[i].name;
	return "a pixel format this library does not know";
}

/* MTLVertexFormat -> pipe_format (air_metal.c's vertex format table) */
static const struct {
	unsigned mtl;
	enum pipe_format pipe;
} vertex_formats[] = {
	{ 1, PIPE_FORMAT_R8G8_UINT }, { 2, PIPE_FORMAT_R8G8B8_UINT }, { 3, PIPE_FORMAT_R8G8B8A8_UINT },
	{ 4, PIPE_FORMAT_R8G8_SINT }, { 5, PIPE_FORMAT_R8G8B8_SINT }, { 6, PIPE_FORMAT_R8G8B8A8_SINT },
	{ 7, PIPE_FORMAT_R8G8_UNORM }, { 8, PIPE_FORMAT_R8G8B8_UNORM }, { 9, PIPE_FORMAT_R8G8B8A8_UNORM },
	{ 10, PIPE_FORMAT_R8G8_SNORM }, { 11, PIPE_FORMAT_R8G8B8_SNORM }, { 12, PIPE_FORMAT_R8G8B8A8_SNORM },
	{ 13, PIPE_FORMAT_R16G16_UINT }, { 14, PIPE_FORMAT_R16G16B16_UINT }, { 15, PIPE_FORMAT_R16G16B16A16_UINT },
	{ 16, PIPE_FORMAT_R16G16_SINT }, { 17, PIPE_FORMAT_R16G16B16_SINT }, { 18, PIPE_FORMAT_R16G16B16A16_SINT },
	{ 19, PIPE_FORMAT_R16G16_UNORM }, { 20, PIPE_FORMAT_R16G16B16_UNORM }, { 21, PIPE_FORMAT_R16G16B16A16_UNORM },
	{ 22, PIPE_FORMAT_R16G16_SNORM }, { 23, PIPE_FORMAT_R16G16B16_SNORM }, { 24, PIPE_FORMAT_R16G16B16A16_SNORM },
	{ 25, PIPE_FORMAT_R16G16_FLOAT }, { 26, PIPE_FORMAT_R16G16B16_FLOAT }, { 27, PIPE_FORMAT_R16G16B16A16_FLOAT },
	{ 28, PIPE_FORMAT_R32_FLOAT }, { 29, PIPE_FORMAT_R32G32_FLOAT }, { 30, PIPE_FORMAT_R32G32B32_FLOAT },
	{ 31, PIPE_FORMAT_R32G32B32A32_FLOAT }, { 32, PIPE_FORMAT_R32_SINT }, { 33, PIPE_FORMAT_R32G32_SINT },
	{ 34, PIPE_FORMAT_R32G32B32_SINT }, { 35, PIPE_FORMAT_R32G32B32A32_SINT }, { 36, PIPE_FORMAT_R32_UINT },
	{ 37, PIPE_FORMAT_R32G32_UINT }, { 38, PIPE_FORMAT_R32G32B32_UINT }, { 39, PIPE_FORMAT_R32G32B32A32_UINT },
	{ 42, PIPE_FORMAT_B8G8R8A8_UNORM }, { 45, PIPE_FORMAT_R8_UINT }, { 46, PIPE_FORMAT_R8_SINT },
	{ 47, PIPE_FORMAT_R8_UNORM }, { 48, PIPE_FORMAT_R8_SNORM }, { 49, PIPE_FORMAT_R16_UINT },
	{ 50, PIPE_FORMAT_R16_SINT }, { 51, PIPE_FORMAT_R16_UNORM }, { 52, PIPE_FORMAT_R16_SNORM },
	{ 53, PIPE_FORMAT_R16_FLOAT },
};

enum pipe_format
rwa_vertex_format(unsigned mtl, unsigned *size)
{
	for (unsigned i = 0; i < ARRAY_SIZE(vertex_formats); i++)
		if (vertex_formats[i].mtl == mtl) {
			if (size)
				*size = util_format_get_blocksize(vertex_formats[i].pipe);
			return vertex_formats[i].pipe;
		}
	return PIPE_FORMAT_NONE;
}

uint32_t
rwa_attribute_format(enum pipe_format f)
{
	const struct panfrost_format *pf = &GENX(panfrost_format_from_pipe_format)(f)[0];
	return pf->hw;
}

bool
rwa_rt_format(enum pipe_format f, uint32_t *internal, uint32_t *writeback, uint32_t *swizzle, unsigned *bpp)
{
	if (f == PIPE_FORMAT_NONE || util_format_is_depth_or_stencil(f) || util_format_is_compressed(f))
		return false;
	const struct pan_blendable_format *b = GENX(panfrost_blendable_format_from_pipe_format)(f);
	const struct util_format_description *desc = util_format_description(f);
	unsigned char inverted[4] = { PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W };

	/* pan_rt_init_format */
	if (b->internal) {
		*internal = b->internal;
		*writeback = b->writeback;
		panfrost_invert_swizzle(desc->swizzle, inverted);
		if (bpp)
			*bpp = 4;
	} else {
		/* RAW internal and writeback, the internal one logarithmic from RAW8 (pan_mfbd_raw_format) */
		unsigned bits = desc->block.bits;
		if (bits < 8 || bits > 128)
			return false;
		*internal = MALI_COLOR_BUFFER_INTERNAL_FORMAT_RAW8 + (util_logbase2_ceil(bits) - 3);
		switch (bits) {
		case 8: *writeback = MALI_COLOR_FORMAT_RAW8; break;
		case 16: *writeback = MALI_COLOR_FORMAT_RAW16; break;
		case 24: *writeback = MALI_COLOR_FORMAT_RAW24; break;
		case 32: *writeback = MALI_COLOR_FORMAT_RAW32; break;
		case 48: *writeback = MALI_COLOR_FORMAT_RAW48; break;
		case 64: *writeback = MALI_COLOR_FORMAT_RAW64; break;
		case 96: *writeback = MALI_COLOR_FORMAT_RAW96; break;
		case 128: *writeback = MALI_COLOR_FORMAT_RAW128; break;
		default: return false;
		}
		if (bpp)
			*bpp = util_next_power_of_two(util_format_get_blocksize(f));   /* pan_bytes_per_pixel_tib */
	}
	*swizzle = panfrost_translate_swizzle_4(inverted);
	return true;
}

bool
rwa_rt_blendable(enum pipe_format f)
{
	return GENX(panfrost_blendable_format_from_pipe_format)(f)->internal != 0;
}

bool
rwa_samplable(enum pipe_format f)
{
	const struct panfrost_format *pf = GENX(panfrost_format_from_pipe_format)(f);
	return pf->hw != 0 && (pf->bind & PAN_BIND_SAMPLER_VIEW);
}

void
rwa_pack_clear(enum pipe_format f, const float rgba[4], uint32_t out[4])
{
	union pipe_color_union c;

	memcpy(c.f, rgba, sizeof(c.f));
	pan_pack_color(GENX(panfrost_blendable_formats), out, &c, f, false);
}

uint64_t
rwa_rt_conversion(enum pipe_format f, unsigned rt, unsigned size)
{
	return GENX(pan_blend_get_internal_desc)(f, rt, size, false) >> 32;
}

/* --- textures and samplers ---------------------------------------------------------------------------------------- */

void
rwa_surface_desc(void *out, uint64_t pointer, int32_t row_stride, int32_t surface_stride)
{
	pan_cast_and_pack((out), SURFACE_WITH_STRIDE, cfg) {
		cfg.pointer = pointer;
		cfg.row_stride = row_stride;
		cfg.surface_stride = surface_stride;
	}
}

/* GENX(panfrost_new_texture) for a linear image, with what panfrost_create_sampler_view_bo does to the view first on v7:
 * a depth format's X replicated, and a format AFBC could take turned to its canonical component order with the swizzle
 * composed (panfrost_texture_afbc_reswizzle) -- the hardware's own BGRA order is not what Panfrost samples with */
void
rwa_texture_desc(void *out, enum pipe_format format, const unsigned char user_swizzle[4], unsigned dim, unsigned width,
		 unsigned height, unsigned depth, unsigned levels, unsigned layers, unsigned samples, uint64_t surfaces)
{
	unsigned char swizzle[4] = { user_swizzle[0], user_swizzle[1], user_swizzle[2], user_swizzle[3] };

	if (util_format_is_depth_or_stencil(format)) {
		static const unsigned char x[4] = { PIPE_SWIZZLE_X, PIPE_SWIZZLE_X, PIPE_SWIZZLE_X, PIPE_SWIZZLE_X };
		util_format_compose_swizzles(x, swizzle, swizzle);
	} else if (!panfrost_format_is_yuv(format) && panfrost_format_supports_afbc(7, format)) {
		uint32_t hw = GENX(panfrost_format_from_pipe_format)(format)->hw;
		enum mali_rgb_component_order orig = hw & BITFIELD_MASK(12);
		struct pan_decomposed_swizzle d = GENX(pan_decompose_swizzle)(orig);
		if (orig != d.pre)
			format = util_format_rgb_to_bgr(format);
		util_format_compose_swizzles(d.post, swizzle, swizzle);
	}
	uint32_t hw = GENX(panfrost_format_from_pipe_format)(format)->hw;
	pan_cast_and_pack((out), TEXTURE, cfg) {
		cfg.dimension = dim;
		cfg.format = hw;
		cfg.width = width;
		cfg.height = height;
		if (dim == MALI_TEXTURE_DIMENSION_3D)
			cfg.depth = depth;
		else
			cfg.sample_count = samples ? samples : 1;
		cfg.swizzle = panfrost_translate_swizzle_4(swizzle);
		cfg.texel_ordering = MALI_TEXTURE_LAYOUT_LINEAR;
		cfg.levels = levels;
		cfg.array_size = layers;
		cfg.surfaces = surfaces;
		cfg.minimum_lod = 0;
		cfg.maximum_lod = levels - 1;
	}
}

static enum mali_wrap_mode
metal_wrap(unsigned m, bool *border_zero)
{
	switch (m) {
	case 0: return MALI_WRAP_MODE_CLAMP_TO_EDGE;            /* ClampToEdge */
	case 1: return MALI_WRAP_MODE_MIRRORED_CLAMP_TO_EDGE;   /* MirrorClampToEdge */
	case 2: return MALI_WRAP_MODE_REPEAT;
	case 3: return MALI_WRAP_MODE_MIRRORED_REPEAT;
	case 4: *border_zero = true; return MALI_WRAP_MODE_CLAMP_TO_BORDER;   /* ClampToZero */
	case 5: return MALI_WRAP_MODE_CLAMP_TO_BORDER;          /* ClampToBorderColor: transparent black */
	default: return MALI_WRAP_MODE_CLAMP_TO_EDGE;
	}
}

int
rwa_sampler_desc(void *out, const struct rwa_sampler_state *s, bool compare, char *why, size_t why_size)
{
	bool zero = false;

	if (s->mip_filter > 2 || s->min_filter > 1 || s->mag_filter > 1) {
		say(why, why_size, "a sampler filter this GPU has no mode for (min %u mag %u mip %u)", s->min_filter,
		    s->mag_filter, s->mip_filter);
		return -1;
	}
	pan_cast_and_pack((out), SAMPLER, cfg) {
		cfg.magnify_nearest = s->mag_filter == 0;
		cfg.minify_nearest = s->min_filter == 0;
		cfg.normalized_coordinates = s->normalized_coordinates != 0;
		cfg.minimum_lod = s->lod_min;
		cfg.maximum_lod = s->lod_max > 1000.0f ? 1000.0f : s->lod_max;
		cfg.wrap_mode_s = metal_wrap(s->address[0], &zero);
		cfg.wrap_mode_t = metal_wrap(s->address[1], &zero);
		cfg.wrap_mode_r = metal_wrap(s->address[2], &zero);
		cfg.mipmap_mode = s->mip_filter == 1 ? MALI_MIPMAP_MODE_NEAREST
		    : s->mip_filter == 2 ? MALI_MIPMAP_MODE_TRILINEAR : MALI_MIPMAP_MODE_NONE;
		/* MTLCompareFunction is Gallium's and Mali's order; a shadow sample's is flipped (panfrost_sampler_compare_func) */
		cfg.compare_function = compare ? panfrost_flip_compare_func((enum mali_func)(s->compare_function & 7))
					       : MALI_FUNC_NEVER;
		cfg.seamless_cube_map = true;
		if (s->max_anisotropy > 1) {
			cfg.maximum_anisotropy = s->max_anisotropy > 16 ? 16 : s->max_anisotropy;
			cfg.lod_algorithm = MALI_LOD_ALGORITHM_ANISOTROPIC;
		}
	}
	return 0;
}

/* a constexpr sampler of the shader, in Metal's terms (air_metal.c sampler_state_from_air) */
static void
sampler_from_air(const struct air_sampler_state *a, struct rwa_sampler_state *m)
{
	static const unsigned address[] = {
		[AIR_SAMPLER_CLAMP_TO_ZERO] = 4, [AIR_SAMPLER_CLAMP_TO_EDGE] = 0, [AIR_SAMPLER_REPEAT] = 2,
		[AIR_SAMPLER_MIRRORED_REPEAT] = 3, [AIR_SAMPLER_CLAMP_TO_BORDER] = 5,
	};
	memset(m, 0, sizeof(*m));
	for (unsigned i = 0; i < 3; i++)
		m->address[i] = a->address[i] <= AIR_SAMPLER_CLAMP_TO_BORDER ? address[a->address[i]] : 0;
	m->min_filter = a->min_filter == AIR_SAMPLER_NEAREST ? 0 : 1;
	m->mag_filter = a->mag_filter == AIR_SAMPLER_NEAREST ? 0 : 1;
	m->mip_filter = a->mip_filter == AIR_SAMPLER_MIP_NONE ? 0 : a->mip_filter == AIR_SAMPLER_MIP_NEAREST ? 1 : 2;
	/* the word's compare_func: none 0, then Metal's eight in another order -- air_metal.c's table */
	static const unsigned cmp[] = { 0, 1, 3, 4, 6, 2, 5, 7, 0 };
	m->compare_function = a->compare_func < ARRAY_SIZE(cmp) ? cmp[a->compare_func] : 0;
	m->max_anisotropy = a->max_anisotropy;
	m->normalized_coordinates = !a->pixel_coord;
	m->lod_min = _mesa_half_to_float(a->lod_min);
	m->lod_max = a->lod_max == 0 && a->lod_min == 0 ? 1000.0f : _mesa_half_to_float(a->lod_max);
}

/* --- system values -------------------------------------------------------------------------------------------------- */

struct sysval_ctx {
	struct rwa_stage *st;
	bool need_ubo;
};

static unsigned
sysval_slot(struct rwa_stage *st, uint32_t sv)
{
	for (unsigned i = 0; i < st->sysval_count; i++)
		if (st->sysvals[i] == sv)
			return i;
	if (st->sysval_count >= RWA_MAX_SYSVALS)
		return ~0u;
	st->sysvals[st->sysval_count] = sv;
	return st->sysval_count++;
}

/* panfrost_nir_lower_sysvals' intrinsics, with their byte offsets in the vec4 */
static bool
sysval_of(nir_intrinsic_instr *intr, uint32_t *sv, unsigned *offset)
{
	*offset = 0;
	switch (intr->intrinsic) {
	case nir_intrinsic_load_ssbo_address:
		*sv = RWA_SV(RWA_SV_SSBO, nir_src_as_uint(intr->src[0]));
		return nir_src_is_const(intr->src[0]);
	case nir_intrinsic_get_ssbo_size:
		*sv = RWA_SV(RWA_SV_SSBO, nir_src_as_uint(intr->src[0]));
		*offset = 8;
		return nir_src_is_const(intr->src[0]);
	case nir_intrinsic_load_viewport_scale: *sv = RWA_SV_VIEWPORT_SCALE; return true;
	case nir_intrinsic_load_viewport_offset: *sv = RWA_SV_VIEWPORT_OFFSET; return true;
	case nir_intrinsic_load_raw_vertex_offset_pan: *sv = RWA_SV_VERTEX_OFFSETS; return true;
	case nir_intrinsic_load_base_vertex: *sv = RWA_SV_VERTEX_OFFSETS; *offset = 4; return true;
	case nir_intrinsic_load_base_instance: *sv = RWA_SV_VERTEX_OFFSETS; *offset = 8; return true;
	case nir_intrinsic_load_num_vertices: *sv = RWA_SV_NUM_VERTICES; return true;
	case nir_intrinsic_load_draw_id: *sv = RWA_SV_DRAW_ID; return true;
	case nir_intrinsic_load_multisampled_pan: *sv = RWA_SV_MULTISAMPLED; return true;
	case nir_intrinsic_load_sample_positions_pan: *sv = RWA_SV_SAMPLE_POSITIONS; return true;
	case nir_intrinsic_load_num_workgroups: *sv = RWA_SV_NUM_WORKGROUPS; return true;
	case nir_intrinsic_load_workgroup_size: *sv = RWA_SV_LOCAL_SIZE; return true;
	case nir_intrinsic_load_rt_conversion_pan: {
		unsigned size = nir_alu_type_get_type_size(nir_intrinsic_src_type(intr));
		*sv = RWA_SV(RWA_SV_RT_CONVERSION, nir_intrinsic_base(intr) | (size << 4));
		return true;
	}
	default:
		return false;
	}
}

static bool
lower_sysval(nir_builder *b, nir_instr *instr, void *data)
{
	struct sysval_ctx *ctx = data;
	nir_def *old;
	uint32_t sv;
	unsigned offset = 0;

	if (instr->type == nir_instr_type_intrinsic) {
		nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
		if (!sysval_of(intr, &sv, &offset))
			return false;
		old = &intr->def;
	} else if (instr->type == nir_instr_type_tex) {
		nir_tex_instr *tex = nir_instr_as_tex(instr);
		if (tex->op == nir_texop_texture_samples || tex->op == nir_texop_samples_identical) {
			/* one sample a texel: no multisample texture is made here */
			b->cursor = nir_after_instr(instr);
			nir_def *v = tex->op == nir_texop_texture_samples ? nir_imm_intN_t(b, 1, tex->def.bit_size)
									  : nir_imm_true(b);
			nir_def_rewrite_uses(&tex->def, v);
			nir_instr_remove(instr);
			return true;
		}
		if (tex->op == nir_texop_query_levels) {
			sv = RWA_SV(RWA_SV_TEXTURE_LEVELS, tex->texture_index);
			unsigned slot = sysval_slot(ctx->st, sv);
			if (slot == ~0u)
				return false;
			ctx->need_ubo = true;
			b->cursor = nir_after_instr(instr);
			nir_def *v = nir_load_ubo(b, 1, 32, nir_imm_int(b, ctx->st->sysval_ubo), nir_imm_int(b, slot * 16),
						  .align_mul = 4, .align_offset = 0, .range_base = slot * 16, .range = 4);
			nir_def_rewrite_uses(&tex->def, v);
			nir_instr_remove(instr);
			return true;
		}
		if (tex->op != nir_texop_txs)
			return false;
		unsigned dims = nir_tex_instr_dest_size(tex) - (tex->is_array ? 1 : 0);
		sv = RWA_SV(RWA_SV_TEXTURE_SIZE, tex->texture_index | (dims << 8) | ((tex->is_array ? 1u : 0u) << 12));
		old = &tex->def;
	} else {
		return false;
	}
	unsigned slot = sysval_slot(ctx->st, sv);
	if (slot == ~0u)
		return false;
	ctx->need_ubo = true;
	b->cursor = nir_after_instr(instr);
	nir_def *val = nir_load_ubo(b, old->num_components, old->bit_size, nir_imm_int(b, ctx->st->sysval_ubo),
				    nir_imm_int(b, slot * 16 + offset), .align_mul = old->bit_size / 8, .align_offset = 0,
				    .range_base = slot * 16 + offset, .range = old->num_components * old->bit_size / 8);
	nir_def_rewrite_uses(old, val);
	nir_instr_remove(instr);
	return true;
}

/* a boot-arg (the host build: RWA_BOOTARGS) */
static bool
boot_arg_set(const char *text)
{
#ifndef RWA_HOST_FAKE
	char args[1024] = "";
	size_t n = sizeof(args) - 1;
	return sysctlbyname("kern.bootargs", args, &n, NULL, 0) == 0 && strstr(args, text) != NULL;
#else
	const char *args = getenv("RWA_BOOTARGS");
	return args != NULL && strstr(args, text) != NULL;
#endif
}

/* uniform buffers read at computed offsets: noted (pass 1), and every load of them given the binding's distance past
 * 16 bytes (pass 2) -- a copy of such a buffer, its extent unknown, was up to 64 KB read out of write-combined memory a
 * draw (run 537: ~95 us of a fragment stage's ~100) */
static bool
ubo_scan(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
	struct rwa_stage *st = data;
	(void)b;
	if (intr->intrinsic != nir_intrinsic_load_ubo)
		return false;
	if (!nir_src_is_const(intr->src[0])) {
		st->ubo_extent_known = false;
		return false;
	}
	unsigned u = nir_src_as_uint(intr->src[0]);
	if (u >= RWA_MAX_UBOS)
		return false;
	if (!nir_src_is_const(intr->src[1])) {
		st->ubo_dynamic[u / 64] |= 1ull << (u % 64);
		return false;
	}
	uint32_t end = nir_src_as_uint(intr->src[1]) + intr->def.num_components * intr->def.bit_size / 8;
	st->ubo_extent[u] = MAX2(st->ubo_extent[u], end);
	return false;
}

static bool
ubo_misalign(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
	struct sysval_ctx *ctx = data;
	struct rwa_stage *st = ctx->st;
	if (intr->intrinsic != nir_intrinsic_load_ubo || !nir_src_is_const(intr->src[0]))
		return false;
	unsigned u = nir_src_as_uint(intr->src[0]);
	if (u >= RWA_MAX_UBOS || u == st->sysval_ubo || !(st->ubo_dynamic[u / 64] & (1ull << (u % 64))))
		return false;
	unsigned slot = sysval_slot(st, RWA_SV(RWA_SV_UBO_MISALIGN, u));
	if (slot == ~0u)
		return false;
	ctx->need_ubo = true;
	b->cursor = nir_before_instr(&intr->instr);
	nir_def *mis = nir_load_ubo(b, 1, 32, nir_imm_int(b, st->sysval_ubo), nir_imm_int(b, slot * 16), .align_mul = 4,
				    .align_offset = 0, .range_base = slot * 16, .range = 4);
	nir_src_rewrite(&intr->src[1], nir_iadd(b, intr->src[1].ssa, mis));
	/* a binding is 4-byte aligned at least (one that is not is copied), and what the load knew of its offset's
	 * alignment beyond that is gone */
	nir_intrinsic_set_align(intr, MIN2(nir_intrinsic_align_mul(intr), 4), 0);
	nir_intrinsic_set_range_base(intr, 0);
	nir_intrinsic_set_range(intr, ~0);
	return true;
}

/* --- a stage -------------------------------------------------------------------------------------------------------- */

/* Metal's discard_fragment is NIR's demote (air_intrinsics.c); Bifrost knows terminate, and its DISCARD leaves the
 * thread running for its quad's derivatives -- the helper lane demote asks for */
static bool
demote_to_terminate(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
	(void)data;
	if (intr->intrinsic == nir_intrinsic_demote) {
		b->cursor = nir_before_instr(&intr->instr);
		nir_terminate(b);
		nir_instr_remove(&intr->instr);
		return true;
	}
	if (intr->intrinsic == nir_intrinsic_demote_if) {
		b->cursor = nir_before_instr(&intr->instr);
		nir_terminate_if(b, intr->src[0].ssa);
		nir_instr_remove(&intr->instr);
		return true;
	}
	return false;
}

static void
finalize(nir_shader *nir)
{
	NIR_PASS(_, nir, nir_shader_intrinsics_pass, demote_to_terminate, nir_metadata_none, NULL);
	NIR_PASS(_, nir, nir_lower_system_values);
	static const nir_lower_compute_system_values_options cs_options = { 0 };
	NIR_PASS(_, nir, nir_lower_compute_system_values, &cs_options);
	nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

void
rwa_stage_free(struct rwa_stage *st)
{
	if (st->code)
		km_bo_release(st->code, km_last_seq());
	if (st->mem)
		ralloc_free(st->mem);
	memset(st, 0, sizeof(*st));
}

static int
upload_code(struct rwa_stage *st, const struct util_dynarray *bin, char *why, size_t why_size)
{
	/* the binary and the 128 bytes of prefetch the shader core reads past its end (pan_pool_upload_aligned: 128) */
	st->code = km_bo_new(bin->size + 256, why, why_size);
	if (st->code == NULL)
		return -1;
	memcpy(st->code->cpu, bin->data, bin->size);
	memset(st->code->cpu + bin->size, 0, 256);
	st->code_size = bin->size;
	return 0;
}

int
rwa_stage_compile(struct rwa_device *device, struct rwa_stage *st, const void *bitcode, size_t size, const char *entry,
		  gl_shader_stage want, const struct rwa_constant *constants, unsigned nconst, const void *config,
		  unsigned config_size, const struct rwa_render_shape *shape, char *why, size_t why_size)
{
	(void)shape;
	memset(st, 0, sizeof(*st));
	st->mem = ralloc_context(NULL);

	struct air_constant_value *fc = NULL;
	unsigned num_fc = air_module_function_constants(st->mem, bitcode, size, &fc);
	unsigned defined[64];
	unsigned ndefined = 0;
	for (unsigned i = 0; i < num_fc; i++)
		for (unsigned k = 0; k < nconst; k++)
			if (constants[k].index == fc[i].index)
				fc[i].bits = constants[k].bits;
	for (unsigned k = 0; k < nconst && ndefined < 64; k++)
		defined[ndefined++] = constants[k].index;
	const struct air_const_buffer cb = { .index = 0, .data = config, .size = config_size };
	const struct air_to_nir_options opts = {
		.constants = fc, .num_constants = num_fc,
		.const_buffers = &cb, .num_const_buffers = config_size ? 1 : 0,
		.defined = defined, .num_defined = ndefined,
	};
	const char *error = NULL;
	nir_shader *nir = air_to_nir(st->mem, bitcode, size, entry, &opts, GENX(pan_shader_get_compiler_options)(),
				     &st->air, &error);
	if (nir == NULL) {
		say(why, why_size, "%s", error ? error : "the module could not be translated");
		rwa_stage_free(st);
		return -1;
	}
	snprintf(st->name, sizeof(st->name), "%s", st->air.entry_point ? st->air.entry_point : (entry ? entry : "?"));
	if (nir->info.stage != want) {
		say(why, why_size, "%s is a %s function, and a %s one is wanted", st->name,
		    _mesa_shader_stage_to_string(nir->info.stage), _mesa_shader_stage_to_string(want));
		rwa_stage_free(st);
		return -1;
	}
	for (unsigned i = 0; i < st->air.num_resources; i++) {
		const struct air_resource *r = &st->air.resources[i];
		if (r->unused)
			continue;
		if (r->kind == AIR_RESOURCE_IMAGE) {
			say(why, why_size, "%s writes or reads texture %u as an image (not served yet by the native library)",
			    st->name, r->index);
			rwa_stage_free(st);
			return -1;
		}
		if ((r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_SAMPLER) && r->index >= RWA_MAX_SLOTS) {
			say(why, why_size, "%s binds slot %u, past the %u this library has", st->name, r->index, RWA_MAX_SLOTS);
			rwa_stage_free(st);
			return -1;
		}
		if (r->kind == AIR_RESOURCE_SAMPLER && r->inline_sampler) {
			struct rwa_sampler_state m;
			sampler_from_air(&r->sampler, &m);
			if (rwa_sampler_desc(st->inline_sampler[r->index], &m, m.compare_function != 0, why, why_size) != 0) {
				rwa_stage_free(st);
				return -1;
			}
			st->inline_mask |= 1ull << r->index;
		}
	}
	finalize(nir);
	if (want == MESA_SHADER_VERTEX) {
		/* the inputs it reads after the constants decided, numbered 0.. in attribute order (air_metal.c's
		 * compact_vertex_inputs): Panfrost fetches attribute descriptor N for driver location N, and has as many
		 * as there are inputs read */
		bool progress;
		do {
			progress = false;
			NIR_PASS(progress, nir, nir_copy_prop);
			NIR_PASS(progress, nir, nir_opt_dce);
			NIR_PASS(progress, nir, nir_opt_constant_folding);
			NIR_PASS(progress, nir, nir_opt_dead_cf);
		} while (progress);
		NIR_PASS(_, nir, nir_remove_dead_derefs);
		NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_in, NULL);
		nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
		uint64_t read = nir->info.inputs_read;
		unsigned n = 0;
		for (unsigned loc = VERT_ATTRIB_GENERIC0; loc < VERT_ATTRIB_GENERIC0 + 32; loc++)
			if (read & BITFIELD64_BIT(loc))
				st->vs_attr[n++] = (uint8_t)(loc - VERT_ATTRIB_GENERIC0);
		st->vs_attr_count = n;
		nir_foreach_shader_in_variable(var, nir) {
			unsigned loc = var->data.location;
			if (loc >= VERT_ATTRIB_GENERIC0 && (read & BITFIELD64_BIT(loc)))
				var->data.driver_location = util_bitcount64(read & BITFIELD64_MASK(loc));
		}
	}
	bifrost_preprocess_nir(nir, device->gpu_id);   /* pan_shader_preprocess, v6 and later */
	/* noperspective varyings: the fragment stage's set (pan_nir_collect_noperspective_varyings_fs, as
	 * panfrost_create_shader_state keeps it), and the vertex stage multiplied by W for exactly that set */
	if (want == MESA_SHADER_FRAGMENT)
		st->noperspective = pan_nir_collect_noperspective_varyings_fs(nir);
	if (want == MESA_SHADER_VERTEX)
		NIR_PASS(_, nir, pan_nir_lower_static_noperspective, rwa_next_noperspective);

	/* the system values: one buffer after the shader's own (panfrost_nir_lower_sysvals) */
	st->sysval_ubo = nir->info.num_ubos;
	struct sysval_ctx sctx = { st, false };
	NIR_PASS(_, nir, nir_copy_prop);
	NIR_PASS(_, nir, nir_opt_constant_folding);
	NIR_PASS(_, nir, nir_opt_dce);
	nir_shader_instructions_pass(nir, lower_sysval, nir_metadata_control_flow, &sctx);
	memset(st->ubo_dynamic, 0, sizeof(st->ubo_dynamic));
	memset(st->ubo_extent, 0, sizeof(st->ubo_extent));
	st->ubo_extent_known = true;
	nir_shader_intrinsics_pass(nir, ubo_scan, nir_metadata_all, st);
	if (!boot_arg_set("rwn_ubocopy=1"))
		nir_shader_intrinsics_pass(nir, ubo_misalign, nir_metadata_control_flow, &sctx);
	else
		memset(st->ubo_dynamic, 0, sizeof(st->ubo_dynamic));
	if (sctx.need_ubo)
		nir->info.num_ubos = st->sysval_ubo + 1;
	st->ubo_count = nir->info.num_ubos;
	if (st->ubo_count > RWA_MAX_UBOS) {
		say(why, why_size, "%s has %u uniform buffers, more than the %u here", st->name, st->ubo_count, RWA_MAX_UBOS);
		rwa_stage_free(st);
		return -1;
	}

	struct panfrost_compile_inputs inputs = {
		.gpu_id = device->gpu_id,
		.no_idvs = true,
		.pushable_ubos = ~0u,
	};
	struct util_dynarray bin;
	util_dynarray_init(&bin, NULL);
	GENX(pan_shader_compile)(nir, &inputs, &bin, &st->info);
	st->texture_count = st->info.texture_count;
	st->sampler_count = st->info.sampler_count;
	int e = upload_code(st, &bin, why, why_size);
	util_dynarray_fini(&bin);
	ralloc_free(nir);
	if (e != 0) {
		rwa_stage_free(st);
		return -1;
	}
	if (st->info.vs.idvs || st->info.vs.secondary_enable) {
		say(why, why_size, "%s came out an IDVS shader, which this library does not launch", st->name);
		rwa_stage_free(st);
		return -1;
	}
	struct mali_renderer_state_packed rsd;
	pan_pack(&rsd, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(&st->info, st->code->gpu, &cfg);
	}
	memcpy(st->rsd, rsd.opaque, sizeof(st->rsd));
	return 0;
}

/* --- a render pipeline's fixed state -------------------------------------------------------------------------------- */

static enum pipe_blendfactor
blend_factor(unsigned f, bool *ok)
{
	/* MTLBlendFactor */
	switch (f) {
	case 0: return PIPE_BLENDFACTOR_ZERO;
	case 1: return PIPE_BLENDFACTOR_ONE;
	case 2: return PIPE_BLENDFACTOR_SRC_COLOR;
	case 3: return PIPE_BLENDFACTOR_INV_SRC_COLOR;
	case 4: return PIPE_BLENDFACTOR_SRC_ALPHA;
	case 5: return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
	case 6: return PIPE_BLENDFACTOR_DST_COLOR;
	case 7: return PIPE_BLENDFACTOR_INV_DST_COLOR;
	case 8: return PIPE_BLENDFACTOR_DST_ALPHA;
	case 9: return PIPE_BLENDFACTOR_INV_DST_ALPHA;
	case 10: return PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE;
	case 11: return PIPE_BLENDFACTOR_CONST_COLOR;
	case 12: return PIPE_BLENDFACTOR_INV_CONST_COLOR;
	case 13: return PIPE_BLENDFACTOR_CONST_ALPHA;
	case 14: return PIPE_BLENDFACTOR_INV_CONST_ALPHA;
	case 15: return PIPE_BLENDFACTOR_SRC1_COLOR;
	case 16: return PIPE_BLENDFACTOR_INV_SRC1_COLOR;
	case 17: return PIPE_BLENDFACTOR_SRC1_ALPHA;
	case 18: return PIPE_BLENDFACTOR_INV_SRC1_ALPHA;
	default: *ok = false; return PIPE_BLENDFACTOR_ONE;
	}
}

static enum pipe_blend_func
blend_func(unsigned op, bool *ok)
{
	switch (op) {   /* MTLBlendOperation */
	case 0: return PIPE_BLEND_ADD;
	case 1: return PIPE_BLEND_SUBTRACT;
	case 2: return PIPE_BLEND_REVERSE_SUBTRACT;
	case 3: return PIPE_BLEND_MIN;
	case 4: return PIPE_BLEND_MAX;
	default: *ok = false; return PIPE_BLEND_ADD;
	}
}

bool
rwa_no_blend_shaders(void)
{
	static int off = -1;
#ifndef RWA_HOST_FAKE
	if (off < 0) {
		char args[1024] = "";
		size_t n = sizeof(args) - 1;
		off = sysctlbyname("kern.bootargs", args, &n, NULL, 0) == 0 && strstr(args, "rwn_bs=0") != NULL;
	}
#else
	off = 0;
#endif
	return off;
}

static struct pan_blend_shader_cache blend_cache;
static bool blend_cache_ready;

/* a target the fixed function cannot blend: its shader, as panfrost_get_blend asks pan_blend.c for one (the
 * fragment shader's output types; no constants), kept with the render pipeline until its code is laid out */
static int
blend_shader(struct rwa_render *r, unsigned rt, struct pan_blend_equation eq, char *why, size_t why_size)
{
	if (!blend_cache_ready) {
		pan_blend_shader_cache_init(&blend_cache, r->device->gpu_id);
		blend_cache_ready = true;
	}
	struct pan_blend_state state = { 0 };
	state.rt_count = MAX2(r->rt_count, rt + 1);
	state.rts[rt].format = r->rt_format[rt];
	state.rts[rt].nr_samples = 1;
	state.rts[rt].equation = eq;
	pthread_mutex_lock(&blend_cache.lock);
	struct pan_blend_shader_variant *v = GENX(pan_blend_get_shader_locked)(
	    &blend_cache, &state, r->fs.info.bifrost.blend[rt].type, r->fs.info.bifrost.blend_src1_type, rt);
	if (v == NULL || v->binary.size == 0) {
		pthread_mutex_unlock(&blend_cache.lock);
		say(why, why_size, "colour attachment %u: no blend shader came out", rt);
		return -1;
	}
	r->blend_bin[rt] = malloc(v->binary.size);
	memcpy(r->blend_bin[rt], v->binary.data, v->binary.size);
	r->blend_bin_size[rt] = v->binary.size;
	r->blend_first_tag[rt] = v->first_tag;
	pthread_mutex_unlock(&blend_cache.lock);
	r->blend_shader_mask |= 1u << rt;
	return 0;
}

/* the Blend descriptor of target RT (panfrost_emit_blend, v6/v7); a blend shader's address is filled once the code is
 * laid out (rwa_render_prepare) */
static int
blend_desc(struct rwa_render *r, unsigned rt, char *why, size_t why_size)
{
	const struct rwa_color_blend *cb = rt < r->shape.color_blend_count ? &r->shape.color_blend[rt] : NULL;
	struct mali_blend_packed *packed = (struct mali_blend_packed *)r->blend[rt];
	enum pipe_format format = r->rt_format[rt];
	bool written = (r->fs_writes >> rt) & 1;
	/* MTLColorWriteMask: red 8, green 4, blue 2, alpha 1 -- Gallium's (and pan_blend's) the other way round */
	unsigned metal_mask = cb ? cb->write_mask : 0xf;
	unsigned mask = ((metal_mask >> 3) & 1) | ((metal_mask >> 1) & 2) | ((metal_mask << 1) & 4) | ((metal_mask << 3) & 8);
	/* channels the format does not have are not written whatever the mask says: a mask over all it has is whole */
	if (format != PIPE_FORMAT_NONE) {
		const struct util_format_description *fd = util_format_description(format);
		unsigned have = 0;
		for (unsigned c = 0; c < 4; c++)
			if (fd->swizzle[c] <= PIPE_SWIZZLE_W)
				have |= 1u << c;
		if (have && (mask & have) == have)
			mask = 0xf;
		else if ((mask & have) == 0)
			mask = 0;
	}

	if (format == PIPE_FORMAT_NONE || !written || mask == 0) {
		pan_pack(packed, BLEND, cfg) {
			cfg.enable = false;
			cfg.internal.mode = MALI_BLEND_MODE_OFF;
		}
		return 0;
	}
	struct pan_blend_equation eq = { .color_mask = mask };
	bool ok = true;
	if (cb && cb->blending_enabled) {
		eq.blend_enable = true;
		eq.rgb_func = blend_func(cb->rgb_blend_operation, &ok);
		eq.alpha_func = blend_func(cb->alpha_blend_operation, &ok);
		eq.rgb_src_factor = blend_factor(cb->source_rgb_blend_factor, &ok);
		eq.rgb_dst_factor = blend_factor(cb->destination_rgb_blend_factor, &ok);
		eq.alpha_src_factor = blend_factor(cb->source_alpha_blend_factor, &ok);
		eq.alpha_dst_factor = blend_factor(cb->destination_alpha_blend_factor, &ok);
	}
	if (!ok) {
		say(why, why_size, "colour attachment %u: a blend factor or operation Metal does not have", rt);
		return -1;
	}
	if (eq.blend_enable && pan_blend_constant_mask(eq) != 0) {
		say(why, why_size, "colour attachment %u blends with the blend colour, which this contract does not carry", rt);
		return -1;
	}
	bool opaque = pan_blend_is_opaque(eq);
	bool reads = pan_blend_reads_dest(eq);
	/* boot-arg rwn_bs=0: no blend shaders -- such a target written as it comes out (for telling what they cost) */
	if ((!opaque && !rwa_rt_blendable(format)) && rwa_no_blend_shaders()) {
		eq.blend_enable = false;
		opaque = pan_blend_is_opaque(eq);
		reads = pan_blend_reads_dest(eq);
	}
	if ((!opaque && !rwa_rt_blendable(format)) || !pan_blend_can_fixed_function(eq, true)) {
		/* panfrost_get_blend: a shader, unless the equation is opaque (Bifrost converts any format then) */
		if (blend_shader(r, rt, eq, why, why_size) != 0)
			return -1;
		pan_pack(packed, BLEND, cfg) {
			cfg.srgb = util_format_is_srgb(format);
			cfg.load_destination = reads;
			cfg.round_to_fb_precision = false;
			cfg.alpha_to_one = false;
		}
		if (reads)
			r->reads_dest_mask |= 1u << rt;
		return 0;
	}
	struct mali_blend_equation_packed eqp;
	pan_pack(&eqp, BLEND_EQUATION, cfg) {
		pan_blend_to_fixed_function_equation(eq, &cfg);
	}
	pan_pack(packed, BLEND, cfg) {
		cfg.srgb = util_format_is_srgb(format);
		cfg.load_destination = reads;
		cfg.round_to_fb_precision = false;     /* dithered, as compiler.c's (proved on the phone, runs 513..516) */
		cfg.alpha_to_one = false;
		cfg.constant = 0;
	}
	packed->opaque[1] = eqp.opaque[0];
	struct mali_internal_blend_packed *internal = (struct mali_internal_blend_packed *)&packed->opaque[2];
	pan_pack(internal, INTERNAL_BLEND, cfg) {
		cfg.mode = opaque ? MALI_BLEND_MODE_OPAQUE : MALI_BLEND_MODE_FIXED_FUNCTION;
		cfg.fixed_function.num_comps = 4;
		cfg.fixed_function.conversion.memory_format = GENX(panfrost_dithered_format_from_pipe_format)(format, true);
		if (cfg.mode == MALI_BLEND_MODE_FIXED_FUNCTION &&
		    (cfg.fixed_function.conversion.memory_format & 0xff) == MALI_RGB_COMPONENT_ORDER_RGB1) {
			cfg.fixed_function.conversion.memory_format &= ~0xff;
			cfg.fixed_function.conversion.memory_format |= MALI_RGB_COMPONENT_ORDER_RGBA;
		}
		cfg.fixed_function.rt = rt;
		if (!opaque) {
			cfg.fixed_function.alpha_zero_nop = pan_blend_alpha_zero_nop(eq);
			cfg.fixed_function.alpha_one_store = pan_blend_alpha_one_store(eq);
		}
		cfg.fixed_function.conversion.register_format = r->fs.info.bifrost.blend[rt].format;
	}
	if (reads)
		r->reads_dest_mask |= 1u << rt;
	return 0;
}

static unsigned
varying_size(enum pipe_format f)
{
	return util_format_get_blocksize(f);
}

/* the varyings linked (pan_assign_varyings, panfrost_emit_varying_descs): the general buffer's offsets in the vertex
 * shader's output order for the outputs the fragment shader reads, in the reader's format */
static int
link_varyings(struct rwa_render *r, char *why, size_t why_size)
{
	const struct pan_shader_info *vs = &r->vs.info, *fs = &r->fs.info;
	unsigned stride = 0;

	if (vs->varyings.output_count > RWA_MAX_VARYINGS || fs->varyings.input_count > RWA_MAX_VARYINGS) {
		say(why, why_size, "more than %u varyings", RWA_MAX_VARYINGS);
		return -1;
	}
	r->vs_out_count = vs->varyings.output_count;
	r->fs_in_count = fs->varyings.input_count;
	for (unsigned i = 0; i < vs->varyings.output_count; i++) {
		const struct pan_shader_varying *o = &vs->varyings.output[i];
		struct rwa_varying_rec *rec = &r->vs_out[i];
		if (o->location == VARYING_SLOT_POS) {
			rec->buffer = 1;
			rec->format = (MALI_SNAP_4 << 12) | MALI_RGB_COMPONENT_ORDER_RGBA;   /* panfrost_special_format POS */
			rec->offset = 0;
			continue;
		}
		if (o->location == VARYING_SLOT_PSIZ) {
			rec->buffer = 2;
			rec->format = GENX(panfrost_format_from_pipe_format)(PIPE_FORMAT_R16_FLOAT)->hw;
			rec->offset = 0;
			r->writes_psiz = true;
			continue;
		}
		rec->buffer = 0;
		rec->offset = -1;
		for (unsigned j = 0; j < fs->varyings.input_count; j++) {
			const struct pan_shader_varying *in = &fs->varyings.input[j];
			if (in->location != o->location)
				continue;
			rec->offset = (int32_t)stride;
			rec->format = GENX(panfrost_format_from_pipe_format)(in->format)->hw;
			stride += varying_size(in->format);
			break;
		}
		if (rec->offset < 0) {
			/* written and not read: discarded (MALI_CONSTANT, panfrost_emit_varying for an unlinked one) */
			rec->format = (MALI_CONSTANT << 12) | MALI_RGB_COMPONENT_ORDER_0000;
			rec->offset = 0;
		}
	}
	for (unsigned j = 0; j < fs->varyings.input_count; j++) {
		const struct pan_shader_varying *in = &fs->varyings.input[j];
		struct rwa_varying_rec *rec = &r->fs_in[j];
		rec->buffer = 0;
		rec->offset = -1;
		if (in->location == VARYING_SLOT_POS || in->location == VARYING_SLOT_PNTC || in->location == VARYING_SLOT_FACE) {
			/* Bifrost reads these with LD_VAR_SPECIAL, not through the record (pan_varying_present: only
			 * Midgard has buffers for them); the record is there and unused */
			rec->format = (MALI_CONSTANT << 12) | MALI_RGB_COMPONENT_ORDER_0000;
			rec->offset = 0;
			continue;
		}
		for (unsigned i = 0; i < vs->varyings.output_count; i++)
			if (vs->varyings.output[i].location == in->location) {
				rec->offset = r->vs_out[i].offset;
				rec->format = GENX(panfrost_format_from_pipe_format)(in->format)->hw;
			}
		if (rec->offset < 0) {
			/* read and never written: Metal leaves it undefined; zeroes */
			rec->format = (MALI_CONSTANT << 12) | MALI_RGB_COMPONENT_ORDER_0000;
			rec->offset = 0;
		}
	}
	r->general_stride = stride;
	return 0;
}

int
rwa_render_prepare(struct rwa_render *r, char *why, size_t why_size)
{
	const struct rwa_render_shape *shape = &r->shape;

	r->samples = shape->sample_count ? shape->sample_count : 1;
	if (r->samples != 1 && r->samples != 4) {
		say(why, why_size, "%u samples a pixel (1 or 4 here)", r->samples);
		return -1;
	}
	r->rt_count = shape->color_count;
	for (unsigned i = 0; i < shape->color_count; i++) {
		r->rt_format[i] = shape->color_format[i] ? rwa_pipe_format(shape->color_format[i]) : PIPE_FORMAT_NONE;
		if (shape->color_format[i] && r->rt_format[i] == PIPE_FORMAT_NONE) {
			say(why, why_size, "colour attachment %u: pixel format %u is not one this library knows", i,
			    shape->color_format[i]);
			return -1;
		}
		uint32_t in, wb, sw;
		if (r->rt_format[i] != PIPE_FORMAT_NONE && !rwa_rt_format(r->rt_format[i], &in, &wb, &sw, NULL)) {
			say(why, why_size, "colour attachment %u: %s is not a format this GPU renders into by blending", i,
			    rwa_pixel_format_name(shape->color_format[i]));
			return -1;
		}
	}
	unsigned zs = shape->depth_format ? shape->depth_format : shape->stencil_format;
	r->zs_format = zs ? rwa_pipe_format(zs) : PIPE_FORMAT_NONE;
	if (zs && !util_format_is_depth_or_stencil(r->zs_format)) {
		say(why, why_size, "the depth attachment's pixel format %u is not a depth format here", zs);
		return -1;
	}
	/* what the fragment stage writes */
	r->fs_writes = (unsigned)(r->fs.info.outputs_written >> FRAG_RESULT_DATA0) & 0xff;
	r->fs_writes_depth = r->fs.info.fs.writes_depth;
	r->fs_writes_stencil = r->fs.info.fs.writes_stencil;
	r->can_discard = r->fs.info.fs.can_discard;
	r->fs_fetches = r->fs.air.fbfetch_targets;
	for (unsigned rt = 0; rt < MAX2(r->rt_count, 1); rt++)
		if (blend_desc(r, rt, why, why_size) != 0)
			return -1;
	if (link_varyings(r, why, why_size) != 0)
		return -1;

	/* the vertex attributes, by driver location (build_vertex_state, over the compacted inputs) */
	r->attr_count = r->vs.vs_attr_count;
	if (r->vs.info.attribute_count != r->attr_count) {
		say(why, why_size, "%s: the compiler fetches %u attributes and %u inputs are read", r->vs.name,
		    r->vs.info.attribute_count, r->attr_count);
		return -1;
	}
	for (unsigned i = 0; i < r->attr_count; i++) {
		unsigned index = r->vs.vs_attr[i];
		const struct air_vertex_attribute *a = NULL;
		for (unsigned k = 0; k < r->vs.air.num_vertex_attributes; k++)
			if (r->vs.air.vertex_attributes[k].index == index)
				a = &r->vs.air.vertex_attributes[k];
		struct rwa_attr_rec *rec = &r->attr[i];
		const struct rwa_vertex_attribute *va = index < RWA_MAX_VERTEX_ATTRIBUTES ? &shape->vertex_attribute[index] : NULL;
		if (va == NULL || va->format == 0) {
			if (a == NULL || a->function_constant == NULL) {
				say(why, why_size, "%s reads attribute %u and the vertex descriptor configures none there", r->vs.name,
				    index);
				return -1;
			}
			/* behind a function constant the descriptor did not set up: zeros (air_metal.c's constant element) */
			rec->buffer = RWA_ATTR_ZERO;
			rec->format = rwa_attribute_format(PIPE_FORMAT_R32G32B32A32_FLOAT);
			rec->offset = 0;
			rec->step = 0;
			rec->step_rate = 1;
			continue;
		}
		unsigned fsize = 0;
		enum pipe_format pf = rwa_vertex_format(va->format, &fsize);
		if (pf == PIPE_FORMAT_NONE) {
			say(why, why_size, "attribute %u: MTLVertexFormat %u is not one this library knows", index, va->format);
			return -1;
		}
		if (va->buffer_index >= RWA_MAX_VERTEX_BUFFERS) {
			say(why, why_size, "attribute %u names buffer %u", index, va->buffer_index);
			return -1;
		}
		const struct rwa_vertex_layout *l = &shape->vertex_layout[va->buffer_index];
		rec->buffer = va->buffer_index;
		rec->format = rwa_attribute_format(pf);
		rec->offset = va->offset;
		rec->step = l->step_function;
		rec->step_rate = l->step_rate ? l->step_rate : 1;
		if (l->step_function > 2) {
			say(why, why_size, "buffer %u steps per patch, which is not drawn here", va->buffer_index);
			return -1;
		}
	}

	/* blend shaders: in the fragment shader's buffer, after its code -- the hardware takes a blend shader's address
	 * as the low 32 bits beside the fragment shader's high ones (panfrost_emit_blend's assert) */
	if (r->blend_shader_mask) {
		uint64_t at = ALIGN_POT(r->fs.code_size, 128), total = at;
		uint64_t off[8] = { 0 };
		for (unsigned rt = 0; rt < 8; rt++)
			if (r->blend_shader_mask & (1u << rt)) {
				off[rt] = total;
				total = ALIGN_POT(total + r->blend_bin_size[rt], 128);
			}
		struct km_bo *bo = km_bo_new(total + 256, why, why_size);
		if (bo == NULL)
			return -1;
		memcpy(bo->cpu, r->fs.code->cpu, r->fs.code_size);
		for (unsigned rt = 0; rt < 8; rt++)
			if (r->blend_shader_mask & (1u << rt))
				memcpy(bo->cpu + off[rt], r->blend_bin[rt], r->blend_bin_size[rt]);
		km_bo_release(r->fs.code, 0);
		r->fs.code = bo;
		if (((bo->gpu + total) >> 32) != (bo->gpu >> 32)) {
			say(why, why_size, "the fragment shader's code crosses a 4 GB line, and its blend shaders must not");
			return -1;
		}
		struct mali_renderer_state_packed rsd;
		pan_pack(&rsd, RENDERER_STATE, cfg) {
			pan_shader_prepare_rsd(&r->fs.info, bo->gpu, &cfg);
		}
		memcpy(r->fs.rsd, rsd.opaque, sizeof(r->fs.rsd));
		for (unsigned rt = 0; rt < 8; rt++) {
			if (!(r->blend_shader_mask & (1u << rt)))
				continue;
			struct mali_blend_packed *packed = (struct mali_blend_packed *)r->blend[rt];
			struct mali_internal_blend_packed *internal = (struct mali_internal_blend_packed *)&packed->opaque[2];
			unsigned ret = r->fs.info.bifrost.blend[rt].return_offset;
			pan_pack(internal, INTERNAL_BLEND, cfg) {
				cfg.mode = MALI_BLEND_MODE_SHADER;
				cfg.shader.pc = (uint32_t)(bo->gpu + off[rt]) | r->blend_first_tag[rt];
				cfg.shader.return_value = ret ? (uint32_t)(bo->gpu + ret) : 0;
			}
			free(r->blend_bin[rt]);
			r->blend_bin[rt] = NULL;
		}
	}

	/* the fragment Renderer State before the draw's depth state: panfrost_emit_frag_shader_meta's merge of the
	 * shader's words and the fixed state; the pixel kill and depth words are the draw's (rwa_render_fs_rsd) */
	r->earlyzs = pan_earlyzs_analyze(&r->fs.info);
	memcpy(r->fs_rsd, r->fs.rsd, sizeof(r->fs_rsd));
	return 0;
}

static enum mali_stencil_op
stencil_op(unsigned op)
{
	/* MTLStencilOperation: Keep 0, Zero 1, Replace 2, IncrementClamp 3, DecrementClamp 4, Invert 5, IncrementWrap 6,
	 * DecrementWrap 7 */
	static const enum mali_stencil_op ops[] = {
		MALI_STENCIL_OP_KEEP, MALI_STENCIL_OP_ZERO, MALI_STENCIL_OP_REPLACE, MALI_STENCIL_OP_INCR_SAT,
		MALI_STENCIL_OP_DECR_SAT, MALI_STENCIL_OP_INVERT, MALI_STENCIL_OP_INCR_WRAP, MALI_STENCIL_OP_DECR_WRAP,
	};
	return op < ARRAY_SIZE(ops) ? ops[op] : MALI_STENCIL_OP_KEEP;
}

static bool
face_used(const struct rwa_stencil_face *f)
{
	return f->compare_function != 7 || f->stencil_fail_operation || f->depth_fail_operation ||
	       f->depth_stencil_pass_operation;
}

void
rwa_render_fs_rsd(const struct rwa_render *r, const struct rwa_depth_stencil_state *zs, bool has_zs, unsigned samples,
		  void *out)
{
	struct mali_renderer_state_packed rsd, fixed;
	const bool has_s = has_zs && util_format_has_stencil(util_format_description(r->zs_format));
	const bool has_d = has_zs && util_format_has_depth(util_format_description(r->zs_format));
	const unsigned depth_fn = (zs && has_d) ? (zs->depth_compare_function & 7) : 7;
	const bool depth_write = zs && has_d && zs->depth_write_enabled;
	const bool stencil = zs && has_s && (face_used(&zs->front) || face_used(&zs->back));
	const bool writes_zs = depth_write || (stencil && (zs->front.write_mask || zs->back.write_mask));
	const bool always_passes = depth_fn == 7 && !stencil;
	struct pan_earlyzs_state ez = pan_earlyzs_get(r->earlyzs, writes_zs, false, always_passes);
	bool fpk = r->fs.info.fs.can_fpk && r->reads_dest_mask == 0 && !r->fs.info.writes_global &&
		   (r->fs_writes & ((1u << MAX2(r->rt_count, 1)) - 1)) == ((1u << MAX2(r->rt_count, 1)) - 1);

	pan_pack(&fixed, RENDERER_STATE, cfg) {
		cfg.properties.pixel_kill_operation = (enum mali_pixel_kill)ez.kill;
		cfg.properties.zs_update_operation = (enum mali_pixel_kill)ez.update;
		cfg.properties.allow_forward_pixel_to_kill = fpk;
		cfg.properties.point_sprite_coord_origin_max_y = false;
		cfg.multisample_misc.sample_mask = 0xFFFF;
		cfg.multisample_misc.multisample_enable = samples > 1;
		cfg.multisample_misc.evaluate_per_sample = samples > 1 && r->fs.info.fs.sample_shading;
		cfg.multisample_misc.overdraw_alpha0 = true;
		cfg.multisample_misc.overdraw_alpha1 = true;
		cfg.multisample_misc.fixed_function_near_discard = true;
		cfg.multisample_misc.fixed_function_far_discard = true;
		cfg.multisample_misc.fixed_function_depth_range_fixed = true;
		cfg.multisample_misc.shader_depth_range_fixed = true;
		cfg.multisample_misc.depth_function = (enum mali_func)depth_fn;
		cfg.multisample_misc.depth_write_mask = depth_write;
		cfg.stencil_mask_misc.single_sampled_lines = samples <= 1;
		cfg.stencil_mask_misc.stencil_enable = stencil;
		if (stencil) {
			cfg.stencil_mask_misc.stencil_mask_front = zs->front.write_mask & 0xff;
			cfg.stencil_mask_misc.stencil_mask_back = zs->back.write_mask & 0xff;
			cfg.stencil_front.compare_function = (enum mali_func)(zs->front.compare_function & 7);
			cfg.stencil_front.stencil_fail = stencil_op(zs->front.stencil_fail_operation);
			cfg.stencil_front.depth_fail = stencil_op(zs->front.depth_fail_operation);
			cfg.stencil_front.depth_pass = stencil_op(zs->front.depth_stencil_pass_operation);
			cfg.stencil_front.mask = zs->front.read_mask & 0xff;
			cfg.stencil_back.compare_function = (enum mali_func)(zs->back.compare_function & 7);
			cfg.stencil_back.stencil_fail = stencil_op(zs->back.stencil_fail_operation);
			cfg.stencil_back.depth_fail = stencil_op(zs->back.depth_fail_operation);
			cfg.stencil_back.depth_pass = stencil_op(zs->back.depth_stencil_pass_operation);
			cfg.stencil_back.mask = zs->back.read_mask & 0xff;
		} else {
			cfg.stencil_front.compare_function = MALI_FUNC_ALWAYS;
			cfg.stencil_back.compare_function = MALI_FUNC_ALWAYS;
		}
	}
	memcpy(&rsd, r->fs_rsd, sizeof(rsd));
	pan_merge(rsd, fixed, RENDERER_STATE);
	memcpy(out, rsd.opaque, sizeof(rsd));
}

/* --- the frame shader that loads a target: pan_fb_preload.c's pan_preload_get_shader, one float 2D surface ----------- */

/* the frame shader for the targets in MASK, one texture each in target order (pan_preload_get_shader's loop) */
int
rwa_preload_shader(struct rwa_device *device, struct rwa_stage *st, unsigned mask, char *why, size_t why_size)
{
	nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, GENX(pan_shader_get_compiler_options)(),
						       "rwa_preload_%x", mask);
	nir_def *bary = nir_load_barycentric(&b, nir_intrinsic_load_barycentric_pixel, INTERP_MODE_SMOOTH);
	nir_def *coord = nir_load_interpolated_input(&b, 2, 32, bary, nir_imm_int(&b, 0), .base = 0,
						     .dest_type = nir_type_float32, .io_semantics.location = VARYING_SLOT_VAR0,
						     .io_semantics.num_slots = 1);
	unsigned active = 0;
	for (unsigned rt = 0; rt < 8; rt++) {
		if (!(mask & (1u << rt)))
			continue;
		nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
		tex->dest_type = nir_type_float32;
		tex->texture_index = active;
		tex->sampler_index = 0;
		tex->is_array = false;
		tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
		tex->op = nir_texop_txl;
		tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
		tex->coord_components = 2;
		nir_def_init(&tex->instr, &tex->def, 4, 32);
		nir_builder_instr_insert(&b, &tex->instr);
		nir_store_output(&b, &tex->def, nir_imm_int(&b, 0), .base = active, .src_type = nir_type_float32,
				 .io_semantics.location = FRAG_RESULT_DATA0 + rt, .io_semantics.num_slots = 1,
				 .write_mask = nir_component_mask(4));
		active++;
	}
	struct panfrost_compile_inputs inputs = { .gpu_id = device->gpu_id, .is_blit = true, .no_idvs = true };
	struct util_dynarray bin;
	util_dynarray_init(&bin, NULL);
	nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
	for (unsigned i = 0; i < active; i++)
		BITSET_SET(b.shader->info.textures_used, i);
	bifrost_preprocess_nir(b.shader, inputs.gpu_id);
	memset(st, 0, sizeof(*st));
	GENX(pan_shader_compile)(b.shader, &inputs, &bin, &st->info);
	snprintf(st->name, sizeof(st->name), "preload %x", mask);
	int e = upload_code(st, &bin, why, why_size);
	util_dynarray_fini(&bin);
	ralloc_free(b.shader);
	if (e != 0)
		return -1;
	/* pan_preload_emit_rsd, colour only, one sample */
	struct mali_renderer_state_packed rsd;
	pan_pack(&rsd, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(&st->info, st->code->gpu, &cfg);
		/* one sampler for all the targets: pan_shader_compile counts as many samplers as textures, and a frame
		 * shader of three targets read two sampler descriptors past the one there is -- DATA_INVALID, every
		 * frame of several targets that loaded (runs 526 to 530; pandecode on the host build) */
		cfg.shader.sampler_count = 1;
		cfg.multisample_misc.sample_mask = 0xFFFF;
		cfg.multisample_misc.depth_function = MALI_FUNC_ALWAYS;
		cfg.stencil_front.compare_function = MALI_FUNC_ALWAYS;
		cfg.stencil_front.stencil_fail = MALI_STENCIL_OP_REPLACE;
		cfg.stencil_front.depth_fail = MALI_STENCIL_OP_REPLACE;
		cfg.stencil_front.depth_pass = MALI_STENCIL_OP_REPLACE;
		cfg.stencil_front.mask = 0xFF;
		cfg.stencil_back = cfg.stencil_front;
		cfg.stencil_mask_misc.stencil_mask_front = 0xFF;
		cfg.stencil_mask_misc.stencil_mask_back = 0xFF;
		cfg.properties.zs_update_operation = MALI_PIXEL_KILL_FORCE_EARLY;
		cfg.properties.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
		cfg.properties.allow_forward_pixel_to_kill = true;
	}
	memcpy(st->rsd, rsd.opaque, sizeof(st->rsd));
	return 0;
}

int
rwa_shader_init(struct rwa_device *device, char *why, size_t why_size)
{
	glsl_type_singleton_init_or_ref();   /* the translator builds types (air_metal.c rwa_device_open) */
	if (rwa_preload_shader(device, &device->preload[1], 1, why, why_size) != 0)
		return -1;
	device->preload_ready = true;
	return 0;
}

/* the preload's Blend descriptor of target RT in FORMAT (pan_preload_emit_blend: replace, the shader's register format) */
void rwa_preload_blend(const struct rwa_device *device, enum pipe_format format, unsigned rt, void *out, bool format_comps);
void
rwa_preload_blend(const struct rwa_device *device, enum pipe_format format, unsigned rt, void *out, bool format_comps)
{
	struct mali_blend_packed *packed = out;

	pan_pack(packed, BLEND, cfg) {
		cfg.round_to_fb_precision = true;
		cfg.srgb = util_format_is_srgb(format);
		cfg.internal.mode = MALI_BLEND_MODE_OPAQUE;
		/* 4, as panfrost_emit_blend; or the format's own channels, as pan_blend_get_internal_desc (run 528's B) */
		cfg.internal.fixed_function.num_comps = format_comps ? util_format_description(format)->nr_channels : 4;
		cfg.internal.fixed_function.conversion.memory_format =
		    GENX(panfrost_dithered_format_from_pipe_format)(format, false);
		cfg.internal.fixed_function.rt = rt;
		cfg.internal.fixed_function.conversion.register_format = MALI_REGISTER_FILE_FORMAT_F32;
		cfg.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
		cfg.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
		cfg.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
		cfg.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
		cfg.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
		cfg.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
		cfg.equation.color_mask = 0xf;
	}
}
