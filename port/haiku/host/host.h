/*
HOST.H

Internals of the Haiku port's host program (build/haiku/halo). See
port/haiku/README.md for the overall design and
port/haiku/include/halo_x32_abi.h for the guest contract.

The host is written against POSIX, SDL3 and OpenGL, so that the same
program runs on Linux (x86-64), where the guest and most of the host can be
tested without a Haiku machine; what differs between the two systems is in
host_system.c.
*/

#ifndef __HALO_X32_HOST_H
#define __HALO_X32_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "halo_x32_abi.h"

#define GUEST(type, value) ((type)(uintptr_t)(uint32_t)(value))

/* ---------- logging (standard error) */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
/* guest services also used inside the host */
void host_exit(int code) __attribute__((noreturn));
int host_errno(void);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- the system (host_system.c) */

/* the full path of this program; 1 on success */
int host_executable_path(char *path, size_t size);
/* the Linux errno value of a host one (Haiku's are not Linux's) */
int host_linux_errno(int error);
/* the name of the system, for the guest's uname */
const char *host_system_name(void);
/* the calling thread's id */
long long host_thread_id(void);

/* ---------- guest memory (host_memory.c)

All memory the guest can address lies below 4 GB. The host reserves as
much of that range as it can at start-up, before SDL or the OpenGL driver
take any of it, and hands out pages from it: the image's range, the guest's
own mappings (its malloc arenas, the Xbox window and the Custom Edition tag
cache at the addresses it asks for) and thread stacks. */

/* reserves the range; returns 0 on success */
int host_memory_initialize(void);
/* takes the fixed range of the image; returns 0 on success */
int host_memory_take_image(uint32_t image_base, uint32_t image_size);
/* page-granular allocations below 4 GB; NULL on failure */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
/* the guest's mmap/munmap/mprotect/madvise (host_syscall.c), with Linux's
flags; the results as the system call's */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);
/* the SIGSEGV handler of the write tracking, and the crash report */
void host_install_signal_handlers(void);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
};

extern struct host_guest_image host_image;

/* maps the image from the ELF file in memory; returns 0 on success */
int host_load_image(const void *elf, size_t size);

/* ---------- entering guest code (host_thread.c) */

/* calls the guest function at address with up to four 32-bit arguments on
this thread, which must have been made by host_native_thread_create (giving
the thread a guest struct pthread first if it has none); returns the
guest's eax */
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* starts a thread running function(argument) with its stack in guest
memory, so that it can call guest code; the stack is freed after it exits.
Returns 0 or an errno value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
/* 1 if this thread runs on a stack in guest memory */
int host_on_guest_stack(void);
uint32_t host_get_tp(void);
void host_set_tp(uint32_t thread);

/* ---------- import table (host_import_table.c, generated) */

/* the host function for an import name, or NULL */
void *host_resolve_import(const char *name);

/* ---------- SDL / GL (host_sdl.c, host_gl.c) */

/* an import of an OpenGL function (hostgl_<name>): its slot is filled in
once a context exists (host_gl_bind_imports); until then a call of it is
fatal */
void host_gl_note_import(uint64_t *slot, const char *name);
/* fills in the imports of OpenGL functions, with the current context's;
the number missing */
int host_gl_bind_imports(void);
/* the program's folder, ending with a separator (SDL_GetBasePath's) */
const char *host_base_path(void);

#endif
