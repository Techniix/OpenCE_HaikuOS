/*
HALO_X32_ABI.H

The contract between the two halves of the Haiku port (see
port/haiku/README.md):

- the guest: the game, the platform layer shared with the Linux port and a
  small C runtime (musl), compiled as x32 code (x86-64 instructions with
  32-bit pointers, clang's x86_64-linux-gnux32 target) and linked into a
  static image that runs in the low 4 GB of the process;
- the host: an ordinary 64-bit program (Haiku's, or Linux's for testing)
  that loads the image, owns the process (SDL3, OpenGL, the C library) and
  serves the guest's requests.

This is the Android port's design (port/android/README.md, "ILP32 code") on
x86-64. The guest calls the host through import stubs that jump through a
table of 64-bit function pointers the host fills in at load time. Only
types whose layout and register treatment agree between the two ABIs cross
this boundary: 32-bit integers, 64-bit integers, floats, and pointers (which
the x32 ABI passes zero-extended to 64 bits, in registers and on the stack
alike). Structures shared here are made of fixed-width members only.

This header is included by both halves.
*/

#ifndef __HALO_X32_ABI_H
#define __HALO_X32_ABI_H

#include <stdint.h>

/* the Xbox contiguous memory window (port/linux/src/platform.h): the
desktop builds' 512 MB */
#define HALO_GUEST_WINDOW_BASE 0x80000000u
#define HALO_GUEST_WINDOW_SIZE 0x20000000u

/* the guest image is linked to run here (port/haiku/guest/guest.ld), below
2 GB: clang's x32 code reaches some of its data with 32-bit displacements
that the processor sign-extends (R_X86_64_32S) */
#define HALO_GUEST_IMAGE_BASE 0x50000000u
/* the room the host reserves for the image there */
#define HALO_GUEST_IMAGE_RESERVE 0x10000000u

/* the window Halo Custom Edition tag data are linked to
(CUSTOM_EDITION_TAG_CACHE_ADDRESS, port/linux/game/cache_file_formats.h),
which the guest maps itself at that address: the host's own allocations for
the guest stay out of it */
#define HALO_GUEST_CUSTOM_EDITION_BASE 0x40000000u
#define HALO_GUEST_CUSTOM_EDITION_SIZE 0x02000000u

#define HALO_GUEST_MAGIC 0x32335848u /* 'HX32' */
#define HALO_GUEST_ABI_VERSION 1

/* at HALO_GUEST_IMAGE_BASE */
struct halo_guest_header
{
	uint32_t magic;
	uint32_t abi_version;
	uint32_t image_end;          /* end of .bss */
	uint32_t import_table;       /* uint64_t[import_count], filled by the host */
	uint32_t import_names;       /* import_count NUL-terminated names */
	uint32_t import_count;       /* address of a uint32_t holding the count */
	uint32_t start;              /* void __guest_start(struct halo_guest_boot *) */
	uint32_t thread_start;       /* void __guest_thread_start(uint32_t thread) */
	uint32_t thread_attach;      /* uint32_t __guest_thread_attach(void) */
	uint32_t thread_detach;      /* void __guest_thread_detach(void) */
	uint32_t init_array_start;   /* void (*)(void) entries, 4 bytes each */
	uint32_t init_array_end;
};

/* the host's description of the process, handed to __guest_start */
struct halo_guest_boot
{
	uint32_t argc;
	uint32_t argv;               /* char ** in guest memory */
	uint32_t environment;        /* char ** in guest memory, NULL-terminated */
	uint32_t page_size;
};

#endif
