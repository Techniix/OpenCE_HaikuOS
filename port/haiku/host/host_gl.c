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
#include <string.h>

#define MAXIMUM_GL_IMPORTS 256

static struct
{
	uint64_t *slot;
	const char *name;
} gl_imports[MAXIMUM_GL_IMPORTS];
static int gl_import_count;

/* the driver's function of this name: SDL's lookup, then Mesa's own. On
Haiku, SDL looks only among the names libGL exports (get_image_symbol), and
Mesa's libGL exports no newer function (glClipControl, glCreateQueries):
_glapi_get_proc_address, in libglapi, which libGL itself asks, has them all */
static int glapi_found;

static SDL_FunctionPointer gl_lookup(const char *name)
{
	static int tried;
	static void *(*glapi_get_proc_address)(const char *);
	SDL_FunctionPointer function = SDL_GL_GetProcAddress(name);

	if (function)
		return function;
	if (!tried)
	{
		static const char *const libraries[] = { "libglapi.so.0", "libglapi.so", "libGL.so.1" };
		size_t index;

		tried = 1;
		for (index = 0; index < sizeof(libraries) / sizeof(libraries[0]) && !glapi_get_proc_address; index++)
		{
			void *library = dlopen(libraries[index], RTLD_NOW | RTLD_LOCAL);

			if (library)
				glapi_get_proc_address = (void *(*)(const char *))dlsym(library, "_glapi_get_proc_address");
		}
	}
	if (glapi_get_proc_address && (function = (SDL_FunctionPointer)glapi_get_proc_address(name)) != NULL)
		glapi_found++;
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
	if (glapi_found)
		host_logf(HOST_LOG_INFO, "OpenGL: %d of %d functions found through Mesa's glapi", glapi_found,
			gl_import_count);
	if (missing)
		host_logf(HOST_LOG_WARN, "OpenGL: %d of %d functions are missing", missing, gl_import_count);
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
