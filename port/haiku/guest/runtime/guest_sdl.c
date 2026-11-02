/*
GUEST_SDL.C

The SDL3 functions the platform layer calls (sdl_platform.c, xinput_sdl.c,
dsound_sdl.c, d3d8_gl.c, updater.c and the rest), for the x32 guest. SDL
itself runs in the host (port/haiku/host/host_sdl.c); this is the Android
guest's bridge (port/android/guest/runtime/guest_sdl.c) grown to what the
desktop build calls.

- Objects SDL returns (windows, contexts, gamepads, joysticks, audio
  streams, renderers, cursors) are 64-bit pointers there, so the guest only
  ever sees small integer handles that the host maps back.
- Events are written by SDL straight into the guest's buffer: SDL_Event has
  the same 128-byte layout in both ABIs for every event type that carries
  no pointer, which covers all the platform layer reads (keys, the mouse,
  gamepads, the window); the host clears the pointers of the others.
- What needs no host is done here, with the guest's own C library: threads,
  mutexes and condition variables (musl's pthreads), atomics, the I/O
  streams (stdio) and the properties a caller makes.
- Surfaces live in guest memory, their pixels too: the host reads and
  writes them in place (the guest's addresses are the host's), wrapping
  them in surfaces of its own only for the call.
- Strings SDL returns are copied into buffers of the guest.
*/

/* before SDL: musl's alloca.h must come first */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "guest_host.h"

/* guest/runtime/guest_gl.c (generated) */
SDL_FunctionPointer guest_gl_get_proc_address(const char *name);

#define HANDLE(type, value) ((type *)(uintptr_t)(value))
#define HANDLE_OF(pointer) ((unsigned int)(uintptr_t)(pointer))

/* ---------- errors: the guest's own (SDL_SetError, a failure here), else
the host's */

static __thread char error_text[512];
static __thread int error_local;

bool SDL_SetError(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(error_text, sizeof(error_text), format, arguments);
	va_end(arguments);
	error_local = 1;
	return false;
}

const char *SDL_GetError(void)
{
	if (!error_local)
		host_sdl_get_error(error_text, sizeof(error_text));
	error_local = 0;
	return error_text;
}

/* a host call failed: the host's error is the one to read */
static void error_from_host(void)
{
	error_local = 0;
}

void SDL_free(void *memory)
{
	free(memory);
}

/* ---------- general */

bool SDL_Init(SDL_InitFlags flags)
{
	error_from_host();
	return host_sdl_init(flags) != 0;
}

bool SDL_SetHint(const char *name, const char *value)
{
	return host_sdl_set_hint(name, value) != 0;
}

bool SDL_GetHintBoolean(const char *name, bool default_value)
{
	return host_sdl_get_hint_boolean(name, default_value) != 0;
}

const char *SDL_GetCurrentVideoDriver(void)
{
	static char buffer[64];

	host_sdl_current_video_driver(buffer, sizeof(buffer));
	return buffer[0] ? buffer : NULL;
}

const char *SDL_GetBasePath(void)
{
	static char buffer[1024];

	if (!buffer[0])
		host_sdl_base_path(buffer, sizeof(buffer));
	return buffer[0] ? buffer : NULL;
}

bool SDL_OpenURL(const char *url)
{
	return host_sdl_open_url(url) != 0;
}

/* ---------- time */

Uint64 SDL_GetTicks(void)
{
	return (Uint64)host_sdl_ticks();
}

Uint64 SDL_GetTicksNS(void)
{
	return (Uint64)host_sdl_ticks_ns();
}

void SDL_Delay(Uint32 milliseconds)
{
	host_sdl_delay_ns((long long)milliseconds * 1000000LL);
}

void SDL_DelayPrecise(Uint64 nanoseconds)
{
	host_sdl_delay_ns((long long)nanoseconds);
}

bool SDL_GetCurrentTime(SDL_Time *ticks)
{
	return host_sdl_current_time((long long *)ticks) != 0;
}

/* (SDL_DateTime is made of ints: one layout in both ABIs) */
bool SDL_TimeToDateTime(SDL_Time ticks, SDL_DateTime *date_time, bool local_time)
{
	return host_sdl_time_to_date_time((long long)ticks, date_time, local_time) != 0;
}

/* ---------- threads, with musl's pthreads */

struct SDL_Thread
{
	pthread_t thread;
	SDL_ThreadFunction function;
	void *data;
	int result;
};

static void *thread_main(void *context)
{
	SDL_Thread *thread = context;

	thread->result = thread->function(thread->data);
	return NULL;
}

SDL_Thread *SDL_CreateThreadRuntime(SDL_ThreadFunction function, const char *name, void *data,
	SDL_FunctionPointer begin, SDL_FunctionPointer end)
{
	SDL_Thread *thread = calloc(1, sizeof(*thread));
	pthread_attr_t attributes;
	int error;

	(void)name;
	(void)begin;
	(void)end;
	if (!thread)
	{
		SDL_SetError("out of memory");
		return NULL;
	}
	thread->function = function;
	thread->data = data;
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, 1024 * 1024);
	error = pthread_create(&thread->thread, &attributes, thread_main, thread);
	pthread_attr_destroy(&attributes);
	if (error)
	{
		free(thread);
		SDL_SetError("cannot start a thread (%d)", error);
		return NULL;
	}
	return thread;
}

void SDL_WaitThread(SDL_Thread *thread, int *status)
{
	if (!thread)
		return;
	pthread_join(thread->thread, NULL);
	if (status)
		*status = thread->result;
	free(thread);
}

/* (the thread's structure goes when it ends: pthread_detach frees musl's,
and this one is no longer read) */
void SDL_DetachThread(SDL_Thread *thread)
{
	if (!thread)
		return;
	pthread_detach(thread->thread);
}

SDL_ThreadID SDL_GetCurrentThreadID(void)
{
	return (SDL_ThreadID)host_sdl_thread_id();
}

struct SDL_Mutex
{
	pthread_mutex_t mutex;
};

SDL_Mutex *SDL_CreateMutex(void)
{
	SDL_Mutex *mutex = calloc(1, sizeof(*mutex));
	pthread_mutexattr_t attributes;

	if (!mutex)
		return NULL;
	/* (SDL's mutexes are recursive) */
	pthread_mutexattr_init(&attributes);
	pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&mutex->mutex, &attributes);
	pthread_mutexattr_destroy(&attributes);
	return mutex;
}

void SDL_LockMutex(SDL_Mutex *mutex)
{
	if (mutex)
		pthread_mutex_lock(&mutex->mutex);
}

void SDL_UnlockMutex(SDL_Mutex *mutex)
{
	if (mutex)
		pthread_mutex_unlock(&mutex->mutex);
}

void SDL_DestroyMutex(SDL_Mutex *mutex)
{
	if (!mutex)
		return;
	pthread_mutex_destroy(&mutex->mutex);
	free(mutex);
}

struct SDL_Condition
{
	pthread_cond_t condition;
};

SDL_Condition *SDL_CreateCondition(void)
{
	SDL_Condition *condition = calloc(1, sizeof(*condition));
	pthread_condattr_t attributes;

	if (!condition)
		return NULL;
	pthread_condattr_init(&attributes);
	pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	pthread_cond_init(&condition->condition, &attributes);
	pthread_condattr_destroy(&attributes);
	return condition;
}

void SDL_DestroyCondition(SDL_Condition *condition)
{
	if (!condition)
		return;
	pthread_cond_destroy(&condition->condition);
	free(condition);
}

void SDL_SignalCondition(SDL_Condition *condition)
{
	if (condition)
		pthread_cond_signal(&condition->condition);
}

void SDL_BroadcastCondition(SDL_Condition *condition)
{
	if (condition)
		pthread_cond_broadcast(&condition->condition);
}

bool SDL_WaitConditionTimeout(SDL_Condition *condition, SDL_Mutex *mutex, Sint32 milliseconds)
{
	struct timespec deadline;

	if (!condition || !mutex)
		return false;
	if (milliseconds < 0)
		return pthread_cond_wait(&condition->condition, &mutex->mutex) == 0;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += milliseconds / 1000;
	deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L)
	{
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	return pthread_cond_timedwait(&condition->condition, &mutex->mutex, &deadline) == 0;
}

int SDL_AddAtomicInt(SDL_AtomicInt *atomic, int value)
{
	return __atomic_fetch_add(&atomic->value, value, __ATOMIC_SEQ_CST);
}

int SDL_GetAtomicInt(SDL_AtomicInt *atomic)
{
	return __atomic_load_n(&atomic->value, __ATOMIC_SEQ_CST);
}

int SDL_SetAtomicInt(SDL_AtomicInt *atomic, int value)
{
	return __atomic_exchange_n(&atomic->value, value, __ATOMIC_SEQ_CST);
}

/* ---------- I/O streams and files, with the guest's stdio */

struct SDL_IOStream
{
	FILE *file;
	const unsigned char *memory;
	size_t size, position;
};

SDL_IOStream *SDL_IOFromFile(const char *path, const char *mode)
{
	SDL_IOStream *stream;
	FILE *file = fopen(path, mode);

	if (!file)
	{
		SDL_SetError("cannot open %s: %s", path, strerror(errno));
		return NULL;
	}
	stream = calloc(1, sizeof(*stream));
	if (!stream)
	{
		fclose(file);
		return NULL;
	}
	stream->file = file;
	return stream;
}

SDL_IOStream *SDL_IOFromConstMem(const void *memory, size_t size)
{
	SDL_IOStream *stream = calloc(1, sizeof(*stream));

	if (!stream)
		return NULL;
	stream->memory = memory;
	stream->size = size;
	return stream;
}

size_t SDL_ReadIO(SDL_IOStream *stream, void *buffer, size_t size)
{
	size_t count;

	if (!stream)
		return 0;
	if (stream->file)
		return fread(buffer, 1, size, stream->file);
	count = stream->size - stream->position;
	if (count > size)
		count = size;
	memcpy(buffer, stream->memory + stream->position, count);
	stream->position += count;
	return count;
}

size_t SDL_WriteIO(SDL_IOStream *stream, const void *buffer, size_t size)
{
	if (!stream || !stream->file)
		return 0;
	return fwrite(buffer, 1, size, stream->file);
}

Sint64 SDL_SeekIO(SDL_IOStream *stream, Sint64 offset, SDL_IOWhence whence)
{
	Sint64 position;

	if (!stream)
		return -1;
	if (stream->file)
	{
		int origin = whence == SDL_IO_SEEK_CUR ? SEEK_CUR : whence == SDL_IO_SEEK_END ? SEEK_END : SEEK_SET;

		if (fseeko(stream->file, (off_t)offset, origin) != 0)
			return -1;
		return (Sint64)ftello(stream->file);
	}
	position = whence == SDL_IO_SEEK_CUR ? (Sint64)stream->position + offset :
		whence == SDL_IO_SEEK_END ? (Sint64)stream->size + offset : offset;
	if (position < 0 || position > (Sint64)stream->size)
		return -1;
	stream->position = (size_t)position;
	return position;
}

Sint64 SDL_TellIO(SDL_IOStream *stream)
{
	return SDL_SeekIO(stream, 0, SDL_IO_SEEK_CUR);
}

Sint64 SDL_GetIOSize(SDL_IOStream *stream)
{
	Sint64 here, size;

	if (!stream)
		return -1;
	if (!stream->file)
		return (Sint64)stream->size;
	here = SDL_TellIO(stream);
	size = SDL_SeekIO(stream, 0, SDL_IO_SEEK_END);
	SDL_SeekIO(stream, here, SDL_IO_SEEK_SET);
	return size;
}

bool SDL_CloseIO(SDL_IOStream *stream)
{
	int result = 0;

	if (!stream)
		return true;
	if (stream->file)
		result = fclose(stream->file);
	free(stream);
	return result == 0;
}

/* the whole stream, NUL terminated (as SDL's) */
static void *read_all(SDL_IOStream *stream, size_t *size)
{
	size_t capacity = 65536, length = 0;
	unsigned char *data = malloc(capacity + 1);

	while (data)
	{
		size_t count = SDL_ReadIO(stream, data + length, capacity - length);

		length += count;
		if (length < capacity)
			break;
		capacity *= 2;
		{
			unsigned char *larger = realloc(data, capacity + 1);

			if (!larger)
			{
				free(data);
				data = NULL;
				break;
			}
			data = larger;
		}
	}
	if (data)
	{
		data[length] = 0;
		if (size)
			*size = length;
	}
	return data;
}

void *SDL_LoadFile(const char *path, size_t *size)
{
	SDL_IOStream *stream = SDL_IOFromFile(path, "rb");
	void *data;

	if (!stream)
		return NULL;
	data = read_all(stream, size);
	SDL_CloseIO(stream);
	return data;
}

bool SDL_SaveFile(const char *path, const void *data, size_t size)
{
	SDL_IOStream *stream = SDL_IOFromFile(path, "wb");
	bool written;

	if (!stream)
		return false;
	written = SDL_WriteIO(stream, data, size) == size;
	return SDL_CloseIO(stream) && written;
}

/* (SDL_PathInfo is an enum and 64-bit numbers: one layout in both ABIs) */
bool SDL_GetPathInfo(const char *path, SDL_PathInfo *information)
{
	error_from_host();
	return host_sdl_get_path_info(path, information) != 0;
}

bool SDL_CreateDirectory(const char *path)
{
	error_from_host();
	return host_sdl_create_directory(path) != 0;
}

bool SDL_RemovePath(const char *path)
{
	error_from_host();
	return host_sdl_remove_path(path) != 0;
}

bool SDL_RenamePath(const char *from, const char *to)
{
	error_from_host();
	return host_sdl_rename_path(from, to) != 0;
}

/* the names, NUL-separated, from the host; one allocation holding the
array and the names, as SDL's (the caller frees it with SDL_free) */
char **SDL_GlobDirectory(const char *path, const char *pattern, SDL_GlobFlags flags, int *count)
{
	unsigned int size = 0;
	int found;
	char **result;
	char *names;
	int index;

	if (count)
		*count = 0;
	found = host_sdl_glob_directory(path, pattern, (unsigned int)flags, NULL, 0, &size);
	if (found < 0)
		return NULL;
	result = malloc((size_t)(found + 1) * sizeof(char *) + size + 1);
	if (!result)
		return NULL;
	names = (char *)(result + found + 1);
	found = host_sdl_glob_directory(path, pattern, (unsigned int)flags, names, size + 1, &size);
	if (found < 0)
	{
		free(result);
		return NULL;
	}
	for (index = 0; index < found; index++)
	{
		result[index] = names;
		names += strlen(names) + 1;
	}
	result[found] = NULL;
	if (count)
		*count = found;
	return result;
}

/* ---------- properties: the guest's own (text input's), or the host's (a
window's) */

#define LOCAL_PROPERTIES 0x40000000u
#define MAXIMUM_LOCAL_PROPERTIES 16
#define MAXIMUM_PROPERTY_VALUES 16

struct local_property
{
	char name[64];
	Sint64 number;
	bool boolean;
};

static struct
{
	bool used;
	int count;
	struct local_property values[MAXIMUM_PROPERTY_VALUES];
} local_properties[MAXIMUM_LOCAL_PROPERTIES];
static pthread_mutex_t properties_lock = PTHREAD_MUTEX_INITIALIZER;

static int local_index(SDL_PropertiesID properties)
{
	unsigned int index = properties - LOCAL_PROPERTIES;

	return properties >= LOCAL_PROPERTIES && index < MAXIMUM_LOCAL_PROPERTIES && local_properties[index].used ?
		(int)index : -1;
}

SDL_PropertiesID SDL_CreateProperties(void)
{
	int index;

	pthread_mutex_lock(&properties_lock);
	for (index = 0; index < MAXIMUM_LOCAL_PROPERTIES; index++)
	{
		if (!local_properties[index].used)
		{
			memset(&local_properties[index], 0, sizeof(local_properties[index]));
			local_properties[index].used = true;
			pthread_mutex_unlock(&properties_lock);
			return LOCAL_PROPERTIES + (unsigned int)index;
		}
	}
	pthread_mutex_unlock(&properties_lock);
	return 0;
}

void SDL_DestroyProperties(SDL_PropertiesID properties)
{
	int index;

	pthread_mutex_lock(&properties_lock);
	if ((index = local_index(properties)) >= 0)
		local_properties[index].used = false;
	pthread_mutex_unlock(&properties_lock);
}

static struct local_property *local_property(SDL_PropertiesID properties, const char *name, bool create)
{
	int index = local_index(properties), value;

	if (index < 0)
		return NULL;
	for (value = 0; value < local_properties[index].count; value++)
		if (!strcmp(local_properties[index].values[value].name, name))
			return &local_properties[index].values[value];
	if (!create || local_properties[index].count == MAXIMUM_PROPERTY_VALUES)
		return NULL;
	value = local_properties[index].count++;
	memset(&local_properties[index].values[value], 0, sizeof(local_properties[index].values[value]));
	snprintf(local_properties[index].values[value].name, sizeof(local_properties[index].values[value].name), "%s",
		name);
	return &local_properties[index].values[value];
}

bool SDL_SetBooleanProperty(SDL_PropertiesID properties, const char *name, bool value)
{
	struct local_property *property;

	pthread_mutex_lock(&properties_lock);
	property = local_property(properties, name, true);
	if (property)
		property->boolean = value;
	pthread_mutex_unlock(&properties_lock);
	return property != NULL;
}

bool SDL_SetNumberProperty(SDL_PropertiesID properties, const char *name, Sint64 value)
{
	struct local_property *property;

	pthread_mutex_lock(&properties_lock);
	property = local_property(properties, name, true);
	if (property)
		property->number = value;
	pthread_mutex_unlock(&properties_lock);
	return property != NULL;
}

static Sint64 local_number(SDL_PropertiesID properties, const char *name, Sint64 default_value)
{
	struct local_property *property;
	Sint64 value = default_value;

	pthread_mutex_lock(&properties_lock);
	if ((property = local_property(properties, name, false)))
		value = property->number;
	pthread_mutex_unlock(&properties_lock);
	return value;
}

bool SDL_GetBooleanProperty(SDL_PropertiesID properties, const char *name, bool default_value)
{
	struct local_property *property;
	bool value = default_value;

	if (local_index(properties) < 0)
		return host_sdl_get_boolean_property(properties, name, default_value) != 0;
	pthread_mutex_lock(&properties_lock);
	if ((property = local_property(properties, name, false)))
		value = property->boolean;
	pthread_mutex_unlock(&properties_lock);
	return value;
}

float SDL_GetFloatProperty(SDL_PropertiesID properties, const char *name, float default_value)
{
	if (local_index(properties) >= 0)
		return default_value;
	return host_sdl_get_float_property(properties, name, default_value);
}

/* ---------- windows and displays */

SDL_Window *SDL_CreateWindow(const char *title, int width, int height, SDL_WindowFlags flags)
{
	error_from_host();
	return HANDLE(SDL_Window, host_sdl_create_window(title, width, height, (long long)flags));
}

void SDL_DestroyWindow(SDL_Window *window)
{
	host_sdl_destroy_window(HANDLE_OF(window));
}

SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window)
{
	return host_sdl_window_properties(HANDLE_OF(window));
}

bool SDL_GetWindowSize(SDL_Window *window, int *width, int *height)
{
	int w = 0, h = 0;
	int result = host_sdl_window_size(HANDLE_OF(window), &w, &h, 0);

	if (width)
		*width = w;
	if (height)
		*height = h;
	return result != 0;
}

bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *width, int *height)
{
	int w = 0, h = 0;
	int result = host_sdl_window_size(HANDLE_OF(window), &w, &h, 1);

	if (width)
		*width = w;
	if (height)
		*height = h;
	return result != 0;
}

bool SDL_SetWindowSize(SDL_Window *window, int width, int height)
{
	return host_sdl_set_window_size(HANDLE_OF(window), width, height) != 0;
}

bool SDL_SetWindowPosition(SDL_Window *window, int x, int y)
{
	return host_sdl_set_window_position(HANDLE_OF(window), x, y) != 0;
}

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window)
{
	return (SDL_WindowFlags)host_sdl_window_flags(HANDLE_OF(window));
}

float SDL_GetWindowPixelDensity(SDL_Window *window)
{
	return host_sdl_window_pixel_density(HANDLE_OF(window));
}

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen)
{
	return host_sdl_set_window_fullscreen(HANDLE_OF(window), fullscreen) != 0;
}

/* (SDL_DisplayMode ends with a pointer, SDL's own: the guest's copies carry
none, and the host finds its mode again from the rest) */
bool SDL_SetWindowFullscreenMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
	if (!mode)
		return host_sdl_set_window_fullscreen_mode(HANDLE_OF(window), 0, 0, 0, 0.0f, 0.0f, 0) != 0;
	return host_sdl_set_window_fullscreen_mode(HANDLE_OF(window), 1, mode->displayID, mode->w, mode->h,
		mode->refresh_rate, mode->pixel_density > 1.0f) != 0;
}

bool SDL_SetWindowMouseGrab(SDL_Window *window, bool grabbed)
{
	return host_sdl_set_window_mouse_grab(HANDLE_OF(window), grabbed) != 0;
}

bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled)
{
	return host_sdl_set_relative_mouse(HANDLE_OF(window), enabled) != 0;
}

void SDL_WarpMouseInWindow(SDL_Window *window, float x, float y)
{
	host_sdl_warp_mouse(HANDLE_OF(window), x, y);
}

SDL_Window *SDL_GetKeyboardFocus(void)
{
	return HANDLE(SDL_Window, host_sdl_keyboard_focus());
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window)
{
	return (SDL_DisplayID)host_sdl_display_for_window(HANDLE_OF(window));
}

SDL_DisplayID SDL_GetPrimaryDisplay(void)
{
	return (SDL_DisplayID)host_sdl_primary_display();
}

bool SDL_GetDisplayUsableBounds(SDL_DisplayID display, SDL_Rect *rect)
{
	/* (SDL_Rect is four ints) */
	return host_sdl_display_usable_bounds(display, rect) != 0;
}

/* the host's modes: struct host_display_mode (guest_host.h) */
static void mode_from_host(SDL_DisplayMode *mode, const struct host_display_mode *host)
{
	memset(mode, 0, sizeof(*mode));
	mode->displayID = host->display;
	mode->format = (SDL_PixelFormat)host->format;
	mode->w = host->w;
	mode->h = host->h;
	mode->pixel_density = host->pixel_density;
	mode->refresh_rate = host->refresh_rate;
	mode->refresh_rate_numerator = host->refresh_numerator;
	mode->refresh_rate_denominator = host->refresh_denominator;
}

const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID display)
{
	static __thread SDL_DisplayMode mode;
	struct host_display_mode host;

	if (!host_sdl_display_mode(display, 0, &host))
		return NULL;
	mode_from_host(&mode, &host);
	return &mode;
}

const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID display)
{
	static __thread SDL_DisplayMode mode;
	struct host_display_mode host;

	if (!host_sdl_display_mode(display, 1, &host))
		return NULL;
	mode_from_host(&mode, &host);
	return &mode;
}

/* one allocation: the array, then the modes (the caller frees it) */
SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID display, int *count)
{
	struct host_display_mode host[128];
	SDL_DisplayMode **result, *modes;
	int found = host_sdl_fullscreen_display_modes(display, host, 128), index;

	if (count)
		*count = 0;
	if (found < 0)
		return NULL;
	result = malloc((size_t)(found + 1) * sizeof(*result) + (size_t)found * sizeof(SDL_DisplayMode));
	if (!result)
		return NULL;
	modes = (SDL_DisplayMode *)(result + found + 1);
	for (index = 0; index < found; index++)
	{
		mode_from_host(&modes[index], &host[index]);
		result[index] = &modes[index];
	}
	result[found] = NULL;
	if (count)
		*count = found;
	return result;
}

bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID display, int w, int h, float refresh_rate,
	bool include_high_density_modes, SDL_DisplayMode *closest)
{
	struct host_display_mode host;

	if (!host_sdl_closest_display_mode(display, w, h, refresh_rate, include_high_density_modes, &host))
		return false;
	mode_from_host(closest, &host);
	return true;
}

/* ---------- surfaces, in guest memory */

struct guest_surface
{
	SDL_Surface *alternate;
};

static SDL_Surface *surface_allocate(int width, int height, SDL_PixelFormat format)
{
	SDL_Surface *surface;
	int bytes = SDL_BYTESPERPIXEL(format);
	struct guest_surface *extra;

	if (width <= 0 || height <= 0 || bytes <= 0)
	{
		SDL_SetError("surface %dx%d of format %x", width, height, (unsigned int)format);
		return NULL;
	}
	surface = calloc(1, sizeof(*surface) + sizeof(*extra));
	if (!surface)
		return NULL;
	extra = (struct guest_surface *)(surface + 1);
	surface->format = format;
	surface->w = width;
	surface->h = height;
	surface->pitch = (width * bytes + 3) & ~3;
	surface->refcount = 1;
	surface->reserved = extra;
	surface->pixels = calloc((size_t)surface->pitch, (size_t)height);
	if (!surface->pixels)
	{
		free(surface);
		return NULL;
	}
	return surface;
}

SDL_Surface *SDL_CreateSurface(int width, int height, SDL_PixelFormat format)
{
	return surface_allocate(width, height, format);
}

void SDL_DestroySurface(SDL_Surface *surface)
{
	struct guest_surface *extra;

	if (!surface || --surface->refcount > 0)
		return;
	extra = surface->reserved;
	if (extra && extra->alternate)
		SDL_DestroySurface(extra->alternate);
	free(surface->pixels);
	free(surface);
}

SDL_Surface *SDL_DuplicateSurface(SDL_Surface *surface)
{
	SDL_Surface *copy;

	if (!surface)
		return NULL;
	copy = surface_allocate(surface->w, surface->h, surface->format);
	if (copy)
		memcpy(copy->pixels, surface->pixels, (size_t)surface->pitch * (size_t)surface->h);
	return copy;
}

SDL_Surface *SDL_ScaleSurface(SDL_Surface *surface, int width, int height, SDL_ScaleMode mode)
{
	SDL_Surface *scaled;

	if (!surface)
		return NULL;
	scaled = surface_allocate(width, height, surface->format);
	if (!scaled)
		return NULL;
	if (!host_sdl_scale_pixels(surface->format, surface->w, surface->h, surface->pitch, surface->pixels,
		width, height, scaled->pitch, scaled->pixels, (int)mode))
	{
		SDL_DestroySurface(scaled);
		error_from_host();
		return NULL;
	}
	return scaled;
}

/* (kept with the surface: the cursor made of it takes both) */
bool SDL_AddSurfaceAlternateImage(SDL_Surface *surface, SDL_Surface *image)
{
	struct guest_surface *extra;

	if (!surface || !image || !(extra = surface->reserved))
		return false;
	if (extra->alternate)
		SDL_DestroySurface(extra->alternate);
	image->refcount++;
	extra->alternate = image;
	return true;
}

/* decoded by the host, into pixels of the guest */
SDL_Surface *SDL_LoadPNG_IO(SDL_IOStream *stream, bool close)
{
	size_t size = 0;
	void *data = stream ? read_all(stream, &size) : NULL;
	int width = 0, height = 0;
	unsigned int format = 0;
	SDL_Surface *surface = NULL;

	if (close)
		SDL_CloseIO(stream);
	if (!data)
		return NULL;
	if (host_sdl_png_size(data, (unsigned int)size, &width, &height, &format))
	{
		surface = surface_allocate(width, height, (SDL_PixelFormat)format);
		if (surface && !host_sdl_png_decode(data, (unsigned int)size, surface->pitch, surface->pixels))
		{
			SDL_DestroySurface(surface);
			surface = NULL;
		}
	}
	else
	{
		error_from_host();
	}
	free(data);
	return surface;
}

bool SDL_SavePNG(SDL_Surface *surface, const char *path)
{
	if (!surface)
		return false;
	error_from_host();
	return host_sdl_save_png(surface->format, surface->w, surface->h, surface->pitch, surface->pixels, path) != 0;
}

bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon)
{
	if (!icon)
		return false;
	return host_sdl_set_window_icon(HANDLE_OF(window), icon->format, icon->w, icon->h, icon->pitch,
		icon->pixels) != 0;
}

/* ---------- the mouse pointer */

SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *surface, int hot_x, int hot_y)
{
	struct guest_surface *extra;
	SDL_Surface *alternate;

	if (!surface)
		return NULL;
	extra = surface->reserved;
	alternate = extra ? extra->alternate : NULL;
	return HANDLE(SDL_Cursor, host_sdl_create_color_cursor(surface->format, surface->w, surface->h, surface->pitch,
		surface->pixels, alternate ? alternate->w : 0, alternate ? alternate->h : 0, alternate ? alternate->pitch : 0,
		alternate ? alternate->pixels : NULL, hot_x, hot_y));
}

SDL_Cursor *SDL_GetDefaultCursor(void)
{
	return HANDLE(SDL_Cursor, host_sdl_default_cursor());
}

bool SDL_SetCursor(SDL_Cursor *cursor)
{
	return host_sdl_set_cursor(HANDLE_OF(cursor)) != 0;
}

void SDL_DestroyCursor(SDL_Cursor *cursor)
{
	host_sdl_destroy_cursor(HANDLE_OF(cursor));
}

bool SDL_ShowCursor(void)
{
	return host_sdl_cursor(1) != 0;
}

bool SDL_HideCursor(void)
{
	return host_sdl_cursor(0) != 0;
}

bool SDL_CursorVisible(void)
{
	return host_sdl_cursor(-1) != 0;
}

/* ---------- the software renderer (the updater's and the disc import's
screens) */

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name)
{
	error_from_host();
	return HANDLE(SDL_Renderer, host_sdl_create_renderer(HANDLE_OF(window), name));
}

void SDL_DestroyRenderer(SDL_Renderer *renderer)
{
	host_sdl_destroy_renderer(HANDLE_OF(renderer));
}

bool SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	return host_sdl_render_color(HANDLE_OF(renderer), r, g, b, a) != 0;
}

bool SDL_RenderClear(SDL_Renderer *renderer)
{
	return host_sdl_render_clear(HANDLE_OF(renderer)) != 0;
}

bool SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_FRect *rect)
{
	if (!rect)
		return host_sdl_render_fill(HANDLE_OF(renderer), 0, 0.0f, 0.0f, 0.0f, 0.0f) != 0;
	return host_sdl_render_fill(HANDLE_OF(renderer), 1, rect->x, rect->y, rect->w, rect->h) != 0;
}

bool SDL_RenderDebugText(SDL_Renderer *renderer, float x, float y, const char *text)
{
	return host_sdl_render_text(HANDLE_OF(renderer), x, y, text) != 0;
}

bool SDL_RenderPresent(SDL_Renderer *renderer)
{
	return host_sdl_render_present(HANDLE_OF(renderer)) != 0;
}

bool SDL_SetRenderScale(SDL_Renderer *renderer, float x, float y)
{
	return host_sdl_render_scale(HANDLE_OF(renderer), x, y) != 0;
}

bool SDL_SetRenderVSync(SDL_Renderer *renderer, int vsync)
{
	return host_sdl_render_vsync(HANDLE_OF(renderer), vsync) != 0;
}

/* ---------- OpenGL */

bool SDL_GL_SetAttribute(SDL_GLAttr attribute, int value)
{
	return host_sdl_gl_set_attribute((int)attribute, value) != 0;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
	error_from_host();
	return HANDLE(struct SDL_GLContextState, host_sdl_gl_create_context(HANDLE_OF(window)));
}

bool SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
	return host_sdl_gl_make_current(HANDLE_OF(window), HANDLE_OF(context)) != 0;
}

bool SDL_GL_SetSwapInterval(int interval)
{
	return host_sdl_gl_set_swap_interval(interval) != 0;
}

bool SDL_GL_SwapWindow(SDL_Window *window)
{
	return host_sdl_gl_swap_window(HANDLE_OF(window)) != 0;
}

SDL_FunctionPointer SDL_GL_GetProcAddress(const char *name)
{
	return guest_gl_get_proc_address(name);
}

/* ---------- the open file dialog: answered by the host when it is shown
and closed, its callback called here (on the thread that pumps the events,
as SDL may) */

static struct
{
	SDL_DialogFileCallback callback;
	void *userdata;
	bool waiting;
} file_dialog;

void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
	const SDL_DialogFileFilter *filters, int filter_count, const char *default_location, bool allow_many)
{
	const char *names[4] = { NULL, NULL, NULL, NULL };
	const char *patterns[4] = { NULL, NULL, NULL, NULL };
	int index;

	(void)allow_many;
	if (filter_count > 4)
		filter_count = 4;
	for (index = 0; index < filter_count; index++)
	{
		names[index] = filters[index].name;
		patterns[index] = filters[index].pattern;
	}
	file_dialog.callback = callback;
	file_dialog.userdata = userdata;
	file_dialog.waiting = true;
	host_sdl_show_open_file_dialog(HANDLE_OF(window), filter_count, names[0], patterns[0], names[1], patterns[1],
		names[2], patterns[2], names[3], patterns[3], default_location);
}

static void file_dialog_check(void)
{
	char path[1024];
	int filter = -1;
	int answer;

	if (!file_dialog.waiting)
		return;
	/* 1 a file, 0 none chosen, -1 no dialog could be shown, 2 still open */
	answer = host_sdl_file_dialog_answer(path, sizeof(path), &filter);
	if (answer == 2)
		return;
	file_dialog.waiting = false;
	if (answer < 0)
	{
		file_dialog.callback(file_dialog.userdata, NULL, -1);
	}
	else
	{
		const char *list[2] = { answer ? path : NULL, NULL };

		file_dialog.callback(file_dialog.userdata, list, filter);
	}
}

/* ---------- events */

void SDL_PumpEvents(void)
{
	host_sdl_pump_events();
	file_dialog_check();
}

bool SDL_PollEvent(SDL_Event *event)
{
	SDL_Event discarded;

	file_dialog_check();
	return host_sdl_poll_event(event ? event : &discarded) != 0;
}

/* (the platform layer pushes only events without pointers: the quit) */
bool SDL_PushEvent(SDL_Event *event)
{
	return host_sdl_push_event(event) != 0;
}

/* ---------- the keyboard */

const char *SDL_GetScancodeName(SDL_Scancode scancode)
{
	static __thread char buffer[64];

	host_sdl_scancode_name((int)scancode, buffer, sizeof(buffer));
	return buffer;
}

SDL_Scancode SDL_GetScancodeFromName(const char *name)
{
	return (SDL_Scancode)host_sdl_scancode_from_name(name);
}

SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode, SDL_Keymod modifiers, bool key_event)
{
	return (SDL_Keycode)host_sdl_key_from_scancode((int)scancode, (int)modifiers, key_event);
}

bool SDL_StartTextInputWithProperties(SDL_Window *window, SDL_PropertiesID properties)
{
	return host_sdl_text_input(HANDLE_OF(window),
		(int)local_number(properties, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT)) != 0;
}

bool SDL_StopTextInput(SDL_Window *window)
{
	host_sdl_text_input(HANDLE_OF(window), -1);
	return true;
}

bool SDL_TextInputActive(SDL_Window *window)
{
	return host_sdl_text_input(HANDLE_OF(window), -2) != 0;
}

bool SDL_HasScreenKeyboardSupport(void)
{
	return host_sdl_text_input(0, -3) != 0;
}

/* ---------- the clipboard */

bool SDL_SetClipboardText(const char *text)
{
	return host_sdl_set_clipboard_text(text) != 0;
}

char *SDL_GetClipboardText(void)
{
	char buffer[4096];

	host_sdl_get_clipboard_text(buffer, sizeof(buffer));
	return strdup(buffer);
}

/* ---------- messages for the player */

bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *window)
{
	(void)window;
	return host_sdl_show_simple_message_box((unsigned int)flags, title, message) != 0;
}

/* (up to three buttons, their ids those of the guest's) */
bool SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *buttonid)
{
	const char *labels[3] = { NULL, NULL, NULL };
	int default_index = -1;
	int index;
	int chosen;

	if (!data || data->numbuttons < 1 || data->numbuttons > 3)
		return false;
	for (index = 0; index < data->numbuttons; index++)
	{
		labels[index] = data->buttons[index].text;
		if (data->buttons[index].flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT)
			default_index = index;
	}
	chosen = host_sdl_show_message_box((unsigned int)data->flags, data->title, data->message, default_index,
		labels[0], labels[1], labels[2]);
	if (chosen < 0)
		return false;
	if (buttonid)
		*buttonid = data->buttons[chosen].buttonID;
	return true;
}

/* ---------- gamepads */

SDL_JoystickID *SDL_GetGamepads(int *count)
{
	unsigned int ids[64];
	int found = host_sdl_get_gamepads(ids, 64), index;
	SDL_JoystickID *result = malloc(sizeof(SDL_JoystickID) * (size_t)(found + 1));

	if (count)
		*count = 0;
	if (!result)
		return NULL;
	for (index = 0; index < found; index++)
		result[index] = ids[index];
	result[found] = 0;
	if (count)
		*count = found;
	return result;
}

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id)
{
	error_from_host();
	return HANDLE(SDL_Gamepad, host_sdl_open_gamepad(id));
}

SDL_Gamepad *SDL_GetGamepadFromID(SDL_JoystickID id)
{
	return HANDLE(SDL_Gamepad, host_sdl_gamepad_from_id(id));
}

void SDL_CloseGamepad(SDL_Gamepad *gamepad)
{
	host_sdl_close_gamepad(HANDLE_OF(gamepad));
}

Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
	return (Sint16)host_sdl_gamepad_axis(HANDLE_OF(gamepad), (int)axis);
}

bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
	return host_sdl_gamepad_button(HANDLE_OF(gamepad), (int)button) != 0;
}

SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad)
{
	return (SDL_GamepadType)host_sdl_gamepad_type(HANDLE_OF(gamepad));
}

bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low, Uint16 high, Uint32 milliseconds)
{
	return host_sdl_rumble_gamepad(HANDLE_OF(gamepad), low, high, milliseconds) != 0;
}

/* a gamepad's name or path, in a buffer of the gamepad's own (the host's
handles are below 256) */
static const char *gamepad_string(SDL_Gamepad *gamepad, int which)
{
	static char buffers[2][256][256];
	unsigned int handle = HANDLE_OF(gamepad);
	char *buffer;

	if (handle >= 256)
		return NULL;
	buffer = buffers[which][handle];
	if (!host_sdl_gamepad_string(handle, which, buffer, sizeof(buffers[0][0])))
		return NULL;
	return buffer;
}

const char *SDL_GetGamepadName(SDL_Gamepad *gamepad)
{
	return gamepad_string(gamepad, 0);
}

const char *SDL_GetGamepadPath(SDL_Gamepad *gamepad)
{
	return gamepad_string(gamepad, 1);
}

SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *gamepad)
{
	return (SDL_JoystickID)host_sdl_gamepad_number(HANDLE_OF(gamepad), 0);
}

Uint16 SDL_GetGamepadVendor(SDL_Gamepad *gamepad)
{
	return (Uint16)host_sdl_gamepad_number(HANDLE_OF(gamepad), 1);
}

Uint16 SDL_GetGamepadProduct(SDL_Gamepad *gamepad)
{
	return (Uint16)host_sdl_gamepad_number(HANDLE_OF(gamepad), 2);
}

const char *SDL_GetGamepadStringForType(SDL_GamepadType type)
{
	static char buffers[SDL_GAMEPAD_TYPE_COUNT][32];

	if ((int)type < 0 || type >= SDL_GAMEPAD_TYPE_COUNT)
		return NULL;
	if (!buffers[type][0])
		host_sdl_gamepad_type_string((int)type, buffers[type], sizeof(buffers[type]));
	return buffers[type][0] ? buffers[type] : NULL;
}

/* ---------- virtual joysticks (input.virtual_gamepads: xinput_sdl.c) */

SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc *desc)
{
	if (!desc)
		return 0;
	error_from_host();
	return (SDL_JoystickID)host_sdl_attach_virtual_joystick(desc->type, desc->naxes, desc->nbuttons, desc->axis_mask,
		desc->button_mask, desc->name);
}

SDL_Joystick *SDL_OpenJoystick(SDL_JoystickID id)
{
	error_from_host();
	return HANDLE(SDL_Joystick, host_sdl_open_joystick(id));
}

bool SDL_SetJoystickVirtualAxis(SDL_Joystick *joystick, int axis, Sint16 value)
{
	return host_sdl_set_virtual(HANDLE_OF(joystick), 0, axis, value) != 0;
}

bool SDL_SetJoystickVirtualButton(SDL_Joystick *joystick, int button, bool down)
{
	return host_sdl_set_virtual(HANDLE_OF(joystick), 1, button, down) != 0;
}

/* ---------- audio */

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec *spec,
	SDL_AudioStreamCallback callback, void *userdata)
{
	/* (SDL_AudioSpec is three ints: one layout) */
	error_from_host();
	return HANDLE(SDL_AudioStream, host_sdl_open_audio_stream(device, spec, HANDLE_OF(callback),
		HANDLE_OF(userdata)));
}

bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *data, int length)
{
	return host_sdl_put_audio_stream_data(HANDLE_OF(stream), data, length) != 0;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream)
{
	return host_sdl_resume_audio_stream_device(HANDLE_OF(stream)) != 0;
}

int SDL_GetAudioStreamData(SDL_AudioStream *stream, void *data, int length)
{
	return host_sdl_get_audio_stream_data(HANDLE_OF(stream), data, length);
}

int SDL_GetAudioStreamAvailable(SDL_AudioStream *stream)
{
	return host_sdl_get_audio_stream_available(HANDLE_OF(stream));
}

void SDL_DestroyAudioStream(SDL_AudioStream *stream)
{
	host_sdl_destroy_audio_stream(HANDLE_OF(stream));
}

static SDL_AudioDeviceID *audio_devices(int recording, int *count)
{
	unsigned int ids[64];
	int found = host_sdl_audio_devices(recording, ids, 64), index;
	SDL_AudioDeviceID *result = malloc(sizeof(SDL_AudioDeviceID) * (size_t)(found + 1));

	if (count)
		*count = 0;
	if (!result || found < 0)
	{
		free(result);
		return NULL;
	}
	for (index = 0; index < found; index++)
		result[index] = ids[index];
	result[found] = 0;
	if (count)
		*count = found;
	return result;
}

SDL_AudioDeviceID *SDL_GetAudioPlaybackDevices(int *count)
{
	return audio_devices(0, count);
}

SDL_AudioDeviceID *SDL_GetAudioRecordingDevices(int *count)
{
	return audio_devices(1, count);
}

const char *SDL_GetAudioDeviceName(SDL_AudioDeviceID device)
{
	static __thread char buffer[4][256];
	static __thread int next;
	char *result = buffer[next++ & 3];

	if (!host_sdl_audio_device_name(device, result, sizeof(buffer[0])))
		return NULL;
	return result;
}

bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID device, SDL_AudioSpec *spec, int *sample_frames)
{
	return host_sdl_audio_device_format(device, spec, sample_frames) != 0;
}
