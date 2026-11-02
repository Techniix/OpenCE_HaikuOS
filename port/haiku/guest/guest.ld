/*
GUEST.LD

Layout of the x32 guest image (tools/haiku_build.py): linked to run at
HALO_GUEST_IMAGE_BASE (port/haiku/include/halo_x32_abi.h), with the image
header first. The host copies the loadable segments to these addresses and
makes the code read-only. The constructors are the ELF .init_array's, in
the order of their priorities (xbox_memory.c's reserve the fixed windows
first), which __guest_start runs.
*/

ENTRY(__guest_header)

SECTIONS
{
	. = 0x50000000;
	.guest_header : { KEEP(*(.guest_header)) }
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(0x10000);
	.data : { *(.data .data.*) }
	.init_array : {
		__guest_init_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array .ctors))
		__guest_init_end = .;
	}
	.bss : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(0x1000);
	__guest_image_end = .;
	/DISCARD/ : { *(.comment) *(.note.*) *(.fini_array .fini_array.*) *(.dtors) *(.eh_frame) }
}
