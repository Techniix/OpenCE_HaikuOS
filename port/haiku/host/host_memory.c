/*
HOST_MEMORY.C

Guest address space for the x32 port.

Everything the guest touches must lie below 4 GB. The host reserves as much
of that range as it can at start-up (address space only: nothing is backed
until it is mapped), before SDL, the OpenGL driver or the C library's own
allocations can take any of it, and keeps a map of its pages:

- the image's range (HALO_GUEST_IMAGE_BASE), taken by the loader;
- the guest's mappings at the addresses it asks for: the Xbox window at
  0x80000000 and the Custom Edition tag cache at 0x40440000
  (port/linux/src/xbox_memory.c);
- the guest's other mappings (malloc arenas) and the stacks of the threads
  that run guest code, wherever there is room outside those ranges.

The reservation is made of 16 MB chunks, each one taken only if all of it
is free: the host program and its libraries may sit anywhere below 4 GB.

This file also implements guest memory write tracking (the interface of
port/linux/src/memory_watch.c): the renderer write-protects the pages behind
the textures it caches, and the SIGSEGV handler here records the first
write to each, as the Android host's does (port/android/host/host_memory.c).
Other faults are reported (with the guest's registers) and end the program.
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if !defined(__HAIKU__)
#include <ucontext.h> /* (Haiku's ucontext_t is <signal.h>'s) */
#endif
#include <unistd.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define PAGE 0x1000ULL
#define PAGE_COUNT (0x100000000ULL / PAGE)
#define CHUNK 0x1000000ULL
#define CHUNK_COUNT (0x100000000ULL / CHUNK)
/* the lowest chunk the host reserves (the first 16 MB stay the host's: the
null page, and what the system puts low) */
#define LOW_START CHUNK
/* the highest: the last 16 MB stay unreserved, as a margin below 4 GB */
#define LOW_END (0x100000000ULL - CHUNK)

/* the guest's Linux mmap flags and protections (the generic table's) */
#define LINUX_PROT_READ 0x1
#define LINUX_PROT_WRITE 0x2
#define LINUX_PROT_EXEC 0x4
#define LINUX_MAP_SHARED 0x01
#define LINUX_MAP_PRIVATE 0x02
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_NORESERVE 0x4000
#define LINUX_MAP_FIXED_NOREPLACE 0x100000
#define LINUX_ENOMEM 12
#define LINUX_EINVAL 22
#define LINUX_EEXIST 17

static unsigned char chunk_reserved[CHUNK_COUNT];
/* per page: 1 if handed out (to the guest, or a stack) */
static unsigned char page_used[PAGE_COUNT];
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t next_search = LOW_START;
static uint64_t image_base, image_end;

static uint64_t round_up(uint64_t value)
{
	return (value + PAGE - 1) & ~(PAGE - 1);
}

static int protection_to_host(int protection)
{
	return (protection & LINUX_PROT_READ ? PROT_READ : 0) | (protection & LINUX_PROT_WRITE ? PROT_WRITE : 0) |
		(protection & LINUX_PROT_EXEC ? PROT_EXEC : 0);
}

/* address space, unbacked: what a range goes back to when it is unmapped */
static int make_reserved(uint64_t address, uint64_t size)
{
	return mmap((void *)(uintptr_t)address, size, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == (void *)(uintptr_t)address ? 0 : -1;
}

/* takes the chunk at address if all of it is free; 1 if it did */
static int reserve_chunk(uint64_t address)
{
	void *wanted = (void *)(uintptr_t)address;
	void *result;

#ifdef MAP_FIXED_NOREPLACE
	result = mmap(wanted, CHUNK, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
#else
	/* (an address the system takes as a hint, as POSIX has it: somewhere
	else when anything is there) */
	result = mmap(wanted, CHUNK, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#endif
	if (result == wanted)
		return 1;
	if (result != MAP_FAILED)
		munmap(result, CHUNK);
	return 0;
}

int host_memory_initialize(void)
{
	uint64_t address;
	int count = 0;

	for (address = LOW_START; address < LOW_END; address += CHUNK)
	{
		if (reserve_chunk(address))
		{
			chunk_reserved[address / CHUNK] = 1;
			count++;
		}
	}
	host_logf(HOST_LOG_INFO, "guest memory: %d MB of the low 4 GB reserved", count * 16);
	/* the fixed ranges must be wholly there */
	for (address = HALO_GUEST_WINDOW_BASE; address < (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE;
		address += CHUNK)
	{
		if (!chunk_reserved[address / CHUNK])
		{
			host_logf(HOST_LOG_ERROR, "guest memory: %08llx, in the Xbox window, is taken", (unsigned long long)address);
			return -1;
		}
	}
	for (address = HALO_GUEST_IMAGE_BASE; address < (uint64_t)HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE;
		address += CHUNK)
	{
		if (!chunk_reserved[address / CHUNK])
		{
			host_logf(HOST_LOG_ERROR, "guest memory: %08llx, in the guest image's range, is taken",
				(unsigned long long)address);
			return -1;
		}
	}
	for (address = HALO_GUEST_CUSTOM_EDITION_BASE;
		address < (uint64_t)HALO_GUEST_CUSTOM_EDITION_BASE + HALO_GUEST_CUSTOM_EDITION_SIZE; address += CHUNK)
	{
		if (!chunk_reserved[address / CHUNK])
			host_logf(HOST_LOG_WARN, "guest memory: %08llx, in the Custom Edition tag cache, is taken: "
				"Custom Edition maps cannot run", (unsigned long long)address);
	}
	return 0;
}

/* 1 if every page of the range is reserved and none is handed out */
static int range_free(uint64_t address, uint64_t size)
{
	uint64_t page;

	if (address < LOW_START || address + size > LOW_END || address + size < address)
		return 0;
	for (page = address / PAGE; page < (address + size) / PAGE; page++)
	{
		if (!chunk_reserved[page * PAGE / CHUNK] || page_used[page])
			return 0;
	}
	return 1;
}

/* 1 if every page of the range was handed out */
static int range_used(uint64_t address, uint64_t size)
{
	uint64_t page;

	if (address < LOW_START || address + size > LOW_END || address + size < address)
		return 0;
	for (page = address / PAGE; page < (address + size) / PAGE; page++)
	{
		if (!page_used[page])
			return 0;
	}
	return 1;
}

static void mark(uint64_t address, uint64_t size, int used)
{
	memset(&page_used[address / PAGE], used, (size_t)(size / PAGE));
}

/* the ranges the guest asks for by address, which nothing else gets */
static int in_fixed_range(uint64_t address, uint64_t size)
{
	static const uint64_t ranges[][2] =
	{
		{ HALO_GUEST_CUSTOM_EDITION_BASE, (uint64_t)HALO_GUEST_CUSTOM_EDITION_BASE + HALO_GUEST_CUSTOM_EDITION_SIZE },
		{ HALO_GUEST_WINDOW_BASE, (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE },
		{ HALO_GUEST_IMAGE_BASE, (uint64_t)HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE },
	};
	size_t index;

	for (index = 0; index < sizeof(ranges) / sizeof(ranges[0]); index++)
	{
		if (address < ranges[index][1] && address + size > ranges[index][0])
			return 1;
	}
	return 0;
}

/* a free range of size bytes outside the fixed ranges, marked used; 0 if
there is none (under memory_lock) */
static uint64_t find_free(uint64_t size)
{
	uint64_t pages = size / PAGE;
	int pass;

	for (pass = 0; pass < 2; pass++)
	{
		uint64_t start = pass ? LOW_START : next_search;
		uint64_t end = pass ? next_search : LOW_END;
		uint64_t page = start / PAGE, run = 0;

		for (; page < end / PAGE; page++)
		{
			uint64_t address = page * PAGE;

			if (!chunk_reserved[address / CHUNK] || page_used[page] || in_fixed_range(address, PAGE))
			{
				run = 0;
				/* (a whole chunk at a time where it is not the guest's) */
				if (!chunk_reserved[address / CHUNK] || in_fixed_range(address, PAGE))
					page = (address / CHUNK + 1) * CHUNK / PAGE - 1;
				continue;
			}
			if (++run == pages)
			{
				uint64_t found = (page + 1 - pages) * PAGE;

				mark(found, size, 1);
				next_search = found + size;
				return found;
			}
		}
	}
	return 0;
}

int host_memory_take_image(uint32_t base, uint32_t size)
{
	uint64_t length = round_up(size);
	int result = -1;

	pthread_mutex_lock(&memory_lock);
	if (range_free(base, length) &&
		mmap((void *)(uintptr_t)base, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
		(void *)(uintptr_t)base)
	{
		mark(base, length, 1);
		image_base = base;
		image_end = base + length;
		result = 0;
	}
	pthread_mutex_unlock(&memory_lock);
	if (result)
		host_logf(HOST_LOG_ERROR, "guest memory: cannot map the guest image at %08x", base);
	return result;
}

void *host_low_map(size_t size, int protection)
{
	uint64_t length = round_up(size), address;

	pthread_mutex_lock(&memory_lock);
	address = find_free(length);
	if (address && mmap((void *)(uintptr_t)address, length, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
		0) != (void *)(uintptr_t)address)
	{
		mark(address, length, 0);
		address = 0;
	}
	pthread_mutex_unlock(&memory_lock);
	return (void *)(uintptr_t)address;
}

void host_low_unmap(void *pointer, size_t size)
{
	uint64_t address = (uint64_t)(uintptr_t)pointer, length = round_up(size);

	if (!pointer)
		return;
	pthread_mutex_lock(&memory_lock);
	if (range_used(address, length))
	{
		make_reserved(address, length);
		mark(address, length, 0);
	}
	pthread_mutex_unlock(&memory_lock);
}

/* ---------- the guest's mmap, munmap and mprotect */

void host_memory_watch_forget(uint32_t address, uint32_t size);

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size);
	int fixed = (flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE)) != 0;
	int host_flags = MAP_FIXED | (flags & LINUX_MAP_SHARED ? MAP_SHARED : MAP_PRIVATE) |
		(flags & LINUX_MAP_ANONYMOUS ? MAP_ANONYMOUS : 0) | (flags & LINUX_MAP_NORESERVE ? MAP_NORESERVE : 0);
	long result = -LINUX_ENOMEM;

	if (!size || (address & (PAGE - 1)) || (offset & (int64_t)(PAGE - 1)))
		return -LINUX_EINVAL;
	pthread_mutex_lock(&memory_lock);
	if (fixed)
	{
		/* (the Xbox window, the tag cache: their whole range at once, free
		as the guest asks for them; MAP_FIXED over what the guest has
		mapped already replaces it) */
		if (!range_free(address, length) && !((flags & LINUX_MAP_FIXED) && range_used(address, length)))
		{
			pthread_mutex_unlock(&memory_lock);
			return flags & LINUX_MAP_FIXED_NOREPLACE ? -LINUX_EEXIST : -LINUX_ENOMEM;
		}
		mark(address, length, 1);
	}
	else if (!(address && !in_fixed_range(address, length) && range_free(address, length) &&
		(mark(address, length, 1), 1)))
	{
		address = find_free(length);
	}
	if (address)
	{
		void *mapped = mmap((void *)(uintptr_t)address, length, protection_to_host(protection), host_flags,
			flags & LINUX_MAP_ANONYMOUS ? -1 : fd, (off_t)offset);

		if (mapped == (void *)(uintptr_t)address)
		{
			result = (long)address;
		}
		else
		{
			result = -host_linux_errno(errno);
			make_reserved(address, length);
			mark(address, length, 0);
		}
	}
	pthread_mutex_unlock(&memory_lock);
	if (result > 0)
		host_memory_watch_forget((uint32_t)address, (uint32_t)length);
	return result;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size);
	long result = -LINUX_EINVAL;

	if ((address & (PAGE - 1)) || !size)
		return -LINUX_EINVAL;
	pthread_mutex_lock(&memory_lock);
	if (address >= LOW_START && address + length <= LOW_END &&
		!(address < image_end && address + length > image_base))
	{
		make_reserved(address, length);
		mark(address, length, 0);
		result = 0;
	}
	pthread_mutex_unlock(&memory_lock);
	if (result == 0)
		host_memory_watch_forget((uint32_t)address, (uint32_t)length);
	return result;
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	uint64_t length = round_up(size);

	if ((address & (PAGE - 1)) || address + length > 0x100000000ULL)
		return -LINUX_EINVAL;
	host_memory_watch_forget((uint32_t)address, (uint32_t)length);
	if (mprotect((void *)(uintptr_t)address, length, protection_to_host(protection)) != 0)
		return -host_linux_errno(errno);
	return 0;
}

/* ---------- write tracking (port/linux/src/memory_watch.c's interface) */

#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / PAGE)

/* per page of the Xbox window: 0 unwatched, 1 write-protected, 2 written */
static volatile unsigned char page_protected[WATCH_PAGE_COUNT];
static volatile uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;

static int in_window(uint64_t address)
{
	return address >= HALO_GUEST_WINDOW_BASE && address - HALO_GUEST_WINDOW_BASE < HALO_GUEST_WINDOW_SIZE;
}

static uint64_t watch_page(uint64_t address)
{
	return (address - HALO_GUEST_WINDOW_BASE) / PAGE;
}

static void mark_written(uint64_t page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 2;
	mprotect((void *)(uintptr_t)(HALO_GUEST_WINDOW_BASE + page * PAGE), PAGE, PROT_READ | PROT_WRITE);
}

/* the guest's frames: rbp chains (the guest keeps frame pointers) of 4-byte
saved rbp and 8-byte return addresses: x32 pushes 64-bit values */
static void report_frames(uint64_t rbp)
{
	int index;

	for (index = 0; index < 24 && rbp && rbp < 0x100000000ULL && (rbp & 7) == 0; index++)
	{
		const uint64_t *frame = (const uint64_t *)(uintptr_t)rbp;

		if (!page_used[rbp / PAGE] || !page_used[(rbp + 15) / PAGE])
			break;
		host_logf(HOST_LOG_ERROR, "  frame %2d: return %016llx", index, (unsigned long long)frame[1]);
		rbp = frame[0];
	}
}

static void report_crash(int signal_number, siginfo_t *information, void *context)
{
	ucontext_t *ucontext = context;
	uint64_t pc, sp, bp;

#if defined(__HAIKU__)
	pc = ucontext->uc_mcontext.rip;
	sp = ucontext->uc_mcontext.rsp;
	bp = ucontext->uc_mcontext.rbp;
#else
	pc = (uint64_t)ucontext->uc_mcontext.gregs[REG_RIP];
	sp = (uint64_t)ucontext->uc_mcontext.gregs[REG_RSP];
	bp = (uint64_t)ucontext->uc_mcontext.gregs[REG_RBP];
#endif
	host_logf(HOST_LOG_ERROR, "signal %d at address %p: pc %016llx sp %016llx bp %016llx",
		signal_number, information->si_addr, (unsigned long long)pc, (unsigned long long)sp,
		(unsigned long long)bp);
	if (pc >= host_image.base && pc < host_image.end)
		host_logf(HOST_LOG_ERROR, "  in the guest image: llvm-symbolizer --obj=halo_guest.elf 0x%llx",
			(unsigned long long)pc);
	report_frames(bp);
}

static void fatal_handler(int signal_number, siginfo_t *information, void *context)
{
	struct sigaction action;

	report_crash(signal_number, information, context);
	/* the default action, when the instruction runs again */
	memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	sigaction(signal_number, &action, NULL);
}

static void segv_handler(int signal_number, siginfo_t *information, void *context)
{
	uint64_t address = (uint64_t)(uintptr_t)information->si_addr;

	if (watch_active && in_window(address))
	{
		uint64_t page = watch_page(address);

		if (page_protected[page] == 1)
		{
			mark_written(page);
			return;
		}
		if (page_protected[page] == 2)
			return;
	}
	fatal_handler(signal_number, information, context);
}

void host_install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = segv_handler;
	sigaction(SIGSEGV, &action, NULL);
	action.sa_sigaction = fatal_handler;
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGFPE, &action, NULL);
}

void host_memory_watch_initialize(void)
{
	watch_active = 1;
}

/* memory_watch_protect: write-protects the pages of a range; a range
outside the guest window is ignored */
void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || !size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page] != 1)
		{
			page_protected[page] = 1;
			mprotect((void *)(uintptr_t)(HALO_GUEST_WINDOW_BASE + page * PAGE), PAGE, PROT_READ);
		}
	}
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

/* memory_watch_generation: the newest generation of the pages of a range */
uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;
	uint32_t newest = 0;

	if (!size || !in_window(address))
		return 0;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

/* memory_watch_prepare_write: unprotects the pages the caller itself is
about to write into */
void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t start = address, first, last, page;

	if (!watch_active || !size)
		return;
	if (start + size <= HALO_GUEST_WINDOW_BASE || start >= (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		return;
	if (start < HALO_GUEST_WINDOW_BASE)
		start = HALO_GUEST_WINDOW_BASE;
	first = watch_page(start);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page] == 1)
			mark_written(page);
	}
}

/* memory_watch_forget: the pages of a range were remapped or reprotected, so
treat them as written and unwatched */
void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}

void host_memory_watch_begin_frame(void)
{
}
