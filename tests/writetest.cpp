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
	int64_t mtime_ns = INT64_MIN;
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

// ---------------------------------------------------------------- engine helpers

struct Rw
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

	std::vector<uint8_t> ReadAll(const std::string &path)
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

	// Writes data at off in chunks of `chunk` bytes (exercises partial pages).
	void Write(const std::string &path, uint64_t off, const std::vector<uint8_t> &data, size_t chunk)
	{
		apfsrw_node *n = Walk(path);
		apfsrw_file *f = nullptr;
		CHECK(n && apfsrw_open(n, true, &f) == 0, "open for write %s", path.c_str());
		if (!f)
			return;
		size_t done = 0;
		while (done < data.size())
		{
			size_t len = std::min(chunk, data.size() - done), w = 0;
			int err = apfsrw_write(f, off + done, data.data() + done, len, &w);
			CHECK(err == 0 && w == len, "write %s at %llu: err %d wrote %zu", path.c_str(), (unsigned long long)(off + done), err, w);
			if (err || !w)
				break;
			done += w;
		}
		apfsrw_release(f);
		apfsrw_put(n);
	}
};

Rw g_rw;

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

// model updates mirror every operation
void ModelFile(const std::string &path, const std::vector<uint8_t> &data)
{
	Expect &e = g_model[path];
	int mode = e.mode;
	int64_t mtime = e.mtime_ns;
	e = Expect();
	e.type = "file";
	e.data = data;
	e.have_data = true;
	e.mode = mode;
	e.mtime_ns = mtime;
}

void OpCreate(const std::string &path, const std::vector<uint8_t> &data, size_t chunk)
{
	TraceOp t("create", path);
	apfsrw_node *dir = g_rw.Walk(Parent(path)), *n = nullptr;
	std::string name = Base(path);
	int err = dir ? apfsrw_create(dir, name.data(), name.size(), 0644, &n) : -2;
	CHECK(err == 0, "create %s: %d", path.c_str(), err);
	if (dir)
		apfsrw_put(dir);
	if (n)
		apfsrw_put(n);
	if (!data.empty())
		g_rw.Write(path, 0, data, chunk);
	ModelFile(path, data);
}

void OpPatch(const std::string &path, uint64_t off, const std::vector<uint8_t> &patch, size_t chunk)
{
	TraceOp t("patch", path);
	std::vector<uint8_t> cur = g_rw.ReadAll(path);
	g_rw.Write(path, off, patch, chunk);
	if (cur.size() < off + patch.size())
		cur.resize(off + patch.size(), 0);
	memcpy(cur.data() + off, patch.data(), patch.size());
	ModelFile(path, cur);
}

void OpTruncate(const std::string &path, uint64_t size)
{
	TraceOp t("truncate", path);
	std::vector<uint8_t> cur = g_rw.ReadAll(path);
	apfsrw_node *n = g_rw.Walk(path);
	apfsrw_setattr_t a = {};
	a.mask = APFSRW_SET_SIZE;
	a.size = size;
	int err = n ? apfsrw_setattr(n, &a) : -2;
	CHECK(err == 0, "truncate %s to %llu: %d", path.c_str(), (unsigned long long)size, err);
	if (n)
		apfsrw_put(n);
	cur.resize(size, 0);
	ModelFile(path, cur);
}

void OpMkdir(const std::string &path)
{
	TraceOp t("mkdir", path);
	apfsrw_node *dir = g_rw.Walk(Parent(path)), *n = nullptr;
	std::string name = Base(path);
	int err = dir ? apfsrw_mkdir(dir, name.data(), name.size(), 0755, &n) : -2;
	CHECK(err == 0, "mkdir %s: %d", path.c_str(), err);
	if (dir)
		apfsrw_put(dir);
	if (n)
		apfsrw_put(n);
	g_model[path] = Expect();
	g_model[path].type = "dir";
}

void OpRename(const std::string &from, const std::string &to)
{
	TraceOp t("rename", from + " -> " + to);
	apfsrw_node *od = g_rw.Walk(Parent(from)), *nd = g_rw.Walk(Parent(to));
	std::string on = Base(from), nn = Base(to);
	int err = (od && nd) ? apfsrw_rename(od, on.data(), on.size(), nd, nn.data(), nn.size(), 0) : -2;
	CHECK(err == 0, "rename %s -> %s: %d", from.c_str(), to.c_str(), err);
	if (od)
		apfsrw_put(od);
	if (nd)
		apfsrw_put(nd);
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
	apfsrw_node *d = g_rw.Walk(Parent(path));
	std::string name = Base(path);
	int err = d ? (dir ? apfsrw_rmdir(d, name.data(), name.size()) : apfsrw_unlink(d, name.data(), name.size())) : -2;
	CHECK(err == 0, "%s %s: %d", dir ? "rmdir" : "unlink", path.c_str(), err);
	if (d)
		apfsrw_put(d);
	g_model.erase(path);
}

void OpSymlink(const std::string &path, const std::string &target)
{
	TraceOp t("symlink", path);
	apfsrw_node *dir = g_rw.Walk(Parent(path));
	std::string name = Base(path);
	int err = dir ? apfsrw_symlink(dir, name.data(), name.size(), target.c_str(), nullptr) : -2;
	CHECK(err == 0, "symlink %s: %d", path.c_str(), err);
	if (dir)
		apfsrw_put(dir);
	g_model[path] = Expect();
	g_model[path].type = "symlink";
	g_model[path].target = target;
}

void OpLink(const std::string &existing, const std::string &path)
{
	TraceOp t("link", path);
	apfsrw_node *src = g_rw.Walk(existing), *dir = g_rw.Walk(Parent(path));
	std::string name = Base(path);
	int err = (src && dir) ? apfsrw_link(src, dir, name.data(), name.size(), nullptr) : -2;
	CHECK(err == 0, "link %s -> %s: %d", existing.c_str(), path.c_str(), err);
	if (src)
		apfsrw_put(src);
	if (dir)
		apfsrw_put(dir);
	g_model[path] = g_model[existing];
}

void OpSetattr(const std::string &path, int mode, int64_t mtime_ns)
{
	TraceOp t("setattr", path);
	apfsrw_node *n = g_rw.Walk(path);
	apfsrw_setattr_t a = {};
	if (mode >= 0)
	{
		a.mask |= APFSRW_SET_MODE;
		a.mode = static_cast<uint32_t>(mode);
	}
	if (mtime_ns != INT64_MIN)
	{
		a.mask |= APFSRW_SET_MTIME;
		a.mtime_ns = mtime_ns;
	}
	int err = n ? apfsrw_setattr(n, &a) : -2;
	CHECK(err == 0, "setattr %s: %d", path.c_str(), err);
	if (n)
		apfsrw_put(n);
	if (mode >= 0)
		g_model[path].mode = mode;
	if (mtime_ns != INT64_MIN)
		g_model[path].mtime_ns = mtime_ns;
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
	}
	for (const auto &kv : seen)
		CHECK(g_model.count(kv.first), "[%s] unexpected %s", v.EngineName(), kv.first.c_str());
	printf("[%s] %zu entries checked, %d problems\n", v.EngineName(), seen.size(), g_failures - before);
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
	if (argc < 4)
	{
		fprintf(stderr, "usage: apfsrw-writetest <manifest.json> <source.img> <work.img> [volume]\n");
		return 2;
	}
	std::string manifest = WideToUtf8(argv[1]), work = WideToUtf8(argv[3]);
	std::string volname = argc > 4 ? WideToUtf8(argv[4]) : "";
	apfsrw_set_log(Log);

	if (!CopyFileW(argv[2], argv[3], FALSE))
	{
		fprintf(stderr, "cannot copy image (error %lu)\n", GetLastError());
		return 2;
	}

	// the untouched state, from the manifest
	std::ifstream mf(argv[1]);
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
		g_model[path] = e;
	}

	std::string err;
	std::unique_ptr<Container> c = Container::Open(work, err);
	if (!c)
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 2;
	}
	const VolumeInfo *vi = volname.empty() ? &c->Volumes().front() : c->FindVolume(volname);
	uint64_t offset = c->PartitionOffset(), size = c->PartitionSize();
	unsigned fsid = vi->fsid;
	c.reset();

	// ---- writes
	int rc = apfsrw_mount_volume(argv[3], offset, size, fsid, true, nullptr, &g_rw.m);
	if (rc)
	{
		fprintf(stderr, "read-write mount failed: %d\n", rc);
		return 1;
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
		OpSetattr("/hello.txt", -1, 985392000LL * 1000000000LL + 123456789);
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
	}

	rc = apfsrw_unmount(g_rw.m);
	CHECK(rc == 0, "unmount: %d", rc);
	g_rw.m = nullptr;
	printf("writes done, %d failures so far\n", g_failures);

	// ---- independent read-back
	c = Container::Open(work, err);
	if (!c)
	{
		fprintf(stderr, "reopen: %s\n", err.c_str());
		return 1;
	}
	{
		std::unique_ptr<Volume> lib = c->OpenVolume(fsid, "", err);
		CHECK(lib != nullptr, "apfs-fuse open: %s", err.c_str());
		if (lib)
			Compare(*lib);
	}
	{
		std::unique_ptr<Volume> rw = OpenRwVolume(*c, fsid, false, err);
		CHECK(rw != nullptr, "linux-apfs-rw reopen: %s", err.c_str());
		if (rw)
			Compare(*rw);
	}

	printf("%s: %d failures\n", g_failures ? "FAILED" : "OK", g_failures);
	return g_failures ? 1 : 0;
}
