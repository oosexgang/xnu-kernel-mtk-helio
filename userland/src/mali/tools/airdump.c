/*
 * airdump LIBRARY.metallib FUNCTION OUT.bc: one function's AIR bitcode out of a Metal library, for llvm-dis -- how the
 * shaders of CoreAnimation's QuartzCore-default.metallib were read (the path rasterizer's, the generic vertex stage's
 * two [[point_size]] outputs). Built by mk-rwa-host.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_metal.h"
#include "nir_air.h"

int
main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "airdump LIBRARY.metallib FUNCTION OUT.bc\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL) {
		perror(argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *lib = malloc((size_t)n);
	if (lib == NULL || fread(lib, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "airdump: cannot read %s\n", argv[1]);
		return 1;
	}
	fclose(f);
	struct air_metallib ml;
	const char *err = NULL;
	if (!air_metallib_parse(NULL, lib, (size_t)n, &ml, &err)) {
		fprintf(stderr, "airdump: %s: %s\n", argv[1], err ? err : "not a metallib");
		return 1;
	}
	for (unsigned i = 0; i < ml.num_functions; i++) {
		if (strcmp(ml.functions[i].name, argv[2]) != 0)
			continue;
		FILE *o = fopen(argv[3], "wb");
		if (o == NULL || fwrite(ml.functions[i].bitcode, 1, ml.functions[i].bitcode_size, o) !=
				     ml.functions[i].bitcode_size) {
			perror(argv[3]);
			return 1;
		}
		fclose(o);
		return 0;
	}
	fprintf(stderr, "airdump: no function %s in %s\n", argv[2], argv[1]);
	return 1;
}
