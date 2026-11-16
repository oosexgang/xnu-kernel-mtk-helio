/*
 * The compiler inside the driver (compiler.h): the library's programs as NIR, compiled by Mesa's Bifrost compiler in this
 * process, and their Renderer State, Blend, push and varying tables as Panfrost prepares them. The same code writes
 * shaders_v7.h on the Mac (tools/bishaders.c) and compiles on the phone, so the two can be compared word for word.
 *
 * Every vertex shader goes through pan_shader_preprocess (bifrost_preprocess_nir), which puts in the viewport transform
 * as load_viewport_scale/offset; Panfrost turns those into system values pushed from a buffer (panfrost_nir_lower_sysvals),
 * and so does this: uniform buffer 1, pushed (viewport_sysvals). NDC (x, y) lands on pixel
 * (w/2 + w/2 x, h/2 + h/2 y) of a w x h target. No IDVS (inputs.no_idvs): a vertex job and a tiler job. The fragment shader's Renderer State
 * is merged for a fixed state -- no stencil, MSAA, alpha-to-coverage or query, one RGBA8Unorm target -- without depth,
 * with Metal's Less and writes, and with forward pixel kill off for a blend that reads the destination; the Blend
 * descriptor of the target off and source-over (One, OneMinusSourceAlpha).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "panfrost/compiler/bifrost_compile.h"
#include "util/format/u_format.h"
#include "util/u_dynarray.h"

#include "genxml/gen_macros.h"
#include "pan_blend.h"
#include "pan_earlyzs.h"
#include "pan_format.h"
#include "pan_shader.h"
#include "pan_util.h"

#include "compiler.h"
#include "nir_air.h"

/*
 * load_viewport_scale/offset as system values in uniform buffer 1, which the compiler pushes: words 0..2 the scale,
 * 4..6 the offset (panfrost_nir_lower_sysvals puts Panfrost's own in a uniform buffer after the user's, pushed the
 * same way). The driver fills them for the target: scale (w/2, h/2, 0.5), offset (w/2, h/2, 0.5).
 */
#define HM_SYSVAL_UBO 1
static bool
viewport_sysvals(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
	if (intr->intrinsic != nir_intrinsic_load_viewport_scale && intr->intrinsic != nir_intrinsic_load_viewport_offset) {
		return false;
	}
	unsigned offset = intr->intrinsic == nir_intrinsic_load_viewport_scale ? 0 : 16;

	b->cursor = nir_before_instr(&intr->instr);
	nir_def_replace(&intr->def, nir_load_ubo(b, 3, 32, nir_imm_int(b, HM_SYSVAL_UBO), nir_imm_int(b, offset),
	    .align_mul = 16, .align_offset = 0, .range_base = offset, .range = 12));
	return true;
}

static void
compile(nir_shader *s, unsigned gpu_id, struct util_dynarray *binary, struct pan_shader_info *info)
{
	struct panfrost_compile_inputs inputs = { .gpu_id = gpu_id, .no_idvs = true,
		.pushable_ubos = (1u << 0) | (1u << HM_SYSVAL_UBO) };

	bifrost_preprocess_nir(s, gpu_id);       /* pan_shader_preprocess for an architecture of 6 or more */
	if (s->info.stage == MESA_SHADER_VERTEX) {
		NIR_PASS(_, s, nir_shader_intrinsics_pass, viewport_sysvals, nir_metadata_control_flow, NULL);
		if (s->info.num_ubos < HM_SYSVAL_UBO + 1) {
			s->info.num_ubos = HM_SYSVAL_UBO + 1;
		}
		/* panfrost_shader_compile: no varying is noperspective (key->vs.noperspective_varyings 0) */
		NIR_PASS(_, s, pan_nir_lower_static_noperspective, 0);
	}
	util_dynarray_init(binary, NULL);
	GENX(pan_shader_compile)(s, &inputs, binary, info);
}

static nir_variable *
var(nir_builder *b, nir_variable_mode mode, const struct glsl_type *type, const char *name, int location, int driver)
{
	nir_variable *v = nir_variable_create(b->shader, mode, type, name);

	v->data.location = location;
	v->data.driver_location = driver;
	return v;
}

/* the fragment shader's Renderer State for the fixed state above, as panfrost_emit_frag_shader merges it */
static void
fs_state(const struct pan_shader_info *fs, struct mali_renderer_state_packed *fs_rsd, bool depth)
{
	struct mali_renderer_state_packed partial;
	/* pan_earlyzs_get(fs->earlyzs, writes_zs, alpha_to_coverage, zs_always_passes): a depth test that writes */
	struct pan_earlyzs_state earlyzs = pan_earlyzs_get(pan_earlyzs_analyze(fs), depth, false, !depth);

	pan_pack(fs_rsd, RENDERER_STATE, cfg) {
		cfg.properties.pixel_kill_operation = (enum mali_pixel_kill)earlyzs.kill;
		cfg.properties.zs_update_operation = (enum mali_pixel_kill)earlyzs.update;
		/* pan_allow_forward_pixel_to_kill: the one target written; a blend that reads the destination says no below */
		cfg.properties.allow_forward_pixel_to_kill = fs->fs.can_fpk;
		cfg.multisample_misc.sample_mask = 0xFFFF;
		cfg.multisample_misc.evaluate_per_sample = false;
		cfg.properties.point_sprite_coord_origin_max_y = false;
		cfg.multisample_misc.overdraw_alpha0 = true;
		cfg.multisample_misc.overdraw_alpha1 = true;
	}
	pan_pack(&partial, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(fs, 0, &cfg);
	}
	pan_merge(*fs_rsd, partial, RENDERER_STATE);
	{
		/* the rasterizer's (GL defaults, depth clipped near and far) and the depth and stencil state's (depth off, so
		 * ALWAYS; stencil off) words 8..11 */
		struct mali_multisample_misc_packed rast_ms, zsa_depth;
		struct mali_stencil_mask_misc_packed rast_st, zsa_st;
		struct mali_stencil_packed front;

		pan_pack(&rast_ms, MULTISAMPLE_MISC, cfg) {
			cfg.multisample_enable = false;
			cfg.fixed_function_near_discard = true;
			cfg.fixed_function_far_discard = true;
			cfg.fixed_function_depth_range_fixed = true;
			cfg.shader_depth_range_fixed = true;
		}
		pan_pack(&rast_st, STENCIL_MASK_MISC, cfg) {
			cfg.single_sampled_lines = true;
		}
		/* panfrost_create_depth_stencil_state: depth off is ALWAYS; on, LESS with writes (Metal's Less, write) */
		pan_pack(&zsa_depth, MULTISAMPLE_MISC, cfg) {
			cfg.depth_function = depth ? MALI_FUNC_LESS : MALI_FUNC_ALWAYS;
			cfg.depth_write_mask = depth;
		}
		pan_pack(&zsa_st, STENCIL_MASK_MISC, cfg) {
			cfg.stencil_enable = false;
		}
		pan_pack(&front, STENCIL, s) {
			s.mask = 0;
			s.compare_function = MALI_FUNC_NEVER;
			s.stencil_fail = MALI_STENCIL_OP_KEEP;
			s.depth_fail = MALI_STENCIL_OP_KEEP;
			s.depth_pass = MALI_STENCIL_OP_KEEP;
		}
		fs_rsd->opaque[8] |= zsa_depth.opaque[0] | rast_ms.opaque[0];
		fs_rsd->opaque[9] |= zsa_st.opaque[0] | rast_st.opaque[0];
		fs_rsd->opaque[10] |= front.opaque[0];
		fs_rsd->opaque[11] |= front.opaque[0];
	}
}

/* the target's Blend descriptor (panfrost_emit_blend) for equation EQ */
static void
blend_desc(const struct pan_shader_info *fs, struct pan_blend_equation eq, enum pipe_format format,
    struct mali_blend_packed *blend)
{
	struct mali_blend_equation_packed eqp;
	bool opaque = pan_blend_is_opaque(eq);

	pan_pack(&eqp, BLEND_EQUATION, cfg) {
		pan_blend_to_fixed_function_equation(eq, &cfg);
	}
	pan_pack(blend, BLEND, cfg) {
		cfg.srgb = false;
		cfg.load_destination = pan_blend_reads_dest(eq);
		cfg.round_to_fb_precision = false;       /* !dithered: Gallium's default blend state dithers */
		cfg.alpha_to_one = false;
		cfg.constant = 0;
	}
	blend->opaque[1] = eqp.opaque[0];
	struct mali_internal_blend_packed *internal = (struct mali_internal_blend_packed *)&blend->opaque[2];

	pan_pack(internal, INTERNAL_BLEND, cfg) {
		cfg.mode = opaque ? MALI_BLEND_MODE_OPAQUE : MALI_BLEND_MODE_FIXED_FUNCTION;
		cfg.fixed_function.num_comps = 4;
		cfg.fixed_function.conversion.memory_format =
		    GENX(panfrost_dithered_format_from_pipe_format)(format, true);
		cfg.fixed_function.rt = 0;
		if (!opaque) {
			cfg.fixed_function.alpha_zero_nop = pan_blend_alpha_zero_nop(eq);
			cfg.fixed_function.alpha_one_store = pan_blend_alpha_one_store(eq);
		}
		cfg.fixed_function.conversion.register_format = fs->bifrost.blend[0].format;
	}
}

static nir_builder
shader(gl_shader_stage stage, const char *name)
{
	return nir_builder_init_simple_shader(stage, GENX(pan_shader_get_compiler_options)(), "%s", name);
}


static void *
copy(const void *data, size_t size)
{
	void *p = malloc(size ? size : 4);

	if (p != NULL && size) {
		memcpy(p, data, size);
	}
	return p;
}

static struct hm_varying *
varying_table(const struct pan_shader_varying *v, unsigned count)
{
	struct hm_varying *t = calloc(count ? count : 1, sizeof(*t));

	for (unsigned i = 0; i < count; i++) {
		t[i].location = v[i].location;
		t[i].format = v[i].location == VARYING_SLOT_POS ? 0u : (unsigned)GENX(panfrost_format_from_pipe_format)(v[i].format)->hw;
		t[i].size = v[i].location == VARYING_SLOT_POS ? 16u : util_format_get_blocksize(v[i].format);
	}
	return t;
}

static struct hm_push_word *
push_table(const struct pan_shader_info *info)
{
	struct hm_push_word *t = calloc(info->push.count ? info->push.count : 1, sizeof(*t));

	for (unsigned i = 0; i < info->push.count; i++) {
		t[i].ubo = info->push.words[i].ubo;
		t[i].offset = info->push.words[i].offset;
	}
	return t;
}

static int
build(const char *name, unsigned gpu_id, nir_shader *vs_nir, nir_shader *fs_nir, const enum pipe_format *attributes,
    unsigned nattr, bool blend_over, struct hm_program *out, char *why, size_t why_size)
{
	struct util_dynarray vs_bin, fs_bin;
	struct pan_shader_info vs, fs;

	compile(vs_nir, gpu_id, &vs_bin, &vs);
	compile(fs_nir, gpu_id, &fs_bin, &fs);
	if (vs.vs.idvs || vs.vs.secondary_enable || vs.tls_size || fs.tls_size || vs.attribute_count != nattr) {
		snprintf(why, why_size, "%s is not what the descriptors here expect (idvs %d, tls %u/%u, attributes %u)",
		    name, vs.vs.idvs, vs.tls_size, fs.tls_size, vs.attribute_count);
		return -1;
	}
	struct mali_renderer_state_packed vs_rsd, fs_rsd, fs_rsd_depth, fs_rsd_blended;
	pan_pack(&vs_rsd, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(&vs, 0, &cfg);
	}
	fs_state(&fs, &fs_rsd, false);
	fs_state(&fs, &fs_rsd_depth, true);
	fs_rsd_blended = fs_rsd;
	/* a blend that reads the destination keeps the GPU from killing the pixels it covers (pan_allow_forward_pixel_to_kill) */
	fs_rsd_blended.opaque[4] &= ~(1u << 19);
	struct mali_blend_packed off, over, off_bgra, over_bgra;
	const struct pan_blend_equation eq_off = { .blend_enable = false, .color_mask = 0xf };
	const struct pan_blend_equation eq_over = {
		.blend_enable = true, .rgb_func = PIPE_BLEND_ADD, .rgb_src_factor = PIPE_BLENDFACTOR_ONE,
		.rgb_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA, .alpha_func = PIPE_BLEND_ADD,
		.alpha_src_factor = PIPE_BLENDFACTOR_ONE, .alpha_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA,
		.color_mask = 0xf };
	blend_desc(&fs, eq_off, PIPE_FORMAT_R8G8B8A8_UNORM, &off);
	blend_desc(&fs, eq_over, PIPE_FORMAT_R8G8B8A8_UNORM, &over);
	/* the same for a BGRA8Unorm target (the window server's surfaces) */
	blend_desc(&fs, eq_off, PIPE_FORMAT_B8G8R8A8_UNORM, &off_bgra);
	blend_desc(&fs, eq_over, PIPE_FORMAT_B8G8R8A8_UNORM, &over_bgra);
	uint32_t *formats = calloc(nattr ? nattr : 1, 4), *sizes = calloc(nattr ? nattr : 1, 4);
	for (unsigned i = 0; i < nattr; i++) {
		formats[i] = GENX(panfrost_format_from_pipe_format)(attributes[i])->hw;
		sizes[i] = util_format_get_blocksize(attributes[i]);
	}
	memset(out, 0, sizeof(*out));
	out->name = name;
	out->vs_code = copy(vs_bin.data, vs_bin.size);
	out->vs_size = vs_bin.size;
	out->fs_code = copy(fs_bin.data, fs_bin.size);
	out->fs_size = fs_bin.size;
	out->vs_rsd = copy(vs_rsd.opaque, 64);
	out->fs_rsd = copy(fs_rsd.opaque, 64);
	out->fs_rsd_blended = copy(fs_rsd_blended.opaque, 64);
	out->fs_rsd_depth = copy(fs_rsd_depth.opaque, 64);
	out->blend_off = copy(off.opaque, 16);
	out->blend_over = copy(over.opaque, 16);
	out->blend_off_bgra = copy(off_bgra.opaque, 16);
	out->blend_over_bgra = copy(over_bgra.opaque, 16);
	out->vs_push = push_table(&vs);
	out->vs_push_count = vs.push.count;
	out->fs_push = push_table(&fs);
	out->fs_push_count = fs.push.count;
	out->vs_out = varying_table(vs.varyings.output, vs.varyings.output_count);
	out->vs_out_count = vs.varyings.output_count;
	out->fs_in = varying_table(fs.varyings.input, fs.varyings.input_count);
	out->fs_in_count = fs.varyings.input_count;
	out->attribute_formats = formats;
	out->attribute_sizes = sizes;
	out->attribute_count = nattr;
	out->texture_count = fs.texture_count;
	out->sampler_count = fs.sampler_count;
	out->can_blend = blend_over;
	out->vs_work_registers = vs.work_reg_count;
	out->fs_work_registers = fs.work_reg_count;
	util_dynarray_fini(&vs_bin);
	util_dynarray_fini(&fs_bin);
	ralloc_free(vs_nir);
	ralloc_free(fs_nir);
	return 0;
}

void
hm_program_free(struct hm_program *p)
{
	free((void *)p->vs_code); free((void *)p->fs_code); free((void *)p->vs_rsd); free((void *)p->fs_rsd);
	free((void *)p->fs_rsd_blended); free((void *)p->fs_rsd_depth); free((void *)p->blend_off);
	free((void *)p->blend_over); free((void *)p->blend_off_bgra); free((void *)p->blend_over_bgra);
	free((void *)p->vs_push); free((void *)p->fs_push); free((void *)p->vs_out);
	free((void *)p->fs_in); free((void *)p->attribute_formats); free((void *)p->attribute_sizes);
	memset(p, 0, sizeof(*p));
}

void
hm_compile_init(void)
{
	glsl_type_singleton_init_or_ref();
}

int
hm_compile_program(const char *name, unsigned gpu_id, struct hm_program *out, char *why, size_t why_size)
{
	if (strcmp(name, "tri") == 0) {
	{
		nir_builder v = shader(MESA_SHADER_VERTEX, "tri_vs");
		nir_variable *in = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_position", VERT_ATTRIB_GENERIC0, 0);
		nir_variable *pos = var(&v, nir_var_shader_out, glsl_vec4_type(), "gl_Position", VARYING_SLOT_POS, 0);
		v.shader->info.inputs_read = BITFIELD64_BIT(VERT_ATTRIB_GENERIC0);
		v.shader->info.outputs_written = BITFIELD64_BIT(VARYING_SLOT_POS);
		nir_store_var(&v, pos, nir_load_var(&v, in), 0xf);

		nir_builder f = shader(MESA_SHADER_FRAGMENT, "tri_fs");
		nir_variable *color = var(&f, nir_var_shader_out, glsl_vec4_type(), "color", FRAG_RESULT_DATA0, 0);
		f.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
		nir_store_var(&f, color, nir_imm_vec4(&f, 1.0f, 0.0f, 0.0f, 1.0f), 0xf);
		enum pipe_format attrs[] = { PIPE_FORMAT_R32G32B32A32_FLOAT };
		return build(name, gpu_id, v.shader, f.shader, attrs, 1, false, out, why, why_size);
	}
	}
	if (strcmp(name, "color") == 0) {
	/* a varying and a pushed uniform */
	{
		nir_builder v = shader(MESA_SHADER_VERTEX, "color_vs");
		nir_variable *in = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_position", VERT_ATTRIB_GENERIC0, 0);
		nir_variable *in_c = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_color", VERT_ATTRIB_GENERIC1, 1);
		nir_variable *pos = var(&v, nir_var_shader_out, glsl_vec4_type(), "gl_Position", VARYING_SLOT_POS, 0);
		nir_variable *col = var(&v, nir_var_shader_out, glsl_vec4_type(), "v_color", VARYING_SLOT_VAR0, 1);
		v.shader->info.inputs_read = BITFIELD64_BIT(VERT_ATTRIB_GENERIC0) | BITFIELD64_BIT(VERT_ATTRIB_GENERIC1);
		v.shader->info.outputs_written = BITFIELD64_BIT(VARYING_SLOT_POS) | BITFIELD64_BIT(VARYING_SLOT_VAR0);
		nir_store_var(&v, pos, nir_load_var(&v, in), 0xf);
		nir_store_var(&v, col, nir_load_var(&v, in_c), 0xf);

		nir_builder f = shader(MESA_SHADER_FRAGMENT, "color_fs");
		nir_variable *vc = var(&f, nir_var_shader_in, glsl_vec4_type(), "v_color", VARYING_SLOT_VAR0, 0);
		nir_variable *color = var(&f, nir_var_shader_out, glsl_vec4_type(), "color", FRAG_RESULT_DATA0, 0);
		vc->data.interpolation = INTERP_MODE_SMOOTH;
		f.shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_VAR0);
		f.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
		f.shader->info.num_ubos = 1;
		f.shader->info.first_ubo_is_default_ubo = true;
		nir_def *tint = nir_load_ubo(&f, 4, 32, nir_imm_int(&f, 0), nir_imm_int(&f, 0), .align_mul = 16,
		    .align_offset = 0, .range_base = 0, .range = 16);
		nir_store_var(&f, color, nir_fmul(&f, nir_load_var(&f, vc), tint), 0xf);
		enum pipe_format attrs[] = { PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R32G32B32A32_FLOAT };
		return build(name, gpu_id, v.shader, f.shader, attrs, 2, true, out, why, why_size);
	}
	}
	if (strcmp(name, "inst") == 0) {
	/* instancing: a position per vertex, an offset and a colour per instance (attributes 1 and 2 stepped by instance:
	 * the driver's attribute buffers, not the shader, make them so), the colour times the pushed tint */
	{
		nir_builder v = shader(MESA_SHADER_VERTEX, "inst_vs");
		nir_variable *in = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_position", VERT_ATTRIB_GENERIC0, 0);
		nir_variable *in_o = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_offset", VERT_ATTRIB_GENERIC1, 1);
		nir_variable *in_c = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_color", VERT_ATTRIB_GENERIC2, 2);
		nir_variable *pos = var(&v, nir_var_shader_out, glsl_vec4_type(), "gl_Position", VARYING_SLOT_POS, 0);
		nir_variable *col = var(&v, nir_var_shader_out, glsl_vec4_type(), "v_color", VARYING_SLOT_VAR0, 1);
		v.shader->info.inputs_read = BITFIELD64_BIT(VERT_ATTRIB_GENERIC0) | BITFIELD64_BIT(VERT_ATTRIB_GENERIC1) |
		    BITFIELD64_BIT(VERT_ATTRIB_GENERIC2);
		v.shader->info.outputs_written = BITFIELD64_BIT(VARYING_SLOT_POS) | BITFIELD64_BIT(VARYING_SLOT_VAR0);
		nir_def *o = nir_load_var(&v, in_o);
		nir_def *shift = nir_vec4(&v, nir_channel(&v, o, 0), nir_channel(&v, o, 1), nir_imm_float(&v, 0.0f),
		    nir_imm_float(&v, 0.0f));
		nir_store_var(&v, pos, nir_fadd(&v, nir_load_var(&v, in), shift), 0xf);
		nir_store_var(&v, col, nir_load_var(&v, in_c), 0xf);

		nir_builder f = shader(MESA_SHADER_FRAGMENT, "inst_fs");
		nir_variable *vc = var(&f, nir_var_shader_in, glsl_vec4_type(), "v_color", VARYING_SLOT_VAR0, 0);
		nir_variable *color = var(&f, nir_var_shader_out, glsl_vec4_type(), "color", FRAG_RESULT_DATA0, 0);
		vc->data.interpolation = INTERP_MODE_SMOOTH;
		f.shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_VAR0);
		f.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
		nir_store_var(&f, color, nir_load_var(&f, vc), 0xf);
		enum pipe_format attrs[] = { PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R32G32B32A32_FLOAT,
		    PIPE_FORMAT_R32G32B32A32_FLOAT };
		return build(name, gpu_id, v.shader, f.shader, attrs, 3, false, out, why, why_size);
	}
	}
	if (strcmp(name, "tex") == 0) {
	/* a texture sampled at a varying */
	{
		nir_builder v = shader(MESA_SHADER_VERTEX, "tex_vs");
		nir_variable *in = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_position", VERT_ATTRIB_GENERIC0, 0);
		nir_variable *in_uv = var(&v, nir_var_shader_in, glsl_vec4_type(), "a_uv", VERT_ATTRIB_GENERIC1, 1);
		nir_variable *pos = var(&v, nir_var_shader_out, glsl_vec4_type(), "gl_Position", VARYING_SLOT_POS, 0);
		nir_variable *uv = var(&v, nir_var_shader_out, glsl_vec_type(2), "v_uv", VARYING_SLOT_VAR0, 1);
		v.shader->info.inputs_read = BITFIELD64_BIT(VERT_ATTRIB_GENERIC0) | BITFIELD64_BIT(VERT_ATTRIB_GENERIC1);
		v.shader->info.outputs_written = BITFIELD64_BIT(VARYING_SLOT_POS) | BITFIELD64_BIT(VARYING_SLOT_VAR0);
		nir_store_var(&v, pos, nir_load_var(&v, in), 0xf);
		nir_store_var(&v, uv, nir_trim_vector(&v, nir_load_var(&v, in_uv), 2), 0x3);

		nir_builder f = shader(MESA_SHADER_FRAGMENT, "tex_fs");
		nir_variable *vuv = var(&f, nir_var_shader_in, glsl_vec_type(2), "v_uv", VARYING_SLOT_VAR0, 0);
		nir_variable *color = var(&f, nir_var_shader_out, glsl_vec4_type(), "color", FRAG_RESULT_DATA0, 0);
		vuv->data.interpolation = INTERP_MODE_SMOOTH;
		f.shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_VAR0);
		f.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
		f.shader->info.num_textures = 1;
		BITSET_SET(f.shader->info.textures_used, 0);
		BITSET_SET(f.shader->info.samplers_used, 0);
		/* texture(t0, v_uv) as the state tracker leaves it: a tex instruction naming texture 0 and sampler 0 */
		nir_tex_instr *tex = nir_tex_instr_create(f.shader, 1);
		tex->op = nir_texop_tex;
		tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
		tex->dest_type = nir_type_float32;
		tex->coord_components = 2;
		tex->texture_index = 0;
		tex->sampler_index = 0;
		tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_load_var(&f, vuv));
		nir_def_init(&tex->instr, &tex->def, 4, 32);
		nir_builder_instr_insert(&f, &tex->instr);
		nir_store_var(&f, color, &tex->def, 0xf);
		enum pipe_format attrs[] = { PIPE_FORMAT_R32G32B32A32_FLOAT, PIPE_FORMAT_R32G32B32A32_FLOAT };
		return build(name, gpu_id, v.shader, f.shader, attrs, 2, false, out, why, why_size);
	}
	}
	snprintf(why, why_size, "no program %s", name);
	return -1;
}

/* share: threadgroup memory and a barrier -- 64 threads a workgroup, each writes s[lid] = lid * 2 + 1, all wait, each
 * reads s[63 - lid] and writes out[gid] = that + 1000 * the workgroup's x; the buffer's address pushed from uniform
 * buffer 0 (words 0 and 1) */
static int
share_kernel(unsigned gpu_id, struct hm_kernel *out)
{
	nir_builder c = shader(MESA_SHADER_COMPUTE, "share_cs");
	c.shader->info.workgroup_size[0] = 64;
	c.shader->info.workgroup_size[1] = 1;
	c.shader->info.workgroup_size[2] = 1;
	c.shader->info.num_ubos = 1;
	c.shader->info.first_ubo_is_default_ubo = true;
	c.shader->info.shared_size = 256;
	nir_def *lid = nir_channel(&c, nir_load_local_invocation_id(&c), 0);
	nir_def *wg = nir_channel(&c, nir_load_workgroup_id(&c), 0);
	nir_def *gid = nir_iadd(&c, nir_imul_imm(&c, wg, 64), lid);
	nir_store_shared(&c, nir_iadd_imm(&c, nir_imul_imm(&c, lid, 2), 1), nir_imul_imm(&c, lid, 4), .base = 0,
	    .align_mul = 4);
	nir_barrier(&c, .execution_scope = SCOPE_WORKGROUP, .memory_scope = SCOPE_WORKGROUP,
	    .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_mem_shared);
	nir_def *other = nir_load_shared(&c, 1, 32, nir_imul_imm(&c, nir_isub(&c, nir_imm_int(&c, 63), lid), 4), .base = 0,
	    .align_mul = 4);
	nir_def *ubo = nir_load_ubo(&c, 2, 32, nir_imm_int(&c, 0), nir_imm_int(&c, 0), .align_mul = 16, .align_offset = 0,
	    .range_base = 0, .range = 8);
	nir_def *addr = nir_pack_64_2x32(&c, ubo);
	nir_store_global(&c, nir_iadd(&c, addr, nir_u2u64(&c, nir_imul_imm(&c, gid, 4))), 4,
	    nir_iadd(&c, other, nir_imul_imm(&c, wg, 1000)), 0x1);
	NIR_PASS(_, c.shader, nir_lower_compute_system_values, NULL);

	struct util_dynarray bin;
	struct pan_shader_info cs;
	compile(c.shader, gpu_id, &bin, &cs);
	struct mali_renderer_state_packed rsd;
	pan_pack(&rsd, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(&cs, 0, &cfg);
	}
	memset(out, 0, sizeof(*out));
	out->name = "share";
	out->code = copy(bin.data, bin.size);
	out->size = bin.size;
	out->rsd = copy(rsd.opaque, 64);
	out->push = push_table(&cs);
	out->push_count = cs.push.count;
	out->local[0] = 64, out->local[1] = 1, out->local[2] = 1;
	out->tls_size = cs.tls_size;
	out->work_registers = cs.work_reg_count;
	out->ubo_count = cs.ubo_count;
	out->wls_size = cs.wls_size;
	util_dynarray_fini(&bin);
	ralloc_free(c.shader);
	return 0;
}

/* copy: a blit of words, out[gid] = in[gid] for gid < count; uniform buffer 0: the source's address (words 0, 1), the
 * destination's (2, 3), the count (4); 64 threads a workgroup (MTLBlitCommandEncoder's copyFromBuffer, as a kernel) */
static int
copy_kernel(unsigned gpu_id, struct hm_kernel *out)
{
	nir_builder c = shader(MESA_SHADER_COMPUTE, "copy_cs");
	c.shader->info.workgroup_size[0] = 64;
	c.shader->info.workgroup_size[1] = 1;
	c.shader->info.workgroup_size[2] = 1;
	c.shader->info.num_ubos = 1;
	c.shader->info.first_ubo_is_default_ubo = true;
	nir_def *gid = nir_channel(&c, nir_load_global_invocation_id(&c, 32), 0);
	nir_def *u = nir_load_ubo(&c, 4, 32, nir_imm_int(&c, 0), nir_imm_int(&c, 0), .align_mul = 16, .align_offset = 0,
	    .range_base = 0, .range = 16);
	nir_def *count = nir_load_ubo(&c, 1, 32, nir_imm_int(&c, 0), nir_imm_int(&c, 16), .align_mul = 16,
	    .align_offset = 0, .range_base = 16, .range = 4);
	nir_push_if(&c, nir_ult(&c, gid, count));
	{
		nir_def *off = nir_u2u64(&c, nir_imul_imm(&c, gid, 4));
		nir_def *src = nir_iadd(&c, nir_pack_64_2x32_split(&c, nir_channel(&c, u, 0), nir_channel(&c, u, 1)), off);
		nir_def *dst = nir_iadd(&c, nir_pack_64_2x32_split(&c, nir_channel(&c, u, 2), nir_channel(&c, u, 3)), off);
		nir_store_global(&c, dst, 4, nir_load_global(&c, src, 4, 1, 32), 0x1);
	}
	nir_pop_if(&c, NULL);
	NIR_PASS(_, c.shader, nir_lower_compute_system_values, NULL);

	struct util_dynarray bin;
	struct pan_shader_info cs;
	compile(c.shader, gpu_id, &bin, &cs);
	struct mali_renderer_state_packed rsd;
	pan_pack(&rsd, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(&cs, 0, &cfg);
	}
	memset(out, 0, sizeof(*out));
	out->name = "copy";
	out->code = copy(bin.data, bin.size);
	out->size = bin.size;
	out->rsd = copy(rsd.opaque, 64);
	out->push = push_table(&cs);
	out->push_count = cs.push.count;
	out->local[0] = 64, out->local[1] = 1, out->local[2] = 1;
	out->tls_size = cs.tls_size;
	out->work_registers = cs.work_reg_count;
	out->ubo_count = cs.ubo_count;
	out->wls_size = cs.wls_size;
	util_dynarray_fini(&bin);
	ralloc_free(c.shader);
	return 0;
}

/* copy2d: a rectangle of 32-bit texels, dst[y * dst_stride + x] = src[y * src_stride + x] for x < width, y < height;
 * uniform buffer 0: the source's address (words 0, 1), the destination's (2, 3), width, height, source and destination
 * row strides in words (4..7); 64x1 threads a workgroup, the grid (width / 64 rounded up, height) -- a texture's region
 * copied as MTLBlitCommandEncoder's copyFromTexture does it, with no render pass to load and store */
static int
copy2d_kernel(unsigned gpu_id, struct hm_kernel *out)
{
	nir_builder c = shader(MESA_SHADER_COMPUTE, "copy2d_cs");
	c.shader->info.workgroup_size[0] = 64;
	c.shader->info.workgroup_size[1] = 1;
	c.shader->info.workgroup_size[2] = 1;
	c.shader->info.num_ubos = 1;
	c.shader->info.first_ubo_is_default_ubo = true;
	nir_def *gid = nir_load_global_invocation_id(&c, 32);
	nir_def *x = nir_channel(&c, gid, 0), *y = nir_channel(&c, gid, 1);
	nir_def *a = nir_load_ubo(&c, 4, 32, nir_imm_int(&c, 0), nir_imm_int(&c, 0), .align_mul = 16, .align_offset = 0,
	    .range_base = 0, .range = 32);
	nir_def *g = nir_load_ubo(&c, 4, 32, nir_imm_int(&c, 0), nir_imm_int(&c, 16), .align_mul = 16, .align_offset = 0,
	    .range_base = 0, .range = 32);
	nir_push_if(&c, nir_iand(&c, nir_ult(&c, x, nir_channel(&c, g, 0)), nir_ult(&c, y, nir_channel(&c, g, 1))));
	{
		nir_def *so = nir_u2u64(&c, nir_imul_imm(&c, nir_iadd(&c, nir_imul(&c, y, nir_channel(&c, g, 2)), x), 4));
		nir_def *dof = nir_u2u64(&c, nir_imul_imm(&c, nir_iadd(&c, nir_imul(&c, y, nir_channel(&c, g, 3)), x), 4));
		nir_def *src = nir_iadd(&c, nir_pack_64_2x32_split(&c, nir_channel(&c, a, 0), nir_channel(&c, a, 1)), so);
		nir_def *dst = nir_iadd(&c, nir_pack_64_2x32_split(&c, nir_channel(&c, a, 2), nir_channel(&c, a, 3)), dof);
		nir_store_global(&c, dst, 4, nir_load_global(&c, src, 4, 1, 32), 0x1);
	}
	nir_pop_if(&c, NULL);
	NIR_PASS(_, c.shader, nir_lower_compute_system_values, NULL);

	struct util_dynarray bin;
	struct pan_shader_info cs;
	compile(c.shader, gpu_id, &bin, &cs);
	struct mali_renderer_state_packed rsd;
	pan_pack(&rsd, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(&cs, 0, &cfg);
	}
	memset(out, 0, sizeof(*out));
	out->name = "copy2d";
	out->code = copy(bin.data, bin.size);
	out->size = bin.size;
	out->rsd = copy(rsd.opaque, 64);
	out->push = push_table(&cs);
	out->push_count = cs.push.count;
	out->local[0] = 64, out->local[1] = 1, out->local[2] = 1;
	out->tls_size = cs.tls_size;
	out->work_registers = cs.work_reg_count;
	out->ubo_count = cs.ubo_count;
	out->wls_size = cs.wls_size;
	util_dynarray_fini(&bin);
	ralloc_free(c.shader);
	return 0;
}

int
hm_compile_kernel(const char *name, unsigned gpu_id, struct hm_kernel *out, char *why, size_t why_size)
{
	if (strcmp(name, "copy2d") == 0) {
		return copy2d_kernel(gpu_id, out);
	}
	if (strcmp(name, "share") == 0) {
		return share_kernel(gpu_id, out);
	}
	if (strcmp(name, "copy") == 0) {
		return copy_kernel(gpu_id, out);
	}
	if (strcmp(name, "fill") != 0) {
		snprintf(why, why_size, "no kernel %s", name);
		return -1;
	}
	/* fill: a compute kernel, out[gid] = gid * 3 + k, the buffer's address and k pushed from uniform buffer 0 (words 0
	 * and 1 the address, word 2 k), 16 invocations a workgroup; the system values lowered as the state tracker does
	 * (nir_lower_compute_system_values), so that the global id is the workgroup's and the local one, preloaded */
	{
		nir_builder c = shader(MESA_SHADER_COMPUTE, "fill_cs");
		c.shader->info.workgroup_size[0] = 16;
		c.shader->info.workgroup_size[1] = 1;
		c.shader->info.workgroup_size[2] = 1;
		c.shader->info.num_ubos = 1;
		c.shader->info.first_ubo_is_default_ubo = true;
		nir_def *gid = nir_channel(&c, nir_load_global_invocation_id(&c, 32), 0);
		nir_def *ubo = nir_load_ubo(&c, 3, 32, nir_imm_int(&c, 0), nir_imm_int(&c, 0), .align_mul = 16, .align_offset = 0,
		    .range_base = 0, .range = 12);
		nir_def *addr = nir_pack_64_2x32_split(&c, nir_channel(&c, ubo, 0), nir_channel(&c, ubo, 1));
		nir_def *value = nir_iadd(&c, nir_imul_imm(&c, gid, 3), nir_channel(&c, ubo, 2));
		nir_store_global(&c, nir_iadd(&c, addr, nir_u2u64(&c, nir_imul_imm(&c, gid, 4))), 4, value, 0x1);
		NIR_PASS(_, c.shader, nir_lower_compute_system_values, NULL);

		struct util_dynarray bin;
		struct pan_shader_info cs;
		compile(c.shader, gpu_id, &bin, &cs);
		struct mali_renderer_state_packed rsd;
		pan_pack(&rsd, RENDERER_STATE, cfg) {
			pan_shader_prepare_rsd(&cs, 0, &cfg);
		}
		memset(out, 0, sizeof(*out));
		out->name = "fill";
		out->code = copy(bin.data, bin.size);
		out->size = bin.size;
		out->rsd = copy(rsd.opaque, 64);
		out->push = push_table(&cs);
		out->push_count = cs.push.count;
		out->local[0] = 16, out->local[1] = 1, out->local[2] = 1;
		out->tls_size = cs.tls_size;
		out->work_registers = cs.work_reg_count;
		out->ubo_count = cs.ubo_count;
		out->wls_size = cs.wls_size;
		util_dynarray_fini(&bin);
		ralloc_free(c.shader);
	}
	return 0;
}

void
hm_kernel_free(struct hm_kernel *k)
{
	free((void *)k->code); free((void *)k->rsd); free((void *)k->push);
	memset(k, 0, sizeof(*k));
}

uint32_t
hm_compile_texture_format_rgba8(void)
{
	return GENX(panfrost_format_from_pipe_format)(PIPE_FORMAT_R8G8B8A8_UNORM)->hw;
}

int
hm_compile_format(const char *name, struct hm_format *out)
{
	static const struct { const char *name; enum pipe_format f; } formats[] = {
		{ "RGBA8Unorm", PIPE_FORMAT_R8G8B8A8_UNORM }, { "BGRA8Unorm", PIPE_FORMAT_B8G8R8A8_UNORM },
		{ "R8Unorm", PIPE_FORMAT_R8_UNORM }, { "RG8Unorm", PIPE_FORMAT_R8G8_UNORM },
	};
	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (strcmp(name, formats[i].name) != 0) {
			continue;
		}
		enum pipe_format f = formats[i].f;
		const struct util_format_description *desc = util_format_description(f);
		const struct pan_blendable_format *b = GENX(panfrost_blendable_format_from_pipe_format)(f);
		unsigned char swizzle[4];

		memset(out, 0, sizeof(*out));
		out->name = formats[i].name;
		/* pan_prepare_rt */
		out->rt_internal = b->internal;
		out->rt_writeback = b->writeback;
		panfrost_invert_swizzle(desc->swizzle, swizzle);
		out->rt_swizzle = panfrost_translate_swizzle_4(swizzle);
		/* panfrost_emit_blend: dithered, and RGB1 made RGBA for the fixed function */
		out->blend_memory_format = GENX(panfrost_dithered_format_from_pipe_format)(f, true);
		if ((out->blend_memory_format & 0xff) == MALI_RGB_COMPONENT_ORDER_RGB1) {
			out->blend_memory_format = (out->blend_memory_format & ~0xffu) | MALI_RGB_COMPONENT_ORDER_RGBA;
		}
		out->texture_format = GENX(panfrost_format_from_pipe_format)(f)->hw;
		return 0;
	}
	return -1;
}

uint32_t
hm_compile_swizzle_identity(void)
{
	const unsigned char identity[4] = { PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W };

	return panfrost_translate_swizzle_4(identity);
}

/* --- Apple's kernels ------------------------------------------------------------------------------------------- */

/* a device buffer access (load_ssbo, store_ssbo, ssbo_atomic, ssbo_atomic_swap with an immediate buffer index, which is
 * how the AIR frontend writes them) as the same access to global memory at the buffer's address, read out of the
 * system value buffer, plus the offset */
static bool
ssbo_to_global(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
	uint32_t *used = data;
	int index_src, offset_src;

	switch (intr->intrinsic) {
	case nir_intrinsic_load_ssbo:
		index_src = 0, offset_src = 1;
		break;
	case nir_intrinsic_store_ssbo:
		index_src = 1, offset_src = 2;
		break;
	case nir_intrinsic_ssbo_atomic:
	case nir_intrinsic_ssbo_atomic_swap:
		index_src = 0, offset_src = 1;
		break;
	default:
		return false;
	}
	if (!nir_src_is_const(intr->src[index_src])) {
		return false;                   /* left alone: the compiler refuses it by name */
	}
	unsigned index = nir_src_as_uint(intr->src[index_src]);
	*used |= 1u << index;
	b->cursor = nir_before_instr(&intr->instr);
	nir_def *words = nir_load_ubo(b, 2, 32, nir_imm_int(b, HM_AIR_SYSVAL_UBO), nir_imm_int(b, 8 * index),
	    .align_mul = 8, .align_offset = 0, .range_base = 8 * index, .range = 8);
	nir_def *addr = nir_iadd(b, nir_pack_64_2x32(b, words), nir_u2u64(b, intr->src[offset_src].ssa));
	unsigned align = (intr->intrinsic == nir_intrinsic_load_ssbo || intr->intrinsic == nir_intrinsic_store_ssbo)
	    ? nir_intrinsic_align(intr) : 4;

	switch (intr->intrinsic) {
	case nir_intrinsic_load_ssbo: {
		nir_def *v = nir_build_load_global(b, intr->def.num_components, intr->def.bit_size, addr, .align_mul = align,
		    .access = nir_intrinsic_access(intr));
		nir_def_replace(&intr->def, v);
		break;
	}
	case nir_intrinsic_store_ssbo:
		nir_build_store_global(b, intr->src[0].ssa, addr, .align_mul = align, .write_mask = nir_intrinsic_write_mask(intr),
		    .access = nir_intrinsic_access(intr));
		nir_instr_remove(&intr->instr);
		break;
	case nir_intrinsic_ssbo_atomic: {
		nir_def *v = nir_global_atomic(b, intr->def.bit_size, addr, intr->src[2].ssa,
		    .atomic_op = nir_intrinsic_atomic_op(intr));
		nir_def_replace(&intr->def, v);
		break;
	}
	case nir_intrinsic_ssbo_atomic_swap: {
		nir_def *v = nir_global_atomic_swap(b, intr->def.bit_size, addr, intr->src[2].ssa, intr->src[3].ssa,
		    .atomic_op = nir_intrinsic_atomic_op(intr));
		nir_def_replace(&intr->def, v);
		break;
	}
	default:
		break;
	}
	return true;
}

/* the dispatch's sizes as system values in the same buffer, as Panfrost pushes PAN_SYSVAL_NUM_WORK_GROUPS and
 * PAN_SYSVAL_LOCAL_GROUP_SIZE: the number of threadgroups at byte HM_AIR_SYSVAL_GRID, the threadgroup's size at
 * HM_AIR_SYSVAL_LOCAL (a Metal kernel's size is the dispatch's) */
static bool
grid_sysvals(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
	unsigned offset;

	if (intr->intrinsic == nir_intrinsic_load_num_workgroups) {
		offset = HM_AIR_SYSVAL_GRID;
	} else if (intr->intrinsic == nir_intrinsic_load_workgroup_size) {
		offset = HM_AIR_SYSVAL_LOCAL;
	} else {
		return false;
	}
	b->cursor = nir_before_instr(&intr->instr);
	nir_def *v = nir_load_ubo(b, 3, 32, nir_imm_int(b, HM_AIR_SYSVAL_UBO), nir_imm_int(b, offset), .align_mul = 16,
	    .align_offset = 0, .range_base = offset, .range = 12);
	nir_def_replace(&intr->def, nir_u2uN(b, v, intr->def.bit_size));
	return true;
}

/* [[vertex_id]] of a draw without indices: the raw vertex ID, the draw's offset start being 0 (pan_lower_vertex_id.c:
 * vertex_id = raw_vertex_id_pan + raw_vertex_offset_pan) */
static bool
raw_vertex_id(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
	if (intr->intrinsic != nir_intrinsic_load_vertex_id && intr->intrinsic != nir_intrinsic_load_vertex_id_zero_base) {
		return false;
	}
	b->cursor = nir_before_instr(&intr->instr);
	nir_def_replace(&intr->def, nir_load_raw_vertex_id_pan(b));
	return true;
}

static nir_shader *
air_function(void *mem, const struct air_metallib *ml, const char *entry, struct air_shader_info *info, char *why,
    size_t why_size)
{
	const struct air_metallib_function *f = NULL;
	const char *err = NULL;

	for (unsigned i = 0; i < ml->num_functions; i++) {
		if (strcmp(ml->functions[i].name, entry) == 0) f = &ml->functions[i];
	}
	if (f == NULL) {
		snprintf(why, why_size, "no function %s in the .metallib", entry);
		return NULL;
	}
	nir_shader *s = air_to_nir(mem, f->bitcode, f->bitcode_size, NULL, NULL, GENX(pan_shader_get_compiler_options)(),
	    info, &err);
	if (s == NULL) {
		snprintf(why, why_size, "%s: AIR to NIR: %s", entry, err ? err : "refused");
	}
	return s;
}

int
hm_compile_air_program(const void *lib, size_t size, const char *vs_entry, const char *fs_entry, unsigned gpu_id,
    struct hm_program *out, uint32_t *vs_device_buffers, bool *fs_linear, char *why, size_t why_size)
{
	void *mem = ralloc_context(NULL);
	struct air_metallib ml;
	const char *err = NULL;
	struct air_shader_info vi, fi;

	if (!air_metallib_parse(mem, lib, size, &ml, &err)) {
		snprintf(why, why_size, "the .metallib: %s", err ? err : "not one");
		ralloc_free(mem);
		return -1;
	}
	nir_shader *vs = air_function(mem, &ml, vs_entry, &vi, why, why_size);
	nir_shader *fs = vs ? air_function(mem, &ml, fs_entry, &fi, why, why_size) : NULL;
	if (fs == NULL) {
		ralloc_free(mem);
		return -1;
	}
	const char *no = NULL;
	if (vs->info.stage != MESA_SHADER_VERTEX || fs->info.stage != MESA_SHADER_FRAGMENT) {
		no = "not a vertex function and a fragment function";
	} else if (vi.num_vertex_attributes != 0) {
		no = "a vertex function with [[stage_in]] attributes (not served yet)";
	} else if (fi.num_render_targets != 1) {
		no = "a fragment function that does not write one colour target";
	}
	*fs_linear = false;
	for (unsigned i = 0; no == NULL && i < vi.num_resources; i++) {
		const struct air_resource *r = &vi.resources[i];
		if (r->kind == AIR_RESOURCE_CONSTANT_BUFFER && !r->unused) {
			no = "a vertex function with a constant buffer (not served yet)";
		} else if (r->kind == AIR_RESOURCE_DEVICE_BUFFER && r->write) {
			no = "a vertex function that writes a buffer";
		} else if (r->kind != AIR_RESOURCE_CONSTANT_BUFFER && r->kind != AIR_RESOURCE_DEVICE_BUFFER) {
			no = "a vertex function with a texture, image or sampler (not served yet)";
		}
	}
	for (unsigned i = 0; no == NULL && i < fi.num_resources; i++) {
		const struct air_resource *r = &fi.resources[i];
		if ((r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_SAMPLER) && r->index != 0) {
			no = "a fragment function with a texture or sampler other than 0 (not served yet)";
		} else if (r->kind == AIR_RESOURCE_SAMPLER && r->inline_sampler) {
			*fs_linear = r->sampler.min_filter != AIR_SAMPLER_NEAREST;
		} else if (r->kind != AIR_RESOURCE_TEXTURE && r->kind != AIR_RESOURCE_SAMPLER && !r->unused) {
			no = "a fragment function with a buffer (not served yet)";
		}
	}
	if (no != NULL) {
		snprintf(why, why_size, "%s, %s: %s", vs_entry, fs_entry, no);
		ralloc_free(mem);
		return -1;
	}
	*vs_device_buffers = 0;
	NIR_PASS(_, vs, nir_shader_intrinsics_pass, ssbo_to_global, nir_metadata_control_flow, vs_device_buffers);
	NIR_PASS(_, vs, nir_shader_intrinsics_pass, raw_vertex_id, nir_metadata_control_flow, NULL);
	if (vs->info.num_ubos < HM_AIR_SYSVAL_UBO + 1) {
		vs->info.num_ubos = HM_AIR_SYSVAL_UBO + 1;
	}
	char *name = malloc(strlen(vs_entry) + strlen(fs_entry) + 2);
	sprintf(name, "%s+%s", vs_entry, fs_entry);
	int e = build(name, gpu_id, vs, fs, NULL, 0, true, out, why, why_size);
	ralloc_free(mem);
	return e;
}

int
hm_air_describe(const void *lib, size_t size, const char *entry, void *fp, char *why, size_t why_size)
{
	FILE *out = fp;
	void *mem = ralloc_context(NULL);
	struct air_metallib ml;
	const char *err = NULL;
	const struct air_metallib_function *f = NULL;

	if (!air_metallib_parse(mem, lib, size, &ml, &err)) {
		snprintf(why, why_size, "the .metallib: %s", err ? err : "not one");
		ralloc_free(mem);
		return -1;
	}
	for (unsigned i = 0; i < ml.num_functions; i++) {
		if (strcmp(ml.functions[i].name, entry) == 0) f = &ml.functions[i];
	}
	if (f == NULL) {
		snprintf(why, why_size, "no function %s", entry);
		ralloc_free(mem);
		return -1;
	}
	struct air_shader_info info;
	nir_shader *s = air_to_nir(mem, f->bitcode, f->bitcode_size, NULL, NULL, GENX(pan_shader_get_compiler_options)(),
	    &info, &err);
	if (s == NULL) {
		snprintf(why, why_size, "%s: AIR to NIR: %s", entry, err ? err : "refused");
		ralloc_free(mem);
		return -1;
	}
	fprintf(out, "%s: stage %s, %u resources, %u varyings, %u vertex attributes (read %#x), %u render targets\n", entry,
	    gl_shader_stage_name(s->info.stage), info.num_resources, info.num_varyings, info.num_vertex_attributes,
	    info.vertex_attributes_read, info.num_render_targets);
	for (unsigned i = 0; i < info.num_resources; i++) {
		const struct air_resource *r = &info.resources[i];
		fprintf(out, "  resource kind %d index %u size %u%s%s%s %s %s\n", r->kind, r->index, r->size,
		    r->unused ? " unused" : "", r->read ? " read" : "", r->write ? " write" : "", r->name ? r->name : "",
		    r->type_name ? r->type_name : "");
	}
	for (unsigned i = 0; i < info.num_varyings; i++) {
		const struct air_varying *v = &info.varyings[i];
		fprintf(out, "  varying %s slot %u components %u%s%s\n", v->name, v->slot, v->components, v->flat ? " flat" : "",
		    v->no_perspective ? " noperspective" : "");
	}
	nir_print_shader(s, out);
	ralloc_free(mem);
	return 0;
}

int
hm_air_kernels(const void *lib, size_t size, char *names, size_t names_size)
{
	void *mem = ralloc_context(NULL);
	struct air_metallib ml;
	const char *err = NULL;
	int count = 0;
	size_t at = 0;

	if (!air_metallib_parse(mem, lib, size, &ml, &err)) {
		ralloc_free(mem);
		return -1;
	}
	for (unsigned i = 0; i < ml.num_functions; i++) {
		const char *calls = NULL;
		int stage = air_module_stage(mem, ml.functions[i].bitcode, ml.functions[i].bitcode_size, &calls, &err);
		size_t n = strlen(ml.functions[i].name) + 1;

		if (stage == MESA_SHADER_COMPUTE && at + n <= names_size) {
			memcpy(names + at, ml.functions[i].name, n);
			at += n;
			count++;
		}
	}
	ralloc_free(mem);
	return count;
}

int
hm_compile_air_kernel(const void *lib, size_t size, const char *entry, unsigned gpu_id, struct hm_kernel *out,
    uint32_t *device_buffers, uint32_t *constant_buffers, char *why, size_t why_size)
{
	void *mem = ralloc_context(NULL);
	struct air_metallib ml;
	const char *err = NULL;
	const struct air_metallib_function *f = NULL;

	if (!air_metallib_parse(mem, lib, size, &ml, &err)) {
		snprintf(why, why_size, "the .metallib: %s", err ? err : "not one");
		ralloc_free(mem);
		return -1;
	}
	for (unsigned i = 0; i < ml.num_functions; i++) {
		if (strcmp(ml.functions[i].name, entry) == 0) f = &ml.functions[i];
	}
	if (f == NULL) {
		snprintf(why, why_size, "no function %s in the .metallib", entry);
		ralloc_free(mem);
		return -1;
	}
	struct air_shader_info info;
	nir_shader *s = air_to_nir(mem, f->bitcode, f->bitcode_size, NULL, NULL, GENX(pan_shader_get_compiler_options)(),
	    &info, &err);
	if (s == NULL) {
		snprintf(why, why_size, "%s: AIR to NIR: %s", entry, err ? err : "refused");
		ralloc_free(mem);
		return -1;
	}
	if (s->info.stage != MESA_SHADER_COMPUTE) {
		snprintf(why, why_size, "%s is not a kernel", entry);
		ralloc_free(mem);
		return -1;
	}
	*constant_buffers = 0;
	for (unsigned i = 0; i < info.num_resources; i++) {
		const struct air_resource *r = &info.resources[i];

		if (r->kind == AIR_RESOURCE_CONSTANT_BUFFER && r->index < HM_AIR_SYSVAL_UBO) {
			*constant_buffers |= 1u << r->index;
		} else if (r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_IMAGE || r->kind == AIR_RESOURCE_SAMPLER ||
		    (r->kind == AIR_RESOURCE_CONSTANT_BUFFER && !r->unused)) {
			if (r->kind != AIR_RESOURCE_CONSTANT_BUFFER) {
				snprintf(why, why_size, "%s binds a texture, image or sampler (not served yet)", entry);
				ralloc_free(mem);
				return -1;
			}
		}
	}
	if (getenv("HM_PRINT_NIR") != NULL) {
		nir_print_shader(s, stderr);
	}
	*device_buffers = 0;
	NIR_PASS(_, s, nir_shader_intrinsics_pass, ssbo_to_global, nir_metadata_control_flow, device_buffers);
	if (s->info.num_ubos < HM_AIR_SYSVAL_UBO + 1) {
		s->info.num_ubos = HM_AIR_SYSVAL_UBO + 1;
	}
	NIR_PASS(_, s, nir_lower_compute_system_values, NULL);
	NIR_PASS(_, s, nir_shader_intrinsics_pass, grid_sysvals, nir_metadata_control_flow, NULL);

	struct util_dynarray bin;
	struct pan_shader_info cs;
	struct panfrost_compile_inputs inputs = { .gpu_id = gpu_id, .no_idvs = true, .pushable_ubos = ~0u };
	bifrost_preprocess_nir(s, gpu_id);
	util_dynarray_init(&bin, NULL);
	GENX(pan_shader_compile)(s, &inputs, &bin, &cs);
	if (bin.size == 0) {
		snprintf(why, why_size, "%s: the Bifrost compiler gave no code", entry);
		util_dynarray_fini(&bin);
		ralloc_free(mem);
		return -1;
	}
	struct mali_renderer_state_packed rsd;
	pan_pack(&rsd, RENDERER_STATE, cfg) {
		pan_shader_prepare_rsd(&cs, 0, &cfg);
	}
	memset(out, 0, sizeof(*out));
	out->name = strdup(entry);
	out->code = copy(bin.data, bin.size);
	out->size = bin.size;
	out->rsd = copy(rsd.opaque, 64);
	out->push = push_table(&cs);
	out->push_count = cs.push.count;
	out->local[0] = s->info.workgroup_size[0];
	out->local[1] = s->info.workgroup_size[1];
	out->local[2] = s->info.workgroup_size[2];
	out->tls_size = cs.tls_size;
	out->work_registers = cs.work_reg_count;
	out->ubo_count = cs.ubo_count;
	out->wls_size = cs.wls_size;
	util_dynarray_fini(&bin);
	ralloc_free(mem);
	return 0;
}
