// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// WINAPFS_PROFILE=1: sample every thread of this process (~1 ms) and print
// the hottest functions to stderr at exit (needs the PDB next to the exe).

#pragma once

namespace winapfs {

void StartProfilerFromEnv();
void StopProfilerAndReport();

} // namespace winapfs
