/*
 * rwavariant LIBRARY.metallib CAPTURE: a pipeline the phone captured (boot-arg rwm_capture=TEXT: the "cap ..." lines of
 * the host's kept lines, userland/src/metal/capture.cpp) built through the native library on this Mac (RWA_HOST=1),
 * with the configuration of the captured draw's buffer 0 folded in (rwa_render_config) as the plug-in builds a generic
 * pipeline's variant -- and each stage's resources and system values said. Built with assertions, the host's Mesa
 * stops where the phone's, built without, goes on wrong: the generic vertex stage's two [[point_size]] outputs stopped
 * at pan_collect_varyings here (run 563's capture) and slid the varyings on the phone. Built by mk-rwa-host.sh.
 *   rwavariant QuartzCore-default.metallib capture.txt [--no-config]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_metal.h"
#include "nir_air.h"
#include "rwa_priv.h"

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

/* base64 S into OUT, at most ROOM bytes: how many */
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
		fprintf(stderr, "rwavariant LIBRARY.metallib CAPTURE [--no-config]\n");
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
		fprintf(stderr, "rwavariant: %s\n", err ? err : "not a metallib");
		return 1;
	}

	static struct rwa_constant vc[128], fc[128];
	static unsigned char shape[2048], vb0[2048], fb0[2048];
	unsigned nv = 0, nf = 0;
	size_t sn = 0, vn = 0, fn = 0;
	char vname[96] = "", fname[96] = "", line[4096];
	f = fopen(argv[2], "r");
	if (f == NULL) {
		perror(argv[2]);
		return 1;
	}
	while (fgets(line, sizeof(line), f)) {
		const char *l = strncmp(line, "!!", 2) == 0 ? line + 2 : line;
		unsigned idx;
		unsigned long long bits;
		const char *colon;
		if (sscanf(l, "cap vconst %u %llx", &idx, &bits) == 2 && nv < 128) {
			vc[nv].index = idx;
			vc[nv++].bits = bits;
		} else if (sscanf(l, "cap fconst %u %llx", &idx, &bits) == 2 && nf < 128) {
			fc[nf].index = idx;
			fc[nf++].bits = bits;
		} else if (strncmp(l, "cap shapebytes ", 15) == 0 && (colon = strchr(l + 15, ':'))) {
			sn += b64(colon + 2, shape + sn, sizeof(shape) - sn);
		} else if (strncmp(l, "cap vbuf0 ", 10) == 0 && (colon = strchr(l + 10, ':'))) {
			vn += b64(colon + 2, vb0 + vn, sizeof(vb0) - vn);
		} else if (strncmp(l, "cap fbuf0 ", 10) == 0 && (colon = strchr(l + 10, ':'))) {
			fn += b64(colon + 2, fb0 + fn, sizeof(fb0) - fn);
		} else if (strncmp(l, "cap functions ", 14) == 0) {
			sscanf(l, "cap functions %95s %*s %*s %95s", vname, fname);
		}
	}
	fclose(f);
	const struct air_metallib_function *vf = function(&ml, vname), *ff = function(&ml, fname);
	if (vf == NULL || ff == NULL) {
		fprintf(stderr, "rwavariant: no %s or %s in the library\n", vname, fname);
		return 1;
	}
	struct rwa_render_shape sh;
	memset(&sh, 0, sizeof(sh));
	memcpy(&sh, shape, sn < sizeof(sh) ? sn : sizeof(sh));
	char why[512];
	struct rwa_device *dev = rwa_device_open(why, sizeof(why));
	if (dev == NULL) {
		fprintf(stderr, "rwavariant: %s\n", why);
		return 1;
	}
	printf("%s + %s: %u and %u constants, %u attachments; buffer 0: %zu and %zu bytes captured\n", vname, fname, nv, nf,
	       sh.color_count, vn, fn);
	rwa_render_constants(vc, nv, fc, nf);
	if (!(argc > 3 && strcmp(argv[3], "--no-config") == 0) && vn >= 4 && fn >= 52) {
		/* as pipeline.cpp's variants: 4 bytes of the vertex stage's, 52 of the fragment stage's, the padding cleared */
		fb0[3] = fb0[15] = fb0[19] = fb0[31] = 0;
		rwa_render_config(vb0, 4, fb0, 52);
	}
	struct rwa_render_info info;
	struct rwa_render *r = rwa_render_create(dev, vf->bitcode, vf->bitcode_size, NULL, ff->bitcode, ff->bitcode_size,
						 NULL, &sh, &info, why, sizeof(why));
	if (r == NULL) {
		printf("refused: %s\n", why);
		return 1;
	}
	for (int s = 0; s < 2; s++) {
		const struct rwa_stage *st = s ? &r->fs : &r->vs;
		printf("%s: %u resources, %u system values, %u pushed words\n", st->name, st->air.num_resources,
		       st->sysval_count, st->info.push.count);
		for (unsigned i = 0; i < st->air.num_resources; i++) {
			const struct air_resource *x = &st->air.resources[i];
			printf("  kind %d index %u size %u%s reads %u %s\n", x->kind, x->index, x->size, x->unused ? " unused" : "",
			       x->reads, x->name ? x->name : "");
		}
	}
	return 0;
}
