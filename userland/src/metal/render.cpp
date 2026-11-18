/*
 * A render encoder's pass and state, and its draws recorded from them (the plug-in reads Metal's descriptor and hands
 * over each object with the library's and the core's under it). Nothing runs here: what a draw draws with is what was
 * bound when it was encoded, recorded into a work that src/metal/run.cpp runs at -commit, and the objects it was
 * recorded from listed for the plug-in to keep alive with it.
 */
#include "rwm.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <stdlib.h>

namespace {

struct rwa_texture *
mesa_of(const struct rwm_texture *core)
{
	return core != nullptr ? rwm_texture_mesa(core) : nullptr;
}

/* the pass's attachments: the colours over a surface copied back after the buffer (attachment 0 alone for a clear, as
 * the copy-back road counted it), and every attachment's surface drawn in place, by attachment */
void
record_targets(const struct rwm_render_state *s, struct rwm_work *w, bool first_only)
{
	for (unsigned i = 0; i < s->color_count; i++) {
		struct rwm_texture *core = s->color[i].core;

		rwm_texture_note_filled(core);      /* (drawn into: what is in it is what is drawn) */
		if (i == 0) {
			rwm_target_counts[core != nullptr && rwm_texture_in_place(core) != 0 ? 1 : 0]++;
			if (core != nullptr && rwm_texture_in_place(core) != 0) {
				rwm_target_last_surface = rwm_texture_in_place(core);
			}
		}

		if (core != nullptr && (!first_only || i == 0)) {
			w->targets[w->target_count++] = core;
		}
		w->target_surface[i] = core != nullptr ? rwm_texture_in_place(core) : 0;
		rwm_work_keep(w, s->color[i].object);
	}
	w->target_surface_count = s->color_count;
	rwm_work_keep(w, s->depth.object);
}

/*
 * A pass's load actions are for its start, once: the draws after the first draw over what the first left. Until 26
 * October every draw of a pass that began with MTLLoadActionClear cleared the attachments again (rwa_render_draw
 * clears when a draw carries a clear colour), and only the last draw of such a pass survived -- an application's
 * snapshot (82 draws, each clearing) came out black, and so did whatever else CoreAnimation drew offscreen that way.
 */
void
loaded(struct rwm_render_state *s)
{
	s->clears = false;
	s->clears_depth = false;
	s->clears_stencil = false;
	for (unsigned i = 0; i < 8; i++) {
		s->color_load[i] = 1;   /* MTLLoadActionLoad: what the first draw left */
	}
}

} // namespace

extern "C" void
rwm_work_keep(struct rwm_work *w, void *object)
{
	if (object != nullptr && w->keep_count < RWM_WORK_KEEP) {
		w->keep[w->keep_count++] = object;
	}
}

namespace {

/*
 * Boot-arg rwm_passlog=S: the window server's passes in order, from S seconds after its first pass and for as many as
 * fit in 700 lines ("!!P" lines into the host's kept buffer). "b N" a pass begins; "c I ID WxH F L" its attachment I,
 * the texture's id, size, format, L 1 when it clears (MTLLoadActionClear) and its store action; "d LABEL c:IDS t:IDS"
 * a draw, its pipeline and the textures it samples; "e N" the pass ends after N draws. A texture's id is the low 16
 * bits of its address, so a pass that samples what an earlier one drew shows it by the same id.
 */
unsigned passlog_pass;
unsigned passlog_draws;
char passlog_buf[3000], passlog_prev[3000];
size_t passlog_len;
unsigned passlog_repeat;
unsigned passlog_lines;

/* on from S seconds after the window server's first pass, until 1100 lines were said */
bool
passlog_on(void)
{
	static int state = -1;
	static uint64_t start_ns;

	if (state < 0) {
		const char *at = strstr(rwm_boot_args(), "rwm_passlog=");
		unsigned sec = 0;

		state = at != nullptr && sscanf(at + 12, "%u", &sec) == 1 && strcmp(getprogname(), "backboardd") == 0;
		start_ns = rwm_ns() + (uint64_t)sec * 1000000000ull;
	}
	return state == 1 && passlog_lines < 1100 && rwm_ns() >= start_ns;
}

void
passlog_say(const char *text)
{
	char line[220];

	snprintf(line, sizeof(line), "!!P %s", text);
	sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
	passlog_lines++;
}

/* a line of the pass being recorded, kept until its end */
void
passlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void
passlog(const char *fmt, ...)
{
	va_list ap;

	if (passlog_len + 220 > sizeof(passlog_buf)) {
		return;
	}
	va_start(ap, fmt);
	int n = vsnprintf(passlog_buf + passlog_len, sizeof(passlog_buf) - passlog_len - 1, fmt, ap);
	va_end(ap);
	if (n > 0) {
		passlog_len += (size_t)n < sizeof(passlog_buf) - passlog_len - 2 ? (size_t)n : 0;
		passlog_buf[passlog_len++] = '\n';
		passlog_buf[passlog_len] = 0;
	}
}

/* the pass ended: said line by line unless it is the same as the one before (textures' ids aside: the swap chain's
 * surfaces turn), which only counts it */
void
passlog_flush(void)
{
	char key[3000];
	size_t k = 0;

	for (size_t i = 0; i < passlog_len && k < sizeof(key) - 1; i++) {
		if (passlog_buf[i] == 'c' && (i == 0 || passlog_buf[i - 1] == '\n')) {
			while (i < passlog_len && passlog_buf[i] != '\n') {   /* attachments: only their formats count */
				i++;
			}
			continue;
		}
		key[k++] = passlog_buf[i];
	}
	key[k] = 0;
	if (strcmp(key, passlog_prev) == 0) {
		passlog_repeat++;
	} else {
		char line[64];

		if (passlog_repeat != 0) {
			snprintf(line, sizeof(line), "r %u", passlog_repeat);
			passlog_say(line);
		}
		passlog_repeat = 0;
		snprintf(line, sizeof(line), "b %u", passlog_pass);
		passlog_say(line);
		for (char *at = passlog_buf, *nl; *at != 0 && (nl = strchr(at, '\n')) != nullptr; at = nl + 1) {
			*nl = 0;
			passlog_say(at);
		}
		memcpy(passlog_prev, key, k + 1);
	}
	passlog_len = 0;
	passlog_buf[0] = 0;
}

unsigned
tex_id(const struct rwm_texture *t)
{
	return t != nullptr ? (unsigned)((uintptr_t)t >> 4 & 0xffff) : 0;
}

} // namespace

extern "C" void
rwm_render_begin(struct rwm_render_state *s, struct rwa_device *device)
{
	memset(s, 0, sizeof(*s));
	s->device = device;
	passlog_draws = 0;
	passlog_pass++;
	passlog_len = 0;
	passlog_buf[0] = 0;
}

/*
 * What the pass draws into, and whether it begins by clearing: Metal keeps it in the pass descriptor and not in the
 * pipeline, so it belongs to the encoder. A resolve is the one thing a multisample pass asks for that this driver
 * cannot do, and it is said rather than ignored: the library renders into multisample attachments (docs/metal.md
 * 10.34) and resolves nothing, so the one-sample texture an application means to READ would come back holding what it
 * held before -- a pass that seems to have drawn nothing. (MTLStoreActionMultisampleResolve is 2, ...AndStore... 3.)
 */
extern "C" void
rwm_render_color(struct rwm_render_state *s, unsigned index, unsigned level, void *object, struct rwm_texture *core, bool clears,
		 unsigned long load_action, const double color[4], unsigned long store, bool resolve_texture)
{
	if (index >= 8 || object == nullptr) {
		return;
	}
	s->color[index] = rwm_bound_texture{ object, core };
	s->color_level[index] = level;
	if (core != nullptr && passlog_on()) {
		const struct rwa_texture_shape *sh = rwm_texture_shape(core);

		/* and the IOSurface it lies over (0: none), to be matched with the swaps' surfaces (15 November) */
		passlog("c %u %04x %ux%u %u %d %lu s%u L%u", index, tex_id(core), sh->width, sh->height, sh->pixel_format,
			clears ? 1 : 0, store, rwm_texture_in_place(core), level);
	}
	s->color_count = index + 1;
	if (store == 2 || store == 3 || resolve_texture) {
		rwm_print("colour attachment %u asks for a multisample "
			  "resolve (storeAction %lu%s) and this driver resolves nothing: "
			  "the attachment itself is stored and the resolve texture is "
			  "left as it was\n", index, store, resolve_texture ? ", with a resolveTexture" : "");
	}
	/* boot-arg rwm_loadclear=1: an attachment 0 that would be loaded is cleared to transparent black instead -- whether
	 * the draws of a pass that loads are lost with what it loads (15 November: icons and labels are drawn in such
	 * passes and never seen, the background of the clearing pass before them is) */
	if (index == 0 && !clears && strstr(rwm_boot_args(), "rwm_loadclear=1") != nullptr) {
		static const double transparent[4] = { 0, 0, 0, 0 };

		clears = true;
		load_action = 2;
		color = transparent;
	}
	/* every attachment's own actions and clear colour, for the library (rwa_draw.color_actions) */
	s->color_actions = true;
	s->color_load[index] = (unsigned char)(load_action <= 2 ? load_action : 1);
	s->color_store[index] = (unsigned char)(store <= 255 ? store : 1);
	for (unsigned i = 0; i < 4; i++) {
		s->color_clear[index][i] = (float)color[i];
	}
	if (index == 0 && clears) {
		for (unsigned i = 0; i < 4; i++) {
			s->clear_color[i] = (float)color[i];
		}
		s->clears = true;
	}
}

/*
 * The depth/stencil attachment, which is two descriptors over one texture: for Depth32Float_Stencil8 an application
 * sets the same texture on both, for a depth-only format only the first. Whichever carries a texture is the attachment,
 * and both carrying DIFFERENT ones is refused out loud -- a Gallium framebuffer has one zsbuf, and quietly taking the
 * depth one would drop the stencil half. Each aspect keeps its own load action (a pass that clears the depth and loads
 * the stencil is an ordinary pass).
 */
extern "C" void
rwm_render_depth(struct rwm_render_state *s, void *depth, struct rwm_texture *depth_core, bool clears_depth,
		 double clear_depth, void *stencil, struct rwm_texture *stencil_core, bool clears_stencil,
		 unsigned clear_stencil)
{
	if (depth != nullptr && stencil != nullptr && depth != stencil) {
		rwm_print("the depth attachment and the stencil attachment are two "
			  "different textures; one texture carries both aspects here, so this "
			  "pass is encoded with neither\n");
		return;
	}
	s->depth = depth != nullptr ? rwm_bound_texture{ depth, depth_core } : rwm_bound_texture{ stencil, stencil_core };
	if (depth != nullptr && clears_depth) {
		s->clear_depth = (float)clear_depth;
		s->clears_depth = true;
	}
	if (stencil != nullptr && clears_stencil) {
		s->clear_stencil = clear_stencil;
		s->clears_stencil = true;
	}
}

extern "C" void
rwm_render_set_pipeline(struct rwm_render_state *s, void *object, struct rwa_render *render, unsigned stream_mask,
			unsigned stream_count, bool writes_depth)
{
	s->pipeline = object;
	s->render = render;
	s->stream_mask = stream_mask;
	s->stream_count = stream_count;
	s->writes_depth = writes_depth;
}

/*
 * One table, two meanings, and the offset kept. Metal keeps vertex streams and the vertex stage's [[buffer(n)]]
 * arguments in the SAME table, and which an index is depends on the pipeline's vertex descriptor -- which may not be
 * set yet. So the offset is recorded here and judged at the draw (src/metal/draw.cpp, with the pipeline's own mask).
 * The fragment table likewise: an argument buffer is bound through it with an offset of its own.
 */
extern "C" void
rwm_render_set_vertex_buffer(struct rwm_render_state *s, size_t index, void *object, struct rwa_buffer *buffer,
			     size_t offset)
{
	if (index >= RWM_MAX_BUFFERS) {
		rwm_print("vertex buffer %lu is past this driver's limit of %u\n", (unsigned long)index, RWM_MAX_BUFFERS);
		s->complained = true;
		return;
	}
	s->vertex[index] = rwm_bound_buffer{ object, buffer, offset };
	if (index + 1 > s->vertex_count) {
		s->vertex_count = (unsigned)index + 1;
	}
}

extern "C" void
rwm_render_set_fragment_buffer(struct rwm_render_state *s, size_t index, void *object, struct rwa_buffer *buffer,
			       size_t offset)
{
	if (index >= RWM_MAX_BUFFERS) {
		rwm_print("fragment buffer %lu is past this driver's limit of %u\n", (unsigned long)index, RWM_MAX_BUFFERS);
		s->complained = true;
		return;
	}
	s->fragment[index] = rwm_bound_buffer{ object, buffer, offset };
	if (index + 1 > s->fragment_count) {
		s->fragment_count = (unsigned)index + 1;
	}
}

/* the offset alone, which an application changes between draws out of one buffer: the same table entry */
extern "C" void
rwm_render_set_vertex_offset(struct rwm_render_state *s, size_t index, size_t offset)
{
	if (index >= RWM_MAX_BUFFERS) {
		return;
	}
	s->vertex[index].offset = offset;
	if (index + 1 > s->vertex_count) {
		s->vertex_count = (unsigned)index + 1;
	}
}

extern "C" void
rwm_render_set_fragment_offset(struct rwm_render_state *s, size_t index, size_t offset)
{
	if (index >= RWM_MAX_BUFFERS) {
		return;
	}
	s->fragment[index].offset = offset;
	if (index + 1 > s->fragment_count) {
		s->fragment_count = (unsigned)index + 1;
	}
}

extern "C" void
rwm_render_set_texture(struct rwm_render_state *s, size_t index, void *object, struct rwm_texture *core)
{
	if (index >= RWM_MAX_BUFFERS) {
		return;
	}
	s->textures[index] = rwm_bound_texture{ object, core };
	if (index + 1 > s->texture_count) {
		s->texture_count = (unsigned)index + 1;
	}
}

extern "C" void
rwm_render_set_sampler(struct rwm_render_state *s, size_t index, void *object, struct rwa_sampler *sampler)
{
	if (index >= RWM_MAX_BUFFERS) {
		return;
	}
	s->sampler_object[index] = object;
	s->samplers[index] = sampler;
	if (index + 1 > s->sampler_count) {
		s->sampler_count = (unsigned)index + 1;
	}
}

/* recorded like the pipeline: what a draw tests against is what was bound when it was encoded; NULL is Metal's "no
 * test, no write", and the library draws with its own zeroed state, which is the same thing */
extern "C" void
rwm_render_set_depth_stencil(struct rwm_render_state *s, void *object, struct rwa_depth_stencil *ds)
{
	s->depth_stencil_object = object;
	s->depth_stencil = ds;
}

/* Metal keeps the stencil reference on the encoder and not in the state object */
extern "C" void
rwm_render_set_stencil_reference(struct rwm_render_state *s, unsigned front, unsigned back)
{
	s->stencil_reference[0] = front;
	s->stencil_reference[1] = back;
}

/*
 * The viewport: its depth range is the only thing in Metal that moves a quad in z through this driver, and its
 * rectangle has been kept since 27 October (it was ignored: a layer meant for one part of the target was drawn over
 * the whole of it) -- when it is not the whole of the first attachment.
 */
extern "C" void
rwm_render_set_viewport(struct rwm_render_state *s, const double v[6])
{
	const struct rwm_texture *first = s->color_count > 0 ? s->color[0].core : s->depth.core;
	const unsigned long w = first != nullptr ? rwm_texture_shape(first)->width : 0;
	const unsigned long h = first != nullptr ? rwm_texture_shape(first)->height : 0;

	s->has_viewport = v[0] != 0.0 || v[1] != 0.0 || (unsigned long)v[2] != w || (unsigned long)v[3] != h;
	for (unsigned i = 0; i < 4; i++) {
		s->viewport[i] = (float)v[i];
	}
	s->depth_range[0] = (float)v[4];
	s->depth_range[1] = (float)v[5];
	s->has_depth_range = true;
}

/* -setScissorRect:, ignored until 27 October: CoreAnimation draws a dirty region with every layer that touches it
 * under a scissor of that region, and without it those layers covered the frame beyond the region */
extern "C" void
rwm_render_set_scissor(struct rwm_render_state *s, unsigned long x, unsigned long y, unsigned long width,
		       unsigned long height)
{
	s->has_scissor = true;
	s->scissor[0] = (unsigned)x;
	s->scissor[1] = (unsigned)y;
	s->scissor[2] = (unsigned)width;
	s->scissor[3] = (unsigned)height;
}

extern "C" bool
rwm_render_draw(struct rwm_render_state *s, const struct rwm_draw_call *call, struct rwm_work *w)
{
	if (s->pipeline == nullptr || (s->color_count == 0 && s->depth.object == nullptr)) {
		rwm_print("this draw has %s and is dropped\n",
			  s->pipeline == nullptr ? "no pipeline state" : "nothing to draw into");
		return false;
	}
	passlog_draws++;
	/* boot-arg rwm_drawonly=TEXT: only the draws whose pipeline's label has TEXT in it are drawn, the others dropped --
	 * with rwm_loadclear=1 and rwa_tint=2, what one kind of draw puts on the screen alone (15 November) */
	{
		static int only = -1;
		static char text[48];

		if (only < 0) {
			const char *at = strstr(rwm_boot_args(), "rwm_drawonly=");

			only = at != nullptr && sscanf(at + 13, "%47s", text) == 1;
		}
		if (only == 1) {
			const char *l = rwm_area_label_of(s->render);

			if (l == nullptr || strstr(l, text) == nullptr) {
				return false;
			}
		}
	}
	/* boot-arg rwm_texshot_draw=TEXT: a draw of a pipeline with TEXT in its label marks its command buffer, whose targets
	 * are then taken by rwm_texshot after the GPU has drawn them (src/metal/texture.cpp, 15 November) */
	{
		static int on = -1;
		static char text[48];

		if (on < 0) {
			const char *at = strstr(rwm_boot_args(), "rwm_texshot_draw=");

			on = at != nullptr && sscanf(at + 17, "%47s", text) == 1;
		}
		if (on == 1) {
			const char *l = rwm_area_label_of(s->render);

			if (l != nullptr && strstr(l, text) != nullptr) {
				rwm_texshot_marked = true;
			}
		}
	}
	/* a generic pipeline drawn with its variant for the configuration bound in buffer 0 (src/metal/pipeline.cpp) */
	struct rwa_render *special = rwm_generic_variant(s->render, s->vertex_count > 0 ? s->vertex[0].buffer : nullptr,
							  s->vertex_count > 0 ? s->vertex[0].offset : 0,
							  s->fragment_count > 0 ? s->fragment[0].buffer : nullptr,
							  s->fragment_count > 0 ? s->fragment[0].offset : 0);
	if (passlog_on()) {
		const char *label = rwm_area_label_of(s->render);
		char tex[120];
		int n = 0;

		if (label != nullptr && strncmp(label, "com.apple.coreanimation.", 24) == 0) {
			label += 24;
		}
		tex[0] = 0;
		for (unsigned i = 0; i < s->texture_count && i < 12 && n < (int)sizeof(tex) - 8; i++) {
			if (s->textures[i].core != nullptr) {
				const struct rwa_texture_shape *sh = rwm_texture_shape(s->textures[i].core);

				/* the size of what is sampled, for slots 3 and 4 (the images), so an icon can be told from a 1 x 1 */
				if (i <= 4 && sh != nullptr && (sh->width > 1 || sh->height > 1)) {
					n += snprintf(tex + n, sizeof(tex) - n, "%u=%04x:%ux%u/%u,", i, tex_id(s->textures[i].core),
						      sh->width, sh->height, sh->pixel_format);
				} else if (i <= 4) {
					n += snprintf(tex + n, sizeof(tex) - n, "%u=%04x,", i, tex_id(s->textures[i].core));
				}
			}
		}
		char vtag[16] = "";

		if (special != nullptr) {
			snprintf(vtag, sizeof(vtag), "+V%u", rwm_generic_last_variant());
		}
		/* and the fragment samplers bound, index=id, so a slot the shader samples with nothing in it shows (15 November) */
		char smp[64];
		int m = 0;

		smp[0] = 0;
		for (unsigned i = 0; i < s->sampler_count && i < 8 && m < (int)sizeof(smp) - 12; i++) {
			if (s->samplers[i] != nullptr) {
				m += snprintf(smp + m, sizeof(smp) - m, "%u=%04x,", i,
					      (unsigned)(((uintptr_t)s->samplers[i] >> 4) & 0xffff));
			}
		}
		/* and the scissor and viewport, so a draw that lands outside what is kept shows (15 November) */
		char clip[96] = "";

		if (s->has_scissor) {
			snprintf(clip, sizeof(clip), " sc:%u,%u,%ux%u", s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3]);
		}
		if (s->has_viewport) {
			size_t at = strlen(clip);

			snprintf(clip + at, sizeof(clip) - at, " vp:%d,%d,%dx%d", (int)s->viewport[0], (int)s->viewport[1],
				 (int)s->viewport[2], (int)s->viewport[3]);
		}
		/* and the depth/stencil state with the stencil reference, and whether the pass has such an attachment */
		{
			size_t at = strlen(clip);

			snprintf(clip + at, sizeof(clip) - at, " ds:%04x/%u/%c", (unsigned)(((uintptr_t)s->depth_stencil >> 4) & 0xffff),
				 s->stencil_reference[0], s->depth.core != nullptr ? 'Z' : '-');
		}
		passlog("d %s%s n%llu t:%s s:%s%s", label != nullptr ? label : "?", vtag, (unsigned long long)call->count, tex,
			smp, clip);
	}
	/*
	 * What the library carries in 32 bits and Metal hands over in 64: `index_bias` in Gallium is a signed int and
	 * instance counts unsigned, so a number that does not fit would be recorded as a DIFFERENT number -- a base
	 * vertex of 2^32 as 0. Saying so is the only answer that is not a wrong picture.
	 */
	if (call->base_vertex < INT32_MIN || call->base_vertex > INT32_MAX || call->instances > UINT32_MAX ||
	    call->base_instance > UINT32_MAX || call->count > UINT32_MAX || call->start > UINT32_MAX) {
		rwm_print("this draw does not fit the contract's 32 bits (count %lu, "
			  "start %lu, base vertex %ld, %lu instances from %lu) and is dropped "
			  "rather than wrapped\n", call->count, call->start, call->base_vertex, call->instances,
			  call->base_instance);
		return false;
	}
	/* said at the draw, because it is the pipeline that knows: Metal drops a [[depth]] write without a depth
	 * attachment, and so does this -- but a caller measuring such a shader would be left reading a clear value */
	if (s->writes_depth && s->depth.object == nullptr && !s->complained) {
		rwm_print("this pipeline's fragment shader stores to [[depth]] and "
			  "this pass has no depth attachment, so those writes go nowhere\n");
		s->complained = true;
	}

	struct rwm_draw *d = &w->draw;

	memset(w, 0, sizeof(*w));
	w->kind = RWM_WORK_DRAW;
	w->pipeline = s->pipeline;
	rwm_work_keep(w, s->pipeline);
	record_targets(s, w, false);
	rwm_work_keep(w, s->depth_stencil_object);
	rwm_work_keep(w, call->index_object);

	d->render = special != nullptr ? special : s->render;
	d->stream_mask = s->stream_mask;
	d->stream_count = s->stream_count;
	d->color_count = s->color_count;
	for (unsigned i = 0; i < s->color_count; i++) {
		d->color[i] = mesa_of(s->color[i].core);
		d->color_level[i] = s->color_level[i];
	}
	memcpy(w->clear_color, s->clear_color, sizeof(w->clear_color));
	d->clear_color = s->clears ? w->clear_color : nullptr;
	d->color_actions = s->color_actions;
	memcpy(d->color_load, s->color_load, sizeof(d->color_load));
	memcpy(d->color_store, s->color_store, sizeof(d->color_store));
	memcpy(d->color_clear, s->color_clear, sizeof(d->color_clear));
	d->depth = mesa_of(s->depth.core);
	w->clear_depth = s->clear_depth;
	w->clear_stencil = s->clear_stencil;
	d->clear_depth = s->clears_depth ? &w->clear_depth : nullptr;
	d->clear_stencil = s->clears_stencil ? &w->clear_stencil : nullptr;
	d->depth_stencil = s->depth_stencil;
	d->stencil_reference[0] = s->stencil_reference[0];
	d->stencil_reference[1] = s->stencil_reference[1];
	memcpy(w->depth_range, s->depth_range, sizeof(w->depth_range));
	memcpy(w->viewport, s->viewport, sizeof(w->viewport));
	memcpy(w->scissor, s->scissor, sizeof(w->scissor));
	d->depth_range = s->has_depth_range ? w->depth_range : nullptr;
	d->viewport = s->has_viewport ? w->viewport : nullptr;
	d->scissor = s->has_scissor ? w->scissor : nullptr;

	d->vertex_count_bound = s->vertex_count;
	for (unsigned i = 0; i < s->vertex_count; i++) {
		d->vertex[i] = s->vertex[i].buffer;
		d->vertex_offset[i] = s->vertex[i].offset;
		d->vertex_bound[i] = s->vertex[i].object != nullptr;
		rwm_work_keep(w, s->vertex[i].object);
	}
	d->fragment_count_bound = s->fragment_count;
	for (unsigned i = 0; i < s->fragment_count; i++) {
		d->fragment[i] = s->fragment[i].buffer;
		d->fragment_offset[i] = s->fragment[i].offset;
		d->fragment_bound[i] = s->fragment[i].object != nullptr;
		rwm_work_keep(w, s->fragment[i].object);
	}
	/* the textures that are another process's surface: copied in before the draw, and read in place counted in use */
	d->texture_count = s->texture_count;
	for (unsigned i = 0; i < s->texture_count; i++) {
		struct rwm_texture *core = s->textures[i].core;

		d->textures[i] = mesa_of(core);
		/* boot-arg rwm_noimg=1: a texture written by the processor (-replaceRegion:) is not bound, the slot reads
		 * zeros -- whether binding the icons' own images is what keeps their draws off the screen (15 November) */
		{
			static int noimg = -1;

			if (noimg < 0) {
				noimg = strstr(rwm_boot_args(), "rwm_noimg=1") != nullptr;
			}
			if (noimg && core != nullptr && (rwm_texture_fill_kind(core) & 2) != 0 && !(rwm_texture_fill_kind(core) & 1)) {
				d->textures[i] = nullptr;
			}
		}
		rwm_texture_note_sampled(core);
		if (core != nullptr) {
			w->copy_in[w->copy_in_count++] = core;
			if (rwm_texture_in_place(core) != 0) {
				w->sampled[w->sampled_count++] = rwm_texture_in_place(core);
			}
		}
		rwm_work_keep(w, s->textures[i].object);
	}
	/* boot-arg rwm_tdump=1: for twelve textured draws past the first 3000, what is bound at each texture slot -- the
	 * 27 release's frames sample thousands of empty 1 x 1 textures and hardly any of its images (run 49) */
	{
		static int wanted = -1;
		static unsigned long seen;
		static unsigned said;

		if (wanted < 0) {
			wanted = strstr(rwm_boot_args(), "rwm_tdump=1") != nullptr;
		}
		if (wanted && s->texture_count != 0 && ++seen > 3000 && said < 12) {
			char line[240];
			int at = snprintf(line, sizeof(line), "!!rwm tdump %u: %s, %u slots:", ++said,
					  rwm_area_label_of(s->render), s->texture_count);

			for (unsigned i = 0; i < s->texture_count && at < (int)sizeof(line) - 24; i++) {
				const struct rwm_texture *core = s->textures[i].core;
				const struct rwa_texture_shape *sh = core != nullptr ? rwm_texture_shape(core) : nullptr;

				at += snprintf(line + at, sizeof(line) - at, sh ? " %u:%ux%u/%u" : " %u:-", i,
					       sh ? sh->width : 0, sh ? sh->height : 0, sh ? sh->pixel_format : 0);
			}
			sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
		}
	}
	{
		const struct rwa_texture_shape *shapes[RWM_MAX_BUFFERS] = {};

		for (unsigned i = 0; i < s->texture_count && i < RWM_MAX_BUFFERS; i++) {
			shapes[i] = s->textures[i].core != nullptr ? rwm_texture_shape(s->textures[i].core) : nullptr;
		}
		rwm_capture_textures(s->render, shapes, s->texture_count < RWM_MAX_BUFFERS ? s->texture_count : RWM_MAX_BUFFERS);
	}
	d->sampler_count = s->sampler_count;
	for (unsigned i = 0; i < s->sampler_count; i++) {
		d->samplers[i] = s->samplers[i];
		rwm_work_keep(w, s->sampler_object[i]);
	}

	d->primitive = (unsigned)call->primitive;
	d->vertex_start = (unsigned)call->start;
	d->vertex_count = (unsigned)call->count;
	d->index_buffer = call->index_buffer;
	d->index_bound = call->index_object != nullptr;
	d->index_type = (unsigned)call->index_type;
	d->index_offset = call->index_offset;
	d->base_vertex = (int)call->base_vertex;
	d->instance_count = (unsigned)call->instances;
	d->base_instance = (unsigned)call->base_instance;
	loaded(s);
	return true;
}

/* a pass that clears and draws nothing still clears (26 October), in its place among the others */
extern "C" bool
rwm_render_end(struct rwm_render_state *s, struct rwm_work *w)
{
	if (passlog_on()) {
		passlog("e %u", passlog_draws);
		passlog_flush();
	}
	if (!s->clears && !s->clears_depth && !s->clears_stencil) {
		return false;
	}

	struct rwm_blit *b = &w->blit;

	memset(w, 0, sizeof(*w));
	w->kind = RWM_WORK_BLIT;
	record_targets(s, w, true);
	b->kind = RWM_BLIT_CLEAR;
	b->device = s->device;
	for (unsigned i = 0; i < s->color_count; i++) {
		b->colors[i] = mesa_of(s->color[i].core);
	}
	b->color_count = s->color_count;
	memcpy(w->clear_color, s->clear_color, sizeof(w->clear_color));
	b->clear_color = s->clears ? w->clear_color : nullptr;
	b->depth = mesa_of(s->depth.core);
	w->clear_depth = s->clear_depth;
	w->clear_stencil = s->clear_stencil;
	b->clear_depth = s->clears_depth ? &w->clear_depth : nullptr;
	b->clear_stencil = s->clears_stencil ? &w->clear_stencil : nullptr;
	loaded(s);
	return true;
}
