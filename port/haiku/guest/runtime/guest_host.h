/*
GUEST_HOST.H

The host services the guest imports (the guest's view; the host defines
them in port/haiku/host). Every name here must also be listed in
port/haiku/host_imports.list, which generates the import stubs.

Parameter types follow the rules in halo_x32_abi.h: 32-bit values are int
or unsigned int, 64-bit values long long, and pointers are passed as they
are (zero-extended by the x32 ABI). Never long: it is 32-bit here and
64-bit in the host.
*/

#ifndef __GUEST_HOST_H
#define __GUEST_HOST_H

/* ---------- process */

/* performs a Linux system call (the generic table's number) on the guest's
behalf with the host's C library, converting the structures whose layout
differs; returns the raw result (-errno on failure) */
long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f);

/* writes a line to the host's log (standard error) */
void host_log(int priority, const char *text);
void host_abort(const char *reason) __attribute__((noreturn));
void host_exit(int code) __attribute__((noreturn));
/* the host's errno on this thread, as a Linux errno value, after a call to
a host function */
int host_errno(void);

/* ---------- threads

The guest's thread pointer (its struct pthread) is kept by the host for
each thread. */

unsigned int host_get_tp(void);
void host_set_tp(unsigned int thread);
/* starts a host thread with a stack in guest memory that calls the image's
__guest_thread_start(thread); returns 0 or an errno value */
int host_thread_create(unsigned int thread, unsigned int stack_size);

/* ---------- memory write tracking (port/linux/src/memory_watch.c) */

void host_memory_watch_initialize(void);
void host_memory_watch_protect(unsigned int address, unsigned int size);
unsigned int host_memory_watch_generation(unsigned int address, unsigned int size);
unsigned int host_memory_watch_serial(void);
void host_memory_watch_prepare_write(unsigned int address, unsigned int size);
void host_memory_watch_forget(unsigned int address, unsigned int size);
void host_memory_watch_begin_frame(void);

/* ---------- SDL (guest/runtime/guest_sdl.c)

SDL objects (windows, contexts, gamepads, joysticks, audio streams,
renderers, cursors) are small integer handles on this side. */

/* a display mode, without SDL_DisplayMode's pointer */
struct host_display_mode
{
	unsigned int display;
	unsigned int format;
	int w, h;
	float pixel_density;
	float refresh_rate;
	int refresh_numerator, refresh_denominator;
};

int host_sdl_init(unsigned int flags);
int host_sdl_set_hint(const char *name, const char *value);
int host_sdl_get_hint_boolean(const char *name, int default_value);
void host_sdl_get_error(char *buffer, unsigned int size);
void host_sdl_current_video_driver(char *buffer, unsigned int size);
/* the folder of the program, ending with a separator */
void host_sdl_base_path(char *buffer, unsigned int size);
int host_sdl_open_url(const char *url);
long long host_sdl_ticks(void);
long long host_sdl_ticks_ns(void);
long long host_sdl_thread_id(void);
void host_sdl_delay_ns(long long nanoseconds);
int host_sdl_current_time(long long *ticks);
int host_sdl_time_to_date_time(long long ticks, void *date_time, int local_time);

/* the file system (SDL_PathInfo has one layout in both ABIs) */
int host_sdl_get_path_info(const char *path, void *information);
int host_sdl_create_directory(const char *path);
int host_sdl_remove_path(const char *path);
int host_sdl_rename_path(const char *from, const char *to);
/* the matching names, NUL-separated, into names (when it is not NULL and
holds them all) and their size into *size; the count, or -1 */
int host_sdl_glob_directory(const char *path, const char *pattern, unsigned int flags, char *names,
	unsigned int capacity, unsigned int *size);

int host_sdl_get_boolean_property(unsigned int properties, const char *name, int default_value);
float host_sdl_get_float_property(unsigned int properties, const char *name, float default_value);

/* windows and displays */
unsigned int host_sdl_create_window(const char *title, int width, int height, long long flags);
void host_sdl_destroy_window(unsigned int window);
unsigned int host_sdl_window_properties(unsigned int window);
/* in points, or in pixels */
int host_sdl_window_size(unsigned int window, int *width, int *height, int pixels);
int host_sdl_set_window_size(unsigned int window, int width, int height);
int host_sdl_set_window_position(unsigned int window, int x, int y);
long long host_sdl_window_flags(unsigned int window);
float host_sdl_window_pixel_density(unsigned int window);
int host_sdl_set_window_fullscreen(unsigned int window, int fullscreen);
/* with has_mode 0, the desktop's mode for fullscreen */
int host_sdl_set_window_fullscreen_mode(unsigned int window, int has_mode, unsigned int display, int w, int h,
	float refresh_rate, int high_density);
int host_sdl_set_window_mouse_grab(unsigned int window, int grabbed);
int host_sdl_set_relative_mouse(unsigned int window, int enabled);
void host_sdl_warp_mouse(unsigned int window, float x, float y);
unsigned int host_sdl_keyboard_focus(void);
unsigned int host_sdl_display_for_window(unsigned int window);
unsigned int host_sdl_primary_display(void);
int host_sdl_display_usable_bounds(unsigned int display, void *rect);
/* the desktop's (current 0) or the current (1) mode */
int host_sdl_display_mode(unsigned int display, int current, struct host_display_mode *mode);
int host_sdl_fullscreen_display_modes(unsigned int display, struct host_display_mode *modes, int capacity);
int host_sdl_closest_display_mode(unsigned int display, int w, int h, float refresh_rate, int high_density,
	struct host_display_mode *mode);

/* surfaces: the pixels are the guest's */
int host_sdl_scale_pixels(unsigned int format, int width, int height, int pitch, const void *pixels,
	int scaled_width, int scaled_height, int scaled_pitch, void *scaled, int mode);
/* a PNG's size and format (SDL_LoadPNG_IO's), then its pixels, in that
format */
int host_sdl_png_size(const void *data, unsigned int size, int *width, int *height, unsigned int *format);
int host_sdl_png_decode(const void *data, unsigned int size, int pitch, void *pixels);
int host_sdl_save_png(unsigned int format, int width, int height, int pitch, const void *pixels, const char *path);
int host_sdl_set_window_icon(unsigned int window, unsigned int format, int width, int height, int pitch,
	const void *pixels);

/* the mouse pointer */
unsigned int host_sdl_create_color_cursor(unsigned int format, int width, int height, int pitch,
	const void *pixels, int alternate_width, int alternate_height, int alternate_pitch, const void *alternate,
	int hot_x, int hot_y);
unsigned int host_sdl_default_cursor(void);
int host_sdl_set_cursor(unsigned int cursor);
void host_sdl_destroy_cursor(unsigned int cursor);
/* shows (1) or hides (0) the pointer, or tells whether it shows (-1) */
int host_sdl_cursor(int show);

/* the software renderer */
unsigned int host_sdl_create_renderer(unsigned int window, const char *name);
void host_sdl_destroy_renderer(unsigned int renderer);
int host_sdl_render_color(unsigned int renderer, int r, int g, int b, int a);
int host_sdl_render_clear(unsigned int renderer);
int host_sdl_render_fill(unsigned int renderer, int has_rect, float x, float y, float w, float h);
int host_sdl_render_text(unsigned int renderer, float x, float y, const char *text);
int host_sdl_render_present(unsigned int renderer);
int host_sdl_render_scale(unsigned int renderer, float x, float y);
int host_sdl_render_vsync(unsigned int renderer, int vsync);

/* OpenGL contexts */
int host_sdl_gl_set_attribute(int attribute, int value);
unsigned int host_sdl_gl_create_context(unsigned int window);
int host_sdl_gl_make_current(unsigned int window, unsigned int context);
int host_sdl_gl_set_swap_interval(int interval);
int host_sdl_gl_swap_window(unsigned int window);

/* the open file dialog, of up to four filters; its answer, once it is
closed: 1 a file (its path into path, the filter into *filter), 0 none
chosen, -1 no dialog could be shown, 2 while it is still open */
void host_sdl_show_open_file_dialog(unsigned int window, int filter_count, const char *name0, const char *pattern0,
	const char *name1, const char *pattern1, const char *name2, const char *pattern2, const char *name3,
	const char *pattern3, const char *default_location);
int host_sdl_file_dialog_answer(char *path, unsigned int size, int *filter);

/* events: the next one, written into the guest's SDL_Event (the pointers of
the events that carry them cleared); 0 if none */
void host_sdl_pump_events(void);
int host_sdl_poll_event(void *event);
int host_sdl_push_event(const void *event);

/* the keyboard */
void host_sdl_scancode_name(int scancode, char *buffer, unsigned int size);
int host_sdl_scancode_from_name(const char *name);
int host_sdl_key_from_scancode(int scancode, int modifiers, int key_event);
/* starts the window's text input as the SDL_TextInputType given (>= 0),
stops it (-1), tells whether it runs (-2) or whether there is a screen
keyboard (-3) */
int host_sdl_text_input(unsigned int window, int type);

int host_sdl_set_clipboard_text(const char *text);
void host_sdl_get_clipboard_text(char *buffer, unsigned int size);

int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message);
/* SDL_ShowMessageBox of up to three buttons (SDL_MessageBoxData has not the
guest's layout): the index of the one chosen, else -1 */
int host_sdl_show_message_box(unsigned int flags, const char *title, const char *message, int default_index,
	const char *button0, const char *button1, const char *button2);

/* gamepads */
int host_sdl_get_gamepads(unsigned int *ids, int capacity);
unsigned int host_sdl_open_gamepad(unsigned int id);
unsigned int host_sdl_gamepad_from_id(unsigned int id);
void host_sdl_close_gamepad(unsigned int gamepad);
int host_sdl_gamepad_axis(unsigned int gamepad, int axis);
int host_sdl_gamepad_button(unsigned int gamepad, int button);
int host_sdl_gamepad_type(unsigned int gamepad);
int host_sdl_rumble_gamepad(unsigned int gamepad, unsigned int low, unsigned int high, unsigned int milliseconds);
/* its name (0) or path (1); 0 if it has none */
int host_sdl_gamepad_string(unsigned int gamepad, int which, char *buffer, unsigned int size);
/* its joystick id (0), vendor (1) or product (2) */
unsigned int host_sdl_gamepad_number(unsigned int gamepad, int which);
void host_sdl_gamepad_type_string(int type, char *buffer, unsigned int size);

/* virtual joysticks */
unsigned int host_sdl_attach_virtual_joystick(int type, int axes, int buttons, unsigned int axis_mask,
	unsigned int button_mask, const char *name);
unsigned int host_sdl_open_joystick(unsigned int id);
/* an axis (0) or a button (1) */
int host_sdl_set_virtual(unsigned int joystick, int button, int index, int value);

/* audio. callback: void (*)(void *userdata, unsigned int stream, int
additional, int total), called on a thread with a guest stack */
unsigned int host_sdl_open_audio_stream(unsigned int device, const void *spec, unsigned int callback, unsigned int userdata);
int host_sdl_put_audio_stream_data(unsigned int stream, const void *data, int length);
int host_sdl_resume_audio_stream_device(unsigned int stream);
int host_sdl_get_audio_stream_data(unsigned int stream, void *data, int length);
int host_sdl_get_audio_stream_available(unsigned int stream);
void host_sdl_destroy_audio_stream(unsigned int stream);
/* the playback (0) or recording (1) devices' ids; the count, or -1 */
int host_sdl_audio_devices(int recording, unsigned int *ids, int capacity);
int host_sdl_audio_device_name(unsigned int device, char *buffer, unsigned int size);
int host_sdl_audio_device_format(unsigned int device, void *spec, int *sample_frames);

/* ---------- HTTPS (guest/runtime/guest_update.c; port/linux/src/update.h)

update_executable_path, update_replace_file, update_delete_file,
update_make_directory and update_launch are imported under their own names
(only strings and ints). */

/* progress: void (*)(void *context, unsigned long long received, unsigned
long long total), called on this thread */
int host_update_download(const char *url, const char *path, unsigned long long maximum, unsigned int progress,
	unsigned int context, char *error, int error_size);
/* update_https_request, its body kept by the host for host_update_take_body
(its size in *body_size) */
int host_update_https_request(const char *method, const char *url, const char *headers, const void *body,
	unsigned int body_size, unsigned int maximum, int *status, char *response_headers, unsigned int headers_size,
	unsigned int *response_body_size, char *error, int error_size);
/* copies the body of this thread's last request into body (when it is not
NULL) and lets it go */
void host_update_take_body(void *body);

/* ---------- OpenGL */

/* nonzero if the current context's driver has the function */
int host_gl_available(const char *name);
/* copies glGetString(name) (or glGetStringi when index >= 0) */
void host_gl_get_string(unsigned int name, int index, char *buffer, unsigned int size);
/* nonzero if the context supports the named extension */
int host_gl_has_extension(const char *name);
/* glFenceSync, glClientWaitSync and glDeleteSync with small handles for the
GLsync objects */
unsigned int host_gl_fence_sync(unsigned int condition, unsigned int flags);
unsigned int host_gl_client_wait_sync(unsigned int sync, unsigned int flags, unsigned long long timeout);
void host_gl_delete_sync(unsigned int sync);
/* logs the driver's debug messages (glDebugMessageCallback), or stops */
void host_gl_debug_output(int enabled);

#endif
