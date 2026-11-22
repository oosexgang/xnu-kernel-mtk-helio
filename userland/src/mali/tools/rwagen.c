/*
 * rwagen LIBRARY.metallib LOG VARIANTS: every variant configuration of a generic pipeline (userland/share/
 * rwm-variants27.txt, "LABEL vHEX fHEX") built on this Mac through the native library (RWA_HOST=1) the way the plug-in
 * builds it on the phone (src/metal/pipeline.cpp, variant_main): the generic pipeline's constants, the configuration
 * folded in -- 4 bytes of the vertex stage's buffer 0 for fixed_vert_*, 49 of the fragment stage's --, its functions
 * out of LIBRARY and its shape, as a run with boot-arg rwm_capture_gen=1 said them ("!!gen LABEL ..." in LOG); and
 * first each generic pipeline's BASE, as pipeline.cpp builds it at start (without_generic: constant 0, fc_generic,
 * made 0; no configuration) -- 34 stages run 594 still compiled on the phone while it started. With
 * RWA_CACHE_OUT=FILE every stage compiled goes into the stage cache (userland/tools/rwacache.sh). Built by mk-rwa-host.sh.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "air_metal.h"
#include "nir_air.h"
#include "rwa_priv.h"

#ifndef MAX2
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#endif

struct gen {
	char label[96], vname[96], fname[96];
	struct rwa_constant vc[128], fc[128];
	unsigned nv, nf;
	unsigned char shape[4096];
	size_t shape_size;
};

static struct gen gens[16];
static unsigned ngens;

static unsigned char
b64v(char c)
{
	if (c >= 'A' && c <= 'Z')
		return (unsigned char)(c - 'A');
	if (c >= 'a' && c <= 'z')
		return (unsigned char)(c - 'a' + 26);
	if (c >= '0' && c <= '9')
		return (unsigned char)(c - '0' + 52);
	return c == '+' ? 62 : 63;
}

static size_t
b64(const char *s, unsigned char *out, size_t room)
{
	size_t n = 0;
	unsigned v = 0, bits = 0;

	for (; *s && *s != '\n' && *s != '='; s++) {
		v = (v << 6) | b64v(*s);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n < room)
				out[n++] = (unsigned char)(v >> bits);
		}
	}
	return n;
}

static struct gen *
gen_of(const char *label, bool make)
{
	for (unsigned i = 0; i < ngens; i++)
		if (strcmp(gens[i].label, label) == 0)
			return &gens[i];
	if (!make || ngens == 16)
		return NULL;
	snprintf(gens[ngens].label, sizeof(gens[ngens].label), "%s", label);
	return &gens[ngens++];
}

static const struct air_metallib_function *
function(const struct air_metallib *ml, const char *name)
{
	for (unsigned i = 0; i < ml->num_functions; i++)
		if (strcmp(ml->functions[i].name, name) == 0)
			return &ml->functions[i];
	return NULL;
}

static int
hex(const char *h, unsigned char *out, unsigned room)
{
	unsigned n = 0, byte;
	while (h[0] && h[1] && h[0] != ' ' && h[0] != '\n' && n < room && sscanf(h, "%2x", &byte) == 1) {
		out[n++] = (unsigned char)byte;
		h += 2;
	}
	return (int)n;
}

int
main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "rwagen LIBRARY.metallib LOG VARIANTS\n");
		return 2;
	}
	setenv("RWA_HOST", "1", 1);
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL) {
		perror(argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *lib = malloc((size_t)n);
	if (lib == NULL || fread(lib, 1, (size_t)n, f) != (size_t)n)
		return 1;
	fclose(f);
	struct air_metallib ml;
	const char *err = NULL;
	if (!air_metallib_parse(NULL, lib, (size_t)n, &ml, &err)) {
		fprintf(stderr, "rwagen: %s\n", err ? err : "not a metallib");
		return 1;
	}

	/* the generic pipelines the run said */
	f = fopen(argv[2], "r");
	if (f == NULL) {
		perror(argv[2]);
		return 1;
	}
	char line[8192];
	while (fgets(line, sizeof(line), f)) {
		/* "!!gen ..." as said, "gen ..." as the log has it (the host's copy drops the marks) */
		/* "gen LABEL ..." and the shape's "cap gen-LABEL-shape K: ..." (say_bytes), "!!" before each as said */
		const char *l = strstr(line, "cap gen-");
		if (l != NULL) {
			l += 4;
		} else {
			l = strstr(line, "gen ");
			while (l != NULL && l[4] != 'P')
				l = strstr(l + 1, "gen ");
			if (l == NULL || (l != line && l[-1] != ' ' && l[-1] != '!'))
				continue;
			l += 4;
		}
		char label[96], what[32];
		if (sscanf(l, "%95s %31s", label, what) != 2)
			continue;
		if (strncmp(label, "gen-", 4) == 0) {
			/* "!!gen gen-LABEL-shape K: BASE64" (say_bytes) */
			char *dash = strstr(label, "-shape");
			if (dash == NULL)
				continue;
			*dash = 0;
			struct gen *g = gen_of(label + 4, true);
			const char *colon = strchr(l, ':');
			unsigned part = (unsigned)atoi(what);   /* "K:", 120 bytes a part (say_bytes); a line may come twice */
			if (g && colon && part * 120 < sizeof(g->shape)) {
				size_t got = b64(colon + 2, g->shape + part * 120, sizeof(g->shape) - part * 120);
				g->shape_size = MAX2(g->shape_size, part * 120 + got);
			}
			continue;
		}
		struct gen *g = gen_of(label, true);
		if (g == NULL)
			continue;
		unsigned idx;
		unsigned long long bits;
		if (strcmp(what, "functions") == 0) {
			sscanf(l, "%*s %*s %95s %*s %*s %95s", g->vname, g->fname);
		} else if (strcmp(what, "vconst") == 0 && sscanf(l, "%*s %*s %u %llx", &idx, &bits) == 2 && g->nv < 128) {
			bool seen = false;
			for (unsigned k = 0; k < g->nv; k++)
				seen |= g->vc[k].index == idx;
			if (!seen) {
				g->vc[g->nv].index = idx;
				g->vc[g->nv++].bits = bits;
			}
		} else if (strcmp(what, "fconst") == 0 && sscanf(l, "%*s %*s %u %llx", &idx, &bits) == 2 && g->nf < 128) {
			bool seen = false;
			for (unsigned k = 0; k < g->nf; k++)
				seen |= g->fc[k].index == idx;
			if (!seen) {
				g->fc[g->nf].index = idx;
				g->fc[g->nf++].bits = bits;
			}
		}
	}
	fclose(f);
	if (ngens == 0) {
		fprintf(stderr, "rwagen: no \"!!gen\" lines in %s (a run with boot-arg rwm_capture_gen=1)\n", argv[2]);
		return 1;
	}

	char why[512];
	struct rwa_device *dev = rwa_device_open(why, sizeof(why));
	if (dev == NULL) {
		fprintf(stderr, "rwagen: %s\n", why);
		return 1;
	}
	/* the whole list read before the first fork: a child that leaves through exit() closes the parent's stream with
	 * it, and the descriptor's offset the two share goes back to where the stream had buffered -- the list read again
	 * and again (a night's 100 000 stages) */
	f = fopen(argv[3], "r");
	if (f == NULL) {
		perror(argv[3]);
		return 1;
	}
	static char lines[1024][384];
	unsigned nlines = 0;
	while (nlines < 1024 && fgets(lines[nlines], sizeof(lines[0]), f))
		nlines++;
	fclose(f);
	fflush(NULL);
	unsigned built = 0, refused = 0, crashed = 0, unknown = 0, bases = 0;
	for (unsigned gi = 0; gi < ngens; gi++) {
		struct gen *g = &gens[gi];
		const struct air_metallib_function *vf = function(&ml, g->vname), *ff = function(&ml, g->fname);
		if (ff == NULL || g->shape_size < sizeof(struct rwa_render_shape))
			continue;
		pid_t pid = fork();
		if (pid == 0) {
			struct rwa_render_shape sh;
			struct rwa_constant vc[128], fc[128];
			memcpy(&sh, g->shape, sizeof(sh));
			memcpy(vc, g->vc, sizeof(vc));
			memcpy(fc, g->fc, sizeof(fc));
			for (unsigned k = 0; k < g->nv; k++)
				if (vc[k].index == 0)
					vc[k].bits = 0;
			for (unsigned k = 0; k < g->nf; k++)
				if (fc[k].index == 0)
					fc[k].bits = 0;
			rwa_render_constants(vc, g->nv, fc, g->nf);
			rwa_render_config(NULL, 0, NULL, 0);
			struct rwa_render_info info;
			struct rwa_render *r = rwa_render_create(dev, vf ? vf->bitcode : NULL, vf ? vf->bitcode_size : 0, NULL,
								 ff->bitcode, ff->bitcode_size, NULL, &sh, &info, why,
								 sizeof(why));
			_exit(r ? 0 : 1);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
			bases++;
	}
	for (unsigned li = 0; li < nlines; li++) {
		const char *line = lines[li];
		char label[96], vh[64], fh[256];
		if (line[0] == '#' || sscanf(line, "%95s %63s %255s", label, vh, fh) != 3)
			continue;
		struct gen *g = gen_of(label, false);
		const struct air_metallib_function *vf = g ? function(&ml, g->vname) : NULL, *ff = g ? function(&ml, g->fname) : NULL;
		if (g == NULL || ff == NULL || g->shape_size < sizeof(struct rwa_render_shape)) {
			unknown++;
			continue;
		}
		unsigned char vcfg[8] = { 0 }, fcfg[64] = { 0 };
		int vn = hex(vh + 1, vcfg, sizeof(vcfg)), fn = hex(fh + 1, fcfg, sizeof(fcfg));
		pid_t pid = fork();
		if (pid == 0) {
			/* each in a process of its own: the Mac's Mesa, built with assertions, may stop on one */
			struct rwa_render_shape sh;
			memcpy(&sh, g->shape, sizeof(sh));
			bool fixed_vert = strncmp(g->vname, "fixed_vert", 10) == 0;
			rwa_render_constants(g->vc, g->nv, g->fc, g->nf);
			rwa_render_config(fixed_vert ? vcfg : NULL, fixed_vert ? 4 : 0, fcfg, 49);
			(void)vn;
			(void)fn;
			struct rwa_render_info info;
			struct rwa_render *r = rwa_render_create(dev, vf ? vf->bitcode : NULL, vf ? vf->bitcode_size : 0, NULL,
								 ff->bitcode, ff->bitcode_size, NULL, &sh, &info, why,
								 sizeof(why));
			_exit(r ? 0 : 1);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
			built++;
		else if (WIFEXITED(st))
			refused++;
		else
			crashed++;
	}
	printf("rwagen: %u generic pipelines, %u bases built; %u variants built, %u refused, %u stopped the Mac's compiler, "
	       "%u of no generic pipeline said\n", ngens, bases, built, refused, crashed, unknown);
	return 0;
}
