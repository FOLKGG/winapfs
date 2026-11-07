/* winapfs: <sys/ioctl.h> for apfsck: BLKGETSIZE64 only. */
#pragma once
#define BLKGETSIZE64	0x80081272
int kfsck_ioctl(int fd, unsigned long req, void *arg);
#define ioctl kfsck_ioctl
