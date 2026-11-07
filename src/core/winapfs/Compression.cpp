// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// decmpfs. The header in com.apple.decmpfs is {'fpmc', algo, uncompressed
// size}. Odd algos keep the compressed data inline after the header; even
// algos keep it in the resource fork, split into 64 KiB blocks:
//   zlib (4):  classic resource-fork layout; at be32 data_offset + 4 there is a
//              LE block table {count, {off, size}[count]} relative to that
//              point.
//   others:    a LE uint32 offset table at the start of the fork; block i is
//              [off[i], off[i+1]).
// A block whose first byte marks "stored" holds raw data after that byte.
// Blocks are decompressed on demand and the last few are cached, so large
// compressed files don't have to fit in memory.

#include "Compression.h"

#include <ApfsLib/ApfsDir.h>
#include <ApfsLib/ApfsVolume.h>
#include <ApfsLib/Decmpfs.h>
#include <ApfsLib/DiskStruct.h>
#include <ApfsLib/Util.h>

#include <cstring>
#include <list>
#include <mutex>

namespace winapfs {

namespace {

constexpr uint32_t kBlock = 0x10000;
constexpr uint32_t kDecmpfsMagic = 0x636d7066;  // 'fpmc' read as LE

enum Algo : uint32_t
{
	ZlibAttr = 3, ZlibRsrc = 4,
	LzvnAttr = 7, LzvnRsrc = 8,
	RawAttr = 9, RawRsrc = 10,
	LzfseAttr = 11, LzfseRsrc = 12,
	LzbitmapAttr = 13, LzbitmapRsrc = 14,
};

// Decompresses one chunk (a whole inline payload or one 64 KiB block).
bool DecodeChunk(uint32_t algo, const uint8_t *src, size_t src_len, uint8_t *dst, size_t want)
{
	if (src_len == 0)
		return want == 0;

	size_t n = 0;
	switch (algo)
	{
	case ZlibAttr:
	case ZlibRsrc:
		if ((src[0] & 0x0F) == 0x0F)
		{
			n = src_len - 1;
			if (n != want)
				return false;
			memcpy(dst, src + 1, n);
			return true;
		}
		n = DecompressZLib(dst, want, src, src_len);
		break;
	case LzvnAttr:
	case LzvnRsrc:
		if (src[0] == 0x06)
		{
			n = src_len - 1;
			if (n != want)
				return false;
			memcpy(dst, src + 1, n);
			return true;
		}
		n = DecompressLZVN(dst, want, src, src_len);
		break;
	case RawAttr:
	case RawRsrc:
		n = src_len - 1;
		if (n != want)
			return false;
		memcpy(dst, src + 1, n);
		return true;
	case LzfseAttr:
	case LzfseRsrc:
		// Apple's stored variant for LZFSE starts with 0xFF; lzfse streams start with "bvx".
		if (src[0] == 0xFF)
		{
			n = src_len - 1;
			if (n != want)
				return false;
			memcpy(dst, src + 1, n);
			return true;
		}
		n = DecompressLZFSE(dst, want, src, src_len);
		break;
	case LzbitmapAttr:
	case LzbitmapRsrc:
		if (src[0] == 0xFF)
		{
			n = src_len - 1;
			if (n != want)
				return false;
			memcpy(dst, src + 1, n);
			return true;
		}
		n = DecompressLZBITMAP(dst, want, src, src_len);
		break;
	default:
		return false;
	}
	return n == want;
}

// Whole file decoded at open: inline (attr) algorithms. These are small (the
// payload has to fit in an xattr record).
class InlineReader : public FileReader
{
public:
	std::vector<uint8_t> data;

	uint64_t Size() const override { return data.size(); }

	bool Read(uint64_t off, void *buf, size_t len, size_t &got) override
	{
		got = 0;
		if (off >= data.size())
			return true;
		got = static_cast<size_t>(std::min<uint64_t>(len, data.size() - off));
		memcpy(buf, data.data() + off, got);
		return true;
	}
};

// Byte source for the resource fork: embedded xattr bytes or a data stream.
struct RsrcSource
{
	Volume *vol = nullptr;
	bool stream = false;
	uint64_t obj_id = 0;
	uint64_t size = 0;
	std::vector<uint8_t> embedded;

	bool Read(uint64_t off, void *buf, size_t len) const
	{
		if (off > size || len > size - off)
			return false;
		if (!stream)
		{
			memcpy(buf, embedded.data() + off, len);
			return true;
		}
		return vol->ReadStream(obj_id, off, buf, len);
	}
};

class RsrcReader : public FileReader
{
public:
	RsrcReader(uint32_t algo, uint64_t size, RsrcSource src, std::vector<std::pair<uint64_t, uint32_t>> blocks)
		: m_algo(algo), m_size(size), m_src(std::move(src)), m_blocks(std::move(blocks))
	{
	}

	uint64_t Size() const override { return m_size; }

	bool Read(uint64_t off, void *buf, size_t len, size_t &got) override
	{
		uint8_t *out = reinterpret_cast<uint8_t *>(buf);
		got = 0;
		if (off >= m_size)
			return true;
		if (len > m_size - off)
			len = static_cast<size_t>(m_size - off);

		while (len > 0)
		{
			uint64_t idx = off / kBlock;
			uint32_t in_blk = static_cast<uint32_t>(off % kBlock);
			std::shared_ptr<std::vector<uint8_t>> blk = GetBlock(idx);
			if (!blk)
				return false;
			size_t n = std::min<size_t>(len, blk->size() - in_blk);
			if (n == 0)
				return false;
			memcpy(out, blk->data() + in_blk, n);
			out += n;
			off += n;
			len -= n;
			got += n;
		}
		return true;
	}

private:
	std::shared_ptr<std::vector<uint8_t>> GetBlock(uint64_t idx)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
				if (it->first == idx)
				{
					m_cache.splice(m_cache.begin(), m_cache, it);
					return it->second;
				}
		}

		if (idx >= m_blocks.size())
			return nullptr;
		size_t want = static_cast<size_t>(std::min<uint64_t>(kBlock, m_size - idx * kBlock));
		const auto &b = m_blocks[idx];
		if (b.second > kBlock + 1)
			return nullptr;
		std::vector<uint8_t> src(b.second);
		if (!m_src.Read(b.first, src.data(), src.size()))
			return nullptr;
		auto blk = std::make_shared<std::vector<uint8_t>>(want);
		if (!DecodeChunk(m_algo, src.data(), src.size(), blk->data(), want))
			return nullptr;

		std::lock_guard<std::mutex> lock(m_mutex);
		m_cache.emplace_front(idx, blk);
		if (m_cache.size() > 8)
			m_cache.pop_back();
		return blk;
	}

	uint32_t m_algo;
	uint64_t m_size;
	RsrcSource m_src;
	std::vector<std::pair<uint64_t, uint32_t>> m_blocks;  // (offset in fork, compressed length)
	std::mutex m_mutex;
	std::list<std::pair<uint64_t, std::shared_ptr<std::vector<uint8_t>>>> m_cache;
};

uint32_t Le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint32_t Be32(const uint8_t *p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

} // namespace

std::unique_ptr<FileReader> OpenCompressedFile(Volume &vol, const Stat &st, std::string &err)
{
	std::vector<uint8_t> hdr;
	if (!vol.GetXattr(st.inode, "com.apple.decmpfs", hdr) || hdr.size() < sizeof(CompressionHeader))
	{
		err = "compressed file without com.apple.decmpfs";
		return nullptr;
	}
	const CompressionHeader *h = reinterpret_cast<const CompressionHeader *>(hdr.data());
	if (h->signature != kDecmpfsMagic)
	{
		err = "bad decmpfs signature";
		return nullptr;
	}
	uint32_t algo = h->algo;
	uint64_t size = h->size;

	if (!IsDecompAlgoSupported(static_cast<uint16_t>(algo)))
	{
		err = "unsupported decmpfs algorithm " + std::to_string(algo);
		return nullptr;
	}

	if (!IsDecompAlgoInRsrc(static_cast<uint16_t>(algo)))
	{
		std::unique_ptr<InlineReader> r(new InlineReader());
		r->data.resize(static_cast<size_t>(size));
		const uint8_t *src = hdr.data() + sizeof(CompressionHeader);
		size_t src_len = hdr.size() - sizeof(CompressionHeader);
		if (size > 0 && !DecodeChunk(algo, src, src_len, r->data.data(), r->data.size()))
		{
			err = "decmpfs: inline data does not decode";
			return nullptr;
		}
		return r;
	}

	// Resource fork
	RsrcSource src;
	src.vol = &vol;
	{
		ApfsDir dir(vol.Raw());
		ApfsDir::XAttr xa;
		if (!dir.GetAttributeInfo(xa, st.inode, "com.apple.ResourceFork"))
		{
			err = "compressed file without resource fork";
			return nullptr;
		}
		if (xa.flags & XATTR_DATA_STREAM)
		{
			src.stream = true;
			src.obj_id = xa.xstrm.xattr_obj_id;
			src.size = xa.xstrm.dstream.size;
		}
		else
		{
			if (!dir.GetAttribute(src.embedded, st.inode, "com.apple.ResourceFork"))
			{
				err = "cannot read resource fork";
				return nullptr;
			}
			src.size = src.embedded.size();
		}
	}

	uint64_t nblocks = (size + kBlock - 1) / kBlock;
	std::vector<std::pair<uint64_t, uint32_t>> blocks;
	blocks.reserve(static_cast<size_t>(nblocks));

	if (algo == ZlibRsrc)
	{
		uint8_t rh[16];
		if (!src.Read(0, rh, sizeof(rh)))
		{
			err = "short resource fork";
			return nullptr;
		}
		uint64_t base = static_cast<uint64_t>(Be32(rh)) + 4;
		uint8_t cnt_raw[4];
		if (!src.Read(base, cnt_raw, 4))
		{
			err = "short resource fork";
			return nullptr;
		}
		uint32_t count = Le32(cnt_raw);
		if (count < nblocks)
		{
			err = "decmpfs: resource fork has too few blocks";
			return nullptr;
		}
		std::vector<uint8_t> table(static_cast<size_t>(count) * 8);
		if (!src.Read(base + 4, table.data(), table.size()))
		{
			err = "short resource fork block table";
			return nullptr;
		}
		for (uint64_t i = 0; i < nblocks; i++)
			blocks.emplace_back(base + Le32(&table[i * 8]), Le32(&table[i * 8 + 4]));
	}
	else
	{
		std::vector<uint8_t> table(static_cast<size_t>(nblocks + 1) * 4);
		if (!src.Read(0, table.data(), table.size()))
		{
			err = "short resource fork offset table";
			return nullptr;
		}
		for (uint64_t i = 0; i < nblocks; i++)
		{
			uint32_t a = Le32(&table[i * 4]), b = Le32(&table[(i + 1) * 4]);
			if (b < a)
			{
				err = "decmpfs: bad offset table";
				return nullptr;
			}
			blocks.emplace_back(a, b - a);
		}
	}

	return std::unique_ptr<FileReader>(new RsrcReader(algo, size, std::move(src), std::move(blocks)));
}

} // namespace winapfs
