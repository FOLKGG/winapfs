// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Gpt.h"

#include <ApfsLib/Device.h>

#include <zlib.h>

#include <cstring>

namespace winapfs {

namespace {

#pragma pack(push, 1)
struct GptHeader
{
	char signature[8];          // "EFI PART"
	uint32_t revision;
	uint32_t header_size;
	uint32_t header_crc32;
	uint32_t reserved;
	uint64_t my_lba;
	uint64_t alternate_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	uint8_t disk_guid[16];
	uint64_t entries_lba;
	uint32_t num_entries;
	uint32_t entry_size;
	uint32_t entries_crc32;
};

struct GptEntry
{
	uint8_t type_guid[16];
	uint8_t part_guid[16];
	uint64_t first_lba;
	uint64_t last_lba;
	uint64_t attributes;
	uint16_t name[36];
};
#pragma pack(pop)

static_assert(sizeof(GptHeader) == 92, "GPT header layout");
static_assert(sizeof(GptEntry) == 128, "GPT entry layout");

// 7C3457EF-0000-11AA-AA11-00306543ECAC in on-disk (mixed-endian) order
const uint8_t kApfsType[16] = { 0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11, 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC };

bool LoadHeader(Device &dev, uint64_t lba, uint32_t sector, GptHeader &hdr)
{
	std::vector<uint8_t> buf(sector);
	if ((lba + 1) * sector > dev.GetSize())
		return false;
	if (!dev.Read(buf.data(), lba * sector, sector))
		return false;
	memcpy(&hdr, buf.data(), sizeof(hdr));
	if (memcmp(hdr.signature, "EFI PART", 8) != 0)
		return false;
	if (hdr.header_size < sizeof(GptHeader) || hdr.header_size > sector)
		return false;
	uint32_t want = hdr.header_crc32;
	reinterpret_cast<GptHeader *>(buf.data())->header_crc32 = 0;
	if (crc32(0, buf.data(), hdr.header_size) != want)
		return false;
	return hdr.entry_size >= sizeof(GptEntry) && hdr.num_entries <= 4096;
}

bool LoadEntries(Device &dev, const GptHeader &hdr, uint32_t sector, std::vector<PartitionInfo> &parts)
{
	std::vector<uint8_t> ents(static_cast<size_t>(hdr.num_entries) * hdr.entry_size);
	if (!dev.Read(ents.data(), hdr.entries_lba * sector, ents.size()))
		return false;
	if (crc32(0, ents.data(), static_cast<uInt>(ents.size())) != hdr.entries_crc32)
		return false;

	for (uint32_t i = 0; i < hdr.num_entries; i++)
	{
		const GptEntry *e = reinterpret_cast<const GptEntry *>(ents.data() + static_cast<size_t>(i) * hdr.entry_size);
		if (memcmp(e->type_guid, kApfsType, 16) != 0)
			continue;
		if (e->last_lba < e->first_lba)
			continue;
		PartitionInfo p;
		p.index = static_cast<int>(i);
		p.offset = e->first_lba * sector;
		p.size = (e->last_lba - e->first_lba + 1) * sector;
		std::wstring name;
		for (int k = 0; k < 36 && e->name[k]; k++)
			name.push_back(static_cast<wchar_t>(e->name[k]));
		p.name = WideToUtf8(name);
		parts.push_back(p);
	}
	return true;
}

} // namespace

bool ReadGptApfsPartitions(Device &dev, std::vector<PartitionInfo> &parts)
{
	parts.clear();

	uint32_t sectors[2] = { dev.GetSectorSize(), dev.GetSectorSize() == 512 ? 4096u : 512u };
	for (uint32_t sector : sectors)
	{
		GptHeader hdr;
		if (LoadHeader(dev, 1, sector, hdr) && LoadEntries(dev, hdr, sector, parts))
			return true;
		uint64_t last = dev.GetSize() / sector;
		if (last > 1 && LoadHeader(dev, last - 1, sector, hdr) && LoadEntries(dev, hdr, sector, parts))
			return true;
		parts.clear();
	}
	return false;
}

} // namespace winapfs
