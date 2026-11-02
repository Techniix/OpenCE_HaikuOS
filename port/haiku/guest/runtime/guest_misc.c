/*
GUEST_MISC.C

Runtime support functions the platform layer or the code generator expect,
which neither the game nor musl provides.
*/

#include "guest_host.h"

#include <stddef.h>
#include <string.h>

void __stack_chk_fail(void)
{
	host_abort("stack smashing detected");
}

/* <execinfo.h>: the guest has no unwinder */
int backtrace(void **frames, int size)
{
	(void)frames;
	(void)size;
	return 0;
}

void backtrace_symbols_fd(void *const *frames, int size, int fd)
{
	(void)frames;
	(void)size;
	(void)fd;
}

/* ---------- the floating-point environment (msvc_crt.c's _control87)

As glibc's on i386, which the Linux build's _control87 was written
against: fegetenv and fesetenv save and load the whole x87 environment and
keep the SSE unit's rounding, exception masks and flags in step with the
x87 control and status words (musl's x32 ones load the saved MXCSR as it
was, so a change to the control word would not reach SSE arithmetic, which
is all the guest's) */

#include <fenv.h>

int fegetenv(fenv_t *environment)
{
	__asm__ __volatile__("fnstenv (%0)\n\tfldcw (%0)" : : "r"(environment) : "memory");
	__asm__ __volatile__("stmxcsr %0" : "=m"(environment->__mxcsr));
	return 0;
}

int fesetenv(const fenv_t *environment)
{
	fenv_t copy;
	unsigned int mxcsr;

	if (environment == FE_DFL_ENV)
	{
		memset(&copy, 0, sizeof(copy));
		copy.__control_word = 0x37f;
		copy.__tags = 0xffff;
		copy.__mxcsr = 0x1f80;
		environment = &copy;
	}
	__asm__ __volatile__("fldenv (%0)" : : "r"(environment) : "memory");
	/* rounding (RC, bits 10-11 of the control word, to 13-14), the masks
	(bits 0-5, to 7-12) and the flags (the status word's bits 0-5) */
	mxcsr = environment->__mxcsr & ~0x7fbfu;
	mxcsr |= ((unsigned int)environment->__control_word & 0xc00u) << 3;
	mxcsr |= ((unsigned int)environment->__control_word & 0x3fu) << 7;
	mxcsr |= (unsigned int)environment->__status_word & 0x3fu;
	__asm__ __volatile__("ldmxcsr %0" : : "m"(mxcsr));
	return 0;
}
