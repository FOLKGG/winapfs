/*
 * winapfs: <unistd.h> for apfsck. File descriptors come from kfsck_open();
 * reads are positional and add the partition offset set by the front-end.
 */
#pragma once
#include <stddef.h>
#include <fcntl.h>
ssize_t kfsck_pread(int fd, void *buf, size_t count, off_t offset);
int kfsck_open(const char *path, int flags, ...);
int kfsck_close(int fd);
#define pread kfsck_pread
#define open kfsck_open
#define close kfsck_close
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);
