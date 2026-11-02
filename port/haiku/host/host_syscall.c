/*
HOST_SYSCALL.C

System calls on behalf of the guest's musl runtime (its syscall_arch.h
sends every call here, with the numbers of the generic Linux table).

The Android host passes most calls straight to the Linux kernel
(port/android/host/host_syscall.c). This host may be Haiku's, which has
no Linux system calls, so every call is performed with the host's own C
library instead, converting what differs:

- numbers: errno values (host_linux_errno), open flags, *at flags,
  AT_FDCWD, poll events and clock ids are Linux's in the guest;
- structures whose ILP32 or Linux layout differs from the host's: timespec
  and timeval (the guest's time_t is 32-bit, as in the MSVC runtime), iovec,
  struct stat, directory entries, utsname;
- futexes, which musl's locks and condition variables are built on: the
  host keeps its own wait queues (Haiku has no futex call);
- memory mappings, which must stay below 4 GB (host_memory.c);
- process exit, and calls that have no meaning for the guest (signal
  handlers, which the host owns).

The guest's pointers are valid host addresses, and its integer arguments
arrive extended to 64 bits (syscall_arch.h's long long).
*/

#include "host.h"
#include "guest_syscalls.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* ---------- the guest's (Linux's) numbers */

#define LINUX_EPERM 1
#define LINUX_ENOENT 2
#define LINUX_EINTR 4
#define LINUX_EBADF 9
#define LINUX_EAGAIN 11
#define LINUX_EFAULT 14
#define LINUX_EINVAL 22
#define LINUX_ENOTTY 25
#define LINUX_ERANGE 34
#define LINUX_ENOSYS 38
#define LINUX_ETIMEDOUT 110

#define LINUX_O_ACCMODE 03
#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_NOCTTY 0400
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_DSYNC 010000
#define LINUX_O_DIRECTORY 040000
#define LINUX_O_NOFOLLOW 0100000
#define LINUX_O_CLOEXEC 02000000
#define LINUX_O_SYNC 04010000

#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EACCESS 0x200
#define LINUX_AT_EMPTY_PATH 0x1000

#define LINUX_F_DUPFD 0
#define LINUX_F_GETFD 1
#define LINUX_F_SETFD 2
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4
#define LINUX_F_DUPFD_CLOEXEC 1030

#define LINUX_POLLIN 0x001
#define LINUX_POLLPRI 0x002
#define LINUX_POLLOUT 0x004
#define LINUX_POLLERR 0x008
#define LINUX_POLLHUP 0x010
#define LINUX_POLLNVAL 0x020

#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_MONOTONIC 1
#define LINUX_CLOCK_PROCESS_CPUTIME_ID 2
#define LINUX_CLOCK_THREAD_CPUTIME_ID 3
#define LINUX_CLOCK_MONOTONIC_RAW 4
#define LINUX_CLOCK_REALTIME_COARSE 5
#define LINUX_CLOCK_MONOTONIC_COARSE 6
#define LINUX_CLOCK_BOOTTIME 7
#define LINUX_TIMER_ABSTIME 1

#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_REQUEUE 3
#define LINUX_FUTEX_CMP_REQUEUE 4
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10
#define LINUX_FUTEX_PRIVATE 128
#define LINUX_FUTEX_CLOCK_REALTIME 256

#define LINUX_SIGABRT 6

#define LINUX_DT_UNKNOWN 0
#define LINUX_DT_DIR 4
#define LINUX_DT_REG 8
#define LINUX_DT_LNK 10

/* the guest's structures */
struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

/* port/haiku/guest/libc/arch/x32/kstat.h */
struct guest_stat
{
	uint64_t dev;
	uint64_t ino;
	uint32_t mode;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint64_t rdev;
	uint64_t pad;
	int64_t size;
	int32_t blksize;
	int32_t pad2;
	int64_t blocks;
	int64_t atime_seconds;
	int64_t atime_nanoseconds;
	int64_t mtime_seconds;
	int64_t mtime_nanoseconds;
	int64_t ctime_seconds;
	int64_t ctime_nanoseconds;
	uint32_t unused[2];
};

struct guest_dirent64
{
	uint64_t ino;
	int64_t offset;
	uint16_t record_length;
	uint8_t type;
	char name[];
};

struct guest_utsname
{
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

static int timespec_in(uint64_t address, struct timespec *result)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	if (!address)
		return 0;
	result->tv_sec = value->seconds;
	result->tv_nsec = value->nanoseconds;
	return 1;
}

static void timespec_out(uint64_t address, const struct timespec *value)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!address)
		return;
	result->seconds = (int32_t)value->tv_sec;
	result->nanoseconds = (int32_t)value->tv_nsec;
}

static long result_of(long value)
{
	return value == -1 ? -host_linux_errno(errno) : value;
}

static int file_descriptor(long long fd)
{
	return fd == LINUX_AT_FDCWD ? AT_FDCWD : (int)fd;
}

static int open_flags_to_host(int flags)
{
	int result;

	switch (flags & LINUX_O_ACCMODE)
	{
	case 1: result = O_WRONLY; break;
	case 2: result = O_RDWR; break;
	default: result = O_RDONLY; break;
	}
	if (flags & LINUX_O_CREAT)
		result |= O_CREAT;
	if (flags & LINUX_O_EXCL)
		result |= O_EXCL;
	if (flags & LINUX_O_NOCTTY)
		result |= O_NOCTTY;
	if (flags & LINUX_O_TRUNC)
		result |= O_TRUNC;
	if (flags & LINUX_O_APPEND)
		result |= O_APPEND;
	if (flags & LINUX_O_NONBLOCK)
		result |= O_NONBLOCK;
	if ((flags & LINUX_O_SYNC) == LINUX_O_SYNC)
		result |= O_SYNC;
#ifdef O_DSYNC
	else if (flags & LINUX_O_DSYNC)
		result |= O_DSYNC;
#endif
#ifdef O_DIRECTORY
	if (flags & LINUX_O_DIRECTORY)
		result |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
	if (flags & LINUX_O_NOFOLLOW)
		result |= O_NOFOLLOW;
#endif
	if (flags & LINUX_O_CLOEXEC)
		result |= O_CLOEXEC;
	return result;
}

static int open_flags_to_linux(int flags)
{
	int result = (flags & O_ACCMODE) == O_WRONLY ? 1 : (flags & O_ACCMODE) == O_RDWR ? 2 : 0;

	if (flags & O_APPEND)
		result |= LINUX_O_APPEND;
	if (flags & O_NONBLOCK)
		result |= LINUX_O_NONBLOCK;
	return result;
}

static clockid_t clock_to_host(long long clock)
{
	switch (clock)
	{
	case LINUX_CLOCK_REALTIME:
	case LINUX_CLOCK_REALTIME_COARSE:
		return CLOCK_REALTIME;
	case LINUX_CLOCK_PROCESS_CPUTIME_ID:
		return CLOCK_PROCESS_CPUTIME_ID;
	case LINUX_CLOCK_THREAD_CPUTIME_ID:
		return CLOCK_THREAD_CPUTIME_ID;
	default:
		return CLOCK_MONOTONIC;
	}
}

/* ---------- reading and writing */

static long guest_writev(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	long total = 0;
	int index;

	if (count < 0 || count > 1024)
		return -LINUX_EINVAL;
	/* one write at a time (positional writes too: Haiku's pwritev is not
	every system's) */
	for (index = 0; index < count; index++)
	{
		const char *base = GUEST(const char *, guest_vector[index].base);
		size_t length = guest_vector[index].length;
		ssize_t written;

		if (!length)
			continue;
		written = positional ? pwrite(fd, base, length, (off_t)(offset + total)) : write(fd, base, length);
		if (written < 0)
			return total ? total : -host_linux_errno(errno);
		total += written;
		if ((size_t)written < length)
			break;
	}
	return total;
}

static long guest_readv(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	long total = 0;
	int index;

	if (count < 0 || count > 1024)
		return -LINUX_EINVAL;
	for (index = 0; index < count; index++)
	{
		char *base = GUEST(char *, guest_vector[index].base);
		size_t length = guest_vector[index].length;
		ssize_t done;

		if (!length)
			continue;
		done = positional ? pread(fd, base, length, (off_t)(offset + total)) : read(fd, base, length);
		if (done < 0)
			return total ? total : -host_linux_errno(errno);
		total += done;
		if ((size_t)done < length)
			break;
	}
	return total;
}

/* ---------- files */

static void stat_out(uint64_t address, const struct stat *information)
{
	struct guest_stat *result = GUEST(struct guest_stat *, address);

	memset(result, 0, sizeof(*result));
	result->dev = (uint64_t)information->st_dev;
	result->ino = (uint64_t)information->st_ino;
	/* (the type bits are POSIX's on both: Haiku's extra ones are dropped) */
	result->mode = (uint32_t)(information->st_mode & (S_IFMT | 07777));
	result->nlink = (uint32_t)information->st_nlink;
	result->uid = (uint32_t)information->st_uid;
	result->gid = (uint32_t)information->st_gid;
	result->rdev = (uint64_t)information->st_rdev;
	result->size = (int64_t)information->st_size;
	result->blksize = (int32_t)information->st_blksize;
	result->blocks = (int64_t)information->st_blocks;
	result->atime_seconds = (int64_t)information->st_atim.tv_sec;
	result->atime_nanoseconds = (int64_t)information->st_atim.tv_nsec;
	result->mtime_seconds = (int64_t)information->st_mtim.tv_sec;
	result->mtime_nanoseconds = (int64_t)information->st_mtim.tv_nsec;
	result->ctime_seconds = (int64_t)information->st_ctim.tv_sec;
	result->ctime_nanoseconds = (int64_t)information->st_ctim.tv_nsec;
}

static long guest_fstatat(long long fd, uint64_t path, uint64_t buffer, int flags)
{
	struct stat information;
	const char *name = GUEST(const char *, path);
	int result;

	if ((flags & LINUX_AT_EMPTY_PATH) && (!name || !*name))
		result = fstat(file_descriptor(fd), &information);
	else
		result = fstatat(file_descriptor(fd), name, &information,
			flags & LINUX_AT_SYMLINK_NOFOLLOW ? AT_SYMLINK_NOFOLLOW : 0);
	if (result != 0)
		return -host_linux_errno(errno);
	stat_out(buffer, &information);
	return 0;
}

/* The directories the guest has open: musl reads them with getdents64 on
their descriptor, which only Linux has, so each gets a DIR of the host's
(fdopendir) the first time it is read. */
#define MAXIMUM_DIRECTORIES 64

static struct
{
	int fd;
	DIR *directory;
	/* an entry that did not fit the last buffer */
	struct dirent *pending;
} directories[MAXIMUM_DIRECTORIES];
static pthread_mutex_t directories_lock = PTHREAD_MUTEX_INITIALIZER;

static long guest_getdents64(int fd, uint64_t buffer, uint32_t size)
{
	char *out = GUEST(char *, buffer);
	long written = 0;
	int index, slot = -1;

	pthread_mutex_lock(&directories_lock);
	for (index = 0; index < MAXIMUM_DIRECTORIES; index++)
	{
		if (directories[index].directory && directories[index].fd == fd)
			slot = index;
	}
	if (slot < 0)
	{
		for (index = 0; index < MAXIMUM_DIRECTORIES && slot < 0; index++)
		{
			if (!directories[index].directory)
				slot = index;
		}
		if (slot < 0)
		{
			pthread_mutex_unlock(&directories_lock);
			return -LINUX_EINVAL;
		}
		/* (a duplicate: closedir closes the descriptor it is given, and the
		guest closes its own) */
		{
			int duplicate = dup(fd);

			directories[slot].directory = duplicate >= 0 ? fdopendir(duplicate) : NULL;
			if (!directories[slot].directory)
			{
				if (duplicate >= 0)
					close(duplicate);
				pthread_mutex_unlock(&directories_lock);
				return -host_linux_errno(errno);
			}
		}
		directories[slot].fd = fd;
		directories[slot].pending = NULL;
	}
	for (;;)
	{
		struct dirent *entry = directories[slot].pending ? directories[slot].pending :
			readdir(directories[slot].directory);
		struct guest_dirent64 *record;
		size_t name_length, record_length;

		directories[slot].pending = NULL;
		if (!entry)
			break;
		name_length = strlen(entry->d_name);
		record_length = (offsetof(struct guest_dirent64, name) + name_length + 1 + 7) & ~(size_t)7;
		if ((size_t)written + record_length > size)
		{
			directories[slot].pending = entry;
			if (!written)
			{
				pthread_mutex_unlock(&directories_lock);
				return -LINUX_EINVAL;
			}
			break;
		}
		record = (struct guest_dirent64 *)(out + written);
		record->ino = (uint64_t)entry->d_ino;
		record->offset = written + (long)record_length;
		record->record_length = (uint16_t)record_length;
#ifdef DT_DIR
		record->type = entry->d_type == DT_DIR ? LINUX_DT_DIR : entry->d_type == DT_REG ? LINUX_DT_REG :
			entry->d_type == DT_LNK ? LINUX_DT_LNK : LINUX_DT_UNKNOWN;
#else
		record->type = LINUX_DT_UNKNOWN;
#endif
		memcpy(record->name, entry->d_name, name_length + 1);
		written += (long)record_length;
	}
	pthread_mutex_unlock(&directories_lock);
	return written;
}

static long guest_close(int fd)
{
	int index;

	pthread_mutex_lock(&directories_lock);
	for (index = 0; index < MAXIMUM_DIRECTORIES; index++)
	{
		if (directories[index].directory && directories[index].fd == fd)
		{
			closedir(directories[index].directory);
			directories[index].directory = NULL;
		}
	}
	pthread_mutex_unlock(&directories_lock);
	/* (the standard streams are the host's too) */
	if (fd >= 0 && fd <= 2)
		return 0;
	return result_of(close(fd));
}

static long guest_fcntl(int fd, int command, long long argument)
{
	switch (command)
	{
	case LINUX_F_DUPFD:
		return result_of(fcntl(fd, F_DUPFD, (int)argument));
	case LINUX_F_DUPFD_CLOEXEC:
		return result_of(fcntl(fd, F_DUPFD_CLOEXEC, (int)argument));
	case LINUX_F_GETFD:
		return result_of(fcntl(fd, F_GETFD));
	case LINUX_F_SETFD:
		return result_of(fcntl(fd, F_SETFD, (int)argument & FD_CLOEXEC));
	case LINUX_F_GETFL:
	{
		int flags = fcntl(fd, F_GETFL);

		return flags < 0 ? -host_linux_errno(errno) : open_flags_to_linux(flags);
	}
	case LINUX_F_SETFL:
	{
		int flags = fcntl(fd, F_GETFL);

		if (flags < 0)
			return -host_linux_errno(errno);
		flags &= ~(O_APPEND | O_NONBLOCK);
		if (argument & LINUX_O_APPEND)
			flags |= O_APPEND;
		if (argument & LINUX_O_NONBLOCK)
			flags |= O_NONBLOCK;
		return result_of(fcntl(fd, F_SETFL, flags));
	}
	default:
		return -LINUX_EINVAL;
	}
}

static long guest_readlinkat(long long fd, uint64_t path, uint64_t buffer, uint64_t size)
{
	const char *name = GUEST(const char *, path);
	ssize_t length;

	/* (the platform layer finds its folder so: xbox_files.c) */
	if (name && !strcmp(name, "/proc/self/exe"))
	{
		char executable[1024];

		if (!host_executable_path(executable, sizeof(executable)))
			return -LINUX_ENOENT;
		length = (ssize_t)strlen(executable);
		if ((uint64_t)length > size)
			length = (ssize_t)size;
		memcpy(GUEST(char *, buffer), executable, (size_t)length);
		return length;
	}
	length = readlinkat(file_descriptor(fd), name, GUEST(char *, buffer), (size_t)size);
	return result_of((long)length);
}

static long guest_uname(uint64_t address)
{
	struct guest_utsname *result = GUEST(struct guest_utsname *, address);
	struct utsname information;

	memset(result, 0, sizeof(*result));
	if (uname(&information) == 0)
	{
		snprintf(result->nodename, sizeof(result->nodename), "%s", information.nodename);
		snprintf(result->release, sizeof(result->release), "%s", information.release);
		snprintf(result->version, sizeof(result->version), "%s", information.version);
	}
	snprintf(result->sysname, sizeof(result->sysname), "%s", host_system_name());
	snprintf(result->machine, sizeof(result->machine), "x86_64");
	return 0;
}

/* ---------- time */

static long guest_clock_nanosleep(long long clock, int flags, uint64_t request_address, uint64_t remaining_address)
{
	struct timespec request, now, remaining;

	if (!timespec_in(request_address, &request))
		return -LINUX_EFAULT;
	if (flags & LINUX_TIMER_ABSTIME)
	{
		clock_gettime(clock_to_host(clock), &now);
		request.tv_sec -= now.tv_sec;
		request.tv_nsec -= now.tv_nsec;
		if (request.tv_nsec < 0)
		{
			request.tv_sec--;
			request.tv_nsec += 1000000000L;
		}
		if (request.tv_sec < 0)
			return 0;
		remaining_address = 0;
	}
	if (nanosleep(&request, &remaining) != 0)
	{
		if (errno == EINTR)
			timespec_out(remaining_address, &remaining);
		return -host_linux_errno(errno);
	}
	return 0;
}

static long guest_ppoll(uint64_t descriptors, uint32_t count, uint64_t timeout_address)
{
	struct pollfd *guest = GUEST(struct pollfd *, descriptors);
	struct pollfd stack[64];
	struct pollfd *host = count <= 64 ? stack : malloc(sizeof(*host) * count);
	struct timespec timeout;
	int milliseconds = -1;
	uint32_t index;
	int result;

	if (!host)
		return -LINUX_EAGAIN;
	if (timespec_in(timeout_address, &timeout))
		milliseconds = (int)(timeout.tv_sec * 1000 + (timeout.tv_nsec + 999999) / 1000000);
	for (index = 0; index < count; index++)
	{
		short events = guest[index].events;

		host[index].fd = guest[index].fd;
		host[index].events = (short)((events & LINUX_POLLIN ? POLLIN : 0) | (events & LINUX_POLLPRI ? POLLPRI : 0) |
			(events & LINUX_POLLOUT ? POLLOUT : 0));
		host[index].revents = 0;
	}
	result = poll(host, (nfds_t)count, milliseconds);
	for (index = 0; index < count && result >= 0; index++)
	{
		short events = host[index].revents;

		guest[index].revents = (short)((events & POLLIN ? LINUX_POLLIN : 0) | (events & POLLPRI ? LINUX_POLLPRI : 0) |
			(events & POLLOUT ? LINUX_POLLOUT : 0) | (events & POLLERR ? LINUX_POLLERR : 0) |
			(events & POLLHUP ? LINUX_POLLHUP : 0) | (events & POLLNVAL ? LINUX_POLLNVAL : 0));
	}
	if (host != stack)
		free(host);
	return result_of(result);
}

/* ---------- futexes: wait queues of the host's own, the same on every
system (on Linux too, where the kernel's would do: so that Linux tests
these) */

#define FUTEX_BUCKETS 256

struct futex_waiter
{
	struct futex_waiter *next;
	uint64_t address;
	pthread_cond_t condition;
	int woken;
};

static struct
{
	pthread_mutex_t lock;
	struct futex_waiter *waiters;
} futex_buckets[FUTEX_BUCKETS];
static pthread_once_t futex_once = PTHREAD_ONCE_INIT;

static void futex_initialize(void)
{
	int index;

	for (index = 0; index < FUTEX_BUCKETS; index++)
		pthread_mutex_init(&futex_buckets[index].lock, NULL);
}

static unsigned int futex_bucket(uint64_t address)
{
	return (unsigned int)((address >> 2) * 2654435761u >> 24) % FUTEX_BUCKETS;
}

static long futex_wait(uint64_t address, uint32_t value, const struct timespec *timeout, int absolute_realtime)
{
	unsigned int bucket = futex_bucket(address);
	struct futex_waiter waiter, **link;
	struct timespec deadline;
	long result = 0;

	if (timeout)
	{
		if (absolute_realtime)
		{
			deadline = *timeout;
		}
		else
		{
			clock_gettime(CLOCK_REALTIME, &deadline);
			deadline.tv_sec += timeout->tv_sec;
			deadline.tv_nsec += timeout->tv_nsec;
			if (deadline.tv_nsec >= 1000000000L)
			{
				deadline.tv_sec++;
				deadline.tv_nsec -= 1000000000L;
			}
		}
	}
	pthread_mutex_lock(&futex_buckets[bucket].lock);
	if (__atomic_load_n(GUEST(volatile uint32_t *, address), __ATOMIC_SEQ_CST) != value)
	{
		pthread_mutex_unlock(&futex_buckets[bucket].lock);
		return -LINUX_EAGAIN;
	}
	waiter.address = address;
	waiter.woken = 0;
	pthread_cond_init(&waiter.condition, NULL);
	waiter.next = futex_buckets[bucket].waiters;
	futex_buckets[bucket].waiters = &waiter;
	while (!waiter.woken)
	{
		int error = timeout ? pthread_cond_timedwait(&waiter.condition, &futex_buckets[bucket].lock, &deadline) :
			pthread_cond_wait(&waiter.condition, &futex_buckets[bucket].lock);

		if (error == ETIMEDOUT && !waiter.woken)
		{
			result = -LINUX_ETIMEDOUT;
			break;
		}
	}
	for (link = &futex_buckets[bucket].waiters; *link; link = &(*link)->next)
	{
		if (*link == &waiter)
		{
			*link = waiter.next;
			break;
		}
	}
	pthread_mutex_unlock(&futex_buckets[bucket].lock);
	pthread_cond_destroy(&waiter.condition);
	return result;
}

static long futex_wake(uint64_t address, uint32_t count)
{
	unsigned int bucket = futex_bucket(address);
	struct futex_waiter *waiter;
	long woken = 0;

	pthread_mutex_lock(&futex_buckets[bucket].lock);
	for (waiter = futex_buckets[bucket].waiters; waiter && (uint32_t)woken < count; waiter = waiter->next)
	{
		if (waiter->address == address && !waiter->woken)
		{
			waiter->woken = 1;
			pthread_cond_signal(&waiter->condition);
			woken++;
		}
	}
	pthread_mutex_unlock(&futex_buckets[bucket].lock);
	return woken;
}

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout_or_count,
	uint64_t address2, uint32_t value3)
{
	int command = operation & ~(LINUX_FUTEX_PRIVATE | LINUX_FUTEX_CLOCK_REALTIME);
	struct timespec timeout;

	pthread_once(&futex_once, futex_initialize);
	switch (command)
	{
	case LINUX_FUTEX_WAIT:
		return futex_wait(address, value, timespec_in(timeout_or_count, &timeout) ? &timeout : NULL, 0);
	case LINUX_FUTEX_WAIT_BITSET:
		/* (an absolute timeout; on the monotonic clock unless the flag says
		the real one: the wait is measured on the real one regardless, with
		the monotonic deadline made relative first) */
		if (!timespec_in(timeout_or_count, &timeout))
			return futex_wait(address, value, NULL, 0);
		if (!(operation & LINUX_FUTEX_CLOCK_REALTIME))
		{
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);
			timeout.tv_sec -= now.tv_sec;
			timeout.tv_nsec -= now.tv_nsec;
			if (timeout.tv_nsec < 0)
			{
				timeout.tv_sec--;
				timeout.tv_nsec += 1000000000L;
			}
			if (timeout.tv_sec < 0)
				return -LINUX_ETIMEDOUT;
			return futex_wait(address, value, &timeout, 0);
		}
		return futex_wait(address, value, &timeout, 1);
	case LINUX_FUTEX_WAKE:
	case LINUX_FUTEX_WAKE_BITSET:
		return futex_wake(address, value);
	case LINUX_FUTEX_CMP_REQUEUE:
		if (__atomic_load_n(GUEST(volatile uint32_t *, address), __ATOMIC_SEQ_CST) != value3)
			return -LINUX_EAGAIN;
		/* fall through */
	case LINUX_FUTEX_REQUEUE:
		/* the waiters that would move to address2 are woken instead: they
		find the lock taken and wait again, as after any spurious wake */
		(void)address2;
		{
			long woken = futex_wake(address, value + (uint32_t)timeout_or_count);

			return woken < (long)value ? woken : (long)value;
		}
	default:
		return -LINUX_ENOSYS;
	}
}

/* ---------- dispatch */

long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	switch (number)
	{
	case GUEST_SYS_read:
		return result_of((long)read((int)a, GUEST(void *, b), (size_t)(uint32_t)c));
	case GUEST_SYS_write:
		return result_of((long)write((int)a, GUEST(const void *, b), (size_t)(uint32_t)c));
	case GUEST_SYS_writev:
		return guest_writev((int)a, (uint64_t)b, (int)c, 0, 0);
	case GUEST_SYS_pwritev:
		return guest_writev((int)a, (uint64_t)b, (int)c, d, 1);
	case GUEST_SYS_readv:
		return guest_readv((int)a, (uint64_t)b, (int)c, 0, 0);
	case GUEST_SYS_preadv:
		return guest_readv((int)a, (uint64_t)b, (int)c, d, 1);
	case GUEST_SYS_pread64:
		return result_of((long)pread((int)a, GUEST(void *, b), (size_t)(uint32_t)c, (off_t)d));
	case GUEST_SYS_pwrite64:
		return result_of((long)pwrite((int)a, GUEST(const void *, b), (size_t)(uint32_t)c, (off_t)d));
	case GUEST_SYS_openat:
		return result_of(openat(file_descriptor(a), GUEST(const char *, b), open_flags_to_host((int)c),
			(mode_t)d));
	case GUEST_SYS_close:
		return guest_close((int)a);
	case GUEST_SYS_lseek:
		return result_of((long)lseek((int)a, (off_t)b, (int)c));
	case GUEST_SYS_getdents64:
		return guest_getdents64((int)a, (uint64_t)b, (uint32_t)c);
	case GUEST_SYS_unlinkat:
		return result_of(unlinkat(file_descriptor(a), GUEST(const char *, b),
			c & LINUX_AT_REMOVEDIR ? AT_REMOVEDIR : 0));
	case GUEST_SYS_renameat:
	case GUEST_SYS_renameat2:
		if (number == GUEST_SYS_renameat2 && e)
			return -LINUX_EINVAL;
		return result_of(renameat(file_descriptor(a), GUEST(const char *, b), file_descriptor(c),
			GUEST(const char *, d)));
	case GUEST_SYS_mkdirat:
		return result_of(mkdirat(file_descriptor(a), GUEST(const char *, b), (mode_t)c));
	case GUEST_SYS_fchmod:
		return result_of(fchmod((int)a, (mode_t)b));
	case GUEST_SYS_fchmodat:
		return result_of(fchmodat(file_descriptor(a), GUEST(const char *, b), (mode_t)c, 0));
	case GUEST_SYS_ftruncate:
		return result_of(ftruncate((int)a, (off_t)b));
	case GUEST_SYS_fsync:
	case GUEST_SYS_fdatasync:
		return result_of(fsync((int)a));
	case GUEST_SYS_fcntl:
		return guest_fcntl((int)a, (int)b, c);
	case GUEST_SYS_getcwd:
	{
		char *buffer = GUEST(char *, a);

		if (!getcwd(buffer, (size_t)(uint32_t)b))
			return -host_linux_errno(errno);
		return (long long)strlen(buffer) + 1;
	}
	case GUEST_SYS_chdir:
		return result_of(chdir(GUEST(const char *, a)));
	case GUEST_SYS_readlinkat:
		return guest_readlinkat(a, (uint64_t)b, (uint64_t)c, (uint64_t)(uint32_t)d);
	case GUEST_SYS_faccessat:
	case GUEST_SYS_faccessat2:
	{
		int mode = (int)c;
		int host_mode = (mode & 4 ? R_OK : 0) | (mode & 2 ? W_OK : 0) | (mode & 1 ? X_OK : 0);

		return result_of(faccessat(file_descriptor(a), GUEST(const char *, b), host_mode ? host_mode : F_OK,
			number == GUEST_SYS_faccessat2 && (d & LINUX_AT_EACCESS) ? AT_EACCESS : 0));
	}
	case GUEST_SYS_dup:
		return result_of(dup((int)a));
	case GUEST_SYS_dup3:
	{
		int result = dup2((int)a, (int)b);

		if (result >= 0 && (c & LINUX_O_CLOEXEC))
			fcntl(result, F_SETFD, FD_CLOEXEC);
		return result_of(result);
	}
	case GUEST_SYS_pipe2:
	{
		int ends[2];
		int32_t *result = GUEST(int32_t *, a);

		if (pipe(ends) != 0)
			return -host_linux_errno(errno);
		if (b & LINUX_O_CLOEXEC)
		{
			fcntl(ends[0], F_SETFD, FD_CLOEXEC);
			fcntl(ends[1], F_SETFD, FD_CLOEXEC);
		}
		result[0] = ends[0];
		result[1] = ends[1];
		return 0;
	}
	case GUEST_SYS_fstat:
		return guest_fstatat(a, 0, (uint64_t)b, LINUX_AT_EMPTY_PATH);
	case GUEST_SYS_newfstatat:
		return guest_fstatat(a, (uint64_t)b, (uint64_t)c, (int)d);
	case GUEST_SYS_utimensat:
	{
		struct timespec times[2];
		const struct guest_timespec *guest_times = GUEST(const struct guest_timespec *, c);

		if (guest_times)
		{
			timespec_in((uint64_t)c, &times[0]);
			timespec_in((uint64_t)c + sizeof(struct guest_timespec), &times[1]);
		}
		return result_of(utimensat(file_descriptor(a), GUEST(const char *, b), guest_times ? times : NULL,
			d & LINUX_AT_SYMLINK_NOFOLLOW ? AT_SYMLINK_NOFOLLOW : 0));
	}
	case GUEST_SYS_umask:
		return (long long)umask((mode_t)a);
	case GUEST_SYS_flock:
		return result_of(flock((int)a, (int)b));
	case GUEST_SYS_ioctl:
		/* (isatty asks for a terminal's size: the guest's streams are not
		terminals) */
		return -LINUX_ENOTTY;

	case GUEST_SYS_clock_gettime:
	case GUEST_SYS_clock_getres:
	{
		struct timespec value;
		int result = number == GUEST_SYS_clock_gettime ? clock_gettime(clock_to_host(a), &value) :
			clock_getres(clock_to_host(a), &value);

		if (result == 0)
			timespec_out((uint64_t)b, &value);
		return result_of(result);
	}
	case GUEST_SYS_gettimeofday:
	{
		struct timespec value;
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);

		clock_gettime(CLOCK_REALTIME, &value);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)(value.tv_nsec / 1000);
		}
		return 0;
	}
	case GUEST_SYS_nanosleep:
		return guest_clock_nanosleep(LINUX_CLOCK_MONOTONIC, 0, (uint64_t)a, (uint64_t)b);
	case GUEST_SYS_clock_nanosleep:
	{
		long result = guest_clock_nanosleep(a, (int)b, (uint64_t)c, (uint64_t)d);

		/* (it returns the error, not -1 and errno) */
		return result;
	}
	case GUEST_SYS_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case GUEST_SYS_ppoll:
		return guest_ppoll((uint64_t)a, (uint32_t)b, (uint64_t)c);
	case GUEST_SYS_sched_yield:
		return result_of(sched_yield());
	case GUEST_SYS_getrandom:
	{
		char *buffer = GUEST(char *, a);
		uint32_t size = (uint32_t)b, done = 0;

		while (done < size)
		{
			uint32_t part = size - done > 256 ? 256 : size - done;

			if (getentropy(buffer + done, part) != 0)
				return done ? (long long)done : -host_linux_errno(errno);
			done += part;
		}
		return done;
	}

	case GUEST_SYS_mmap:
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case GUEST_SYS_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case GUEST_SYS_mprotect:
		return host_guest_mprotect((uint64_t)a, (uint64_t)b, (int)c);
	case GUEST_SYS_madvise:
		/* (advice: musl's malloc giving back pages it keeps mapped) */
		return 0;
	case GUEST_SYS_mremap:
	case GUEST_SYS_brk:
		/* musl then falls back to mmap and copying */
		return -12;

	case GUEST_SYS_exit:
	case GUEST_SYS_exit_group:
		host_exit((int)a);

	case GUEST_SYS_gettid:
	case GUEST_SYS_set_tid_address:
		return host_thread_id();
	case GUEST_SYS_getpid:
		return getpid();
	case GUEST_SYS_getppid:
		return getppid();
	case GUEST_SYS_getuid:
		return getuid();
	case GUEST_SYS_geteuid:
		return geteuid();
	case GUEST_SYS_getgid:
		return getgid();
	case GUEST_SYS_getegid:
		return getegid();
	case GUEST_SYS_uname:
		return guest_uname((uint64_t)a);
	case GUEST_SYS_kill:
	case GUEST_SYS_tkill:
	case GUEST_SYS_tgkill:
	{
		/* (abort's raise: the host's abort, with its report) */
		long long signal_number = number == GUEST_SYS_tgkill ? c : b;

		if (signal_number == LINUX_SIGABRT)
		{
			host_logf(HOST_LOG_ERROR, "the guest called abort");
			abort();
		}
		return 0;
	}
	case GUEST_SYS_rt_sigprocmask:
	case GUEST_SYS_rt_sigaction:
	case GUEST_SYS_sigaltstack:
		/* the host owns signal handling */
		return 0;
	case GUEST_SYS_prlimit64:
	case GUEST_SYS_getrlimit:
	case GUEST_SYS_statx:
	case GUEST_SYS_statfs:
	case GUEST_SYS_fstatfs:
	case GUEST_SYS_clone:
	case GUEST_SYS_clone3:
	case GUEST_SYS_execve:
	case GUEST_SYS_rt_sigtimedwait:
	case GUEST_SYS_pselect6:
	case GUEST_SYS_epoll_pwait:
	case GUEST_SYS_sysinfo:
	case GUEST_SYS_sendmsg:
	case GUEST_SYS_recvmsg:
	case GUEST_SYS_timer_create:
	case GUEST_SYS_timer_settime:
	case GUEST_SYS_timer_gettime:
	case GUEST_SYS_setitimer:
	case GUEST_SYS_getitimer:
	case GUEST_SYS_times:
	case GUEST_SYS_getrusage:
	case GUEST_SYS_wait4:
	case GUEST_SYS_waitid:
	case GUEST_SYS_membarrier:
		return -LINUX_ENOSYS;

	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -LINUX_ENOSYS;
	}
}

int host_errno(void)
{
	return host_linux_errno(errno);
}
