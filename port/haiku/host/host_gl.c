/*
HOST_GL.C

OpenGL for the guest. Its generated entry points (guest_gl.c,
tools/haiku_gl_stubs.py) import hostgl_<function>, which the host fills in
with the driver's function once a context exists (SDL_GL_GetProcAddress
needs one on some systems); the arguments already have host types by then.
What returns host memory is handled here: strings are copied, and GLsync
objects get small handles.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAXIMUM_GL_IMPORTS 256

static struct
{
	uint64_t *slot;
	const char *name;
} gl_imports[MAXIMUM_GL_IMPORTS];
static int gl_import_count;

/* the driver's function of this name. SDL's lookup first; on Haiku it looks
only among the names libGL exports (get_image_symbol), and neither Mesa's
libGL nor glvnd's exports the newer functions (glClipControl,
glCreateQueries). Then the driver's own lookups:
- Mesa's _glapi_get_proc_address, in libglapi, which Mesa's libGL itself
  asks: Haiku's own OpenGL (llvmpipe);
- eglGetProcAddress, in libEGL: glvnd's, which Nebula (Mesa's NVK and Zink
  for NVIDIA GPUs) puts in place of Haiku's Mesa, and which Mesa's libEGL
  also answers.
HALO_GL_LOOKUP ("sdl", "glapi" or "egl") takes one of them alone, to compare
them on a driver. */
enum
{
	_gl_source_sdl,
	_gl_source_glapi,
	_gl_source_egl,
	NUMBER_OF_GL_SOURCES
};

static const char *const gl_source_names[NUMBER_OF_GL_SOURCES] = { "SDL", "Mesa's glapi", "EGL" };
static int gl_found[NUMBER_OF_GL_SOURCES];

static void *library_function(const char *const *libraries, size_t count, const char *name)
{
	size_t index;

	for (index = 0; index < count; index++)
	{
		void *library = dlopen(libraries[index], RTLD_NOW | RTLD_LOCAL);
		void *function = library ? dlsym(library, name) : NULL;

		if (function)
			return function;
	}
	return NULL;
}

static SDL_FunctionPointer gl_lookup(const char *name)
{
	static int initialized;
	static int only = -1;
	static void *(*glapi_get_proc_address)(const char *);
	static void *(*egl_get_proc_address)(const char *);
	SDL_FunctionPointer function = NULL;
	int source;

	if (!initialized)
	{
		static const char *const glapi_libraries[] = { "libglapi.so.0", "libglapi.so", "libGL.so.1" };
		static const char *const egl_libraries[] = { "libEGL.so.1", "libEGL.so" };
		const char *choice = getenv("HALO_GL_LOOKUP");

		initialized = 1;
		for (source = 0; choice && *choice && source < NUMBER_OF_GL_SOURCES; source++)
		{
			static const char *const choices[NUMBER_OF_GL_SOURCES] = { "sdl", "glapi", "egl" };

			if (!strcmp(choice, choices[source]))
				only = source;
		}
		if (choice && *choice && only < 0)
			host_logf(HOST_LOG_WARN, "HALO_GL_LOOKUP is \"%s\", not sdl, glapi or egl: ignored", choice);
		glapi_get_proc_address = (void *(*)(const char *))library_function(glapi_libraries,
			sizeof(glapi_libraries) / sizeof(glapi_libraries[0]), "_glapi_get_proc_address");
		egl_get_proc_address = (void *(*)(const char *))library_function(egl_libraries,
			sizeof(egl_libraries) / sizeof(egl_libraries[0]), "eglGetProcAddress");
		host_logf(HOST_LOG_INFO, "OpenGL lookups: SDL, Mesa's glapi %s, EGL %s%s%s", glapi_get_proc_address ?
			"found" : "not found", egl_get_proc_address ? "found" : "not found", only >= 0 ? "; only " : "",
			only >= 0 ? gl_source_names[only] : "");
	}
	for (source = 0; source < NUMBER_OF_GL_SOURCES && !function; source++)
	{
		if (only >= 0 && source != only)
			continue;
		switch (source)
		{
		case _gl_source_sdl:
			function = SDL_GL_GetProcAddress(name);
			break;
		case _gl_source_glapi:
			function = glapi_get_proc_address ? (SDL_FunctionPointer)glapi_get_proc_address(name) : NULL;
			break;
		case _gl_source_egl:
			function = egl_get_proc_address ? (SDL_FunctionPointer)egl_get_proc_address(name) : NULL;
			break;
		}
		if (function)
			gl_found[source]++;
	}
	return function;
}

static void unbound_gl(void)
{
	host_fatal("the guest called OpenGL without a context");
}

void host_gl_note_import(uint64_t *slot, const char *name)
{
	*slot = (uint64_t)(uintptr_t)unbound_gl;
	if (gl_import_count == MAXIMUM_GL_IMPORTS)
		host_fatal("too many OpenGL imports");
	gl_imports[gl_import_count].slot = slot;
	gl_imports[gl_import_count].name = name;
	gl_import_count++;
}

static void host_gl_log_driver(void);

int host_gl_bind_imports(void)
{
	int index, missing = 0;

	for (index = 0; index < gl_import_count; index++)
	{
		SDL_FunctionPointer function = gl_lookup(gl_imports[index].name);

		if (!function)
		{
			/* (the guest's renderer checks for the ones it may lack: as
			NULL in its table, which gl_functions_load finds) */
			missing++;
			continue;
		}
		*gl_imports[index].slot = (uint64_t)(uintptr_t)function;
	}
	host_logf(HOST_LOG_INFO, "OpenGL: %d functions from SDL, %d from Mesa's glapi, %d from EGL, %d missing",
		gl_found[_gl_source_sdl], gl_found[_gl_source_glapi], gl_found[_gl_source_egl], missing);
	if (missing)
		host_logf(HOST_LOG_WARN, "OpenGL: %d of %d functions are missing", missing, gl_import_count);
	host_gl_log_driver();
	return missing;
}

/* the driver's functions this file calls itself */
static const GLubyte *(APIENTRY *get_string)(GLenum);
static const GLubyte *(APIENTRY *get_string_index)(GLenum, GLuint);
static void (APIENTRY *get_integer)(GLenum, GLint *);
static GLsync (APIENTRY *fence_sync)(GLenum, GLbitfield);
static GLenum (APIENTRY *client_wait_sync)(GLsync, GLbitfield, GLuint64);
static void (APIENTRY *delete_sync)(GLsync);
static void (APIENTRY *debug_message_callback)(GLDEBUGPROC, const void *);

static void resolve(void)
{
	if (get_string)
		return;
	get_string = (const GLubyte *(APIENTRY *)(GLenum))gl_lookup("glGetString");
	get_string_index = (const GLubyte *(APIENTRY *)(GLenum, GLuint))gl_lookup("glGetStringi");
	get_integer = (void (APIENTRY *)(GLenum, GLint *))gl_lookup("glGetIntegerv");
	fence_sync = (GLsync (APIENTRY *)(GLenum, GLbitfield))gl_lookup("glFenceSync");
	client_wait_sync = (GLenum (APIENTRY *)(GLsync, GLbitfield, GLuint64))gl_lookup("glClientWaitSync");
	delete_sync = (void (APIENTRY *)(GLsync))gl_lookup("glDeleteSync");
	debug_message_callback = (void (APIENTRY *)(GLDEBUGPROC, const void *))
		gl_lookup("glDebugMessageCallback");
}

/* which driver draws: the vendor, renderer and version, and whether it is
the processor (Mesa's llvmpipe or softpipe) or a GPU (Zink over NVK on
NVIDIA, with Nebula) */
static void host_gl_log_driver(void)
{
	const char *vendor, *renderer, *version;

	resolve();
	if (!get_string)
		return;
	vendor = (const char *)get_string(GL_VENDOR);
	renderer = (const char *)get_string(GL_RENDERER);
	version = (const char *)get_string(GL_VERSION);
	host_logf(HOST_LOG_INFO, "OpenGL driver: %s, %s, version %s (%s)", vendor ? vendor : "?",
		renderer ? renderer : "?", version ? version : "?",
		renderer && (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")) ?
		"software: drawn by the processor" : "hardware");
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	const GLubyte *text;

	resolve();
	if (!size)
		return;
	buffer[0] = 0;
	text = index >= 0 ? (get_string_index ? get_string_index(name, (GLuint)index) : NULL) :
		(get_string ? get_string(name) : NULL);
	if (text)
	{
		strncpy(buffer, (const char *)text, size - 1);
		buffer[size - 1] = 0;
	}
}

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	resolve();
	if (!get_integer || !get_string_index)
		return 0;
	get_integer(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)get_string_index(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

/* ---------- GLsync objects, as handles (the renderer keeps a few: one for
each frame in flight, and its visibility tests' batches) */

#define SYNC_COUNT 256

static GLsync syncs[SYNC_COUNT];
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t host_gl_fence_sync(uint32_t condition, uint32_t flags)
{
	GLsync sync;
	uint32_t index;

	resolve();
	if (!fence_sync || !(sync = fence_sync(condition, flags)))
		return 0;
	pthread_mutex_lock(&sync_lock);
	for (index = 1; index < SYNC_COUNT; index++)
	{
		if (!syncs[index])
		{
			syncs[index] = sync;
			pthread_mutex_unlock(&sync_lock);
			return index;
		}
	}
	pthread_mutex_unlock(&sync_lock);
	host_logf(HOST_LOG_WARN, "OpenGL: out of fence handles");
	delete_sync(sync);
	return 0;
}

static GLsync sync_get(uint32_t handle)
{
	GLsync sync = NULL;

	if (handle && handle < SYNC_COUNT)
	{
		pthread_mutex_lock(&sync_lock);
		sync = syncs[handle];
		pthread_mutex_unlock(&sync_lock);
	}
	return sync;
}

uint32_t host_gl_client_wait_sync(uint32_t handle, uint32_t flags, unsigned long long timeout)
{
	GLsync sync = sync_get(handle);

	resolve();
	if (!sync || !client_wait_sync)
		return GL_WAIT_FAILED;
	return client_wait_sync(sync, flags, (GLuint64)timeout);
}

void host_gl_delete_sync(uint32_t handle)
{
	GLsync sync = NULL;

	resolve();
	if (handle && handle < SYNC_COUNT)
	{
		pthread_mutex_lock(&sync_lock);
		sync = syncs[handle];
		syncs[handle] = NULL;
		pthread_mutex_unlock(&sync_lock);
	}
	if (sync && delete_sync)
		delete_sync(sync);
}

/* ---------- the driver's debug messages (debug.gl_debug) */

static void APIENTRY debug_message(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
	const GLchar *message, const void *user)
{
	(void)length;
	(void)user;
	host_logf(HOST_LOG_INFO, "GL debug: source 0x%x type 0x%x id %u severity 0x%x: %s", source, type, id, severity,
		message);
}

void host_gl_debug_output(int enabled)
{
	resolve();
	if (debug_message_callback)
		debug_message_callback(enabled ? debug_message : NULL, NULL);
}

int host_gl_available(const char *name)
{
	return gl_lookup(name) != NULL;
}
