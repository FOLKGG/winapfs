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
#include <psapi.h>

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
	int files = 0;   // --files N: N files of <MiB> each, with memory and page-cache stats after each
	int small_files = 0;   // --small N: N files of 16 KiB in one directory, rate per 1000
	int rereads = 0;   // --reread N: write one file of <MiB>, then read it N times (page-cache churn in one mapping)
	int sparse_gib = 0;   // --sparse G: read a G GiB sparse file once (ever-new pages in one mapping)
	for (int i = 5; i + 1 < argc; i++)
	{
		if (wcscmp(argv[i], L"--files") == 0)
			files = _wtoi(argv[i + 1]);
		if (wcscmp(argv[i], L"--small") == 0)
			small_files = _wtoi(argv[i + 1]);
		if (wcscmp(argv[i], L"--reread") == 0)
			rereads = _wtoi(argv[i + 1]);
		if (wcscmp(argv[i], L"--sparse") == 0)
			sparse_gib = _wtoi(argv[i + 1]);
	}

	apfsrw_mount *m = nullptr;
	int err = apfsrw_mount_volume(img, offset, 0, vol, true, nullptr, &m);
	if (err)
	{
		fprintf(stderr, "mount: %d\n", err);
		return 1;
	}
	if (small_files > 0)
	{
		apfsrw_node *r = nullptr, *dir = nullptr;
		apfsrw_root(m, &r);
		apfsrw_mkdir(r, "small", 5, 0755, &dir);
		std::vector<uint8_t> data(16384, 0x33);
		char stats[512];
		std::thread smp;
		if (profile)
		{
			HANDLE self = nullptr;
			DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
			g_sampling = true;
			smp = std::thread(Sampler, self);
		}
		double s0 = Now();
		for (int k = 0; k < small_files; k++)
		{
			std::string name = "f" + std::to_string(k) + ".bin";
			apfsrw_node *fn = nullptr;
			if (apfsrw_create(dir, name.data(), name.size(), 0644, &fn))
			{
				fprintf(stderr, "create %s failed\n", name.c_str());
				break;
			}
			apfsrw_file *ff = nullptr;
			apfsrw_open(fn, true, &ff);
			size_t w = 0;
			apfsrw_write(ff, 0, data.data(), data.size(), &w);
			apfsrw_release(ff);
			apfsrw_put(fn);
			if ((k + 1) % 1000 == 0)
			{
				double s1 = Now();
				PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
				GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
				apfsrw_debug_stats(stats, sizeof(stats));
				printf("small %6d: %6.0f files/s, private %5zu MB | %s\n", k + 1, 1000 / (s1 - s0), pmc.PrivateUsage >> 20, stats);
				s0 = s1;
			}
		}
		if (profile)
		{
			g_sampling = false;
			smp.join();
			Report();
		}
		// Inode cache bound (2026-11-17): every inode a session touched used to
		// stay cached until unmount (a mapping each). Committed, unreferenced
		// inodes are evicted beyond ~4K now.
		apfsrw_sync(m);
		apfsrw_debug_stats(stats, sizeof(stats));
		const char *mp = strstr(stats, "mappings ");
		long mappings = mp ? atol(mp + 9) : -1;
		printf("after sync: %ld mappings\n", mappings);
		int rc = 0;
		if (small_files >= 20000 && (mappings < 0 || mappings > 16384))
		{
			fprintf(stderr, "FAIL: %ld mappings cached after %d files: inodes are not evicted\n", mappings, small_files);
			rc = 1;
		}
		apfsrw_put(dir);
		apfsrw_put(r);
		apfsrw_unmount(m);
		return rc;
	}
	if (sparse_gib > 0)
	{
		// Regression for the page-cache index (2026-11-17): a huge file read
		// once creates and reclaims ever-new pages in one mapping. The old
		// open-addressing table ran out of empty slots after a few GB
		// (tombstones) and every lookup of an uncached page looped forever.
		// A sparse file needs no space: holes read as zero pages. Fails on a
		// GiB that takes 5x the first, or after 60 s without progress.
		apfsrw_node *r = nullptr, *fn = nullptr;
		apfsrw_root(m, &r);
		apfsrw_unlink(r, "sparse.bin", 10);
		if (apfsrw_create(r, "sparse.bin", 10, 0644, &fn))
		{
			fprintf(stderr, "create failed\n");
			return 1;
		}
		apfsrw_setattr_t sa = {};
		sa.mask = APFSRW_SET_SIZE;
		sa.size = static_cast<uint64_t>(sparse_gib) << 30;
		if (apfsrw_setattr(fn, &sa))
		{
			fprintf(stderr, "setattr size failed\n");
			return 1;
		}
		static volatile LONG progress = 0;
		std::thread watchdog([] {
			LONG last = -1;
			for (;;)
			{
				Sleep(60000);
				LONG p = progress;
				if (p < 0)
					return;
				if (p == last)
				{
					fprintf(stderr, "FAIL: no progress for 60 s (hang)\n");
					fflush(stderr);
					ExitProcess(3);
				}
				last = p;
			}
		});
		std::vector<uint8_t> data(1 << 20);
		apfsrw_file *ff = nullptr;
		apfsrw_open(fn, false, &ff);
		double first = 0, s0 = Now();
		int rc = 0;
		for (uint64_t off = 0; off < sa.size && !rc; off += data.size())
		{
			size_t got = 0;
			data[0] = 1;
			if (apfsrw_read(ff, off, data.data(), data.size(), &got) || got != data.size() || data[0] != 0)
			{
				fprintf(stderr, "FAIL: read at %llu\n", (unsigned long long)off);
				rc = 1;
			}
			InterlockedIncrement(&progress);
			if (((off + data.size()) & ((1ull << 30) - 1)) == 0)
			{
				double s = Now() - s0;
				printf("GiB %3llu: %6.0f MB/s\n", (unsigned long long)((off + data.size()) >> 30), 1073.741824 / s);
				fflush(stdout);
				if (first == 0)
					first = s > 0.05 ? s : 0.05;
				else if (s > 5 * first)
				{
					fprintf(stderr, "FAIL: a GiB took %.2f s, the first %.2f s\n", s, first);
					rc = 1;
				}
				s0 = Now();
			}
		}
		InterlockedExchange(&progress, -1);
		watchdog.detach();
		apfsrw_release(ff);
		apfsrw_unlink(r, "sparse.bin", 10);
		apfsrw_put(fn);
		apfsrw_put(r);
		apfsrw_unmount(m);
		if (!rc)
			printf("OK: read a %d GiB sparse file\n", sparse_gib);
		return rc;
	}
	if (rereads > 0)
	{
		// Regression for the page-cache index: pages created and reclaimed
		// in one mapping for gigabytes made lookups slower and slower, then
		// loop forever (2026-11-17). Fails if a pass takes 5x the first, and
		// a watchdog ends a hung run.
		apfsrw_node *r = nullptr, *fn = nullptr;
		apfsrw_root(m, &r);
		apfsrw_unlink(r, "reread.bin", 10);
		if (apfsrw_create(r, "reread.bin", 10, 0644, &fn))
		{
			fprintf(stderr, "create failed\n");
			return 1;
		}
		std::vector<uint8_t> data(1 << 20);
		apfsrw_file *ff = nullptr;
		apfsrw_open(fn, true, &ff);
		uint64_t tot = static_cast<uint64_t>(mib) << 20;
		for (uint64_t off = 0; off < tot; off += data.size())
		{
			memset(data.data(), static_cast<int>(off >> 20) & 0xff, data.size());
			size_t w = 0;
			if (apfsrw_write(ff, off, data.data(), data.size(), &w))
			{
				fprintf(stderr, "write failed at %llu\n", (unsigned long long)off);
				return 1;
			}
		}
		apfsrw_sync(m);
		static volatile LONG progress = 0;
		std::thread watchdog([] {
			LONG last = -1;
			for (;;)
			{
				Sleep(60000);
				LONG p = progress;
				if (p < 0)
					return;
				if (p == last)
				{
					fprintf(stderr, "FAIL: no progress for 60 s (hang)\n");
					fflush(stderr);
					ExitProcess(3);
				}
				last = p;
			}
		});
		double first = 0;
		int rc = 0;
		for (int k = 0; k < rereads && !rc; k++)
		{
			double s0 = Now();
			for (uint64_t off = 0; off < tot; off += data.size())
			{
				size_t got = 0;
				if (apfsrw_read(ff, off, data.data(), data.size(), &got) || got != data.size() ||
				    data[0] != static_cast<uint8_t>((off >> 20) & 0xff) || data[data.size() - 1] != data[0])
				{
					fprintf(stderr, "FAIL: pass %d reads back wrong at %llu\n", k, (unsigned long long)off);
					rc = 1;
					break;
				}
				InterlockedIncrement(&progress);
			}
			double s = Now() - s0;
			printf("pass %2d: %6.0f MB/s (%.2f s)\n", k, tot / 1e6 / s, s);
			fflush(stdout);
			if (k == 0)
				first = s > 0.05 ? s : 0.05;
			else if (s > 5 * first)
			{
				fprintf(stderr, "FAIL: pass %d took %.2f s, the first %.2f s\n", k, s, first);
				rc = 1;
			}
		}
		InterlockedExchange(&progress, -1);
		watchdog.detach();
		apfsrw_release(ff);
		apfsrw_put(fn);
		apfsrw_put(r);
		apfsrw_unmount(m);
		if (!rc)
			printf("OK: %d passes over %zu MiB\n", rereads, mib);
		return rc;
	}
	if (files > 0)
	{
		apfsrw_node *r = nullptr;
		apfsrw_root(m, &r);
		std::vector<uint8_t> data(chunk, 0x5a);
		char stats[512];
		for (int k = 0; k < files; k++)
		{
			std::string name = "many" + std::to_string(k) + ".bin";
			apfsrw_node *fn = nullptr;
			apfsrw_unlink(r, name.data(), name.size());
			if (apfsrw_create(r, name.data(), name.size(), 0644, &fn))
				break;
			apfsrw_file *ff = nullptr;
			apfsrw_open(fn, true, &ff);
			double s0 = Now();
			uint64_t tot = static_cast<uint64_t>(mib) << 20;
			for (uint64_t off = 0; off < tot; off += chunk)
			{
				size_t w = 0;
				if (apfsrw_write(ff, off, data.data(), static_cast<size_t>(std::min<uint64_t>(chunk, tot - off)), &w))
					break;
			}
			apfsrw_release(ff);
			apfsrw_put(fn);
			double s1 = Now();
			PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
			GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
			apfsrw_debug_stats(stats, sizeof(stats));
			printf("file %2d: %5.0f MB/s, private %5zu MB | %s\n", k, tot / 1e6 / (s1 - s0), pmc.PrivateUsage >> 20, stats);
		}
		apfsrw_sync(m);
		apfsrw_debug_stats(stats, sizeof(stats));
		printf("after sync | %s\n", stats);
		apfsrw_put(r);
		apfsrw_unmount(m);
		return 0;
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
