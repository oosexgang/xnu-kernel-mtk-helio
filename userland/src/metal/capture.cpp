/*
 * Boot-arg rwm_capture=TEXT: one pipeline whose label has TEXT in it, and one draw of it, written whole into the host's
 * kept lines ("!!cap ..."), for userland/tools/air/ca27_replay.c to draw again on the Mac through the same library on softpipe.
 * The 27 release's image layers come out with their colour near zero whatever their textures hold (runs 55 to 61), and
 * finding which step of the shader makes it so is done there and not with a run on the phone for every guess.
 *
 * The pipeline: its functions' names and their modules' size and FNV-1a hash (the Mac takes the modules out of the
 * same QuartzCore library), every function constant, and the shape (struct rwa_render_shape) as its bytes. The draw:
 * primitive, counts, instances, the viewport and scissor, and for every bound buffer its index, offset and the 512
 * bytes from there, and every texture's shape. Base64, 160 characters a line.
 */
#include "rwm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

namespace {

const char *
wanted(void)
{
	static char text[64];
	static int read;

	if (!read) {
		const char *at = strstr(rwm_boot_args(), "rwm_capture=");

		read = 1;
		if (at != nullptr) {
			size_t n = 0;

			at += 12;
			while (at[n] != 0 && at[n] != ' ' && n < sizeof(text) - 1) {
				text[n] = at[n];
				n++;
			}
			text[n] = 0;
		}
	}
	return text[0] != 0 ? text : nullptr;
}

void
say(const char *line)
{
	sysctlbyname("kern.hakios.say", NULL, NULL, (void *)line, strlen(line));
}

uint64_t
fnv(const void *bytes, size_t size)
{
	uint64_t h = 1469598103934665603ull;

	for (size_t i = 0; i < size; i++) {
		h = (h ^ ((const uint8_t *)bytes)[i]) * 1099511628211ull;
	}
	return h;
}

/* `bytes` as base64 lines "!!cap NAME k: ...", 120 bytes (160 characters) a line */
void
say_bytes(const char *name, const void *bytes, size_t size)
{
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const uint8_t *b = (const uint8_t *)bytes;

	for (size_t at = 0, k = 0; at < size; at += 120, k++) {
		char line[240];
		int n = snprintf(line, sizeof(line), "!!cap %s %zu:", name, k);
		size_t end = at + 120 < size ? at + 120 : size;

		line[n++] = ' ';
		for (size_t i = at; i < end; i += 3) {
			uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < end ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < end ? b[i + 2] : 0);

			line[n++] = digits[v >> 18 & 63];
			line[n++] = digits[v >> 12 & 63];
			line[n++] = i + 1 < end ? digits[v >> 6 & 63] : '=';
			line[n++] = i + 2 < end ? digits[v & 63] : '=';
		}
		line[n] = 0;
		say(line);
	}
}

const struct rwa_render *captured_render;
bool draw_captured;

} // namespace

/*
 * Boot-arg rwm_capture_gen=1: each generic pipeline, once, as it is kept (generic_keep) -- its functions, constants
 * and shape, "!!gen LABEL ..." -- for the Mac to build every variant configuration of userland/share/
 * rwm-variants27.txt the way the phone does (userland/src/mali/tools/rwagen.c) into the stage cache.
 */
extern "C" void
rwm_capture_generic(const char *label, const struct rwm_render_request *r, const struct rwa_render_shape *shape)
{
	char line[240];
	const char *l;

	if (label == nullptr || strstr(rwm_boot_args(), "rwm_capture_gen=1") == nullptr) {
		return;
	}
	l = strncmp(label, "com.apple.coreanimation.", 24) == 0 ? label + 24 : label;
	snprintf(line, sizeof(line), "!!gen %s functions %s %zu %016llx %s %zu %016llx", l,
		 r->vertex_name ? r->vertex_name : "-", r->vertex_size, (unsigned long long)fnv(r->vertex, r->vertex_size),
		 r->fragment_name ? r->fragment_name : "-", r->fragment_size,
		 (unsigned long long)fnv(r->fragment, r->fragment_size));
	say(line);
	for (unsigned i = 0; i < r->vertex_constant_count; i++) {
		snprintf(line, sizeof(line), "!!gen %s vconst %u %016llx", l, r->vertex_constants[i].index,
			 (unsigned long long)r->vertex_constants[i].bits);
		say(line);
	}
	for (unsigned i = 0; i < r->fragment_constant_count; i++) {
		snprintf(line, sizeof(line), "!!gen %s fconst %u %016llx", l, r->fragment_constants[i].index,
			 (unsigned long long)r->fragment_constants[i].bits);
		say(line);
	}
	snprintf(line, sizeof(line), "gen-%s-shape", l);
	say_bytes(line, shape, sizeof(*shape));
}

extern "C" void
rwm_capture_pipeline(const char *label, const struct rwm_render_request *r, const struct rwa_render_shape *shape,
		     const struct rwa_render *render)
{
	const char *text = wanted();
	static bool refused_said;
	char line[240];

	if (text == nullptr || label == nullptr || strstr(label, text) == nullptr || captured_render != nullptr) {
		return;
	}
	/* a pipeline the library refused: its request once, as "cap refused" (the same lines after it), for the Mac
	 * to build again with the very constants and find the refusal there; no draw follows */
	if (render == nullptr) {
		if (refused_said) {
			return;
		}
		refused_said = true;
		snprintf(line, sizeof(line), "!!cap refused %s", label);
	} else {
		captured_render = render;
		snprintf(line, sizeof(line), "!!cap pipeline %s", label);
	}
	say(line);
	snprintf(line, sizeof(line), "!!cap functions %s %zu %016llx %s %zu %016llx", r->vertex_name ? r->vertex_name : "-",
		 r->vertex_size, (unsigned long long)fnv(r->vertex, r->vertex_size), r->fragment_name ? r->fragment_name : "-",
		 r->fragment_size, (unsigned long long)fnv(r->fragment, r->fragment_size));
	say(line);
	for (unsigned i = 0; i < r->vertex_constant_count; i++) {
		snprintf(line, sizeof(line), "!!cap vconst %u %016llx", r->vertex_constants[i].index,
			 (unsigned long long)r->vertex_constants[i].bits);
		say(line);
	}
	for (unsigned i = 0; i < r->fragment_constant_count; i++) {
		snprintf(line, sizeof(line), "!!cap fconst %u %016llx", r->fragment_constants[i].index,
			 (unsigned long long)r->fragment_constants[i].bits);
		say(line);
	}
	snprintf(line, sizeof(line), "!!cap shape %zu", sizeof(*shape));
	say(line);
	say_bytes("shapebytes", shape, sizeof(*shape));
}

extern "C" void
rwm_capture_draw(const struct rwm_draw *d)
{
	static unsigned long seen;
	const struct rwm_air *air = rwm_air();
	char line[240];

	/* the 50th draw of the pipeline, or the Nth with boot-arg rwm_capture_after=N: a pipeline drawn twice a frame in
	 * the few frames a still home screen presents never reached 50 (the 27 release's downsample, run 240) */
	static unsigned long after;

	if (after == 0) {
		const char *at = strstr(rwm_boot_args(), "rwm_capture_after=");

		after = at != nullptr ? strtoul(at + 18, nullptr, 10) : 50;
		after = after != 0 ? after : 1;
	}
	/* a generic pipeline is drawn by its variant: the draw is matched by the pipeline as bound too */
	if (draw_captured || captured_render == nullptr ||
	    (d->render != captured_render && d->base_render != captured_render) || ++seen < after ||
	    air->buffer_map == nullptr) {
		return;
	}
	draw_captured = true;
	snprintf(line, sizeof(line), "!!cap draw prim %u start %u count %u instances %u base %u index %s/%u/%zu viewport %s "
		 "scissor %s %u %u %u %u colors %u", d->primitive, d->vertex_start, d->vertex_count, d->instance_count,
		 d->base_instance, d->index_buffer ? "yes" : "no", d->index_type, d->index_offset, d->viewport ? "set" : "none",
		 d->scissor ? "set" : "none", d->scissor ? d->scissor[0] : 0, d->scissor ? d->scissor[1] : 0,
		 d->scissor ? d->scissor[2] : 0, d->scissor ? d->scissor[3] : 0, d->color_count);
	say(line);
	if (d->viewport != nullptr) {
		snprintf(line, sizeof(line), "!!cap viewport %g %g %g %g", d->viewport[0], d->viewport[1], d->viewport[2],
			 d->viewport[3]);
		say(line);
	}
	for (unsigned i = 0; i < d->vertex_count_bound && i < RWM_MAX_BUFFERS; i++) {
		const uint8_t *m;
		char name[32];

		if (!d->vertex_bound[i] || d->vertex[i] == nullptr || (m = (const uint8_t *)air->buffer_map(d->vertex[i])) == nullptr) {
			continue;
		}
		size_t size = air->buffer_size != nullptr ? air->buffer_size(d->vertex[i]) : 0;
		size_t take = size > d->vertex_offset[i] ? size - d->vertex_offset[i] : 0;

		take = take > 2048 ? 2048 : take;
		snprintf(line, sizeof(line), "!!cap vbuf %u offset %zu size %zu stream %u take %zu", i, d->vertex_offset[i], size,
			 (d->stream_mask >> i) & 1, take);
		say(line);
		snprintf(name, sizeof(name), "vbuf%u", i);
		say_bytes(name, m + d->vertex_offset[i], take);
	}
	for (unsigned i = 0; i < d->fragment_count_bound && i < RWM_MAX_BUFFERS; i++) {
		const uint8_t *m;
		char name[32];

		if (!d->fragment_bound[i] || d->fragment[i] == nullptr ||
		    (m = (const uint8_t *)air->buffer_map(d->fragment[i])) == nullptr) {
			continue;
		}
		size_t size = air->buffer_size != nullptr ? air->buffer_size(d->fragment[i]) : 0;
		size_t take = size > d->fragment_offset[i] ? size - d->fragment_offset[i] : 0;

		take = take > 2048 ? 2048 : take;
		snprintf(line, sizeof(line), "!!cap fbuf %u offset %zu size %zu take %zu", i, d->fragment_offset[i], size, take);
		say(line);
		snprintf(name, sizeof(name), "fbuf%u", i);
		say_bytes(name, m + d->fragment_offset[i], take);
	}
	snprintf(line, sizeof(line), "!!cap textures %u samplers %u", d->texture_count, d->sampler_count);
	say(line);
}

extern "C" void
rwm_capture_textures(const struct rwa_render *render, const struct rwa_texture_shape *const *shapes, unsigned count)
{
	static bool said;
	char line[240];
	int at;

	if (said || render == nullptr || render != captured_render) {
		return;
	}
	said = true;
	at = snprintf(line, sizeof(line), "!!cap slots %u:", count);
	for (unsigned i = 0; i < count && at < (int)sizeof(line) - 32; i++) {
		at += snprintf(line + at, sizeof(line) - at, shapes[i] ? " %u:%ux%u/%u/t%u/u%#x" : " %u:-", i,
			       shapes[i] ? shapes[i]->width : 0, shapes[i] ? shapes[i]->height : 0,
			       shapes[i] ? shapes[i]->pixel_format : 0, shapes[i] ? shapes[i]->type : 0, shapes[i] ? shapes[i]->usage : 0);
	}
	say(line);
}
