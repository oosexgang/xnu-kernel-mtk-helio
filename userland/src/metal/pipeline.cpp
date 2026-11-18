/*
 * A render pipeline built from the shape the plug-in read out of Metal's descriptor (its colour attachments and their
 * blend states, the depth and stencil formats, the vertex descriptor) and the modules it found: what is asked for
 * said, CoreAnimation's in-shader blends given source-over, one the warm-up thread built taken, else the recipe
 * recorded and the modules compiled -- one build at a time, on a stack of 16 MB, with the functions' constants.
 */
#include "rwm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

namespace {

/*
 * boot-arg rwm_pipelog=1 (backboardd): each pipeline once, as "!!pl LABEL w.. f.. b.. fmt.. fc.." -- the colour outputs
 * its fragment shader stores to (w) and the targets it fetches (f), a bit per [[color(n)]], the attachments that blend
 * (b), their pixel formats, and the fragment constants 3 4 5 29 34 39 (layout, framebuffer fetch, attachment count,
 * blend function, destination index, unbounded range) where it was given them. Up to 400 lines.
 */
void
pipelog(const char *label, const struct rwm_render_request *r, const struct rwa_render_shape *shape,
	const struct rwa_render *render, const struct rwa_render_info *info)
{
	static int on = -1;
	static unsigned lines;
	static const unsigned wanted[] = { 3, 4, 5, 29, 34, 39 };
	const struct rwm_air *air = rwm_air();
	unsigned writes = 0, fetches = 0;
	char line[400];
	int n;

	if (on < 0) {
		on = strstr(rwm_boot_args(), "rwm_pipelog=1") != nullptr && strcmp(getprogname(), "backboardd") == 0;
	}
	if (on != 1 || lines >= 400 || label == nullptr) {
		return;
	}
	lines++;
	if (air->render_outputs != nullptr) {
		air->render_outputs(render, &writes, &fetches);
	}
	const char *name = strncmp(label, "com.apple.coreanimation.", 24) == 0 ? label + 24 : label;
	n = snprintf(line, sizeof(line), "!!pl %s w%x f%x b%x fmt", name, writes, fetches, info->blend_enabled_mask);
	for (unsigned i = 0; i < shape->color_count && i < 8 && n > 0 && (size_t)n < sizeof(line); i++) {
		n += snprintf(line + n, sizeof(line) - n, "%s%u", i ? "," : " ", shape->color_format[i]);
	}
	for (unsigned k = 0; k < sizeof(wanted) / sizeof(wanted[0]) && n > 0 && (size_t)n < sizeof(line); k++) {
		for (unsigned i = 0; i < r->fragment_constant_count; i++) {
			if (r->fragment_constants[i].index == wanted[k]) {
				n += snprintf(line + n, sizeof(line) - n, " %u=%#llx", wanted[k],
					      (unsigned long long)r->fragment_constants[i].bits);
				break;
			}
		}
	}
	sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
}

bool
coreanimation_draw(const char *label)
{
	/* 16.5's labels are com.apple.coreanimation.draw.*, the 27 release's com.apple.coreanimation.PBGRA... and the
	 * like: no pipeline of 27 was recorded as a recipe, so none was built ahead (16 November) */
	static const char prefix[] = "com.apple.coreanimation.";

	return label != nullptr && strncmp(label, prefix, sizeof(prefix) - 1) == 0;
}

struct compile_call {
	struct rwa_device *device;
	const struct rwm_render_request *request;
	struct rwa_render_shape *shape;
	struct rwa_render_info *info;
	char *why;
	size_t why_size;
	struct rwa_render *render;
};

/* (set on the thread that compiles: rwa_render_constants is per thread) */
void
compile_main(void *context)
{
	compile_call *c = (compile_call *)context;
	const struct rwm_render_request *r = c->request;
	const struct rwm_air *air = rwm_air();

	if (air->render_constants != nullptr) {
		air->render_constants(r->vertex_constants, r->vertex_constant_count, r->fragment_constants,
				      r->fragment_constant_count);
	}
	/* No entry names: a module out of a .metallib has one entry point, and the compiler takes it. Named, it insists
	 * the two agree, which refuses an alias -- a second function-list entry over another's module, which is how
	 * QuartzCore's library here answers for the few functions the system image 20F66 asks for and the macOS library it comes from
	 * has not got (p3_clamp, display_overdrive_*, path_stroke_*: userland/build.sh, QC_METALLIB). */
	c->render = air->render_create(c->device, r->vertex, r->vertex_size, nullptr, r->fragment, r->fragment_size,
				       nullptr, c->shape, c->info, c->why, c->why_size);
}

} // namespace

namespace {

/*
 * CoreAnimation 27's GENERIC pipelines: fixed_vert_lph_gen and fixed_frag_lph_cpf with fc_generic (constant 0) set,
 * labelled "...Xghfc". They read the layer's configuration while they run -- VertexShaderConfig (4 bytes) and
 * FragmentShaderConfig (52 bytes) in buffer 0 of each stage -- and CoreAnimation draws most of the 27 release's frame
 * with them (379 of 536 draws, run 89): its specialized pipelines come out of a binary archive made for Apple's GPUs.
 * Built whole the generic shader is 31588 Mali instructions and 138 s (run 64); built with fc_generic 0 it reads its
 * configuration from constants nobody gave, has no image and no blend, and the icons were not drawn (runs 68-91).
 *
 * So the pipeline CoreAnimation gets is built with fc_generic 0, as before, and kept here with what it was asked
 * for; at each draw its configuration is read out of buffer 0 of each stage and the pipeline drawn with is a VARIANT
 * built for that configuration -- fc_generic as asked and buffer 0 folded into the shader (rwa_render_config), so NIR
 * takes away every feature that configuration does not use: what Apple's own specialization would make. A variant
 * is built once per configuration (48 a pipeline at most; past that, the pipeline as it is). Boot-arg
 * rwm_specialize=0: never; rwm_generic=1: the pipeline itself built with fc_generic as asked.
 */
struct generic_variant {
	uint8_t vertex[8], fragment[64];
	struct rwa_render *render;      /* NULL: the build failed, said once */
	bool building;                  /* being built (by a draw or ahead, prebuild_main): wait for generic_cond */
};

struct generic_record {
	struct rwa_render *base;
	struct rwa_device *device;
	char label[96];
	void *vertex, *fragment;        /* copies of the modules */
	size_t vertex_size, fragment_size;
	struct rwa_constant vc[128], fc[128];
	unsigned nv, nf;
	struct rwa_render_shape shape;
	unsigned vertex_config, fragment_config;  /* bytes of buffer 0 read for each stage */
	struct generic_variant variants[192];   /* run 557: past 48, the generic pipeline itself, which draws nothing */
	unsigned nvariants;
	bool full_said;
};

pthread_mutex_t generic_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t generic_cond = PTHREAD_COND_INITIALIZER;   /* a variant's build ended */
void prebuild_start(struct generic_record *g);
/* the number of the variant the last rwm_generic_variant on this thread gave (its "rwm variant LABEL #N" line), 0: none */
thread_local unsigned last_variant;
struct generic_record *generics[64];
unsigned ngenerics;

bool
boot_arg(const char *what)
{
	return strstr(rwm_boot_args(), what) != nullptr;
}

/* the uber shaders' fc_generic (constant 0 in all four of them) set: the pipeline is a generic one */
bool
generic_request(const struct rwm_render_request *r)
{
	if (r->fragment_name == nullptr || strncmp(r->fragment_name, "fixed_frag", 10) != 0) {
		return false;
	}
	for (unsigned i = 0; i < r->fragment_constant_count; i++) {
		if (r->fragment_constants[i].index == 0 && r->fragment_constants[i].bits != 0) {
			return true;
		}
	}
	return false;
}

/* the constants with fc_generic made 0, for the pipeline CoreAnimation gets */
unsigned
without_generic(const struct rwa_constant *from, unsigned n, struct rwa_constant *to)
{
	n = n < 128 ? n : 128;
	for (unsigned i = 0; i < n; i++) {
		to[i] = from[i];
		if (to[i].index == 0) {
			to[i].bits = 0;
		}
	}
	return n;
}

void
generic_keep(struct rwa_device *device, const char *label, const struct rwm_render_request *r,
	     const struct rwa_render_shape *shape, struct rwa_render *base)
{
	struct generic_record *g = (struct generic_record *)calloc(1, sizeof(*g));

	if (g == nullptr) {
		return;
	}
	g->base = base;
	g->device = device;
	strlcpy(g->label, label != nullptr ? label : "?", sizeof(g->label));
	g->vertex = r->vertex != nullptr ? malloc(r->vertex_size) : nullptr;
	g->fragment = malloc(r->fragment_size);
	if ((r->vertex != nullptr && g->vertex == nullptr) || g->fragment == nullptr) {
		free(g->vertex);
		free(g->fragment);
		free(g);
		return;
	}
	if (g->vertex != nullptr) {
		memcpy(g->vertex, r->vertex, r->vertex_size);
	}
	memcpy(g->fragment, r->fragment, r->fragment_size);
	g->vertex_size = r->vertex != nullptr ? r->vertex_size : 0;
	g->fragment_size = r->fragment_size;
	g->nv = r->vertex_constant_count < 128 ? r->vertex_constant_count : 128;
	g->nf = r->fragment_constant_count < 128 ? r->fragment_constant_count : 128;
	memcpy(g->vc, r->vertex_constants, g->nv * sizeof(g->vc[0]));
	memcpy(g->fc, r->fragment_constants, g->nf * sizeof(g->fc[0]));
	g->shape = *shape;
	/* FragmentShaderConfig's fields end at byte 48 (has_lut_texture); 49 to 51 are its padding, and hold whatever
	 * CoreAnimation's ring of uniforms had there -- run 93 built a variant again for each new value in them */
	g->fragment_config = 49;
	g->vertex_config = r->vertex_name != nullptr && strncmp(r->vertex_name, "fixed_vert", 10) == 0 ? 4 : 0;
	struct generic_record *kept = nullptr;

	pthread_mutex_lock(&generic_lock);
	if (ngenerics < sizeof(generics) / sizeof(generics[0])) {
		generics[ngenerics++] = g;
		kept = g;
		g = nullptr;
	}
	pthread_mutex_unlock(&generic_lock);
	if (g != nullptr) {
		free(g->vertex);
		free(g->fragment);
		free(g);
	}
	if (kept != nullptr) {
		prebuild_start(kept);
	}
}

struct variant_call {
	struct generic_record *g;
	const uint8_t *vertex, *fragment;
	struct rwa_render_info info;
	char why[256];
	struct rwa_render *render;
};

void
variant_main(void *context)
{
	variant_call *v = (variant_call *)context;
	struct generic_record *g = v->g;
	const struct rwm_air *air = rwm_air();

	air->render_constants(g->vc, g->nv, g->fc, g->nf);
	air->render_config(g->vertex_config ? v->vertex : nullptr, g->vertex_config, v->fragment, g->fragment_config);
	v->render = air->render_create(g->device, g->vertex, g->vertex_size, nullptr, g->fragment, g->fragment_size,
				       nullptr, &g->shape, &v->info, v->why, sizeof(v->why));
}

/*
 * Variants built AHEAD. A variant built at the draw stops the frame for 1 to 4 s on the phone (83 of them, 125 s, in
 * run 202 alone: the home screen's frames came 5 to 15 s apart). Each one's configuration is in the log ("rwm variant
 * LABEL #N built in T ms: vHEX fHEX"), so tools/rwm-variants.py folds them into userland/share/rwm-variants27.txt,
 * the image has it as /hakios/share/rwm-variants.txt, and once a generic pipeline is made a thread of its own builds
 * every configuration listed for its label, one at a time under the compile lock, while the frames go on. A draw that
 * wants one still being built waits for it (generic_cond) instead of building it again. Boot-arg rwm_prebuild=0: none.
 */
struct prebuilt_config {
	char label[96];
	uint8_t vertex[8], fragment[64];
};
struct prebuilt_config *prebuilt;
unsigned nprebuilt;
pthread_once_t prebuilt_once = PTHREAD_ONCE_INIT;

int
hex_bytes(const char *hex, uint8_t *out, unsigned room)
{
	unsigned n = 0;

	while (hex[0] != 0 && hex[1] != 0 && hex[0] != ' ' && hex[0] != '\n' && n < room) {
		unsigned byte;

		if (sscanf(hex, "%2x", &byte) != 1) {
			return -1;
		}
		out[n++] = (uint8_t)byte;
		hex += 2;
	}
	return (int)n;
}

void
prebuilt_load(void)
{
	FILE *f = fopen("/hakios/share/rwm-variants.txt", "r");
	char line[512];

	if (f == nullptr) {
		return;
	}
	while (fgets(line, sizeof(line), f) != nullptr) {
		char label[96], v[64], fr[200];
		struct prebuilt_config c = {};

		if (line[0] == '#' || sscanf(line, "%95s v%63s f%199s", label, v, fr) != 3 ||
		    hex_bytes(v, c.vertex, sizeof(c.vertex)) < 0 || hex_bytes(fr, c.fragment, sizeof(c.fragment)) < 0) {
			continue;
		}
		strlcpy(c.label, label, sizeof(c.label));
		struct prebuilt_config *grown =
			(struct prebuilt_config *)realloc(prebuilt, (nprebuilt + 1) * sizeof(*prebuilt));

		if (grown == nullptr) {
			break;
		}
		prebuilt = grown;
		prebuilt[nprebuilt++] = c;
	}
	fclose(f);
}

/* the generic pipeline drawn as it is, which draws nothing: why, the first 40 times and then every 256th (run 559: the
 * glass's backdrops still came out holding only their clear) */
void
generic_fallback(const char *label, const char *why)
{
	static unsigned n;
	char line[200];

	if (++n <= 40 || (n & 255) == 0) {
		snprintf(line, sizeof(line), "!!rwm generic %s drawn as it is (%u so far): %s", label +
			 (strncmp(label, "com.apple.coreanimation.", 24) == 0 ? 24 : 0), n, why);
		sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
	}
}

bool
variant_sync(void)
{
	static int sync = -1;

	if (sync < 0) {
		sync = boot_arg("rwm_variant_sync=1") ? 1 : 0;
	}
	return sync == 1;
}

struct async_variant {
	struct generic_record *g;
	struct generic_variant *v;
	uint64_t began;
};

/* a variant a draw asked for, built off the draw's thread (rwm_generic_variant) */
void *
async_variant_main(void *context)
{
	struct async_variant *a = (struct async_variant *)context;
	struct generic_record *g = a->g;
	struct generic_variant *v = a->v;
	variant_call call = {};

	call.g = g;
	call.vertex = v->vertex;
	call.fragment = v->fragment;
	rwm_compile_lock();
	rwm_on_big_stack(variant_main, &call);
	rwm_compile_unlock();
	pthread_mutex_lock(&generic_lock);
	v->render = call.render;
	v->building = false;
	pthread_cond_broadcast(&generic_cond);
	if (call.render != nullptr) {
		rwm_area_label(call.render, g->label);
	}
	pthread_mutex_unlock(&generic_lock);
	{
		static unsigned said;
		char line[300];

		/* as the draw's own line, for tools/rwm-variants.py: "built in T ms: vHEX fHEX" */
		if (said++ < 120) {
			int n = snprintf(line, sizeof(line), "!!rwm variant %s #%u %s in %llu ms: v", g->label +
					 (strncmp(g->label, "com.apple.coreanimation.", 24) == 0 ? 24 : 0),
					 (unsigned)(v - g->variants) + 1, call.render != nullptr ? "built" : "REFUSED",
					 (unsigned long long)((rwm_ns() - a->began) / 1000000));
			for (unsigned i = 0; i < g->vertex_config && n > 0 && n < (int)sizeof(line) - 4; i++) {
				n += snprintf(line + n, sizeof(line) - n, "%02x", v->vertex[i]);
			}
			n += snprintf(line + n, sizeof(line) - n, " f");
			for (unsigned i = 0; i < g->fragment_config && n > 0 && n < (int)sizeof(line) - 4; i++) {
				n += snprintf(line + n, sizeof(line) - n, "%02x", v->fragment[i]);
			}
			sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
		}
	}
	free(a);
	return nullptr;
}

void *
prebuild_main(void *context)
{
	struct generic_record *g = (struct generic_record *)context;
	const char *label = g->label + (strncmp(g->label, "com.apple.coreanimation.", 24) == 0 ? 24 : 0);
	unsigned built = 0;
	const uint64_t began = rwm_ns();

	for (unsigned k = 0; k < nprebuilt; k++) {
		const struct prebuilt_config *c = &prebuilt[k];
		struct generic_variant *v = nullptr;
		bool known = false;

		if (strcmp(c->label, label) != 0) {
			continue;
		}
		pthread_mutex_lock(&generic_lock);
		for (unsigned i = 0; i < g->nvariants && !known; i++) {
			known = memcmp(g->variants[i].vertex, c->vertex, sizeof(c->vertex)) == 0 &&
				memcmp(g->variants[i].fragment, c->fragment, sizeof(c->fragment)) == 0;
		}
		if (!known && g->nvariants < sizeof(g->variants) / sizeof(g->variants[0])) {
			v = &g->variants[g->nvariants++];
			memcpy(v->vertex, c->vertex, sizeof(v->vertex));
			memcpy(v->fragment, c->fragment, sizeof(v->fragment));
			v->render = nullptr;
			v->building = true;
		}
		pthread_mutex_unlock(&generic_lock);
		if (v == nullptr) {
			continue;
		}

		variant_call call = {};

		call.g = g;
		call.vertex = v->vertex;
		call.fragment = v->fragment;
		rwm_compile_lock();
		rwm_on_big_stack(variant_main, &call);
		rwm_compile_unlock();
		pthread_mutex_lock(&generic_lock);
		v->render = call.render;
		v->building = false;
		pthread_cond_broadcast(&generic_cond);
		pthread_mutex_unlock(&generic_lock);
		built += call.render != nullptr;
	}
	if (built != 0) {
		char line[200];

		snprintf(line, sizeof(line), "!!rwm prebuilt %u variants of %s in %llu ms", built, label,
			 (unsigned long long)((rwm_ns() - began) / 1000000));
		sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
	}
	return nullptr;
}

void
prebuild_start(struct generic_record *g)
{
	pthread_t thread;
	pthread_attr_t attr;

	if (boot_arg("rwm_prebuild=0")) {
		return;
	}
	pthread_once(&prebuilt_once, prebuilt_load);
	if (nprebuilt == 0) {
		return;
	}
	/* a big stack: the build runs the compiler (rwm_on_big_stack wants to be called from one that can hold it) */
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1u << 20);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attr, prebuild_main, g) != 0) {
		rwm_print("no thread to build %s's variants ahead\n", g->label);
	}
	pthread_attr_destroy(&attr);
}

/* `size` bytes of `buffer` at `offset` into `out`; false when they are not all there */
bool
config_of(struct rwa_buffer *buffer, size_t offset, unsigned size, uint8_t *out)
{
	const struct rwm_air *air = rwm_air();

	if (size == 0) {
		return true;
	}
	if (buffer == nullptr || air->buffer_map == nullptr || air->buffer_size == nullptr) {
		return false;
	}
	const uint8_t *map = (const uint8_t *)air->buffer_map(buffer);

	if (map == nullptr || offset + size > air->buffer_size(buffer)) {
		return false;
	}
	memcpy(out, map + offset, size);
	return true;
}

} // namespace

extern "C" bool
rwm_render_prepare(const char *label, struct rwa_render_shape *shape, bool has_vertex_descriptor,
		   struct rwa_render **render, struct rwa_render_info *info)
{
	/* what it asks for, said before the source-over that CoreAnimation's in-shader blends are given, so that the
	 * lines say what was asked (src/metal/shape.cpp) */
	rwm_shape_say(label, shape, has_vertex_descriptor);
	rwm_xm_blend_apply(label, shape);
	if (coreanimation_draw(label) && rwm_warm_take(label, shape, render, info)) {
		rwm_area_label(*render, label);
		if (shape->color_count > 0) {
			rwm_blend_result(info->blend_enabled_mask, info->color_write_masks);
		}
		return true;
	}
	return false;
}

extern "C" struct rwa_render *
rwm_render_compile(struct rwa_device *device, const char *label, const struct rwm_render_request *r,
		   struct rwa_render_shape *shape, struct rwa_render_info *info, char *why, size_t why_size)
{
	static unsigned long refused;
	/* a generic pipeline: built with fc_generic 0 and kept, its variants built at the draws (see generic_record) */
	const bool generic = generic_request(r) && !boot_arg("rwm_generic=1");
	struct rwa_constant vc0[128], fc0[128];
	struct rwm_render_request base = *r;

	if (generic) {
		base.vertex_constant_count = without_generic(r->vertex_constants, r->vertex_constant_count, vc0);
		base.fragment_constant_count = without_generic(r->fragment_constants, r->fragment_constant_count, fc0);
		base.vertex_constants = vc0;
		base.fragment_constants = fc0;
	}
	/* boot-arg rwm_nonoise=1: CoreAnimation's dither off -- fc_has_noise_scale (8) and fc_has_noise_tex (21) made 0, so
	 * no draw reads the 32 x 32 noise texture at slot 9 through the shader's own sampler (15 November: the icons' and
	 * labels' pipelines are the same functions as the glass that shows, and differ from it in the noise) */
	bool nonoise = false;

	if (boot_arg("rwm_nonoise=1")) {
		if (base.fragment_constants != fc0) {
			base.fragment_constant_count = r->fragment_constant_count < 128 ? r->fragment_constant_count : 128;
			memcpy(fc0, r->fragment_constants, base.fragment_constant_count * sizeof(fc0[0]));
			base.fragment_constants = fc0;
		}
		for (unsigned i = 0; i < base.fragment_constant_count; i++) {
			if ((fc0[i].index == 8 || fc0[i].index == 21) && fc0[i].bits != 0) {
				fc0[i].bits = 0;
				nonoise = true;
			}
		}
	}
	/* boot-arg rwm_fcset=I:V,I:V...: those fragment constants given those values in every pipeline whose label has
	 * "Timg" in it (15 November: the icons' pipelines and the glass that shows are the same functions and differ in
	 * fc_texture_function 28, fc_blend_function 29 and fc_dest_count 35) */
	{
		const char *at = strstr(rwm_boot_args(), "rwm_fcset=");

		if (at != nullptr && label != nullptr && strstr(label, "Timg") != nullptr) {
			if (base.fragment_constants != fc0) {
				base.fragment_constant_count = r->fragment_constant_count < 128 ? r->fragment_constant_count : 128;
				memcpy(fc0, r->fragment_constants, base.fragment_constant_count * sizeof(fc0[0]));
				base.fragment_constants = fc0;
			}
			at += 10;
			while (*at >= '0' && *at <= '9') {
				char *end;
				unsigned long index = strtoul(at, &end, 10), value;

				if (*end != ':') {
					break;
				}
				value = strtoul(end + 1, &end, 0);
				for (unsigned i = 0; i < base.fragment_constant_count; i++) {
					if (fc0[i].index == index) {
						fc0[i].bits = value;
					}
				}
				nonoise = true;
				at = *end == ',' ? end + 1 : end;
			}
		}
	}
	/* boot-arg rwm_vcset=I:V,...: the same for the vertex constants of "Timg" pipelines (fc_texcoord_count1 is 54) */
	{
		const char *at = strstr(rwm_boot_args(), "rwm_vcset=");

		if (at != nullptr && label != nullptr && strstr(label, "Timg") != nullptr) {
			if (base.vertex_constants != vc0) {
				base.vertex_constant_count = r->vertex_constant_count < 128 ? r->vertex_constant_count : 128;
				memcpy(vc0, r->vertex_constants, base.vertex_constant_count * sizeof(vc0[0]));
				base.vertex_constants = vc0;
			}
			at += 10;
			while (*at >= '0' && *at <= '9') {
				char *end;
				unsigned long index = strtoul(at, &end, 10), value;

				if (*end != ':') {
					break;
				}
				value = strtoul(end + 1, &end, 0);
				for (unsigned i = 0; i < base.vertex_constant_count; i++) {
					if (vc0[i].index == index) {
						vc0[i].bits = value;
					}
				}
				nonoise = true;
				at = *end == ',' ? end + 1 : end;
			}
		}
	}
	compile_call call = { device, generic || nonoise ? &base : r, shape, info, why, why_size, nullptr };

	if (coreanimation_draw(label)) {
		rwm_recipe_record(label, shape, r->vertex_name, r->fragment_name, r->vertex_constants,
				  r->vertex_constant_count, r->fragment_constants, r->fragment_constant_count);
	}
	rwm_compile_lock();     /* (one build at a time: the warm-up thread builds too) */
	rwm_on_big_stack(compile_main, &call);
	rwm_compile_unlock();
	if (call.render == nullptr) {
		/*
		 * Printed as well as returned, and only the first, second, fourth... time, because the same specification
		 * is asked for over and over: the caller's NSError is the sentence and nothing more, and CoreAnimation
		 * discards it and retries. A refusal that reaches no log is one nobody can grep for -- one specification was
		 * declined 80 times over 42 lives of backboardd before the vertex layout existed, counted by hand.
		 */
		if ((++refused & (refused - 1)) == 0) {
			rwm_print("this render pipeline is refused: %s\n", why);
			if (refused > 1) {
				rwm_print("  ... and that is the %lu'th time\n", refused);
			}
		}
		rwm_capture_pipeline(label, r, shape, nullptr);   /* (a refused one too: its constants, for the Mac) */
		return nullptr;
	}
	/* what the library made of it, in GALLIUM's bits: for any mask but All, None, green|blue and red|alpha these
	 * nibbles must DIFFER from the writeMask said above, the two enumerations numbering their channels in opposite
	 * directions */
	if (shape->color_count > 0) {
		rwm_blend_result(info->blend_enabled_mask, info->color_write_masks);
	}
	rwm_area_label(call.render, label);
	rwm_capture_pipeline(label, r, shape, call.render);
	pipelog(label, r, shape, call.render, info);
	if (generic && !boot_arg("rwm_specialize=0") && rwm_air()->render_config != nullptr) {
		generic_keep(device, label, r, shape, call.render);
	}
	return call.render;
}

/* the variant of a generic pipeline for the configuration in buffer 0 of each stage, built the first time it is
 * drawn with; NULL: draw with the pipeline itself (not a generic one, no configuration bound, no room, a failed build) */
extern "C" struct rwa_render *
rwm_generic_variant(struct rwa_render *base, struct rwa_buffer *vertex, size_t vertex_offset,
		    struct rwa_buffer *fragment, size_t fragment_offset, bool wait)
{
	struct generic_record *g = nullptr;
	uint8_t vcfg[8] = { 0 }, fcfg[64] = { 0 };
	struct rwa_render *found = nullptr;

	if (base == nullptr || ngenerics == 0) {
		return nullptr;
	}
	pthread_mutex_lock(&generic_lock);
	for (unsigned i = 0; i < ngenerics; i++) {
		if (generics[i]->base == base) {
			g = generics[i];
			break;
		}
	}
	if (g == nullptr) {
		pthread_mutex_unlock(&generic_lock);
		return nullptr;
	}
	if (!config_of(vertex, vertex_offset, g->vertex_config, vcfg) ||
	    !config_of(fragment, fragment_offset, g->fragment_config, fcfg)) {
		pthread_mutex_unlock(&generic_lock);
		generic_fallback(g->label, "no configuration in buffer 0");
		return nullptr;
	}
	/* the padding inside the two FragmentShaderConfig::Image entries (bytes 3 and 15 of each 16): never read */
	fcfg[3] = fcfg[15] = fcfg[19] = fcfg[31] = 0;
	for (unsigned i = 0; i < g->nvariants; i++) {
		if (memcmp(g->variants[i].vertex, vcfg, sizeof(vcfg)) == 0 &&
		    memcmp(g->variants[i].fragment, fcfg, sizeof(fcfg)) == 0) {
			/* built ahead and not done yet: drawn with the generic pipeline meanwhile (run 551: the draw that waited
			 * held the screen and the touches for 20 s opening Settings), or waited for with rwm_variant_sync=1 */
			if (g->variants[i].building && !variant_sync() && !wait) {
				pthread_mutex_unlock(&generic_lock);
				generic_fallback(g->label, "its variant still building (a surface's pass)");
				return nullptr;
			}
			while (g->variants[i].building) {
				pthread_cond_wait(&generic_cond, &generic_lock);
			}
			found = g->variants[i].render;
			last_variant = i + 1;
			pthread_mutex_unlock(&generic_lock);
			if (found == nullptr) {
				generic_fallback(g->label, "its variant was refused");
			}
			return found;
		}
	}
	if (g->nvariants == sizeof(g->variants) / sizeof(g->variants[0])) {
		if (!g->full_said) {
			g->full_said = true;
			rwm_print("generic pipeline %s: %u configurations built, the rest drawn as it is\n", g->label,
				  g->nvariants);
		}
		pthread_mutex_unlock(&generic_lock);
		generic_fallback(g->label, "no room for another variant");
		return nullptr;
	}

	variant_call call = {};
	const uint64_t began = rwm_ns();
	/* its place taken first, marked as being built, and the lock let go for the build: the thread that builds ahead
	 * (prebuild_main) and the other draws go on meanwhile; entries never move, so the pointer stays good */
	struct generic_variant *v = &g->variants[g->nvariants++];

	memcpy(v->vertex, vcfg, sizeof(vcfg));
	memcpy(v->fragment, fcfg, sizeof(fcfg));
	v->render = nullptr;
	v->building = true;
	pthread_mutex_unlock(&generic_lock);

	/* built on a thread of its own, the draw going on with the generic pipeline (which reads the same configuration
	 * from buffer 0 and draws the same, only slower): a variant takes ~1 s on the phone, and run 551's Settings asked
	 * for 23 in a row */
	if (!variant_sync() && !wait) {
		struct async_variant *a = (struct async_variant *)calloc(1, sizeof(*a));
		pthread_t thread;
		pthread_attr_t attr;
		bool started = false;

		if (a != nullptr) {
			a->g = g;
			a->v = v;
			a->began = began;
			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, 1u << 20);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			started = pthread_create(&thread, &attr, async_variant_main, a) == 0;
			pthread_attr_destroy(&attr);
			if (!started) {
				free(a);
			}
		}
		if (started) {
			generic_fallback(g->label, "its variant started off the draw (a surface's pass)");
			return nullptr;
		}
	}

	call.g = g;
	call.vertex = vcfg;
	call.fragment = fcfg;
	rwm_compile_lock();
	rwm_on_big_stack(variant_main, &call);
	rwm_compile_unlock();

	pthread_mutex_lock(&generic_lock);
	v->render = call.render;
	v->building = false;
	pthread_cond_broadcast(&generic_cond);
	if (call.render != nullptr) {
		rwm_area_label(call.render, g->label);
	}
	{
		static unsigned said;
		char line[300];
		int n;

		if (said++ < 120) {
			n = snprintf(line, sizeof(line), "!!rwm variant %s #%u %s in %llu ms: v", g->label + (strncmp(g->label,
				     "com.apple.coreanimation.", 24) == 0 ? 24 : 0), (unsigned)(v - g->variants) + 1,
				     call.render != nullptr ? "built" : "REFUSED", (unsigned long long)((rwm_ns() - began) / 1000000));
			for (unsigned i = 0; i < g->vertex_config && n > 0 && n < (int)sizeof(line) - 4; i++) {
				n += snprintf(line + n, sizeof(line) - n, "%02x", vcfg[i]);
			}
			n += snprintf(line + n, sizeof(line) - n, " f");
			for (unsigned i = 0; i < g->fragment_config && n > 0 && n < (int)sizeof(line) - 4; i++) {
				n += snprintf(line + n, sizeof(line) - n, "%02x", fcfg[i]);
			}
			if (call.render == nullptr && n > 0 && n < (int)sizeof(line) - 8) {
				snprintf(line + n, sizeof(line) - n, ": %s", call.why);
			}
			sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
		}
	}
	found = call.render;
	last_variant = (unsigned)(v - g->variants) + 1;
	pthread_mutex_unlock(&generic_lock);
	return found;
}

extern "C" unsigned
rwm_generic_last_variant(void)
{
	return last_variant;
}
