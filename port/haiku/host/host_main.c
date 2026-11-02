/*
HOST_MAIN.C

The host program's entry point: reserves the guest's address space before
anything else can take it, loads the guest image (halo_guest.elf, beside
the program), copies the command line and the environment into guest
memory, and runs the game's main() on a thread with its stack there. The
process ends when the guest exits (host_exit).
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#define GUEST_IMAGE_NAME "halo_guest.elf"
/* the game's main thread: as the Linux build's (8 MB), within Haiku's
largest thread stack (16 MB) */
#define MAIN_STACK_SIZE (8 * 1024 * 1024)

extern char **environ;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_line(int priority, const char *text)
{
	static const char *const names[] = { "", "", "", "", "info", "warning", "error" };

	pthread_mutex_lock(&log_lock);
	fprintf(stderr, "host %s: %s\n", priority >= 4 && priority <= 6 ? names[priority] : "", text);
	fflush(stderr);
	pthread_mutex_unlock(&log_lock);
}

void host_logf(int priority, const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	log_line(priority, text);
}

void host_fatal(const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	log_line(HOST_LOG_ERROR, text);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", text, NULL);
	_exit(EXIT_FAILURE);
}

/* ---------- services of the process, for the guest */

void host_log(int priority, const char *text)
{
	log_line(priority, text);
}

void host_abort(const char *reason)
{
	host_fatal("the game stopped: %s", reason ? reason : "abort");
}

void host_exit(int code)
{
	/* the game's own exit has run by now (its atexit functions, its files,
	in the guest's libc); the host's exit() would also tear down the
	system's libraries, and on Haiku the application kit SDL started, from
	the game's thread, not the one running it: that waits forever. The
	system takes the window and the rest with the process. */
	fflush(stdout);
	fflush(stderr);
	_exit(code);
}

const char *host_base_path(void)
{
	static char path[1024];

	if (!path[0])
	{
		char *slash;

		if (!host_executable_path(path, sizeof(path) - 1))
			strcpy(path, "./");
		slash = strrchr(path, '/');
		if (slash)
			slash[1] = 0;
	}
	return path;
}

/* ---------- loading */

static void *read_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	struct stat information;
	void *data;

	if (!file)
		return NULL;
	if (fstat(fileno(file), &information) != 0 || information.st_size <= 0 ||
		!(data = malloc((size_t)information.st_size)))
	{
		fclose(file);
		return NULL;
	}
	if (fread(data, 1, (size_t)information.st_size, file) != (size_t)information.st_size)
	{
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	*size = (size_t)information.st_size;
	return data;
}

/* a copy of a NULL-terminated list of strings in guest memory */
static uint32_t copy_strings(char *const *strings, uint32_t *count)
{
	size_t total = 0, index, number = 0;
	uint32_t *array;
	char *text;

	for (index = 0; strings[index]; index++)
	{
		total += strlen(strings[index]) + 1;
		number++;
	}
	array = host_low_map((number + 1) * sizeof(uint32_t) + total, PROT_READ | PROT_WRITE);
	if (!array)
		host_fatal("cannot allocate guest memory for the command line");
	text = (char *)(array + number + 1);
	for (index = 0; index < number; index++)
	{
		size_t length = strlen(strings[index]) + 1;

		memcpy(text, strings[index], length);
		array[index] = (uint32_t)(uintptr_t)text;
		text += length;
	}
	array[number] = 0;
	if (count)
		*count = (uint32_t)number;
	return (uint32_t)(uintptr_t)array;
}

static void *guest_main(void *boot)
{
	host_logf(HOST_LOG_INFO, "game thread started");
	((void (*)(uint32_t))(uintptr_t)host_image.header->start)((uint32_t)(uintptr_t)boot);
	host_fatal("the guest returned from __guest_start");
	return NULL;
}

/* the command line, for posix_command_line_argument (posix_net.c), which on
Linux reads /proc/self/cmdline: Haiku has no /proc */
int halo_host_argc;
char **halo_host_argv;

int main(int argc, char **argv)
{
	const char *image_path = getenv("HALO_GUEST_IMAGE");
	char path[1100];
	struct halo_guest_boot *boot;
	void *image;
	size_t image_size = 0;
	int error;

	halo_host_argc = argc;
	halo_host_argv = argv;
	/* first: the address space below 4 GB, before SDL and the OpenGL
	driver take any of it */
	if (host_memory_initialize() != 0)
		host_fatal("the memory the game needs below 4 GB is in use (see the log)");
	if (!image_path || !*image_path)
	{
		snprintf(path, sizeof(path), "%s%s", host_base_path(), GUEST_IMAGE_NAME);
		image_path = path;
	}
	image = read_file(image_path, &image_size);
	if (!image)
		host_fatal("cannot read the game's image, %s: %s", image_path, strerror(errno));
	if (host_load_image(image, image_size) != 0)
		host_fatal("cannot load the game's image, %s (see the log)", image_path);
	free(image);
	host_install_signal_handlers();

	boot = host_low_map(sizeof(*boot), PROT_READ | PROT_WRITE);
	if (!boot)
		host_fatal("cannot allocate guest memory");
	boot->argv = copy_strings(argv, &boot->argc);
	boot->environment = copy_strings(environ, NULL);
	boot->page_size = (uint32_t)sysconf(_SC_PAGESIZE);

	error = host_native_thread_create(guest_main, boot, MAIN_STACK_SIZE);
	if (error)
		host_fatal("cannot start the game's thread (%s)", strerror(error));
	/* (the game ends the process: host_exit) */
	for (;;)
		pause();
	return 0;
}
