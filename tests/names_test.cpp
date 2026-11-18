// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Round-trip tests for the APFS <-> Windows name mapping. Every stored name
// must come back byte for byte, and no two stored names may map to the same
// Windows name.

#include <winapfs/Names.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using winapfs::ApfsNameToWindows;
using winapfs::WindowsNameToApfs;

static int g_fail = 0;

static std::string Hex(const std::string &s)
{
	std::string out;
	char buf[4];
	for (unsigned char b : s)
	{
		std::snprintf(buf, sizeof buf, "%02X ", b);
		out += buf;
	}
	return out;
}

static void Check(bool ok, const char *what, const std::string &name)
{
	if (!ok)
	{
		std::printf("FAIL %s: %s\n", what, Hex(name).c_str());
		g_fail++;
	}
}

int main()
{
	const std::vector<std::string> names = {
		"plain.txt",
		"a:b",                          // forbidden, mapped to U+F03A
		"\xEF\x80\xBA",                 // stored U+F03A on its own
		"a\xEF\x80\xBA" "b",            // stored U+F03A inside a name
		"x\xEF\x80\xAA",                // stored U+F02A ('*')
		"q\xEF\x80\xBF" "z",            // stored U+F03F ('?')
		"tab\x01" "x",                  // control character
		"\xEF\x80\x81",                 // stored U+F001
		"trail ",                       // trailing space, mapped
		"trail.",                       // trailing dot, mapped
		"trail\xEF\x80\xA0",            // stored U+F020 at the end
		"trail\xEF\x80\xAE",            // stored U+F02E at the end
		"mid\xEF\x80\xA0" "x",          // stored U+F020 not at the end: no escape needed
		"\xEF\x80\x80",                 // U+F000 is outside the mapped range
		"\xEF\x81\x80",                 // U+F040 ('@' is allowed)
		"bad\xFF" "byte",               // invalid UTF-8
		"caf\xC3\xA9",                  // NFC
		"cafe\xCC\x81",                 // NFD
		"\xF0\x9F\x8D\x8E",             // outside the BMP
	};

	std::map<std::wstring, std::string> seen;
	for (const auto &n : names)
	{
		std::wstring w = ApfsNameToWindows(n);
		Check(WindowsNameToApfs(w) == n, "round trip", n);

		auto it = seen.find(w);
		Check(it == seen.end(), "collision", n);
		seen.emplace(w, n);

		for (wchar_t ch : w)
		{
			bool forbidden = (ch >= 1 && ch < 0x20) || ch == L'"' || ch == L'*' || ch == L':' ||
				ch == L'<' || ch == L'>' || ch == L'?' || ch == L'\\' || ch == L'|';
			Check(!forbidden, "forbidden character in Windows name", n);
		}
		if (!w.empty())
			Check(w.back() != L' ' && w.back() != L'.', "trailing space or dot in Windows name", n);
	}

	// The forbidden-character mapping itself is unchanged.
	Check(ApfsNameToWindows("a:b") == std::wstring(L"a\xF03A" L"b"), "':' maps to U+F03A", "a:b");
	Check(WindowsNameToApfs(L"a\xF03A" L"b") == "a:b", "U+F03A from Windows maps to ':'", "a:b");

	std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "names: OK", g_fail);
	return g_fail ? 1 : 0;
}
