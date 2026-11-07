/*
 * Copyright (C) 2020 Corellium LLC
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "apfs/raw.h"
#include "apfs/libzbitmap.h"
#include "apfs/zlib_inflate/zlib.h"
#include <lzfse.h>		/* winapfs: Apple's lzfse, for LZVN/LZFSE */
#include <lzvn_decode_base.h>
#include "apfsck.h"
#include "btree.h"
#include "compress.h"
#include "extents.h"
#include "super.h"

/* maximum size of compressed data currently supported */
#define MAX_FBUF_SIZE        (1024 * 1024 * 1024)

/*
 * winapfs (upstream candidate): only zlib and plain resource forks use the
 * classic resource fork header; LZVN, LZFSE and LZBITMAP forks start with a
 * table of block offsets. This matches linux-apfs-rw's compress.c. Before,
 * every ditto-compressed (LZVN) file was reported as corrupted.
 */
static bool rsrc_has_classic_header(u32 algo)
{
    return algo != APFS_COMPRESS_LZBITMAP_RSRC && algo != APFS_COMPRESS_LZVN_RSRC && algo != APFS_COMPRESS_LZFSE_RSRC;
}

/*
 * winapfs (upstream candidate): decode one LZVN or LZFSE chunk (a whole inline
 * payload or one 64 KiB resource fork block), the way linux-apfs-rw does: a
 * leading 0x06 (LZVN) or anything but an lzfse magic (LZFSE) marks stored
 * data. Returns the decoded length, or -1 if the data does not decode.
 */
static ssize_t decode_lz_chunk(u32 algo, u8 *dst, size_t dst_size, const u8 *src, size_t src_len)
{
    if(src_len == 0)
        return -1;
    if(algo == APFS_COMPRESS_LZVN_ATTR || algo == APFS_COMPRESS_LZVN_RSRC) {
        lzvn_decoder_state dstate = {0};

        if(src[0] == 0x06) {
            if(src_len - 1 > dst_size)
                return -1;
            memcpy(dst, src + 1, src_len - 1);
            return src_len - 1;
        }
        dstate.src = src;
        dstate.src_end = src + src_len;
        dstate.dst = dstate.dst_begin = dst;
        dstate.dst_end = dst + dst_size;
        lzvn_decode(&dstate);
        return dstate.dst - dst;
    }
    if(src[0] == 0x62 && src_len >= 2) {   /* "bvx" block magic */
        size_t res = lzfse_decode_buffer(dst, dst_size, src, src_len, NULL);
        return res ? (ssize_t)res : -1;
    }
    if(src_len - 1 > dst_size)
        return -1;
    memcpy(dst, src + 1, src_len - 1);
    return src_len - 1;
}

void read_whole_dstream(u64 oid, void *buf, loff_t size)
{
    void *block = NULL;
    u64 curr_copylen;
    u64 bno;
    int ret;
    u64 i;

    i = 0;
    while(size) {
        curr_copylen = MIN(size, sb->s_blocksize);

        if(apfs_volume_is_sealed())
            ret = fext_tree_lookup(oid, i << sb->s_blocksize_bits, &bno);
        else
            ret = file_extent_lookup(oid, i << sb->s_blocksize_bits, &bno);

        if(ret)
            report("Compressed file", "dstream read failed.");

        if(bno == 0)
            report("Compressed file", "has a hole.");

        block = apfs_mmap(NULL, sb->s_blocksize, PROT_READ, MAP_PRIVATE, bno * sb->s_blocksize);
        if(block == MAP_FAILED)
            system_error();
        memcpy(buf, block, curr_copylen);
        munmap(block, sb->s_blocksize);
        block = NULL;

        size -= curr_copylen;
        buf = (char *)buf + curr_copylen; /* winapfs: no void* arithmetic in MSVC */
        ++i;
    }
}

static inline bool apfs_compressed_in_dstream(struct compress *compress)
{
    /* For sealed volumes we may use a fake dstream that only holds the hash */
    return compress->rsrc_dstream && !compress->rsrc_dstream->d_inline;
}

void apfs_compress_open(struct compress *compress)
{
    struct dstream *rsrc = compress->rsrc_dstream;
    struct apfs_compress_file_data *fd;
    ssize_t res;
    u8 *cdata;
    ssize_t csize;

    fd = calloc(1, sizeof(*fd));
    if(!fd)
        system_error();

    if(!compress->decmpfs)
        report("Compressed file", "missing decmpfs xattr.");
    memcpy(&fd->hdr, compress->decmpfs, sizeof(fd->hdr));

    if(apfs_compressed_in_dstream(compress) && rsrc_has_classic_header(le32_to_cpu(fd->hdr.algo))) {
        struct apfs_compress_rsrc_hdr *rsrc_hdr = NULL;
        struct apfs_compress_rsrc_data *rsrc_data = NULL;
    	u32 data_end;

        fd->buf = malloc(APFS_COMPRESS_BLOCK);
        if(!fd->buf)
            system_error();
        fd->bufblk = -1;

        if(rsrc->d_size > MAX_FBUF_SIZE)
            report_unknown("Large compressed file");

        fd->size = rsrc->d_size;
        fd->data = malloc(fd->size);
        if(!fd->data)
            system_error();
        read_whole_dstream(rsrc->d_id, fd->data, fd->size);

        if(fd->size < sizeof(*rsrc_hdr))
            report("Resource compressed file", "header won't fit.");
        rsrc_hdr = fd->data;

        /* TODO: check for overlaps, figure out 'mgmt' */
        compress->data_offs = be32_to_cpu(rsrc_hdr->data_offs);
        compress->data_size = be32_to_cpu(rsrc_hdr->data_size);
        data_end = compress->data_offs + compress->data_size;
        if(data_end < compress->data_offs || data_end > fd->size)
            report("Resource compressed file", "block metadata is too big.");

        if(compress->data_size < sizeof(*rsrc_data))
            report("Resource compressed file", "block metadata header won't fit.");
        rsrc_data = (void *)((char *)fd->data + compress->data_offs);

        compress->block_num = le32_to_cpu(rsrc_data->num);
        if(compress->block_num > MAX_FBUF_SIZE) /* Rough bound to avoid overflow */
            report_unknown("Large compressed file");
        if(compress->data_size < compress->block_num * sizeof(rsrc_data->block[0]) + sizeof(*rsrc_data))
            report("Resource compressed file", "block metadata won't fit.");
        /* TODO: figure out the 'unknown' field */
    } else if(apfs_compressed_in_dstream(compress)) {
        __le32 *block_offs;
        int i;

        fd->buf = malloc(APFS_COMPRESS_BLOCK);
        if(!fd->buf)
            system_error();
        fd->bufblk = -1;

        if(rsrc->d_size > MAX_FBUF_SIZE)
            report_unknown("Large compressed file");

        fd->size = rsrc->d_size;
        fd->data = malloc(fd->size);
        if(!fd->data)
            system_error();
        read_whole_dstream(rsrc->d_id, fd->data, fd->size);
        block_offs = fd->data;

        compress->data_offs = 0;
        compress->data_size = fd->size;
        /* Put a rough bound on block count to avoid overflow */
        for(i = 0; i < MAX_FBUF_SIZE; ++i) {
            if((i + 1) * sizeof(*block_offs) >= fd->size)
                report("LZBITMAP-compressed file", "block offsets won't fit.");
            if(le32_to_cpu(block_offs[i]) == fd->size)
                break;
        }
        compress->block_num = i;
        if(compress->block_num == MAX_FBUF_SIZE)
            report("LZBITMAP-compressed file", "missing final block offset.");
    } else {
        if(le64_to_cpu(fd->hdr.size) > MAX_FBUF_SIZE)
            report("Inline compressed file", "size is too big.");

        fd->size = le64_to_cpu(fd->hdr.size);
        fd->data = malloc(le64_to_cpu(fd->hdr.size));
        if(!fd->data)
            system_error();

        cdata = (void *)((char *)compress->decmpfs + sizeof(fd->hdr));
        csize = compress->decmpfs_len - sizeof(fd->hdr);

        compress->block_num = 1;

        switch(le32_to_cpu(fd->hdr.algo)) {
        case APFS_COMPRESS_ZLIB_ATTR:
            if(cdata[0] == 0x78 && csize >= 2) {
                res = zlib_inflate_blob(fd->data, fd->size, cdata + 2, csize - 2);
                if(res != fd->size)
                    report("Inline compressed file", "wrong reported length.");
            } else if((cdata[0] & 0x0F) == 0x0F) {
                if(csize - 1 != fd->size)
                    report("Inline compressed file", "wrong reported length.");
                memcpy(fd->data, cdata + 1, csize - 1);
            } else {
                report("Inline compressed file", "invalid header for zlib.");
            }
            break;
        case APFS_COMPRESS_PLAIN_ATTR:
            if(csize - 1 != fd->size)
                report("Inline uncompressed file", "wron reported length.");
            memcpy(fd->data, cdata + 1, csize - 1);
            break;
        case APFS_COMPRESS_LZVN_ATTR:	/* winapfs */
        case APFS_COMPRESS_LZFSE_ATTR:
            res = decode_lz_chunk(le32_to_cpu(fd->hdr.algo), fd->data, fd->size, cdata, csize);
            if(res != fd->size)
                report("Inline compressed file", "wrong reported length.");
            break;
        default:
            report_unknown("Compression algorithm");
        }
    }
    compress->compress_data = fd;
}

/* If @buf is NULL, just return the size */
static ssize_t apfs_compress_read_block(struct compress *compress, char *buf, size_t size, loff_t off)
{
    struct apfs_compress_file_data *fd = compress->compress_data;
    u32 doffs, coffs;
    loff_t block;
    u8 *cdata, *tmp = fd->buf;
    size_t csize, bsize;
    ssize_t res;

    if(off >= (loff_t)le64_to_cpu(fd->hdr.size))
        return 0;
    if(size > le64_to_cpu(fd->hdr.size) - (size_t)off)
        size = le64_to_cpu(fd->hdr.size) - off;

    block = off / APFS_COMPRESS_BLOCK;
    off -= block * APFS_COMPRESS_BLOCK;
    if(block != fd->bufblk) {
        doffs = compress->data_offs;

        if(block >= compress->block_num)
            return 0;

        if(rsrc_has_classic_header(le32_to_cpu(fd->hdr.algo))) {
            struct apfs_compress_rsrc_data *cd = (void *)((char *)fd->data + doffs);

            bsize = le64_to_cpu(fd->hdr.size) - block * APFS_COMPRESS_BLOCK;
            if(bsize > APFS_COMPRESS_BLOCK)
                bsize = APFS_COMPRESS_BLOCK;

            csize = le32_to_cpu(cd->block[block].size);
            coffs = le32_to_cpu(cd->block[block].offs) + 4;
        } else {
            __le32 *block_offs = (void *)((char *)fd->data + doffs);

            bsize = le64_to_cpu(fd->hdr.size) - block * APFS_COMPRESS_BLOCK;
            if(bsize > APFS_COMPRESS_BLOCK)
                bsize = APFS_COMPRESS_BLOCK;

            coffs = le32_to_cpu(block_offs[block]);
            csize = le32_to_cpu(block_offs[block + 1]) - coffs;
        }
        if(coffs >= fd->size - doffs || fd->size - doffs - coffs < (loff_t)csize || csize > APFS_COMPRESS_BLOCK + 1)
            report("Resource compressed file", "invalid block size or position.");
        cdata = (void *)((char *)fd->data + doffs + coffs);

        switch(le32_to_cpu(fd->hdr.algo)) {
        case APFS_COMPRESS_ZLIB_RSRC:
            if(cdata[0] == 0x78 && csize >= 2) {
                res = zlib_inflate_blob(tmp, bsize, cdata + 2, csize - 2);
                if(res < 0)
                    report("Resource compressed file", "invalid compression.");
                bsize = res;
            } else if((cdata[0] & 0x0F) == 0x0F) {
                memcpy(tmp, &cdata[1], csize - 1);
                bsize = csize - 1;
            } else {
                report("Resource compressed file", "invalid header for zlib.");
            }
            break;
        case APFS_COMPRESS_LZBITMAP_RSRC:
            if(cdata[0] == 0x5a) {
                res = zbm_decompress(tmp, bsize, cdata, csize, &bsize);
                if(res < 0)
                    report("LZBITMAP compressed file", "invalid compression.");
            } else if((cdata[0] & 0x0F) == 0x0F) {
                memcpy(tmp, &cdata[1], csize - 1);
                bsize = csize - 1;
            } else {
                report("LZBITMAP compressed file", "invalid header.");
            }
            break;
        case APFS_COMPRESS_PLAIN_RSRC:
            memcpy(tmp, &cdata[1], csize - 1);
            bsize = csize - 1;
            break;
        case APFS_COMPRESS_LZVN_RSRC:	/* winapfs */
        case APFS_COMPRESS_LZFSE_RSRC:
            res = decode_lz_chunk(le32_to_cpu(fd->hdr.algo), tmp, bsize, cdata, csize);
            if(res < 0)
                report("Resource compressed file", "invalid compression.");
            bsize = res;
            break;
        default:
            return -EINVAL;
        }
        fd->bufblk = block;
        fd->bufsize = bsize;
    } else
        bsize = fd->bufsize;

    if (block != compress->block_num - 1 && bsize != APFS_COMPRESS_BLOCK)
        report("Resource compressed file", "wrong size for uncompressed block.");

    if(bsize < (size_t)off)
        return 0;
    bsize -= off;
    if(size > bsize)
        size = bsize;
    if(buf)
        memcpy(buf, tmp + off, size);
    return size;
}

ssize_t apfs_compress_read(struct compress *compress, char *buf, size_t size, loff_t *off)
{
    struct apfs_compress_file_data *fd = compress->compress_data;
    loff_t step;
    ssize_t block, res;

    if(apfs_compressed_in_dstream(compress)) {
        step = 0;
        while(!buf || step < (int64_t)size) {
            block = APFS_COMPRESS_BLOCK - ((*off + step) & (APFS_COMPRESS_BLOCK - 1));
            if(buf && block > (int64_t)size - step)
                block = size - step;
            res = apfs_compress_read_block(compress, buf ? buf + step : NULL, block, *off + step);
            if(res < block) {
                step += res > 0 ? res : 0;
                break;
            }
            step += block;
        }
        *off += step;
        return step;
    } else {
        if(!buf)
            return fd->size;
        if(*off >= fd->size)
            return 0;
        if((int64_t)size > fd->size - *off)
            size = fd->size - *off;
        memcpy(buf, (void *)((char *)fd->data + *off), size);
        *off += size;
        return size;
    }
}

void apfs_compress_check(struct compress *compress)
{
    ssize_t size;
    loff_t off = 0;
    u64 block_num;

    /* Inline compression was already checked on open */
    if (!compress->rsrc_dstream)
        return;

    size = apfs_compress_read(compress, NULL, 0, &off);
    if(compress->size != size)
        report("Resource compressed file", "wrong reported length.");

    block_num = DIV_ROUND_UP(compress->size, APFS_COMPRESS_BLOCK);
    if(block_num != compress->block_num)
        report("Resource compressed file", "inconsistent block count.");
}

void apfs_compress_close(struct compress *compress)
{
    struct apfs_compress_file_data *fd = compress->compress_data;

    if (!fd)
        return;

    if(fd->data)
        free(fd->data);
    if(fd->buf)
        free(fd->buf);
    free(fd);
    compress->compress_data = NULL;
}
