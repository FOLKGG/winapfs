/*
 * winapfs - read-only APFS for Windows
 * Copyright (C) 2026 winapfs contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * kshim: printk, memory, strings, bitops, atomics, locks, time, crc32c,
 * zlib, utf8, mount-option parser, seq_file and other small primitives.
 */
#define KSHIM_ALLOC_IMPL   /* this file defines kmalloc & co. */
#include <kshim.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <zlib.h>

/* this file implements snprintf for the driver; use the CRT one here */
#undef snprintf
#undef vsnprintf

/* ------------------------------------------------------------ printk */

static void (*g_log_sink)(int level, const char *msg);

void kshim_set_log_sink(void (*sink)(int level, const char *msg))
{
	g_log_sink = sink;
}

/* vsnprintf with support for %pV (nested struct va_format) and %pg (bdev name) */
static int kshim_vformat(char *out, size_t size, const char *fmt, va_list ap)
{
	size_t n = 0;
	char spec[32];

	while (*fmt) {
		if (*fmt != '%') {
			if (n + 1 < size)
				out[n] = *fmt;
			n++;
			fmt++;
			continue;
		}
		if (fmt[1] == '%') {
			if (n + 1 < size)
				out[n] = '%';
			n++;
			fmt += 2;
			continue;
		}
		if (fmt[1] == 'p' && fmt[2] == 'V') {
			struct va_format *vaf = va_arg(ap, struct va_format *);
			va_list inner;
			va_copy(inner, *vaf->va);
			n += kshim_vformat(n < size ? out + n : NULL, n < size ? size - n : 0, vaf->fmt, inner);
			va_end(inner);
			fmt += 3;
			continue;
		}
		if (fmt[1] == 'p' && fmt[2] == 'g') {
			struct block_device *b = va_arg(ap, struct block_device *);
			int k = snprintf(n < size ? out + n : NULL, n < size ? size - n : 0, "%s", b ? b->bd_name : "(null)");
			n += k > 0 ? k : 0;
			fmt += 3;
			continue;
		}
		/* copy one conversion spec and hand it to vsnprintf */
		size_t len = 1;
		while (fmt[len] && !strchr("diouxXcspfeEgGn", fmt[len]) && len < sizeof(spec) - 2)
			len++;
		if (!fmt[len]) {
			break;
		}
		memcpy(spec, fmt, len + 1);
		spec[len + 1] = 0;
		/* MSVC has no %zd/%zu issues; %ll is fine. Pull the argument by type. */
		char conv = fmt[len];
		int k;
		bool is_ll = strstr(spec, "ll") != NULL;
		bool is_l = !is_ll && strchr(spec, 'l') != NULL;
		bool is_z = strchr(spec, 'z') != NULL;
		bool star = strchr(spec, '*') != NULL;
		int width = star ? va_arg(ap, int) : 0;
		char *dst = n < size ? out + n : NULL;
		size_t room = n < size ? size - n : 0;
		if (conv == 's') {
			const char *s = va_arg(ap, const char *);
			k = star ? snprintf(dst, room, spec, width, s ? s : "(null)") : snprintf(dst, room, spec, s ? s : "(null)");
		} else if (conv == 'p') {
			void *p = va_arg(ap, void *);
			k = snprintf(dst, room, "%p", p);
		} else if (conv == 'c') {
			int c = va_arg(ap, int);
			k = snprintf(dst, room, spec, c);
		} else if (strchr("fFeEgG", conv)) {
			double d = va_arg(ap, double);
			k = snprintf(dst, room, spec, d);
		} else if (is_ll || is_z || is_l) {
			long long v = va_arg(ap, long long);
			k = star ? snprintf(dst, room, spec, width, v) : snprintf(dst, room, spec, v);
		} else {
			int v = va_arg(ap, int);
			k = star ? snprintf(dst, room, spec, width, v) : snprintf(dst, room, spec, v);
		}
		n += k > 0 ? (size_t)k : 0;
		fmt += len + 1;
	}
	if (size)
		out[n < size ? n : size - 1] = 0;
	return (int)n;
}

int kshim_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = kshim_vformat(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

int kshim_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	return kshim_vformat(buf, size, fmt, ap);
}

void kshim_printk(const char *fmt, ...)
{
	char buf[1024];
	int level = 6;
	va_list ap;

	if (fmt[0] == '<' && fmt[1] >= '0' && fmt[1] <= '7' && fmt[2] == '>') {
		level = fmt[1] - '0';
		fmt += 3;
	}
	va_start(ap, fmt);
	kshim_vformat(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (g_log_sink)
		g_log_sink(level, buf);
	else if (level <= 4)
		fprintf(stderr, "apfs: %s%s", buf, buf[0] && buf[strlen(buf) - 1] == '\n' ? "" : "\n");
}

void kshim_warn(const char *file, int line, const char *cond)
{
	kshim_printk(KERN_WARNING "WARNING at %s:%d: %s\n", file, line, cond);
}

void kshim_bug(const char *file, int line)
{
	kshim_printk(KERN_EMERG "BUG at %s:%d\n", file, line);
	fflush(stderr);
	abort();
}

/* ------------------------------------------------------------ memory */

#ifdef _DEBUG
void *kshim_kmalloc_dbg(size_t size, gfp_t flags, const char *file, int line)
{
	void *p = _malloc_dbg(size ? size : 1, _NORMAL_BLOCK, file, line);
	if (p && (flags & __GFP_ZERO))
		memset(p, 0, size);
	return p;
}

void *kshim_kcalloc_dbg(size_t n, size_t size, gfp_t flags, const char *file, int line)
{
	if (size && n > SIZE_MAX / size)
		return NULL;
	return _calloc_dbg(n ? n : 1, size ? size : 1, _NORMAL_BLOCK, file, line);
}
#endif

void *kmalloc(size_t size, gfp_t flags)
{
	void *p = malloc(size ? size : 1);
	if (p && (flags & __GFP_ZERO))
		memset(p, 0, size);
	return p;
}

void *kzalloc(size_t size, gfp_t flags)
{
	return calloc(1, size ? size : 1);
}

void *kcalloc(size_t n, size_t size, gfp_t flags)
{
	if (size && n > SIZE_MAX / size)
		return NULL;
	return calloc(n ? n : 1, size ? size : 1);
}

void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{
	if (size && n > SIZE_MAX / size)
		return NULL;
	return kmalloc(n * size, flags);
}

void *krealloc(const void *p, size_t size, gfp_t flags)
{
	return realloc((void *)p, size ? size : 1);
}

void kfree(const void *p)
{
	free((void *)p);
}

char *kstrdup(const char *s, gfp_t flags)
{
	return s ? _strdup(s) : NULL;
}

char *kstrndup(const char *s, size_t max, gfp_t flags)
{
	size_t n = strnlen(s, max);
	char *p = malloc(n + 1);
	if (p) {
		memcpy(p, s, n);
		p[n] = 0;
	}
	return p;
}

void *kmemdup(const void *src, size_t len, gfp_t flags)
{
	void *p = malloc(len ? len : 1);
	if (p)
		memcpy(p, src, len);
	return p;
}

struct kmem_cache {
	unsigned int size;
	void (*ctor)(void *);
};

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align, unsigned long flags, void (*ctor)(void *))
{
	struct kmem_cache *c = calloc(1, sizeof(*c));
	if (c) {
		c->size = size;
		c->ctor = ctor;
	}
	return c;
}

void *kmem_cache_alloc(struct kmem_cache *c, gfp_t flags)
{
	void *p = calloc(1, c->size);
	if (p && c->ctor)
		c->ctor(p);
	return p;
}

void kmem_cache_free(struct kmem_cache *c, void *p)
{
	free(p);
}

void kmem_cache_destroy(struct kmem_cache *c)
{
	free(c);
}

void si_meminfo(struct sysinfo *val)
{
	MEMORYSTATUSEX ms = { sizeof(ms) };
	GlobalMemoryStatusEx(&ms);
	val->mem_unit = PAGE_SIZE;
	val->totalram = (unsigned long)(ms.ullTotalPhys / PAGE_SIZE);
	val->freeram = (unsigned long)(ms.ullAvailPhys / PAGE_SIZE);
}

unsigned long copy_from_user(void *to, const void *from, unsigned long n)
{
	memcpy(to, from, n);
	return 0;
}

unsigned long copy_to_user(void *to, const void *from, unsigned long n)
{
	memcpy(to, from, n);
	return 0;
}

/* ------------------------------------------------------------ strings */

size_t strscpy(char *dst, const char *src, size_t size)
{
	size_t n;
	if (!size)
		return (size_t)-E2BIG;
	n = strnlen(src, size);
	if (n == size) {
		memcpy(dst, src, size - 1);
		dst[size - 1] = 0;
		return (size_t)-E2BIG;
	}
	memcpy(dst, src, n + 1);
	return n;
}

char *strsep(char **s, const char *ct)
{
	char *start = *s, *end;
	if (!start)
		return NULL;
	end = start + strcspn(start, ct);
	if (*end) {
		*end = 0;
		*s = end + 1;
	} else {
		*s = NULL;
	}
	return start;
}

int kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
	char *end;
	unsigned long v = strtoul(s, &end, base);
	if (end == s || (*end && *end != '\n'))
		return -EINVAL;
	*res = (unsigned int)v;
	return 0;
}

/* ------------------------------------------------------------ bitops / atomics */

int kshim_fls64(u64 x)
{
	unsigned long idx;
	if (!x)
		return 0;
	_BitScanReverse64(&idx, x);
	return (int)idx + 1;
}

void set_bit(long nr, volatile void *addr)
{
	_interlockedbittestandset64((volatile long long *)addr + nr / 64, nr % 64);
}

void clear_bit(long nr, volatile void *addr)
{
	_interlockedbittestandreset64((volatile long long *)addr + nr / 64, nr % 64);
}

bool test_and_set_bit(long nr, volatile void *addr)
{
	return _interlockedbittestandset64((volatile long long *)addr + nr / 64, nr % 64) != 0;
}

bool test_and_clear_bit(long nr, volatile void *addr)
{
	return _interlockedbittestandreset64((volatile long long *)addr + nr / 64, nr % 64) != 0;
}

/*
 * Little-endian bitmap searches, 64 bits at a time (x64 is little-endian, so
 * a u64 load sees bit i of the bitmap as bit i % 64 of word i / 64). The
 * driver's allocator scans a 32768-bit chunk bitmap from 0 for every block;
 * bit-at-a-time this was 30% of a large write. invert: search for a zero.
 */
static unsigned long find_le(const void *addr, unsigned long size, unsigned long offset, bool invert)
{
	const u8 *p = addr;
	while (offset < size) {
		unsigned long word = offset / 64, bit = offset % 64;
		u64 w;
		unsigned long idx;
		if ((word + 1) * 64 <= size) {
			memcpy(&w, p + word * 8, 8);
		} else {
			/* tail word: only the bytes inside the bitmap */
			size_t bytes = (size_t)((size - word * 64 + 7) / 8);
			w = 0;
			memcpy(&w, p + word * 8, bytes);
		}
		if (invert)
			w = ~w;
		w &= ~0ull << bit;
		if (w && _BitScanForward64(&idx, w)) {
			unsigned long found = word * 64 + idx;
			return found < size ? found : size;
		}
		offset = (word + 1) * 64;
	}
	return size;
}

unsigned long find_next_zero_bit_le(const void *addr, unsigned long size, unsigned long offset)
{
	return find_le(addr, size, offset, true);
}

unsigned long find_next_bit_le(const void *addr, unsigned long size, unsigned long offset)
{
	return find_le(addr, size, offset, false);
}

long kshim_atomic_add(volatile long *p, long v)
{
	return InterlockedAdd(p, v);
}

/* ------------------------------------------------------------ locks (SRWLOCK) */

void spin_lock_init(spinlock_t *l) { InitializeSRWLock((PSRWLOCK)&l->srw); }
void spin_lock(spinlock_t *l) { AcquireSRWLockExclusive((PSRWLOCK)&l->srw); }
void spin_unlock(spinlock_t *l) { ReleaseSRWLockExclusive((PSRWLOCK)&l->srw); }

/*
 * The driver never takes a mutex recursively, and SRW locks are not
 * reentrant either, so they map directly.
 */
void mutex_init(struct mutex *m) { InitializeSRWLock((PSRWLOCK)&m->srw); }
void mutex_lock(struct mutex *m) { AcquireSRWLockExclusive((PSRWLOCK)&m->srw); }
void mutex_unlock(struct mutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)&m->srw); }
bool mutex_is_locked(struct mutex *m)
{
	if (TryAcquireSRWLockExclusive((PSRWLOCK)&m->srw)) {
		ReleaseSRWLockExclusive((PSRWLOCK)&m->srw);
		return false;
	}
	return true;
}

void init_rwsem(struct rw_semaphore *s) { InitializeSRWLock((PSRWLOCK)&s->srw); }
void down_read(struct rw_semaphore *s) { AcquireSRWLockShared((PSRWLOCK)&s->srw); }
void up_read(struct rw_semaphore *s) { ReleaseSRWLockShared((PSRWLOCK)&s->srw); }
void down_write(struct rw_semaphore *s) { AcquireSRWLockExclusive((PSRWLOCK)&s->srw); }
void up_write(struct rw_semaphore *s) { ReleaseSRWLockExclusive((PSRWLOCK)&s->srw); }

/* ------------------------------------------------------------ time */

volatile unsigned long jiffies;

u64 ktime_get_real_ns(void)
{
	FILETIME ft;
	ULARGE_INTEGER u;
	GetSystemTimePreciseAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	/* 100ns since 1601 -> ns since 1970 */
	return (u.QuadPart - 116444736000000000ULL) * 100;
}

void ktime_get_real_ts64(struct timespec64 *ts)
{
	*ts = ns_to_timespec64((s64)ktime_get_real_ns());
}

struct timespec64 current_time(struct inode *inode)
{
	struct timespec64 t;
	ktime_get_real_ts64(&t);
	return t;
}

/* ------------------------------------------------------------ crc32c (Castagnoli) */

static u32 g_crc32c_table[8][256];
static INIT_ONCE g_crc32c_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK crc32c_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	for (u32 i = 0; i < 256; i++) {
		u32 c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0x82F63B78 : c >> 1;
		g_crc32c_table[0][i] = c;
	}
	for (u32 i = 0; i < 256; i++)
		for (int t = 1; t < 8; t++)
			g_crc32c_table[t][i] = (g_crc32c_table[t - 1][i] >> 8) ^ g_crc32c_table[0][g_crc32c_table[t - 1][i] & 0xFF];
	return TRUE;
}

/* Linux crc32c(): no pre/post inversion; callers pass ~0 seeds themselves. */
u32 crc32c(u32 crc, const void *data, unsigned int len)
{
	const u8 *p = data;
	InitOnceExecuteOnce(&g_crc32c_once, crc32c_init, NULL, NULL);
	while (len >= 8) {
		u64 v;
		memcpy(&v, p, 8);
		v ^= crc;
		crc = g_crc32c_table[7][v & 0xFF] ^ g_crc32c_table[6][(v >> 8) & 0xFF] ^
		      g_crc32c_table[5][(v >> 16) & 0xFF] ^ g_crc32c_table[4][(v >> 24) & 0xFF] ^
		      g_crc32c_table[3][(v >> 32) & 0xFF] ^ g_crc32c_table[2][(v >> 40) & 0xFF] ^
		      g_crc32c_table[1][(v >> 48) & 0xFF] ^ g_crc32c_table[0][v >> 56];
		p += 8;
		len -= 8;
	}
	while (len--)
		crc = g_crc32c_table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc;
}

/* ------------------------------------------------------------ zlib */

int zlib_inflate_blob(void *dst, unsigned int dst_sz, const void *src, unsigned int src_sz)
{
	z_stream s;
	int ret;

	memset(&s, 0, sizeof(s));
	/* raw deflate, like lib/zlib_inflate: callers strip the 2-byte zlib header */
	if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
		return -ENOMEM;
	s.next_in = (Bytef *)src;
	s.avail_in = src_sz;
	s.next_out = dst;
	s.avail_out = dst_sz;
	ret = inflate(&s, Z_FINISH);
	inflateEnd(&s);
	/* the trailing adler32 is left unread; a full output buffer is fine too */
	if (ret != Z_STREAM_END && !(ret == Z_BUF_ERROR && s.avail_out == 0))
		return -EINVAL;
	return (int)(dst_sz - s.avail_out);
}

/* ------------------------------------------------------------ utf8 (linux/nls.h) */

int utf8_to_utf32(const u8 *s, int inlen, u32 *pu)
{
	u32 c;
	int len, k;

	if (inlen <= 0)
		return -1;
	c = s[0];
	if (c < 0x80) {
		*pu = c;
		return 1;
	}
	if ((c & 0xE0) == 0xC0) { len = 2; c &= 0x1F; }
	else if ((c & 0xF0) == 0xE0) { len = 3; c &= 0x0F; }
	else if ((c & 0xF8) == 0xF0) { len = 4; c &= 0x07; }
	else return -1;
	if (inlen < len)
		return -1;
	for (k = 1; k < len; k++) {
		if ((s[k] & 0xC0) != 0x80)
			return -1;
		c = (c << 6) | (s[k] & 0x3F);
	}
	if ((len == 2 && c < 0x80) || (len == 3 && c < 0x800) || (len == 4 && c < 0x10000) || c > 0x10FFFF ||
	    (c >= 0xD800 && c <= 0xDFFF))
		return -1;
	*pu = c;
	return len;
}

/* ------------------------------------------------------------ mount option parser (lib/parser.c) */

/* Supports the subset of patterns the driver uses: "name", "name=%u", "name=%s". */
int match_token(char *s, const match_table_t table, substring_t args[])
{
	const struct match_token *p;

	for (p = table; p->pattern; p++) {
		const char *pct = strchr(p->pattern, '%');
		size_t fixed = pct ? (size_t)(pct - p->pattern) : strlen(p->pattern);
		if (strncmp(s, p->pattern, fixed) != 0)
			continue;
		if (!pct) {
			if (s[fixed] == 0)
				return p->token;
			continue;
		}
		args[0].from = s + fixed;
		args[0].to = s + strlen(s);
		if (pct[1] == 'u' || pct[1] == 'd') {
			char *end;
			strtoll(args[0].from, &end, 0);
			if (end == args[0].from || *end)
				continue;
		}
		return p->token;
	}
	return p->token;
}

int match_int(substring_t *s, int *result)
{
	char buf[32];
	size_t n = (size_t)(s->to - s->from);
	char *end;
	if (n >= sizeof(buf))
		return -ERANGE;
	memcpy(buf, s->from, n);
	buf[n] = 0;
	*result = (int)strtol(buf, &end, 0);
	return *end ? -EINVAL : 0;
}

int match_u64(substring_t *s, u64 *result)
{
	char buf[32];
	size_t n = (size_t)(s->to - s->from);
	char *end;
	if (n >= sizeof(buf))
		return -ERANGE;
	memcpy(buf, s->from, n);
	buf[n] = 0;
	*result = strtoull(buf, &end, 0);
	return *end ? -EINVAL : 0;
}

char *match_strdup(const substring_t *s)
{
	return kstrndup(s->from, (size_t)(s->to - s->from), GFP_KERNEL);
}

/* ------------------------------------------------------------ seq_file */

void seq_printf(struct seq_file *m, const char *fmt, ...)
{
	va_list ap;
	int n;
	if (m->count >= m->size)
		return;
	va_start(ap, fmt);
	n = kshim_vformat(m->buf + m->count, m->size - m->count, fmt, ap);
	va_end(ap);
	m->count += n > 0 ? (size_t)n : 0;
	if (m->count > m->size)
		m->count = m->size;
}

void seq_puts(struct seq_file *m, const char *s)
{
	seq_printf(m, "%s", s);
}

void seq_show_option(struct seq_file *m, const char *name, const char *value)
{
	if (value)
		seq_printf(m, ",%s=%s", name, value);
	else
		seq_printf(m, ",%s", name);
}

/* ------------------------------------------------------------ process context */

struct user_namespace init_user_ns;
struct mnt_idmap nop_mnt_idmap;
static struct cred g_cred;
void *current = &g_cred;

const struct cred *current_cred(void)
{
	return &g_cred;
}

bool fatal_signal_pending(void *task)
{
	return false;
}
