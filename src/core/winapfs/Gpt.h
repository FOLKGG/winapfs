// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Apfs.h"

class Device;

namespace winapfs {

// Reads the primary GPT (falling back to the backup header) and returns every
// partition whose type GUID is 7C3457EF-0000-11AA-AA11-00306543ECAC. Tries
// 512- and 4096-byte logical sectors. Returns false if there is no valid GPT.
bool ReadGptApfsPartitions(Device &dev, std::vector<PartitionInfo> &parts);

} // namespace winapfs
