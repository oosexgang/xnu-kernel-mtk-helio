/*
 * ca27_replay CAPTURE VERTEX.bc FRAGMENT.bc [OUT.ppm]: one draw of the 27 release's window server, as the phone wrote it
 * with boot-arg rwm_capture= (userland/src/metal/capture.cpp: the "cap ..." lines of the host's kept lines), drawn again
 * on this Mac through the same library (rwa_*, userland/tools/mesa/air) on softpipe, and what came out of it said: how many of
 * the pixels under the scissor are not zero, and their average.
 *
 * The modules are the pipeline's two functions out of QuartzCore's library, as userland/tools/air/ca27_module.py writes them;
 * their size and hash are checked against the capture's. Textures are made of the captured slots' shapes and filled
 * with opaque white (A8: 255), samplers are linear and clamped: the frames on the phone came out the same whatever the
 * textures held (runs 55 and 58), so what is wrong is before or after the sample.
 *
 * Build (userland/tools/air/mk-host-mac.sh first):
 *   cc -O1 -g -I userland/tools/mesa/air -o build/ca27_replay userland/tools/air/ca27_replay.c \
 *      build/mesa-host-mac/out/libOSMesa.8.dylib
 *   RWA_SOFTWARE=1 build/ca27_replay capture.txt vert.bc frag.bc
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <stdlib.h>
#include <string.h>

#include "air_metal.h"

#define MAXB 32

static struct rwa_constant vconst[64], fconst[64];
static unsigned nvconst, nfconst;
static uint8_t shape_bytes[sizeof(struct rwa_render_shape) + 64];
static size_t shape_size, shape_have;
static char vname[128], fname[128];
static size_t vsize, fsize;
static unsigned long long vhash, fhash;
static unsigned prim, vstart, vcount, instances, scissor[4], have_scissor, colors = 1;
static float viewport[4];
static int have_viewport;
static struct { int bound, stream; size_t offset, size, take, have; uint8_t data[2048]; } vbuf[MAXB], fbuf[MAXB];
static struct { unsigned w, h, f, t, u; int bound; } slot[MAXB];
static unsigned nslots;

static int
b64(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static size_t
decode(const char *s, uint8_t *out, size_t room)
{
	size_t n = 0;
	int v[4], k = 0;

	for (; *s && *s != '\n'; s++) {
		if (*s == '=') {
			v[k++] = 0;
		} else if (b64(*s) >= 0) {
			v[k++] = b64(*s);
		} else {
			continue;
		}
		if (k == 4) {
			uint32_t w = v[0] << 18 | v[1] << 12 | v[2] << 6 | v[3];
			int pad = (s[0] == '=') + (s[-1] == '=');

			if (n < room) out[n++] = w >> 16;
			if (pad < 2 && n < room) out[n++] = w >> 8 & 255;
			if (pad < 1 && n < room) out[n++] = w & 255;
			k = 0;
		}
	}
	return n;
}

static uint64_t
fnv(const uint8_t *b, size_t n)
{
	uint64_t h = 1469598103934665603ull;

	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 1099511628211ull;
	return h;
}

static uint8_t *
slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;

	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(*size);
	if (fread(b, 1, *size, f) != *size) { perror(path); exit(1); }
	fclose(f);
	return b;
}

static void
parse(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1024];

	if (!f) { perror(path); exit(1); }
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, "cap ");
		unsigned i, k;
		unsigned long long bits;

		if (!p) continue;
		p += 4;
		if (sscanf(p, "functions %127s %zu %llx %127s %zu %llx", vname, &vsize, &vhash, fname, &fsize, &fhash) == 6) continue;
		if (sscanf(p, "vconst %u %llx", &i, &bits) == 2 && nvconst < 64) { vconst[nvconst].index = i; vconst[nvconst++].bits = bits; continue; }
		if (sscanf(p, "fconst %u %llx", &i, &bits) == 2 && nfconst < 64) { fconst[nfconst].index = i; fconst[nfconst++].bits = bits; continue; }
		if (sscanf(p, "shape %zu", &shape_size) == 1) continue;
		if (sscanf(p, "shapebytes %u:", &k) == 1) {
			char *d = strchr(p, ':') + 1;
			shape_have += decode(d, shape_bytes + k * 120, sizeof(shape_bytes) - k * 120) ;
			continue;
		}
		if (!strncmp(p, "draw ", 5)) {
			char idx[16], vp[8], sc[8];
			sscanf(p, "draw prim %u start %u count %u instances %u base %*u index %15s viewport %7s scissor %7s %u %u %u %u colors %u",
			       &prim, &vstart, &vcount, &instances, idx, vp, sc, &scissor[0], &scissor[1], &scissor[2], &scissor[3], &colors);
			have_scissor = !strcmp(sc, "set");
			continue;
		}
		if (sscanf(p, "viewport %f %f %f %f", &viewport[0], &viewport[1], &viewport[2], &viewport[3]) == 4) { have_viewport = 1; continue; }
		if (!strncmp(p, "vbuf ", 5) && strstr(p, "offset")) {
			unsigned st; size_t off, size, take;
			if (sscanf(p, "vbuf %u offset %zu size %zu stream %u take %zu", &i, &off, &size, &st, &take) == 5 && i < MAXB) {
				vbuf[i].bound = 1; vbuf[i].stream = st; vbuf[i].offset = off; vbuf[i].size = size; vbuf[i].take = take;
			}
			continue;
		}
		if (!strncmp(p, "fbuf ", 5) && strstr(p, "offset")) {
			size_t off, size, take;
			if (sscanf(p, "fbuf %u offset %zu size %zu take %zu", &i, &off, &size, &take) == 4 && i < MAXB) {
				fbuf[i].bound = 1; fbuf[i].offset = off; fbuf[i].size = size; fbuf[i].take = take;
			}
			continue;
		}
		if (sscanf(p, "vbuf%u %u:", &i, &k) == 2 && i < MAXB) {
			vbuf[i].have += decode(strchr(p, ':') + 1, vbuf[i].data + k * 120, 2048 - k * 120);
			continue;
		}
		if (sscanf(p, "fbuf%u %u:", &i, &k) == 2 && i < MAXB) {
			fbuf[i].have += decode(strchr(p, ':') + 1, fbuf[i].data + k * 120, 2048 - k * 120);
			continue;
		}
		if (sscanf(p, "slots %u:", &nslots) == 1) {
			char *q = strchr(p, ':') + 1, *tok;
			while ((tok = strsep(&q, " \n"))) {
				unsigned s, w, h, fm, t, u;
				if (sscanf(tok, "%u:%ux%u/%u/t%u/u%x", &s, &w, &h, &fm, &t, &u) == 6 && s < MAXB) {
					slot[s] = (typeof(slot[0])){ w, h, fm, t, u, 1 };
				}
			}
			continue;
		}
	}
	fclose(f);
}

#if defined(__arm64e__)
/* on the phone, every line printed goes to the kernel as a kept line ("!!", into the host's log): what etc/rc's programs
 * print at boot is not in a run's log otherwise */
static char say_line[400];
static size_t say_used;

static int
say_write(void *cookie, const char *data, int n)
{
	(void)cookie;
	for (int i = 0; i < n; i++) {
		if (data[i] == '\n' || say_used >= sizeof(say_line) - 4) {
			char out[420];

			say_line[say_used] = 0;
			snprintf(out, sizeof(out), "!!replay %s", say_line);
			sysctlbyname("kern.hakios.say", NULL, NULL, out, strlen(out));
			say_used = 0;
		} else {
			say_line[say_used++] = data[i];
		}
	}
	return n;
}
#endif

int
main(int argc, char **argv)
{
#if defined(__arm64e__)
	{
		FILE *f = funopen(NULL, NULL, say_write, NULL, NULL);

		if (f != NULL) {
			setvbuf(f, NULL, _IONBF, 0);
			stdout = f;
			stderr = f;
		}
	}
#endif
	char why[1024] = "";
	size_t vsz, fsz;
	uint8_t *vbc, *fbc;
	struct rwa_render_shape shape;
	struct rwa_render_info info;

	if (argc < 4) {
		fprintf(stderr, "usage: ca27_replay CAPTURE VERTEX.bc FRAGMENT.bc [OUT.ppm]\n");
		return 2;
	}
#if defined(__arm64e__)
	/* on the phone (etc/rc runs it before launchd): only with boot-arg rwa_replay=1, and always over a green clear,
	 * so that the Mali's own result of a captured draw is in the run's log (16 November) */
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || strstr(args, "rwa_replay=1") == NULL)
			return 0;
		setenv("RWA_REPLAY_CLEAR", "1", 1);
		if (strstr(args, "rwa_noblend=1"))
			setenv("RWA_NOBLEND", "1", 1);
		fprintf(stderr, "ca27_replay: %s on the phone, tint %s noblend %s probe %s out32 %s\n", argv[1],
			getenv("RWA_TINT") ? getenv("RWA_TINT") : "-", getenv("RWA_NOBLEND") ? "1" : "-",
			getenv("RWA_PROBE") ? getenv("RWA_PROBE") : "-", getenv("RWA_OUT32") ? "1" : "-");
	}
#endif
	parse(argv[1]);
	vbc = slurp(argv[2], &vsz);
	fbc = slurp(argv[3], &fsz);
	printf("pipeline %s (%zu bytes, %016llx) %s (%zu bytes, %016llx); %u vertex and %u fragment constants\n", vname, vsize,
	       vhash, fname, fsize, fhash, nvconst, nfconst);
	printf("modules given: %zu bytes %016llx, %zu bytes %016llx%s\n", vsz, (unsigned long long)fnv(vbc, vsz), fsz,
	       (unsigned long long)fnv(fbc, fsz), fnv(vbc, vsz) == vhash && fnv(fbc, fsz) == fhash ? " -- the same" : " -- NOT the captured ones");
	if (shape_size != sizeof(shape) || shape_have < sizeof(shape)) {
		fprintf(stderr, "the captured shape is %zu bytes (%zu read), this library's is %zu\n", shape_size, shape_have, sizeof(shape));
		return 1;
	}
	memcpy(&shape, shape_bytes, sizeof(shape));

	struct rwa_device *dev = rwa_device_open(why, sizeof(why));
	if (!dev) { fprintf(stderr, "device: %s\n", why); return 1; }
	printf("device %s\n", rwa_device_name(dev));
	/* RWA_FCSET=I:V,I:V...: those fragment constants given those values (added when the capture has none) -- which of
	 * a pipeline's constants makes the Mali fail it */
	if (getenv("RWA_FCSET")) {
		const char *at = getenv("RWA_FCSET");
		unsigned idx;
		unsigned long long val;
		int used;

		while (sscanf(at, "%u:%llx%n", &idx, &val, &used) == 2) {
			unsigned k;

			for (k = 0; k < nfconst && fconst[k].index != idx; k++)
				;
			if (k == nfconst && nfconst < 64)
				fconst[nfconst++].index = idx;
			if (k < nfconst)
				fconst[k].bits = val;
			printf("fragment constant %u = %llx\n", idx, val);
			at += used;
			if (*at != ',')
				break;
			at++;
		}
	}
	rwa_render_constants(vconst, nvconst, fconst, nfconst);
	/* no entry names, as userland/src/metal/pipeline.cpp makes it */
	struct rwa_render *render = rwa_render_create(dev, vbc, vsz, NULL, fbc, fsz, NULL, &shape, &info, why, sizeof(why));
	if (!render) { fprintf(stderr, "pipeline: %s\n", why); return 1; }
	if (vcount == 0) {
		/* "cap refused ...": the phone's library refused this one, and here it was made */
		printf("pipeline made; the capture has no draw to replay\n");
		return 0;
	}

	/* the target: the display's size, cleared to nothing */
	struct rwa_texture_shape ts = { .pixel_format = 80, .width = 750, .height = 1624, .depth = 1, .array_length = 1,
					.mip_levels = 1, .sample_count = 1, .type = 2, .usage = 0x5 };
	/* attachment 0 of the shape's own format when it names one (the downsample of the 27 release draws into an
	 * RGBA16Float, run 242); read back as BGRA8 bytes below either way */
	if (shape.color_format[0] == 115)
		ts.pixel_format = 115;
	struct rwa_texture *target = rwa_texture_create(dev, &ts, why, sizeof(why));
	if (!target) { fprintf(stderr, "target: %s\n", why); return 1; }
	/* and the pipeline's other colour attachments (the 27 release's passes have 2 or 3: RGBA16Float, R32Float), of
	 * the formats its shape names, as the draw's "colors" says how many there were */
	struct rwa_texture *targets[8] = { target };
	unsigned ntargets = colors >= 1 && colors <= 8 ? colors : 1;
	for (unsigned i = 1; i < ntargets; i++) {
		struct rwa_texture_shape ti = ts;
		ti.pixel_format = shape.color_format[i];
		targets[i] = rwa_texture_create(dev, &ti, why, sizeof(why));
		if (!targets[i]) { fprintf(stderr, "colour attachment %u (format %u): %s\n", i, ti.pixel_format, why); return 1; }
	}

	struct rwa_buffer *vb[MAXB] = { 0 }, *fb[MAXB] = { 0 };
	struct rwa_vertex_stream streams[MAXB] = { 0 };
	struct rwa_buffer_bind vtable[MAXB] = { 0 }, ftable[MAXB] = { 0 };
	unsigned nv = 0, nf = 0;

	for (unsigned i = 0; i < MAXB; i++) {
		if (vbuf[i].bound) {
			vb[i] = rwa_buffer_create(dev, vbuf[i].take ? vbuf[i].take : 16, why, sizeof(why));
			memcpy(rwa_buffer_map(vb[i]), vbuf[i].data, vbuf[i].have);
			streams[i].buffer = vbuf[i].stream ? vb[i] : NULL;
			vtable[i].buffer = vb[i];
			nv = i + 1;
		}
		if (fbuf[i].bound) {
			fb[i] = rwa_buffer_create(dev, fbuf[i].take ? fbuf[i].take : 16, why, sizeof(why));
			memcpy(rwa_buffer_map(fb[i]), fbuf[i].data, fbuf[i].have);
			ftable[i].buffer = fb[i];
			nf = i + 1;
		}
	}
	struct rwa_texture *tex[MAXB] = { 0 };
	struct rwa_sampler *samp[MAXB] = { 0 };
	struct rwa_sampler_state ss = { .address = { 0, 0, 0 }, .min_filter = 1, .mag_filter = 1, .mip_filter = 0,
					.compare_function = 0, .max_anisotropy = 1, .normalized_coordinates = 1,
					.lod_min = 0, .lod_max = 1000 };
	for (unsigned i = 0; i < nslots && i < MAXB; i++) {
		if (!slot[i].bound) continue;
		struct rwa_texture_shape s = { .pixel_format = slot[i].f, .width = slot[i].w, .height = slot[i].h, .depth = 1,
					       .array_length = 1, .mip_levels = 1, .sample_count = 1, .type = slot[i].t ? slot[i].t : 2,
					       .usage = 1 };
		tex[i] = rwa_texture_create(dev, &s, why, sizeof(why));
		if (!tex[i]) { fprintf(stderr, "texture %u (%ux%u format %u): %s\n", i, s.width, s.height, s.pixel_format, why); continue; }
		size_t row = slot[i].w * 16;
		uint8_t *white = malloc(row * slot[i].h);
		memset(white, 255, row * slot[i].h);
		/* half-float RGBA (115): bytes of 255 are NaN, so white is 1.0 in every half (0x3c00) */
		if (slot[i].f == 115)
			for (size_t k = 0; k < row * slot[i].h / 2; k++)
				((uint16_t *)white)[k] = 0x3c00;
		unsigned o[3] = { 0, 0, 0 }, z[3] = { slot[i].w, slot[i].h, 1 };
		if (rwa_texture_write(tex[i], 0, 0, o, z, white, 0, why, sizeof(why)) != 0)
			fprintf(stderr, "texture %u write: %s\n", i, why);
		free(white);
	}
	/* a sampler in every slot: CoreAnimation's shaders take their textures and samplers at different indices
	 * (fixed_frag_lph_*: texture 3, img_samp_0 at sampler 0), and an empty sampler slot reads garbage on llvmpipe */
	for (unsigned i = 0; i < nslots && i < MAXB; i++)
		samp[i] = rwa_sampler_create(dev, &ss, why, sizeof(why));

	float clear[4] = { 0, 0, 0, 0 };
	if (getenv("RWA_REPLAY_CLEAR")) { clear[1] = 1; clear[3] = 1; }   /* a check that the target is read */
	struct rwa_draw d = { 0 };
	d.color = targets;
	d.color_count = ntargets;
	d.clear_color = clear;
	d.vertex.buffers = vb;
	d.vertex.buffer_count = nv;
	d.fragment.buffers = fb;
	d.fragment.buffer_count = nf;
	d.fragment.textures = tex;
	d.fragment.texture_count = nslots;
	d.fragment.samplers = samp;
	d.fragment.sampler_count = nslots;
	d.primitive = prim;
	d.vertex_start = vstart;
	d.vertex_count = vcount;
	d.instance_count = instances;
	d.vertex_streams = streams;
	d.vertex_stream_count = nv;
	d.vertex_buffer_table = vtable;
	d.vertex_buffer_table_count = nv;
	d.fragment_buffer_table = ftable;
	d.fragment_buffer_table_count = nf;
	d.viewport = have_viewport ? viewport : NULL;
	d.scissor = have_scissor ? scissor : NULL;
	unsigned failed_before = 0, failed_after = 0;
	size_t fcsz = sizeof(failed_before);
	/* on the phone: whether the Mali failed the draw's job chains (kern.hakios.mali_failed) */
	sysctlbyname("kern.hakios.mali_failed", &failed_before, &fcsz, NULL, 0);
	if (rwa_render_draw(render, &d, why, sizeof(why)) != 0) { fprintf(stderr, "draw: %s\n", why); return 1; }
	if (rwa_finish(dev, why, sizeof(why)) != 0) { fprintf(stderr, "finish: %s\n", why); return 1; }
	fcsz = sizeof(failed_after);
	if (sysctlbyname("kern.hakios.mali_failed", &failed_after, &fcsz, NULL, 0) == 0)
		printf("the GPU failed %u job chain(s) of this draw\n", failed_after - failed_before);

	uint32_t *px = malloc(750 * 1624 * 4);
	unsigned o[3] = { 0, 0, 0 }, z[3] = { 750, 1624, 1 };
	if (ts.pixel_format == 115) {
		/* halves made 8-bit BGRA, clamped to 0..1 */
		uint16_t *hp = malloc((size_t)750 * 1624 * 8);
		if (!hp || rwa_texture_read(target, 0, 0, o, z, hp, 750 * 8, why, sizeof(why)) != 0) { fprintf(stderr, "read: %s\n", why); return 1; }
		for (size_t k = 0; k < (size_t)750 * 1624; k++) {
			uint8_t c8[4];
			for (unsigned c = 0; c < 4; c++) {
				_Float16 h;
				memcpy(&h, &hp[k * 4 + c], 2);
				float v = (float)h;
				c8[c] = v != v || v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255 + 0.5f);
			}
			px[k] = (uint32_t)c8[3] << 24 | (uint32_t)c8[0] << 16 | (uint32_t)c8[1] << 8 | c8[2];
		}
		free(hp);
	} else if (rwa_texture_read(target, 0, 0, o, z, px, 750 * 4, why, sizeof(why)) != 0) { fprintf(stderr, "read: %s\n", why); return 1; }
	unsigned x0 = have_scissor ? scissor[0] : 0, y0 = have_scissor ? scissor[1] : 0;
	unsigned x1 = have_scissor ? scissor[0] + scissor[2] : 750, y1 = have_scissor ? scissor[1] + scissor[3] : 1624;
	unsigned long n = 0, lit = 0, sum[4] = { 0 };
	for (unsigned y = y0; y < y1 && y < 1624; y++)
		for (unsigned x = x0; x < x1 && x < 750; x++) {
			uint32_t p = px[y * 750 + x];
			n++;
			lit += p != 0;
			sum[0] += p >> 16 & 255; sum[1] += p >> 8 & 255; sum[2] += p & 255; sum[3] += p >> 24;
		}
	printf("under the scissor (%u,%u %ux%u): %lu of %lu pixels not zero, average r %lu g %lu b %lu a %lu\n", x0, y0, x1 - x0,
	       y1 - y0, lit, n, n ? sum[0] / n : 0, n ? sum[1] / n : 0, n ? sum[2] / n : 0, n ? sum[3] / n : 0);
	/* the commonest values that are not zero, as r g b a: what the shader made of the white image */
	{
		uint32_t val[8] = { 0 };
		unsigned long cnt[8] = { 0 };
		for (unsigned i = 0; i < 750u * 1624u; i++) {
			uint32_t p = px[i];
			unsigned k;
			if (p == 0) continue;
			for (k = 0; k < 8 && cnt[k] && val[k] != p; k++) {}
			if (k < 8) { val[k] = p; cnt[k]++; }
		}
		for (unsigned k = 0; k < 8 && cnt[k]; k++)
			printf("  %lu pixels of r %u g %u b %u a %u\n", cnt[k], val[k] >> 16 & 255, val[k] >> 8 & 255, val[k] & 255,
			       val[k] >> 24);
	}
	if (argc > 4) {
		FILE *o2 = fopen(argv[4], "wb");
		fprintf(o2, "P6 750 1624 255\n");
		for (unsigned i = 0; i < 750 * 1624; i++) {
			uint8_t rgb[3] = { px[i] >> 16 & 255, px[i] >> 8 & 255, px[i] & 255 };
			fwrite(rgb, 1, 3, o2);
		}
		fclose(o2);
	}
	return 0;
}
