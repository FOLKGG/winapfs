/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * kshim: VFS, block device, buffer cache and page cache (Linux 6.12 flavour).
 * Included from kshim.h.
 */
#ifndef KSHIM_FS_H
#define KSHIM_FS_H

struct super_block;
struct inode;
struct dentry;
struct file;
struct folio;
struct page;
struct address_space;
struct buffer_head;
struct kstat;
struct iattr;
struct path;
struct fileattr;
struct seq_file;
struct vm_area_struct;
struct vm_fault;
struct kiocb;
struct iov_iter;
struct pipe_inode_info;
struct fiemap_extent_info;
struct kstatfs;
struct poll_table_struct;
struct writeback_control;
struct readahead_control;
struct delayed_call;
struct mnt_idmap { int unused; };
extern struct mnt_idmap nop_mnt_idmap;
struct vfsmount { struct dentry *mnt_root; struct super_block *mnt_sb; };
struct cred { kuid_t fsuid; kgid_t fsgid; };

/* ------------------------------------------------------------ work queues */
struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);
struct work_struct {
	work_func_t func;
	volatile long pending;
	void *kshim;              /* private */
};
struct delayed_work {
	struct work_struct work;
	u64 due_ms;
};
#define INIT_WORK(w, f)		kshim_init_work((w), (f))
#define INIT_DELAYED_WORK(w, f)	kshim_init_work(&(w)->work, (f))
void kshim_init_work(struct work_struct *w, work_func_t f);
bool schedule_work(struct work_struct *w);
bool queue_work(void *wq, struct work_struct *w);
bool schedule_delayed_work(struct delayed_work *w, unsigned long delay);
bool mod_delayed_work(void *wq, struct delayed_work *w, unsigned long delay);
bool queue_delayed_work(void *wq, struct delayed_work *w, unsigned long delay);
bool cancel_delayed_work(struct delayed_work *w);
bool cancel_delayed_work_sync(struct delayed_work *w);
bool cancel_work_sync(struct work_struct *w);
bool flush_work(struct work_struct *w);
bool flush_delayed_work(struct delayed_work *w);
static inline struct delayed_work *to_delayed_work(struct work_struct *w) { return container_of(w, struct delayed_work, work); }
extern void *system_wq;
struct work_struct *kshim_current_work(void);
#define current_work()	kshim_current_work()

/* ------------------------------------------------------------ names */
struct qstr {
	union {
		struct { u32 hash; u32 len; };
		u64 hash_len;
	};
	const unsigned char *name;
};
#define QSTR_INIT(n, l)	{ { { .hash = 0, .len = (l) } }, .name = (const unsigned char *)(n) }
static inline unsigned long init_name_hash(const void *salt) { (void)salt; return 0; }
static inline unsigned long partial_name_hash(unsigned long c, unsigned long prevhash) { return (prevhash + (c << 4) + (c >> 4)) * 11; }
static inline unsigned int end_name_hash(unsigned long hash) { return (unsigned int)hash; }

/* ------------------------------------------------------------ block device */
struct block_device {
	void *kshim_dev;          /* winapfs device (C++ Device*), see kshim_bdev.cpp */
	struct gendisk *bd_disk;
	struct address_space *bd_mapping;
	dev_t bd_dev;
	struct super_block *bd_super;
	unsigned int bd_block_size;
	u64 bd_size;              /* bytes */
	bool bd_read_only;
	char bd_name[64];
};
struct backing_dev_info { unsigned long ra_pages; unsigned long io_pages; unsigned int capabilities; };
#define BDI_CAP_WRITEBACK		(1 << 0)
#define BDI_CAP_WRITEBACK_ACCT	(1 << 1)
struct gendisk { struct backing_dev_info *bdi; char disk_name[32]; };
#define BLK_OPEN_READ		1
#define BLK_OPEN_WRITE		2
#define BLK_OPEN_EXCL		4
typedef unsigned int blk_mode_t;
struct file *bdev_file_open_by_path(const char *path, blk_mode_t mode, void *holder, const void *hops);
struct block_device *file_bdev(struct file *f);
void bdev_fput(struct file *f);
int lookup_bdev(const char *path, dev_t *dev);
int set_blocksize(struct file *f, int size);
int sb_set_blocksize(struct super_block *sb, int size);
int sb_min_blocksize(struct super_block *sb, int size);
static inline unsigned int bdev_logical_block_size(struct block_device *b) { return 512; }
static inline loff_t bdev_nr_bytes(struct block_device *b) { return (loff_t)b->bd_size; }
static inline bool bdev_read_only(struct block_device *b) { return b->bd_read_only; }
int sync_blockdev(struct block_device *b);
int blkdev_issue_flush(struct block_device *b);
static inline int blksize_bits(unsigned int size) { return ilog2(size); }
int get_anon_bdev(dev_t *p);
void free_anon_bdev(dev_t d);
#define fs_holder_ops	NULL

/* ------------------------------------------------------------ buffer heads */
enum bh_state_bits {
	BH_Uptodate, BH_Dirty, BH_Lock, BH_Req, BH_Mapped, BH_New, BH_Async_Read,
	BH_Async_Write, BH_Delay, BH_Boundary, BH_Write_EIO, BH_Unwritten, BH_Quiet,
	BH_Meta, BH_Prio, BH_Defer_Completion,
	BH_PrivateStart
};
typedef void (bh_end_io_t)(struct buffer_head *bh, int uptodate);
struct buffer_head {
	unsigned long b_state;
	struct buffer_head *b_this_page;  /* circular list of a folio's buffers */
	union {
		struct page *b_page;
		struct folio *b_folio;
	};
	sector_t b_blocknr;               /* in units of b_size */
	size_t b_size;
	char *b_data;
	struct block_device *b_bdev;
	bh_end_io_t *b_end_io;
	void *b_private;
	struct list_head b_assoc_buffers;
	struct address_space *b_assoc_map;
	atomic_t b_count;
	/* kshim private */
	struct list_head kshim_lru;
	struct buffer_head *kshim_hnext;
	bool kshim_cached;                /* owned by the block cache (not a folio) */
	struct mutex kshim_lock;
};

#define BUFFER_FNS(bit, name)								\
static inline void set_buffer_##name(struct buffer_head *bh) { set_bit(BH_##bit, &bh->b_state); }	\
static inline void clear_buffer_##name(struct buffer_head *bh) { clear_bit(BH_##bit, &bh->b_state); }	\
static inline int buffer_##name(const struct buffer_head *bh) { return test_bit(BH_##bit, &bh->b_state); }
#define TAS_BUFFER_FNS(bit, name)							\
static inline int test_set_buffer_##name(struct buffer_head *bh) { return test_and_set_bit(BH_##bit, &bh->b_state); } \
static inline int test_clear_buffer_##name(struct buffer_head *bh) { return test_and_clear_bit(BH_##bit, &bh->b_state); }
BUFFER_FNS(Uptodate, uptodate)
BUFFER_FNS(Dirty, dirty)
TAS_BUFFER_FNS(Dirty, dirty)
BUFFER_FNS(Lock, locked)
BUFFER_FNS(Req, req)
BUFFER_FNS(Mapped, mapped)
BUFFER_FNS(New, new)
BUFFER_FNS(Delay, delay)
BUFFER_FNS(Boundary, boundary)
BUFFER_FNS(Write_EIO, write_io_error)
BUFFER_FNS(Unwritten, unwritten)
BUFFER_FNS(Meta, meta)
BUFFER_FNS(Prio, prio)
BUFFER_FNS(Async_Read, async_read)
BUFFER_FNS(Async_Write, async_write)

struct buffer_head *__bread_gfp(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp);
struct buffer_head *__getblk_gfp(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp);
struct buffer_head *sb_bread(struct super_block *sb, sector_t block);
struct buffer_head *sb_getblk(struct super_block *sb, sector_t block);
struct buffer_head *__bread(struct block_device *bdev, sector_t block, unsigned size);
struct buffer_head *__getblk(struct block_device *bdev, sector_t block, unsigned size);
struct buffer_head *bdev_getblk(struct block_device *bdev, sector_t block, unsigned size, gfp_t gfp);
void brelse(struct buffer_head *bh);
void __brelse(struct buffer_head *bh);
static inline void get_bh(struct buffer_head *bh) { atomic_inc(&bh->b_count); }
static inline void put_bh(struct buffer_head *bh) { atomic_dec(&bh->b_count); }
void mark_buffer_dirty(struct buffer_head *bh);
void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode);
int sync_dirty_buffer(struct buffer_head *bh);
int __sync_dirty_buffer(struct buffer_head *bh, unsigned int op_flags);
void write_dirty_buffer(struct buffer_head *bh, unsigned int op_flags);
void lock_buffer(struct buffer_head *bh);
int trylock_buffer(struct buffer_head *bh);
void unlock_buffer(struct buffer_head *bh);
void wait_on_buffer(struct buffer_head *bh);   /* writes the calling thread's plug first */
void kshim_unplug(void);
void ll_rw_block(unsigned int op, int nr, struct buffer_head *bhs[]);
int bh_read(struct buffer_head *bh, unsigned int op_flags);
void bh_read_nowait(struct buffer_head *bh, unsigned int op_flags);
int bh_uptodate_or_lock(struct buffer_head *bh);
void end_buffer_read_sync(struct buffer_head *bh, int uptodate);
void end_buffer_write_sync(struct buffer_head *bh, int uptodate);
void submit_bh(unsigned int opf, struct buffer_head *bh);
void map_bh(struct buffer_head *bh, struct super_block *sb, sector_t block);
void clean_bdev_aliases(struct block_device *bdev, sector_t block, sector_t len);
static inline void clean_bdev_bh_alias(struct buffer_head *bh) { clean_bdev_aliases(bh->b_bdev, bh->b_blocknr, 1); }
void invalidate_bdev(struct block_device *bdev);
bool try_to_free_buffers(struct folio *folio);
struct buffer_head *create_empty_buffers(struct folio *folio, unsigned long blocksize, unsigned long b_state);
struct buffer_head *folio_create_empty_buffers(struct folio *folio, unsigned long blocksize, unsigned long b_state);
#define REQ_OP_READ	0
#define REQ_OP_WRITE	1
#define REQ_OP_MASK	0xff
#define REQ_SYNC	(1u << 8)
#define REQ_META	(1u << 9)
#define REQ_PRIO	(1u << 10)
#define REQ_FUA		(1u << 11)
#define REQ_PREFLUSH	(1u << 12)

/* ------------------------------------------------------------ pages / folios */
#define PAGE_SHIFT	12
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#define PAGE_MASK	(~(PAGE_SIZE - 1))
typedef unsigned long pgoff_t;
enum {
	PG_locked, PG_uptodate, PG_dirty, PG_writeback, PG_private, PG_error, PG_mappedtodisk,
};
struct page {
	unsigned long flags;
	struct address_space *mapping;
	pgoff_t index;
	void *private;            /* buffer_head list */
	void *data;               /* PAGE_SIZE bytes */
	atomic_t refcount;
	struct mutex kshim_lock;
};
/* One page per folio: folio_size() == PAGE_SIZE always. */
struct folio {
	struct page page;
};
static inline struct folio *page_folio(struct page *p) { return container_of(p, struct folio, page); }
#define folio_page(f, n)	(&(f)->page)
static inline struct address_space *folio_mapping(struct folio *f) { return f->page.mapping; }
static inline pgoff_t folio_index(struct folio *f) { return f->page.index; }
static inline loff_t folio_pos(struct folio *f) { return (loff_t)f->page.index << PAGE_SHIFT; }
static inline size_t folio_size(struct folio *f) { (void)f; return PAGE_SIZE; }
static inline unsigned int folio_shift(struct folio *f) { (void)f; return PAGE_SHIFT; }
static inline long folio_nr_pages(struct folio *f) { (void)f; return 1; }
static inline struct buffer_head *folio_buffers(struct folio *f) { return (struct buffer_head *)f->page.private; }
static inline struct buffer_head *page_buffers(struct page *p) { return (struct buffer_head *)p->private; }
static inline bool page_has_buffers(struct page *p) { return p->private != NULL; }
static inline void folio_attach_private(struct folio *f, void *d) { f->page.private = d; }
static inline void *folio_detach_private(struct folio *f) { void *d = f->page.private; f->page.private = NULL; return d; }
#define PAGE_FLAG_FNS(name, bit)									\
static inline bool folio_test_##name(struct folio *f) { return test_bit(bit, &f->page.flags); }	\
static inline void folio_set_##name(struct folio *f) { set_bit(bit, &f->page.flags); }		\
static inline void folio_clear_##name(struct folio *f) { clear_bit(bit, &f->page.flags); }	\
static inline int Page##name(struct page *p) { return test_bit(bit, &p->flags); }		\
static inline void SetPage##name(struct page *p) { set_bit(bit, &p->flags); }			\
static inline void ClearPage##name(struct page *p) { clear_bit(bit, &p->flags); }
PAGE_FLAG_FNS(uptodate, PG_uptodate)
PAGE_FLAG_FNS(dirty, PG_dirty)
PAGE_FLAG_FNS(locked, PG_locked)
PAGE_FLAG_FNS(writeback, PG_writeback)
PAGE_FLAG_FNS(error, PG_error)
PAGE_FLAG_FNS(mappedtodisk, PG_mappedtodisk)
/* kernel spellings */
#define PageUptodate(p)		Pageuptodate(p)
#define SetPageUptodate(p)	SetPageuptodate(p)
#define ClearPageUptodate(p)	ClearPageuptodate(p)
#define PageDirty(p)		Pagedirty(p)
#define PageLocked(p)		Pagelocked(p)
static inline void wait_for_stable_page(struct page *p) { (void)p; }
#define folio_mark_uptodate	folio_set_uptodate
void folio_lock(struct folio *f);
void folio_unlock(struct folio *f);
bool folio_trylock(struct folio *f);
void folio_get(struct folio *f);
void folio_put(struct folio *f);
static inline void folio_wait_stable(struct folio *f) { (void)f; }
static inline void folio_wait_writeback(struct folio *f) { (void)f; }
bool folio_mark_dirty(struct folio *f);
bool folio_clear_dirty_for_io(struct folio *f);
int folio_mkclean(struct folio *f);
static inline void *kmap_local_folio(struct folio *f, size_t off) { return (char *)f->page.data + off; }
static inline void *kmap_local_page(struct page *p) { return p->data; }
static inline void kunmap_local(const void *a) { (void)a; }
static inline void *kmap(struct page *p) { return p->data; }
static inline void kunmap(struct page *p) { (void)p; }
static inline void *kmap_atomic(struct page *p) { return p->data; }
static inline void kunmap_atomic(void *a) { (void)a; }
static inline void *page_address(struct page *p) { return p->data; }
static inline void *folio_address(struct folio *f) { return f->page.data; }
static inline void flush_dcache_page(struct page *p) { (void)p; }
static inline void flush_dcache_folio(struct folio *f) { (void)f; }
static inline void lock_page(struct page *p) { folio_lock(page_folio(p)); }
static inline void unlock_page(struct page *p) { folio_unlock(page_folio(p)); }
static inline void get_page(struct page *p) { folio_get(page_folio(p)); }
static inline void put_page(struct page *p) { folio_put(page_folio(p)); }
static inline bool set_page_dirty(struct page *p) { return folio_mark_dirty(page_folio(p)); }
static inline int page_mkclean(struct page *p) { return folio_mkclean(page_folio(p)); }
void folio_zero_segment(struct folio *f, size_t start, size_t end);
void folio_zero_range(struct folio *f, size_t start, size_t len);
static inline void zero_user_segment(struct page *p, unsigned int start, unsigned int end) { folio_zero_segment(page_folio(p), start, end); }
static inline void zero_user(struct page *p, unsigned int start, unsigned int len) { folio_zero_segment(page_folio(p), start, start + len); }
static inline void memcpy_to_page(struct page *p, size_t off, const char *from, size_t len) { memcpy((char *)p->data + off, from, len); }
static inline void memcpy_from_page(char *to, struct page *p, size_t off, size_t len) { memcpy(to, (char *)p->data + off, len); }
#define offset_in_page(p)	((unsigned long)(uintptr_t)(p) & ~PAGE_MASK)
#define offset_in_folio(f, p)	((size_t)(p) & (PAGE_SIZE - 1))

/* ------------------------------------------------------------ address space */
struct address_space_operations;
struct address_space {
	struct inode *host;
	const struct address_space_operations *a_ops;
	unsigned long nrpages;
	gfp_t gfp_mask;
	void *kshim_pages;        /* index -> folio map, see kshim_pagecache.c */
	struct mutex kshim_lock;
	struct list_head i_private_list;
	unsigned long flags;
};
struct folio *filemap_get_folio(struct address_space *m, pgoff_t index);
struct folio *__filemap_get_folio(struct address_space *m, pgoff_t index, unsigned int fgp, gfp_t gfp);
struct folio *read_mapping_folio(struct address_space *m, pgoff_t index, struct file *f);
struct page *read_mapping_page(struct address_space *m, pgoff_t index, struct file *f);
struct page *pagecache_get_page(struct address_space *m, pgoff_t index, int fgp, gfp_t gfp);
struct page *grab_cache_page_write_begin(struct address_space *m, pgoff_t index);
#define FGP_ACCESSED	0x1
#define FGP_LOCK	0x2
#define FGP_CREAT	0x4
#define FGP_WRITE	0x8
#define FGP_NOFS	0x10
#define FGP_NOWAIT	0x20
#define FGP_STABLE	0x40
#define FGP_WRITEBEGIN	(FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE)
static inline unsigned int fgf_set_order(size_t size) { (void)size; return 0; }
static inline gfp_t mapping_gfp_mask(struct address_space *m) { return m->gfp_mask; }
static inline void mapping_set_gfp_mask(struct address_space *m, gfp_t g) { m->gfp_mask = g; }
static inline void mapping_set_large_folios(struct address_space *m) { (void)m; }
int filemap_write_and_wait(struct address_space *m);
int filemap_write_and_wait_range(struct address_space *m, loff_t start, loff_t end);
int filemap_fdatawrite(struct address_space *m);
int filemap_fdatawait(struct address_space *m);
void truncate_inode_pages(struct address_space *m, loff_t start);
void truncate_inode_pages_final(struct address_space *m);
void truncate_pagecache(struct inode *inode, loff_t newsize);
void truncate_setsize(struct inode *inode, loff_t newsize);
void truncate_pagecache_range(struct inode *inode, loff_t start, loff_t end);
void invalidate_mapping_pages(struct address_space *m, pgoff_t start, pgoff_t end);
void pagecache_isize_extended(struct inode *inode, loff_t from, loff_t to);
bool block_dirty_folio(struct address_space *m, struct folio *f);
bool filemap_dirty_folio(struct address_space *m, struct folio *f);
void block_invalidate_folio(struct folio *f, size_t offset, size_t length);
bool __set_page_dirty_buffers(struct page *p);

typedef int (get_block_t)(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create);
int mpage_read_folio(struct folio *f, get_block_t get_block);
void mpage_readahead(struct readahead_control *rac, get_block_t get_block);
int block_read_full_folio(struct folio *f, get_block_t get_block);
int block_write_begin(struct address_space *m, loff_t pos, unsigned len, struct folio **foliop, get_block_t *get_block);
int __block_write_begin(struct folio *f, loff_t pos, unsigned len, get_block_t *get_block);
int block_write_end(struct file *file, struct address_space *m, loff_t pos, unsigned len, unsigned copied, struct folio *f, void *fsdata);
int generic_write_end(struct file *file, struct address_space *m, loff_t pos, unsigned len, unsigned copied, struct folio *f, void *fsdata);
int block_write_full_folio(struct folio *f, struct writeback_control *wbc, void *get_block);
int block_truncate_page(struct address_space *m, loff_t from, get_block_t *get_block);
int block_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf, get_block_t get_block);
static inline vm_fault_t block_page_mkwrite_return(int err) { return err ? 2 : 0; }
sector_t generic_block_bmap(struct address_space *m, sector_t block, get_block_t *get_block);
struct readahead_control {
	struct file *file;
	struct address_space *mapping;
	pgoff_t _index;
	unsigned int _nr_pages;
};
struct folio *readahead_folio(struct readahead_control *rac);
static inline pgoff_t readahead_index(struct readahead_control *rac) { return rac->_index; }
static inline unsigned int readahead_count(struct readahead_control *rac) { return rac->_nr_pages; }
struct writeback_control { long nr_to_write; int sync_mode; loff_t range_start, range_end; };
#define WB_SYNC_NONE	0
#define WB_SYNC_ALL	1

struct address_space_operations {
	int (*writepage)(struct page *page, struct writeback_control *wbc);
	int (*read_folio)(struct file *, struct folio *);
	int (*writepages)(struct address_space *, struct writeback_control *);
	bool (*dirty_folio)(struct address_space *, struct folio *);
	void (*readahead)(struct readahead_control *);
	int (*write_begin)(struct file *, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata);
	int (*write_end)(struct file *, struct address_space *mapping, loff_t pos, unsigned len, unsigned copied, struct folio *folio, void *fsdata);
	sector_t (*bmap)(struct address_space *, sector_t);
	void (*invalidate_folio)(struct folio *, size_t offset, size_t len);
	bool (*release_folio)(struct folio *, gfp_t);
	void (*free_folio)(struct folio *folio);
	ssize_t (*direct_IO)(struct kiocb *, struct iov_iter *iter);
	int (*migrate_folio)(struct address_space *, struct folio *dst, struct folio *src, int);
	int (*launder_folio)(struct folio *);
	bool (*is_partially_uptodate)(struct folio *, size_t from, size_t count);
	int (*error_remove_folio)(struct address_space *, struct folio *);
};
#define buffer_migrate_folio	NULL

/* ------------------------------------------------------------ inode */
#undef S_IFMT
#undef S_IFDIR
#undef S_IFCHR
#undef S_IFREG
#undef S_IFIFO
#define S_IFMT		00170000
#define S_IFSOCK	0140000
#define S_IFLNK		0120000
#define S_IFREG		0100000
#define S_IFBLK		0060000
#define S_IFDIR		0040000
#define S_IFCHR		0020000
#define S_IFIFO		0010000
#define S_ISUID		0004000
#define S_ISGID		0002000
#define S_ISVTX		0001000
#define S_IRWXU		00700
#define S_IRUSR		00400
#define S_IWUSR		00200
#define S_IXUSR		00100
#define S_IRWXG		00070
#define S_IRWXO		00007
#define S_IRUGO		00444
#define S_IWUGO		00222
#define S_IXUGO		00111
#define S_IALLUGO	07777
#define S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)	(((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)	(((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)	(((m) & S_IFMT) == S_IFSOCK)

/* i_state */
#define I_NEW		(1 << 0)
#define I_DIRTY_SYNC	(1 << 1)
#define I_DIRTY_DATASYNC (1 << 2)
#define I_DIRTY_PAGES	(1 << 3)
#define I_FREEING	(1 << 5)
#define I_CLEAR		(1 << 6)
#define I_WILL_FREE	(1 << 4)
#define I_LINKABLE	(1 << 10)
#define I_DIRTY		(I_DIRTY_SYNC | I_DIRTY_DATASYNC | I_DIRTY_PAGES)
#define I_DIRTY_ALL	(I_DIRTY | I_DIRTY_PAGES)
/* i_flags */
#define S_SYNC		(1 << 0)
#define S_NOATIME	(1 << 1)
#define S_APPEND	(1 << 2)
#define S_IMMUTABLE	(1 << 3)
#define S_NOCMTIME	(1 << 7)
#define S_DAX		0
#define S_NOSEC		(1 << 12)

struct inode_operations;
struct file_operations;
struct inode {
	umode_t i_mode;
	unsigned short i_opflags;
	kuid_t i_uid;
	kgid_t i_gid;
	unsigned int i_flags;
	const struct inode_operations *i_op;
	struct super_block *i_sb;
	struct address_space *i_mapping;
	unsigned long i_ino;
	union {
		const unsigned int i_nlink;
		unsigned int __i_nlink;
	};
	dev_t i_rdev;
	loff_t i_size;
	struct timespec64 __i_atime;
	struct timespec64 __i_mtime;
	struct timespec64 __i_ctime;
	u8 i_blkbits;
	blkcnt_t i_blocks;
	unsigned short i_bytes;
	unsigned long i_state;
	struct rw_semaphore i_rwsem;
	atomic_t i_count;
	atomic64_t i_version;
	const struct file_operations *i_fop;
	struct address_space i_data;
	void *i_private;
	struct rcu_head i_rcu;
	u32 i_generation;
	/* kshim private */
	struct inode *kshim_hnext;
	unsigned long kshim_hashval;
	struct list_head kshim_sb_list;
	struct mutex kshim_wait;  /* held while I_NEW */
	void *kshim_link;         /* cached symlink body */
};
static inline loff_t i_size_read(const struct inode *i) { return i->i_size; }
static inline void i_size_write(struct inode *i, loff_t s) { i->i_size = s; }
static inline u32 i_uid_read(const struct inode *i) { return i->i_uid.val; }
static inline u32 i_gid_read(const struct inode *i) { return i->i_gid.val; }
static inline void i_uid_write(struct inode *i, u32 uid) { i->i_uid.val = uid; }
static inline void i_gid_write(struct inode *i, u32 gid) { i->i_gid.val = gid; }
static inline kuid_t i_uid_into_vfsuid(struct mnt_idmap *m, const struct inode *i) { (void)m; return i->i_uid; }
static inline struct timespec64 inode_get_atime(const struct inode *i) { return i->__i_atime; }
static inline struct timespec64 inode_get_mtime(const struct inode *i) { return i->__i_mtime; }
static inline struct timespec64 inode_get_ctime(const struct inode *i) { return i->__i_ctime; }
static inline struct timespec64 inode_set_atime_to_ts(struct inode *i, struct timespec64 t) { i->__i_atime = t; return t; }
static inline struct timespec64 inode_set_mtime_to_ts(struct inode *i, struct timespec64 t) { i->__i_mtime = t; return t; }
static inline struct timespec64 inode_set_ctime_to_ts(struct inode *i, struct timespec64 t) { i->__i_ctime = t; return t; }
struct timespec64 inode_set_ctime_current(struct inode *i);
static inline time64_t inode_get_atime_sec(const struct inode *i) { return i->__i_atime.tv_sec; }
static inline time64_t inode_get_mtime_sec(const struct inode *i) { return i->__i_mtime.tv_sec; }
static inline time64_t inode_get_ctime_sec(const struct inode *i) { return i->__i_ctime.tv_sec; }
static inline void inode_set_iversion(struct inode *i, u64 v) { i->i_version.counter = (long long)v; }
static inline u64 inode_peek_iversion(const struct inode *i) { return (u64)i->i_version.counter; }
static inline void inode_inc_iversion(struct inode *i) { i->i_version.counter++; }
static inline void inode_lock(struct inode *i) { down_write(&i->i_rwsem); }
static inline void inode_unlock(struct inode *i) { up_write(&i->i_rwsem); }
static inline void inode_lock_shared(struct inode *i) { down_read(&i->i_rwsem); }
static inline void inode_unlock_shared(struct inode *i) { up_read(&i->i_rwsem); }
void set_nlink(struct inode *i, unsigned int n);
void inc_nlink(struct inode *i);
void drop_nlink(struct inode *i);
void clear_nlink(struct inode *i);
void inode_add_bytes(struct inode *i, loff_t bytes);
void inode_sub_bytes(struct inode *i, loff_t bytes);
void inode_set_bytes(struct inode *i, loff_t bytes);
loff_t inode_get_bytes(struct inode *i);
void inode_init_once(struct inode *i);
void inode_init_owner(struct mnt_idmap *idmap, struct inode *i, const struct inode *dir, umode_t mode);
bool inode_owner_or_capable(struct mnt_idmap *idmap, const struct inode *i);
void inode_set_flags(struct inode *i, unsigned int flags, unsigned int mask);
void init_special_inode(struct inode *i, umode_t mode, dev_t rdev);
void mark_inode_dirty(struct inode *i);
void mark_inode_dirty_sync(struct inode *i);
void clear_inode(struct inode *i);
struct inode *new_inode(struct super_block *sb);
struct inode *iget_locked(struct super_block *sb, unsigned long ino);
struct inode *iget5_locked(struct super_block *sb, unsigned long hashval, int (*test)(struct inode *, void *), int (*set)(struct inode *, void *), void *data);
struct inode *ilookup(struct super_block *sb, unsigned long ino);
int insert_inode_locked(struct inode *i);
int insert_inode_locked4(struct inode *i, unsigned long hashval, int (*test)(struct inode *, void *), void *data);
void unlock_new_inode(struct inode *i);
void discard_new_inode(struct inode *i);
void iget_failed(struct inode *i);
void iput(struct inode *i);
void ihold(struct inode *i);
void make_bad_inode(struct inode *i);
bool is_bad_inode(struct inode *i);
void inode_nohighmem(struct inode *i);
void *alloc_inode_sb(struct super_block *sb, struct kmem_cache *cache, gfp_t gfp);
struct timespec64 simple_inode_init_ts(struct inode *i);
int generic_update_time(struct inode *i, int flags);
void file_update_time(struct file *f);
void file_accessed(struct file *f);
void inode_dio_wait(struct inode *i);
#define S_ATIME		1
#define S_MTIME		2
#define S_CTIME		4
#define S_VERSION	8
#define IS_RDONLY(i)	sb_rdonly((i)->i_sb)
#define IS_IMMUTABLE(i)	((i)->i_flags & S_IMMUTABLE)
#define IS_APPEND(i)	((i)->i_flags & S_APPEND)
#define IS_NOCMTIME(i)	((i)->i_flags & S_NOCMTIME)
#define HAS_UNMAPPED_ID(m, i)	0

/* ------------------------------------------------------------ dentry */
struct dentry_operations;
struct dentry {
	unsigned int d_flags;
	struct dentry *d_parent;
	struct qstr d_name;
	struct inode *d_inode;
	unsigned char d_iname[32];
	const struct dentry_operations *d_op;
	struct super_block *d_sb;
	void *d_fsdata;
	/* kshim private */
	atomic_t kshim_count;
	char *kshim_name;
};
static inline struct inode *d_inode(const struct dentry *d) { return d->d_inode; }
static inline struct inode *d_backing_inode(const struct dentry *d) { return d->d_inode; }
static inline bool d_really_is_positive(const struct dentry *d) { return d->d_inode != NULL; }
static inline bool d_really_is_negative(const struct dentry *d) { return d->d_inode == NULL; }
static inline bool d_is_dir(const struct dentry *d) { return d->d_inode && S_ISDIR(d->d_inode->i_mode); }
void d_instantiate(struct dentry *d, struct inode *i);
void d_instantiate_new(struct dentry *d, struct inode *i);
struct dentry *d_splice_alias(struct inode *i, struct dentry *d);
struct dentry *d_obtain_alias(struct inode *i);
struct dentry *d_make_root(struct inode *root);
struct dentry *dget(struct dentry *d);
struct dentry *dget_parent(struct dentry *d);
void dput(struct dentry *d);
void d_drop(struct dentry *d);
void d_add(struct dentry *d, struct inode *i);
void set_default_d_op(struct super_block *sb, const struct dentry_operations *ops);
void d_set_d_op(struct dentry *d, const struct dentry_operations *ops);
const char *kshim_dentry_name(const struct dentry *d);
#define LOOKUP_RCU	0x0040
#define LOOKUP_CREATE	0x0200
#define LOOKUP_RENAME_TARGET	0x0800
#define RENAME_NOREPLACE	(1 << 0)
#define RENAME_EXCHANGE		(1 << 1)
#define RENAME_WHITEOUT		(1 << 2)
struct dentry_operations {
	int (*d_revalidate)(struct dentry *, unsigned int);
	int (*d_weak_revalidate)(struct dentry *, unsigned int);
	int (*d_hash)(const struct dentry *, struct qstr *);
	int (*d_compare)(const struct dentry *, unsigned int, const char *, const struct qstr *);
	int (*d_delete)(const struct dentry *);
	void (*d_release)(struct dentry *);
};

/* ------------------------------------------------------------ files */
#define FMODE_READ	0x1
#define FMODE_WRITE	0x2
#define FMODE_LSEEK	0x4
#define FMODE_EXCL	0x80
#define O_ACCMODE_	3
#define O_DIRECT_	040000
#define O_LARGEFILE	0100000
#define MAX_NON_LFS	((1UL << 31) - 1)
struct path {
	struct vfsmount *mnt;
	struct dentry *dentry;
};
struct file {
	struct path f_path;
	struct inode *f_inode;
	const struct file_operations *f_op;
	unsigned int f_flags;
	fmode_t f_mode;
	loff_t f_pos;
	struct address_space *f_mapping;
	void *private_data;
	const struct cred *f_cred;
	/* kshim private: block device files */
	struct block_device *kshim_bdev;
};
static inline struct inode *file_inode(const struct file *f) { return f->f_inode; }
static inline struct dentry *file_dentry(const struct file *f) { return f->f_path.dentry; }
int generic_file_open(struct inode *i, struct file *f);
void fput(struct file *f);
int mnt_want_write_file(struct file *f);
void mnt_drop_write_file(struct file *f);
static inline bool capable(int cap) { (void)cap; return true; }
#define CAP_SYS_ADMIN		21
#define CAP_LINUX_IMMUTABLE	9
#define CAP_FOWNER		3
const struct cred *current_cred(void);
#define current_fsuid()	(current_cred()->fsuid)
#define current_fsgid()	(current_cred()->fsgid)
bool fatal_signal_pending(void *task);
extern void *current;

struct dir_context;
typedef bool (*filldir_t)(struct dir_context *, const char *, int, loff_t, u64, unsigned);
struct dir_context {
	filldir_t actor;
	loff_t pos;
};
#define DT_UNKNOWN	0
#define DT_FIFO		1
#define DT_CHR		2
#define DT_DIR		4
#define DT_BLK		6
#define DT_REG		8
#define DT_LNK		10
#define DT_SOCK		12
#define DT_WHT		14
static inline bool dir_emit(struct dir_context *ctx, const char *name, int namelen, u64 ino, unsigned type)
{
	return ctx->actor(ctx, name, namelen, ctx->pos, ino, type);
}
bool dir_emit_dots(struct file *file, struct dir_context *ctx);
static inline unsigned char fs_umode_to_dtype(umode_t mode) { return (unsigned char)((mode & S_IFMT) >> 12); }

struct kiocb { struct file *ki_filp; loff_t ki_pos; int ki_flags; };
struct iov_iter { int kshim_unused; };
loff_t generic_file_llseek(struct file *f, loff_t off, int whence);
ssize_t generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter);
ssize_t generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from);
ssize_t generic_read_dir(struct file *f, char __user *buf, size_t n, loff_t *pos);
ssize_t filemap_splice_read(struct file *in, loff_t *ppos, struct pipe_inode_info *pipe, size_t len, unsigned int flags);
ssize_t iter_file_splice_write(struct pipe_inode_info *pipe, struct file *out, loff_t *ppos, size_t len, unsigned int flags);
ssize_t splice_copy_file_range(struct file *in, loff_t pos_in, struct file *out, loff_t pos_out, size_t len);
ssize_t generic_copy_file_range(struct file *in, loff_t pos_in, struct file *out, loff_t pos_out, size_t len, unsigned int flags);
int generic_file_mmap(struct file *f, struct vm_area_struct *vma);
#define REMAP_FILE_DEDUP		(1 << 0)
#define REMAP_FILE_CAN_SHORTEN		(1 << 1)
#define REMAP_FILE_ADVISORY		(REMAP_FILE_CAN_SHORTEN)
int generic_remap_file_range_prep(struct file *in, loff_t pos_in, struct file *out, loff_t pos_out, loff_t *count, unsigned int remap_flags);
#define COPY_FILE_SPLICE		(1 << 0)

struct vm_operations_struct {
	vm_fault_t (*fault)(struct vm_fault *vmf);
	vm_fault_t (*map_pages)(struct vm_fault *vmf, pgoff_t start, pgoff_t end);
	vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);
};
struct vm_area_struct { struct file *vm_file; const struct vm_operations_struct *vm_ops; unsigned long vm_flags; };
struct vm_fault { struct vm_area_struct *vma; struct page *page; };
vm_fault_t filemap_fault(struct vm_fault *vmf);
vm_fault_t filemap_map_pages(struct vm_fault *vmf, pgoff_t start, pgoff_t end);
vm_fault_t vmf_fs_error(int err);
#define VM_FAULT_LOCKED		0x0200
#define VM_FAULT_SIGBUS		0x0002
#define VM_FAULT_NOPAGE		0x0100
#define VM_SHARED		0x8
#define VM_MAYWRITE		0x20
static inline void sb_start_pagefault(struct super_block *sb) { (void)sb; }
static inline void sb_end_pagefault(struct super_block *sb) { (void)sb; }
static inline void sb_start_write(struct super_block *sb) { (void)sb; }
static inline void sb_end_write(struct super_block *sb) { (void)sb; }

struct file_operations {
	void *owner;
	loff_t (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*read_iter)(struct kiocb *, struct iov_iter *);
	ssize_t (*write_iter)(struct kiocb *, struct iov_iter *);
	int (*iterate_shared)(struct file *, struct dir_context *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	int (*mmap)(struct file *, struct vm_area_struct *);
	int (*open)(struct inode *, struct file *);
	int (*release)(struct inode *, struct file *);
	int (*fsync)(struct file *, loff_t, loff_t, int datasync);
	ssize_t (*splice_read)(struct file *, loff_t *, struct pipe_inode_info *, size_t, unsigned int);
	ssize_t (*splice_write)(struct pipe_inode_info *, struct file *, loff_t *, size_t, unsigned int);
	ssize_t (*copy_file_range)(struct file *, loff_t, struct file *, loff_t, size_t, unsigned int);
	loff_t (*remap_file_range)(struct file *file_in, loff_t pos_in, struct file *file_out, loff_t pos_out, loff_t len, unsigned int remap_flags);
	int (*fadvise)(struct file *, loff_t, loff_t, int);
};

/* ------------------------------------------------------------ attributes */
#define ATTR_MODE	(1 << 0)
#define ATTR_UID	(1 << 1)
#define ATTR_GID	(1 << 2)
#define ATTR_SIZE	(1 << 3)
#define ATTR_ATIME	(1 << 4)
#define ATTR_MTIME	(1 << 5)
#define ATTR_CTIME	(1 << 6)
#define ATTR_ATIME_SET	(1 << 7)
#define ATTR_MTIME_SET	(1 << 8)
#define ATTR_FORCE	(1 << 9)
#define ATTR_KILL_SUID	(1 << 11)
#define ATTR_KILL_SGID	(1 << 12)
#define ATTR_FILE	(1 << 13)
#define ATTR_KILL_PRIV	(1 << 14)
#define ATTR_OPEN	(1 << 15)
#define ATTR_TIMES_SET	(1 << 16)
#define ATTR_TOUCH	(1 << 17)
struct iattr {
	unsigned int ia_valid;
	umode_t ia_mode;
	kuid_t ia_uid;
	kgid_t ia_gid;
	loff_t ia_size;
	struct timespec64 ia_atime;
	struct timespec64 ia_mtime;
	struct timespec64 ia_ctime;
	struct file *ia_file;
};
int setattr_prepare(struct mnt_idmap *idmap, struct dentry *d, struct iattr *a);
void setattr_copy(struct mnt_idmap *idmap, struct inode *i, const struct iattr *a);
int inode_newsize_ok(const struct inode *i, loff_t offset);

#define STATX_TYPE	0x0001
#define STATX_MODE	0x0002
#define STATX_NLINK	0x0004
#define STATX_UID	0x0008
#define STATX_GID	0x0010
#define STATX_ATIME	0x0020
#define STATX_MTIME	0x0040
#define STATX_CTIME	0x0080
#define STATX_INO	0x0100
#define STATX_SIZE	0x0200
#define STATX_BLOCKS	0x0400
#define STATX_BASIC_STATS 0x07ff
#define STATX_BTIME	0x0800
#define STATX_ATTR_COMPRESSED	0x0004
#define STATX_ATTR_IMMUTABLE	0x0010
#define STATX_ATTR_APPEND	0x0020
#define STATX_ATTR_NODUMP	0x0040
struct kstat {
	u32 result_mask;
	umode_t mode;
	unsigned int nlink;
	u32 blksize;
	u64 attributes;
	u64 attributes_mask;
	u64 ino;
	dev_t dev;
	dev_t rdev;
	kuid_t uid;
	kgid_t gid;
	loff_t size;
	struct timespec64 atime;
	struct timespec64 mtime;
	struct timespec64 ctime;
	struct timespec64 btime;
	u64 blocks;
};
void generic_fillattr(struct mnt_idmap *idmap, u32 request_mask, struct inode *i, struct kstat *stat);

/* fileattr (FS_IOC_GETFLAGS / FS_IOC_FSGETXATTR) */
#define FS_IMMUTABLE_FL		0x00000010
#define FS_APPEND_FL		0x00000020
#define FS_NODUMP_FL		0x00000040
#define FS_NOATIME_FL		0x00000080
#define FS_COMPR_FL		0x00000004
#define FS_FL_USER_VISIBLE	0x0003DFFF
#define FS_XFLAG_IMMUTABLE	0x00000008
#define FS_XFLAG_APPEND		0x00000010
#define FS_XFLAG_NODUMP		0x00000080
#define FS_XFLAG_NOATIME	0x00000040
struct fileattr {
	u32 flags;
	u32 fsx_xflags;
	u32 fsx_extsize;
	u32 fsx_nextents;
	u32 fsx_projid;
	u32 fsx_cowextsize;
	bool flags_valid;
	bool fsx_valid;
};
void fileattr_fill_flags(struct fileattr *fa, u32 flags);
static inline bool fileattr_has_fsx(const struct fileattr *fa) { return fa->fsx_valid; }

/* ------------------------------------------------------------ inode ops */
struct delayed_call { void (*fn)(void *); void *arg; };
static inline void set_delayed_call(struct delayed_call *c, void (*fn)(void *), void *arg) { c->fn = fn; c->arg = arg; }
struct inode_operations {
	struct dentry *(*lookup)(struct inode *, struct dentry *, unsigned int);
	const char *(*get_link)(struct dentry *, struct inode *, struct delayed_call *);
	int (*permission)(struct mnt_idmap *, struct inode *, int);
	int (*readlink)(struct dentry *, char __user *, int);
	int (*create)(struct mnt_idmap *, struct inode *, struct dentry *, umode_t, bool);
	int (*link)(struct dentry *, struct inode *, struct dentry *);
	int (*unlink)(struct inode *, struct dentry *);
	int (*symlink)(struct mnt_idmap *, struct inode *, struct dentry *, const char *);
	int (*mkdir)(struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
	int (*rmdir)(struct inode *, struct dentry *);
	int (*mknod)(struct mnt_idmap *, struct inode *, struct dentry *, umode_t, dev_t);
	int (*rename)(struct mnt_idmap *, struct inode *, struct dentry *, struct inode *, struct dentry *, unsigned int);
	int (*setattr)(struct mnt_idmap *, struct dentry *, struct iattr *);
	int (*getattr)(struct mnt_idmap *, const struct path *, struct kstat *, u32, unsigned int);
	ssize_t (*listxattr)(struct dentry *, char *, size_t);
	int (*fiemap)(struct inode *, struct fiemap_extent_info *, u64 start, u64 len);
	int (*update_time)(struct inode *, int);
	int (*fileattr_set)(struct mnt_idmap *idmap, struct dentry *dentry, struct fileattr *fa);
	int (*fileattr_get)(struct dentry *dentry, struct fileattr *fa);
};
int generic_readlink(struct dentry *d, char __user *buf, int len);
const char *page_get_link(struct dentry *d, struct inode *i, struct delayed_call *c);
void kfree_link(void *p);

/* fiemap */
#define FIEMAP_EXTENT_LAST	0x00000001
#define FIEMAP_EXTENT_UNKNOWN	0x00000002
#define FIEMAP_EXTENT_SHARED	0x00002000
#define FIEMAP_EXTENT_MERGED	0x00001000
#define FIEMAP_EXTENT_DATA_ENCRYPTED 0x00000080
#define FIEMAP_FLAG_SYNC	0x00000001
struct fiemap_extent_info { unsigned int fi_flags; unsigned int fi_extents_mapped; unsigned int fi_extents_max; void *fi_extents_start; };
int fiemap_prep(struct inode *i, struct fiemap_extent_info *fi, u64 start, u64 *len, u32 supported_flags);
int fiemap_fill_next_extent(struct fiemap_extent_info *fi, u64 logical, u64 phys, u64 len, u32 flags);

/* ------------------------------------------------------------ xattr */
#define XATTR_CREATE	0x1
#define XATTR_REPLACE	0x2
/* winapfs: Linux caps xattr values at 64 KiB (a VFS limit, not an APFS one);
 * resource forks are often larger and are exposed as streams on Windows */
#define XATTR_SIZE_MAX	(256 << 20)
#define XATTR_NAME_MAX	255
#define XATTR_LIST_MAX	65536
#define XATTR_USER_PREFIX	"user."
#define XATTR_USER_PREFIX_LEN	5
#define XATTR_SECURITY_PREFIX	"security."
#define XATTR_TRUSTED_PREFIX	"trusted."
#define XATTR_MAC_OSX_PREFIX	"osx."
#define XATTR_MAC_OSX_PREFIX_LEN 4
struct xattr_handler {
	const char *name;
	const char *prefix;
	int flags;
	bool (*list)(struct dentry *dentry);
	int (*get)(const struct xattr_handler *, struct dentry *dentry, struct inode *inode, const char *name, void *buffer, size_t size);
	int (*set)(const struct xattr_handler *, struct mnt_idmap *idmap, struct dentry *dentry, struct inode *inode, const char *name, const void *buffer, size_t size, int flags);
};

/* ------------------------------------------------------------ super block */
#define SB_RDONLY	(1 << 0)
#define SB_NOSUID	(1 << 1)
#define SB_NODEV	(1 << 2)
#define SB_NOEXEC	(1 << 3)
#define SB_SYNCHRONOUS	(1 << 4)
#define SB_SILENT	(1 << 15)
#define SB_POSIXACL	(1 << 16)
#define SB_NOSEC	(1 << 28)
#define SB_ACTIVE	(1 << 30)
#define SB_NOATIME	(1 << 10)
#define SB_NODIRATIME	(1 << 11)
#define MAX_LFS_FILESIZE	((loff_t)INT64_MAX)
struct kstatfs {
	long f_type;
	long f_bsize;
	u64 f_blocks;
	u64 f_bfree;
	u64 f_bavail;
	u64 f_files;
	u64 f_ffree;
	struct { int val[2]; } f_fsid;
	long f_namelen;
	long f_frsize;
	long f_flags;
};
static inline u64 huge_encode_dev(dev_t d) { return d; }
#define new_encode_dev(d)	(d)
static inline void *u64_to_fsid(u64 v) { (void)v; return NULL; }

struct super_operations;
struct file_system_type;
struct super_block {
	struct list_head s_list;
	dev_t s_dev;
	unsigned char s_blocksize_bits;
	unsigned long s_blocksize;
	loff_t s_maxbytes;
	struct file_system_type *s_type;
	const struct super_operations *s_op;
	unsigned long s_flags;
	unsigned long s_iflags;
	unsigned long s_magic;
	struct dentry *s_root;
	struct rw_semaphore s_umount;
	int s_count;
	atomic_t s_active;
	const struct xattr_handler * const *s_xattr;
	struct block_device *s_bdev;
	struct file *s_bdev_file;
	struct backing_dev_info *s_bdi;
	void *s_fs_info;
	u32 s_time_gran;
	time64_t s_time_min;
	time64_t s_time_max;
	char s_id[32];
	uuid_t s_uuid;
	fmode_t s_mode;
	const struct dentry_operations *s_d_op;
	/* kshim private */
	void *kshim_icache;       /* inode hash table */
	struct mutex kshim_ilock;
	struct list_head kshim_inodes;
};
static inline bool sb_rdonly(const struct super_block *sb) { return (sb->s_flags & SB_RDONLY) != 0; }
#define SB_I_CGROUPWB	0
static inline fmode_t sb_open_mode(unsigned long flags) { return (flags & SB_RDONLY) ? FMODE_READ : (FMODE_READ | FMODE_WRITE); }
struct super_operations {
	struct inode *(*alloc_inode)(struct super_block *sb);
	void (*destroy_inode)(struct inode *);
	void (*free_inode)(struct inode *);
	void (*dirty_inode)(struct inode *, int flags);
	int (*write_inode)(struct inode *, struct writeback_control *wbc);
	int (*drop_inode)(struct inode *);
	void (*evict_inode)(struct inode *);
	void (*put_super)(struct super_block *);
	int (*sync_fs)(struct super_block *sb, int wait);
	int (*freeze_fs)(struct super_block *);
	int (*unfreeze_fs)(struct super_block *);
	int (*statfs)(struct dentry *, struct kstatfs *);
	int (*remount_fs)(struct super_block *, int *, char *);
	void (*umount_begin)(struct super_block *);
	int (*show_options)(struct seq_file *, struct dentry *);
};
struct file_system_type {
	const char *name;
	int fs_flags;
	struct dentry *(*mount)(struct file_system_type *, int, const char *, void *);
	void (*kill_sb)(struct super_block *);
	void *owner;
	struct file_system_type *next;
};
#define FS_REQUIRES_DEV		1
#define FS_ALLOW_IDMAP		32
int register_filesystem(struct file_system_type *fs);
int unregister_filesystem(struct file_system_type *fs);
struct dentry *mount_bdev(struct file_system_type *fs_type, int flags, const char *dev_name, void *data, int (*fill_super)(struct super_block *, void *, int));
struct super_block *sget(struct file_system_type *type, int (*test)(struct super_block *, void *), int (*set)(struct super_block *, void *), int flags, void *data);
void deactivate_locked_super(struct super_block *sb);
void generic_shutdown_super(struct super_block *sb);
void kill_block_super(struct super_block *sb);
void kill_anon_super(struct super_block *sb);
void kill_super_notify(struct super_block *sb);
int set_anon_super(struct super_block *sb, void *data);
int sync_filesystem(struct super_block *sb);
int super_setup_bdi(struct super_block *sb);
void evict_inodes(struct super_block *sb);
#define MODULE_ALIAS_FS(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_VERSION(x)
#define MODULE_ALIAS(x)
#define THIS_MODULE	NULL
/* the driver's module init/exit become callable entry points */
#define module_init(fn)	int kshim_module_init(void) { return fn(); }
#define module_exit(fn)	void kshim_module_exit(void) { fn(); }

/* mount options (old-style parser, linux/parser.h) */
#define MAX_OPT_ARGS	3
typedef struct { char *from; char *to; } substring_t;
struct match_token { int token; const char *pattern; };
typedef struct match_token match_table_t[];
int match_token(char *s, const match_table_t table, substring_t args[]);
int match_int(substring_t *s, int *result);
int match_u64(substring_t *s, u64 *result);
char *match_strdup(const substring_t *s);

/* seq_file (show_options) */
struct seq_file { char *buf; size_t size; size_t count; };
void seq_printf(struct seq_file *m, const char *fmt, ...);
void seq_puts(struct seq_file *m, const char *s);
void seq_show_option(struct seq_file *m, const char *name, const char *value);

/* ioctls (only referenced through the driver's ioctl handlers) */
#define _IOC(dir, type, nr, size)	(((dir) << 30) | ((type) << 8) | (nr) | ((u32)(size) << 16))
#define _IO(t, n)	_IOC(0, (t), (n), 0)
#define _IOR(t, n, s)	_IOC(2, (t), (n), sizeof(s))
#define _IOW(t, n, s)	_IOC(1, (t), (n), sizeof(s))
#define _IOWR(t, n, s)	_IOC(3, (t), (n), sizeof(s))
#define FS_IOC_GETFLAGS	_IOR('f', 1, long)
#define FS_IOC_SETFLAGS	_IOW('f', 2, long)
unsigned long copy_from_user(void *to, const void __user *from, unsigned long n);
unsigned long copy_to_user(void __user *to, const void *from, unsigned long n);
#define __copy_from_user	copy_from_user
#define __copy_to_user		copy_to_user
#define get_user(x, p)	(memcpy(&(x), (p), sizeof(*(p))), 0)
#define put_user(x, p)	(*(p) = (x), 0)

/* memory info (sizing caches) */
struct sysinfo { unsigned long totalram; unsigned long freeram; unsigned int mem_unit; };
void si_meminfo(struct sysinfo *val);

/* nls / utf8 */
int utf8_to_utf32(const u8 *s, int inlen, u32 *pu);

#endif /* KSHIM_FS_H */
