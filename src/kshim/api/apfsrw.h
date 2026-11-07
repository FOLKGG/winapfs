/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * apfsrw: C API over the linux-apfs-rw engine running on kshim. This is what
 * the winapfs front-ends call; it hides every kernel type.
 *
 * Errors are negative errno values. Names are raw on-disk UTF-8 bytes.
 * A node is a referenced dentry+inode; release it with apfsrw_put().
 * Thread safety: the engine serializes metadata operations internally (one
 * container-wide semaphore), so calls may come from several threads.
 */
#ifndef APFSRW_H
#define APFSRW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct apfsrw_mount apfsrw_mount;
typedef struct apfsrw_node apfsrw_node;
typedef struct apfsrw_file apfsrw_file;

typedef struct apfsrw_stat {
	uint64_t ino;
	uint32_t mode;          /* st_mode incl. S_IFMT */
	uint32_t nlink;
	uint32_t uid, gid;
	uint64_t size;
	uint64_t blocks;        /* 512-byte units */
	int64_t atime_ns, mtime_ns, ctime_ns, btime_ns;
	uint32_t rdev;
	uint64_t attributes;    /* STATX_ATTR_* (compressed 0x4, immutable 0x10, append 0x20, nodump 0x40) */
	uint32_t bsd_flags;      /* APFS BSD flags (UF_HIDDEN, UF_IMMUTABLE, UF_COMPRESSED, ...) */
} apfsrw_stat;

typedef struct apfsrw_statfs {
	uint64_t block_size;
	uint64_t blocks;
	uint64_t blocks_free;
	uint64_t blocks_avail;
	uint64_t files;
	uint64_t files_free;
} apfsrw_statfs;

/* d_type values passed to readdir callbacks */
#define APFSRW_DT_FIFO	1
#define APFSRW_DT_CHR	2
#define APFSRW_DT_DIR	4
#define APFSRW_DT_BLK	6
#define APFSRW_DT_REG	8
#define APFSRW_DT_LNK	10
#define APFSRW_DT_SOCK	12

typedef void (*apfsrw_log_fn)(int level, const char *msg);
void apfsrw_set_log(apfsrw_log_fn fn);

/*
 * Mount volume vol_index (index in the container's volume array) of the APFS
 * container stored at [offset, offset+size) of path (image file or raw disk;
 * size 0 = to the end). readwrite needs the device to be writable.
 * snapshot: snapshot label to mount read-only, or NULL.
 */
int apfsrw_mount_volume(const wchar_t *path, uint64_t offset, uint64_t size, unsigned vol_index, bool readwrite,
			const char *snapshot, apfsrw_mount **out);
/* Commits everything, writes the unmount time, and releases the device. */
int apfsrw_unmount(apfsrw_mount *m);
int apfsrw_sync(apfsrw_mount *m);
int apfsrw_get_statfs(apfsrw_mount *m, apfsrw_statfs *st);
bool apfsrw_is_readwrite(apfsrw_mount *m);

int apfsrw_root(apfsrw_mount *m, apfsrw_node **out);
/* -ENOENT if the name does not exist */
int apfsrw_lookup(apfsrw_node *dir, const char *name, size_t len, apfsrw_node **out);
/* Like apfsrw_lookup, but also returns the name as stored on disk (its case and
 * normalization may differ from name on insensitive volumes). */
int apfsrw_lookup_stored(apfsrw_node *dir, const char *name, size_t len, apfsrw_node **out, char *stored, size_t stored_size);
apfsrw_node *apfsrw_get(apfsrw_node *n);
void apfsrw_put(apfsrw_node *n);
int apfsrw_getattr(apfsrw_node *n, apfsrw_stat *st);
const char *apfsrw_name(apfsrw_node *n, size_t *len);   /* stored name as found by lookup */

/* Calls cb for each entry except "." and ".."; cb returns nonzero to stop. */
typedef int (*apfsrw_dir_cb)(void *ctx, const char *name, size_t len, uint64_t ino, unsigned dtype);
int apfsrw_readdir(apfsrw_node *dir, apfsrw_dir_cb cb, void *ctx);

int apfsrw_open(apfsrw_node *n, bool write, apfsrw_file **out);
int apfsrw_read(apfsrw_file *f, uint64_t off, void *buf, size_t len, size_t *got);
int apfsrw_release(apfsrw_file *f);

/* Returns the target length, or a negative errno; buf gets a NUL-terminated prefix. */
int apfsrw_readlink(apfsrw_node *n, char *buf, size_t size);

/* xattr names are on-disk names (no "osx." prefix). Return length or -errno.
 * With size 0, return the required size. listxattr fills NUL-separated names. */
long apfsrw_listxattr(apfsrw_node *n, char *buf, size_t size);
long apfsrw_getxattr(apfsrw_node *n, const char *name, void *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
