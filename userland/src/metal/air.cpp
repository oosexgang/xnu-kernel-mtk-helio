/*
 * The compiler library, loaded with dlopen rather than linked, for a measured reason: the device
 * object must exist even when there is no GPU, and a device that refuses to be created at all takes
 * Metal's whole discovery down with it (the comment at the top of src/hakiosmetal.m).
 *
 * Which one: with boot-arg mali_native=1 the native library (userland/src/mali/rwa), if it serves this
 * phone's GPU -- its rwa_device_open is asked here, before the device is made, and says why when it does
 * not (a Mali of another architecture than Bifrost v7: rwa_helio.c). It is then left loaded but unused, and
 * the Gallium library takes its place: Mesa's panfrost knows every Mali from the T600 to the G57, so a phone
 * whose GPU the native library has not learned still draws (Documentation/metal-porting.md). Without
 * mali_native=1, the Gallium library.
 */
#include "rwm.h"
#include <sys/sysctl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <dlfcn.h>
#include <mutex>

#define NATIVE_LIBRARY  "/hakios/lib/libhakiosmali.dylib"
#define GALLIUM_LIBRARY "/hakios/lib/libhakiosair.dylib"

namespace {
const char *library_path;               /* the library in use, once load() has chosen */
void load();
std::once_flag once;
}

extern "C" const char *
rwm_air_library_path(void)
{
	std::call_once(once, load);
	return library_path;
}

namespace {


struct Call {
	const char *name;
	void **slot;
	bool required;
};

struct rwm_air calls;
bool loaded;

template <typename F>
Call
call(const char *name, F *slot, bool required)
{
	return Call{ name, reinterpret_cast<void **>(slot), required };
}

/* the library at `path` into `calls`, every entry looked up; false when it is not there or too old */
bool
take(const char *path)
{
	void *handle = dlopen(path, RTLD_NOW);

	if (handle == nullptr) {
		rwm_print("no compiler library: %s\n", dlerror());
		return false;
	}

	struct rwm_air &c = calls;
	const Call table[] = {
		call("rwa_device_open", &c.device_open, true),
		call("rwa_device_close", &c.device_close, false),
		call("rwa_device_name", &c.device_name, false),
		call("rwa_buffer_create", &c.buffer_create, false),
		call("rwa_buffer_map", &c.buffer_map, false),
		call("rwa_texture_set_swizzle", &c.texture_set_swizzle, false),
		call("rwa_buffer_size", &c.buffer_size, false),
		call("rwa_buffer_destroy", &c.buffer_destroy, false),
		call("rwa_compute_create", &c.compute_create, true),
		call("rwa_compute_destroy", &c.compute_destroy, false),
		call("rwa_texture_create", &c.texture_create, true),
		call("rwa_texture_write", &c.texture_write, false),
		call("rwa_texture_read", &c.texture_read, false),
		call("rwa_texture_copy", &c.texture_copy, false),
		call("rwa_buffer_copy", &c.buffer_copy, false),
		call("rwa_buffer_fill", &c.buffer_fill, false),
		call("rwa_texture_mipmaps", &c.texture_mipmaps, false),
		call("rwa_render_clear", &c.render_clear, false),
		call("rwa_depth_read", &c.depth_read, false),
		call("rwa_texture_destroy", &c.texture_destroy, false),
		call("rwa_texture_view_create", &c.texture_view_create, false),
		call("rwa_sampler_create", &c.sampler_create, true),
		call("rwa_sampler_destroy", &c.sampler_destroy, false),
		call("rwa_dispatch", &c.dispatch, true),
		/* argument buffers, drawing and depth: looked up, not required -- the plug-in checks
		 * for them before it answers an encoder, a pipeline or a depth state */
		call("rwa_dispatch_buffers", &c.dispatch_buffers, false),
		call("rwa_dispatch_threadgroup_memory", &c.dispatch_threadgroup_memory, false),
		call("rwa_module_argbufs", &c.module_argbufs, false),
		call("rwa_argbuf_encode_buffer", &c.argbuf_encode_buffer, false),
		call("rwa_argbuf_encode_texture", &c.argbuf_encode_texture, false),
		call("rwa_argbuf_encode_sampler", &c.argbuf_encode_sampler, false),
		call("rwa_argbuf_encoded_members", &c.argbuf_encoded_members, false),
		call("rwa_render_constants", &c.render_constants, false),
		call("rwa_render_config", &c.render_config, false),
		call("rwa_render_create", &c.render_create, false),
		call("rwa_render_destroy", &c.render_destroy, false),
		call("rwa_render_outputs", &c.render_outputs, false),
		call("rwa_render_draw", &c.render_draw, false),
		call("rwa_depth_stencil_create", &c.depth_stencil_create, false),
		call("rwa_depth_stencil_destroy", &c.depth_stencil_destroy, false),
		call("rwa_finish", &c.finish, false),
		call("rwa_flush_seq", &c.flush_seq, false),
		call("rwa_wait_seq", &c.wait_seq, false),
		call("rwa_surface_seq", &c.surface_seq, false),
	};

	for (const Call &entry : table) {
		*entry.slot = dlsym(handle, entry.name);
		if (entry.required && *entry.slot == nullptr) {
			/* rwa_dispatch's arguments changed when textures were added to the contract, so
			 * an older library would be called with the wrong shape of argument and do
			 * something arbitrary: refusing it outright is the only safe reading */
			rwm_print("the compiler library has no %s, so it is older than this driver and "
				  "will not be used\n", entry.name);
			calls = {};
			dlclose(handle);
			return false;
		}
	}
	c.handle = handle;
	library_path = path;
	loaded = true;
	/* where it lies in this process, into the host's kept lines: the kernel's stack samples (ustacks=) give bare
	 * addresses, and this is what turns those in the library into its own functions (atos -l BASE) */
	{
		Dl_info info;

		if (c.device_open != nullptr && dladdr(reinterpret_cast<const void *>(c.device_open), &info) != 0) {
			char line[200];

			snprintf(line, sizeof(line), "!rwm %s: the compiler library at %p (%s)", getprogname(), info.dli_fbase,
				 info.dli_fname != nullptr ? info.dli_fname : "?");
			sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
		}
	}
	return true;
}

void
load()
{
	library_path = GALLIUM_LIBRARY;
	if (rwm_boot_arg("mali_native=1") && take(NATIVE_LIBRARY)) {
		char why[256] = "";

		/* the native library's device is one a process: asked for again by the device, it is this one */
		if (calls.device_open(why, sizeof(why)) != nullptr) {
			return;
		}
		char line[400];

		snprintf(line, sizeof(line), "!!rwm %s: the native Mali library does not serve this GPU (%s): Mesa's "
			 "panfrost in its place", getprogname(), why);
		rwm_print("%s\n", line + 2);
		sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
		calls = {};
		loaded = false;
		library_path = GALLIUM_LIBRARY;
	}
	(void)take(GALLIUM_LIBRARY);
}

} // namespace

extern "C" bool
rwm_air_available(void)
{
	std::call_once(once, load);
	return loaded;
}

extern "C" const struct rwm_air *
rwm_air(void)
{
	std::call_once(once, load);
	return &calls;
}
