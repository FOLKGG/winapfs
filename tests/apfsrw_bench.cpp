// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// apfsrw-bench: engine write/read throughput, with a sampling profiler.
//
//   apfsrw-bench <work.img> <container offset> <volume index> <MiB> [chunk KiB] [--profile]
//
// Mounts the bare container image read-write, writes a new file of <MiB>
// in chunks, unmounts (timed separately), then reads it back. --profile
// samples the writing thread every millisecond and prints the hottest
// functions (needs the PDBs next to the binaries).

#include <apfsrw.h>

#include <Windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_sampling{ false };
std::map<DWORD64, int> g_samples;

void Sampler(HANDLE thread)
{
	while (g_sampling)
	{
		if (SuspendThread(thread) != (DWORD)-1)
		{
			CONTEXT ctx = {};
			ctx.ContextFlags = CONTEXT_CONTROL;
			if (GetThreadContext(thread, &ctx))
				g_samples[ctx.Rip]++;
			ResumeThread(thread);
		}
		Sleep(1);
	}
}

void Report()
{
	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(proc, nullptr, TRUE);
	std::map<std::string, int> by_fn;
	int total = 0;
	for (const auto &kv : g_samples)
	{
		char buf[sizeof(SYMBOL_INFO) + 256];
		SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		std::string name = SymFromAddr(proc, kv.first, &disp, sym) ? sym->Name : "?";
		by_fn[name] += kv.second;
		total += kv.second;
	}
	std::vector<std::pair<int, std::string>> v;
	for (const auto &kv : by_fn)
		v.emplace_back(kv.second, kv.first);
	std::sort(v.rbegin(), v.rend());
	printf("profile: %d samples\n", total);
	for (size_t i = 0; i < v.size() && i < 25; i++)
		printf("  %5.1f%%  %s\n", 100.0 * v[i].first / total, v[i].second.c_str());
}

double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
	if (argc < 5)
	{
		fprintf(stderr, "usage: apfsrw-bench <work.img> <container offset> <volume index> <MiB> [chunk KiB] [--profile]\n");
		return 2;
	}
	const wchar_t *img = argv[1];
	uint64_t offset = _wcstoui64(argv[2], nullptr, 10);
	unsigned vol = static_cast<unsigned>(_wtoi(argv[3]));
	size_t mib = static_cast<size_t>(_wtoi(argv[4]));
	size_t chunk = (argc > 5 && argv[5][0] != L'-' ? static_cast<size_t>(_wtoi(argv[5])) : 1024) * 1024;
	bool profile = argc > 5 && wcscmp(argv[argc - 1], L"--profile") == 0;

	apfsrw_mount *m = nullptr;
	int err = apfsrw_mount_volume(img, offset, 0, vol, true, nullptr, &m);
	if (err)
	{
		fprintf(stderr, "mount: %d\n", err);
		return 1;
	}
	apfsrw_node *root = nullptr, *n = nullptr;
	apfsrw_root(m, &root);
	apfsrw_unlink(root, "bench.bin", 9);
	err = apfsrw_create(root, "bench.bin", 9, 0644, &n);
	if (err)
	{
		fprintf(stderr, "create: %d\n", err);
		return 1;
	}
	std::vector<uint8_t> buf(chunk);
	for (size_t i = 0; i < buf.size(); i++)
		buf[i] = static_cast<uint8_t>(i * 7 + 1);

	std::thread sampler;
	if (profile)
	{
		HANDLE self = nullptr;
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
		g_sampling = true;
		sampler = std::thread(Sampler, self);
	}
	double t0 = Now();
	apfsrw_file *f = nullptr;
	apfsrw_open(n, true, &f);
	uint64_t total = static_cast<uint64_t>(mib) << 20;
	for (uint64_t off = 0; off < total; off += chunk)
	{
		size_t w = 0;
		err = apfsrw_write(f, off, buf.data(), static_cast<size_t>(std::min<uint64_t>(chunk, total - off)), &w);
		if (err)
		{
			fprintf(stderr, "write at %llu: %d\n", (unsigned long long)off, err);
			return 1;
		}
	}
	apfsrw_release(f);
	double t1 = Now();
	err = apfsrw_sync(m);
	double t2 = Now();
	if (profile)
	{
		g_sampling = false;
		sampler.join();
	}
	apfsrw_put(n);
	apfsrw_put(root);
	apfsrw_unmount(m);
	double t3 = Now();
	printf("write %zu MiB in %zu KiB chunks: %.2f s (%.0f MB/s), sync %.2f s, unmount %.2f s\n", mib, chunk >> 10, t1 - t0,
		total / 1e6 / (t1 - t0), t2 - t1, t3 - t2);
	if (profile)
		Report();
	return err ? 1 : 0;
}
