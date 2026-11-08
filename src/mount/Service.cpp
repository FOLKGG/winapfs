// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The auto-mount service. Runs as LocalSystem, so its drive letters are
// global (Mount Manager) and visible to every user:
//   - mounts every APFS volume on every disk at start and when a disk arrives
//     (disk interface notifications), at a drive letter remembered per volume
//     UUID;
//   - unmounts a disk's volumes when Windows asks to remove it ("Safely
//     remove", eject: handle notifications on each disk), when it is gone,
//     and at shutdown (pre-shutdown, so read-write volumes unmount cleanly);
//   - read-only unless configured (and allowed) read-write; refused writes
//     fall back to read-only.
//
// Settings: HKLM\SOFTWARE\winapfs (tests: WINAPFS_CONFIG_HKCU=1 uses
// HKCU\Software\winapfs-test)
//   AllowDiskWrites  DWORD  1: read-write volumes on physical disks (W5 opt-in)
//   ReadWrite        DWORD  default for volumes without their own setting
//   HideDotfiles     DWORD
//   Volumes\<UUID>   DriveLetter (REG_SZ "X:"), ReadWrite, HideDotfiles,
//                    Disabled (DWORD), Name (REG_SZ, informational)
// Encrypted volumes are skipped (a per-user helper will ask for passwords).

#include "Service.h"

#include "ApfsFs.h"
#include "Mount.h"
#include "messages.h"

#include <Windows.h>
#include <dbt.h>
#include <initguid.h>
#include <winioctl.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace winapfs;

namespace {

const wchar_t *kServiceName = L"winapfs";
const wchar_t *kDisplayName = L"winapfs (APFS for Windows)";
const wchar_t *kDescription = L"Mounts APFS (Mac) disks at drive letters when they are connected.";

// ---------------------------------------------------------------- settings

struct Config
{
	HKEY root = HKEY_LOCAL_MACHINE;
	std::wstring base = L"SOFTWARE\\winapfs";

	Config()
	{
		wchar_t v[8];
		if (GetEnvironmentVariableW(L"WINAPFS_CONFIG_HKCU", v, 8))
		{
			root = HKEY_CURRENT_USER;
			base = L"Software\\winapfs-test";
		}
	}

	std::wstring Key(const std::wstring &sub) const { return sub.empty() ? base : base + L"\\" + sub; }

	DWORD Dword(const std::wstring &sub, const wchar_t *name, DWORD def) const
	{
		DWORD v = 0, size = sizeof(v);
		if (RegGetValueW(root, Key(sub).c_str(), name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
			return v;
		return def;
	}

	std::wstring String(const std::wstring &sub, const wchar_t *name) const
	{
		wchar_t buf[256];
		DWORD size = sizeof(buf);
		if (RegGetValueW(root, Key(sub).c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
			return buf;
		return L"";
	}

	void SetString(const std::wstring &sub, const wchar_t *name, const std::wstring &value) const
	{
		HKEY k;
		if (RegCreateKeyExW(root, Key(sub).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
			return;
		RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
		RegCloseKey(k);
	}
};

// ---------------------------------------------------------------- logging

HANDLE g_event_source = nullptr;

void EventLogSink(int level, const std::string &msg)
{
	std::wstring w = Utf8ToWide(msg);
	const wchar_t *strs[] = { w.c_str() };
	WORD type = level == 0 ? EVENTLOG_ERROR_TYPE : level == 1 ? EVENTLOG_WARNING_TYPE : EVENTLOG_INFORMATION_TYPE;
	if (g_event_source)
		ReportEventW(g_event_source, type, 0, MSG_TEXT, nullptr, 1, 0, strs, nullptr);
	OutputDebugStringW((L"winapfs: " + w + L"\n").c_str());
}

// ---------------------------------------------------------------- drive letters

bool LetterFree(wchar_t c)
{
	DWORD used = GetLogicalDrives();
	wchar_t root[] = { c, L':', 0 };
	wchar_t target[MAX_PATH];
	return !(used & (1u << (c - L'A'))) && !QueryDosDeviceW(root, target, MAX_PATH);
}

wchar_t PickLetter(const std::wstring &remembered)
{
	if (remembered.size() == 2 && iswalpha(remembered[0]) && remembered[1] == L':')
	{
		wchar_t c = static_cast<wchar_t>(towupper(remembered[0]));
		if (c >= L'D' && LetterFree(c))
			return c;
	}
	for (wchar_t c = L'Z'; c >= L'D'; c--)
		if (LetterFree(c))
			return c;
	return 0;
}

// ---------------------------------------------------------------- auto-mounter

class AutoMounter
{
public:
	AutoMounter(const Config &cfg, std::vector<std::string> images, SERVICE_STATUS_HANDLE status)
		: m_cfg(cfg), m_images(std::move(images)), m_status(status) {}

	~AutoMounter() { UnmountAll(); }

	// Mounts what is new, unmounts what is gone.
	void Scan()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<std::string> sources = m_images;
		if (sources.empty())
			for (const auto &d : EnumeratePhysicalDrives())
				if (!d.access_denied && !d.apfs.empty())
					sources.push_back(d.path);

		// disks that disappeared
		for (size_t i = 0; i < m_disks.size();)
		{
			bool present = false;
			for (const auto &s : sources)
				present = present || s == m_disks[i]->path;
			if (present)
				i++;
			else
			{
				Log(2, "%s is gone", m_disks[i]->path.c_str());
				Drop(i);
			}
		}
		for (const auto &src : sources)
			ScanDisk(src);
	}

	// A disk Windows wants to remove: release everything on it (query-remove).
	bool ReleaseByNotifyHandle(HANDLE h)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (size_t i = 0; i < m_disks.size(); i++)
			if (m_disks[i]->notify_handle == h)
			{
				Log(2, "%s: removal requested, unmounting", m_disks[i]->path.c_str());
				Drop(i);
				return true;
			}
		return false;
	}

	void UnmountAll()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		while (!m_disks.empty())
			Drop(m_disks.size() - 1);
	}

	// name -> mount point, for --run in debug mode
	std::vector<std::pair<std::string, std::wstring>> Mounts()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<std::pair<std::string, std::wstring>> out;
		for (const auto &d : m_disks)
			for (const auto &m : d->mounts)
				out.emplace_back(m.name, m.fs->MountPoint());
		return out;
	}

private:
	struct Mounted
	{
		std::string uuid, name;
		std::unique_ptr<ApfsFs> fs;
	};
	struct Disk
	{
		std::string path;
		DiskLocks locks;   // released after the mounts and containers below
		std::vector<std::unique_ptr<Container>> containers;
		std::vector<Mounted> mounts;
		std::set<std::string> skipped;   // volumes not mounted (said once, not retried)
		HANDLE notify_handle = INVALID_HANDLE_VALUE;
		HDEVNOTIFY notify = nullptr;
	};

	void Drop(size_t i)
	{
		Disk &d = *m_disks[i];
		for (auto &m : d.mounts)
		{
			std::wstring mp = m.fs->MountPoint();
			m.fs->Stop();
			Log(2, "unmounted %s (%ls)", m.name.c_str(), mp.c_str());
		}
		d.mounts.clear();
		d.containers.clear();
		d.locks.clear();
		if (d.notify)
			UnregisterDeviceNotification(d.notify);
		if (d.notify_handle != INVALID_HANDLE_VALUE)
			CloseHandle(d.notify_handle);
		m_disks.erase(m_disks.begin() + static_cast<ptrdiff_t>(i));
	}

	void ScanDisk(const std::string &src)
	{
		Disk *disk = nullptr;
		for (auto &d : m_disks)
			if (d->path == src)
				disk = d.get();
		bool is_new = !disk;
		if (is_new)
		{
			m_disks.emplace_back(new Disk());
			disk = m_disks.back().get();
			disk->path = src;
		}

		std::string err;
		std::vector<PartitionInfo> parts;
		Container::FindApfsPartitions(src, parts, err);
		std::vector<int> which;
		for (const auto &p : parts)
			which.push_back(p.index);
		if (which.empty())
			which.push_back(-1);   // a bare container image

		std::set<std::string> seen;
		for (int part : which)
		{
			std::unique_ptr<Container> c = Container::Open(src, err, part);
			if (!c)
			{
				Log(1, "%s: %s", src.c_str(), err.c_str());
				continue;
			}
			// read-write volumes first: the engine opens the container's
			// device writable for them, and read-only volumes can share it
			// (not the other way round)
			std::vector<const VolumeInfo *> order;
			for (const auto &vi : c->Volumes())
				if (WantsReadWrite(vi))
					order.push_back(&vi);
			for (const auto &vi : c->Volumes())
				if (!WantsReadWrite(vi))
					order.push_back(&vi);
			bool used = false;
			for (const VolumeInfo *vi : order)
			{
				seen.insert(vi->uuid);
				bool mounted = false;
				for (const auto &m : disk->mounts)
					mounted = mounted || m.uuid == vi->uuid;
				if (!mounted && MountVolume(*disk, *c, *vi))
					used = true;
			}
			if (used)
				disk->containers.push_back(std::move(c));
		}

		// volumes that left the disk (another disk took its number)
		for (size_t i = 0; i < disk->mounts.size();)
		{
			if (seen.count(disk->mounts[i].uuid))
			{
				i++;
				continue;
			}
			disk->mounts[i].fs->Stop();
			Log(2, "unmounted %s (gone from %s)", disk->mounts[i].name.c_str(), src.c_str());
			disk->mounts.erase(disk->mounts.begin() + static_cast<ptrdiff_t>(i));
		}

		if (!disk->mounts.empty() && disk->notify == nullptr && m_status && src.compare(0, 4, "\\\\.\\") == 0)
			WatchRemoval(*disk);
	}

	bool WantsReadWrite(const VolumeInfo &vi) const
	{
		std::wstring sub = L"Volumes\\" + Utf8ToWide(vi.uuid);
		return m_cfg.Dword(sub, L"ReadWrite", m_cfg.Dword(L"", L"ReadWrite", 0)) != 0;
	}

	bool MountVolume(Disk &disk, Container &c, const VolumeInfo &vi)
	{
		if (disk.skipped.count(vi.uuid))
			return false;
		bool ok = TryMount(disk, c, vi);
		if (!ok)
			disk.skipped.insert(vi.uuid);
		return ok;
	}

	bool TryMount(Disk &disk, Container &c, const VolumeInfo &vi)
	{
		std::wstring sub = L"Volumes\\" + Utf8ToWide(vi.uuid);
		if (m_cfg.Dword(sub, L"Disabled", 0))
			return false;
		if (vi.encrypted)
		{
			Log(2, "%s: encrypted, not mounted automatically (mount it with winapfs mount --ask)", vi.name.c_str());
			return false;
		}
		bool rw = WantsReadWrite(vi);
		OpenOptions o;
		o.allow_disk_writes = m_cfg.Dword(L"", L"AllowDiskWrites", 0) != 0;
		o.locks = &disk.locks;
		std::unique_ptr<Volume> vol;
		if (rw)
		{
			o.rw = true;
			vol = OpenVolume(c, vi, o);
			if (!vol)
				Log(1, "%s: mounting read-only instead", vi.name.c_str());
		}
		if (!vol)
		{
			o.rw = false;
			vol = OpenVolume(c, vi, o);
		}
		if (!vol)
			return false;

		wchar_t letter = PickLetter(m_cfg.String(sub, L"DriveLetter"));
		if (!letter)
		{
			Log(0, "%s: no free drive letter", vi.name.c_str());
			return false;
		}
		MountOptions mo;
		mo.hide_dotfiles = m_cfg.Dword(sub, L"HideDotfiles", m_cfg.Dword(L"", L"HideDotfiles", 0)) != 0;
		std::unique_ptr<ApfsFs> fs(new ApfsFs(std::move(vol), mo));
		wchar_t mp[] = { letter, L':', 0 };
		NTSTATUS r = fs->Start(mp);
		if (!NT_SUCCESS(r))
		{
			Log(0, "%s: cannot mount at %lc: (NTSTATUS 0x%08lx)", vi.name.c_str(), letter, static_cast<unsigned long>(r));
			return false;
		}
		m_cfg.SetString(sub, L"DriveLetter", mp);
		m_cfg.SetString(sub, L"Name", Utf8ToWide(vi.name));
		Log(2, "mounted %s at %ls (%s) from %s", vi.name.c_str(), fs->MountPoint().c_str(),
			fs->Vol().Writable() ? "read-write" : "read-only", disk.path.c_str());
		disk.mounts.push_back(Mounted{ vi.uuid, vi.name, std::move(fs) });
		return true;
	}

	// Handle notifications on the disk: Windows asks before removing it.
	void WatchRemoval(Disk &disk)
	{
		std::wstring p = Utf8ToWide(disk.path);
		HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return;
		DEV_BROADCAST_HANDLE f = {};
		f.dbch_size = sizeof(f);
		f.dbch_devicetype = DBT_DEVTYP_HANDLE;
		f.dbch_handle = h;
		disk.notify = RegisterDeviceNotificationW(m_status, &f, DEVICE_NOTIFY_SERVICE_HANDLE);
		if (!disk.notify)
		{
			CloseHandle(h);
			return;
		}
		disk.notify_handle = h;
	}

	const Config &m_cfg;
	std::vector<std::string> m_images;
	SERVICE_STATUS_HANDLE m_status;
	std::mutex m_mutex;
	std::vector<std::unique_ptr<Disk>> m_disks;
};

// ---------------------------------------------------------------- the service

SERVICE_STATUS_HANDLE g_status = nullptr;
SERVICE_STATUS g_ss = {};
HANDLE g_stop = nullptr, g_rescan = nullptr;
AutoMounter *g_mounter = nullptr;

void ReportStatus(DWORD state, DWORD wait_hint = 0)
{
	g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	g_ss.dwCurrentState = state;
	g_ss.dwControlsAccepted = state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN) : 0;
	g_ss.dwWin32ExitCode = NO_ERROR;
	g_ss.dwWaitHint = wait_hint;
	if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
		g_ss.dwCheckPoint++;
	else
		g_ss.dwCheckPoint = 0;
	SetServiceStatus(g_status, &g_ss);
}

DWORD WINAPI Handler(DWORD control, DWORD type, LPVOID data, LPVOID)
{
	switch (control)
	{
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_PRESHUTDOWN:
		ReportStatus(SERVICE_STOP_PENDING, 30000);
		SetEvent(g_stop);
		return NO_ERROR;
	case SERVICE_CONTROL_INTERROGATE:
		return NO_ERROR;
	case SERVICE_CONTROL_DEVICEEVENT:
	{
		auto *hdr = static_cast<DEV_BROADCAST_HDR *>(data);
		if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_HANDLE)
		{
			auto *h = static_cast<DEV_BROADCAST_HANDLE *>(data);
			// unmount before answering, so that removal can go ahead
			if ((type == DBT_DEVICEQUERYREMOVE || type == DBT_DEVICEREMOVEPENDING || type == DBT_DEVICEREMOVECOMPLETE) && g_mounter)
				g_mounter->ReleaseByNotifyHandle(h->dbch_handle);
			else if (type == DBT_DEVICEQUERYREMOVEFAILED)
				SetEvent(g_rescan);
		}
		else if (type == DBT_DEVICEARRIVAL || type == DBT_DEVICEREMOVECOMPLETE)
			SetEvent(g_rescan);
		return NO_ERROR;
	}
	default:
		return ERROR_CALL_NOT_IMPLEMENTED;
	}
}

void WINAPI ServiceMain(DWORD, LPWSTR *)
{
	g_status = RegisterServiceCtrlHandlerExW(kServiceName, Handler, nullptr);
	if (!g_status)
		return;
	ReportStatus(SERVICE_START_PENDING, 10000);
	g_event_source = RegisterEventSourceW(nullptr, kServiceName);
	SetLogSink(EventLogSink);
	g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_rescan = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	if (!LoadWinFsp())
	{
		Log(0, "WinFsp is not installed (https://winfsp.dev); the service cannot mount anything");
		g_ss.dwWin32ExitCode = ERROR_SERVICE_DEPENDENCY_FAIL;
		ReportStatus(SERVICE_STOPPED);
		return;
	}

	// disk arrival and removal
	DEV_BROADCAST_DEVICEINTERFACE_W f = {};
	f.dbcc_size = sizeof(f);
	f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	f.dbcc_classguid = GUID_DEVINTERFACE_DISK;
	HDEVNOTIFY dn = RegisterDeviceNotificationW(g_status, &f, DEVICE_NOTIFY_SERVICE_HANDLE);

	Config cfg;
	{
		AutoMounter m(cfg, {}, g_status);
		g_mounter = &m;
		ReportStatus(SERVICE_RUNNING);
		Log(2, "started");
		m.Scan();
		HANDLE waits[] = { g_stop, g_rescan };
		for (;;)
		{
			DWORD w = WaitForMultipleObjects(2, waits, FALSE, 60000);   // and a rescan every minute
			if (w == WAIT_OBJECT_0)
				break;
			if (w == WAIT_OBJECT_0 + 1)
			{
				// a new disk takes a moment to show its partitions
				Sleep(1500);
				m.Scan();
				Sleep(3000);
			}
			m.Scan();
		}
		g_mounter = nullptr;
		m.UnmountAll();
	}
	if (dn)
		UnregisterDeviceNotification(dn);
	Log(2, "stopped");
	if (g_event_source)
		DeregisterEventSource(g_event_source);
	ReportStatus(SERVICE_STOPPED);
}

// ---------------------------------------------------------------- debug mode

HANDLE g_debug_stop = nullptr;

BOOL WINAPI DebugCtrl(DWORD)
{
	SetEvent(g_debug_stop);
	return TRUE;
}

int Debug(const std::vector<std::wstring> &args)
{
	std::vector<std::string> images;
	std::wstring run;
	int seconds = 0;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (args[i] == L"--image" && i + 1 < args.size())
			images.push_back(WideToUtf8(args[++i]));
		else if (args[i] == L"--run" && i + 1 < args.size())
			run = args[++i];
		else if (args[i] == L"--seconds" && i + 1 < args.size())
			seconds = _wtoi(args[++i].c_str());
		else
		{
			fprintf(stderr, "usage: winapfs service debug [--image <img>]... [--run <command>] [--seconds <n>]\n");
			return 2;
		}
	}
	if (!LoadWinFsp())
	{
		fprintf(stderr, "WinFsp is not installed (https://winfsp.dev)\n");
		return 1;
	}
	Config cfg;
	AutoMounter m(cfg, images, nullptr);
	m.Scan();
	if (!run.empty())
	{
		for (const auto &mp : m.Mounts())
			SetEnvironmentVariableW(Utf8ToWide(MountEnvName(mp.first)).c_str(), mp.second.c_str());
		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		if (!CreateProcessW(nullptr, &run[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
		{
			fprintf(stderr, "cannot run command (error %lu)\n", GetLastError());
			return 1;
		}
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return static_cast<int>(code);
	}
	g_debug_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	SetConsoleCtrlHandler(DebugCtrl, TRUE);
	fprintf(stderr, "auto-mounting (rescan every 3 s); press Ctrl+C to stop\n");
	ULONGLONG end = seconds ? GetTickCount64() + seconds * 1000ull : 0;
	while (WaitForSingleObject(g_debug_stop, 3000) == WAIT_TIMEOUT && (!end || GetTickCount64() < end))
		m.Scan();
	return 0;
}

// ---------------------------------------------------------------- install & control

std::wstring ExePath()
{
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return buf;
}

int Fail(const char *what)
{
	DWORD e = GetLastError();
	if (e == ERROR_ACCESS_DENIED)
		fprintf(stderr, "%s: access denied (run as administrator)\n", what);
	else
		fprintf(stderr, "%s failed (error %lu)\n", what, e);
	return 1;
}

bool WaitState(SC_HANDLE svc, DWORD want, DWORD timeout_ms)
{
	SERVICE_STATUS st;
	ULONGLONG end = GetTickCount64() + timeout_ms;
	while (QueryServiceStatus(svc, &st))
	{
		if (st.dwCurrentState == want)
			return true;
		if (GetTickCount64() > end)
			return false;
		Sleep(250);
	}
	return false;
}

int Install()
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
	if (!scm)
		return Fail("opening the service manager");
	std::wstring cmd = L"\"" + ExePath() + L"\" service run";
	SC_HANDLE svc = CreateServiceW(scm, kServiceName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
		SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr, nullptr /* LocalSystem */, nullptr);
	if (!svc && GetLastError() == ERROR_SERVICE_EXISTS)
	{
		svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
		if (svc)
			ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, cmd.c_str(), nullptr, nullptr, nullptr,
				nullptr, nullptr, kDisplayName);
	}
	if (!svc)
	{
		int rc = Fail("creating the service");
		CloseServiceHandle(scm);
		return rc;
	}
	SERVICE_DESCRIPTIONW d = { const_cast<LPWSTR>(kDescription) };
	ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);
	SC_ACTION actions[] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 }, { SC_ACTION_NONE, 0 } };
	SERVICE_FAILURE_ACTIONSW fa = {};
	fa.dwResetPeriod = 86400;
	fa.cActions = 3;
	fa.lpsaActions = actions;
	ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
	SERVICE_PRESHUTDOWN_INFO pi = { 60000 };
	ChangeServiceConfig2W(svc, SERVICE_CONFIG_PRESHUTDOWN_INFO, &pi);

	// event log source: the message table is in winapfs.exe
	HKEY k;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\winapfs", 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS)
	{
		std::wstring exe = ExePath();
		DWORD types = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
		RegSetValueExW(k, L"EventMessageFile", 0, REG_SZ, reinterpret_cast<const BYTE *>(exe.c_str()), static_cast<DWORD>((exe.size() + 1) * 2));
		RegSetValueExW(k, L"TypesSupported", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&types), sizeof(types));
		RegCloseKey(k);
	}

	bool started = StartServiceW(svc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
	printf("service installed (%ls)%s\n", cmd.c_str(), started ? " and started" : "");
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return started ? 0 : Fail("starting the service");
}

int Uninstall()
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!scm)
		return Fail("opening the service manager");
	SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
	if (!svc)
	{
		int rc = GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? (printf("service not installed\n"), 0) : Fail("opening the service");
		CloseServiceHandle(scm);
		return rc;
	}
	SERVICE_STATUS st;
	if (ControlService(svc, SERVICE_CONTROL_STOP, &st))
		WaitState(svc, SERVICE_STOPPED, 60000);
	bool ok = DeleteService(svc) != 0;
	int rc = ok ? 0 : Fail("deleting the service");
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\winapfs");
	if (ok)
		printf("service removed (settings in HKLM\\SOFTWARE\\winapfs are kept)\n");
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return rc;
}

int Control(bool start)
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!scm)
		return Fail("opening the service manager");
	SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
	int rc = 0;
	if (!svc)
		rc = Fail("opening the service");
	else if (start)
		rc = (StartServiceW(svc, 0, nullptr) && WaitState(svc, SERVICE_RUNNING, 30000)) ? 0 : Fail("starting the service");
	else
	{
		SERVICE_STATUS st;
		rc = (ControlService(svc, SERVICE_CONTROL_STOP, &st) && WaitState(svc, SERVICE_STOPPED, 60000)) ? 0 : Fail("stopping the service");
	}
	if (svc)
		CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return rc;
}

int Status()
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	SC_HANDLE svc = scm ? OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS) : nullptr;
	SERVICE_STATUS st = {};
	const char *s = "not installed";
	if (svc && QueryServiceStatus(svc, &st))
		s = st.dwCurrentState == SERVICE_RUNNING ? "running" : st.dwCurrentState == SERVICE_STOPPED ? "stopped" : "changing state";
	printf("winapfs service: %s\n", s);
	if (svc)
		CloseServiceHandle(svc);
	if (scm)
		CloseServiceHandle(scm);
	return svc ? 0 : 1;
}

void ServiceUsage()
{
	fprintf(stderr,
		"usage: winapfs service install     install and start the auto-mount service (administrator)\n"
		"       winapfs service uninstall   stop and remove it\n"
		"       winapfs service start|stop|status\n"
		"       winapfs service debug [--image <img>]... [--run <cmd>] [--seconds <n>]\n"
		"                                   the same in this console (images instead of disks)\n"
		"\n"
		"The service mounts every APFS volume read-only at a drive letter it remembers\n"
		"per volume. Settings (HKLM\\SOFTWARE\\winapfs): AllowDiskWrites and ReadWrite\n"
		"(DWORD, both 1 for read-write disks), HideDotfiles; per volume under\n"
		"Volumes\\<UUID>: DriveLetter (\"X:\"), ReadWrite, HideDotfiles, Disabled.\n");
}

} // namespace

int CmdService(const std::vector<std::wstring> &args)
{
	std::wstring sub = args.empty() ? L"" : args[0];
	if (sub == L"run")
	{
		SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(kServiceName), ServiceMain }, { nullptr, nullptr } };
		if (!StartServiceCtrlDispatcherW(table))
		{
			fprintf(stderr, "winapfs service run is started by the service manager; use winapfs service debug\n");
			return 1;
		}
		return 0;
	}
	if (sub == L"debug")
		return Debug(std::vector<std::wstring>(args.begin() + 1, args.end()));
	if (sub == L"install")
		return Install();
	if (sub == L"uninstall")
		return Uninstall();
	if (sub == L"start" || sub == L"stop")
		return Control(sub == L"start");
	if (sub == L"status")
		return Status();
	ServiceUsage();
	return 2;
}
