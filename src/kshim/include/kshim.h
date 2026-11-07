/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * kshim: the subset of the Linux kernel API that linux-apfs-rw uses,
 * implemented in user mode on Windows. Every <linux/...> header the driver
 * includes resolves to this file.
 *
 * We present ourselves as Linux 6.12. The driver keeps compatibility branches
 * for many kernel versions; picking one fixes which API flavour has to be
 * emulated (folios, mnt_idmap, old mount API with mount_bdev).
 *
 * Semantics are those of a single-host, single-mount kernel: there is no
 * page reclaim, no writeback thread and no RCU. The buffer cache and page
 * cache keep everything until it is explicitly released.
 */
#ifndef KSHIM_H
#define KSHIM_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>   /* first, so the kernel S_IF* below replace the CRT ones */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ version */
#define KERNEL_VERSION(a, b, c)	(((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#define LINUX_VERSION_CODE	KERNEL_VERSION(6, 12, 0)

/* ------------------------------------------------------------ compiler */
#define __packed
#define __aligned(x)
#define __user
#define __force
#define __init
#define __exit
#define __iomem
#define __rcu
#define __must_check
#define __always_inline		__inline
#define noinline		__declspec(noinline)
#define __maybe_unused
#define __printf(a, b)
#define __cold
#define likely(x)		(x)
#define unlikely(x)		(x)
#define fallthrough		do {} while (0)
#define READ_ONCE(x)		(*(volatile __typeof_unqual__(x) *)&(x))
#define WRITE_ONCE(x, v)	(*(volatile __typeof_unqual__(x) *)&(x) = (v))
#define barrier()		_ReadWriteBarrier()
#define BUILD_BUG_ON(c)		static_assert(!(c), #c)
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)

/* MSVC C has no statement expressions; these are evaluated once by helpers */
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* ------------------------------------------------------------ types */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef u8 __u8;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;
typedef s32 __s32;
typedef s64 __s64;
typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;
typedef u64 __be64;
typedef s64 loff_t;
typedef u64 sector_t;
typedef u64 blkcnt_t;
typedef u16 umode_t;
typedef u32 dev_t;
typedef u32 gfp_t;
typedef u32 fmode_t;
typedef intptr_t ssize_t_;
#define ssize_t ssize_t_
typedef s64 time64_t;
typedef u32 vm_fault_t;
typedef u32 uid_t_;
typedef u32 gid_t_;
#define uid_t uid_t_
#define gid_t gid_t_
typedef u32 unicode_t;
#define UUID_SIZE	16
#define NAME_MAX	255
#define PATH_MAX_	4096
#define NSEC_PER_SEC	1000000000L
#define NSEC_PER_MSEC	1000000L
typedef struct { u32 val; } kuid_t;
typedef struct { u32 val; } kgid_t;
typedef struct { u8 b[16]; } uuid_t;
typedef struct { volatile long counter; } atomic_t;
typedef struct { volatile long long counter; } atomic64_t;

struct timespec64 {
	time64_t tv_sec;
	long tv_nsec;
};

/* ------------------------------------------------------------ errors */
#ifndef EUCLEAN
#define EUCLEAN		117
#endif
#ifndef EBADMSG
#define EBADMSG		74
#endif
#define ENOTBLK		15
#define EOPNOTSUPP_	95
#undef EOPNOTSUPP
#define EOPNOTSUPP	EOPNOTSUPP_
#define ENODATA_	61
#undef ENODATA
#define ENODATA		ENODATA_
#define ESTALE		116
#define EDQUOT		122
#define ENOTSUPP	524
#define ERESTARTSYS	512

#define MAX_ERRNO	4095
#define IS_ERR_VALUE(x)	((uintptr_t)(void *)(x) >= (uintptr_t)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)(intptr_t)error; }
static inline long PTR_ERR(const void *ptr) { return (long)(intptr_t)ptr; }
static inline bool IS_ERR(const void *ptr) { return IS_ERR_VALUE(ptr); }
static inline bool IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR_VALUE(ptr); }
static inline void *ERR_CAST(const void *ptr) { return (void *)ptr; }
static inline int PTR_ERR_OR_ZERO(const void *ptr) { return IS_ERR(ptr) ? (int)PTR_ERR(ptr) : 0; }

/* ------------------------------------------------------------ printk */
#define KERN_EMERG	"<0>"
#define KERN_ALERT	"<1>"
#define KERN_CRIT	"<2>"
#define KERN_ERR	"<3>"
#define KERN_WARNING	"<4>"
#define KERN_NOTICE	"<5>"
#define KERN_INFO	"<6>"
#define KERN_DEBUG	"<7>"
#define KERN_CONT	""
/* %pV in a format prints a nested struct va_format */
struct va_format { const char *fmt; va_list *va; };
void kshim_printk(const char *fmt, ...);
/* the driver formats %pg (block device) with snprintf */
int kshim_snprintf(char *buf, size_t size, const char *fmt, ...);
int kshim_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
#define snprintf kshim_snprintf
#define vsnprintf kshim_vsnprintf
#define scnprintf kshim_snprintf
#define printk		kshim_printk
#define pr_err(...)	kshim_printk(KERN_ERR __VA_ARGS__)
#define pr_warn(...)	kshim_printk(KERN_WARNING __VA_ARGS__)
#define pr_info(...)	kshim_printk(KERN_INFO __VA_ARGS__)
#define pr_notice(...)	kshim_printk(KERN_NOTICE __VA_ARGS__)
#define pr_debug(...)	((void)0)
#define pr_err_once	pr_err
#define pr_warn_once	pr_warn
#define pr_info_once	pr_info
#define no_printk(...)	((void)0)
void kshim_warn(const char *file, int line, const char *cond);
#define WARN_ON(c)	((c) ? (kshim_warn(__FILE__, __LINE__, #c), 1) : 0)
#define WARN_ON_ONCE(c)	WARN_ON(c)
#define WARN(c, ...)	WARN_ON(c)
__declspec(noreturn) void kshim_bug(const char *file, int line);
#define BUG()		kshim_bug(__FILE__, __LINE__)
#define BUG_ON(c)	do { if (c) BUG(); } while (0)
#define lockdep_assert_held(l)		((void)(l))
#define lockdep_assert_held_write(l)	((void)(l))
#define might_sleep()	((void)0)
#define cond_resched()	((void)0)

/* ------------------------------------------------------------ arithmetic */
#undef min
#undef max
#define min(a, b)	((a) < (b) ? (a) : (b))
#define max(a, b)	((a) > (b) ? (a) : (b))
#define min_t(t, a, b)	((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b)	((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define clamp_t(t, v, lo, hi)	min_t(t, max_t(t, v, lo), hi)
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define round_up(x, y)		((((x) - 1) | ((y) - 1)) + 1)
#define round_down(x, y)	((x) & ~((y) - 1))
#define ALIGN(x, a)		round_up(x, a)
#define IS_ALIGNED(x, a)	(((x) & ((a) - 1)) == 0)
#define U8_MAX		0xFF
#define U16_MAX		0xFFFF
#define U32_MAX		0xFFFFFFFFU
#define U64_MAX		0xFFFFFFFFFFFFFFFFULL
#define S64_MAX		INT64_MAX
#define S64_MIN		INT64_MIN
#define S32_MAX		INT32_MAX
#define BITS_PER_LONG	64
#define BITS_PER_BYTE	8
#define BIT(n)		(1UL << (n))
#define BIT_ULL(n)	(1ULL << (n))
/* do_div: divides n in place, returns the remainder */
static inline u32 kshim_do_div(u64 *n, u32 base) { u32 r = (u32)(*n % base); *n /= base; return r; }
#define do_div(n, base)	kshim_do_div(&(n), (base))
static inline u64 div_u64(u64 a, u32 b) { return a / b; }
static inline u64 div64_u64(u64 a, u64 b) { return a / b; }
static inline s64 div_s64(s64 a, s32 b) { return a / b; }
static inline u64 div_u64_rem(u64 a, u32 b, u32 *rem) { *rem = (u32)(a % b); return a / b; }
int kshim_fls64(u64 x);
static inline int ilog2(u64 n) { return kshim_fls64(n) - 1; }
static inline int order_base_2(u64 n) { return n <= 1 ? 0 : kshim_fls64(n - 1); }
static inline bool is_power_of_2(u64 n) { return n && !(n & (n - 1)); }
static inline int fls(u32 x) { return kshim_fls64(x); }
static inline int fls64(u64 x) { return kshim_fls64(x); }

/* ------------------------------------------------------------ byte order (x64 is LE) */
static inline u16 kshim_bswap16(u16 x) { return (u16)((x >> 8) | (x << 8)); }
static inline u32 kshim_bswap32(u32 x) { return _byteswap_ulong(x); }
static inline u64 kshim_bswap64(u64 x) { return _byteswap_uint64(x); }
#define le16_to_cpu(x)	((u16)(x))
#define le32_to_cpu(x)	((u32)(x))
#define le64_to_cpu(x)	((u64)(x))
#define cpu_to_le16(x)	((__le16)(x))
#define cpu_to_le32(x)	((__le32)(x))
#define cpu_to_le64(x)	((__le64)(x))
#define be16_to_cpu(x)	kshim_bswap16((u16)(x))
#define be32_to_cpu(x)	kshim_bswap32((u32)(x))
#define be64_to_cpu(x)	kshim_bswap64((u64)(x))
#define cpu_to_be16(x)	kshim_bswap16((u16)(x))
#define cpu_to_be32(x)	kshim_bswap32((u32)(x))
#define cpu_to_be64(x)	kshim_bswap64((u64)(x))
#define le16_to_cpup(p)	(*(const u16 *)(p))
#define le32_to_cpup(p)	(*(const u32 *)(p))
#define le64_to_cpup(p)	(*(const u64 *)(p))
#define cpu_to_le64p(p)	(*(const u64 *)(p))
static inline void le16_add_cpu(__le16 *v, u16 a) { *v = (u16)(*v + a); }
static inline void le32_add_cpu(__le32 *v, u32 a) { *v += a; }
static inline void le64_add_cpu(__le64 *v, u64 a) { *v += a; }
static inline u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 get_unaligned_le64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static inline u16 get_unaligned_le16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline void put_unaligned_le32(u32 v, void *p) { memcpy(p, &v, 4); }
static inline void put_unaligned_le64(u64 v, void *p) { memcpy(p, &v, 8); }

/* ------------------------------------------------------------ bitops */
static inline bool test_bit(long nr, const volatile void *addr) { return (((const volatile u64 *)addr)[nr / 64] >> (nr % 64)) & 1; }
void set_bit(long nr, volatile void *addr);
void clear_bit(long nr, volatile void *addr);
bool test_and_set_bit(long nr, volatile void *addr);
bool test_and_clear_bit(long nr, volatile void *addr);
static inline void __set_bit(long nr, volatile void *addr) { ((volatile u64 *)addr)[nr / 64] |= 1ULL << (nr % 64); }
static inline void __clear_bit(long nr, volatile void *addr) { ((volatile u64 *)addr)[nr / 64] &= ~(1ULL << (nr % 64)); }
/* little-endian bitmaps (on-disk): byte-addressed */
static inline bool test_bit_le(long nr, const void *addr) { return (((const u8 *)addr)[nr / 8] >> (nr % 8)) & 1; }
static inline void __set_bit_le(long nr, void *addr) { ((u8 *)addr)[nr / 8] |= (u8)(1 << (nr % 8)); }
static inline void __clear_bit_le(long nr, void *addr) { ((u8 *)addr)[nr / 8] &= (u8)~(1 << (nr % 8)); }
static inline bool __test_and_clear_bit_le(long nr, void *addr) { bool o = test_bit_le(nr, addr); __clear_bit_le(nr, addr); return o; }
static inline bool __test_and_set_bit_le(long nr, void *addr) { bool o = test_bit_le(nr, addr); __set_bit_le(nr, addr); return o; }
unsigned long find_next_zero_bit_le(const void *addr, unsigned long size, unsigned long offset);
unsigned long find_next_bit_le(const void *addr, unsigned long size, unsigned long offset);
static inline int hweight64(u64 w) { return (int)__popcnt64(w); }

/* ------------------------------------------------------------ atomics */
static inline int atomic_read(const atomic_t *v) { return (int)v->counter; }
static inline void atomic_set(atomic_t *v, int i) { v->counter = i; }
long kshim_atomic_add(volatile long *p, long v);
static inline void atomic_inc(atomic_t *v) { kshim_atomic_add(&v->counter, 1); }
static inline void atomic_dec(atomic_t *v) { kshim_atomic_add(&v->counter, -1); }
static inline int atomic_inc_return(atomic_t *v) { return (int)kshim_atomic_add(&v->counter, 1); }
static inline bool atomic_dec_and_test(atomic_t *v) { return kshim_atomic_add(&v->counter, -1) == 0; }
#define ATOMIC_INIT(i)	{ (i) }

/* ------------------------------------------------------------ memory */
#define GFP_KERNEL	0x1u
#define GFP_NOFS	0x2u
#define GFP_NOIO	0x4u
#define GFP_ATOMIC	0x8u
#define __GFP_NOFAIL	0x10u
#define __GFP_ZERO	0x20u
#define __GFP_MOVABLE	0x40u
#define GFP_USER	GFP_KERNEL
#define GFP_HIGHUSER_MOVABLE	GFP_KERNEL
void *kmalloc(size_t size, gfp_t flags);
void *kzalloc(size_t size, gfp_t flags);
void *kcalloc(size_t n, size_t size, gfp_t flags);
void *kmalloc_array(size_t n, size_t size, gfp_t flags);
void *krealloc(const void *p, size_t size, gfp_t flags);
void kfree(const void *p);
char *kstrdup(const char *s, gfp_t flags);
char *kstrndup(const char *s, size_t max, gfp_t flags);
void *kmemdup(const void *src, size_t len, gfp_t flags);
#define kvmalloc(s, f)	kmalloc(s, f)
#define kvzalloc(s, f)	kzalloc(s, f)
#define kvcalloc(n, s, f)	kcalloc(n, s, f)
#define kvmalloc_array(n, s, f)	kmalloc_array(n, s, f)
#define kvfree(p)	kfree(p)
#define vmalloc(s)	kmalloc(s, GFP_KERNEL)
#define vzalloc(s)	kzalloc(s, GFP_KERNEL)
#define vfree(p)	kfree(p)
static inline unsigned int memalloc_nofs_save(void) { return 0; }
static inline void memalloc_nofs_restore(unsigned int f) { (void)f; }

struct kmem_cache;
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align, unsigned long flags, void (*ctor)(void *));
void *kmem_cache_alloc(struct kmem_cache *c, gfp_t flags);
void kmem_cache_free(struct kmem_cache *c, void *p);
void kmem_cache_destroy(struct kmem_cache *c);
#define SLAB_RECLAIM_ACCOUNT	0x1
#define SLAB_MEM_SPREAD		0x2
#define SLAB_ACCOUNT		0x4

/* ------------------------------------------------------------ strings */
size_t strscpy(char *dst, const char *src, size_t size);
char *strsep(char **s, const char *ct);
int kstrtouint(const char *s, unsigned int base, unsigned int *res);
#define strnicmp	_strnicmp
static inline bool isascii_(int c) { return (unsigned)c < 128; }
#undef isascii
#define isascii isascii_

/* ------------------------------------------------------------ lists */
struct list_head {
	struct list_head *next, *prev;
};
struct hlist_node {
	struct hlist_node *next, **pprev;
};
#define LIST_HEAD_INIT(name)	{ &(name), &(name) }
#define LIST_HEAD(name)		struct list_head name = LIST_HEAD_INIT(name)
static inline void INIT_LIST_HEAD(struct list_head *l) { l->next = l; l->prev = l; }
static inline void __list_add(struct list_head *n, struct list_head *prev, struct list_head *next)
{
	next->prev = n; n->next = next; n->prev = prev; prev->next = n;
}
static inline void list_add(struct list_head *n, struct list_head *h) { __list_add(n, h, h->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *h) { __list_add(n, h->prev, h); }
static inline void list_del(struct list_head *e) { e->next->prev = e->prev; e->prev->next = e->next; e->next = e->prev = NULL; }
static inline void list_del_init(struct list_head *e) { e->next->prev = e->prev; e->prev->next = e->next; INIT_LIST_HEAD(e); }
static inline bool list_empty(const struct list_head *h) { return h->next == h; }
static inline void list_move(struct list_head *e, struct list_head *h) { list_del(e); list_add(e, h); }
static inline void list_move_tail(struct list_head *e, struct list_head *h) { list_del(e); list_add_tail(e, h); }
static inline void list_splice_init(struct list_head *l, struct list_head *h)
{
	if (!list_empty(l)) {
		struct list_head *first = l->next, *last = l->prev, *at = h->next;
		first->prev = h; h->next = first; last->next = at; at->prev = last;
		INIT_LIST_HEAD(l);
	}
}
#define list_entry(ptr, type, member)	container_of(ptr, type, member)
#define list_first_entry(ptr, type, member)	list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member)	list_entry((ptr)->prev, type, member)
#define list_first_entry_or_null(ptr, type, member)	(list_empty(ptr) ? NULL : list_first_entry(ptr, type, member))
/* MSVC: typeof(*pos) is spelled __typeof__ in C23 mode (/std:clatest) */
#define list_next_entry(pos, member)	list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_for_each(pos, head)	for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head)	for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_for_each_entry(pos, head, member) \
	for (pos = list_first_entry(head, __typeof__(*pos), member); &pos->member != (head); pos = list_next_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = list_first_entry(head, __typeof__(*pos), member), n = list_next_entry(pos, member); \
	     &pos->member != (head); pos = n, n = list_next_entry(n, member))

/* ------------------------------------------------------------ locks */
typedef struct { void *srw; } spinlock_t;   /* SRWLOCK, exclusive */
struct mutex { void *srw; };
struct rw_semaphore { void *srw; };
void spin_lock_init(spinlock_t *l);
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_is_locked(struct mutex *m);
void init_rwsem(struct rw_semaphore *s);
void down_read(struct rw_semaphore *s);
void up_read(struct rw_semaphore *s);
void down_write(struct rw_semaphore *s);
void up_write(struct rw_semaphore *s);
#define DEFINE_MUTEX(name)	struct mutex name = { 0 }
#define DEFINE_SPINLOCK(name)	spinlock_t name = { 0 }
#define __SPIN_LOCK_UNLOCKED(n)	{ 0 }
static inline void rcu_read_lock(void) {}
static inline void rcu_read_unlock(void) {}
static inline void rcu_barrier(void) {}
struct rcu_head { void *next; void (*func)(struct rcu_head *); };
/* no RCU readers outlive a call here, so callbacks run immediately */
static inline void call_rcu(struct rcu_head *h, void (*f)(struct rcu_head *)) { f(h); }

/* completions (used by synchronous buffer I/O) */
struct completion { volatile long done; };
static inline void init_completion(struct completion *c) { c->done = 0; }
static inline void complete(struct completion *c) { c->done = 1; }

/* ------------------------------------------------------------ time */
struct timespec64 current_time(struct inode *inode);
u64 ktime_get_real_ns(void);
void ktime_get_real_ts64(struct timespec64 *ts);
static inline struct timespec64 ns_to_timespec64(s64 ns)
{
	struct timespec64 t;
	t.tv_sec = ns / 1000000000; t.tv_nsec = (long)(ns % 1000000000);
	if (t.tv_nsec < 0) { t.tv_nsec += 1000000000; t.tv_sec--; }
	return t;
}
static inline s64 timespec64_to_ns(const struct timespec64 *t) { return t->tv_sec * 1000000000LL + t->tv_nsec; }
static inline unsigned long msecs_to_jiffies(unsigned int m) { return m; }
#define HZ	1000
extern volatile unsigned long jiffies;

/* ------------------------------------------------------------ ids */
#define GLOBAL_ROOT_UID	((kuid_t){ 0 })
#define GLOBAL_ROOT_GID	((kgid_t){ 0 })
#define INVALID_UID	((kuid_t){ (u32)-1 })
#define INVALID_GID	((kgid_t){ (u32)-1 })
struct user_namespace { int unused; };
extern struct user_namespace init_user_ns;
static inline struct user_namespace *current_user_ns(void) { return &init_user_ns; }
static inline kuid_t make_kuid(struct user_namespace *ns, u32 uid) { kuid_t k; (void)ns; k.val = uid; return k; }
static inline kgid_t make_kgid(struct user_namespace *ns, u32 gid) { kgid_t k; (void)ns; k.val = gid; return k; }
static inline u32 from_kuid(struct user_namespace *ns, kuid_t k) { (void)ns; return k.val; }
static inline u32 from_kgid(struct user_namespace *ns, kgid_t k) { (void)ns; return k.val; }
static inline bool uid_valid(kuid_t k) { return k.val != (u32)-1; }
static inline bool gid_valid(kgid_t k) { return k.val != (u32)-1; }
static inline bool uid_eq(kuid_t a, kuid_t b) { return a.val == b.val; }
static inline bool gid_eq(kgid_t a, kgid_t b) { return a.val == b.val; }
static inline bool uuid_equal(const uuid_t *a, const uuid_t *b) { return memcmp(a, b, 16) == 0; }
static inline bool uuid_is_null(const uuid_t *u) { static const uuid_t z; return uuid_equal(u, &z); }
static inline void uuid_copy(uuid_t *d, const uuid_t *s) { memcpy(d, s, 16); }
static inline void import_uuid(uuid_t *d, const u8 *s) { memcpy(d, s, 16); }

/* ------------------------------------------------------------ crc / zlib */
u32 crc32c(u32 crc, const void *data, unsigned int len);
/* returns bytes produced or a negative errno (see linux/zlib.h zlib_inflate_blob) */
int zlib_inflate_blob(void *dst, unsigned int dst_sz, const void *src, unsigned int src_sz);

#include "kshim_fs.h"

#ifdef __cplusplus
}
#endif

#endif /* KSHIM_H */
