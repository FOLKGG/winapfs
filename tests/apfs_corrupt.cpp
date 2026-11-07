// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// apfs-corrupt: damage a copy of a bare APFS container in a controlled way,
// so the tests can check that winapfs-fsck notices.
//
//   apfs-corrupt <in.img> <out.img> omap-flip       flip a byte in the omap B-tree root
//   apfs-corrupt <in.img> <out.img> omap-misdirect  point the first omap record at the
//                                                   wrong block, with a valid checksum
//
// Offsets are from Apple's "Apple File System Reference" (nx_superblock_t,
// omap_phys_t, btree_node_phys_t, omap_val_t).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kNxMagic = 0x4253584E;  // 'NXSB'

uint64_t Fletcher64(const uint8_t *block, size_t size)
{
	uint64_t sum1 = 0, sum2 = 0;
	for (size_t i = 8; i < size; i += 4)
	{
		uint32_t w;
		memcpy(&w, block + i, 4);
		sum1 = (sum1 + w) % 0xFFFFFFFF;
		sum2 = (sum2 + sum1) % 0xFFFFFFFF;
	}
	uint64_t c1 = 0xFFFFFFFF - ((sum1 + sum2) % 0xFFFFFFFF);
	uint64_t c2 = 0xFFFFFFFF - ((sum1 + c1) % 0xFFFFFFFF);
	return (c2 << 32) | c1;
}

template <typename T> T Get(const std::vector<uint8_t> &b, size_t off)
{
	T v;
	memcpy(&v, b.data() + off, sizeof(T));
	return v;
}

template <typename T> void Put(std::vector<uint8_t> &b, size_t off, T v)
{
	memcpy(b.data() + off, &v, sizeof(T));
}

} // namespace

int main(int argc, char **argv)
{
	if (argc != 4)
	{
		fprintf(stderr, "usage: apfs-corrupt <in.img> <out.img> omap-flip|omap-misdirect\n");
		return 2;
	}
	std::ifstream in(argv[1], std::ios::binary);
	std::vector<uint8_t> img((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (img.size() < 4096 || Get<uint32_t>(img, 32) != kNxMagic)
	{
		fprintf(stderr, "%s: not a bare APFS container\n", argv[1]);
		return 2;
	}
	const uint32_t bs = Get<uint32_t>(img, 36);  // nx_block_size
	auto block = [&](uint64_t n) { return n * bs; };

	// latest valid checkpoint superblock in the descriptor area
	uint64_t desc_base = Get<uint64_t>(img, 112);   // nx_xp_desc_base
	uint32_t desc_blocks = Get<uint32_t>(img, 104); // nx_xp_desc_blocks
	uint64_t best = 0, best_xid = 0;
	for (uint64_t b = desc_base; b < desc_base + desc_blocks; b++)
	{
		size_t o = block(b);
		std::vector<uint8_t> blk(img.begin() + o, img.begin() + o + bs);
		if (Get<uint32_t>(blk, 32) != kNxMagic || Get<uint64_t>(blk, 0) != Fletcher64(blk.data(), bs))
			continue;
		uint64_t xid = Get<uint64_t>(blk, 16);  // o_xid
		if (xid > best_xid)
		{
			best_xid = xid;
			best = b;
		}
	}
	if (!best)
	{
		fprintf(stderr, "no checkpoint superblock\n");
		return 1;
	}
	uint64_t omap = Get<uint64_t>(img, block(best) + 160);       // nx_omap_oid (physical)
	uint64_t omap_root = Get<uint64_t>(img, block(omap) + 48);   // om_tree_oid (physical)
	size_t root = block(omap_root);
	printf("checkpoint sb at block %llu (xid %llu), omap %llu, omap root %llu\n", (unsigned long long)best,
		(unsigned long long)best_xid, (unsigned long long)omap, (unsigned long long)omap_root);

	std::string mode = argv[3];
	if (mode == "omap-flip")
	{
		img[root + bs / 2] ^= 0x5A;
	}
	else if (mode == "omap-misdirect")
	{
		// btree_node_phys_t: btn_flags u16 @32, btn_level @34, btn_nkeys u32 @36,
		// btn_table_space {off u16, len u16} @40, data @56. A leaf root of the
		// omap has fixed-size kvoff_t entries {k u16, v u16} in the toc; values
		// are counted back from the end of the block, before the 40-byte
		// btree_info_t of a root node. omap_val_t: ov_flags u32, ov_size u32, ov_paddr u64.
		uint16_t flags = Get<uint16_t>(img, root + 32);
		uint16_t level = Get<uint16_t>(img, root + 34);
		if (level != 0 || !(flags & 0x0004))  // BTNODE_FIXED_KV_SIZE
		{
			fprintf(stderr, "omap root is not a fixed-size leaf (level %u flags 0x%x)\n", level, flags);
			return 1;
		}
		uint16_t toc_off = Get<uint16_t>(img, root + 40);
		size_t toc = root + 56 + toc_off;
		uint16_t v = Get<uint16_t>(img, toc + 2);
		size_t val = root + bs - 40 - v;
		uint64_t paddr = Get<uint64_t>(img, val + 8);
		Put<uint64_t>(img, val + 8, paddr + 1);
		std::vector<uint8_t> blk(img.begin() + root, img.begin() + root + bs);
		Put<uint64_t>(img, root, Fletcher64(blk.data(), bs));
		printf("first omap record: paddr %llu -> %llu, checksum fixed\n", (unsigned long long)paddr, (unsigned long long)paddr + 1);
	}
	else
	{
		fprintf(stderr, "unknown mode %s\n", mode.c_str());
		return 2;
	}

	std::ofstream out(argv[2], std::ios::binary);
	out.write(reinterpret_cast<const char *>(img.data()), img.size());
	return out ? 0 : 1;
}
