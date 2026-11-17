// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Profiler.h"

#include <Windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace winapfs {

namespace {

std::atomic<bool> g_on{ false };
std::thread g_thread;
std::map<DWORD64, int> g_samples;   // RIP -> count (all threads)
int g_idle = 0;                     // samples of threads waiting in the kernel

void Sample()
{
	DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
	timeBeginPeriod(1);
	while (g_on)
	{
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		THREADENTRY32 te = { sizeof(te) };
		for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
		{
			if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
				continue;
			HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
			if (!t)
				continue;
			if (SuspendThread(t) != static_cast<DWORD>(-1))
			{
				CONTEXT ctx = {};
				ctx.ContextFlags = CONTEXT_CONTROL;
				if (GetThreadContext(t, &ctx))
					g_samples[ctx.Rip]++;
				ResumeThread(t);
			}
			CloseHandle(t);
		}
		CloseHandle(snap);
		Sleep(1);
	}
	timeEndPeriod(1);
}

} // namespace

void StartProfilerFromEnv()
{
	char v[8];
	if (!GetEnvironmentVariableA("WINAPFS_PROFILE", v, sizeof(v)) || v[0] == '0')
		return;
	g_on = true;
	g_thread = std::thread(Sample);
}

void StopProfilerAndReport()
{
	if (!g_on)
		return;
	g_on = false;
	g_thread.join();
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
	fprintf(stderr, "profile: %d samples over all threads (waits included)\n", total);
	for (size_t i = 0; i < v.size() && i < 30; i++)
		fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * v[i].first / total, v[i].second.c_str());
}

} // namespace winapfs
