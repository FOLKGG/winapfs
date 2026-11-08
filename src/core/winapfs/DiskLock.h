// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Raw-disk writes: Windows creates a volume (RAW, it has no APFS driver) for
// the APFS partition. Writes through \\.\PhysicalDriveN into a volume's
// extents are only allowed while that volume is locked or dismounted, and
// nothing else (a format prompt, a scanner) may use it meanwhile. The lock
// holds every Windows volume overlapping the partition until destroyed.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace winapfs {

class DiskWriteLock
{
public:
	~DiskWriteLock();
	size_t Volumes() const { return m_handles.size(); }

	// drive: \\.\PhysicalDriveN; [offset, offset + size) is the partition.
	// Paths that aren't physical drives need no lock (an empty lock).
	static std::unique_ptr<DiskWriteLock> Acquire(const std::string &drive, uint64_t offset, uint64_t size, std::string &err);

private:
	DiskWriteLock() = default;
	std::vector<void *> m_handles;   // HANDLEs of locked volumes
};

} // namespace winapfs
