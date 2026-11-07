// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// apfsrw-smoke: mount a volume with the linux-apfs-rw engine, list a
// directory and print a file.
//   apfsrw-smoke <image> <offset> <vol> [dir] [file]

#include <apfsrw.h>

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static void Log(int level, const char *msg)
{
	fprintf(stderr, "[apfs %d] %s%s", level, msg, (*msg && msg[strlen(msg) - 1] == '\n') ? "" : "\n");
}

static int ListCb(void *, const char *name, size_t len, uint64_t ino, unsigned dtype)
{
	printf("%6llu %2u %.*s\n", (unsigned long long)ino, dtype, (int)len, name);
	return 0;
}

static apfsrw_node *Walk(apfsrw_node *root, const std::string &path)
{
	apfsrw_node *cur = apfsrw_get(root);
	size_t pos = 0;
	while (pos < path.size())
	{
		size_t next = path.find('/', pos);
		if (next == std::string::npos)
			next = path.size();
		if (next > pos)
		{
			apfsrw_node *child = nullptr;
			int err = apfsrw_lookup(cur, path.data() + pos, next - pos, &child);
			apfsrw_put(cur);
			if (err)
			{
				fprintf(stderr, "lookup %s: %d\n", path.substr(pos, next - pos).c_str(), err);
				return nullptr;
			}
			cur = child;
		}
		pos = next + 1;
	}
	return cur;
}

int wmain(int argc, wchar_t **argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "usage: apfsrw-smoke <image> <offset> <vol> [dir] [file]\n");
		return 2;
	}
	apfsrw_set_log(Log);
	apfsrw_mount *m = nullptr;
	int err = apfsrw_mount_volume(argv[1], _wtoi64(argv[2]), 0, (unsigned)_wtoi(argv[3]), false, nullptr, &m);
	if (err)
	{
		fprintf(stderr, "mount failed: %d\n", err);
		return 1;
	}
	apfsrw_node *root = nullptr;
	apfsrw_root(m, &root);

	char buf[256];
	auto narrow = [&](const wchar_t *w) {
		WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
		return std::string(buf);
	};

	std::string dir = argc > 4 ? narrow(argv[4]) : "/";
	apfsrw_node *d = Walk(root, dir);
	int rc = 0;
	if (d)
	{
		apfsrw_stat st;
		apfsrw_getattr(d, &st);
		printf("%s: ino %llu mode %o nlink %u\n", dir.c_str(), (unsigned long long)st.ino, st.mode, st.nlink);
		err = apfsrw_readdir(d, ListCb, nullptr);
		if (err)
		{
			fprintf(stderr, "readdir: %d\n", err);
			rc = 1;
		}
		apfsrw_put(d);
	}
	else
		rc = 1;

	if (argc > 5)
	{
		apfsrw_node *f = Walk(root, narrow(argv[5]));
		apfsrw_file *fh = nullptr;
		if (f && apfsrw_open(f, false, &fh) == 0)
		{
			char data[4096];
			size_t got = 0;
			uint64_t off = 0;
			while (apfsrw_read(fh, off, data, sizeof(data), &got) == 0 && got)
			{
				fwrite(data, 1, got, stdout);
				off += got;
			}
			apfsrw_release(fh);
		}
		else
			rc = 1;
		if (f)
			apfsrw_put(f);
	}

	apfsrw_put(root);
	err = apfsrw_unmount(m);
	if (err)
		fprintf(stderr, "unmount: %d\n", err);
	return rc;
}
