// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Names.h"

#include <cstdint>

namespace winapfs {

static bool IsWinForbidden(char32_t c)
{
	if (c >= 1 && c < 0x20)
		return true;
	switch (c)
	{
	case '"': case '*': case ':': case '<': case '>': case '?': case '\\': case '|':
		return true;
	default:
		return false;
	}
}

static void PutUtf16(std::wstring &out, char32_t c)
{
	if (c >= 0x10000)
	{
		c -= 0x10000;
		out.push_back(static_cast<wchar_t>(0xD800 + (c >> 10)));
		out.push_back(static_cast<wchar_t>(0xDC00 + (c & 0x3FF)));
	}
	else
	{
		out.push_back(static_cast<wchar_t>(c));
	}
}

std::wstring ApfsNameToWindows(const std::string &s)
{
	std::wstring out;
	out.reserve(s.size());
	const uint8_t *p = reinterpret_cast<const uint8_t *>(s.data());
	size_t n = s.size(), i = 0;

	while (i < n)
	{
		uint8_t b = p[i];
		char32_t c = 0;
		size_t len = 0;
		if (b < 0x80) { c = b; len = 1; }
		else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; len = 2; }
		else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; len = 3; }
		else if ((b & 0xF8) == 0xF0) { c = b & 0x07; len = 4; }

		bool ok = len > 0 && i + len <= n;
		for (size_t k = 1; ok && k < len; k++)
		{
			if ((p[i + k] & 0xC0) != 0x80)
				ok = false;
			else
				c = (c << 6) | (p[i + k] & 0x3F);
		}
		if (ok)
		{
			// reject overlong forms, surrogates and out-of-range values
			static const char32_t min_for_len[5] = { 0, 0, 0x80, 0x800, 0x10000 };
			if (c < min_for_len[len] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
				ok = false;
		}
		if (!ok)
		{
			out.push_back(static_cast<wchar_t>(0xDC00 + b));  // b >= 0x80 here
			i += 1;
			continue;
		}

		if (IsWinForbidden(c))
			c += 0xF000;
		PutUtf16(out, c);
		i += len;
	}

	if (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
	{
		// "." and ".." are never stored, but keep them intact anyway
		if (!(out == L"." || out == L".."))
			out.back() = static_cast<wchar_t>(0xF000 + out.back());
	}
	return out;
}

std::string WindowsNameToApfs(const std::wstring &w)
{
	std::string out;
	out.reserve(w.size() * 3);
	size_t n = w.size();

	for (size_t i = 0; i < n; i++)
	{
		char32_t c = w[i];
		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
		{
			c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
			i++;
		}
		else if (c >= 0xDC80 && c <= 0xDCFF)
		{
			out.push_back(static_cast<char>(c - 0xDC00));  // escaped raw byte
			continue;
		}
		else if (c >= 0xF001 && c <= 0xF07F)
		{
			char32_t orig = c - 0xF000;
			bool trailing = (i + 1 == n);
			if (IsWinForbidden(orig) || (trailing && (orig == ' ' || orig == '.')))
				c = orig;
		}

		if (c < 0x80)
			out.push_back(static_cast<char>(c));
		else if (c < 0x800)
		{
			out.push_back(static_cast<char>(0xC0 | (c >> 6)));
			out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		}
		else if (c < 0x10000)
		{
			out.push_back(static_cast<char>(0xE0 | (c >> 12)));
			out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		}
		else
		{
			out.push_back(static_cast<char>(0xF0 | (c >> 18)));
			out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

} // namespace winapfs
