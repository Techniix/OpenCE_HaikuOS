/*
HOST_SYSTEM.C

What differs between the systems the host runs on: Haiku, and Linux (for
testing). The guest sees Linux's numbers whatever the host (its musl is
built for Linux's system call table), so the host's errno values are
translated to Linux's here.
*/

#include "host.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#if defined(__HAIKU__)
#include <OS.h>
#include <image.h>
#else
#include <sys/syscall.h>
#endif

int host_executable_path(char *path, size_t size)
{
#if defined(__HAIKU__)
	image_info information;
	int32 cookie = 0;

	/* (the program's own image is the first of B_APP_IMAGE) */
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &information) == B_OK)
	{
		if (information.type == B_APP_IMAGE)
		{
			if (strlen(information.name) >= size)
				return 0;
			strcpy(path, information.name);
			return 1;
		}
	}
	return 0;
#else
	ssize_t length = readlink("/proc/self/exe", path, size - 1);

	if (length <= 0)
		return 0;
	path[length] = 0;
	return 1;
#endif
}

const char *host_system_name(void)
{
#if defined(__HAIKU__)
	return "Haiku";
#else
	return "Linux";
#endif
}

long long host_thread_id(void)
{
#if defined(__HAIKU__)
	return (long long)find_thread(NULL);
#else
	return (long long)syscall(SYS_gettid);
#endif
}

int host_linux_errno(int error)
{
	static const struct
	{
		int host;
		int guest;
	} errors[] =
	{
		{ EPERM, 1 }, { ENOENT, 2 }, { ESRCH, 3 }, { EINTR, 4 }, { EIO, 5 }, { ENXIO, 6 }, { E2BIG, 7 },
		{ ENOEXEC, 8 }, { EBADF, 9 }, { ECHILD, 10 }, { EAGAIN, 11 }, { ENOMEM, 12 }, { EACCES, 13 },
		{ EFAULT, 14 }, { EBUSY, 16 }, { EEXIST, 17 }, { EXDEV, 18 }, { ENODEV, 19 }, { ENOTDIR, 20 },
		{ EISDIR, 21 }, { EINVAL, 22 }, { ENFILE, 23 }, { EMFILE, 24 }, { ENOTTY, 25 }, { ETXTBSY, 26 },
		{ EFBIG, 27 }, { ENOSPC, 28 }, { ESPIPE, 29 }, { EROFS, 30 }, { EMLINK, 31 }, { EPIPE, 32 },
		{ EDOM, 33 }, { ERANGE, 34 }, { EDEADLK, 35 }, { ENAMETOOLONG, 36 }, { ENOLCK, 37 }, { ENOSYS, 38 },
		{ ENOTEMPTY, 39 }, { ELOOP, 40 }, { ENOMSG, 42 }, { EIDRM, 43 }, { ENOSTR, 60 }, { ENODATA, 61 },
		{ ETIME, 62 }, { ENOSR, 63 }, { ENOLINK, 67 }, { EPROTO, 71 }, { EMULTIHOP, 72 }, { EBADMSG, 74 },
		{ EOVERFLOW, 75 }, { EILSEQ, 84 }, { ENOTSOCK, 88 }, { EDESTADDRREQ, 89 }, { EMSGSIZE, 90 },
		{ EPROTOTYPE, 91 }, { ENOPROTOOPT, 92 }, { EPROTONOSUPPORT, 93 }, { EOPNOTSUPP, 95 },
		{ EPFNOSUPPORT, 96 }, { EAFNOSUPPORT, 97 }, { EADDRINUSE, 98 }, { EADDRNOTAVAIL, 99 },
		{ ENETDOWN, 100 }, { ENETUNREACH, 101 }, { ENETRESET, 102 }, { ECONNABORTED, 103 },
		{ ECONNRESET, 104 }, { ENOBUFS, 105 }, { EISCONN, 106 }, { ENOTCONN, 107 }, { ESHUTDOWN, 108 },
		{ ETIMEDOUT, 110 }, { ECONNREFUSED, 111 }, { EHOSTDOWN, 112 }, { EHOSTUNREACH, 113 },
		{ EALREADY, 114 }, { EINPROGRESS, 115 }, { ESTALE, 116 }, { EDQUOT, 122 }, { ECANCELED, 125 },
		{ EOWNERDEAD, 130 }, { ENOTRECOVERABLE, 131 },
	};
	size_t index;

	if (error == 0)
		return 0;
	/* (EWOULDBLOCK and ENOTSUP are EAGAIN and EOPNOTSUPP on Linux) */
	if (error == EWOULDBLOCK)
		return 11;
	if (error == ENOTSUP)
		return 95;
	for (index = 0; index < sizeof(errors) / sizeof(errors[0]); index++)
	{
		if (errors[index].host == error)
			return errors[index].guest;
	}
	/* EIO for one without a Linux number */
	return 5;
}
