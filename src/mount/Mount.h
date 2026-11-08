// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Opening volumes for a mount, shared by the command line (main.cpp) and the
// auto-mount service (Service.cpp).

#pragma once

#include <winapfs/Apfs.h>
#include <winapfs/DiskLock.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace winapfs {

// Messages go to stderr unless a sink is set (the service logs to the event log).
// level: 0 error, 1 warning, 2 information
using LogSink = void (*)(int level, const std::string &msg);
void SetLogSink(LogSink sink);
void Log(int level, const char *fmt, ...);

// Locks on Windows volumes held for raw-disk writes, one per partition; the
// owner keeps them for as long as volumes of that partition are mounted.
using DiskLocks = std::vector<std::pair<std::string, std::unique_ptr<DiskWriteLock>>>;

struct OpenOptions
{
	bool rw = false;
	bool allow_disk_writes = false;   // with rw: physical disks too
	bool force = false;               // with rw: ignore an unclean unmount
	std::string engine = "auto";      // auto | rw | lib
	std::string password;
	bool ask = false;                 // prompt on the console for passwords
	DiskLocks *locks = nullptr;       // required for rw on physical disks
};

// Opens a volume for mounting, applying the write-safety refusals. Returns
// null (after logging why) if it can't or mustn't be opened.
std::unique_ptr<Volume> OpenVolume(Container &c, const VolumeInfo &vi, const OpenOptions &o);

// WINAPFS_MOUNT_<volume name, non-alphanumerics as _>
std::string MountEnvName(const std::string &vol);

// "WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos" and
// its link: the WinFsp FLOSS exception asks for it in the UI and docs.
extern const char *const kWinFspNotice;

} // namespace winapfs
