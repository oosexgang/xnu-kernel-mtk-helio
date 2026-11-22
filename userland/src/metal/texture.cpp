/*
 * A texture: the compiler library's (struct rwa_texture), its shape, and the two roads between it and
 * an IOSurface the Mali cannot hand over as GPU memory -- copied IN from a surface another process
 * draws, copied BACK into a surface the window server shows. Which thread does the copying, and what
 * is said when it fails, is the plug-in's (HakiosMetalTexture in src/hakiosmetal.m).
 */
#include "rwm.h"

#include <IOKit/IOReturn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#include <memory>
#include <new>

struct rwm_texture {
	struct rwa_texture *texture = nullptr;
	struct rwa_texture_shape shape = {};
	IOSurfaceRef surface = nullptr;   /* the copy-back road's (rwm_texture_sync_surface) */
	IOSurfaceRef source = nullptr;    /* the copy-in road's (rwm_texture_copy_in) */
	uint32_t source_seed = 0;         /* the source's seed when it was last copied in */
	bool source_two_planes = false;   /* ... and it is YCbCr 4:2:0 in two planes, packed into one */
	bool memoryless = false;          /* asked for with MTLStorageModeMemoryless: nothing of it outlives its pass */
	uint32_t in_place = 0;            /* the surface it lies over, drawn in place (src/metal/import.cpp) */
	/* the copy-in road from memory (-newTextureWithBytesNoCopy:...): the caller's bytes, re-copied before a use
	 * whenever their signature has moved -- the caller fills them after it makes the texture */
	const uint8_t *memory = nullptr;
	size_t memory_length = 0, memory_stride = 0;
	unsigned memory_layers = 1;
	uint64_t memory_signature = 0;
	bool memory_written = false;
	/* for rwm_texture_sample_report: where it came from (0 made here, 1 over a surface in place, 2 copied in from a
	 * surface, 3 copied in from memory, 4 a view) and whether anything ever went into it */
	uint8_t origin = 0;
	bool filled = false;
	int8_t surface_empty = -1;        /* origin 1: its surface's memory all zeroes when first sampled (1), or not (0) */
	unsigned samples = 0;             /* draws that sampled it (texshot) */
	unsigned fills = 0;               /* passes that drew into it (texshot of RGBA16Float / R32Float targets) */
	uint8_t fill_kind = 0;            /* what filled it (texshot): 1 drawn into, 2 written, 4 copied in from memory, 8 a view */

	~rwm_texture()
	{
		if (texture != nullptr) {
			rwm_air()->texture_destroy(texture);
		}
		if (surface != nullptr) {
			CFRelease(surface);
		}
		if (source != nullptr) {
			CFRelease(source);
		}
	}
};

namespace {

struct free_deleter {
	void operator()(void *p) const { free(p); }
};
using block = std::unique_ptr<uint8_t, free_deleter>;

/* a lock held for as long as the copy reads the surface */
class surface_lock {
public:
	surface_lock(IOSurfaceRef surface, IOSurfaceLockOptions options)
		: surface_(surface), options_(options),
		  held_(IOSurfaceLock(surface, options, nullptr) == kIOReturnSuccess) {}
	~surface_lock()
	{
		if (held_) {
			IOSurfaceUnlock(surface_, options_, nullptr);
		}
	}
	bool held() const { return held_; }

private:
	IOSurfaceRef surface_;
	IOSurfaceLockOptions options_;
	bool held_;
};

/*
 * A plane of a surface as a copy reads it. The surface is another process's, and its size is its own:
 * the texture's shape says how much is read, and nothing here reads before it is shown that the plane
 * and the surface's allocation hold that much (the automated review's finding, 31 October: the copy
 * trusted the shape). A surface without planes answers the plane calls with zero, so it is read
 * through the whole-surface calls, as the copy-back does.
 */
struct plane {
	const uint8_t *base = nullptr;
	size_t row = 0, width = 0, height = 0, element = 0;
};

plane
plane_of(IOSurfaceRef surface, size_t index)
{
	const size_t planes = IOSurfaceGetPlaneCount(surface);

	if (planes == 0) {
		if (index != 0) {
			return plane{};
		}
		return plane{ (const uint8_t *)IOSurfaceGetBaseAddress(surface), IOSurfaceGetBytesPerRow(surface),
			      IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
			      IOSurfaceGetBytesPerElement(surface) };
	}
	if (index >= planes) {
		return plane{};
	}
	return plane{ (const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface, index),
		      IOSurfaceGetBytesPerRowOfPlane(surface, index), IOSurfaceGetWidthOfPlane(surface, index),
		      IOSurfaceGetHeightOfPlane(surface, index), IOSurfaceGetBytesPerElementOfPlane(surface, index) };
}

/* `rows` rows of `bytes` bytes, a row apart from the plane's start: inside the plane and the allocation */
bool
covers(IOSurfaceRef surface, const plane &p, size_t bytes, size_t rows)
{
	const uint8_t *start = (const uint8_t *)IOSurfaceGetBaseAddress(surface);
	const size_t alloc = IOSurfaceGetAllocSize(surface);
	size_t end;

	if (p.base == nullptr || start == nullptr || p.base < start || bytes == 0 || rows == 0 ||
	    bytes > p.row || rows > p.height) {
		return false;
	}
	if (__builtin_mul_overflow(rows - 1, p.row, &end) || __builtin_add_overflow(end, bytes, &end) ||
	    __builtin_add_overflow(end, (size_t)(p.base - start), &end)) {
		return false;
	}
	return end <= alloc;
}

/* the bytes of a texel of the formats the plug-in copies a surface into (HakiosMetalDevice's import:
 * MTLPixelFormatBGRA8Unorm, its sRGB twin, RGBA8Unorm, RG8Unorm, R8Unorm), 0 for any other */
size_t
texel_bytes(unsigned format)
{
	switch (format) {
	case 80: case 81: case 70:
		return 4;
	case 30:
		return 2;
	case 10:
		return 1;
	/* the 27 release's icons come in RGBA16Float (115) and their names in A8 (1): a surface of either was refused
	 * here, so its texture stayed empty and the draws that sample it drew nothing to see (16 November). The rows are
	 * copied as they are, so only formats whose texels the library stores as the surface holds them: A8, R16Float,
	 * R32Float, RG16Float, RGB10A2, BGR10A2, RGBA16Unorm / Float, RGBA32Float */
	case 1:
		return 1;
	case 25:
		return 2;
	case 55: case 65: case 90: case 94:
		return 4;
	case 110: case 115:
		return 8;
	case 125:
		return 16;
	}
	return 0;
}

} // namespace

namespace {

/* a texture's kilobytes as made, roughly: 8 bytes a texel for the 64-bit formats, 4 for the rest */
long
texture_kb(const struct rwa_texture_shape &sh)
{
	const long texel = sh.pixel_format == 115 || sh.pixel_format == 105 || sh.pixel_format == 125 ? 8 : 4;

	return ((long)sh.width * (sh.height ? sh.height : 1) * (sh.depth ? sh.depth : 1) *
		(sh.array_length ? sh.array_length : 1) * texel) >> 10;
}

} // namespace

extern "C" struct rwm_texture *
rwm_texture_new(struct rwa_texture *texture, const struct rwa_texture_shape *shape)
{
	rwm_texture *made = new (std::nothrow) rwm_texture;

	if (made == nullptr) {
		rwm_air()->texture_destroy(texture);
		return nullptr;
	}
	made->texture = texture;
	made->shape = *shape;
	__atomic_add_fetch(&rwm_live_textures, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&rwm_live_texture_kb, texture_kb(*shape), __ATOMIC_RELAXED);
	return made;
}

extern "C" void
rwm_texture_free(struct rwm_texture *texture)
{
	if (texture != nullptr) {
		__atomic_sub_fetch(&rwm_live_textures, 1, __ATOMIC_RELAXED);
		__atomic_sub_fetch(&rwm_live_texture_kb, texture_kb(texture->shape), __ATOMIC_RELAXED);
		__atomic_add_fetch(&rwm_textures_freed, 1, __ATOMIC_RELAXED);
	}
	delete texture;
}

extern "C" struct rwa_texture *
rwm_texture_mesa(const struct rwm_texture *texture)
{
	return texture->texture;
}

extern "C" const struct rwa_texture_shape *
rwm_texture_shape(const struct rwm_texture *texture)
{
	return &texture->shape;
}

/*
 * The copy-IN road: a surface another process draws (a widget's, a layer's backing store) that cannot
 * be laid under a texture as it is -- rows not a multiple of 64 bytes, or a format the import does not
 * serve ('LA08') -- is copied into the library's own texture, again before every command buffer that
 * samples it once the surface has changed (its seed). Before, such a texture was made empty and stayed
 * so: the widgets' solid squares (24 October).
 */
extern "C" void
rwm_texture_copy_in_from(struct rwm_texture *texture, IOSurfaceRef surface, bool two_planes)
{
	if (texture->source != nullptr) {
		CFRelease(texture->source);
	}
	texture->source = (IOSurfaceRef)CFRetain(surface);
	texture->origin = 2;
	texture->source_seed = IOSurfaceGetSeed(surface) - 1;
	texture->source_two_planes = two_planes;
}

extern "C" void
rwm_texture_copy_in_from_memory(struct rwm_texture *texture, const void *bytes, size_t length, size_t stride,
				unsigned layers)
{
	texture->memory = (const uint8_t *)bytes;
	texture->memory_length = length;
	texture->memory_stride = stride;
	texture->memory_layers = layers ? layers : 1;
	texture->memory_written = false;
	texture->origin = 3;
}

namespace {

/* 256 words spread over the memory and its length: what changes when an image is drawn into it */
uint64_t
memory_signature(const uint8_t *m, size_t length)
{
	uint64_t h = 1469598103934665603ull ^ length;

	for (unsigned i = 0; length >= 8 && i < 256; i++) {
		uint64_t word;
		const size_t at = (length - 8) * i / 255 & ~(size_t)7;

		memcpy(&word, m + at, 8);
		h = (h ^ word) * 1099511628211ull;
	}
	return h;
}

} // namespace

extern "C" int
rwm_texture_copy_in(struct rwm_texture *texture, size_t *bytes, char *why, size_t why_size)
{
	if (texture->memory != nullptr) {
		const uint64_t signature = memory_signature(texture->memory, texture->memory_length);
		const unsigned origin[3] = { 0, 0, 0 };
		const unsigned size[3] = { texture->shape.width, texture->shape.height,
					   texture->shape.depth ? texture->shape.depth : 1 };

		*bytes = 0;
		if (texture->memory_written && signature == texture->memory_signature) {
			return 0;
		}
		for (unsigned layer = 0; layer < texture->memory_layers; layer++) {
			if (!rwm_texture_write(texture, 0, layer, origin, size,
					       texture->memory + layer * (texture->memory_length / texture->memory_layers),
					       texture->memory_stride)) {
				snprintf(why, why_size, "the caller's %zu bytes (%zu a row) would not go into the texture",
					 texture->memory_length, texture->memory_stride);
				return -1;
			}
		}
		texture->memory_signature = signature;
		texture->memory_written = true;
		texture->filled = true;
		texture->fill_kind |= 4;
		*bytes = texture->memory_length;
		{
			static unsigned said;

			if (said++ < 12) {
				unsigned nonzero = 0;
				char line[200];

				for (unsigned i = 0; texture->memory_length >= 4 && i < 64; i++) {
					uint32_t word;

					memcpy(&word, texture->memory + ((texture->memory_length - 4) * i / 63 & ~(size_t)3), 4);
					nonzero += word != 0;
				}
				snprintf(line, sizeof(line), "!!rwm image copied in: %ux%u format %u, %zu bytes, %u of 64 words not zero",
					 texture->shape.width, texture->shape.height, texture->shape.pixel_format,
					 texture->memory_length, nonzero);
				sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
			}
		}
		return 0;
	}

	const struct rwm_air *air = rwm_air();
	const unsigned width = texture->shape.width, height = texture->shape.height;
	const unsigned origin[3] = { 0, 0, 0 }, size[3] = { width, height, 1 };

	*bytes = 0;
	if (texture->source == nullptr || air->texture_write == nullptr) {
		return 0;
	}

	uint32_t seed = IOSurfaceGetSeed(texture->source);

	if (seed != 0 && seed == texture->source_seed) {
		return 0;               /* (a surface whose seed never moves is copied every time) */
	}

	surface_lock lock(texture->source, kIOSurfaceLockReadOnly);

	if (!lock.held()) {
		return 0;
	}
	rwm_surface_to_memory(texture->source);     /* (what the GPU drew into it, not what this cache held) */

	const plane first = plane_of(texture->source, 0);
	const void *texels = first.base;
	size_t row = first.row;
	block packed;

	/*
	 * A YCbCr surface in two planes ('420f', '420v': the wallpaper, photographs), which Metal samples
	 * as one texture of format 500 -- (Y, Cb, Cr) out of the sampler, and QuartzCore's shader
	 * converting with its own matrix (fc_has_ycbcr_matrix). Here it is an RGBA8 texture holding
	 * exactly that: Y in red, the chroma of its 2 x 2 block in green and blue.
	 */
	if (texture->source_two_planes) {
		const plane second = plane_of(texture->source, 1);
		/* a byte of Y per texel; a pair (Cb, Cr) per two texels, a row of them per two rows */
		const size_t chroma_bytes = (size_t)((width - 1) / 2) * 2 + 2, chroma_rows = (height - 1) / 2 + 1;

		if (width == 0 || height == 0 || !covers(texture->source, first, width, height) ||
		    !covers(texture->source, second, chroma_bytes, chroma_rows)) {
			snprintf(why, why_size, "the two-plane surface (%zux%zu, %zu and %zu bytes a row) is smaller "
				 "than its %ux%u texture", first.width, first.height, first.row, second.row, width,
				 height);
			return -1;
		}

		const uint8_t *luma = first.base, *chroma = second.base;
		const size_t luma_row = first.row, chroma_row = second.row;

		packed.reset((uint8_t *)malloc((size_t)width * height * 4));
		if (packed == nullptr) {
			return 0;
		}
		for (unsigned y = 0; y < height; y++) {
			const uint8_t *l = luma + y * luma_row, *c = chroma + (y / 2) * chroma_row;
			uint8_t *out = packed.get() + (size_t)y * width * 4;

			for (unsigned x = 0; x < width; x++) {
				out[4 * x + 0] = l[x];
				out[4 * x + 1] = c[(x / 2) * 2];
				out[4 * x + 2] = c[(x / 2) * 2 + 1];
				out[4 * x + 3] = 255;
			}
		}
		texels = packed.get();
		row = (size_t)width * 4;
	} else {
		/* the library reads `width` texels of the texture's format a row, rows `row` apart: that many
		 * bytes of every one of `height` rows inside the plane and the allocation, measured by the
		 * texture's own format and not the surface's element, which need not agree (the automated
		 * review's finding) */
		const size_t texel = texel_bytes(texture->shape.pixel_format);

		if (texel == 0) {
			snprintf(why, why_size, "a surface is not copied into format %u", texture->shape.pixel_format);
			return -1;
		}
		if (width == 0 || height == 0 || width > first.width ||
		    !covers(texture->source, first, (size_t)width * texel, height)) {
			snprintf(why, why_size, "the surface (%zux%zu, %zu bytes a row) is smaller than its %ux%u "
				 "texture of format %u", first.width, first.height, first.row, width, height,
				 texture->shape.pixel_format);
			return -1;
		}
	}
	*bytes = row * height;
	if (air->texture_write(texture->texture, 0, 0, origin, size, texels, row, why, why_size) != 0) {
		return -1;
	}
	texture->source_seed = seed;
	/* boot-arg rwm_uplog=1: the first surfaces of RGBA16Float / A8 copied in, with how many texels have alpha */
	if (strstr(rwm_boot_args(), "rwm_uplog=1") != nullptr &&
	    (texture->shape.pixel_format == 115 || texture->shape.pixel_format == 1)) {
		static unsigned said;

		if (said < 24) {
			said++;
			const bool half = texture->shape.pixel_format == 115;
			unsigned long lit = 0;

			for (unsigned y = 0; y < height; y++) {
				const uint8_t *r = (const uint8_t *)texels + (size_t)y * row;

				for (unsigned x = 0; x < width; x++) {
					lit += half ? (r[x * 8 + 6] | r[x * 8 + 7]) != 0 : r[x] != 0;
				}
			}
			char line[200];

			snprintf(line, sizeof(line), "!!rwm surface copied in: %ux%u format %u, %lu texels with alpha",
				 width, height, texture->shape.pixel_format, lit);
			sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
		}
	}
	return 0;
}

/*
 * The copy-BACK road, for a GPU whose kernel driver cannot hand an IOSurface over as GPU memory (the
 * Mali's: there is no HakiosGPU service, which is what kHakiosSurface lives on). The texture is then
 * the library's own, and what was drawn into it goes into the surface's memory at the end of each
 * command buffer that drew into it -- the one copy the drawable path exists to save, paid so that the
 * window server's frames reach the display at all.
 */
extern "C" void
rwm_texture_copy_back_to(struct rwm_texture *texture, IOSurfaceRef surface)
{
	if (texture->surface != nullptr) {
		CFRelease(texture->surface);
	}
	texture->surface = (IOSurfaceRef)CFRetain(surface);
}

extern "C" bool
rwm_texture_has_surface(const struct rwm_texture *texture)
{
	return texture->surface != nullptr;
}

/*
 * What touches a texture's memory at once (-replaceRegion:, -getBytes:, a depth read) waits first for the buffers
 * committed before it: in Metal's order they come first, and on the driver's worker they are drawn later.
 */
extern "C" bool
rwm_texture_write(struct rwm_texture *texture, unsigned level, unsigned slice, const unsigned origin[3],
		  const unsigned size[3], const void *bytes, size_t stride)
{
	char why[256] = { 0 };

	rwm_worker_drain("replaceRegion");
	if (rwm_air()->texture_write(texture->texture, level, slice, origin, size, bytes, stride, why, sizeof(why)) != 0) {
		rwm_print("writing texels: %s\n", why);
		return false;
	}
	texture->filled = true;
	texture->fill_kind |= 2;
	/* boot-arg rwm_uplog=1: the first uploads of RGBA16Float (115) and A8 (1) textures -- the 27 release's icons and
	 * names -- said with how many texels have alpha in the caller's bytes and in what reads back (16 November) */
	if (strstr(rwm_boot_args(), "rwm_uplog=1") != nullptr && level == 0 && slice == 0 &&
	    (texture->shape.pixel_format == 115 || texture->shape.pixel_format == 1) && size[0] != 0 && size[1] != 0) {
		static unsigned said;

		if (said < 24) {
			said++;
			const unsigned f = texture->shape.pixel_format;
			const size_t texel = f == 115 ? 8 : 1, row = (size_t)size[0] * texel;
			unsigned long in_lit = 0, back_lit = 0;
			uint8_t *back = (uint8_t *)calloc((size_t)size[1], row);

			for (unsigned y = 0; y < size[1]; y++) {
				const uint8_t *r = (const uint8_t *)bytes + (size_t)y * stride;

				for (unsigned x = 0; x < size[0]; x++) {
					in_lit += f == 115 ? (r[x * 8 + 6] | r[x * 8 + 7]) != 0 : r[x] != 0;
				}
			}
			const bool read = back != nullptr && rwm_texture_read(texture, 0, 0, origin, size, back, row);

			for (size_t i = 0; read && i < (size_t)size[0] * size[1]; i++) {
				back_lit += f == 115 ? (back[i * 8 + 6] | back[i * 8 + 7]) != 0 : back[i] != 0;
			}
			free(back);
			char line[200];

			snprintf(line, sizeof(line), "!!rwm upload %ux%u at %u,%u into %ux%u format %u: %lu texels with alpha given, "
				 "%lu read back%s", size[0], size[1], origin[0], origin[1], texture->shape.width,
				 texture->shape.height, f, in_lit, back_lit, read ? "" : " (no read)");
			sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
		}
	}
	/*
	 * A texture over another process's surface (the copy-in road) shares that surface's memory in Metal, so
	 * -replaceRegion: is a write into the surface. Here the texture is a copy, refreshed from the surface before
	 * every draw that samples it once the surface's seed has moved -- and the first time always. Written only into
	 * the copy, the texels were overwritten by the surface's own (empty) memory at the first draw: every image
	 * CoreAnimation 27 uploads that way came out transparent (15 November). So the rows go into the surface as
	 * well, and the copy is marked as holding what the surface now holds.
	 */
	if (texture->source != nullptr && !texture->source_two_planes && level == 0 && slice == 0 && size[0] != 0 &&
	    size[1] != 0) {
		const size_t texel = texel_bytes(texture->shape.pixel_format);
		bool wrote = false;

		if (texel != 0) {
			surface_lock lock(texture->source, 0);

			if (lock.held()) {
				const plane first = plane_of(texture->source, 0);
				const size_t bytes_row = (size_t)size[0] * texel;

				if (origin[0] + size[0] <= texture->shape.width && origin[1] + size[1] <= texture->shape.height &&
				    covers(texture->source, first, (size_t)(origin[0] + size[0]) * texel, origin[1] + size[1])) {
					for (unsigned y = 0; y < size[1]; y++) {
						memcpy((uint8_t *)first.base + (size_t)(origin[1] + y) * first.row + (size_t)origin[0] * texel,
						       (const uint8_t *)bytes + (size_t)y * stride, bytes_row);
					}
					wrote = true;
				}
			}
		}
		if (wrote) {
			texture->source_seed = IOSurfaceGetSeed(texture->source);
		}
		static unsigned said;

		if (said++ < 8) {
			char line[160];

			snprintf(line, sizeof(line), "!!rwm replaceRegion %ux%u into a surface's texture %ux%u format %u: %s",
				 size[0], size[1], texture->shape.width, texture->shape.height, texture->shape.pixel_format,
				 wrote ? "written into the surface too" : "NOT written into the surface");
			sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
		}
	}
	return true;
}

extern "C" bool
rwm_texture_read(struct rwm_texture *texture, unsigned level, unsigned slice, const unsigned origin[3],
		 const unsigned size[3], void *bytes, size_t stride)
{
	char why[256] = { 0 };

	rwm_worker_drain("getBytes");
	if (rwm_air()->texture_read(texture->texture, level, slice, origin, size, bytes, stride, why, sizeof(why)) != 0) {
		rwm_print("reading texels: %s\n", why);
		return false;
	}
	return true;
}

/* a depth/stencil texture by aspect, which Metal's -getBytes: has no way to name and the library's texture read
 * refuses by name */
extern "C" bool
rwm_texture_read_depth(struct rwm_texture *texture, const unsigned origin[3], const unsigned size[3], float *depth,
		       uint8_t *stencil)
{
	const struct rwm_air *air = rwm_air();
	char why[256] = { 0 };

	if (air->depth_read == nullptr) {
		rwm_print("this compiler library has no rwa_depth_read, so it is "
			  "older than this driver and cannot say what a depth buffer holds\n");
		return false;
	}
	rwm_worker_drain("depth read");
	if (air->depth_read(texture->texture, 0, 0, origin, size, depth, stencil, why, sizeof(why)) != 0) {
		rwm_print("reading the depth of a texture: %s\n", why);
		return false;
	}
	return true;
}

extern "C" uint32_t
rwm_texture_in_place(const struct rwm_texture *texture)
{
	return texture->in_place;
}

extern "C" void
rwm_texture_set_in_place(struct rwm_texture *texture, uint32_t surface)
{
	texture->in_place = surface;
	texture->origin = 1;
}

/*
 * What the frames sample, by where each texture came from, and the ones sampled with nothing ever put into them --
 * a layer drawn as nothing (the 27 release's home screen: its stripes and none of its images). Counted per draw at
 * recording, said every five seconds by rwm_perf_report ("rwm sampled"), the empty ones by their shape.
 */
namespace {

struct sampled_counts {
	unsigned long by_origin[5], empty_by_origin[5];
	struct { const rwm_texture *t; unsigned w, h, f; uint8_t origin; } empty[8];
	unsigned empty_count;
} sampled;
pthread_mutex_t sampled_lock = PTHREAD_MUTEX_INITIALIZER;

} // namespace

namespace {
void texshot(struct rwm_texture *texture, unsigned samples, bool by_fill = false, bool after = false);
} // namespace

extern "C" void
rwm_texture_note_filled(struct rwm_texture *texture)
{
	if (texture != nullptr) {
		texture->filled = true;
		texture->fill_kind |= 1;
		const unsigned f = texture->shape.pixel_format;
		/* by the passes drawing into it: RGBA16Float / R32Float attachments, and a size asked for (rwm_texshot=WxH),
		 * which is how a surface drawn in place and made anew for each frame is ever taken */
		texshot(texture, ++texture->fills, true);
	}
}

extern "C" void
rwm_texture_note_view(struct rwm_texture *texture)
{
	texture->origin = 4;
	texture->filled = true;           /* (what is in it is its parent's) */
	texture->fill_kind |= 8;
}

/*
 * Boot-arg rwm_texshot=WxH: the first texture of that size in format 80 (BGRA8Unorm) that is sampled 30 times is read
 * back from the GPU and written into the host's kept lines as a small picture (48 pixels wide, red green blue alpha,
 * base64: "!!texshot k: ..."; tools/log/texshot.py makes a PNG of it), with how many of its pixels have any alpha. With
 * rwa_tint=1 the 27 release's icons and widgets came on the screen as magenta rounded rectangles and without it as
 * nothing (run 70): the draws that put them on the screen work and what they sample -- a texture the window server
 * drew them into first, 129 x 158 for an icon and its name (rwm_capture, run 66) -- holds nothing to see. This says
 * what it holds. The 30th sample and not the first, so that the pass that draws it has been run.
 */
namespace {

/*
 * ... and rwm_texshot=N (no "x"): N different textures, any size from 32 x 32 in a four-byte format (RGBA8 70/71,
 * BGRA8 80/81), that reach their 30th sample once 100 s have passed since the first draw of this process -- the home
 * screen's and not the boot logo's (the 129 x 158 of run 71 was the Apple logo, whole) -- 32 pixels wide each, as
 * "texshot#k" so that tools/log/texshot.py can tell them apart. Each one's header says what filled it (fill_kind).
 */
/* boot-arg rwm_texshot_at=SECONDS: how long after this process's first draw a texshot may be taken (100 s by default;
 * the home screen's icons come at about 295 s, 15 November) */
} /* namespace */
bool rwm_texshot_marked;
namespace {
uint64_t
texshot_after_ns(void)
{
	static uint64_t ns;

	if (ns == 0) {
		const char *at = strstr(rwm_boot_args(), "rwm_texshot_at=");
		unsigned s = 100;

		if (at != nullptr) {
			sscanf(at + 15, "%u", &s);
		}
		ns = (uint64_t)s * 1000000000ull;
	}
	return ns;
}

void
texshot(struct rwm_texture *texture, unsigned samples, bool by_fill, bool after)
{
	static unsigned want_w, want_h, want_n, shots;
	static unsigned sizes[4][2], nsizes;
	static int read = 0;
	static uint64_t first_ns;
	char line[240];

	if (!read) {
		const char *at = strstr(rwm_boot_args(), "rwm_texshot=");

		read = 1;
		first_ns = rwm_ns();
		/* WxH, or several joined by '+': 750x1624+128x64 */
		for (const char *p = at != nullptr ? at + 12 : nullptr; p != nullptr && nsizes < 4 &&
		     sscanf(p, "%ux%u", &sizes[nsizes][0], &sizes[nsizes][1]) == 2;) {
			nsizes++;
			p = strchr(p, '+');
			p = p != nullptr && p[1] >= '0' && p[1] <= '9' ? p + 1 : nullptr;
		}
		if (nsizes != 0) {
			want_w = sizes[0][0];
			want_h = sizes[0][1];
		} else {
			want_w = want_h = 0;
			if (at == nullptr || sscanf(at + 12, "%u", &want_n) != 1) {
				want_n = 0;
			}
		}
	}
	const unsigned f = texture->shape.pixel_format;
	/* sizes asked for are taken after the GPU drew them (rwm_texshot_after, from src/metal/run.cpp): read while the
	 * draws are recorded, a target holds what it held before them -- zeros for a surface made anew (runs 81 to 86) */
	if (want_w != 0 && (!after || samples < 3)) {
		return;
	}
	/* a drawn RGBA16Float / R32Float attachment is often made anew each frame: its third pass */
	if (want_w == 0 && (by_fill ? samples != 3 || (f != 115 && f != 55) : samples != 30)) {
		return;
	}
	if (want_w != 0) {
		/* one of BGRA8 (by its samples) and one of RGBA16Float (by the passes drawing into it) */
		static unsigned took[4][2];   /* (up to six of each: the surfaces of one frame and the next, 15 November) */
		unsigned k = 0;

		while (k < nsizes && (texture->shape.width != sizes[k][0] || texture->shape.height != sizes[k][1])) {
			k++;
		}
		const bool by_draw = strstr(rwm_boot_args(), "rwm_texshot_draw=") != nullptr;

		if (k == nsizes || (f != 80 && f != 115) || took[k][f == 115] >= 6 ||
		    (by_draw ? !rwm_texshot_marked : rwm_ns() - first_ns < texshot_after_ns())) {
			return;
		}
		took[k][f == 115]++;
	} else if (shots >= want_n || texture->shape.width < 32 || texture->shape.height < 32 ||
		   (f != 70 && f != 71 && f != 80 && f != 81 && f != 115 && f != 55) ||
		   rwm_ns() - first_ns < texshot_after_ns()) {
		return;
	}
	const unsigned shot = shots++;
	const bool bgra = f == 80 || f == 81;
	/* RGBA16Float (8 bytes) and R32Float (4) -- the second and third attachments of the 27 release's layer passes --
	 * are read whole and made RGBA8 here: half clamped to 0..1, the float as grey with alpha 255 where it is not 0 */
	const unsigned bpp = f == 115 ? 8 : 4;

	const unsigned w = texture->shape.width, h = texture->shape.height, cap = want_w != 0 ? 30 : 32;   /* (30: six of them fit the host's kept lines) */
	const unsigned ow = w < cap ? w : cap, oh = (h * ow + w - 1) / w;
	uint8_t *pixels = (uint8_t *)calloc((size_t)w * h, bpp);
	unsigned origin[3] = { 0, 0, 0 }, size[3] = { w, h, 1 };
	unsigned long lit = 0, sum[4] = { 0, 0, 0, 0 };

	/* a texture lying over a surface is read from the surface's own memory: what the library reads back of one is
	 * zeros while the display shows its picture (run 85) */
	bool from_surface = false;

	if (pixels != nullptr && texture->in_place != 0) {
		IOSurfaceRef sf = IOSurfaceLookup(texture->in_place);

		if (sf != nullptr) {
			if (IOSurfaceLock(sf, kIOSurfaceLockReadOnly, nullptr) == 0) {
				const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(sf);
				const size_t row = IOSurfaceGetBytesPerRow(sf);

				if (base != nullptr && row >= (size_t)w * bpp && IOSurfaceGetHeight(sf) >= h) {
					/* the GPU's picture is in memory and this mapping is cached: lines read before would give the
					 * old pixels (zeros), so they are cleaned and invalidated first -- the processor does not write
					 * these surfaces, so nothing old is written back */
					for (uintptr_t line = (uintptr_t)base & ~(uintptr_t)63; line < (uintptr_t)base + (size_t)h * row;
					     line += 64) {
						__asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
					}
					__asm__ volatile("dsb sy" ::: "memory");
					for (unsigned y = 0; y < h; y++) {
						memcpy(pixels + (size_t)y * w * bpp, base + (size_t)y * row, (size_t)w * bpp);
					}
					from_surface = true;
				}
				IOSurfaceUnlock(sf, kIOSurfaceLockReadOnly, nullptr);
			}
			CFRelease(sf);
		}
	}
	if (pixels == nullptr || (!from_surface && !rwm_texture_read(texture, 0, 0, origin, size, pixels, (size_t)w * bpp))) {
		free(pixels);
		return;
	}
	if (f == 115 || f == 55) {
		for (size_t i = 0; i < (size_t)w * h; i++) {
			uint8_t out[4];

			for (unsigned c = 0; c < 4; c++) {
				float v;

				if (f == 115) {
					uint16_t hv;

					memcpy(&hv, pixels + i * 8 + c * 2, 2);
					v = (float)*(const __fp16 *)&hv;
				} else {
					memcpy(&v, pixels + i * 4, 4);
					if (c == 3) {
						v = v != 0.0f ? 1.0f : 0.0f;
					}
				}
				out[c] = (uint8_t)(v != v || v <= 0.0f ? 0 : v >= 1.0f ? 255 : v * 255.0f + 0.5f);
			}
			memcpy(pixels + i * 4, out, 4);
		}
	}
	for (size_t i = 0; i < (size_t)w * h; i++) {
		lit += pixels[i * 4 + 3] != 0;
		for (unsigned c = 0; c < 4; c++) {
			sum[c] += pixels[i * 4 + c];
		}
	}
	snprintf(line, sizeof(line), "!!texshot#%u begin %u %u (of %ux%u format %u, origin %u, filled by %#x%s): %lu of %u "
		 "pixels with alpha, average %lu %lu %lu a %lu", shot, ow, oh, w, h, f, texture->origin, texture->fill_kind, from_surface ? ", read from its surface" : "", lit,
		 w * h, sum[0] / (w * h), sum[1] / (w * h), sum[2] / (w * h), sum[3] / (w * h));
	sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));

	/* nearest pixel, as R G B A, 30 pixels (120 bytes) a line */
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint8_t *small = (uint8_t *)malloc((size_t)ow * oh * 4);

	for (unsigned y = 0; small != nullptr && y < oh; y++) {
		for (unsigned x = 0; x < ow; x++) {
			const uint8_t *p = pixels + (((size_t)(y * h / oh) * w + x * w / ow) * 4);
			uint8_t *q = small + ((size_t)y * ow + x) * 4;

			q[0] = bgra ? p[2] : p[0];
			q[1] = p[1];
			q[2] = bgra ? p[0] : p[2];
			q[3] = p[3];
		}
	}
	for (size_t at = 0, k = 0; small != nullptr && at < (size_t)ow * oh * 4; at += 120, k++) {
		const size_t end = at + 120 < (size_t)ow * oh * 4 ? at + 120 : (size_t)ow * oh * 4;
		int n = snprintf(line, sizeof(line), "!!texshot#%u %zu: ", shot, k);

		for (size_t i = at; i < end; i += 3) {
			const uint32_t v = (uint32_t)small[i] << 16 | (i + 1 < end ? (uint32_t)small[i + 1] << 8 : 0) |
					   (i + 2 < end ? small[i + 2] : 0);

			line[n++] = digits[v >> 18 & 63];
			line[n++] = digits[v >> 12 & 63];
			line[n++] = i + 1 < end ? digits[v >> 6 & 63] : '=';
			line[n++] = i + 2 < end ? digits[v & 63] : '=';
		}
		line[n] = 0;
		sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
	}
	snprintf(line, sizeof(line), "!!texshot#%u end", shot);
	sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
	free(small);
	free(pixels);
}

} // namespace

/* boot-arg rwm_texshot=WxH[+WxH...]: true, and the command buffer is then waited for so that its targets can be read */
extern "C" bool
rwm_texshot_sizes(void)
{
	static int on = -1;

	if (on < 0) {
		const char *at = strstr(rwm_boot_args(), "rwm_texshot=");
		unsigned w, h;

		on = at != nullptr && sscanf(at + 12, "%ux%u", &w, &h) == 2;
	}
	return on == 1;
}

/* rwm_drawshot (src/metal/run.cpp): a copy of what a target holds now, from its surface when it lies over one, else read
 * back from the GPU; NULL when it cannot be read. The caller frees it. */
extern "C" uint8_t *
rwm_texture_snapshot(struct rwm_texture *texture, size_t *bytes)
{
	const unsigned w = texture->shape.width, h = texture->shape.height;
	const size_t bpp = texel_bytes(texture->shape.pixel_format);
	uint8_t *pixels;

	if (w == 0 || h == 0 || bpp == 0 || (pixels = (uint8_t *)malloc((size_t)w * h * bpp)) == nullptr) {
		return nullptr;
	}
	*bytes = (size_t)w * h * bpp;
	/* the GPU's resource first: a surface's own memory may be written back only at the command buffer's end */
	{
		unsigned o[3] = { 0, 0, 0 }, z[3] = { w, h, 1 };

		if (rwm_texture_read(texture, 0, 0, o, z, pixels, (size_t)w * bpp)) {
			return pixels;
		}
	}
	if (texture->in_place != 0) {
		IOSurfaceRef sf = IOSurfaceLookup(texture->in_place);
		bool ok = false;

		if (sf != nullptr) {
			if (IOSurfaceLock(sf, kIOSurfaceLockReadOnly, nullptr) == 0) {
				const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(sf);
				const size_t row = IOSurfaceGetBytesPerRow(sf);

				if (base != nullptr && row >= (size_t)w * bpp && IOSurfaceGetHeight(sf) >= h) {
					for (uintptr_t line = (uintptr_t)base & ~(uintptr_t)63; line < (uintptr_t)base + (size_t)h * row;
					     line += 64) {
						__asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
					}
					__asm__ volatile("dsb sy" ::: "memory");
					for (unsigned y = 0; y < h; y++) {
						memcpy(pixels + (size_t)y * w * bpp, base + (size_t)y * row, (size_t)w * bpp);
					}
					ok = true;
				}
				IOSurfaceUnlock(sf, kIOSurfaceLockReadOnly, nullptr);
			}
			CFRelease(sf);
		}
		if (!ok) {
			free(pixels);
			return nullptr;
		}
		return pixels;
	}
	unsigned origin[3] = { 0, 0, 0 }, size[3] = { w, h, 1 };

	if (!rwm_texture_read(texture, 0, 0, origin, size, pixels, (size_t)w * bpp)) {
		free(pixels);
		return nullptr;
	}
	return pixels;
}

/* what filled a texture (rwm_noimg, src/metal/render.cpp): 1 drawn into, 2 written, 4 copied in, 8 a view */
extern "C" unsigned
rwm_texture_fill_kind(const struct rwm_texture *texture)
{
	return texture != nullptr ? texture->fill_kind : 0;
}

/* a target of a command buffer the GPU has finished */
extern "C" void
rwm_texshot_after(struct rwm_texture *texture)
{
	if (texture != nullptr) {
		texshot(texture, texture->fills, true, true);
	}
}

extern "C" void
rwm_texture_note_sampled(struct rwm_texture *texture)
{
	bool empty;

	if (texture == nullptr) {
		return;
	}
	texshot(texture, ++texture->samples);
	if (texture->origin == 1 && texture->surface_empty < 0) {
		IOSurfaceRef s = IOSurfaceLookup(texture->in_place);
		unsigned nonzero = 0;

		if (s != nullptr) {
			const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(s);
			const size_t size = IOSurfaceGetAllocSize(s);

			for (unsigned i = 0; base != nullptr && size >= 4 && i < 64; i++) {
				uint32_t word;

				memcpy(&word, base + ((size - 4) * i / 63 & ~(size_t)3), 4);
				nonzero += word != 0;
			}
			CFRelease(s);
		}
		texture->surface_empty = nonzero == 0;
	}
	empty = texture->origin == 1 ? texture->surface_empty == 1 : !texture->filled;
	pthread_mutex_lock(&sampled_lock);
	sampled.by_origin[texture->origin]++;
	if (empty) {
		unsigned k;

		sampled.empty_by_origin[texture->origin]++;
		for (k = 0; k < sampled.empty_count && sampled.empty[k].t != texture; k++) {
		}
		if (k == sampled.empty_count && k < 8) {
			sampled.empty[k] = { texture, texture->shape.width, texture->shape.height, texture->shape.pixel_format,
					     texture->origin };
			sampled.empty_count++;
		}
	}
	pthread_mutex_unlock(&sampled_lock);
}

extern "C" void
rwm_texture_sample_report(void (*say)(const char *))
{
	static const char *const names[5] = { "made", "surface", "copied", "memory", "view" };
	char line[240];
	int at;

	pthread_mutex_lock(&sampled_lock);
	sampled_counts now = sampled;
	memset(&sampled, 0, sizeof(sampled));
	pthread_mutex_unlock(&sampled_lock);
	at = snprintf(line, sizeof(line), "rwm sampled 5s:");
	for (unsigned o = 0; o < 5; o++) {
		at += snprintf(line + at, sizeof(line) - at, " %s %lu (%lu empty)", names[o], now.by_origin[o],
			       now.empty_by_origin[o]);
	}
	say(line);
	for (unsigned k = 0; k < now.empty_count; k++) {
		snprintf(line, sizeof(line), "rwm sampled empty %u: %ux%u format %u, %s", k + 1, now.empty[k].w, now.empty[k].h,
			 now.empty[k].f, names[now.empty[k].origin]);
		say(line);
	}
}

extern "C" int
rwm_texture_sync_surface(struct rwm_texture *texture, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();
	const unsigned origin[3] = { 0, 0, 0 };
	const unsigned size[3] = { texture->shape.width, texture->shape.height, 1 };
	const size_t row = (size_t)texture->shape.width * 4, rows = texture->shape.height;
	IOSurfaceRef surface = texture->surface;
	char inner[256] = { 0 };

	if (surface == nullptr || air->texture_read == nullptr) {
		return 0;
	}
	/* one buffer per call: the window server commits from more than one thread, and a buffer
	   shared between them was freed under a copy (signal 11 in memmove) */
	block scratch((uint8_t *)malloc(row * rows));

	if (scratch == nullptr || air->texture_read(texture->texture, 0, 0, origin, size, scratch.get(),
						     row /* bytes per row */, inner, sizeof(inner)) != 0) {
		snprintf(why, why_size, "the frame could not be read: %s", inner);
		return -1;
	}

	surface_lock lock(surface, 0);

	if (!lock.held()) {
		snprintf(why, why_size, "the surface could not be locked");
		return -1;
	}
	/* a surface without planes answers the plane calls with zero: the whole-surface ones */
	const bool planar = IOSurfaceGetPlaneCount(surface) > 0;
	uint8_t *base = (uint8_t *)(planar ? IOSurfaceGetBaseAddressOfPlane(surface, 0)
					   : IOSurfaceGetBaseAddress(surface));
	const size_t pitch = planar ? IOSurfaceGetBytesPerRowOfPlane(surface, 0) : IOSurfaceGetBytesPerRow(surface);
	const size_t height = planar ? IOSurfaceGetHeightOfPlane(surface, 0) : IOSurfaceGetHeight(surface);
	const size_t alloc = IOSurfaceGetAllocSize(surface), copy = row < pitch ? row : pitch;

	for (size_t y = 0; base != nullptr && pitch != 0 && y < rows && y < height &&
	     y * pitch + copy <= alloc; y++) {
		memcpy(base + y * pitch, scratch.get() + y * row, copy);
	}
	rwm_surface_to_memory(surface);     /* (out of this cache into memory, where the display and the GPU read) */
	return 0;
}

/* MTLStorageModeMemoryless: what is drawn into it is never stored, whatever the pass's store action says (17 November:
 * the 27 release's window server asks for its passes' RGBA16Float and R32Float attachments memoryless and stores
 * them, and the Mali wrote both back and loaded them again in every pass -- 12 ms a full-screen fragment job) */
extern "C" void
rwm_texture_set_memoryless(struct rwm_texture *texture, bool memoryless)
{
	if (texture != nullptr) {
		texture->memoryless = memoryless;
	}
}

extern "C" bool
rwm_texture_memoryless(const struct rwm_texture *texture)
{
	return texture != nullptr && texture->memoryless;
}
