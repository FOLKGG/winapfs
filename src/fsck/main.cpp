// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// winapfs-fsck: apfsprogs' apfsck on Windows. Finds the APFS partition in a
// GPT image or disk (apfsck itself expects a bare container), then runs the
// unmodified checker. Exit status: 0 clean, 1 something "weird" reported
// with -w, other nonzero on corruption or errors (apfsck's own convention).
//
//   winapfs-fsck [-c] [-u] [-w] [--partition N] <image|\\.\PhysicalDriveN>

#include <winapfs/Apfs.h>

#include <Windows.h>

#include <cstdio>
#include <string>
#include <vector>

extern "C" int apfsck_main(int argc, char *argv[]);
extern "C" void kfsck_set_partition(unsigned long long offset, unsigned long long size);

using namespace winapfs;

int wmain(int argc, wchar_t **wargv)
{
	SetConsoleOutputCP(CP_UTF8);

	std::vector<std::string> args;
	std::string device;
	int partition = -1;
	for (int i = 1; i < argc; i++)
	{
		std::string a = WideToUtf8(wargv[i]);
		if (a == "--partition" && i + 1 < argc)
			partition = _wtoi(wargv[++i]);
		else if (!a.empty() && a[0] == '-')
			args.push_back(a);
		else if (device.empty())
			device = a;
		else
			args.push_back(a);   // let apfsck complain
	}
	if (device.empty())
	{
		fprintf(stderr, "usage: winapfs-fsck [-c] [-u] [-w] [--partition N] <image|\\\\.\\PhysicalDriveN>\n"
			"  -c  report signs of a past crash\n"
			"  -u  report unknown or unsupported features\n"
			"  -w  report things that may not be corruption\n");
		return 2;
	}

	std::string err;
	std::vector<PartitionInfo> parts;
	if (!Container::FindApfsPartitions(device, parts, err))
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 2;
	}
	if (!parts.empty())
	{
		const PartitionInfo *p = nullptr;
		for (const auto &x : parts)
			if (partition < 0 || x.index == partition)
			{
				p = &x;
				break;
			}
		if (!p)
		{
			fprintf(stderr, "no APFS partition %d\n", partition);
			return 2;
		}
		kfsck_set_partition(p->offset, p->size);
	}
	else if (partition >= 0)
	{
		fprintf(stderr, "%s has no GPT partition table\n", device.c_str());
		return 2;
	}

	std::vector<std::string> owned;
	owned.push_back("winapfs-fsck");
	owned.insert(owned.end(), args.begin(), args.end());
	owned.push_back(device);
	std::vector<char *> argv;
	for (auto &s : owned)
		argv.push_back(&s[0]);
	argv.push_back(nullptr);
	return apfsck_main(static_cast<int>(owned.size()), argv.data());
}
