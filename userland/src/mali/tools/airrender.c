/*
 * airrender LIB.metallib FUNCTION...: what each function of an Apple .metallib asks of the driver once the AIR frontend
 * has translated it (compiler.c hm_air_describe): stage, resources, varyings, vertex attributes, and the NIR.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compiler.h"

int
main(int argc, char **argv)
{
	FILE *f = argc > 2 ? fopen(argv[1], "rb") : NULL;
	if (f == NULL) {
		fprintf(stderr, "usage: airrender LIB.metallib FUNCTION...\n");
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *lib = malloc(size);
	if (fread(lib, 1, size, f) != (size_t)size) return 1;
	fclose(f);
	hm_compile_init();
	if (argc == 3 && strcmp(argv[2], "--argbufs") == 0) {
		return hm_air_argbuf_functions(lib, size, stdout) > 0 ? 0 : 1;
	}
	if (argc == 5 && strcmp(argv[2], "--program") == 0) {
		struct hm_program p;
		uint32_t dev = 0;
		bool linear = false;
		char why[300];
		if (hm_compile_air_program(lib, size, argv[3], argv[4], 0x7402, &p, &dev, &linear, why, sizeof(why)) != 0) {
			printf("%s\n", why);
			return 1;
		}
		printf("%s: vertex shader %u bytes, fragment shader %u bytes, device buffers %#x, %s sampler, %u textures, "
		    "%u samplers, vs pushes %u, fs pushes %u, vs out %u, fs in %u\n", p.name, p.vs_size, p.fs_size, dev,
		    linear ? "linear" : "nearest", p.texture_count, p.sampler_count, p.vs_push_count, p.fs_push_count,
		    p.vs_out_count, p.fs_in_count);
		for (unsigned i = 0; i < p.vs_push_count; i++) printf("  vs push %u: ubo %u offset %u\n", i, p.vs_push[i].ubo, p.vs_push[i].offset);
		for (unsigned i = 0; i < p.vs_out_count; i++) printf("  vs out location %u format %#x size %u\n", p.vs_out[i].location, p.vs_out[i].format, p.vs_out[i].size);
		for (unsigned i = 0; i < p.fs_in_count; i++) printf("  fs in location %u format %#x size %u\n", p.fs_in[i].location, p.fs_in[i].format, p.fs_in[i].size);
		return 0;
	}
	int bad = 0;
	for (int i = 2; i < argc; i++) {
		char why[300];
		if (hm_air_describe(lib, size, argv[i], stdout, why, sizeof(why)) != 0) {
			printf("%s\n", why);
			bad++;
		}
	}
	return bad != 0;
}
