/*
This file is part of apfs-fuse, a read-only implementation of APFS
(Apple File System) for FUSE.
Copyright (C) 2017 Simon Gander

Apfs-fuse is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

Apfs-fuse is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with apfs-fuse.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#ifdef _WIN32

#include "Device.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// winapfs: raw disk (\\.\PhysicalDriveN, or any \\.\ device path).
// Raw disk I/O must be sector-aligned in offset, length and buffer address,
// and it bypasses the Windows file cache. So small reads go through an LRU
// cache of aligned 128 KiB chunks (64 MiB), and large reads go straight to the
// device through an aligned bounce buffer. The handle is overlapped so reads
// from several threads run concurrently instead of being serialized on the
// file object.
class DeviceWinPhys : public Device
{
public:
	DeviceWinPhys();
	~DeviceWinPhys();

	bool Open(const char *name) override;
	void Close() override;

	bool Read(void *data, uint64_t offs, uint64_t len) override;
	uint64_t GetSize() const override;

	// Physical sector size reported by the device (I/O alignment unit).
	uint32_t GetIoAlignment() const { return m_io_align; }

private:
	typedef std::shared_ptr<std::vector<uint8_t>> Chunk;

	bool ReadAligned(uint8_t *aligned_buf, uint64_t offs, uint32_t len, uint32_t &got);
	bool ReadDirect(uint8_t *dst, uint64_t offs, uint64_t len);
	Chunk GetChunk(uint64_t idx);

	HANDLE m_drive;
	uint64_t m_size;
	uint32_t m_io_align;

	std::mutex m_cache_mutex;
	std::list<std::pair<uint64_t, Chunk>> m_lru;
	std::unordered_map<uint64_t, std::list<std::pair<uint64_t, Chunk>>::iterator> m_cache;
};

#endif
