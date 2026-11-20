/*
 * A command buffer's work, run when it is committed -- Metal's rule, kept: the recorded commands run here and not
 * when they were encoded, in the order they were encoded.
 */
#include "rwm.h"
#include <sys/sysctl.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

/* the first, second, fourth... time a site says it, with the count after the first (the plug-in's RW_AGAIN) */
bool
again(unsigned long &times)
{
	return (++times & (times - 1)) == 0;
}

void
say_again(unsigned long &times, const char *what, const char *why)
{
	if (again(times)) {
		rwm_print("%s: %s\n", what, why);
		if (times > 1) {
			rwm_print("  ... and that is the %lu'th time\n", times);
		}
	}
}

/* the targets of a command that are over a surface, once each */
void
note_drawn(const struct rwm_work *w, struct rwm_texture **drawn, unsigned &ndrawn, unsigned room)
{
	for (unsigned i = 0; i < w->target_count; i++) {
		struct rwm_texture *t = w->targets[i];
		unsigned k;

		if (t == nullptr || !rwm_texture_has_surface(t)) {
			continue;
		}
		for (k = 0; k < ndrawn && drawn[k] != t; k++) {
		}
		if (k == ndrawn && ndrawn < room) {
			drawn[ndrawn++] = t;
		}
	}
}

} // namespace

/* a texture over another process's surface brought up to date, counted in rwm_perf (also -[HakiosMetalTexture
 * hakiosCopyInNow]) */
extern "C" void
rwm_copy_in(struct rwm_texture *texture)
{
	static unsigned long times;
	char why[256] = { 0 };
	size_t bytes = 0;
	const uint64_t began = rwm_ns();

	if (rwm_texture_copy_in(texture, &bytes, why, sizeof(why)) != 0) {
		say_again(times, "copy-in: the surface could not be written into its texture", why);
		/* and into the host's kept lines, where a run's log has it (it went to stdout only, and nobody saw that the
		 * 27 release's icons were refused here) */
		static unsigned told;

		if (told++ < 6) {
			char line[300];

			snprintf(line, sizeof(line), "!!rwm copy-in refused: %s", why);
			sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
		}
	}
	if (bytes != 0) {
		rwm_perf.copyins++;
		rwm_perf.copyin_bytes += bytes;
		rwm_perf.copyin_ns += rwm_ns() - began;
	}
}

/* at -commit with the driver's worker: the surfaces the works sample copied in as they are now */
extern "C" void
rwm_works_copy_in(struct rwm_work *const *works, unsigned count)
{
	for (unsigned n = 0; n < count; n++) {
		for (unsigned i = 0; i < works[n]->copy_in_count; i++) {
			rwm_copy_in(works[n]->copy_in[i]);
		}
	}
}

/* rwm_drawshot: the draw's own first colour attachment, read back through the library (sized as TARGET) */
static uint8_t *
draw_target_bytes(const struct rwm_work *w, struct rwm_texture *target, size_t *bytes)
{
	const struct rwa_texture_shape *sh = rwm_texture_shape(target);
	const struct rwm_air *air = rwm_air();
	char why[128];

	if (w->draw.color_count == 0 || w->draw.color[0] == nullptr || sh == nullptr || air->texture_read == nullptr) {
		return nullptr;
	}
	const size_t bpp = sh->pixel_format == 115 ? 8 : 4;
	unsigned o[3] = { 0, 0, 0 }, z[3] = { sh->width, sh->height, 1 };
	uint8_t *p = (uint8_t *)malloc((size_t)sh->width * sh->height * bpp);

	if (p == nullptr) {
		return nullptr;
	}
	if (air->texture_read(w->draw.color[0], 0, 0, o, z, p, (size_t)sh->width * bpp, why, sizeof(why)) != 0) {
		free(p);
		return nullptr;
	}
	*bytes = (size_t)sh->width * sh->height * bpp;
	return p;
}

extern "C" int
rwm_run_works(struct rwa_device *device, struct rwm_work *const *works, unsigned count, bool copy, bool finish,
	      uint64_t *seq, uint64_t *waited_ns, char *why, size_t why_size)
{
	struct rwm_texture *drawn[64];
	unsigned ndrawn = 0;
	/* boot-arg rwm_slowcb=MS: a command buffer that took longer, said (kept, the first 60) with where the time went --
	 * runs 598 to 602: buffers of 450 to 970 ms while applications opened, the GPU waited for 0.2 ms of them, no
	 * pipeline built, nothing read from the eMMC that could explain it */
	static long slow_ms = -2;
	if (slow_ms == -2) {
		const char *at = strstr(rwm_boot_args(), "rwm_slowcb=");
		slow_ms = at != nullptr ? atol(at + 11) : -1;
	}
	const uint64_t t_start = rwm_ns();
	uint64_t t_kind[4] = { 0, 0, 0, 0 }, t_copyin = 0, t_draw_max = 0;   /* blit, draw, dispatch, other */
	unsigned n_kind[4] = { 0, 0, 0, 0 }, n_copyin = 0;
	const char *slowest = nullptr;

	*waited_ns = 0;
	for (unsigned n = 0; n < count; n++) {
		struct rwm_work *w = works[n];
		const uint64_t t0 = rwm_ns();

		for (unsigned i = 0; copy && i < w->copy_in_count; i++) {
			rwm_copy_in(w->copy_in[i]);
			n_copyin++;
		}
		const uint64_t t1 = rwm_ns();
		t_copyin += t1 - t0;
		struct work_timer {
			uint64_t t1, *kind, *draw_max;
			unsigned *nk;
			const char **slowest;
			const struct rwm_work *w;
			~work_timer()
			{
				const uint64_t d = rwm_ns() - t1;
				*kind += d;
				(*nk)++;
				if (w->kind == RWM_WORK_DRAW && d > *draw_max) {
					*draw_max = d;
					*slowest = rwm_area_label_of(w->draw.render);
				}
			}
		} timer = { t1, &t_kind[w->kind == RWM_WORK_BLIT ? 0 : w->kind == RWM_WORK_DRAW ? 1 : w->kind == RWM_WORK_DISPATCH ? 2 : 3],
			    &t_draw_max, &n_kind[w->kind == RWM_WORK_BLIT ? 0 : w->kind == RWM_WORK_DRAW ? 1 : w->kind == RWM_WORK_DISPATCH ? 2 : 3],
			    &slowest, w };
		(void)timer;
		switch (w->kind) {
		case RWM_WORK_BLIT:
			if (rwm_run_blit(&w->blit, why, why_size) != 0) {
				static unsigned long times;

				say_again(times, "a copy skipped", why);
			} else {
				note_drawn(w, drawn, ndrawn, sizeof(drawn) / sizeof(drawn[0]));
			}
			why[0] = '\0';
			break;
		case RWM_WORK_DRAW: {
			int failed;

			rwm_perf.draws++;
			/* boot-arg rwm_drawshot=TEXT: the first four draws past 100 s whose pipeline's label has TEXT in it, each
			 * with the GPU finished before and after it, and how many bytes of its first target it changed -- whether
			 * a draw writes anything at all (15 November: the icons' draws, and nothing of them in the surface) */
			static int dshot = -1;
			static char dtext[48];
			static unsigned dshots;
			static uint64_t dfirst;
			uint8_t *before = nullptr;
			size_t bbytes = 0;
			struct rwm_texture *dtarget = w->target_count != 0 ? w->targets[0] : nullptr;
			const char *dlabel = rwm_area_label_of(w->draw.render);

			if (dshot < 0) {
				const char *at = strstr(rwm_boot_args(), "rwm_drawshot=");

				dshot = at != nullptr && sscanf(at + 13, "%47s", dtext) == 1;
				/* TEXT,TEXT2: two of the first, then two of the second */
				dfirst = rwm_ns();
			}
			char dwant[48];
			const char *comma = strchr(dtext, ',');

			strlcpy(dwant, comma != nullptr && dshots >= 2 ? comma + 1 : dtext, sizeof(dwant));
			if (comma != nullptr && dshots < 2 && strchr(dwant, ',') != nullptr) {
				*strchr(dwant, ',') = 0;
			}
			const bool dthis = dshot == 1 && dshots < 4 && dtarget != nullptr && dlabel != nullptr &&
					   strstr(dlabel, dwant) != nullptr && rwm_ns() - dfirst > 100000000000ull;

			if (dthis) {
				uint64_t s0 = 0;
				char r0[128];

				rwm_submit(device, true, &s0, r0, sizeof(r0));
				before = draw_target_bytes(w, dtarget, &bbytes);
			}
			failed = rwm_run_draw(&w->draw, why, why_size);
			/* boot-arg rwm_drawfail=1: the GPU finished after every draw, and a draw whose job chain the Mali failed
			 * (kern.hakios.mali_failed went up) named -- 20 to 40 % of the chains fail with slot status 0x51 since
			 * the 27 release's home screen draws (runs 199 to 222), and which pipeline's shader it is is the question
			 * (16 November). Slow: every draw becomes a pass of its own. */
			{
				static int dfail = -1;
				static unsigned dfails;

				if (dfail < 0) {
					dfail = strstr(rwm_boot_args(), "rwm_drawfail=1") != nullptr;
				}
				if (dfail == 1 && dfails < 40) {
					uint32_t before_failed = 0, after_failed = 0;
					size_t sz = sizeof(before_failed);
					uint64_t s1 = 0;
					char r1[128];

					sysctlbyname("kern.hakios.mali_failed", &before_failed, &sz, NULL, 0);
					rwm_submit(device, true, &s1, r1, sizeof(r1));
					sz = sizeof(after_failed);
					sysctlbyname("kern.hakios.mali_failed", &after_failed, &sz, NULL, 0);
					if (after_failed != before_failed) {
						char line[240];
						const char *lab = rwm_area_label_of(w->draw.render);

						dfails++;
						snprintf(line, sizeof(line), "!!rwm drawfail %u: %s (render %p, %u vertices, %u instances): "
							 "the Mali failed %u chain(s)", dfails, lab != nullptr ? lab : "?",
							 (void *)w->draw.render, w->draw.vertex_count, w->draw.instance_count,
							 after_failed - before_failed);
						sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
					}
				}
			}
			if (dthis) {
				uint64_t s0 = 0;
				char r0[128], line[220];
				size_t abytes = 0;
				uint8_t *after;

				rwm_submit(device, true, &s0, r0, sizeof(r0));
				after = draw_target_bytes(w, dtarget, &abytes);
				unsigned long changed = 0, first = ~0ul;

				for (size_t i = 0; before != nullptr && after != nullptr && i < bbytes && i < abytes; i++) {
					if (before[i] != after[i]) {
						changed++;
						first = first == ~0ul ? i : first;
					}
				}
				const struct rwa_texture_shape *sh = rwm_texture_shape(dtarget);

				dshots++;
				snprintf(line, sizeof(line), "!!rwm drawshot %u %s: %lu bytes of %zu changed in %ux%u/%u (first at %ld)%s%s",
					 dshots, dlabel, changed, abytes, sh ? sh->width : 0, sh ? sh->height : 0,
					 sh ? sh->pixel_format : 0, first == ~0ul ? -1L : (long)first,
					 before == nullptr ? ", not read before" : "", after == nullptr ? ", not read after" : "");
				sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
				free(before);
				free(after);
			}
			if (failed == -2) {
				rwm_print("the draw failed: %s\n", why);
			}
			/* a failed draw's reason, the first eight different ones a process, into the host's kept lines: a draw
			 * that fails ends its command buffer, and the 27 release's frames lost everything after the first one */
			if (failed != 0) {
				static char seen[8][48];
				static unsigned seen_count;
				unsigned k;

				for (k = 0; k < seen_count && strncmp(seen[k], why, sizeof(seen[0]) - 1) != 0; k++) {
				}
				if (k == seen_count && seen_count < 8) {
					char line[240];

					strlcpy(seen[seen_count++], why, sizeof(seen[0]));
					snprintf(line, sizeof(line), "!!rwm draw failed (%d) in %s: %s", failed,
						 rwm_area_label_of(w->draw.render), why);
					sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
				}
			}
			if (failed != 0) {
				/*
				 * A buffer the shader is declared to read and the window server did not bind: QuartzCore's
				 * specialized uber shader keeps its configuration buffer (0) among its arguments although the
				 * constants decide it is never read. The draw is skipped and the rest of the frame drawn --
				 * failing the whole command buffer for it left every frame black (24 October).
				 */
				if (strstr(why, "reads buffer") != nullptr && strstr(why, "were bound") != nullptr) {
					static unsigned long times;

					say_again(times, "a draw skipped, the frame kept", why);
					why[0] = '\0';
					break;
				}
				return RWM_RUN_DRAW_FAILED;
			}
			note_drawn(w, drawn, ndrawn, sizeof(drawn) / sizeof(drawn[0]));
			break;
		}
		case RWM_WORK_DISPATCH:
			if (rwm_run_dispatch(&w->dispatch, why, why_size) != 0) {
				rwm_print("the dispatch failed: %s\n", why);
				return RWM_RUN_DISPATCH_FAILED;
			}
			break;
		}
	}

	/*
	 * Sent off and NOT waited for when the kernel queues the Mali's jobs: the window server goes on to its next frame
	 * while the GPU draws this one; the display waits for the surface's last job before it shows it, the completed
	 * handlers and -waitUntilCompleted for the buffer's. A surface drawn by the copy-back road still needs the frame
	 * now, and waits.
	 */
	const uint64_t began = rwm_ns();

	const bool shots = rwm_texshot_sizes();

	if (rwm_submit(device, ndrawn != 0 || finish || shots, seq, why, why_size) != 0) {
		rwm_print("THE GPU DID NOT FINISH the command buffer: %s\n", why);
	}
	*waited_ns = rwm_ns() - began;
	for (unsigned n = 0; shots && n < count; n++) {
		for (unsigned i = 0; i < works[n]->target_count; i++) {
			rwm_texshot_after(works[n]->targets[i]);
		}
	}
	rwm_texshot_marked = false;
	const uint64_t t_back0 = rwm_ns();
	for (unsigned k = 0; k < ndrawn; k++) {
		static unsigned long times;
		char reason[256] = { 0 };

		if (rwm_texture_sync_surface(drawn[k], reason, sizeof(reason)) != 0) {
			say_again(times, "copy-back", reason);
		}
	}
	const uint64_t t_end = rwm_ns();
	static unsigned slow_said;
	if (slow_ms >= 0 && t_end - t_start > (uint64_t)slow_ms * 1000000ull && slow_said < 60) {
		char line[400];

		slow_said++;
		snprintf(line, sizeof(line), "!!rwm slowcb %.0f ms: copy-in %.1f (%u), blits %.1f (%u), draws %.1f (%u; slowest %.1f %s), "
			 "dispatches %.1f (%u), other %.1f, submit+wait %.1f, copy-back %.1f (%u)", (t_end - t_start) / 1e6,
			 t_copyin / 1e6, n_copyin, t_kind[0] / 1e6, n_kind[0], t_kind[1] / 1e6, n_kind[1], t_draw_max / 1e6,
			 slowest != nullptr ? slowest + (strncmp(slowest, "com.apple.coreanimation.", 24) == 0 ? 24 : 0) : "?",
			 t_kind[2] / 1e6, n_kind[2], t_kind[3] / 1e6, (t_back0 - began) / 1e6, (t_end - t_back0) / 1e6, ndrawn);
		sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
	}
	return 0;
}
