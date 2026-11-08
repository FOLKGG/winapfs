// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiskLock.h"

#include <Windows.h>
#include <winioctl.h>

#include <cwchar>

namespace winapfs {

DiskWriteLock::~DiskWriteLock()
{
	for (void *h : m_handles)
	{
		DWORD got = 0;
		DeviceIoControl(static_cast<HANDLE>(h), FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
		CloseHandle(static_cast<HANDLE>(h));
	}
}

static bool DiskNumber(const std::string &drive, DWORD &n)
{
	const char *prefix = "\\\\.\\PhysicalDrive";
	size_t len = strlen(prefix);
	if (drive.size() <= len || _strnicmp(drive.c_str(), prefix, len) != 0)
		return false;
	char *end = nullptr;
	unsigned long v = strtoul(drive.c_str() + len, &end, 10);
	if (!end || *end)
		return false;
	n = static_cast<DWORD>(v);
	return true;
}

std::unique_ptr<DiskWriteLock> DiskWriteLock::Acquire(const std::string &drive, uint64_t offset, uint64_t size, std::string &err)
{
	std::unique_ptr<DiskWriteLock> lock(new DiskWriteLock());
	DWORD disk;
	if (!DiskNumber(drive, disk))
		return lock;   // an image file or another raw path: nothing of Windows' to lock

	wchar_t name[MAX_PATH];
	HANDLE find = FindFirstVolumeW(name, MAX_PATH);
	if (find == INVALID_HANDLE_VALUE)
		return lock;
	do
	{
		// \\?\Volume{guid}\ -> open without the trailing backslash
		std::wstring vol = name;
		if (!vol.empty() && vol.back() == L'\\')
			vol.pop_back();
		HANDLE h = CreateFileW(vol.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			continue;
		alignas(8) BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT)];
		DWORD got = 0;
		bool overlaps = false;
		if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof(buf), &got, nullptr))
		{
			const VOLUME_DISK_EXTENTS *e = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(buf);
			for (DWORD i = 0; i < e->NumberOfDiskExtents; i++)
			{
				const DISK_EXTENT &x = e->Extents[i];
				uint64_t s = static_cast<uint64_t>(x.StartingOffset.QuadPart), l = static_cast<uint64_t>(x.ExtentLength.QuadPart);
				if (x.DiskNumber == disk && s < offset + size && offset < s + l)
					overlaps = true;
			}
		}
		if (!overlaps)
		{
			CloseHandle(h);
			continue;
		}
		// a RAW volume should lock at once; something holding it open would fail
		if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr))
		{
			err = "cannot lock Windows volume " + std::string(vol.begin(), vol.end()) +
				" on that partition (error " + std::to_string(GetLastError()) + "); close programs using it";
			CloseHandle(h);
			FindVolumeClose(find);
			return nullptr;
		}
		DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
		lock->m_handles.push_back(h);
	} while (FindNextVolumeW(find, name, MAX_PATH));
	FindVolumeClose(find);
	return lock;
}

} // namespace winapfs
