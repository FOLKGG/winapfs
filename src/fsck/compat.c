/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * POSIX pieces apfsck needs, on Win32. The device is opened read-only.
 * Reads go through a page-aligned bounce buffer at aligned offsets, so raw
 * disks (\\.\PhysicalDriveN) work as well as image files. The front-end may
 * restrict the device to one partition (kfsck_set_partition); apfsck then
 * sees a bare container starting at offset 0.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winioctl.h>

#include <zlib.h>

#define MAX_FDS		8
#define ALIGN		4096u
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

static HANDLE g_fds[MAX_FDS];
static unsigned long long g_base;
static unsigned long long g_size;   /* 0: whole device */

void kfsck_set_partition(unsigned long long offset, unsigned long long size)
{
	g_base = offset;
	g_size = size;
}

static HANDLE handle(int fd)
{
	return (fd >= 0 && fd < MAX_FDS) ? g_fds[fd] : NULL;
}

int kfsck_open(const char *path, int flags, ...)
{
	wchar_t wpath[1024];
	HANDLE h;
	int fd;

	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : ENOENT;
		return -1;
	}
	for (fd = 3; fd < MAX_FDS; fd++) {
		if (!g_fds[fd]) {
			g_fds[fd] = h;
			return fd;
		}
	}
	CloseHandle(h);
	errno = EMFILE;
	return -1;
}

int kfsck_close(int fd)
{
	HANDLE h = handle(fd);
	if (!h)
		return -1;
	CloseHandle(h);
	g_fds[fd] = NULL;
	return 0;
}

static unsigned long long device_size(HANDLE h)
{
	GET_LENGTH_INFORMATION li;
	LARGE_INTEGER sz;
	DWORD ret;
	if (g_size)
		return g_size;
	if (GetFileSizeEx(h, &sz))
		return (unsigned long long)sz.QuadPart - g_base;
	if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof(li), &ret, NULL))
		return (unsigned long long)li.Length.QuadPart - g_base;
	return 0;
}

/* Positional read within the partition; returns bytes read or -1. */
ssize_t kfsck_pread(int fd, void *buf, size_t count, off_t offset)
{
	HANDLE h = handle(fd);
	unsigned long long size, start, end, a_start, a_end;
	unsigned char *bounce;
	size_t done = 0;

	if (!h || offset < 0) {
		errno = EBADF;
		return -1;
	}
	size = device_size(h);
	if ((unsigned long long)offset >= size)
		return 0;
	if (count > size - (unsigned long long)offset)
		count = (size_t)(size - (unsigned long long)offset);

	start = g_base + (unsigned long long)offset;
	end = start + count;
	a_start = start & ~(unsigned long long)(ALIGN - 1);
	a_end = (end + ALIGN - 1) & ~(unsigned long long)(ALIGN - 1);
	bounce = _aligned_malloc(1 << 20, ALIGN);
	if (!bounce) {
		errno = ENOMEM;
		return -1;
	}
	while (a_start < a_end) {
		DWORD chunk = (DWORD)MIN(a_end - a_start, (unsigned long long)(1 << 20)), got = 0;
		OVERLAPPED ov = { 0 };
		unsigned long long from, to;
		ov.Offset = (DWORD)a_start;
		ov.OffsetHigh = (DWORD)(a_start >> 32);
		if (!ReadFile(h, bounce, chunk, &got, &ov) && GetLastError() != ERROR_HANDLE_EOF)
			break;
		if (got == 0)
			break;
		from = MAX(a_start, start);
		to = MIN(a_start + got, end);
		if (to > from) {
			memcpy((unsigned char *)buf + (from - start), bounce + (from - a_start), (size_t)(to - from));
			done = (size_t)(to - start);
		}
		if (got < chunk)
			break;
		a_start += got;
	}
	_aligned_free(bounce);
	return (ssize_t)done;
}

void *kfsck_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	void *p = _aligned_malloc(length ? length : 1, ALIGN);
	if (!p) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	if (flags & MAP_ANONYMOUS) {
		memset(p, 0, length);
		return p;
	}
	if (kfsck_pread(fd, p, length, offset) != (ssize_t)length) {
		_aligned_free(p);
		errno = EIO;
		return MAP_FAILED;
	}
	return p;
}

int kfsck_munmap(void *addr, size_t length)
{
	_aligned_free(addr);
	return 0;
}

int kfsck_fstat(int fd, struct kfsck_stat *st)
{
	HANDLE h = handle(fd);
	if (!h) {
		errno = EBADF;
		return -1;
	}
	st->st_mode = S_IFREG;   /* report the partition size directly */
	st->st_size = (long long)device_size(h);
	return 0;
}

int kfsck_ioctl(int fd, unsigned long req, void *arg)
{
	HANDLE h = handle(fd);
	if (!h || req != BLKGETSIZE64) {
		errno = EINVAL;
		return -1;
	}
	*(unsigned long long *)arg = device_size(h);
	return 0;
}

/* ------------------------------------------------------------ getopt (POSIX) */

char *optarg;
int optind = 1, opterr = 1, optopt;

int getopt(int argc, char *const argv[], const char *optstring)
{
	static int pos = 1;
	const char *spec;
	char c;

	if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || !argv[optind][1])
		return -1;
	if (!strcmp(argv[optind], "--")) {
		optind++;
		return -1;
	}
	c = argv[optind][pos];
	spec = strchr(optstring, c);
	if (!spec || c == ':') {
		optopt = c;
		if (opterr)
			fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
		if (!argv[optind][++pos]) {
			optind++;
			pos = 1;
		}
		return '?';
	}
	if (spec[1] == ':') {
		if (argv[optind][pos + 1]) {
			optarg = &argv[optind][pos + 1];
		} else if (optind + 1 < argc) {
			optarg = argv[++optind];
		} else {
			optopt = c;
			optind++;
			pos = 1;
			return '?';
		}
		optind++;
		pos = 1;
		return c;
	}
	if (!argv[optind][++pos]) {
		optind++;
		pos = 1;
	}
	return c;
}

/* ------------------------------------------------------------ zlib */

int zlib_inflate_blob(void *dst, unsigned dst_sz, const void *src, unsigned src_sz)
{
	z_stream s;
	int ret;

	memset(&s, 0, sizeof(s));
	if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
		return -ENOMEM;
	s.next_in = (Bytef *)src;
	s.avail_in = src_sz;
	s.next_out = dst;
	s.avail_out = dst_sz;
	ret = inflate(&s, Z_FINISH);
	inflateEnd(&s);
	if (ret != Z_STREAM_END && !(ret == Z_BUF_ERROR && s.avail_out == 0))
		return -EINVAL;
	return (int)(dst_sz - s.avail_out);
}
