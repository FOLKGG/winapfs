// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// APFS <-> Windows file name mapping.
//
// APFS names are UTF-8 and may contain characters Windows forbids. They are
// mapped to the Unicode private use area the same way Cygwin and WSL do, so
// the mapping round-trips and other tools show the same names:
//   0x01-0x1F and " * : < > ? \ |   ->  U+F000 + c
//   a trailing ' ' or '.'           ->  U+F020 / U+F02E
// Bytes that are not valid UTF-8 become lone surrogates U+DC80 + (b - 0x80)
// (Python's "surrogateescape"), which NTFS-style names tolerate.
// Unicode normalization is left alone: the name Windows sees is exactly the
// stored name (NFC or NFD); lookups use APFS's own normalization rules.

#pragma once

#include <string>

namespace winapfs {

std::wstring ApfsNameToWindows(const std::string &utf8);
std::string WindowsNameToApfs(const std::wstring &name);

} // namespace winapfs
