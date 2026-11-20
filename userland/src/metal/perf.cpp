/*
 * Where a frame's time goes, as kept lines every five seconds ("rwm perf", "rwm waits", "rwm making",
 * "rwm batches", "rwm draw us"): what the window server's frame rate is made of, measured rather than
 * guessed. The counters are added to where the work is (the plug-in and the core alike); the formats of
 * the lines are kept to the character. And the boot arguments, read once.
 */
#include "rwm.h"

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/sysctl.h>

struct rwm_perf rwm_perf;

namespace {

void
say(const char *line)
{
	sysctlbyname("kern.hakios.say", NULL, NULL, (void *)line, strlen(line));
}

/* one of the compiler library's profile calls, if it is loaded and has it (not part of the contract) */
void *
profile_call(const char *name)
{
	void *lib = dlopen(rwm_air_library_path(), RTLD_NOW | RTLD_NOLOAD);

	return lib != nullptr ? dlsym(lib, name) : nullptr;
}


/*
 * Which pipelines cover the frame: per pipeline, its draws and the pixels of their scissor (or of the whole display,
 * 750 x 1624, without one), said every five seconds, the six largest -- one fragment job of 128 ms a frame on the 27
 * release's home screen, and which of its hundred draws make it was not to be told from the job alone.
 */
struct area_entry {
	const struct rwa_render *render;
	char label[72];
	uint64_t draws, pixels;
};
area_entry areas[128];
pthread_mutex_t areas_lock = PTHREAD_MUTEX_INITIALIZER;

area_entry *
area_of(const struct rwa_render *render)
{
	for (area_entry &e : areas) {
		if (e.render == render) {
			return &e;
		}
	}
	for (area_entry &e : areas) {
		if (e.render == nullptr) {
			e.render = render;
			e.label[0] = 0;
			return &e;
		}
	}
	return nullptr;
}

void
area_report(void)
{
	char line[200];
	area_entry top[6] = {};

	pthread_mutex_lock(&areas_lock);
	for (area_entry &e : areas) {
		for (unsigned k = 0; k < 6; k++) {
			if (e.pixels > top[k].pixels) {
				memmove(&top[k + 1], &top[k], (5 - k) * sizeof(top[0]));
				top[k] = e;
				break;
			}
		}
		e.draws = e.pixels = 0;
	}
	pthread_mutex_unlock(&areas_lock);
	for (unsigned k = 0; k < 6 && top[k].pixels != 0; k++) {
		snprintf(line, sizeof(line), "rwm area %u: %s, %llu draws, %llu Mpx", k + 1,
			 top[k].label[0] ? top[k].label : "(no label)", (unsigned long long)top[k].draws,
			 (unsigned long long)(top[k].pixels / 1000000));
		say(line);
	}
}
} // namespace

extern "C" const char *
rwm_boot_args(void)
{
	static char arguments[1024];
	static int read;

	if (!read) {
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
			arguments[0] = '\0';
		}
		arguments[sizeof(arguments) - 1] = '\0';
		read = 1;
	}
	return arguments;
}

extern "C" bool
rwm_boot_arg(const char *text)
{
	return strstr(rwm_boot_args(), text) != nullptr;
}

extern "C" uint64_t
rwm_thread_cpu_ns(void)
{
	thread_basic_info_data_t info;
	mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
	mach_port_t self = mach_thread_self();
	uint64_t ns = 0;

	if (thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS) {
		ns = (uint64_t)(info.user_time.seconds + info.system_time.seconds) * 1000000000ull +
		     (uint64_t)(info.user_time.microseconds + info.system_time.microseconds) * 1000ull;
	}
	mach_port_deallocate(mach_task_self(), self);
	return ns;
}

extern "C" uint64_t
rwm_ns(void)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom;
}

extern "C" void
rwm_perf_report(uint64_t now)
{
	struct rwm_perf &p = rwm_perf;
	char line[320];
	const uint64_t span = now - p.since, cbs = p.cbs ? p.cbs : 1;

	if (p.since == 0) {
		p.since = now;
		return;
	}
	if (span < 5000000000ull) {
		return;
	}
	/* boot-arg rwm_perfkeep=S: "rwm perf" and "rwm waits" into the host's kept lines for the 150 s from S s after boot
	 * (a line said only to the kernel's log is lost with the log's middle; all of them would fill the 64 KB) */
	static long keep_from = -2;

	if (keep_from == -2) {
		const char *at = strstr(rwm_boot_args(), "rwm_perfkeep=");

		keep_from = at != nullptr ? atol(at + 13) : -1;
	}
	const bool keep = keep_from >= 0 && now / 1000000000ull >= (uint64_t)keep_from &&
			  now / 1000000000ull < (uint64_t)keep_from + 150;
	const int mark = keep ? 2 : 0;

	snprintf(line, sizeof(line), "!!rwm perf %llus: %llu cb (%.1f/s), %llu draws, cpu %.1f ms/cb, "
		 "gpu wait %.1f ms/cb, worst cb %.1f ms, %llu pipelines in %.0f ms; recording %.1f, between %.1f ms/cb; "
		 "%llu copied in, %llu KB, %.1f ms",
		 span / 1000000000ull, p.cbs, p.cbs * 1e9 / span, p.draws, p.encode / 1e6 / cbs, p.wait / 1e6 / cbs,
		 p.worst / 1e6, p.pipes, p.compile / 1e6, p.recording / 1e6 / cbs, p.between / 1e6 / cbs, p.copyins,
		 p.copyin_bytes >> 10, p.copyin_ns / 1e6);
	say(line + 2 - mark);
	snprintf(line, sizeof(line), "!!rwm waits: queue full x%llu %.0f ms, until processed x%llu %.0f ms, "
		 "until completed x%llu %.0f ms; the worker %.1f ms a buffer", p.queue_waits, p.queue_ns / 1e6,
		 p.processed_waits, p.processed_ns / 1e6, p.completed_waits, p.completed_ns / 1e6,
		 p.worker_cbs ? p.worker_ns / 1e6 / p.worker_cbs : 0.0);
	{
		size_t at = strlen(line);

		snprintf(line + at, sizeof(line) - at, " (running %.1f of them)",
			 p.worker_cbs ? p.worker_cpu / 1e6 / p.worker_cbs : 0.0);
	}
	say(line + 2 - mark);
	snprintf(line, sizeof(line), "rwm making: %llu buffers, %.1f ms each from made to committed, the maker running %.1f of them",
		 p.made_cbs, p.made_cbs ? p.made_ns / 1e6 / p.made_cbs : 0.0, p.made_cbs ? p.made_cpu / 1e6 / p.made_cbs : 0.0);
	say(line);

	static void (*profile)(uint64_t *);
	static void (*submits)(uint64_t *);
	static int looked;

	if (!looked) {
		looked = 1;
		profile = (void (*)(uint64_t *))profile_call("rwa_draw_profile");
		submits = (void (*)(uint64_t *))profile_call("rwa_submit_profile");
	}
	if (submits != nullptr) {
		uint64_t sb[5] = { 0, 0, 0, 0, 0 };

		submits(sb);
		snprintf(line, sizeof(line), "rwm batches: %llu submitted, %llu us each; %llu blend shaders compiled; "
			 "%llu shader variants compiled in %llu ms", sb[0], sb[0] ? sb[1] / 1000 / sb[0] : 0, sb[2], sb[3],
			 sb[4] / 1000000);
		say(line);
	}
	if (profile != nullptr) {
		uint64_t d[7];

		profile(d);
		if (d[6] != 0) {
			snprintf(line, sizeof(line), "rwm draw us: check %llu, bindcheck %llu, state %llu, "
				 "resources %llu, draw_vbo %llu, unbind %llu (%llu draws)",
				 d[0] / 1000 / d[6], d[1] / 1000 / d[6], d[2] / 1000 / d[6], d[3] / 1000 / d[6],
				 d[4] / 1000 / d[6], d[5] / 1000 / d[6], d[6]);
			say(line);
		}
	}
	area_report();
	rwm_texture_sample_report(say);
	{
		char up[200];

		snprintf(up, sizeof(up), "rwm uploads 5s: %lu textures made (%lu small), %lu replaceRegion (%lu KB, %lu refused), "
			 "blits t>t %lu b>t %lu t>b %lu fill %lu clear %lu mips %lu", rwm_uploads[0], rwm_uploads[1],
			 rwm_uploads[2], rwm_uploads[3], rwm_uploads[4], rwm_uploads[5 + 1], rwm_uploads[5 + 3],
			 rwm_uploads[5 + 4], rwm_uploads[5 + 5], rwm_uploads[5 + 6], rwm_uploads[5 + 7]);
		say(up);
		memset(rwm_uploads, 0, sizeof(rwm_uploads));
		snprintf(up, sizeof(up), "rwm live: %ld textures (%ld MB), %lu freed in 5 s, %ld command buffers", rwm_live_textures,
			 rwm_live_texture_kb >> 10, rwm_textures_freed, rwm_live_command_buffers);
		say(up);
		rwm_textures_freed = 0;
		snprintf(up, sizeof(up), "rwm targets 5s: %lu passes into textures, %lu into surfaces (the last surface %u)",
			 rwm_target_counts[0], rwm_target_counts[1], rwm_target_last_surface);
		say(up);
		rwm_target_counts[0] = rwm_target_counts[1] = 0;
	}
	memset(&p, 0, sizeof(p));
	p.since = now;
}

extern "C" void
rwm_area_label(const struct rwa_render *render, const char *label)
{
	if (render == nullptr || label == nullptr) {
		return;
	}
	pthread_mutex_lock(&areas_lock);
	area_entry *e = area_of(render);

	if (e != nullptr) {
		const char *short_label = strncmp(label, "com.apple.coreanimation.", 24) == 0 ? label + 24 : label;

		snprintf(e->label, sizeof(e->label), "%s", short_label);
	}
	pthread_mutex_unlock(&areas_lock);
}

extern "C" void
rwm_area_draw(const struct rwa_render *render, const unsigned *scissor)
{
	pthread_mutex_lock(&areas_lock);
	area_entry *e = area_of(render);

	if (e != nullptr) {
		e->draws++;
		e->pixels += scissor != nullptr ? (uint64_t)scissor[2] * scissor[3] : 750ull * 1624ull;
	}
	pthread_mutex_unlock(&areas_lock);
}

/* a pipeline's label as rwm_area_label was told it ("" if not) */
extern "C" const char *
rwm_area_label_of(const struct rwa_render *render)
{
	for (area_entry &e : areas) {
		if (e.render == render) {
			return e.label;
		}
	}
	return "";
}

unsigned long rwm_uploads[13];

long rwm_live_textures, rwm_live_texture_kb, rwm_live_command_buffers;
unsigned long rwm_textures_freed;
unsigned long rwm_target_counts[2];
uint32_t rwm_target_last_surface;
