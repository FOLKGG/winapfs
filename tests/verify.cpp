// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// winapfs-verify: compares a volume against a testdata manifest.
//
//   winapfs-verify <manifest.json> --image <img> [--volume V] [--password P] [--fields ...]
//       walks the volume through the core library (no mount)
//   winapfs-verify <manifest.json> --dir X:\ [--fields ...]
//       walks a mounted drive through Win32, the way any Windows program sees it
//
// --fields is a comma list. Default: type,size,sha256,target. Also: nlink,
// mode, uid, gid, mtime, birthtime, inode, flags, xattrs, all.

#include <winapfs/Apfs.h>
#include <winapfs/Names.h>

#include <apfsrw.h>

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <bcrypt.h>
#include <winioctl.h>

#ifndef IO_REPARSE_TAG_LX_FIFO
#define IO_REPARSE_TAG_LX_FIFO 0x80000024L
#endif

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace winapfs;
using json = nlohmann::json;

namespace {

struct Entry
{
	std::string type;
	bool has_size = false;
	uint64_t size = 0;
	std::string sha256;
	std::string target;
	uint64_t nlink = 0;
	std::string mode;
	int64_t uid = -1, gid = -1;
	int64_t mtime_ns = 0;
	int64_t birthtime_s = 0;
	uint64_t inode = 0;
	uint64_t flags = 0;
	std::map<std::string, std::pair<uint64_t, std::string>> xattrs;  // name -> (size, sha256)
	bool has_meta = false;     // nlink..flags filled in
	bool has_xattrs = false;
};

class Sha256
{
public:
	Sha256()
	{
		BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
		BCryptCreateHash(m_alg, &m_hash, nullptr, 0, nullptr, 0, 0);
	}
	~Sha256()
	{
		BCryptDestroyHash(m_hash);
		BCryptCloseAlgorithmProvider(m_alg, 0);
	}
	void Update(const void *p, size_t n) { BCryptHashData(m_hash, (PUCHAR)p, static_cast<ULONG>(n), 0); }
	std::string Hex()
	{
		uint8_t d[32];
		BCryptFinishHash(m_hash, d, sizeof(d), 0);
		static const char *hx = "0123456789abcdef";
		std::string s;
		for (uint8_t b : d)
		{
			s.push_back(hx[b >> 4]);
			s.push_back(hx[b & 15]);
		}
		return s;
	}

private:
	BCRYPT_ALG_HANDLE m_alg = nullptr;
	BCRYPT_HASH_HANDLE m_hash = nullptr;
};

std::string HexDecode(const std::string &h)
{
	std::string out;
	for (size_t i = 0; i + 1 < h.size(); i += 2)
		out.push_back(static_cast<char>(std::stoi(h.substr(i, 2), nullptr, 16)));
	return out;
}

bool Skipped(const std::string &path)
{
	static const char *skip[] = { "/.fseventsd", "/.Spotlight-V100", "/.Trashes", "/.TemporaryItems" };
	for (const char *s : skip)
	{
		size_t n = strlen(s);
		if (path.compare(0, n, s) == 0 && (path.size() == n || path[n] == '/'))
			return true;
	}
	return false;
}

std::string ModeOct(uint16_t mode)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "0o%o", mode & 07777);
	return buf;
}

// ---------------------------------------------------------------- image walk

struct ImageWalker
{
	Volume &vol;
	bool want_hash;
	bool want_meta;
	bool want_xattrs;
	std::map<std::string, Entry> out;
	int errors = 0;

	void Fill(const std::string &path, uint64_t ino)
	{
		Stat st;
		Entry e;
		if (!vol.GetStat(ino, st))
		{
			fprintf(stderr, "stat failed: %s\n", path.c_str());
			errors++;
			return;
		}
		e.type = st.type == FileType::Dir ? "dir" : st.type == FileType::Symlink ? "symlink" : st.type == FileType::Fifo ? "fifo" : st.type == FileType::File ? "file" : "other";
		if (st.type == FileType::File)
		{
			e.has_size = true;
			e.size = st.size;
			if (want_hash)
			{
				std::string err;
				std::unique_ptr<FileReader> r = vol.OpenFile(st, err);
				if (!r)
				{
					fprintf(stderr, "open failed: %s: %s\n", path.c_str(), err.c_str());
					errors++;
				}
				else
				{
					Sha256 h;
					std::vector<uint8_t> buf(1 << 20);
					uint64_t off = 0;
					while (off < r->Size())
					{
						size_t got = 0;
						if (!r->Read(off, buf.data(), buf.size(), got) || got == 0)
						{
							fprintf(stderr, "read failed: %s at %llu\n", path.c_str(), static_cast<unsigned long long>(off));
							errors++;
							break;
						}
						h.Update(buf.data(), got);
						off += got;
					}
					e.sha256 = h.Hex();
				}
			}
		}
		else if (st.type == FileType::Symlink)
		{
			vol.ReadLink(ino, e.target);
		}
		if (want_meta)
		{
			e.has_meta = true;
			e.nlink = st.type == FileType::Dir ? 0 : st.nlink;
			e.mode = ModeOct(st.mode);
			e.uid = st.uid;
			e.gid = st.gid;
			e.mtime_ns = st.mod_ns;
			e.birthtime_s = st.create_ns / 1000000000;
			e.inode = st.inode;
			e.flags = st.bsd_flags;
		}
		if (want_xattrs)
		{
			e.has_xattrs = true;
			std::vector<std::string> names;
			vol.ListXattrs(ino, st, names);
			for (const auto &n : names)
			{
				std::vector<uint8_t> d;
				vol.GetXattr(ino, n, d);
				Sha256 h;
				h.Update(d.data(), d.size());
				e.xattrs[n] = { d.size(), h.Hex() };
			}
		}
		out[path] = std::move(e);
	}

	void Walk(const std::string &path, uint64_t ino, int depth)
	{
		std::vector<DirEntry> ents;
		if (!vol.ListDir(ino, ents))
		{
			fprintf(stderr, "list failed: %s\n", path.c_str());
			errors++;
			return;
		}
		for (const auto &d : ents)
		{
			std::string p = (path == "/" ? "" : path) + "/" + d.name;
			if (Skipped(p))
				continue;
			Fill(p, d.inode);
			if (d.type == FileType::Dir && depth < 512)
				Walk(p, d.inode, depth + 1);
		}
	}
};

// ---------------------------------------------------------------- Win32 walk

std::wstring ToWinPath(const std::wstring &root, const std::string &posix)
{
	std::wstring p = root;
	size_t pos = 1;
	while (pos <= posix.size())
	{
		size_t next = posix.find('/', pos);
		if (next == std::string::npos)
			next = posix.size();
		if (next > pos)
		{
			p += L'\\';
			p += ApfsNameToWindows(posix.substr(pos, next - pos));
		}
		pos = next + 1;
	}
	return p;
}

struct DirWalker
{
	std::wstring root;  // \\?\X:
	bool want_hash;
	std::map<std::string, Entry> out;
	int errors = 0;

	void Hash(const std::wstring &wp, const std::string &path, Entry &e)
	{
		HANDLE h = CreateFileW(wp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			fprintf(stderr, "open failed: %s (error %lu)\n", path.c_str(), GetLastError());
			errors++;
			return;
		}
		Sha256 sha;
		std::vector<uint8_t> buf(1 << 20);
		uint64_t total = 0;
		for (;;)
		{
			DWORD got = 0;
			if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr))
			{
				fprintf(stderr, "read failed: %s (error %lu)\n", path.c_str(), GetLastError());
				errors++;
				break;
			}
			if (got == 0)
				break;
			sha.Update(buf.data(), got);
			total += got;
		}
		CloseHandle(h);
		if (total != e.size)
		{
			fprintf(stderr, "%s: read %llu bytes, size says %llu\n", path.c_str(), static_cast<unsigned long long>(total), static_cast<unsigned long long>(e.size));
			errors++;
		}
		e.sha256 = sha.Hex();
	}

	void Walk(const std::string &path, int depth)
	{
		std::wstring wdir = ToWinPath(root, path);
		WIN32_FIND_DATAW fd;
		HANDLE fh = FindFirstFileExW((wdir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
		if (fh == INVALID_HANDLE_VALUE)
		{
			fprintf(stderr, "list failed: %s (error %lu)\n", path.c_str(), GetLastError());
			errors++;
			return;
		}
		std::vector<std::pair<std::string, WIN32_FIND_DATAW>> items;
		do
		{
			std::wstring n = fd.cFileName;
			if (n == L"." || n == L"..")
				continue;
			items.emplace_back(WindowsNameToApfs(n), fd);
		} while (FindNextFileW(fh, &fd));
		FindClose(fh);

		for (auto &it : items)
		{
			std::string p = (path == "/" ? "" : path) + "/" + it.first;
			if (Skipped(p))
				continue;
			const WIN32_FIND_DATAW &f = it.second;
			Entry e;
			bool is_dir = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			bool is_reparse = (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
			if (is_reparse && f.dwReserved0 == IO_REPARSE_TAG_SYMLINK)
			{
				e.type = "symlink";
				ReadSymlink(ToWinPath(root, p), e.target);
			}
			else if (is_reparse && f.dwReserved0 == IO_REPARSE_TAG_LX_FIFO)
				e.type = "fifo";
			else if (is_dir)
				e.type = "dir";
			else
			{
				e.type = "file";
				e.has_size = true;
				e.size = (static_cast<uint64_t>(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
				if (want_hash)
					Hash(ToWinPath(root, p), p, e);
			}
			out[p] = e;
			if (e.type == "dir" && depth < 512)
				Walk(p, depth + 1);
		}
	}

	void ReadSymlink(const std::wstring &wp, std::string &target)
	{
		HANDLE h = CreateFileW(wp.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return;
		std::vector<uint8_t> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
		DWORD got = 0;
		if (DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr))
		{
			// REPARSE_DATA_BUFFER.SymbolicLinkReparseBuffer
			struct Rdb
			{
				ULONG tag;
				USHORT len;
				USHORT reserved;
				USHORT sub_off, sub_len, print_off, print_len;
				ULONG flags;
				WCHAR path[1];
			};
			const Rdb *r = reinterpret_cast<const Rdb *>(buf.data());
			std::wstring sub(r->path + r->sub_off / 2, r->sub_len / 2);
			// winapfs stores POSIX-style targets with '\' separators; map back
			for (auto &c : sub)
				if (c == L'\\')
					c = L'/';
			std::string t;
			// each component may carry mapped characters
			size_t pos = 0;
			while (pos <= sub.size())
			{
				size_t next = sub.find(L'/', pos);
				if (next == std::wstring::npos)
					next = sub.size();
				if (pos > 0)
					t.push_back('/');
				t += WindowsNameToApfs(sub.substr(pos, next - pos));
				pos = next + 1;
			}
			target = t;
		}
		CloseHandle(h);
	}
};

// ---------------------------------------------------------------- compare

std::set<std::string> ParseFields(const std::string &s)
{
	std::set<std::string> f;
	std::stringstream ss(s);
	std::string x;
	while (std::getline(ss, x, ','))
		if (!x.empty())
			f.insert(x);
	if (f.count("all"))
		f = { "type", "size", "sha256", "target", "nlink", "mode", "uid", "gid", "mtime", "birthtime", "inode", "flags", "xattrs" };
	return f;
}

// ---------------------------------------------------------------- linux-apfs-rw engine walk

const char *DtypeName(uint32_t mode)
{
	switch (mode & 0170000)
	{
	case 0040000: return "dir";
	case 0120000: return "symlink";
	case 0010000: return "fifo";
	case 0100000: return "file";
	default: return "other";
	}
}

struct RwWalker
{
	bool want_hash;
	bool want_meta;
	bool want_xattrs;
	std::map<std::string, Entry> out;
	int errors = 0;

	void Fill(const std::string &path, apfsrw_node *n)
	{
		apfsrw_stat st;
		Entry e;
		if (apfsrw_getattr(n, &st))
		{
			fprintf(stderr, "stat failed: %s\n", path.c_str());
			errors++;
			return;
		}
		e.type = DtypeName(st.mode);
		if (e.type == "file")
		{
			e.has_size = true;
			e.size = st.size;
			if (want_hash)
			{
				apfsrw_file *f = nullptr;
				int err = apfsrw_open(n, false, &f);
				if (err)
				{
					fprintf(stderr, "open failed: %s: %d\n", path.c_str(), err);
					errors++;
				}
				else
				{
					Sha256 h;
					std::vector<uint8_t> buf(1 << 20);
					uint64_t off = 0;
					while (off < st.size)
					{
						size_t got = 0;
						err = apfsrw_read(f, off, buf.data(), buf.size(), &got);
						if (err || !got)
						{
							fprintf(stderr, "read failed: %s at %llu: %d\n", path.c_str(), (unsigned long long)off, err);
							errors++;
							break;
						}
						h.Update(buf.data(), got);
						off += got;
					}
					apfsrw_release(f);
					e.sha256 = h.Hex();
				}
			}
		}
		else if (e.type == "symlink")
		{
			std::vector<char> t(4096);
			int len = apfsrw_readlink(n, t.data(), t.size());
			if (len >= 0)
				e.target.assign(t.data(), std::min<size_t>(len, t.size() - 1));
		}
		if (want_meta)
		{
			e.has_meta = true;
			e.nlink = e.type == "dir" ? 0 : st.nlink;
			e.mode = ModeOct(static_cast<uint16_t>(st.mode));
			e.uid = st.uid;
			e.gid = st.gid;
			e.mtime_ns = st.mtime_ns;
			e.birthtime_s = st.btime_ns / 1000000000;
			e.inode = st.ino;
			e.flags = st.bsd_flags;
		}
		if (want_xattrs)
		{
			e.has_xattrs = true;
			long n_list = apfsrw_listxattr(n, nullptr, 0);
			if (n_list > 0)
			{
				std::vector<char> names(n_list);
				n_list = apfsrw_listxattr(n, names.data(), names.size());
				for (long p = 0; p < n_list; p += (long)strlen(names.data() + p) + 1)
				{
					std::string name = names.data() + p;
					// the same visibility rules as macOS (and the core library)
					if (name == "com.apple.decmpfs" || name.compare(0, 13, "com.apple.fs.") == 0)
						continue;
					long sz = apfsrw_getxattr(n, name.c_str(), nullptr, 0);
					if (sz < 0)
						continue;
					if (name == "com.apple.ResourceFork" && (sz == 0 || (st.bsd_flags & kUF_COMPRESSED)))
						continue;
					std::vector<uint8_t> v(sz);
					sz = apfsrw_getxattr(n, name.c_str(), v.data(), v.size());
					Sha256 h;
					h.Update(v.data(), sz > 0 ? sz : 0);
					e.xattrs[name] = { (uint64_t)(sz > 0 ? sz : 0), h.Hex() };
				}
			}
		}
		out[path] = std::move(e);
	}

	struct Child
	{
		std::string name;
		unsigned dtype;
	};

	void Walk(const std::string &path, apfsrw_node *dir, int depth)
	{
		std::vector<Child> kids;
		int err = apfsrw_readdir(dir, [](void *ctx, const char *name, size_t len, uint64_t, unsigned dtype) {
			static_cast<std::vector<Child> *>(ctx)->push_back({ std::string(name, len), dtype });
			return 0;
		}, &kids);
		if (err)
		{
			fprintf(stderr, "list failed: %s: %d\n", path.c_str(), err);
			errors++;
			return;
		}
		for (const auto &k : kids)
		{
			std::string p = (path == "/" ? "" : path) + "/" + k.name;
			if (Skipped(p))
				continue;
			apfsrw_node *n = nullptr;
			err = apfsrw_lookup(dir, k.name.data(), k.name.size(), &n);
			if (err)
			{
				fprintf(stderr, "lookup failed: %s: %d\n", p.c_str(), err);
				errors++;
				continue;
			}
			Fill(p, n);
			if (k.dtype == APFSRW_DT_DIR && depth < 512)
				Walk(p, n, depth + 1);
			apfsrw_put(n);
		}
	}
};

} // namespace

int wmain(int argc, wchar_t **wargv)
{
	SetConsoleOutputCP(CP_UTF8);

	std::string manifest, image, dir, volume, password, engine = "lib", fields_s = "type,size,sha256,target";
	for (int i = 1; i < argc; i++)
	{
		std::string a = WideToUtf8(wargv[i]);
		auto next = [&]() { return i + 1 < argc ? WideToUtf8(wargv[++i]) : std::string(); };
		if (a == "--image") image = next();
		else if (a == "--engine") engine = next();
		else if (a == "--dir") dir = next();
		else if (a == "--volume") volume = next();
		else if (a == "--password") password = next();
		else if (a == "--fields") fields_s = next();
		else manifest = a;
	}
	if (manifest.empty() || (image.empty() == dir.empty()))
	{
		fprintf(stderr, "usage: winapfs-verify <manifest.json> (--image <img> [--volume V] [--password P] | --dir X:\\) [--fields f,...]\n");
		return 2;
	}
	std::set<std::string> fields = ParseFields(fields_s);

	std::ifstream mf(Utf8ToWide(manifest));
	if (!mf)
	{
		fprintf(stderr, "cannot read %s\n", manifest.c_str());
		return 2;
	}
	json doc = json::parse(mf);

	std::map<std::string, Entry> expected;
	for (const auto &j : doc["entries"])
	{
		std::string path = j["path"] == "/" ? "/" : "/" + HexDecode(j["path_utf8_hex"].get<std::string>());
		if (path == "/")
			continue;
		Entry e;
		e.type = j["type"].get<std::string>();
		if (j.contains("size"))
		{
			e.has_size = true;
			e.size = j["size"].get<uint64_t>();
		}
		if (j.contains("sha256"))
			e.sha256 = j["sha256"].get<std::string>();
		if (j.contains("target"))
			e.target = j["target"].get<std::string>();
		e.has_meta = true;
		e.nlink = e.type == "dir" ? 0 : j["nlink"].get<uint64_t>();
		e.mode = j["mode"].get<std::string>();
		e.uid = j["uid"].get<int64_t>();
		e.gid = j["gid"].get<int64_t>();
		e.mtime_ns = j["mtime_ns"].get<int64_t>();
		e.birthtime_s = static_cast<int64_t>(j["birthtime"].get<double>());
		e.inode = j["inode"].get<uint64_t>();
		e.flags = j["flags"].get<uint64_t>();
		e.has_xattrs = true;
		if (j.contains("xattrs"))
			for (auto it = j["xattrs"].begin(); it != j["xattrs"].end(); ++it)
				e.xattrs[it.key()] = { it.value()["size"].get<uint64_t>(), it.value()["sha256"].get<std::string>() };
		expected[path] = std::move(e);
	}

	std::map<std::string, Entry> actual;
	int walk_errors = 0;
	bool want_hash = fields.count("sha256") != 0;
	bool want_meta = fields.count("nlink") || fields.count("mode") || fields.count("uid") || fields.count("gid") || fields.count("mtime") ||
		fields.count("birthtime") || fields.count("inode") || fields.count("flags");

	if (!image.empty() && engine == "rw")
	{
		// partition offset and volume index from the core library, data from linux-apfs-rw
		std::string err;
		std::unique_ptr<Container> c = Container::Open(image, err);
		if (!c)
		{
			fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		const VolumeInfo *vi = volume.empty() ? (c->Volumes().empty() ? nullptr : &c->Volumes().front()) : c->FindVolume(volume);
		if (!vi)
		{
			fprintf(stderr, "no volume %s\n", volume.c_str());
			return 1;
		}
		uint64_t offset = c->PartitionOffset();
		unsigned fsid = vi->fsid;
		c.reset();

		apfsrw_mount *m = nullptr;
		int rc = apfsrw_mount_volume(Utf8ToWide(image).c_str(), offset, 0, fsid, false, nullptr, &m);
		if (rc)
		{
			fprintf(stderr, "linux-apfs-rw mount failed: %d\n", rc);
			return 1;
		}
		apfsrw_node *root = nullptr;
		apfsrw_root(m, &root);
		RwWalker w{ want_hash, want_meta, fields.count("xattrs") != 0 };
		w.Walk("/", root, 0);
		apfsrw_put(root);
		rc = apfsrw_unmount(m);
		if (rc)
		{
			fprintf(stderr, "unmount failed: %d\n", rc);
			walk_errors++;
		}
		actual = std::move(w.out);
		walk_errors += w.errors;
	}
	else if (!image.empty())
	{
		std::string err;
		std::unique_ptr<Container> c = Container::Open(image, err);
		if (!c)
		{
			fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		const VolumeInfo *vi = volume.empty() ? (c->Volumes().empty() ? nullptr : &c->Volumes().front()) : c->FindVolume(volume);
		if (!vi)
		{
			fprintf(stderr, "no volume %s\n", volume.c_str());
			return 1;
		}
		std::unique_ptr<Volume> v = c->OpenVolume(vi->fsid, password, err);
		if (!v)
		{
			fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		if (doc.contains("case_sensitive") && doc["case_sensitive"].get<bool>() != v->Info().case_sensitive)
		{
			fprintf(stderr, "case sensitivity differs\n");
			walk_errors++;
		}
		ImageWalker w{ *v, want_hash, want_meta, fields.count("xattrs") != 0 };
		w.Walk("/", kRootInode, 0);
		actual = std::move(w.out);
		walk_errors += w.errors;
	}
	else
	{
		std::wstring root = Utf8ToWide(dir);
		while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
			root.pop_back();
		if (root.compare(0, 4, L"\\\\?\\") != 0)
			root = L"\\\\?\\" + root;
		DirWalker w{ root, want_hash };
		w.Walk("/", 0);
		actual = std::move(w.out);
		walk_errors += w.errors;
	}

	std::vector<std::string> problems;
	for (const auto &kv : expected)
		if (!actual.count(kv.first))
			problems.push_back("missing: " + kv.first);
	for (const auto &kv : actual)
		if (!expected.count(kv.first))
			problems.push_back("extra: " + kv.first);

	for (const auto &kv : expected)
	{
		auto it = actual.find(kv.first);
		if (it == actual.end())
			continue;
		const Entry &e = kv.second, &a = it->second;
		const std::string &p = kv.first;
		auto diff = [&](const char *what, const std::string &ev, const std::string &av) {
			if (ev != av)
				problems.push_back(p + ": " + what + " expected " + ev + ", got " + av);
		};
		if (fields.count("type"))
			diff("type", e.type, a.type);
		if (fields.count("size") && e.has_size)
			diff("size", std::to_string(e.size), a.has_size ? std::to_string(a.size) : "none");
		if (fields.count("sha256") && !e.sha256.empty())
			diff("sha256", e.sha256, a.sha256);
		if (fields.count("target") && e.type == "symlink")
			diff("target", e.target, a.target);
		if (a.has_meta)
		{
			if (fields.count("nlink")) diff("nlink", std::to_string(e.nlink), std::to_string(a.nlink));
			if (fields.count("mode")) diff("mode", e.mode, a.mode);
			// stored 99 is macOS _unknown: it shows as whoever looks, so it matches anything
			if (fields.count("uid") && a.uid != 99) diff("uid", std::to_string(e.uid), std::to_string(a.uid));
			if (fields.count("gid") && a.gid != 99) diff("gid", std::to_string(e.gid), std::to_string(a.gid));
			if (fields.count("mtime")) diff("mtime_ns", std::to_string(e.mtime_ns), std::to_string(a.mtime_ns));
			if (fields.count("birthtime")) diff("birthtime", std::to_string(e.birthtime_s), std::to_string(a.birthtime_s));
			if (fields.count("inode")) diff("inode", std::to_string(e.inode), std::to_string(a.inode));
			if (fields.count("flags")) diff("flags", std::to_string(e.flags), std::to_string(a.flags));
		}
		if (fields.count("xattrs") && a.has_xattrs)
		{
			for (const auto &x : e.xattrs)
			{
				auto ax = a.xattrs.find(x.first);
				if (ax == a.xattrs.end())
					problems.push_back(p + ": missing xattr " + x.first);
				else if (ax->second != x.second)
					problems.push_back(p + ": xattr " + x.first + " differs (size " + std::to_string(x.second.first) + " vs " + std::to_string(ax->second.first) + ")");
			}
			for (const auto &x : a.xattrs)
				if (!e.xattrs.count(x.first))
					problems.push_back(p + ": extra xattr " + x.first);
		}
	}

	printf("%s: %zu entries expected, %zu found, %zu differences, %d walk errors\n", manifest.c_str(), expected.size(), actual.size(),
		problems.size(), walk_errors);
	for (size_t i = 0; i < problems.size() && i < 40; i++)
		printf("  %s\n", problems[i].c_str());
	if (problems.size() > 40)
		printf("  ... %zu more\n", problems.size() - 40);
	return (problems.empty() && walk_errors == 0) ? 0 : 1;
}
