/* winapfs: <sys/stat.h> for apfsck: only fstat() of the device is used. */
#pragma once
#define S_IFMT		0170000
#define S_IFREG		0100000
#define S_IFBLK		0060000
#define S_IFDIR		0040000
#define S_IFCHR		0020000
#define S_IFLNK		0120000
#define S_IFSOCK	0140000
#define S_IFIFO		0010000
struct kfsck_stat {
	unsigned int st_mode;
	long long st_size;
};
#define stat kfsck_stat
int kfsck_fstat(int fd, struct kfsck_stat *st);
#define fstat kfsck_fstat
