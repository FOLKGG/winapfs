// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Apfs.h"

namespace winapfs {

// Reader for decmpfs (UF_COMPRESSED) files: zlib, LZVN, LZFSE, LZBITMAP and
// stored-raw, inline in com.apple.decmpfs or chunked in the resource fork.
std::unique_ptr<FileReader> OpenCompressedFile(Volume &vol, const Stat &st, std::string &err);

} // namespace winapfs
