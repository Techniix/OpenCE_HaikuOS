/*
HOST_LOADER.C

Loads the guest image: a statically linked x32 ELF executable (32-bit ELF,
x86-64 code; tools/haiku_build.py) whose segments are copied to the
addresses it was linked at, below 4 GB. The image starts with a struct
halo_guest_header naming its import table, which is filled with the host
functions of the same names. (As the Android port's loader,
port/android/host/host_loader.c, for its 64-bit images.)
*/

#include "host.h"

#include <string.h>
#include <sys/mman.h>

/* the ELF definitions this needs (Haiku's <elf.h> is not every system's) */
#define ELF_MAGIC "\177ELF"
#define ELF_CLASS32 1
#define ELF_EXECUTABLE 2
#define ELF_MACHINE_X86_64 62
#define ELF_SEGMENT_LOAD 1
#define ELF_SEGMENT_EXECUTE 1

struct elf32_header
{
	unsigned char ident[16];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint32_t entry;
	uint32_t program_headers;
	uint32_t section_headers;
	uint32_t flags;
	uint16_t header_size;
	uint16_t program_header_size;
	uint16_t program_header_count;
	uint16_t section_header_size;
	uint16_t section_header_count;
	uint16_t section_names;
};

struct elf32_segment
{
	uint32_t type;
	uint32_t offset;
	uint32_t address;
	uint32_t physical_address;
	uint32_t file_size;
	uint32_t memory_size;
	uint32_t flags;
	uint32_t alignment;
};

struct host_guest_image host_image;

static void missing_import(void)
{
	host_fatal("the guest called a host function that is not available");
}

int host_load_image(const void *file, size_t size)
{
	const struct elf32_header *elf = file;
	const struct elf32_segment *segments;
	uint64_t low = ~0ULL, high = 0;
	const struct halo_guest_header *header;
	uint64_t *table;
	const char *name;
	uint32_t count, index;
	int missing = 0;

	if (size < sizeof(*elf) || memcmp(elf->ident, ELF_MAGIC, 4) || elf->ident[4] != ELF_CLASS32 ||
		elf->machine != ELF_MACHINE_X86_64 || elf->type != ELF_EXECUTABLE ||
		elf->program_header_size != sizeof(struct elf32_segment) ||
		(uint64_t)elf->program_headers + (uint64_t)elf->program_header_count * sizeof(struct elf32_segment) > size)
	{
		host_logf(HOST_LOG_ERROR, "the guest image is not an x32 executable");
		return -1;
	}
	segments = (const struct elf32_segment *)((const char *)file + elf->program_headers);
	for (index = 0; index < elf->program_header_count; index++)
	{
		if (segments[index].type != ELF_SEGMENT_LOAD)
			continue;
		if (segments[index].address < low)
			low = segments[index].address;
		if ((uint64_t)segments[index].address + segments[index].memory_size > high)
			high = (uint64_t)segments[index].address + segments[index].memory_size;
	}
	low &= ~0xfffULL;
	high = (high + 0xfff) & ~0xfffULL;
	if (low != HALO_GUEST_IMAGE_BASE || high > (uint64_t)HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_RESERVE)
	{
		host_logf(HOST_LOG_ERROR, "the guest image spans %llx-%llx", (unsigned long long)low, (unsigned long long)high);
		return -1;
	}
	if (host_memory_take_image((uint32_t)low, (uint32_t)(high - low)) != 0)
		return -1;
	for (index = 0; index < elf->program_header_count; index++)
	{
		const struct elf32_segment *segment = &segments[index];

		if (segment->type != ELF_SEGMENT_LOAD)
			continue;
		if ((uint64_t)segment->offset + segment->file_size > size)
			return -1;
		memcpy(GUEST(void *, segment->address), (const char *)file + segment->offset, segment->file_size);
	}

	header = GUEST(const struct halo_guest_header *, low);
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		return -1;
	}
	host_image.header = header;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;

	table = GUEST(uint64_t *, header->import_table);
	name = GUEST(const char *, header->import_names);
	count = *GUEST(const uint32_t *, header->import_count);
	for (index = 0; index < count; index++)
	{
		void *function = host_resolve_import(name);

		/* (OpenGL's entry points can be had only once there is a context:
		host_gl_bind_imports fills them in then) */
		if (!function && !strncmp(name, "hostgl_", 7))
		{
			host_gl_note_import(&table[index], name + 7);
			name += strlen(name) + 1;
			continue;
		}
		if (!function)
		{
			host_logf(HOST_LOG_WARN, "guest import %s is not available", name);
			function = (void *)missing_import;
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx, %u imports (%d unavailable)",
		(unsigned long long)low, (unsigned long long)high, count, missing);

	/* code becomes read-only and executable */
	for (index = 0; index < elf->program_header_count; index++)
	{
		const struct elf32_segment *segment = &segments[index];
		uint64_t start = segment->address & ~0xfffULL;
		uint64_t end = ((uint64_t)segment->address + segment->memory_size + 0xfff) & ~0xfffULL;

		if (segment->type == ELF_SEGMENT_LOAD && (segment->flags & ELF_SEGMENT_EXECUTE))
			mprotect((void *)(uintptr_t)start, end - start, PROT_READ | PROT_EXEC);
	}
	return 0;
}
