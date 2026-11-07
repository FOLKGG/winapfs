// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// WinFsp file system for one APFS volume (native WinFsp API; read-only, or
// read-write when the volume is writable).

#pragma once

#include <winapfs/Apfs.h>

#include <winfsp/winfsp.h>

#include <mutex>
#include <string>
#include <unordered_map>

namespace winapfs {

struct MountOptions
{
	bool debug = false;
	bool hide_dotfiles = false;   // mark names starting with '.' FILE_ATTRIBUTE_HIDDEN
};

class ApfsFs
{
public:
	ApfsFs(std::unique_ptr<Volume> vol, const MountOptions &opts);
	~ApfsFs();

	// mount_point: "X:" or a directory path, or nullptr to let WinFsp pick a
	// free drive letter.
	NTSTATUS Start(const wchar_t *mount_point);
	void Stop();
	std::wstring MountPoint() const;

	Volume &Vol() { return *m_vol; }

	// Shared with the WinFsp callbacks in ApfsFs.cpp.
	struct Node;
	NTSTATUS Resolve(const wchar_t *path, Stat &st, std::wstring *normalized, std::string *stream);
	// Parent directory inode and last component (APFS bytes) of a path.
	NTSTATUS ResolveParent(const wchar_t *path, uint64_t &parent, std::string &name, std::wstring *parent_normalized);
	// Forget cached path -> inode mappings (after a rename or delete).
	void InvalidateCache();
	void FillFileInfo(const Stat &st, const std::string &name, FSP_FSCTL_FILE_INFO &fi);
	NTSTATUS CopySecurity(PSECURITY_DESCRIPTOR sd, SIZE_T *size);
	uint32_t Attributes(const Stat &st, const std::string &name);

	const MountOptions &Options() const { return m_opts; }
	FSP_FILE_SYSTEM *FileSystem() { return m_fs; }

private:
	bool CacheGet(const std::wstring &dir, uint64_t &inode, std::wstring &normalized);
	void CachePut(const std::wstring &dir, uint64_t inode, const std::wstring &normalized);

	std::unique_ptr<Volume> m_vol;
	MountOptions m_opts;
	FSP_FILE_SYSTEM *m_fs = nullptr;
	PSECURITY_DESCRIPTOR m_sd = nullptr;
	DWORD m_sd_size = 0;
	uint32_t m_block_size = 4096;

	// Directory path -> (inode, normalized path). Only directories are cached
	// (the hot part of path resolution); entries never go stale on a read-only
	// volume.
	std::mutex m_cache_mutex;
	std::unordered_map<std::wstring, std::pair<uint64_t, std::wstring>> m_dir_cache;
};

// Loads winfsp-x64.dll from the WinFsp install directory (delay-loaded).
bool LoadWinFsp();

} // namespace winapfs
