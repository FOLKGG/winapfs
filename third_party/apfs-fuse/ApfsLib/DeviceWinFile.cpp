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

#include <vector>

#include "DeviceWinFile.h"

DeviceWinFile::DeviceWinFile()
{
	m_file = INVALID_HANDLE_VALUE;
	m_size = 0;
}

DeviceWinFile::~DeviceWinFile()
{
	Close();
}

bool DeviceWinFile::Open(const char * name)
{
	int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);
	if (wlen <= 0)
		return false;
	std::vector<wchar_t> wname(wlen);
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wname.data(), wlen);

	m_file = CreateFileW(wname.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
	if (m_file == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER size;
	if (!GetFileSizeEx(m_file, &size))
	{
		Close();
		return false;
	}
	m_size = static_cast<uint64_t>(size.QuadPart);

	return true;
}

void DeviceWinFile::Close()
{
	if (m_file != INVALID_HANDLE_VALUE)
		CloseHandle(m_file);
	m_file = INVALID_HANDLE_VALUE;
	m_size = 0;
}

bool DeviceWinFile::Read(void *data, uint64_t offs, uint64_t len)
{
	uint8_t *p = reinterpret_cast<uint8_t *>(data);

	if (m_file == INVALID_HANDLE_VALUE)
		return false;

	while (len > 0)
	{
		DWORD chunk = len > 0x40000000 ? 0x40000000 : static_cast<DWORD>(len);
		DWORD nread = 0;
		OVERLAPPED ov = {};
		ov.Offset = static_cast<DWORD>(offs);
		ov.OffsetHigh = static_cast<DWORD>(offs >> 32);

		if (!ReadFile(m_file, p, chunk, &nread, &ov) || nread != chunk)
			return false;

		p += nread;
		offs += nread;
		len -= nread;
	}

	return true;
}

#endif
