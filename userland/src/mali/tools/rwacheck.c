/*
 * rwacheck LIBRARY.metallib RECIPES.txt [PROCESS] [--draw]: the window server's CoreAnimation pipelines, as a run of the
 * phone recorded them (userland/share/rwm-recipes27.txt: the label, rwa_render_shape's non-zero words, the two
 * functions and their function constants), built through the native library's rwa_render_create on this Mac
 * (RWA_HOST=1: no GPU, the buffers are this process's memory). Each pipeline built or refused, and why; with --draw,
 * one draw of each built pipeline into a 64x64 target of its formats, laid out and "submitted" -- what the
 * descriptors need, not what they draw.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include "air_metal.h"
#include "nir_air.h"

struct recipe {
	char label[160], vertex[96], fragment[96];
	struct rwa_render_shape shape;
	struct rwa_constant vc[128], fc[128];
	unsigned nv, nf;
};

static char *
slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *p = malloc((size_t)n + 1);
	if (fread(p, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(p);
		return NULL;
	}
	p[n] = 0;
	fclose(f);
	*size = (size_t)n;
	return p;
}

/* recipes.cpp's recipe_parse, the "=K" table expanded first */
static bool
parse(const char *text, struct recipe *out)
{
	const char *space = strchr(text, ' ');
	uint32_t *w = (uint32_t *)(void *)&out->shape;

	memset(out, 0, sizeof(*out));
	if (space == NULL || (size_t)(space - text) >= sizeof(out->label))
		return false;
	memcpy(out->label, text, (size_t)(space - text));
	for (const char *at = space + 1; *at && *at != '\n' && *at != ' ';) {
		char *end;
		unsigned long index = strtoul(at, &end, 16), value;
		if (*end != '=' || index >= sizeof(out->shape) / 4)
			return false;
		value = strtoul(end + 1, &end, 16);
		w[index] = (uint32_t)value;
		if (*end != ',')
			break;
		at = end + 1;
	}
	const char *bar = strstr(space + 1, " |");
	if (bar == NULL)
		return false;
	const char *v = bar + 2, *f = strchr(v, '|'), *c1 = f ? strchr(f + 1, '|') : NULL;
	if (c1 == NULL)
		return false;
	memcpy(out->vertex, v, (size_t)(f - v));
	memcpy(out->fragment, f + 1, (size_t)(c1 - f - 1));
	for (int side = 0; side < 2; side++) {
		struct rwa_constant *c = side == 0 ? out->vc : out->fc;
		unsigned *n = side == 0 ? &out->nv : &out->nf;
		const char *at = c1 + 1;
		while (*at && *at != '|' && *at != '\n' && *n < 128) {
			char *end;
			c[*n].index = (unsigned)strtoul(at, &end, 16);
			if (*end != ':')
				return false;
			c[*n].bits = strtoull(end + 1, &end, 16);
			(*n)++;
			at = *end == ';' ? end + 1 : end;
		}
		c1 = strchr(at, '|');
		if (side == 0 && c1 == NULL)
			return false;
	}
	return true;
}

/* a child that stops in the host's compiler leaves at once, with no crash report: a hundred of them at once kept
 * this Mac from starting any program for twenty minutes (ReportCrash) */
static void
quiet_exit(int sig)
{
	_exit(128 + sig);
}

static const struct air_metallib_function *
function(const struct air_metallib *ml, const char *name)
{
	for (unsigned i = 0; i < ml->num_functions; i++)
		if (strcmp(ml->functions[i].name, name) == 0)
			return &ml->functions[i];
	return NULL;
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "rwacheck LIBRARY.metallib RECIPES.txt [PROCESS] [--draw]\n");
		return 2;
	}
	const char *want = "backboardd";
	bool do_draw = false;
	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--draw") == 0)
			do_draw = true;
		else
			want = argv[i];
	}
	setenv("RWA_HOST", "1", 1);
	size_t lsize = 0, rsize = 0;
	char *lib = slurp(argv[1], &lsize), *rec = slurp(argv[2], &rsize);
	if (lib == NULL || rec == NULL) {
		fprintf(stderr, "rwacheck: cannot read %s or %s\n", argv[1], argv[2]);
		return 1;
	}
	struct air_metallib ml;
	const char *err = NULL;
	if (!air_metallib_parse(NULL, lib, lsize, &ml, &err)) {
		fprintf(stderr, "rwacheck: %s: %s\n", argv[1], err ? err : "not a metallib");
		return 1;
	}
	char why[512];
	struct rwa_device *dev = rwa_device_open(why, sizeof(why));
	if (dev == NULL) {
		fprintf(stderr, "rwacheck: no device: %s\n", why);
		return 1;
	}
	static char shapes[64][4096];
	char proc[64] = "backboardd";
	unsigned built = 0, refused = 0, missing = 0, drawn = 0, draw_refused = 0, crashed = 0;
	for (char *line = strtok(rec, "\n"); line != NULL; line = strtok(NULL, "\n")) {
		if (line[0] == '#' || line[0] == 0)
			continue;
		if (line[0] == '[') {
			snprintf(proc, sizeof(proc), "%.*s", (int)strcspn(line + 1, "]"), line + 1);
			continue;
		}
		if (line[0] == '=') {
			unsigned k = (unsigned)strtoul(line + 1, NULL, 10);
			const char *sp = strchr(line, ' ');
			if (k < 64 && sp)
				snprintf(shapes[k], sizeof(shapes[k]), "%s", sp + 1);
			continue;
		}
		if (strcmp(proc, want) != 0)
			continue;
		char whole[8192];
		const char *at = strstr(line, " @");
		if (at != NULL) {
			unsigned k = (unsigned)strtoul(at + 2, NULL, 10);
			const char *rest = strchr(at + 2, ' ');
			snprintf(whole, sizeof(whole), "%.*s %s%s%s", (int)(at - line), line, k < 64 ? shapes[k] : "",
				 rest ? " " : "", rest ? rest + 1 : "");
		} else {
			snprintf(whole, sizeof(whole), "%s", line);
		}
		struct recipe r;
		if (!parse(whole, &r)) {
			printf("? %s: not a recipe\n", line);
			continue;
		}
		const struct air_metallib_function *vf = function(&ml, r.vertex), *ff = function(&ml, r.fragment);
		const char *shortl = strncmp(r.label, "com.apple.coreanimation.", 24) == 0 ? r.label + 24 : r.label;
		if (vf == NULL || ff == NULL) {
			printf("- %s: no %s or %s in the library\n", shortl, r.vertex, r.fragment);
			missing++;
			continue;
		}
		/* each pipeline in a child: the host's Mesa has not got the phone's compiler patches
		 * (tools/mesa/panfrost-v7-b8csel.patch), and an assertion there is the host's, not this library's */
		fflush(stdout);
		pid_t child = fork();
		if (child > 0) {
			int st = 0;
			waitpid(child, &st, 0);
			if (WIFSIGNALED(st) || WEXITSTATUS(st) >= 128) {
				printf("! %s (%s, %s): stopped (signal %d)\n", shortl, r.vertex, r.fragment,
				       WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st) - 128);
				crashed++;
			} else if (WEXITSTATUS(st) == 0) {
				built++;
				drawn += do_draw;
			} else if (WEXITSTATUS(st) == 1) {
				refused++;
			} else {
				built++;
				draw_refused++;
			}
			continue;
		}
		signal(SIGABRT, quiet_exit);
		signal(SIGSEGV, quiet_exit);
		signal(SIGBUS, quiet_exit);
		signal(SIGTRAP, quiet_exit);
		signal(SIGILL, quiet_exit);
		struct rwa_render_info info;
		rwa_render_constants(r.vc, r.nv, r.fc, r.nf);
		struct rwa_render *p = rwa_render_create(dev, vf->bitcode, vf->bitcode_size, NULL, ff->bitcode,
							 ff->bitcode_size, NULL, &r.shape, &info, why, sizeof(why));
		if (p == NULL) {
			printf("x %s (%s, %s): %s\n", shortl, r.vertex, r.fragment, why);
			fflush(stdout);
			_exit(1);
		}
		printf("+ %s: vs %u buffers, fs %u buffers %u textures %u samplers, attrs %u\n", shortl,
		       info.vertex_buffers, info.fragment_buffers, info.fragment_textures, info.fragment_samplers,
		       info.vertex_attribute_count);
		if (!do_draw) {
			fflush(stdout);
			_exit(0);
		}
		/* one draw: targets of the shape's formats, buffers of 4 KB at every index, textures 16x16 */
		struct rwa_texture *color[8] = { 0 };
		struct rwa_buffer *bufs[32] = { 0 };
		struct rwa_texture *texs[16] = { 0 };
		struct rwa_vertex_stream streams[31] = { 0 };
		bool fine = true;
		for (unsigned i = 0; i < r.shape.color_count && fine; i++) {
			struct rwa_texture_shape ts = { r.shape.color_format[i], 2, 64, 64, 1, 1, 1, 1, 5 };
			color[i] = rwa_texture_create(dev, &ts, why, sizeof(why));
			fine = color[i] != NULL;
		}
		for (unsigned i = 0; i < 32 && fine; i++)
			bufs[i] = rwa_buffer_create(dev, 4096, why, sizeof(why));
		for (unsigned i = 0; i < 31; i++)
			streams[i].buffer = bufs[i];
		for (unsigned i = 0; i < 16 && fine; i++) {
			struct rwa_texture_shape ts = { 80, 2, 16, 16, 1, 1, 1, 1, 1 };
			texs[i] = rwa_texture_create(dev, &ts, why, sizeof(why));
		}
		struct rwa_draw d = { 0 };
		d.color = color;
		d.color_count = r.shape.color_count;
		d.vertex.buffers = bufs;
		d.vertex.buffer_count = 31;
		d.fragment.buffers = bufs;
		d.fragment.buffer_count = 31;
		d.fragment.textures = texs;
		d.fragment.texture_count = 16;
		d.primitive = 4;
		d.vertex_count = 4;
		d.vertex_streams = streams;
		d.vertex_stream_count = info.vertex_streams;   /* what the plug-in covers (air.cpp, render.cpp) */
		int code = 0;
		if (!(fine && rwa_render_draw(p, &d, why, sizeof(why)) == 0 && rwa_finish(dev, why, sizeof(why)) == 0)) {
			printf("  draw refused: %s\n", why);
			code = 2;
		}
		for (unsigned i = 0; i < 8; i++)
			rwa_texture_destroy(color[i]);
		for (unsigned i = 0; i < 16; i++)
			rwa_texture_destroy(texs[i]);
		for (unsigned i = 0; i < 32; i++)
			rwa_buffer_destroy(bufs[i]);
		rwa_render_destroy(p);
		fflush(stdout);
		_exit(code);
	}
	printf("%s: %u pipelines built, %u refused, %u stopped the host's compiler, %u without their functions", want, built,
	       refused, crashed, missing);
	if (do_draw)
		printf("; %u drawn, %u draws refused", drawn, draw_refused);
	printf("\n");
	return refused != 0 || draw_refused != 0;
}
