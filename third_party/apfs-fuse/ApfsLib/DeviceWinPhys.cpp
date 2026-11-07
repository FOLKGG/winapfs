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

#ifdef _WIN32

#include <algorithm>
#include <cstring>

#include "DeviceWinPhys.h"

#include <winioctl.h>

namespace {

constexpr uint32_t kChunk = 128 * 1024;           // cache unit (multiple of any sector size)
constexpr size_t kMaxChunks = 512;                // 64 MiB
constexpr uint64_t kDirectThreshold = 512 * 1024; // larger reads bypass the cache
constexpr uint32_t kMaxIo = 1 << 20;

struct AlignedBuf
{
	uint8_t *p;
	explicit AlignedBuf(size_t n) { p = static_cast<uint8_t *>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); }
	~AlignedBuf() { if (p) VirtualFree(p, 0, MEM_RELEASE); }
};

// One manual-reset event per thread for overlapped reads.
HANDLE ThreadEvent()
{
	thread_local struct Ev
	{
		HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		~Ev() { if (h) CloseHandle(h); }
	} ev;
	return ev.h;
}

} // namespace

DeviceWinPhys::DeviceWinPhys()
{
	m_drive = INVALID_HANDLE_VALUE;
	m_size = 0;
	m_io_align = 512;
}

DeviceWinPhys::~DeviceWinPhys()
{
	Close();
}

bool DeviceWinPhys::Open(const char *name)
{
	DWORD bytes_ret = 0;

	int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);
	if (wlen <= 0)
		return false;
	std::vector<wchar_t> wname(wlen);
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wname.data(), wlen);

	m_drive = CreateFileW(wname.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
	if (m_drive == INVALID_HANDLE_VALUE)
		return false;

	GET_LENGTH_INFORMATION len_info = {};
	if (DeviceIoControl(m_drive, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len_info, sizeof(len_info), &bytes_ret, nullptr))
	{
		m_size = static_cast<uint64_t>(len_info.Length.QuadPart);
	}
	else
	{
		uint8_t buf[0x1000];
		LARGE_INTEGER fsize;
		if (DeviceIoControl(m_drive, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, buf, sizeof(buf), &bytes_ret, nullptr))
			m_size = reinterpret_cast<const DISK_GEOMETRY_EX *>(buf)->DiskSize.QuadPart;
		else if (GetFileSizeEx(m_drive, &fsize))
			m_size = static_cast<uint64_t>(fsize.QuadPart);  // \\.\C:\path\file.img (used by the tests)
		else
		{
			DWORD err = GetLastError();
			Close();
			SetLastError(err);
			return false;
		}
	}

	DISK_GEOMETRY geom = {};
	if (DeviceIoControl(m_drive, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0, &geom, sizeof(geom), &bytes_ret, nullptr) && geom.BytesPerSector >= 512
		&& geom.BytesPerSector <= kChunk && (geom.BytesPerSector & (geom.BytesPerSector - 1)) == 0)
	{
		m_io_align = geom.BytesPerSector;
		// GPT LBAs are in logical sectors.
		SetSectorSize(geom.BytesPerSector);
	}

	return true;
}

void DeviceWinPhys::Close()
{
	if (m_drive != INVALID_HANDLE_VALUE)
		CloseHandle(m_drive);
	m_drive = INVALID_HANDLE_VALUE;
	m_size = 0;
	std::lock_guard<std::mutex> lock(m_cache_mutex);
	m_cache.clear();
	m_lru.clear();
}

// offs and len aligned, aligned_buf page-aligned. got may be short at the end of the device.
bool DeviceWinPhys::ReadAligned(uint8_t *aligned_buf, uint64_t offs, uint32_t len, uint32_t &got)
{
	OVERLAPPED ov = {};
	ov.Offset = static_cast<DWORD>(offs);
	ov.OffsetHigh = static_cast<DWORD>(offs >> 32);
	ov.hEvent = ThreadEvent();
	got = 0;

	DWORD n = 0;
	if (!ReadFile(m_drive, aligned_buf, len, nullptr, &ov))
	{
		if (GetLastError() != ERROR_IO_PENDING)
			return false;
	}
	if (!GetOverlappedResult(m_drive, &ov, &n, TRUE))
		return false;
	got = n;
	return true;
}

bool DeviceWinPhys::ReadDirect(uint8_t *dst, uint64_t offs, uint64_t len)
{
	const uint64_t align = m_io_align;
	AlignedBuf bounce(kMaxIo + align);
	if (!bounce.p)
		return false;

	while (len > 0)
	{
		uint64_t a_offs = offs & ~(align - 1);
		uint64_t skip = offs - a_offs;
		uint64_t want = std::min<uint64_t>(len + skip, kMaxIo);
		uint32_t a_len = static_cast<uint32_t>((want + align - 1) & ~(align - 1));
		uint32_t got = 0;

		if (!ReadAligned(bounce.p, a_offs, a_len, got) || got < want)
			return false;

		uint64_t n = want - skip;
		memcpy(dst, bounce.p + skip, static_cast<size_t>(n));
		dst += n;
		offs += n;
		len -= n;
	}
	return true;
}

DeviceWinPhys::Chunk DeviceWinPhys::GetChunk(uint64_t idx)
{
	{
		std::lock_guard<std::mutex> lock(m_cache_mutex);
		auto it = m_cache.find(idx);
		if (it != m_cache.end())
		{
			m_lru.splice(m_lru.begin(), m_lru, it->second);
			return it->second->second;
		}
	}

	// Read outside the lock; two threads may read the same chunk, which is harmless.
	uint64_t offs = idx * kChunk;
	uint64_t len = std::min<uint64_t>(kChunk, m_size - offs);
	uint32_t a_len = static_cast<uint32_t>((len + m_io_align - 1) & ~static_cast<uint64_t>(m_io_align - 1));
	AlignedBuf buf(kChunk);
	uint32_t got = 0;
	if (!buf.p || !ReadAligned(buf.p, offs, a_len, got) || got < len)
		return nullptr;
	Chunk c = std::make_shared<std::vector<uint8_t>>(buf.p, buf.p + len);

	std::lock_guard<std::mutex> lock(m_cache_mutex);
	auto it = m_cache.find(idx);
	if (it != m_cache.end())
		return it->second->second;
	m_lru.emplace_front(idx, c);
	m_cache[idx] = m_lru.begin();
	if (m_lru.size() > kMaxChunks)
	{
		m_cache.erase(m_lru.back().first);
		m_lru.pop_back();
	}
	return c;
}

bool DeviceWinPhys::Read(void * data, uint64_t offs, uint64_t len)
{
	uint8_t *dst = reinterpret_cast<uint8_t *>(data);

	if (m_drive == INVALID_HANDLE_VALUE)
		return false;
	if (offs > m_size || len > m_size - offs)
		return false;

	if (len >= kDirectThreshold)
		return ReadDirect(dst, offs, len);

	while (len > 0)
	{
		Chunk c = GetChunk(offs / kChunk);
		if (!c)
			return false;
		size_t in = static_cast<size_t>(offs % kChunk);
		if (in >= c->size())
			return false;
		size_t n = static_cast<size_t>(std::min<uint64_t>(len, c->size() - in));
		memcpy(dst, c->data() + in, n);
		dst += n;
		offs += n;
		len -= n;
	}
	return true;
}

uint64_t DeviceWinPhys::GetSize() const
{
	return m_size;
}

#endif
