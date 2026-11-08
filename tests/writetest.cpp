// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// apfsrw-writetest: write to a copy of a test image with the linux-apfs-rw
// engine, then check the result independently.
//
//   apfsrw-writetest <manifest.json> <source.img> <work.img> [volume]
//
// The manifest is the expected state of the untouched image. Every write is
// mirrored into that model; afterwards the whole tree is read back through
// both ApfsLib and a fresh read-only linux-apfs-rw mount and compared with
// the model (type, size, sha256, symlink targets, plus the mode/mtime that
// were set). Exit 0 only if the volume matches. winapfs-fsck is run on the
// result by the CTest wrapper.

#include <apfsrw.h>
#include <winapfs/Apfs.h>
#include <winapfs/Names.h>

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <bcrypt.h>
#include <crtdbg.h>
#include <dbghelp.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace winapfs;
using json = nlohmann::json;

namespace {

struct Expect
{
	std::string type;         // file, dir, symlink, fifo
	std::vector<uint8_t> data; // files we wrote or patched
	bool have_data = false;
	uint64_t size = 0;
	std::string sha256;
	std::string target;
	int mode = -1;            // permission bits to check, -1 = don't
	int orig_mode = -1;       // mode before the test touched it (manifest or 0644 for new files)
	int64_t mtime_ns = INT64_MIN;
	// xattrs as macOS shows them: name -> sha256 of the value
	std::map<std::string, std::string> xattrs;
	bool check_xattrs = false;
	int64_t crtime_ns = INT64_MIN;
	int hidden = -1;          // UF_HIDDEN: 0/1, -1 = don't check
	uint32_t flags = 0;       // BSD flags from the manifest
	uint64_t nlink = 1;
};

std::map<std::string, Expect> g_model;
int g_failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failures++; } } while (0)

std::string Sha256Hex(const void *p, size_t n)
{
	BCRYPT_ALG_HANDLE alg;
	BCRYPT_HASH_HANDLE h;
	uint8_t d[32];
	BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
	BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
	BCryptHashData(h, (PUCHAR)p, static_cast<ULONG>(n), 0);
	BCryptFinishHash(h, d, sizeof(d), 0);
	BCryptDestroyHash(h);
	BCryptCloseAlgorithmProvider(alg, 0);
	static const char *hx = "0123456789abcdef";
	std::string s;
	for (uint8_t b : d)
	{
		s.push_back(hx[b >> 4]);
		s.push_back(hx[b & 15]);
	}
	return s;
}

std::vector<uint8_t> Pattern(uint32_t seed, size_t n)
{
	std::mt19937 rng(seed);
	std::vector<uint8_t> v(n);
	for (auto &b : v)
		b = static_cast<uint8_t>(rng());
	return v;
}

std::string HexDecode(const std::string &h)
{
	std::string out;
	for (size_t i = 0; i + 1 < h.size(); i += 2)
		out.push_back(static_cast<char>(std::stoi(h.substr(i, 2), nullptr, 16)));
	return out;
}

bool Skipped(const std::string &path)
{
	for (const char *s : { "/.fseventsd", "/.Spotlight-V100", "/.Trashes", "/.TemporaryItems" })
	{
		size_t n = strlen(s);
		if (path.compare(0, n, s) == 0 && (path.size() == n || path[n] == '/'))
			return true;
	}
	return false;
}

std::string Parent(const std::string &p) { size_t s = p.rfind('/'); return s == 0 ? "/" : p.substr(0, s); }
std::string Base(const std::string &p) { return p.substr(p.rfind('/') + 1); }

// ---------------------------------------------------------------- backends
//
// The same operations run either directly on the engine (apfsrw API) or
// through a mounted drive with plain Win32 file APIs, the way any Windows
// program writes. Each returns false on failure (and reports it).

struct Backend
{
	virtual ~Backend() = default;
	virtual std::vector<uint8_t> ReadAll(const std::string &path) = 0;
	// Writes data at off in chunks of `chunk` bytes (exercises partial pages).
	virtual bool Write(const std::string &path, uint64_t off, const std::vector<uint8_t> &data, size_t chunk) = 0;
	virtual bool Create(const std::string &path) = 0;
	virtual bool Mkdir(const std::string &path) = 0;
	virtual bool Rename(const std::string &from, const std::string &to) = 0;
	virtual bool Remove(const std::string &path, bool dir) = 0;
	virtual bool SetSize(const std::string &path, uint64_t size) = 0;
	virtual bool SetMtime(const std::string &path, int64_t ns) = 0;
	// Permission bits; the Win32 backend can only toggle "read-only"
	virtual bool SetMode(const std::string &path, int mode) = 0;
	virtual bool CanChmod() const = 0;
	// dir: the target is a directory (Windows makes directory links differently)
	virtual bool Symlink(const std::string &path, const std::string &target, bool dir) = 0;
	virtual bool CanSymlink() const = 0;
	// A link to an absolute path inside the volume; the engine stores it relative
	// to the link (expected_target), which is what the engine backend writes.
	virtual bool SymlinkAbsolute(const std::string &path, const std::string &vol_path, const std::string &expected_target)
	{
		return Symlink(path, expected_target, false);
	}
	virtual bool Link(const std::string &existing, const std::string &path) = 0;
	virtual bool CanLink() const = 0;
	// xattrs (named streams through the drive)
	virtual bool SetXattr(const std::string &path, const std::string &name, const std::vector<uint8_t> &value) = 0;
	virtual bool RemoveXattr(const std::string &path, const std::string &name) = 0;
	virtual bool SetCrtime(const std::string &path, int64_t ns) = 0;
	virtual bool SetHidden(const std::string &path, bool hidden) = 0;
};

struct RwBackend : Backend
{
	apfsrw_mount *m = nullptr;

	apfsrw_node *Walk(const std::string &path)
	{
		apfsrw_node *cur = nullptr;
		apfsrw_root(m, &cur);
		size_t pos = 1;
		while (pos <= path.size() && path.size() > 1)
		{
			size_t next = path.find('/', pos);
			if (next == std::string::npos)
				next = path.size();
			apfsrw_node *child = nullptr;
			int err = apfsrw_lookup(cur, path.data() + pos, next - pos, &child);
			apfsrw_put(cur);
			if (err)
				return nullptr;
			cur = child;
			pos = next + 1;
		}
		return cur;
	}

	std::vector<uint8_t> ReadAll(const std::string &path) override
	{
		std::vector<uint8_t> out;
		apfsrw_node *n = Walk(path);
		apfsrw_stat st;
		apfsrw_file *f = nullptr;
		if (!n || apfsrw_getattr(n, &st) || apfsrw_open(n, false, &f))
		{
			CHECK(false, "cannot read %s", path.c_str());
			if (n)
				apfsrw_put(n);
			return out;
		}
		out.resize(st.size);
		size_t got = 0, off = 0;
		while (off < out.size() && apfsrw_read(f, off, out.data() + off, out.size() - off, &got) == 0 && got)
			off += got;
		CHECK(off == out.size(), "short read of %s", path.c_str());
		apfsrw_release(f);
		apfsrw_put(n);
		return out;
	}

	// The node to write to: a compressed file is first replaced by an
	// uncompressed copy, as the mount does (RwVolume).
	apfsrw_node *WritableNode(const std::string &path)
	{
		apfsrw_node *n = Walk(path);
		apfsrw_stat st;
		if (!n || apfsrw_getattr(n, &st) || !(st.bsd_flags & 0x20))
			return n;
		apfsrw_put(n);
		n = nullptr;
		apfsrw_node *dir = Walk(Parent(path));
		std::string b = Base(path);
		int err = dir ? apfsrw_decompress(dir, b.data(), b.size(), &n) : -2;
		CHECK(err == 0, "decompress %s: %d", path.c_str(), err);
		if (dir)
			apfsrw_put(dir);
		return n;
	}

	bool Write(const std::string &path, uint64_t off, const std::vector<uint8_t> &data, size_t chunk) override
	{
		apfsrw_node *n = WritableNode(path);
		apfsrw_file *f = nullptr;
		CHECK(n && apfsrw_open(n, true, &f) == 0, "open for write %s", path.c_str());
		if (!f)
		{
			if (n)
				apfsrw_put(n);
			return false;
		}
		size_t done = 0;
		bool ok = true;
		while (done < data.size())
		{
			size_t len = std::min(chunk, data.size() - done), w = 0;
			int err = apfsrw_write(f, off + done, data.data() + done, len, &w);
			CHECK(err == 0 && w == len, "write %s at %llu: err %d wrote %zu", path.c_str(), (unsigned long long)(off + done), err, w);
			if (err || !w)
			{
				ok = false;
				break;
			}
			done += w;
		}
		apfsrw_release(f);
		apfsrw_put(n);
		return ok;
	}

	template <typename F> bool InParent(const std::string &path, const char *what, F fn)
	{
		apfsrw_node *dir = Walk(Parent(path));
		std::string name = Base(path);
		int err = dir ? fn(dir, name) : -2;
		CHECK(err == 0, "%s %s: %d", what, path.c_str(), err);
		if (dir)
			apfsrw_put(dir);
		return err == 0;
	}

	bool Create(const std::string &path) override
	{
		return InParent(path, "create", [](apfsrw_node *d, const std::string &n) { return apfsrw_create(d, n.data(), n.size(), 0644, nullptr); });
	}

	bool Mkdir(const std::string &path) override
	{
		return InParent(path, "mkdir", [](apfsrw_node *d, const std::string &n) { return apfsrw_mkdir(d, n.data(), n.size(), 0755, nullptr); });
	}

	bool Remove(const std::string &path, bool dir) override
	{
		return InParent(path, dir ? "rmdir" : "unlink", [dir](apfsrw_node *d, const std::string &n) {
			return dir ? apfsrw_rmdir(d, n.data(), n.size()) : apfsrw_unlink(d, n.data(), n.size());
		});
	}

	bool Symlink(const std::string &path, const std::string &target, bool) override
	{
		return InParent(path, "symlink", [&](apfsrw_node *d, const std::string &n) { return apfsrw_symlink(d, n.data(), n.size(), target.c_str(), nullptr); });
	}

	bool CanSymlink() const override { return true; }

	bool SetXattr(const std::string &path, const std::string &name, const std::vector<uint8_t> &value) override
	{
		apfsrw_node *n = Walk(path);
		int err = n ? apfsrw_setxattr(n, name.c_str(), value.empty() ? "" : (const void *)value.data(), value.size(), 0) : -2;
		CHECK(err == 0, "setxattr %s %s: %d", path.c_str(), name.c_str(), err);
		if (n)
			apfsrw_put(n);
		return err == 0;
	}

	bool RemoveXattr(const std::string &path, const std::string &name) override
	{
		apfsrw_node *n = Walk(path);
		int err = n ? apfsrw_removexattr(n, name.c_str()) : -2;
		CHECK(err == 0, "removexattr %s %s: %d", path.c_str(), name.c_str(), err);
		if (n)
			apfsrw_put(n);
		return err == 0;
	}

	bool SetCrtime(const std::string &path, int64_t ns) override
	{
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_CRTIME;
		a.crtime_ns = ns;
		return Setattr(path, a, "set crtime");
	}

	bool SetHidden(const std::string &path, bool hidden) override
	{
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_FLAGS;
		a.flags = hidden ? 0x8000 : 0;
		a.flags_mask = 0x8000;
		return Setattr(path, a, "set hidden");
	}

	bool Rename(const std::string &from, const std::string &to) override
	{
		apfsrw_node *od = Walk(Parent(from)), *nd = Walk(Parent(to));
		std::string on = Base(from), nn = Base(to);
		int err = (od && nd) ? apfsrw_rename(od, on.data(), on.size(), nd, nn.data(), nn.size(), 0) : -2;
		CHECK(err == 0, "rename %s -> %s: %d", from.c_str(), to.c_str(), err);
		if (od)
			apfsrw_put(od);
		if (nd)
			apfsrw_put(nd);
		return err == 0;
	}

	bool Link(const std::string &existing, const std::string &path) override
	{
		apfsrw_node *src = Walk(existing);
		bool ok = src && InParent(path, "link", [src](apfsrw_node *d, const std::string &n) { return apfsrw_link(src, d, n.data(), n.size(), nullptr); });
		if (src)
			apfsrw_put(src);
		return ok;
	}

	bool Setattr(const std::string &path, const apfsrw_setattr_t &a, const char *what)
	{
		apfsrw_node *n = Walk(path);
		int err = n ? apfsrw_setattr(n, &a) : -2;
		CHECK(err == 0, "%s %s: %d", what, path.c_str(), err);
		if (n)
			apfsrw_put(n);
		return err == 0;
	}

	bool SetSize(const std::string &path, uint64_t size) override
	{
		apfsrw_node *n = WritableNode(path);   // decompresses a compressed file
		if (n)
			apfsrw_put(n);
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_SIZE;
		a.size = size;
		return Setattr(path, a, "truncate");
	}

	bool SetMtime(const std::string &path, int64_t ns) override
	{
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_MTIME;
		a.mtime_ns = ns;
		return Setattr(path, a, "set mtime");
	}

	bool SetMode(const std::string &path, int mode) override
	{
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_MODE;
		a.mode = static_cast<uint32_t>(mode);
		return Setattr(path, a, "chmod");
	}

	bool CanChmod() const override { return true; }
	bool CanLink() const override { return true; }
};

struct Win32Backend : Backend
{
	std::wstring root;   // \\?\X:

	std::wstring Win(const std::string &path)
	{
		std::wstring w = root;
		size_t pos = 1;
		while (pos < path.size())
		{
			size_t next = path.find('/', pos);
			if (next == std::string::npos)
				next = path.size();
			w += L"\\" + ApfsNameToWindows(path.substr(pos, next - pos));
			pos = next + 1;
		}
		return w;
	}

	HANDLE Open(const std::string &path, DWORD access, DWORD disposition, DWORD flags = 0)
	{
		HANDLE h = CreateFileW(Win(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
			flags, nullptr);
		CHECK(h != INVALID_HANDLE_VALUE, "CreateFile %s: error %lu", path.c_str(), GetLastError());
		return h;
	}

	std::vector<uint8_t> ReadAll(const std::string &path) override
	{
		std::vector<uint8_t> out;
		HANDLE h = Open(path, GENERIC_READ, OPEN_EXISTING);
		if (h == INVALID_HANDLE_VALUE)
			return out;
		LARGE_INTEGER size;
		GetFileSizeEx(h, &size);
		out.resize(static_cast<size_t>(size.QuadPart));
		size_t off = 0;
		while (off < out.size())
		{
			DWORD got = 0;
			if (!ReadFile(h, out.data() + off, static_cast<DWORD>(std::min<size_t>(out.size() - off, 1 << 20)), &got, nullptr) || !got)
				break;
			off += got;
		}
		CHECK(off == out.size(), "short read of %s", path.c_str());
		CloseHandle(h);
		return out;
	}

	bool Write(const std::string &path, uint64_t off, const std::vector<uint8_t> &data, size_t chunk) override
	{
		HANDLE h = Open(path, GENERIC_WRITE, OPEN_EXISTING);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		size_t done = 0;
		bool ok = true;
		while (done < data.size())
		{
			DWORD len = static_cast<DWORD>(std::min(chunk, data.size() - done)), w = 0;
			OVERLAPPED ov = {};
			ov.Offset = static_cast<DWORD>(off + done);
			ov.OffsetHigh = static_cast<DWORD>((off + done) >> 32);
			if (!WriteFile(h, data.data() + done, len, &w, &ov) || w != len)
			{
				CHECK(false, "WriteFile %s at %llu: error %lu", path.c_str(), (unsigned long long)(off + done), GetLastError());
				ok = false;
				break;
			}
			done += w;
		}
		CloseHandle(h);
		return ok;
	}

	bool Create(const std::string &path) override
	{
		HANDLE h = Open(path, GENERIC_WRITE, CREATE_NEW);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		CloseHandle(h);
		return true;
	}

	bool Mkdir(const std::string &path) override
	{
		bool ok = CreateDirectoryW(Win(path).c_str(), nullptr) != 0;
		CHECK(ok, "CreateDirectory %s: error %lu", path.c_str(), GetLastError());
		return ok;
	}

	bool Rename(const std::string &from, const std::string &to) override
	{
		bool ok = MoveFileExW(Win(from).c_str(), Win(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
		CHECK(ok, "MoveFileEx %s -> %s: error %lu", from.c_str(), to.c_str(), GetLastError());
		return ok;
	}

	bool Remove(const std::string &path, bool dir) override
	{
		bool ok = dir ? RemoveDirectoryW(Win(path).c_str()) != 0 : DeleteFileW(Win(path).c_str()) != 0;
		CHECK(ok, "%s %s: error %lu", dir ? "RemoveDirectory" : "DeleteFile", path.c_str(), GetLastError());
		return ok;
	}

	bool SetSize(const std::string &path, uint64_t size) override
	{
		HANDLE h = Open(path, GENERIC_WRITE, OPEN_EXISTING);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		FILE_END_OF_FILE_INFO eof;
		eof.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
		bool ok = SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof(eof)) != 0;
		CHECK(ok, "set size %s: error %lu", path.c_str(), GetLastError());
		CloseHandle(h);
		return ok;
	}

	bool SetMtime(const std::string &path, int64_t ns) override
	{
		HANDLE h = Open(path, FILE_WRITE_ATTRIBUTES, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		ULARGE_INTEGER u;
		u.QuadPart = static_cast<ULONGLONG>(ns / 100 + 116444736000000000LL);
		FILETIME ft = { u.LowPart, u.HighPart };
		bool ok = SetFileTime(h, nullptr, nullptr, &ft) != 0;
		CHECK(ok, "SetFileTime %s: error %lu", path.c_str(), GetLastError());
		CloseHandle(h);
		return ok;
	}

	// only the read-only attribute: no write bits -> read-only
	bool SetMode(const std::string &path, int mode) override
	{
		DWORD attrs = GetFileAttributesW(Win(path).c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES)
			return false;
		attrs = (mode & 0222) ? (attrs & ~FILE_ATTRIBUTE_READONLY) : (attrs | FILE_ATTRIBUTE_READONLY);
		if (!attrs)
			attrs = FILE_ATTRIBUTE_NORMAL;
		bool ok = SetFileAttributesW(Win(path).c_str(), attrs) != 0;
		CHECK(ok, "SetFileAttributes %s: error %lu", path.c_str(), GetLastError());
		return ok;
	}

	bool CanChmod() const override { return false; }

	// Symlinks need SeCreateSymbolicLinkPrivilege or Developer Mode; probed once.
	bool can_symlink = false;
	void ProbeSymlink()
	{
		std::wstring probe = root + L"\\.winapfs-symlink-probe";
		can_symlink = CreateSymbolicLinkW(probe.c_str(), L"x", SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
		if (can_symlink)
			DeleteFileW(probe.c_str());
		else
			printf("note: no symlink privilege (error %lu); symlink creation through the drive is not tested\n", GetLastError());
	}
	bool CanSymlink() const override { return can_symlink; }

	static std::wstring WinTarget(const std::string &target)
	{
		std::wstring w;
		size_t pos = 0;
		while (pos <= target.size())
		{
			size_t next = target.find('/', pos);
			if (next == std::string::npos)
				next = target.size();
			std::string c = target.substr(pos, next - pos);
			if (pos > 0)
				w.push_back(L'\\');
			w += (c == "." || c == "..") ? Utf8ToWide(c) : ApfsNameToWindows(c);
			pos = next + 1;
		}
		return w;
	}

	bool MakeLink(const std::string &path, const std::wstring &target, bool dir)
	{
		DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
		bool ok = CreateSymbolicLinkW(Win(path).c_str(), target.c_str(), flags) != 0;
		CHECK(ok, "CreateSymbolicLink %s: error %lu", path.c_str(), GetLastError());
		return ok;
	}

	bool Symlink(const std::string &path, const std::string &target, bool dir) override
	{
		return MakeLink(path, WinTarget(target), dir);
	}

	bool SymlinkAbsolute(const std::string &path, const std::string &vol_path, const std::string &) override
	{
		// "\\?\Z:" + "\w3\x": a full Windows path into the mounted volume
		std::wstring t = root.substr(4) + WinTarget(vol_path);
		return MakeLink(path, t, false);
	}

	bool Link(const std::string &, const std::string &) override { return false; }   // WinFsp has no hard links
	bool CanLink() const override { return false; }

	bool SetXattr(const std::string &path, const std::string &name, const std::vector<uint8_t> &value) override
	{
		std::wstring s = Win(path) + L":" + ApfsNameToWindows(name);
		HANDLE h = CreateFileW(s.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
			FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		CHECK(h != INVALID_HANDLE_VALUE, "create stream %s:%s: error %lu", path.c_str(), name.c_str(), GetLastError());
		if (h == INVALID_HANDLE_VALUE)
			return false;
		size_t done = 0;
		bool ok = true;
		while (ok && done < value.size())
		{
			DWORD w = 0, len = static_cast<DWORD>(std::min<size_t>(value.size() - done, 7000));   // several writes
			ok = WriteFile(h, value.data() + done, len, &w, nullptr) && w == len;
			done += w;
		}
		CHECK(ok, "write stream %s:%s: error %lu", path.c_str(), name.c_str(), GetLastError());
		CloseHandle(h);
		return ok;
	}

	bool RemoveXattr(const std::string &path, const std::string &name) override
	{
		bool ok = DeleteFileW((Win(path) + L":" + ApfsNameToWindows(name)).c_str()) != 0;
		CHECK(ok, "delete stream %s:%s: error %lu", path.c_str(), name.c_str(), GetLastError());
		return ok;
	}

	bool SetCrtime(const std::string &path, int64_t ns) override
	{
		HANDLE h = Open(path, FILE_WRITE_ATTRIBUTES, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		ULARGE_INTEGER u;
		u.QuadPart = static_cast<ULONGLONG>(ns / 100 + 116444736000000000LL);
		FILETIME ft = { u.LowPart, u.HighPart };
		bool ok = SetFileTime(h, &ft, nullptr, nullptr) != 0;
		CHECK(ok, "SetFileTime(creation) %s: error %lu", path.c_str(), GetLastError());
		CloseHandle(h);
		return ok;
	}

	bool SetHidden(const std::string &path, bool hidden) override
	{
		DWORD attrs = GetFileAttributesW(Win(path).c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES)
			return false;
		attrs = hidden ? (attrs | FILE_ATTRIBUTE_HIDDEN) : (attrs & ~FILE_ATTRIBUTE_HIDDEN);
		attrs &= ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
		if (!attrs)
			attrs = FILE_ATTRIBUTE_NORMAL;
		bool ok = SetFileAttributesW(Win(path).c_str(), attrs) != 0;
		CHECK(ok, "SetFileAttributes(hidden) %s: error %lu", path.c_str(), GetLastError());
		return ok;
	}
};

Backend *g_be = nullptr;

// WRITETEST_TRACE=1: print each operation and validate the heap after it
bool g_trace = getenv("WRITETEST_TRACE") != nullptr;

struct TraceOp
{
	const char *op;
	std::string path;
	TraceOp(const char *o, const std::string &p) : op(o), path(p)
	{
		if (g_trace)
			fprintf(stderr, "%s %s\n", op, path.c_str());
	}
	~TraceOp()
	{
#ifdef _DEBUG
		if (g_trace && !_CrtCheckMemory())
		{
			fprintf(stderr, "CRT HEAP CORRUPTED after %s %s\n", op, path.c_str());
			fflush(stderr);
			abort();
		}
#endif
		if (g_trace && !HeapValidate(GetProcessHeap(), 0, nullptr))
		{
			fprintf(stderr, "HEAP CORRUPTED after %s %s\n", op, path.c_str());
			fflush(stderr);
			abort();
		}
	}
};

// ---------------------------------------------------------------- operations
// Each runs on the backend and mirrors its effect into the model.

// New contents; the metadata (mode, times, xattrs, flags) stays.
void ModelFile(const std::string &path, const std::vector<uint8_t> &data)
{
	Expect &e = g_model[path];
	e.type = "file";
	e.data = data;
	e.have_data = true;
	e.size = data.size();
	e.sha256.clear();
}

void OpCreate(const std::string &path, const std::vector<uint8_t> &data, size_t chunk)
{
	TraceOp t("create", path);
	if (g_be->Create(path) && !data.empty())
		g_be->Write(path, 0, data, chunk);
	g_model[path] = Expect();
	ModelFile(path, data);
	Expect &e = g_model[path];
	e.orig_mode = 0644;   // both backends create files 0644
	e.check_xattrs = true;   // a new file has none
	e.hidden = 0;
}

void OpXattr(const std::string &path, const std::string &name, const std::vector<uint8_t> &value)
{
	TraceOp t("setxattr", path + " " + name);
	g_be->SetXattr(path, name, value);
	g_model[path].xattrs[name] = Sha256Hex(value.data(), value.size());
}

void OpRmXattr(const std::string &path, const std::string &name)
{
	TraceOp t("removexattr", path + " " + name);
	g_be->RemoveXattr(path, name);
	g_model[path].xattrs.erase(name);
}

void OpCrtime(const std::string &path, int64_t ns)
{
	TraceOp t("crtime", path);
	g_be->SetCrtime(path, ns);
	g_model[path].crtime_ns = ns;
}

void OpHidden(const std::string &path, bool hidden)
{
	TraceOp t("hidden", path);
	g_be->SetHidden(path, hidden);
	g_model[path].hidden = hidden ? 1 : 0;
}

void OpPatch(const std::string &path, uint64_t off, const std::vector<uint8_t> &patch, size_t chunk)
{
	TraceOp t("patch", path);
	std::vector<uint8_t> cur = g_be->ReadAll(path);
	g_be->Write(path, off, patch, chunk);
	if (cur.size() < off + patch.size())
		cur.resize(off + patch.size(), 0);
	memcpy(cur.data() + off, patch.data(), patch.size());
	ModelFile(path, cur);
}

void OpTruncate(const std::string &path, uint64_t size)
{
	TraceOp t("truncate", path);
	std::vector<uint8_t> cur = g_be->ReadAll(path);
	g_be->SetSize(path, size);
	cur.resize(size, 0);
	ModelFile(path, cur);
}

void OpMkdir(const std::string &path)
{
	TraceOp t("mkdir", path);
	g_be->Mkdir(path);
	g_model[path] = Expect();
	g_model[path].type = "dir";
	g_model[path].check_xattrs = true;
	g_model[path].hidden = 0;
}

void OpRename(const std::string &from, const std::string &to)
{
	TraceOp t("rename", from + " -> " + to);
	g_be->Rename(from, to);
	// move the entry and its subtree
	std::map<std::string, Expect> moved;
	for (auto it = g_model.begin(); it != g_model.end();)
	{
		const std::string &p = it->first;
		if (p == from || (p.size() > from.size() && p.compare(0, from.size(), from) == 0 && p[from.size()] == '/'))
		{
			moved[to + p.substr(from.size())] = it->second;
			it = g_model.erase(it);
		}
		else
			++it;
	}
	for (auto &kv : moved)
		g_model[kv.first] = kv.second;
}

void OpUnlink(const std::string &path, bool dir = false)
{
	TraceOp t(dir ? "rmdir" : "unlink", path);
	g_be->Remove(path, dir);
	g_model.erase(path);
}

void OpSymlink(const std::string &path, const std::string &target, bool dir = false)
{
	TraceOp t("symlink", path);
	if (!g_be->CanSymlink())
		return;
	g_be->Symlink(path, target, dir);
	g_model[path] = Expect();
	g_model[path].type = "symlink";
	g_model[path].target = target;
}

void OpSymlinkAbsolute(const std::string &path, const std::string &vol_path, const std::string &expected_target)
{
	TraceOp t("symlink-abs", path);
	if (!g_be->CanSymlink())
		return;
	g_be->SymlinkAbsolute(path, vol_path, expected_target);
	g_model[path] = Expect();
	g_model[path].type = "symlink";
	g_model[path].target = expected_target;
}

void OpLink(const std::string &existing, const std::string &path)
{
	TraceOp t("link", path);
	if (!g_be->CanLink())
		return;
	g_be->Link(existing, path);
	g_model[path] = g_model[existing];
}

// mode < 0 or mtime INT64_MIN: leave that alone. Without chmod support the
// mode only toggles read-only, so the expected mode is derived from the old one.
void OpSetattr(const std::string &path, int mode, int64_t mtime_ns)
{
	TraceOp t("setattr", path);
	if (mode >= 0)
	{
		g_be->SetMode(path, mode);
		if (g_be->CanChmod())
			g_model[path].mode = mode;
		else if (g_model[path].mode >= 0 || g_model[path].orig_mode >= 0)
		{
			int old = g_model[path].mode >= 0 ? g_model[path].mode : g_model[path].orig_mode;
			g_model[path].mode = (mode & 0222) ? ((old & 0222) ? old : (old | 0200)) : (old & ~0222);
		}
	}
	if (mtime_ns != INT64_MIN)
	{
		g_be->SetMtime(path, mtime_ns);
		g_model[path].mtime_ns = mtime_ns;
	}
}

// ---------------------------------------------------------------- crash phase
//
// After the synced main phase: churn under /crash (creates of up to 2 MiB,
// patches, truncation, renames, deletes, subdirectories, a sync every 50
// operations) while a simulated crash is armed. Whatever state /crash ends in
// is acceptable; everything outside it was synced and must survive.

bool g_ignore_crash = false;   // verification: entries under /crash may exist

uint64_t FreeBytes(apfsrw_mount *m)
{
	apfsrw_statfs st = {};
	apfsrw_get_statfs(m, &st);
	return st.blocks_free * st.block_size;
}

void Churn(apfsrw_mount *m)
{
	std::mt19937 rng(1);
	std::vector<std::string> files;
	int dirs = 0;
	// file sizes scale with the free space (the smallest test container has 31 MiB)
	uint64_t free_bytes = FreeBytes(m);
	size_t big_max = static_cast<size_t>(std::min<uint64_t>(2 * 1024 * 1024, free_bytes / 60));
	size_t small_max = static_cast<size_t>(std::min<uint64_t>(40000, free_bytes / 600));
	OpMkdir("/crash");
	for (int i = 0; i < 400; i++)
	{
		unsigned what = rng() % 10;
		char name[64];
		snprintf(name, sizeof(name), "/crash/f%04d", i);
		size_t pick = files.empty() ? 0 : rng() % files.size();
		if (what < 4 || files.empty())
		{
			size_t size = rng() % (what == 0 ? big_max : small_max);
			OpCreate(name, Pattern(1000 + i, size), 4096 + rng() % 60000);
			files.push_back(name);
		}
		else if (what < 6)
			OpPatch(files[pick], rng() % 50000, Pattern(5000 + i, 1 + rng() % 20000), 8192);
		else if (what == 6)
			OpTruncate(files[pick], rng() % 30000);
		else if (what == 7)
		{
			OpRename(files[pick], files[pick] + "r");
			files[pick] += "r";
		}
		else if (what == 8)
		{
			OpUnlink(files[pick]);
			files.erase(files.begin() + pick);
		}
		else
		{
			std::string d = "/crash/d" + std::to_string(dirs++);
			OpMkdir(d);
			OpCreate(d + "/x", Pattern(9000 + i, rng() % 100000), 16384);
			files.push_back(d + "/x");
		}
		if (i % 50 == 49)
			apfsrw_sync(m);
	}
}

// Disk full: write 1 MiB files under /crash until ENOSPC, delete them all,
// check that the space comes back, and write again.
void Fill(apfsrw_mount *m, RwBackend &be)
{
	OpMkdir("/crash");
	uint64_t before = FreeBytes(m);
	std::vector<uint8_t> chunk = Pattern(42, 1024 * 1024);
	std::vector<std::string> made;
	int err = 0;
	apfsrw_node *dir = be.Walk("/crash");
	CHECK(dir != nullptr, "no /crash");
	for (int i = 0; dir && !err && i < 100000; i++)
	{
		std::string name = "fill" + std::to_string(i);
		apfsrw_node *n = nullptr;
		apfsrw_file *f = nullptr;
		err = apfsrw_create(dir, name.data(), name.size(), 0644, &n);
		if (err)
			break;
		made.push_back(name);
		err = apfsrw_open(n, true, &f);
		CHECK(err == 0, "open %s: %d", name.c_str(), err);
		size_t w = 0;
		for (uint64_t off = 0; !err && off < 8 * chunk.size(); off += w)
		{
			err = apfsrw_write(f, off, chunk.data(), chunk.size(), &w);
			if (!err && w == 0)
				err = -28;
		}
		if (f)
			apfsrw_release(f);
		apfsrw_put(n);
	}
	CHECK(err == -28, "filling the volume ended with %d, expected ENOSPC (-28)", err);
	uint64_t full = FreeBytes(m);
	for (const std::string &name : made)
	{
		int e = apfsrw_unlink(dir, name.data(), name.size());
		CHECK(e == 0, "unlink %s after ENOSPC: %d", name.c_str(), e);
	}
	if (dir)
		apfsrw_put(dir);
	// the data of deleted files is freed by the driver's orphan cleanup in the
	// background (as on Linux), and freed blocks only count once the free
	// queue is flushed at the start of a later transaction
	uint64_t after = 0;
	for (int tries = 0; tries < 200; tries++)
	{
		CHECK(apfsrw_sync(m) == 0, "sync after deleting");
		after = FreeBytes(m);
		if (after + 256 * 1024 >= before)
			break;
		Sleep(100);
	}
	printf("disk full: %zu files, free %llu -> %llu -> %llu KiB\n", made.size(), (unsigned long long)before / 1024,
		(unsigned long long)full / 1024, (unsigned long long)after / 1024);
	CHECK(after + 256 * 1024 >= before, "space not given back: %llu KiB free, %llu before",
		(unsigned long long)after / 1024, (unsigned long long)before / 1024);
	OpCreate("/crash/after-full.bin", Pattern(43, 3 * 1024 * 1024 + 7), 65536);
	CHECK(be.ReadAll("/crash/after-full.bin") == Pattern(43, 3 * 1024 * 1024 + 7), "write after disk full reads back wrong");
}

// ---------------------------------------------------------------- verification

struct Seen
{
	std::string type;
	uint64_t size = 0;
	std::string sha256;
	std::string target;
	int mode = 0;
	int64_t mtime_ns = 0;
	std::map<std::string, std::string> xattrs;
	int64_t crtime_ns = 0;
	int hidden = 0;
};

void WalkVolume(Volume &v, uint64_t ino, const std::string &path, std::map<std::string, Seen> &out, int depth = 0)
{
	std::vector<DirEntry> ents;
	if (!v.ListDir(ino, ents))
	{
		CHECK(false, "[%s] cannot list %s", v.EngineName(), path.c_str());
		return;
	}
	for (const auto &e : ents)
	{
		std::string p = (path == "/" ? "" : path) + "/" + e.name;
		if (Skipped(p))
			continue;
		Stat st;
		if (!v.GetStat(e.inode, st))
		{
			CHECK(false, "[%s] cannot stat %s", v.EngineName(), p.c_str());
			continue;
		}
		Seen s;
		s.type = st.type == FileType::Dir ? "dir" : st.type == FileType::Symlink ? "symlink" : st.type == FileType::Fifo ? "fifo" : "file";
		s.mode = st.mode & 07777;
		s.mtime_ns = st.mod_ns;
		s.crtime_ns = st.create_ns;
		s.hidden = (st.bsd_flags & kUF_HIDDEN) ? 1 : 0;
		std::vector<std::string> xnames;
		CHECK(v.ListXattrs(e.inode, st, xnames), "[%s] cannot list xattrs of %s", v.EngineName(), p.c_str());
		for (const auto &xn : xnames)
		{
			std::vector<uint8_t> xv;
			CHECK(v.GetXattr(e.inode, xn, xv), "[%s] cannot read xattr %s of %s", v.EngineName(), xn.c_str(), p.c_str());
			s.xattrs[xn] = Sha256Hex(xv.data(), xv.size());
		}
		if (s.type == "file")
		{
			s.size = st.size;
			std::string err;
			std::unique_ptr<FileReader> r = v.OpenFile(st, err);
			std::vector<uint8_t> data(st.size);
			size_t off = 0, got = 0;
			while (r && off < data.size() && r->Read(off, data.data() + off, data.size() - off, got) && got)
				off += got;
			CHECK(r && off == data.size(), "[%s] cannot read %s (%s)", v.EngineName(), p.c_str(), err.c_str());
			s.sha256 = Sha256Hex(data.data(), data.size());
		}
		else if (s.type == "symlink")
			v.ReadLink(e.inode, s.target);
		out[p] = s;
		if (s.type == "dir" && depth < 512)
			WalkVolume(v, e.inode, p, out, depth + 1);
	}
}

void Compare(Volume &v)
{
	std::map<std::string, Seen> seen;
	WalkVolume(v, kRootInode, "/", seen);
	int before = g_failures;
	for (const auto &kv : g_model)
	{
		auto it = seen.find(kv.first);
		if (it == seen.end())
		{
			CHECK(false, "[%s] missing %s", v.EngineName(), kv.first.c_str());
			continue;
		}
		const Expect &e = kv.second;
		const Seen &s = it->second;
		CHECK(e.type == s.type, "[%s] %s: type %s, expected %s", v.EngineName(), kv.first.c_str(), s.type.c_str(), e.type.c_str());
		if (e.type == "file")
		{
			uint64_t size = e.have_data ? e.data.size() : e.size;
			std::string sha = e.have_data ? Sha256Hex(e.data.data(), e.data.size()) : e.sha256;
			CHECK(s.size == size, "[%s] %s: size %llu, expected %llu", v.EngineName(), kv.first.c_str(), (unsigned long long)s.size, (unsigned long long)size);
			CHECK(s.sha256 == sha, "[%s] %s: contents differ", v.EngineName(), kv.first.c_str());
		}
		if (e.type == "symlink")
			CHECK(s.target == e.target, "[%s] %s: target '%s', expected '%s'", v.EngineName(), kv.first.c_str(), s.target.c_str(), e.target.c_str());
		if (e.mode >= 0)
			CHECK(s.mode == e.mode, "[%s] %s: mode %o, expected %o", v.EngineName(), kv.first.c_str(), s.mode, e.mode);
		if (e.mtime_ns != INT64_MIN)
			CHECK(s.mtime_ns == e.mtime_ns, "[%s] %s: mtime %lld, expected %lld", v.EngineName(), kv.first.c_str(), (long long)s.mtime_ns, (long long)e.mtime_ns);
		if (e.crtime_ns != INT64_MIN)
			CHECK(s.crtime_ns == e.crtime_ns, "[%s] %s: creation time %lld, expected %lld", v.EngineName(), kv.first.c_str(), (long long)s.crtime_ns, (long long)e.crtime_ns);
		if (e.hidden >= 0)
			CHECK(s.hidden == e.hidden, "[%s] %s: hidden %d, expected %d", v.EngineName(), kv.first.c_str(), s.hidden, e.hidden);
		if (e.check_xattrs)
		{
			for (const auto &x : e.xattrs)
			{
				auto it2 = s.xattrs.find(x.first);
				CHECK(it2 != s.xattrs.end(), "[%s] %s: missing xattr %s", v.EngineName(), kv.first.c_str(), x.first.c_str());
				CHECK(it2 == s.xattrs.end() || it2->second == x.second, "[%s] %s: xattr %s differs", v.EngineName(), kv.first.c_str(), x.first.c_str());
			}
			for (const auto &x : s.xattrs)
				CHECK(e.xattrs.count(x.first), "[%s] %s: unexpected xattr %s", v.EngineName(), kv.first.c_str(), x.first.c_str());
		}
	}
	for (const auto &kv : seen)
		CHECK(g_model.count(kv.first) || (g_ignore_crash && kv.first.compare(0, 6, "/crash") == 0),
			"[%s] unexpected %s", v.EngineName(), kv.first.c_str());
	printf("[%s] %zu entries checked, %d problems\n", v.EngineName(), seen.size(), g_failures - before);
}

// ---------------------------------------------------------------- model files
// (the Win32 phase runs while the drive is mounted; checking comes after)

std::string HexEncode(const std::string &s)
{
	static const char *hx = "0123456789abcdef";
	std::string out;
	for (unsigned char c : s)
	{
		out.push_back(hx[c >> 4]);
		out.push_back(hx[c & 15]);
	}
	return out;
}

void SaveModel(const std::wstring &file)
{
	json doc = json::array();
	for (const auto &kv : g_model)
	{
		const Expect &e = kv.second;
		json j;
		j["path_hex"] = HexEncode(kv.first);
		j["type"] = e.type;
		if (e.type == "file")
		{
			j["size"] = e.have_data ? e.data.size() : e.size;
			j["sha256"] = e.have_data ? Sha256Hex(e.data.data(), e.data.size()) : e.sha256;
		}
		if (e.type == "symlink")
			j["target"] = e.target;
		if (e.mode >= 0)
			j["mode"] = e.mode;
		if (e.mtime_ns != INT64_MIN)
			j["mtime_ns"] = e.mtime_ns;
		if (e.crtime_ns != INT64_MIN)
			j["crtime_ns"] = e.crtime_ns;
		if (e.hidden >= 0)
			j["hidden"] = e.hidden;
		if (e.check_xattrs)
		{
			json x = json::object();
			for (const auto &kv2 : e.xattrs)
				x[HexEncode(kv2.first)] = kv2.second;
			j["xattrs_hex"] = x;
		}
		doc.push_back(j);
	}
	std::ofstream(file) << doc.dump(1);
}

bool LoadModel(const std::wstring &file)
{
	std::ifstream in(file);
	if (!in)
		return false;
	json doc = json::parse(in);
	g_model.clear();
	for (const auto &j : doc)
	{
		Expect e;
		e.type = j["type"].get<std::string>();
		if (j.contains("size"))
			e.size = j["size"].get<uint64_t>();
		if (j.contains("sha256"))
			e.sha256 = j["sha256"].get<std::string>();
		if (j.contains("target"))
			e.target = j["target"].get<std::string>();
		if (j.contains("mode"))
			e.mode = j["mode"].get<int>();
		if (j.contains("mtime_ns"))
			e.mtime_ns = j["mtime_ns"].get<int64_t>();
		if (j.contains("crtime_ns"))
			e.crtime_ns = j["crtime_ns"].get<int64_t>();
		if (j.contains("hidden"))
			e.hidden = j["hidden"].get<int>();
		if (j.contains("xattrs_hex"))
		{
			e.check_xattrs = true;
			for (auto it = j["xattrs_hex"].begin(); it != j["xattrs_hex"].end(); ++it)
				e.xattrs[HexDecode(it.key())] = it.value().get<std::string>();
		}
		g_model[HexDecode(j["path_hex"].get<std::string>())] = e;
	}
	return true;
}

// Walk the image through both engines and compare with the model.
void ReadBack(const std::string &work, const std::string &volname)
{
	std::string err;
	std::unique_ptr<Container> c = Container::Open(work, err);
	if (!c)
	{
		CHECK(false, "reopen: %s", err.c_str());
		return;
	}
	const VolumeInfo *vi = volname.empty() ? &c->Volumes().front() : c->FindVolume(volname);
	if (!vi)
	{
		CHECK(false, "no volume %s", volname.c_str());
		return;
	}
	{
		std::unique_ptr<Volume> lib = c->OpenVolume(vi->fsid, "", err);
		CHECK(lib != nullptr, "apfs-fuse open: %s", err.c_str());
		if (lib)
			Compare(*lib);
	}
	{
		std::unique_ptr<Volume> rw = OpenRwVolume(*c, vi->fsid, false, err);
		CHECK(rw != nullptr, "linux-apfs-rw reopen: %s", err.c_str());
		if (rw)
			Compare(*rw);
	}
}

int VerifyPhase(const std::wstring &model, const std::string &work, const std::string &volname)
{
	if (!LoadModel(model))
	{
		fprintf(stderr, "cannot read model %ls\n", model.c_str());
		return 2;
	}
	ReadBack(work, volname);
	printf("%s: %d failures\n", g_failures ? "FAILED" : "OK", g_failures);
	return g_failures ? 1 : 0;
}

// On a crash: print the exception and a symbolized stack (DbgHelp + PDBs).
LONG WINAPI CrashHandler(EXCEPTION_POINTERS *ep)
{
	HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
	CONTEXT ctx = *ep->ContextRecord;
	STACKFRAME64 frame = {};
	fprintf(stderr, "\n*** exception 0x%08lx at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
	if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
		fprintf(stderr, " (%s address %p)", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
			(void *)ep->ExceptionRecord->ExceptionInformation[1]);
	fprintf(stderr, "\n");
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
	SymInitialize(proc, nullptr, TRUE);
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
	frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
	for (int i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr,
			SymFunctionTableAccess64, SymGetModuleBase64, nullptr); i++)
	{
		char buf[sizeof(SYMBOL_INFO) + 256];
		SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		DWORD ldisp = 0;
		IMAGEHLP_LINE64 line = { sizeof(line) };
		const char *name = SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
		if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &ldisp, &line))
			fprintf(stderr, "  %2d %s  %s:%lu\n", i, name, line.FileName, line.LineNumber);
		else
			fprintf(stderr, "  %2d %s\n", i, name);
	}
	fflush(stderr);
	return EXCEPTION_EXECUTE_HANDLER;
}

void Log(int level, const char *msg)
{
	if (level <= 4)
		fprintf(stderr, "[apfs] %s%s", msg, (*msg && msg[strlen(msg) - 1] == '\n') ? "" : "\n");
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
#ifdef _DEBUG
	// Debug builds: check the CRT heap regularly and report overruns (with
	// the allocating file and line) on stderr instead of in a dialog box.
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF);   // guard bytes checked on free; TraceOp checks fully
	for (int t : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT })
	{
		_CrtSetReportMode(t, _CRTDBG_MODE_FILE);
		_CrtSetReportFile(t, _CRTDBG_FILE_STDERR);
	}
#endif
	SetUnhandledExceptionFilter(CrashHandler);
	// modes:
	//   <manifest> <source.img> <work.img> [vol]            engine: copy, write, verify
	//   --win32 <X:> <manifest> <work.img> <model.json> [vol] write through a mounted drive, save the model
	//   --verify <manifest> <work.img> <model.json> [vol]     check an image against a saved model
	//   --crash <count|kill|power|nobarrier|fill> <at> <seed> <manifest> <source.img> <work.img> <model.json> [vol]
	//        engine writes, sync, save the model, then churn under /crash with a
	//        simulated crash at device write <at> (exit 99); prints the write count
	//   --verify-crash ...        like --verify, but entries under /crash may exist
	//   --recover <work.img> [vol] mount read-write, write a file, unmount cleanly
	enum { ENGINE, WIN32_DRIVE, VERIFY, CRASH, RECOVER } mode = ENGINE;
	std::vector<std::wstring> args(argv + 1, argv + argc);
	std::wstring drive;
	int crash_how = 0;
	uint64_t crash_at = 0;
	uint32_t crash_seed = 0;
	if (!args.empty() && args[0] == L"--win32" && args.size() >= 2)
	{
		mode = WIN32_DRIVE;
		drive = args[1];
		args.erase(args.begin(), args.begin() + 2);
	}
	else if (!args.empty() && (args[0] == L"--verify" || args[0] == L"--verify-crash"))
	{
		mode = VERIFY;
		g_ignore_crash = args[0] == L"--verify-crash";
		args.erase(args.begin());
	}
	else if (!args.empty() && args[0] == L"--crash" && args.size() >= 4)
	{
		mode = CRASH;
		crash_how = args[1] == L"kill" ? APFSRW_CRASH_KILL : args[1] == L"power" ? APFSRW_CRASH_POWER
			: args[1] == L"nobarrier" ? APFSRW_CRASH_NOBARRIER : args[1] == L"fill" ? 0 : APFSRW_CRASH_COUNT;
		crash_at = _wcstoui64(args[2].c_str(), nullptr, 10);
		crash_seed = static_cast<uint32_t>(wcstoul(args[3].c_str(), nullptr, 10));
		args.erase(args.begin(), args.begin() + 4);
	}
	else if (!args.empty() && args[0] == L"--recover")
	{
		mode = RECOVER;
		args.erase(args.begin());
		args.insert(args.begin(), L"");   // no manifest
	}
	const size_t need = mode == RECOVER ? 2 : mode == CRASH ? 4 : 3;
	if (args.size() < need)
	{
		fprintf(stderr, "usage: apfsrw-writetest <manifest.json> <source.img> <work.img> [volume]\n"
			"       apfsrw-writetest --win32 <X:> <manifest.json> <work.img> <model.json> [volume]\n"
			"       apfsrw-writetest --verify[-crash] <manifest.json> <work.img> <model.json> [volume]\n"
			"       apfsrw-writetest --crash <count|kill|power|nobarrier|fill> <at> <seed> <manifest.json> <source.img> <work.img> <model.json> [volume]\n"
			"       apfsrw-writetest --recover <work.img> [volume]\n");
		return 2;
	}
	std::wstring w_manifest = args[0];
	std::wstring w_work = (mode == ENGINE || mode == CRASH) ? args[2] : args[1];
	std::wstring w_model = mode == ENGINE || mode == RECOVER ? L"" : mode == CRASH ? args[3] : args[2];
	size_t vol_arg = mode == RECOVER ? 2 : mode == CRASH ? 4 : 3;
	std::string work = WideToUtf8(w_work);
	std::string volname = args.size() > vol_arg ? WideToUtf8(args[vol_arg]) : "";
	apfsrw_set_log(Log);

	if (mode == VERIFY)
		return VerifyPhase(w_model, work, volname);

	auto mount_rw = [&](apfsrw_mount **out) {
		std::string err;
		std::unique_ptr<Container> c = Container::Open(work, err);
		if (!c)
		{
			fprintf(stderr, "%s\n", err.c_str());
			return -1;
		}
		const VolumeInfo *vi = volname.empty() ? &c->Volumes().front() : c->FindVolume(volname);
		if (!vi)
		{
			fprintf(stderr, "no volume %s\n", volname.c_str());
			return -1;
		}
		uint64_t offset = c->PartitionOffset(), size = c->PartitionSize();
		unsigned fsid = vi->fsid;
		c.reset();
		int rc = apfsrw_mount_volume(w_work.c_str(), offset, size, fsid, true, nullptr, out);
		if (rc)
			fprintf(stderr, "read-write mount failed: %d\n", rc);
		return rc;
	};

	if (mode == RECOVER)
	{
		// after a crash: the engine must mount the last checkpoint and write on
		RwBackend rec;
		if (mount_rw(&rec.m))
			return 1;
		g_be = &rec;
		OpCreate("/crash-recovered.txt", Pattern(77, 12345), 4096);
		std::vector<uint8_t> back = rec.ReadAll("/crash-recovered.txt");
		CHECK(back == Pattern(77, 12345), "recovered write reads back wrong");
		int rc = apfsrw_unmount(rec.m);
		CHECK(rc == 0, "unmount: %d", rc);
		printf("%s: remounted read-write, wrote, unmounted (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
		return g_failures ? 1 : 0;
	}

	if ((mode == ENGINE || mode == CRASH) && !CopyFileW(args[1].c_str(), w_work.c_str(), FALSE))
	{
		fprintf(stderr, "cannot copy image (error %lu)\n", GetLastError());
		return 2;
	}

	// the untouched state, from the manifest
	std::ifstream mf(w_manifest);
	json doc = json::parse(mf);
	for (const auto &j : doc["entries"])
	{
		if (j["path"] == "/")
			continue;
		std::string path = "/" + HexDecode(j["path_utf8_hex"].get<std::string>());
		Expect e;
		e.type = j["type"].get<std::string>();
		if (j.contains("size"))
			e.size = j["size"].get<uint64_t>();
		if (j.contains("sha256"))
			e.sha256 = j["sha256"].get<std::string>();
		if (j.contains("target"))
			e.target = j["target"].get<std::string>();
		if (j.contains("mode"))
			e.orig_mode = static_cast<int>(strtol(j["mode"].get<std::string>().c_str() + 2, nullptr, 8));   // "0o755"
		// the manifest lists every xattr macOS shows, and the BSD flags
		e.check_xattrs = true;
		if (j.contains("xattrs"))
			for (auto it = j["xattrs"].begin(); it != j["xattrs"].end(); ++it)
				e.xattrs[it.key()] = it.value()["sha256"].get<std::string>();
		if (j.contains("flags"))
		{
			e.flags = j["flags"].get<uint32_t>();
			e.hidden = (e.flags & kUF_HIDDEN) ? 1 : 0;
		}
		if (j.contains("nlink") && e.type == "file")
		{
			e.nlink = j["nlink"].get<uint64_t>();
		}
		g_model[path] = e;
	}

	// ---- writes
	RwBackend rw;
	Win32Backend win;
	int rc = 0;
	if (mode == WIN32_DRIVE)
	{
		win.root = drive;
		while (!win.root.empty() && (win.root.back() == L'\\' || win.root.back() == L'/'))
			win.root.pop_back();
		if (win.root.compare(0, 4, L"\\\\?\\") != 0)
			win.root = L"\\\\?\\" + win.root;
		win.ProbeSymlink();
		g_be = &win;
	}
	else
	{
		if (mount_rw(&rw.m))
			return 1;
		g_be = &rw;
	}

	OpMkdir("/w3");
	// WRITETEST_NAMES="a|b|c": only create these files in /w3 (narrowing down
	// bugs). %XX is a raw byte, since the environment is not UTF-8.
	if (const char *only = getenv("WRITETEST_NAMES"))
	{
		std::string s;
		for (const char *p = only; *p; p++)
		{
			if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]))
			{
				s.push_back(static_cast<char>(std::stoi(std::string(p + 1, 2), nullptr, 16)));
				p += 2;
			}
			else
				s.push_back(*p);
		}
		for (size_t pos = 0; pos <= s.size();)
		{
			size_t next = s.find('|', pos);
			if (next == std::string::npos)
				next = s.size();
			if (next > pos)
				OpCreate("/w3/" + s.substr(pos, next - pos), Pattern(8, 20), 4096);
			pos = next + 1;
		}
	}
	else
	{
	OpCreate("/w3/hello.txt", std::vector<uint8_t>({ 'H', 'e', 'l', 'l', 'o', ' ', 'f', 'r', 'o', 'm', ' ', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\n' }), 4096);
	OpCreate("/w3/empty.bin", {}, 4096);
	OpCreate("/w3/big.bin", Pattern(1, 5 * 1024 * 1024 + 123), 100000);   // misaligned chunks
	OpCreate("/w3/page.bin", Pattern(2, 4096), 4096);
	// operations on files macOS wrote; the basic image's own files where they exist
	auto has = [](const char *p) { return g_model.count(p) != 0; };
	std::string mac_file;   // any other image: its first non-empty file
	for (const auto &kv : g_model)
		if (kv.second.type == "file" && kv.second.size > 0 && kv.first.compare(0, 4, "/w3/") != 0)
		{
			mac_file = kv.first;
			break;
		}
	if (has("/sizes/size_65536.bin"))
		OpPatch("/sizes/size_65536.bin", 1000, Pattern(3, 5000), 777);      // CoW of macOS-written blocks
	if (has("/history/appended.txt"))
		OpPatch("/history/appended.txt", 13, std::vector<uint8_t>({ 't', 'h', 'i', 'r', 'd', '\n' }), 4096);   // append
	if (has("/sizes/random_20MiB.bin"))
		OpPatch("/sizes/random_20MiB.bin", 20 * 1024 * 1024 - 10, Pattern(4, 20000), 8192); // straddles EOF
	if (has("/sizes/size_1048577.bin"))
		OpTruncate("/sizes/size_1048577.bin", 1000);                        // shrink
	// WRITETEST_SKIP=group,...: leave out groups of operations (bisecting bugs)
	auto skip = [](const char *group) { const char *s = getenv("WRITETEST_SKIP"); return s && strstr(s, group); };
	if (!mac_file.empty() && !has("/sizes/size_65536.bin") && !skip("mac"))
	{
		OpPatch(mac_file, 3, Pattern(9, 2000), 333);
		OpSetattr(mac_file, 0640, 1234567890LL * 1000000000LL);
	}
	OpTruncate("/w3/page.bin", 3 * 4096 + 5);                               // grow (zero tail)
	if (!skip("renames"))
	{
		OpMkdir("/w3/sub");
		OpMkdir("/w3/sub/deeper");
		OpRename("/w3/hello.txt", "/w3/sub/renamed.txt");
		OpCreate("/w3/a.txt", Pattern(5, 300), 4096);
		OpCreate("/w3/b.txt", Pattern(6, 400), 4096);
		OpRename("/w3/a.txt", "/w3/b.txt");                                 // replace
		OpRename("/w3/sub", "/w3/moved_sub");                               // directory with contents
		OpSetattr("/w3/b.txt", 0444, INT64_MIN);                            // read-only (both backends)
	}
	if (has("/empty.txt"))
		OpUnlink("/empty.txt");
	if (has("/bigdir"))
		for (int i = 0; i < 100; i++)   // deletions spread over a multi-level catalog
		{
			char name[64];
			snprintf(name, sizeof(name), "/bigdir/file_%04d.txt", i * 7);
			OpUnlink(name);
		}
	if (has("/history/new_name.txt"))
		OpUnlink("/history/new_name.txt");
	if (has("/perms/script_0755.sh"))
		OpSetattr("/perms/script_0755.sh", 0600, INT64_MIN);
	if (has("/hello.txt"))
		OpSetattr("/hello.txt", -1, 985392000LL * 1000000000LL + 123456700);   // 100 ns units: FILETIME resolution
	if (!skip("links"))
	{
		OpSymlink("/w3/link", "moved_sub/renamed.txt");
		OpLink("/w3/big.bin", "/w3/moved_sub/hardlink.bin");
	}
	if (!skip("many"))
	{
		OpMkdir("/w3/many");
		for (int i = 0; i < 500; i++)
		{
			char name[64];
			snprintf(name, sizeof(name), "/w3/many/f%03d.dat", i);
			OpCreate(name, Pattern(100 + i, 50 + i * 3), 4096);
		}
	}
	if (!skip("gone"))
	{
		OpMkdir("/w3/gone");
		OpUnlink("/w3/gone", true);
	}
	// names with non-ASCII characters (NFD and an astral-plane emoji)
	if (!skip("nfd"))
		OpCreate("/w3/caf\x65\xcc\x81_nfd.txt", Pattern(7, 10), 4096);
	OpCreate("/w3/\xf0\x9f\x99\x82.txt", Pattern(8, 20), 4096);

	// ---- W4: fidelity
	if (!skip("fidelity"))
	{
		// xattrs (named streams through the drive): inline, stream-backed
		// (> 3804 bytes), empty, replaced by a smaller value, removed, on a
		// directory, next to the xattrs macOS wrote
		OpXattr("/w3/page.bin", "user.small", Pattern(20, 30));
		OpXattr("/w3/page.bin", "user.big", Pattern(21, 100000));
		OpXattr("/w3/page.bin", "user.empty", {});
		OpXattr("/w3/page.bin", "user.big", Pattern(22, 5000));
		OpXattr("/w3/page.bin", "user.gone", Pattern(23, 10));
		OpRmXattr("/w3/page.bin", "user.gone");
		OpXattr("/w3/empty.bin", "com.apple.FinderInfo", std::vector<uint8_t>(32, 0));
		if (has("/w3/moved_sub"))
			OpXattr("/w3/moved_sub", "user.on_dir", Pattern(24, 40));
		if (!mac_file.empty())
			OpXattr(mac_file, "user.added", Pattern(25, 50));
		// creation time (100 ns units: FILETIME) and the hidden flag
		OpCrtime("/w3/page.bin", 1000000000LL * 1000000000LL + 1234500);
		OpHidden("/w3/empty.bin", true);
		if (has("/w3/moved_sub"))
		{
			OpCrtime("/w3/moved_sub", 946684800LL * 1000000000LL);
			OpHidden("/w3/moved_sub/deeper", true);
		}
		if (has("/w3/b.txt"))
		{
			OpHidden("/w3/b.txt", true);   // read-only too: the bit must survive
			OpHidden("/w3/b.txt", false);
		}
		// names Windows can't spell directly (U+F0xx on the Windows side)
		OpCreate("/w3/a:b?c*d|e<f>g\"h.txt", Pattern(26, 10), 4096);
		OpCreate("/w3/trailing.", Pattern(27, 10), 4096);
		OpCreate("/w3/trailing ", Pattern(28, 10), 4096);
		// symlinks (through the drive only with the symlink privilege)
		if (has("/w3/moved_sub"))
		{
			OpSymlink("/w3/dirlink", "moved_sub", true);
			OpSymlink("/w3/moved_sub/up", "../page.bin");
		}
		OpSymlink("/w3/dangling", "does/not/exist");
		OpSymlinkAbsolute("/w3/abs", "/w3/page.bin", "page.bin");
		// compressed (decmpfs) files: written by replacing them with plain copies
		std::vector<std::pair<std::string, uint64_t>> compressed;
		for (const auto &kv : g_model)
			if (kv.second.type == "file" && (kv.second.flags & kUF_COMPRESSED) && kv.second.size > 2 && kv.second.nlink == 1 &&
				!kv.second.have_data && compressed.size() < 3)
				compressed.emplace_back(kv.first, kv.second.size);
		for (size_t i = 0; i < compressed.size(); i++)
		{
			const std::string &p = compressed[i].first;
			uint64_t size = compressed[i].second;
			if (i == 0)
				OpPatch(p, size / 2, Pattern(30, 3000), 1000);
			else if (i == 1)
				OpTruncate(p, size / 3);
			else
			{
				OpTruncate(p, 0);
				OpPatch(p, 0, Pattern(31, 777), 4096);
			}
		}
		printf("fidelity: %zu compressed files written, symlinks %s\n", compressed.size(), g_be->CanSymlink() ? "made" : "skipped");
	}
	}

	if (mode == WIN32_DRIVE)
	{
		// the drive is unmounted (and the image checked) after this process exits
		SaveModel(w_model);
		printf("writes done through %ls, %d failures; model saved\n", drive.c_str(), g_failures);
		return g_failures ? 1 : 0;
	}

	if (mode == CRASH)
	{
		rc = apfsrw_sync(rw.m);
		CHECK(rc == 0, "sync: %d", rc);
		SaveModel(w_model);
		if (g_failures)
			return 1;
		if (crash_how == 0)
			Fill(rw.m, rw);   // "fill": disk full instead of a crash
		else
		{
			apfsrw_test_crash(crash_how, crash_at, crash_seed);
			Churn(rw.m);
		}
		rc = apfsrw_unmount(rw.m);
		CHECK(rc == 0, "unmount: %d", rc);
		// reached only without a crash (the write count includes the unmount)
		printf("crash phase: %llu writes, no crash; %d failures\n", (unsigned long long)apfsrw_test_writes(), g_failures);
		return g_failures ? 1 : 0;
	}

	rc = apfsrw_unmount(rw.m);
	CHECK(rc == 0, "unmount: %d", rc);
	rw.m = nullptr;
	printf("writes done, %d failures so far\n", g_failures);
	ReadBack(work, volname);
	printf("%s: %d failures\n", g_failures ? "FAILED" : "OK", g_failures);
	return g_failures ? 1 : 0;
}
