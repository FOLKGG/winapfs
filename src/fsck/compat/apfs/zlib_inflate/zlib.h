/* winapfs: apfsck only needs zlib_inflate_blob; it is backed by real zlib. */
#pragma once
/* raw deflate (callers strip the 2-byte zlib header); bytes produced or < 0 */
int zlib_inflate_blob(void *dst, unsigned dst_sz, const void *src, unsigned src_sz);
