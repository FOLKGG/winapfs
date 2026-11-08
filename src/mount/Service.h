// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// `winapfs service ...`: the auto-mount service (W7).

#pragma once

#include <string>
#include <vector>

int CmdService(const std::vector<std::wstring> &args);
// `winapfs ctl ...`: talk to the running service (Control.h)
int CmdCtl(const std::vector<std::wstring> &args);
