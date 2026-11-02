/*
HOST_SDL.C

SDL3 on behalf of the guest (guest/runtime/guest_sdl.c). SDL objects are
64-bit pointers, which the guest cannot hold; it gets small handles into the
table here instead. (The Android host's bridge, port/android/host/host_sdl.c,
grown to what the desktop build calls.)

The guest calls these on its own threads, whose stacks are in guest memory.
SDL's audio thread has no guest stack, so the audio callback is handed to a
thread that has one.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define HANDLE_COUNT 256

enum handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_gamepad,
	_handle_joystick,
	_handle_audio,
	_handle_renderer,
	_handle_cursor,
};

struct handle
{
	int type;
	void *object;
	/* an audio stream with a callback: its binding (below) */
	void *binding;
};

static struct handle handles[HANDLE_COUNT];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t handle_new(int type, void *object)
{
	uint32_t index;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
	/* an object that already has a handle keeps it */
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == type && handles[index].object == object)
		{
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == _handle_free)
		{
			handles[index].type = type;
			handles[index].object = object;
			handles[index].binding = NULL;
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	pthread_mutex_unlock(&handle_lock);
	host_logf(HOST_LOG_ERROR, "out of SDL handles");
	return 0;
}

/* the handle an object has, if any (0) */
static uint32_t handle_find(int type, void *object)
{
	uint32_t index, found = 0;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
	for (index = 1; index < HANDLE_COUNT && !found; index++)
	{
		if (handles[index].type == type && handles[index].object == object)
			found = index;
	}
	pthread_mutex_unlock(&handle_lock);
	return found;
}

static void *handle_get(uint32_t handle, int type)
{
	void *object = NULL;

	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
		object = handles[handle].object;
	pthread_mutex_unlock(&handle_lock);
	return object;
}

/* the object of a handle, which the handle no longer refers to; NULL when
there is none. *binding gets what handle_new's caller stored (audio) */
static void *handle_release(uint32_t handle, int type, void **binding)
{
	void *object = NULL;

	if (binding)
		*binding = NULL;
	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
	{
		object = handles[handle].object;
		if (binding)
			*binding = handles[handle].binding;
		handles[handle].type = _handle_free;
		handles[handle].object = NULL;
		handles[handle].binding = NULL;
	}
	pthread_mutex_unlock(&handle_lock);
	return object;
}

static void copy_string(char *buffer, uint32_t size, const char *text)
{
	if (size)
		SDL_strlcpy(buffer, text ? text : "", size);
}

/* ---------- general */

int host_sdl_init(uint32_t flags)
{
	return SDL_Init((SDL_InitFlags)flags);
}

int host_sdl_set_hint(const char *name, const char *value)
{
	return SDL_SetHint(name, value);
}

int host_sdl_get_hint_boolean(const char *name, int default_value)
{
	return SDL_GetHintBoolean(name, default_value != 0);
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	copy_string(buffer, size, SDL_GetError());
}

void host_sdl_current_video_driver(char *buffer, uint32_t size)
{
	copy_string(buffer, size, SDL_GetCurrentVideoDriver());
}

void host_sdl_base_path(char *buffer, uint32_t size)
{
	copy_string(buffer, size, host_base_path());
}

int host_sdl_open_url(const char *url)
{
	return SDL_OpenURL(url) ? 1 : 0;
}

int64_t host_sdl_ticks(void)
{
	return (int64_t)SDL_GetTicks();
}

int64_t host_sdl_ticks_ns(void)
{
	return (int64_t)SDL_GetTicksNS();
}

int64_t host_sdl_thread_id(void)
{
	return (int64_t)SDL_GetCurrentThreadID();
}

void host_sdl_delay_ns(int64_t nanoseconds)
{
	if (nanoseconds > 0)
		SDL_DelayPrecise((Uint64)nanoseconds);
}

int host_sdl_current_time(int64_t *ticks)
{
	SDL_Time time;

	if (!SDL_GetCurrentTime(&time))
		return 0;
	*ticks = (int64_t)time;
	return 1;
}

int host_sdl_time_to_date_time(int64_t ticks, void *date_time, int local_time)
{
	return SDL_TimeToDateTime((SDL_Time)ticks, date_time, local_time != 0);
}

/* ---------- the file system */

int host_sdl_get_path_info(const char *path, void *information)
{
	return SDL_GetPathInfo(path, information);
}

int host_sdl_create_directory(const char *path)
{
	return SDL_CreateDirectory(path);
}

int host_sdl_remove_path(const char *path)
{
	return SDL_RemovePath(path);
}

int host_sdl_rename_path(const char *from, const char *to)
{
	return SDL_RenamePath(from, to);
}

int host_sdl_glob_directory(const char *path, const char *pattern, uint32_t flags, char *names, uint32_t capacity,
	uint32_t *size)
{
	int count = 0, index;
	char **found = SDL_GlobDirectory(path, pattern, (SDL_GlobFlags)flags, &count);
	uint32_t total = 0;

	if (!found)
		return -1;
	for (index = 0; index < count; index++)
		total += (uint32_t)strlen(found[index]) + 1;
	*size = total;
	if (names && capacity >= total)
	{
		for (index = 0; index < count; index++)
		{
			size_t length = strlen(found[index]) + 1;

			memcpy(names, found[index], length);
			names += length;
		}
	}
	SDL_free(found);
	return count;
}

int host_sdl_get_boolean_property(uint32_t properties, const char *name, int default_value)
{
	return SDL_GetBooleanProperty((SDL_PropertiesID)properties, name, default_value != 0);
}

float host_sdl_get_float_property(uint32_t properties, const char *name, float default_value)
{
	return SDL_GetFloatProperty((SDL_PropertiesID)properties, name, default_value);
}

/* ---------- windows and displays */

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	return handle_new(_handle_window, SDL_CreateWindow(title, width, height, (SDL_WindowFlags)flags));
}

void host_sdl_destroy_window(uint32_t window)
{
	SDL_Window *object = handle_release(window, _handle_window, NULL);

	if (object)
		SDL_DestroyWindow(object);
}

uint32_t host_sdl_window_properties(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_GetWindowProperties(object) : 0;
}

int host_sdl_window_size(uint32_t window, int *width, int *height, int pixels)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
	if (!object)
		return 0;
	return pixels ? SDL_GetWindowSizeInPixels(object, width, height) : SDL_GetWindowSize(object, width, height);
}

int host_sdl_set_window_size(uint32_t window, int width, int height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowSize(object, width, height) : 0;
}

int64_t host_sdl_window_flags(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? (int64_t)SDL_GetWindowFlags(object) : 0;
}

float host_sdl_window_pixel_density(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_GetWindowPixelDensity(object) : 0.0f;
}

int host_sdl_set_window_fullscreen(uint32_t window, int fullscreen)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowFullscreen(object, fullscreen != 0) : 0;
}

int host_sdl_set_window_fullscreen_mode(uint32_t window, int has_mode, uint32_t display, int w, int h,
	float refresh_rate, int high_density)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_DisplayMode mode;

	if (!object)
		return 0;
	if (!has_mode)
		return SDL_SetWindowFullscreenMode(object, NULL);
	if (!SDL_GetClosestFullscreenDisplayMode((SDL_DisplayID)display, w, h, refresh_rate, high_density != 0, &mode))
		return 0;
	return SDL_SetWindowFullscreenMode(object, &mode);
}

int host_sdl_set_window_mouse_grab(uint32_t window, int grabbed)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowMouseGrab(object, grabbed != 0) : 0;
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowRelativeMouseMode(object, enabled != 0) : 0;
}

void host_sdl_warp_mouse(uint32_t window, float x, float y)
{
	SDL_Window *object = handle_get(window, _handle_window);

	if (object)
		SDL_WarpMouseInWindow(object, x, y);
}

uint32_t host_sdl_keyboard_focus(void)
{
	return handle_find(_handle_window, SDL_GetKeyboardFocus());
}

uint32_t host_sdl_display_for_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_GetDisplayForWindow(object) : 0;
}

uint32_t host_sdl_primary_display(void)
{
	return SDL_GetPrimaryDisplay();
}

int host_sdl_display_usable_bounds(uint32_t display, void *rect)
{
	return SDL_GetDisplayUsableBounds((SDL_DisplayID)display, rect);
}

/* struct host_display_mode (port/haiku/guest/runtime/guest_host.h) */
struct host_display_mode
{
	uint32_t display;
	uint32_t format;
	int32_t w, h;
	float pixel_density;
	float refresh_rate;
	int32_t refresh_numerator, refresh_denominator;
};

static void mode_to_guest(struct host_display_mode *result, const SDL_DisplayMode *mode)
{
	result->display = mode->displayID;
	result->format = (uint32_t)mode->format;
	result->w = mode->w;
	result->h = mode->h;
	result->pixel_density = mode->pixel_density;
	result->refresh_rate = mode->refresh_rate;
	result->refresh_numerator = mode->refresh_rate_numerator;
	result->refresh_denominator = mode->refresh_rate_denominator;
}

int host_sdl_display_mode(uint32_t display, int current, struct host_display_mode *result)
{
	const SDL_DisplayMode *mode = current ? SDL_GetCurrentDisplayMode((SDL_DisplayID)display) :
		SDL_GetDesktopDisplayMode((SDL_DisplayID)display);

	if (!mode)
		return 0;
	mode_to_guest(result, mode);
	return 1;
}

int host_sdl_fullscreen_display_modes(uint32_t display, struct host_display_mode *result, int capacity)
{
	int count = 0, index;
	SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes((SDL_DisplayID)display, &count);

	if (!modes)
		return -1;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		mode_to_guest(&result[index], modes[index]);
	SDL_free(modes);
	return count;
}

int host_sdl_closest_display_mode(uint32_t display, int w, int h, float refresh_rate, int high_density,
	struct host_display_mode *result)
{
	SDL_DisplayMode mode;

	if (!SDL_GetClosestFullscreenDisplayMode((SDL_DisplayID)display, w, h, refresh_rate, high_density != 0, &mode))
		return 0;
	mode_to_guest(result, &mode);
	return 1;
}

/* ---------- surfaces: wrappers of the guest's pixels, for one call */

static SDL_Surface *wrap(uint32_t format, int width, int height, int pitch, const void *pixels)
{
	return SDL_CreateSurfaceFrom(width, height, (SDL_PixelFormat)format, (void *)pixels, pitch);
}

int host_sdl_scale_pixels(uint32_t format, int width, int height, int pitch, const void *pixels,
	int scaled_width, int scaled_height, int scaled_pitch, void *scaled, int mode)
{
	SDL_Surface *source = wrap(format, width, height, pitch, pixels);
	SDL_Surface *destination = wrap(format, scaled_width, scaled_height, scaled_pitch, scaled);
	int result = 0;

	if (source && destination)
	{
		/* (no blending: the pixels as they are, alpha included) */
		SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
		result = SDL_BlitSurfaceScaled(source, NULL, destination, NULL, (SDL_ScaleMode)mode);
	}
	SDL_DestroySurface(source);
	SDL_DestroySurface(destination);
	return result;
}

/* the last PNG decoded on this thread, until its pixels are taken */
static __thread SDL_Surface *decoded_png;
static __thread const void *decoded_data;

int host_sdl_png_size(const void *data, uint32_t size, int *width, int *height, uint32_t *format)
{
	SDL_Surface *surface;

	SDL_DestroySurface(decoded_png);
	decoded_png = NULL;
	surface = SDL_LoadPNG_IO(SDL_IOFromConstMem(data, size), true);
	if (!surface)
		return 0;
	decoded_png = surface;
	decoded_data = data;
	*width = surface->w;
	*height = surface->h;
	*format = (uint32_t)surface->format;
	return 1;
}

int host_sdl_png_decode(const void *data, uint32_t size, int pitch, void *pixels)
{
	SDL_Surface *surface = decoded_png;
	int row, bytes;

	(void)size;
	if (!surface || data != decoded_data)
		return 0;
	decoded_png = NULL;
	bytes = surface->w * SDL_BYTESPERPIXEL(surface->format);
	if (bytes > pitch)
	{
		SDL_DestroySurface(surface);
		return 0;
	}
	for (row = 0; row < surface->h; row++)
		memcpy((char *)pixels + (size_t)row * (size_t)pitch, (char *)surface->pixels + (size_t)row * surface->pitch,
			(size_t)bytes);
	SDL_DestroySurface(surface);
	return 1;
}

int host_sdl_save_png(uint32_t format, int width, int height, int pitch, const void *pixels, const char *path)
{
	SDL_Surface *surface = wrap(format, width, height, pitch, pixels);
	int result;

	if (!surface)
		return 0;
	result = SDL_SavePNG(surface, path);
	SDL_DestroySurface(surface);
	return result;
}

int host_sdl_set_window_icon(uint32_t window, uint32_t format, int width, int height, int pitch, const void *pixels)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_Surface *surface;
	int result;

	if (!object || !(surface = wrap(format, width, height, pitch, pixels)))
		return 0;
	result = SDL_SetWindowIcon(object, surface);
	SDL_DestroySurface(surface);
	return result;
}

/* ---------- the mouse pointer */

uint32_t host_sdl_create_color_cursor(uint32_t format, int width, int height, int pitch, const void *pixels,
	int alternate_width, int alternate_height, int alternate_pitch, const void *alternate, int hot_x, int hot_y)
{
	SDL_Surface *surface = wrap(format, width, height, pitch, pixels);
	SDL_Surface *other = alternate ? wrap(format, alternate_width, alternate_height, alternate_pitch, alternate) : NULL;
	SDL_Cursor *cursor = NULL;

	if (surface && (!alternate || (other && SDL_AddSurfaceAlternateImage(surface, other))))
		cursor = SDL_CreateColorCursor(surface, hot_x, hot_y);
	SDL_DestroySurface(other);
	SDL_DestroySurface(surface);
	return handle_new(_handle_cursor, cursor);
}

uint32_t host_sdl_default_cursor(void)
{
	return handle_new(_handle_cursor, SDL_GetDefaultCursor());
}

int host_sdl_set_cursor(uint32_t cursor)
{
	return SDL_SetCursor(handle_get(cursor, _handle_cursor));
}

void host_sdl_destroy_cursor(uint32_t cursor)
{
	SDL_Cursor *object = handle_release(cursor, _handle_cursor, NULL);

	/* (the default cursor is SDL's own) */
	if (object && object != SDL_GetDefaultCursor())
		SDL_DestroyCursor(object);
}

int host_sdl_cursor(int show)
{
	if (show < 0)
		return SDL_CursorVisible();
	return show ? SDL_ShowCursor() : SDL_HideCursor();
}

/* ---------- the software renderer */

uint32_t host_sdl_create_renderer(uint32_t window, const char *name)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? handle_new(_handle_renderer, SDL_CreateRenderer(object, name)) : 0;
}

void host_sdl_destroy_renderer(uint32_t renderer)
{
	SDL_Renderer *object = handle_release(renderer, _handle_renderer, NULL);

	if (object)
		SDL_DestroyRenderer(object);
}

int host_sdl_render_color(uint32_t renderer, int r, int g, int b, int a)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderDrawColor(object, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a) : 0;
}

int host_sdl_render_clear(uint32_t renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderClear(object) : 0;
}

int host_sdl_render_fill(uint32_t renderer, int has_rect, float x, float y, float w, float h)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);
	SDL_FRect rect;

	if (!object)
		return 0;
	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;
	return SDL_RenderFillRect(object, has_rect ? &rect : NULL);
}

int host_sdl_render_text(uint32_t renderer, float x, float y, const char *text)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderDebugText(object, x, y, text) : 0;
}

int host_sdl_render_present(uint32_t renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderPresent(object) : 0;
}

int host_sdl_render_scale(uint32_t renderer, float x, float y)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderScale(object, x, y) : 0;
}

int host_sdl_render_vsync(uint32_t renderer, int vsync)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderVSync(object, vsync) : 0;
}

/* ---------- OpenGL contexts */

int host_sdl_gl_set_attribute(int attribute, int value)
{
	return SDL_GL_SetAttribute((SDL_GLAttr)attribute, value);
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_GLContext context = object ? SDL_GL_CreateContext(object) : NULL;

	if (!context)
		return 0;
	/* (the entry points the guest imports, now there is a context) */
	host_gl_bind_imports();
	return handle_new(_handle_context, context);
}

int host_sdl_gl_make_current(uint32_t window, uint32_t context)
{
	return SDL_GL_MakeCurrent(handle_get(window, _handle_window), handle_get(context, _handle_context));
}

int host_sdl_gl_set_swap_interval(int interval)
{
	return SDL_GL_SetSwapInterval(interval);
}

int host_sdl_gl_swap_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_GL_SwapWindow(object) : 0;
}

/* ---------- the open file dialog: SDL answers on any thread; the guest
asks for the answer (host_sdl_file_dialog_answer) as it pumps the events */

static struct
{
	pthread_mutex_t lock;
	/* 2 open, 1 a file, 0 none chosen, -1 no dialog */
	int answer;
	int filter;
	char path[1024];
	SDL_DialogFileFilter filters[4];
	char names[4][128], patterns[4][128];
} file_dialog = { PTHREAD_MUTEX_INITIALIZER, 0 };

static void SDLCALL file_dialog_chosen(void *userdata, const char *const *files, int filter)
{
	(void)userdata;
	pthread_mutex_lock(&file_dialog.lock);
	file_dialog.filter = filter;
	if (!files)
	{
		file_dialog.answer = -1;
	}
	else if (files[0])
	{
		SDL_strlcpy(file_dialog.path, files[0], sizeof(file_dialog.path));
		file_dialog.answer = 1;
	}
	else
	{
		file_dialog.answer = 0;
	}
	pthread_mutex_unlock(&file_dialog.lock);
}

void host_sdl_show_open_file_dialog(uint32_t window, int filter_count, const char *name0, const char *pattern0,
	const char *name1, const char *pattern1, const char *name2, const char *pattern2, const char *name3,
	const char *pattern3, const char *default_location)
{
	const char *names[4] = { name0, name1, name2, name3 };
	const char *patterns[4] = { pattern0, pattern1, pattern2, pattern3 };
	int index;

	if (filter_count > 4)
		filter_count = 4;
	pthread_mutex_lock(&file_dialog.lock);
	file_dialog.answer = 2;
	file_dialog.path[0] = 0;
	for (index = 0; index < filter_count; index++)
	{
		SDL_strlcpy(file_dialog.names[index], names[index] ? names[index] : "", sizeof(file_dialog.names[index]));
		SDL_strlcpy(file_dialog.patterns[index], patterns[index] ? patterns[index] : "*",
			sizeof(file_dialog.patterns[index]));
		file_dialog.filters[index].name = file_dialog.names[index];
		file_dialog.filters[index].pattern = file_dialog.patterns[index];
	}
	pthread_mutex_unlock(&file_dialog.lock);
	SDL_ShowOpenFileDialog(file_dialog_chosen, NULL, handle_get(window, _handle_window), file_dialog.filters,
		filter_count, default_location, false);
}

int host_sdl_file_dialog_answer(char *path, uint32_t size, int *filter)
{
	int answer;

	pthread_mutex_lock(&file_dialog.lock);
	answer = file_dialog.answer;
	if (answer != 2)
	{
		copy_string(path, size, file_dialog.path);
		*filter = file_dialog.filter;
	}
	pthread_mutex_unlock(&file_dialog.lock);
	return answer;
}

/* ---------- events */

void host_sdl_pump_events(void)
{
	SDL_PumpEvents();
}

int host_sdl_poll_event(void *event)
{
	SDL_Event host_event;

	if (!SDL_PollEvent(&host_event))
		return 0;
	/* the layouts agree except for the pointers of text, drop, clipboard and
	user events, which the guest does not read: they are cleared */
	switch (host_event.type)
	{
	case SDL_EVENT_TEXT_EDITING:
		host_event.edit.text = NULL;
		break;
	case SDL_EVENT_TEXT_EDITING_CANDIDATES:
		host_event.edit_candidates.candidates = NULL;
		host_event.edit_candidates.num_candidates = 0;
		break;
	case SDL_EVENT_TEXT_INPUT:
		host_event.text.text = NULL;
		break;
	case SDL_EVENT_DROP_BEGIN:
	case SDL_EVENT_DROP_FILE:
	case SDL_EVENT_DROP_TEXT:
	case SDL_EVENT_DROP_COMPLETE:
	case SDL_EVENT_DROP_POSITION:
		host_event.drop.source = NULL;
		host_event.drop.data = NULL;
		break;
	case SDL_EVENT_CLIPBOARD_UPDATE:
		host_event.clipboard.mime_types = NULL;
		host_event.clipboard.num_mime_types = 0;
		break;
	default:
		if (host_event.type >= SDL_EVENT_USER)
		{
			host_event.user.data1 = NULL;
			host_event.user.data2 = NULL;
		}
		break;
	}
	memcpy(event, &host_event, sizeof(host_event));
	return 1;
}

int host_sdl_push_event(const void *event)
{
	SDL_Event host_event;

	memcpy(&host_event, event, sizeof(host_event));
	return SDL_PushEvent(&host_event);
}

/* ---------- the keyboard */

void host_sdl_scancode_name(int32_t scancode, char *buffer, uint32_t size)
{
	copy_string(buffer, size, SDL_GetScancodeName((SDL_Scancode)scancode));
}

int32_t host_sdl_scancode_from_name(const char *name)
{
	return (int32_t)SDL_GetScancodeFromName(name);
}

int host_sdl_key_from_scancode(int scancode, int modifiers, int key_event)
{
	return (int)SDL_GetKeyFromScancode((SDL_Scancode)scancode, (SDL_Keymod)modifiers, key_event != 0);
}

int host_sdl_text_input(uint32_t window, int type)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_PropertiesID properties;

	if (type == -3)
		return SDL_HasScreenKeyboardSupport();
	if (!object)
		return 0;
	if (type == -2)
		return SDL_TextInputActive(object);
	if (type < 0)
	{
		SDL_StopTextInput(object);
		return 0;
	}
	properties = SDL_CreateProperties();
	SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_TYPE_NUMBER, type);
	SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);
	SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, false);
	SDL_StartTextInputWithProperties(object, properties);
	SDL_DestroyProperties(properties);
	return SDL_TextInputActive(object) ? 1 : 0;
}

/* ---------- the clipboard */

int host_sdl_set_clipboard_text(const char *text)
{
	return SDL_SetClipboardText(text) ? 1 : 0;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	char *text = SDL_GetClipboardText();

	copy_string(buffer, size, text);
	SDL_free(text);
}

/* ---------- messages for the player */

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	return SDL_ShowSimpleMessageBox((SDL_MessageBoxFlags)flags, title, message, NULL) ? 1 : 0;
}

int host_sdl_show_message_box(uint32_t flags, const char *title, const char *message, int default_index,
	const char *button0, const char *button1, const char *button2)
{
	const char *labels[3] = { button0, button1, button2 };
	SDL_MessageBoxButtonData buttons[3];
	SDL_MessageBoxData question;
	int count = 0;
	int answer = -1;
	int index;

	for (index = 0; index < 3; index++)
	{
		if (!labels[index])
			continue;
		buttons[count].flags = index == default_index ?
			SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0;
		buttons[count].buttonID = index;
		buttons[count].text = labels[index];
		count++;
	}
	question.flags = (SDL_MessageBoxFlags)flags;
	question.window = NULL;
	question.title = title;
	question.message = message;
	question.numbuttons = count;
	question.buttons = buttons;
	question.colorScheme = NULL;
	return SDL_ShowMessageBox(&question, &answer) ? answer : -1;
}

/* ---------- gamepads */

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = 0, index;
	SDL_JoystickID *list = SDL_GetGamepads(&count);

	if (!list)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		ids[index] = list[index];
	SDL_free(list);
	return count;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	return handle_new(_handle_gamepad, SDL_OpenGamepad((SDL_JoystickID)id));
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	return handle_new(_handle_gamepad, SDL_GetGamepadFromID((SDL_JoystickID)id));
}

void host_sdl_close_gamepad(uint32_t gamepad)
{
	SDL_Gamepad *object = handle_release(gamepad, _handle_gamepad, NULL);

	if (object)
		SDL_CloseGamepad(object);
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadAxis(object, (SDL_GamepadAxis)axis) : 0;
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadButton(object, (SDL_GamepadButton)button) : 0;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadType(object) : SDL_GAMEPAD_TYPE_UNKNOWN;
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_RumbleGamepad(object, (Uint16)low, (Uint16)high, milliseconds) : 0;
}

int host_sdl_gamepad_string(uint32_t gamepad, int which, char *buffer, uint32_t size)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);
	const char *text = !object ? NULL : which ? SDL_GetGamepadPath(object) : SDL_GetGamepadName(object);

	if (!text)
		return 0;
	copy_string(buffer, size, text);
	return 1;
}

uint32_t host_sdl_gamepad_number(uint32_t gamepad, int which)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	if (!object)
		return 0;
	switch (which)
	{
	case 0: return SDL_GetGamepadID(object);
	case 1: return SDL_GetGamepadVendor(object);
	default: return SDL_GetGamepadProduct(object);
	}
}

void host_sdl_gamepad_type_string(int type, char *buffer, uint32_t size)
{
	copy_string(buffer, size, SDL_GetGamepadStringForType((SDL_GamepadType)type));
}

/* ---------- virtual joysticks */

uint32_t host_sdl_attach_virtual_joystick(int type, int axes, int buttons, uint32_t axis_mask, uint32_t button_mask,
	const char *name)
{
	/* (SDL keeps the name's pointer) */
	static char names[16][64];
	static int next;
	SDL_VirtualJoystickDesc desc;
	char *kept = names[next++ & 15];

	SDL_INIT_INTERFACE(&desc);
	SDL_strlcpy(kept, name ? name : "", sizeof(names[0]));
	desc.type = (Uint16)type;
	desc.naxes = (Uint16)axes;
	desc.nbuttons = (Uint16)buttons;
	desc.axis_mask = axis_mask;
	desc.button_mask = button_mask;
	desc.name = kept;
	return SDL_AttachVirtualJoystick(&desc);
}

uint32_t host_sdl_open_joystick(uint32_t id)
{
	return handle_new(_handle_joystick, SDL_OpenJoystick((SDL_JoystickID)id));
}

int host_sdl_set_virtual(uint32_t joystick, int button, int index, int value)
{
	SDL_Joystick *object = handle_get(joystick, _handle_joystick);

	if (!object)
		return 0;
	return button ? SDL_SetJoystickVirtualButton(object, index, value != 0) :
		SDL_SetJoystickVirtualAxis(object, index, (Sint16)value);
}

/* ---------- audio */

/* SDL calls audio_callback on its own audio thread, which cannot run guest
code; it passes each request to the stream's thread (audio_thread), which
can, and waits for it to be done. SDL holds the stream's lock throughout, so
the audio the guest puts into the stream meanwhile (from audio_thread) is
kept in the binding's buffer instead, and put in by audio_callback once the
guest is done: audio_thread putting it in itself would wait for the lock
forever (the Android host's design, port/android/host/host_sdl.c) */
struct audio_binding
{
	uint32_t handle;
	uint32_t callback;
	uint32_t userdata;
	pthread_mutex_t lock;
	pthread_cond_t requested;
	pthread_cond_t done;
	int pending;
	/* the stream is destroyed: the thread frees the binding and ends */
	int quit;
	int additional;
	int total;
	unsigned char *buffer;
	int buffer_length;
	int buffer_size;
};

/* the binding whose callback this thread is running, if any */
static __thread struct audio_binding *calling_back;

static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;

	pthread_mutex_lock(&binding->lock);
	for (;;)
	{
		int additional, total;

		while (!binding->pending && !binding->quit)
			pthread_cond_wait(&binding->requested, &binding->lock);
		if (binding->quit)
			break;
		additional = binding->additional;
		total = binding->total;
		pthread_mutex_unlock(&binding->lock);
		calling_back = binding;
		host_call_guest(binding->callback, binding->userdata, binding->handle, (uint32_t)additional, (uint32_t)total);
		calling_back = NULL;
		pthread_mutex_lock(&binding->lock);
		binding->pending = 0;
		pthread_cond_signal(&binding->done);
	}
	pthread_mutex_unlock(&binding->lock);
	pthread_cond_destroy(&binding->done);
	pthread_cond_destroy(&binding->requested);
	pthread_mutex_destroy(&binding->lock);
	SDL_free(binding->buffer);
	SDL_free(binding);
	return NULL;
}

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
	struct audio_binding *binding = userdata;

	pthread_mutex_lock(&binding->lock);
	binding->additional = additional;
	binding->total = total;
	binding->pending = 1;
	pthread_cond_signal(&binding->requested);
	while (binding->pending)
		pthread_cond_wait(&binding->done, &binding->lock);
	pthread_mutex_unlock(&binding->lock);
	if (binding->buffer_length)
	{
		SDL_PutAudioStreamData(stream, binding->buffer, binding->buffer_length);
		binding->buffer_length = 0;
	}
}

static int audio_keep(struct audio_binding *binding, const void *data, int length)
{
	if (length < 0)
		return 0;
	if (binding->buffer_length + length > binding->buffer_size)
	{
		int size = (binding->buffer_length + length) * 2;
		unsigned char *buffer = SDL_realloc(binding->buffer, (size_t)size);

		if (!buffer)
			return 0;
		binding->buffer = buffer;
		binding->buffer_size = size;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length += length;
	return 1;
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	struct audio_binding *binding;
	SDL_AudioStream *stream;

	/* (one without a callback, the microphone's, which the guest reads:
	nothing to bind) */
	if (!callback)
	{
		stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec, NULL, NULL);
		return stream ? handle_new(_handle_audio, stream) : 0;
	}
	binding = SDL_calloc(1, sizeof(*binding));
	if (!binding)
		return 0;
	binding->callback = callback;
	binding->userdata = userdata;
	pthread_mutex_init(&binding->lock, NULL);
	pthread_cond_init(&binding->requested, NULL);
	pthread_cond_init(&binding->done, NULL);
	stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec, audio_callback, binding);
	if (!stream)
	{
		SDL_free(binding);
		return 0;
	}
	/* the device starts paused, so no callback can run before this */
	binding->handle = handle_new(_handle_audio, stream);
	if (!binding->handle)
	{
		SDL_DestroyAudioStream(stream);
		pthread_cond_destroy(&binding->done);
		pthread_cond_destroy(&binding->requested);
		pthread_mutex_destroy(&binding->lock);
		SDL_free(binding);
		return 0;
	}
	pthread_mutex_lock(&handle_lock);
	handles[binding->handle].binding = binding;
	pthread_mutex_unlock(&handle_lock);
	if (host_native_thread_create(audio_thread, binding, 256 * 1024) != 0)
		host_fatal("cannot start the audio thread");
	return binding->handle;
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	if (!object)
		return 0;
	if (calling_back && calling_back->handle == stream)
		return audio_keep(calling_back, data, length);
	return SDL_PutAudioStreamData(object, data, length);
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_ResumeAudioStreamDevice(object) : 0;
}

int host_sdl_get_audio_stream_data(uint32_t stream, void *data, int length)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_GetAudioStreamData(object, data, length) : -1;
}

int host_sdl_get_audio_stream_available(uint32_t stream)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_GetAudioStreamAvailable(object) : -1;
}

void host_sdl_destroy_audio_stream(uint32_t stream)
{
	void *context;
	SDL_AudioStream *object = handle_release(stream, _handle_audio, &context);
	struct audio_binding *binding = context;

	if (!object)
		return;
	/* (returns once a callback that is running has finished) */
	SDL_DestroyAudioStream(object);
	if (binding)
	{
		pthread_mutex_lock(&binding->lock);
		binding->quit = 1;
		pthread_cond_signal(&binding->requested);
		pthread_mutex_unlock(&binding->lock);
	}
}

int host_sdl_audio_devices(int recording, uint32_t *ids, int capacity)
{
	int count = 0, index;
	SDL_AudioDeviceID *list = recording ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);

	if (!list)
		return -1;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		ids[index] = list[index];
	SDL_free(list);
	return count;
}

int host_sdl_audio_device_name(uint32_t device, char *buffer, uint32_t size)
{
	const char *name = SDL_GetAudioDeviceName((SDL_AudioDeviceID)device);

	if (!name)
		return 0;
	copy_string(buffer, size, name);
	return 1;
}

int host_sdl_audio_device_format(uint32_t device, void *spec, int *sample_frames)
{
	return SDL_GetAudioDeviceFormat((SDL_AudioDeviceID)device, spec, sample_frames);
}
