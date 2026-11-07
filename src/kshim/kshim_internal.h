/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * kshim internals shared between the shim's C files (not seen by the driver).
 */
#ifndef KSHIM_INTERNAL_H
#define KSHIM_INTERNAL_H

#include <kshim.h>

#define KSHIM_MAPPING_BDEV	(1UL << 0)	/* address space of a block device */

/* page cache */
void kshim_mapping_init(struct address_space *m, struct inode *host);
void kshim_mapping_release_all(struct address_space *m);   /* drop every folio */
int kshim_mapping_writeback(struct address_space *m);      /* write dirty mapped buffers */
void kshim_mapping_drop_clean(struct address_space *m);
void kshim_reclaim(void);

/* block devices */
struct kshim_bdev;
int kshim_bdev_read(struct block_device *bdev, u64 off, void *buf, size_t len);
int kshim_bdev_write(struct block_device *bdev, u64 off, const void *buf, size_t len);
int kshim_bdev_flush(struct block_device *bdev);

/* work queues */
void kshim_work_shutdown(void);

#endif
