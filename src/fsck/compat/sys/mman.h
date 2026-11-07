/*
 * winapfs: <sys/mman.h> for apfsck. apfsck only makes private read mappings
 * of file ranges (and anonymous zero pages), so a mapping is a heap buffer
 * filled by a positional read. A short read fails like an unmappable range.
 */
#pragma once
#include <stddef.h>
#define PROT_READ	1
#define PROT_WRITE	2
#define MAP_SHARED	1
#define MAP_PRIVATE	2
#define MAP_ANONYMOUS	0x20
#define MAP_FAILED	((void *)-1)
void *kfsck_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int kfsck_munmap(void *addr, size_t length);
#define mmap kfsck_mmap
#define munmap kfsck_munmap
