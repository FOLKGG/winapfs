// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ApfsFs.h"

#include <winapfs/Names.h>

#include <sddl.h>

#include <cstdio>

namespace winapfs {

// Fixed security descriptors. macOS owners don't correspond to Windows
// accounts, so like FAT/exFAT there are no per-file ACLs: read-only mounts
// are readable by everyone, writable mounts writable by everyone.
static const wchar_t *kSddlReadOnly = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;WD)";
static const wchar_t *kSddlWritable = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)";

constexpr size_t kDirCacheMax = 65536;

struct ApfsFs::Node
{
	Stat st;
	std::string name;           // last path component (APFS bytes)
	std::mutex mutex;
	std::unique_ptr<FileReader> reader;
	PVOID dir_buffer = nullptr;
};

static ApfsFs *Self(FSP_FILE_SYSTEM *fs)
{
	return static_cast<ApfsFs *>(fs->UserContext);
}

static uint64_t NsToFileTime(int64_t ns)
{
	// 100ns ticks since 1601-01-01; floor division keeps pre-1970 times right
	int64_t ticks = ns >= 0 ? ns / 100 : -((-ns + 99) / 100);
	int64_t ft = ticks + 116444736000000000LL;
	return ft < 0 ? 0 : static_cast<uint64_t>(ft);
}

// ---------------------------------------------------------------- ApfsFs

ApfsFs::ApfsFs(std::unique_ptr<Volume> vol, const MountOptions &opts) : m_vol(std::move(vol)), m_opts(opts)
{
	ConvertStringSecurityDescriptorToSecurityDescriptorW(m_vol->Writable() ? kSddlWritable : kSddlReadOnly, SDDL_REVISION_1, &m_sd, &m_sd_size);
	VolumeSpace space;
	if (m_vol->GetSpace(space) && space.block_size)
		m_block_size = space.block_size;
}

ApfsFs::~ApfsFs()
{
	Stop();
	if (m_sd)
		LocalFree(m_sd);
}

uint32_t ApfsFs::Attributes(const Stat &st, const std::string &name)
{
	uint32_t a = 0;
	if (st.type == FileType::Dir)
		a |= FILE_ATTRIBUTE_DIRECTORY;
	if (st.bsd_flags & kUF_HIDDEN)
		a |= FILE_ATTRIBUTE_HIDDEN;
	if (m_opts.hide_dotfiles && !name.empty() && name[0] == '.')
		a |= FILE_ATTRIBUTE_HIDDEN;
	if (st.bsd_flags & (kUF_IMMUTABLE | kSF_IMMUTABLE))
		a |= FILE_ATTRIBUTE_READONLY;
	if (a == 0)
		a = FILE_ATTRIBUTE_ARCHIVE;
	return a;
}

void ApfsFs::FillFileInfo(const Stat &st, const std::string &name, FSP_FSCTL_FILE_INFO &fi)
{
	uint64_t bs = m_block_size;
	memset(&fi, 0, sizeof(fi));
	fi.FileAttributes = Attributes(st, name);
	fi.ReparseTag = 0;
	if (st.type != FileType::Dir)
	{
		fi.FileSize = st.size;
		fi.AllocationSize = st.compressed ? (st.size + bs - 1) / bs * bs : st.alloc_size;
	}
	fi.CreationTime = NsToFileTime(st.create_ns);
	fi.LastAccessTime = NsToFileTime(st.access_ns);
	fi.LastWriteTime = NsToFileTime(st.mod_ns);
	fi.ChangeTime = NsToFileTime(st.change_ns);
	fi.IndexNumber = st.inode;
	fi.HardLinks = 0;
	fi.EaSize = 0;
}

NTSTATUS ApfsFs::CopySecurity(PSECURITY_DESCRIPTOR sd, SIZE_T *size)
{
	if (!size)
		return STATUS_SUCCESS;
	if (*size < m_sd_size)
	{
		*size = m_sd_size;
		return STATUS_BUFFER_OVERFLOW;
	}
	*size = m_sd_size;
	if (sd)
		memcpy(sd, m_sd, m_sd_size);
	return STATUS_SUCCESS;
}

bool ApfsFs::CacheGet(const std::wstring &dir, uint64_t &inode, std::wstring &normalized)
{
	std::lock_guard<std::mutex> lock(m_cache_mutex);
	auto it = m_dir_cache.find(dir);
	if (it == m_dir_cache.end())
		return false;
	inode = it->second.first;
	normalized = it->second.second;
	return true;
}

void ApfsFs::CachePut(const std::wstring &dir, uint64_t inode, const std::wstring &normalized)
{
	std::lock_guard<std::mutex> lock(m_cache_mutex);
	if (m_dir_cache.size() >= kDirCacheMax)
		m_dir_cache.clear();
	m_dir_cache.emplace(dir, std::make_pair(inode, normalized));
}

NTSTATUS ApfsFs::Resolve(const wchar_t *path, Stat &st, std::wstring *normalized, std::string *stream)
{
	std::wstring p = path ? path : L"";
	std::string stream_name;

	// "\dir\file:stream:$DATA" -> stream. Mapped names never contain ':'.
	size_t colon = p.find(L':');
	if (colon != std::wstring::npos)
	{
		std::wstring s = p.substr(colon + 1);
		p.resize(colon);
		size_t type = s.find(L':');
		if (type != std::wstring::npos)
		{
			std::wstring t = s.substr(type + 1);
			s.resize(type);
			if (_wcsicmp(t.c_str(), L"$DATA") != 0)
				return STATUS_OBJECT_NAME_INVALID;
		}
		stream_name = WindowsNameToApfs(s);
	}
	if (stream)
		*stream = stream_name;
	else if (!stream_name.empty())
		return STATUS_OBJECT_NAME_NOT_FOUND;

	// split into components
	std::vector<std::wstring> comps;
	size_t pos = 0;
	while (pos < p.size())
	{
		size_t next = p.find(L'\\', pos);
		if (next == std::wstring::npos)
			next = p.size();
		if (next > pos)
			comps.push_back(p.substr(pos, next - pos));
		pos = next + 1;
	}

	uint64_t cur = kRootInode;
	std::wstring norm;
	size_t first = 0;

	// longest cached directory prefix
	for (size_t k = comps.size(); k > 0; k--)
	{
		std::wstring key;
		for (size_t i = 0; i < k; i++)
			key += L"\\" + comps[i];
		uint64_t ino;
		std::wstring n;
		if (CacheGet(key, ino, n))
		{
			cur = ino;
			norm = n;
			first = k;
			break;
		}
	}

	std::wstring key = L"";
	for (size_t i = 0; i < first; i++)
		key += L"\\" + comps[i];

	for (size_t i = first; i < comps.size(); i++)
	{
		DirEntry e;
		if (!m_vol->Lookup(cur, WindowsNameToApfs(comps[i]), e))
			return i + 1 == comps.size() ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
		norm += L"\\" + ApfsNameToWindows(e.name);
		key += L"\\" + comps[i];
		if (i + 1 < comps.size())
		{
			if (e.type != FileType::Dir)
				return STATUS_OBJECT_PATH_NOT_FOUND;
			CachePut(key, e.inode, norm);
		}
		cur = e.inode;
	}

	if (!m_vol->GetStat(cur, st))
		return STATUS_FILE_CORRUPT_ERROR;
	if (normalized)
		*normalized = norm.empty() ? L"\\" : norm;
	return STATUS_SUCCESS;
}

NTSTATUS ApfsFs::ResolveParent(const wchar_t *path, uint64_t &parent, std::string &name, std::wstring *parent_normalized)
{
	std::wstring p = path ? path : L"";
	while (p.size() > 1 && p.back() == L'\\')
		p.pop_back();
	size_t slash = p.rfind(L'\\');
	if (slash == std::wstring::npos || slash + 1 >= p.size())
		return STATUS_OBJECT_NAME_INVALID;
	std::wstring last = p.substr(slash + 1);
	if (last.find(L':') != std::wstring::npos)
		return STATUS_OBJECT_NAME_INVALID;   // no named streams yet
	Stat st;
	std::wstring norm;
	NTSTATUS r = Resolve(slash == 0 ? L"\\" : p.substr(0, slash).c_str(), st, &norm, nullptr);
	if (!NT_SUCCESS(r))
		return r == STATUS_OBJECT_NAME_NOT_FOUND ? STATUS_OBJECT_PATH_NOT_FOUND : r;
	if (st.type != FileType::Dir)
		return STATUS_OBJECT_PATH_NOT_FOUND;
	parent = st.inode;
	name = WindowsNameToApfs(last);
	if (parent_normalized)
		*parent_normalized = norm == L"\\" ? L"" : norm;
	return STATUS_SUCCESS;
}

void ApfsFs::InvalidateCache()
{
	std::lock_guard<std::mutex> lock(m_cache_mutex);
	m_dir_cache.clear();
}

// errno from the engine -> NTSTATUS
static NTSTATUS Errno(int err)
{
	switch (-err)
	{
	case 0: return STATUS_SUCCESS;
	case 1:  /* EPERM */
	case 13: /* EACCES */ return STATUS_ACCESS_DENIED;
	case 2:  /* ENOENT */ return STATUS_OBJECT_NAME_NOT_FOUND;
	case 17: /* EEXIST */ return STATUS_OBJECT_NAME_COLLISION;
	case 20: /* ENOTDIR */ return STATUS_NOT_A_DIRECTORY;
	case 21: /* EISDIR */ return STATUS_FILE_IS_A_DIRECTORY;
	case 22: /* EINVAL */ return STATUS_INVALID_PARAMETER;
	case 27: /* EFBIG */
	case 28: /* ENOSPC */ return STATUS_DISK_FULL;
	case 30: /* EROFS */ return STATUS_MEDIA_WRITE_PROTECTED;
	case 36: /* ENAMETOOLONG */ return STATUS_OBJECT_NAME_INVALID;
	case 39: /* ENOTEMPTY */ return STATUS_DIRECTORY_NOT_EMPTY;
	case 95: /* EOPNOTSUPP */ return STATUS_NOT_SUPPORTED;
	case 12: /* ENOMEM */ return STATUS_INSUFFICIENT_RESOURCES;
	default: return STATUS_UNEXPECTED_IO_ERROR;
	}
}

static int64_t FileTimeToNs(uint64_t ft)
{
	return (static_cast<int64_t>(ft) - 116444736000000000LL) * 100;
}

// ---------------------------------------------------------------- WinFsp callbacks

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM *fs, FSP_FSCTL_VOLUME_INFO *vi)
{
	ApfsFs *self = Self(fs);
	const VolumeInfo &info = self->Vol().Info();
	VolumeSpace space;

	self->Vol().GetSpace(space);
	vi->TotalSize = space.total_bytes;
	vi->FreeSize = space.free_bytes;

	std::wstring label = Utf8ToWide(info.name);
	if (label.size() > 32)
		label.resize(32);
	memcpy(vi->VolumeLabel, label.data(), label.size() * sizeof(wchar_t));
	vi->VolumeLabelLength = static_cast<UINT16>(label.size() * sizeof(wchar_t));
	return STATUS_SUCCESS;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM *fs, PWSTR name, PUINT32 attrs, PSECURITY_DESCRIPTOR sd, SIZE_T *sd_size)
{
	ApfsFs *self = Self(fs);
	Stat st;
	std::wstring norm;
	NTSTATUS r = self->Resolve(name, st, &norm, nullptr);
	if (!NT_SUCCESS(r))
		return r;
	if (attrs)
	{
		size_t slash = norm.rfind(L'\\');
		*attrs = self->Attributes(st, WindowsNameToApfs(norm.substr(slash + 1)));
	}
	return self->CopySecurity(sd, sd_size);
}

static NTSTATUS Open(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 create_options, UINT32 granted_access, PVOID *ctx, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	std::unique_ptr<ApfsFs::Node> node(new ApfsFs::Node());
	std::wstring norm;

	NTSTATUS r = self->Resolve(name, node->st, &norm, nullptr);
	if (!NT_SUCCESS(r))
		return r;

	bool is_dir = node->st.type == FileType::Dir;
	if ((create_options & FILE_DIRECTORY_FILE) && !is_dir)
		return STATUS_NOT_A_DIRECTORY;
	if ((create_options & FILE_NON_DIRECTORY_FILE) && is_dir)
		return STATUS_FILE_IS_A_DIRECTORY;

	size_t slash = norm.rfind(L'\\');
	node->name = WindowsNameToApfs(norm.substr(slash + 1));
	self->FillFileInfo(node->st, node->name, *fi);

	FSP_FSCTL_OPEN_FILE_INFO *ofi = FspFileSystemGetOpenFileInfo(fi);
	size_t bytes = norm.size() * sizeof(wchar_t);
	if (ofi->NormalizedName && bytes <= ofi->NormalizedNameSize)
	{
		memcpy(ofi->NormalizedName, norm.data(), bytes);
		ofi->NormalizedNameSize = static_cast<UINT16>(bytes);
	}

	*ctx = node.release();
	return STATUS_SUCCESS;
}

// ---------------------------------------------------------------- writing
// (the create dispatcher needs Create/Overwrite even on read-only volumes)

static void Refresh(ApfsFs *self, ApfsFs::Node *node)
{
	Stat st;
	if (self->Vol().GetStat(node->st.inode, st))
		node->st = st;
}

static uint32_t ModeForAttributes(uint32_t mode, uint32_t attrs)
{
	mode &= 07777;
	if (attrs & FILE_ATTRIBUTE_READONLY)
		return mode & ~0222u;
	if (!(mode & 0222))
		return mode | 0200;
	return mode;
}

static NTSTATUS Create(FSP_FILE_SYSTEM *fs, PWSTR name, UINT32 create_options, UINT32 granted_access, UINT32 attrs,
	PSECURITY_DESCRIPTOR sd, UINT64 alloc, PVOID *ctx, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	if (!self->Vol().Writable())
		return STATUS_MEDIA_WRITE_PROTECTED;

	uint64_t parent;
	std::string aname;
	std::wstring pnorm;
	NTSTATUS r = self->ResolveParent(name, parent, aname, &pnorm);
	if (!NT_SUCCESS(r))
		return r;
	bool dir = (create_options & FILE_DIRECTORY_FILE) != 0;
	uint32_t mode = dir ? 0755 : ((attrs & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
	DirEntry e;
	int err = self->Vol().Create(parent, aname, dir, mode, e);
	if (err)
		return Errno(err);

	std::unique_ptr<ApfsFs::Node> node(new ApfsFs::Node());
	if (!self->Vol().GetStat(e.inode, node->st))
		return STATUS_FILE_CORRUPT_ERROR;
	node->name = aname;
	self->FillFileInfo(node->st, node->name, *fi);

	std::wstring norm = pnorm + L"\\" + ApfsNameToWindows(aname);
	FSP_FSCTL_OPEN_FILE_INFO *ofi = FspFileSystemGetOpenFileInfo(fi);
	size_t bytes = norm.size() * sizeof(wchar_t);
	if (ofi->NormalizedName && bytes <= ofi->NormalizedNameSize)
	{
		memcpy(ofi->NormalizedName, norm.data(), bytes);
		ofi->NormalizedNameSize = static_cast<UINT16>(bytes);
	}
	*ctx = node.release();
	return STATUS_SUCCESS;
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT32 attrs, BOOLEAN replace, UINT64 alloc, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (!self->Vol().Writable())
		return STATUS_MEDIA_WRITE_PROTECTED;
	int err = self->Vol().SetSize(node->st.inode, 0);
	if (!err)
	{
		uint32_t mode = ModeForAttributes(node->st.mode, replace ? attrs : (attrs | self->Attributes(node->st, node->name)));
		if (mode != (node->st.mode & 07777u))
			err = self->Vol().SetMode(node->st.inode, mode);
	}
	Refresh(self, node);
	self->FillFileInfo(node->st, node->name, *fi);
	return Errno(err);
}

static NTSTATUS Write(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, UINT64 off, ULONG len, BOOLEAN to_eof, BOOLEAN constrained,
	PULONG transferred, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	*transferred = 0;
	if (node->st.type == FileType::Dir)
		return STATUS_INVALID_DEVICE_REQUEST;
	Refresh(self, node);
	if (to_eof)
		off = node->st.size;
	if (constrained)
	{
		// paging I/O: never extends the file
		if (off >= node->st.size)
		{
			self->FillFileInfo(node->st, node->name, *fi);
			return STATUS_SUCCESS;
		}
		len = static_cast<ULONG>(std::min<uint64_t>(len, node->st.size - off));
	}
	size_t written = 0;
	int err = self->Vol().Write(node->st.inode, off, buf, len, written);
	*transferred = static_cast<ULONG>(written);
	Refresh(self, node);
	self->FillFileInfo(node->st, node->name, *fi);
	return written ? STATUS_SUCCESS : Errno(err);
}

static NTSTATUS SetFileSize(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT64 size, BOOLEAN set_alloc, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	int err = 0;
	Refresh(self, node);
	// an allocation size below the file size truncates (NTFS semantics);
	// otherwise allocation requests are hints the engine doesn't need
	if (!set_alloc || size < node->st.size)
		err = self->Vol().SetSize(node->st.inode, size);
	Refresh(self, node);
	self->FillFileInfo(node->st, node->name, *fi);
	return Errno(err);
}

static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM *fs, PVOID ctx, UINT32 attrs, UINT64 ctime, UINT64 atime, UINT64 mtime, UINT64 chtime,
	FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	int err = 0;
	Refresh(self, node);
	if (attrs != INVALID_FILE_ATTRIBUTES)
	{
		uint32_t mode = ModeForAttributes(node->st.mode, attrs);
		if (mode != (node->st.mode & 07777u))
			err = self->Vol().SetMode(node->st.inode, mode);
	}
	// creation time can't be set through the engine yet (see CLAUDE.md)
	if (!err && (atime || mtime))
		err = self->Vol().SetTimes(node->st.inode, atime ? FileTimeToNs(atime) : INT64_MIN, mtime ? FileTimeToNs(mtime) : INT64_MIN);
	Refresh(self, node);
	self->FillFileInfo(node->st, node->name, *fi);
	return Errno(err);
}

static NTSTATUS CanDelete(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (!self->Vol().Writable())
		return STATUS_MEDIA_WRITE_PROTECTED;
	if (node->st.type == FileType::Dir)
	{
		std::vector<DirEntry> ents;
		if (!self->Vol().ListDir(node->st.inode, ents))
			return STATUS_FILE_CORRUPT_ERROR;
		if (!ents.empty())
			return STATUS_DIRECTORY_NOT_EMPTY;
	}
	return STATUS_SUCCESS;
}

static VOID Cleanup(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name, ULONG flags)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (!(flags & FspCleanupDelete) || !name)
		return;
	uint64_t parent;
	std::string aname;
	if (!NT_SUCCESS(self->ResolveParent(name, parent, aname, nullptr)))
		return;
	int err = self->Vol().Remove(parent, aname, node->st.type == FileType::Dir);
	if (err && self->Options().debug)
		fprintf(stderr, "delete %ls: errno %d\n", name, -err);
	self->InvalidateCache();
}

static NTSTATUS Rename(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR name, PWSTR new_name, BOOLEAN replace)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	uint64_t op = 0, np = 0;
	std::string on, nn;
	NTSTATUS r = self->ResolveParent(name, op, on, nullptr);
	if (NT_SUCCESS(r))
		r = self->ResolveParent(new_name, np, nn, nullptr);
	if (!NT_SUCCESS(r))
		return r;
	int err = self->Vol().Rename(op, on, np, nn, replace != FALSE);
	if (err == -21 || err == -20)   /* EISDIR/ENOTDIR: replacing across file and directory */
		return STATUS_ACCESS_DENIED;
	if (!err)
		node->name = nn;
	self->InvalidateCache();
	return Errno(err);
}

static NTSTATUS Flush(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	int err = self->Vol().Writable() ? self->Vol().Sync() : 0;
	if (ctx)
	{
		ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
		Refresh(self, node);
		self->FillFileInfo(node->st, node->name, *fi);
	}
	return Errno(err);
}

static VOID Close(FSP_FILE_SYSTEM *fs, PVOID ctx)
{
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	FspFileSystemDeleteDirectoryBuffer(&node->dir_buffer);
	delete node;
}

static NTSTATUS Read(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buf, UINT64 off, ULONG len, PULONG transferred)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	*transferred = 0;

	if (node->st.type == FileType::Dir)
		return STATUS_INVALID_DEVICE_REQUEST;

	FileReader *reader;
	{
		std::lock_guard<std::mutex> lock(node->mutex);
		if (!node->reader)
		{
			std::string err;
			node->reader = self->Vol().OpenFile(node->st, err);
			if (!node->reader)
			{
				if (self->Options().debug)
					fprintf(stderr, "open inode %llu: %s\n", static_cast<unsigned long long>(node->st.inode), err.c_str());
				return STATUS_FILE_CORRUPT_ERROR;
			}
		}
		reader = node->reader.get();
	}

	if (off >= reader->Size())
		return STATUS_END_OF_FILE;

	size_t got = 0;
	if (!reader->Read(off, buf, len, got))
		return STATUS_FILE_CORRUPT_ERROR;
	*transferred = static_cast<ULONG>(got);
	return STATUS_SUCCESS;
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM *fs, PVOID ctx, FSP_FSCTL_FILE_INFO *fi)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	if (self->Vol().Writable())
		Refresh(self, node);
	self->FillFileInfo(node->st, node->name, *fi);
	return STATUS_SUCCESS;
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM *fs, PVOID ctx, PSECURITY_DESCRIPTOR sd, SIZE_T *sd_size)
{
	return Self(fs)->CopySecurity(sd, sd_size);
}

static bool AddDirEntry(ApfsFs *self, ApfsFs::Node *node, const std::wstring &wname, const std::string &name, const Stat &st, NTSTATUS &result)
{
	alignas(8) UINT8 raw[sizeof(FSP_FSCTL_DIR_INFO) + 256 * sizeof(WCHAR)];
	FSP_FSCTL_DIR_INFO *d = reinterpret_cast<FSP_FSCTL_DIR_INFO *>(raw);
	size_t bytes = std::min<size_t>(wname.size(), 255) * sizeof(WCHAR);
	memset(d, 0, sizeof(FSP_FSCTL_DIR_INFO));
	d->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) + bytes);
	self->FillFileInfo(st, name, d->FileInfo);
	memcpy(d->FileNameBuf, wname.data(), bytes);
	return FspFileSystemFillDirectoryBuffer(&node->dir_buffer, d, &result) != FALSE;
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR pattern, PWSTR marker, PVOID buf, ULONG len, PULONG transferred)
{
	ApfsFs *self = Self(fs);
	ApfsFs::Node *node = static_cast<ApfsFs::Node *>(ctx);
	NTSTATUS result = STATUS_SUCCESS;

	if (node->st.type != FileType::Dir)
		return STATUS_NOT_A_DIRECTORY;

	if (FspFileSystemAcquireDirectoryBuffer(&node->dir_buffer, marker == nullptr, &result))
	{
		bool ok = true;
		if (node->st.inode != kRootInode)
		{
			Stat parent;
			if (!self->Vol().GetStat(node->st.parent, parent))
				parent = node->st;
			ok = AddDirEntry(self, node, L".", ".", node->st, result) && AddDirEntry(self, node, L"..", "..", parent, result);
		}

		std::vector<DirEntry> ents;
		if (ok && !self->Vol().ListDir(node->st.inode, ents))
		{
			result = STATUS_FILE_CORRUPT_ERROR;
			ok = false;
		}
		for (size_t i = 0; ok && i < ents.size(); i++)
		{
			Stat st;
			if (!self->Vol().GetStat(ents[i].inode, st))
				continue;
			ok = AddDirEntry(self, node, ApfsNameToWindows(ents[i].name), ents[i].name, st, result);
		}
		FspFileSystemReleaseDirectoryBuffer(&node->dir_buffer);
	}

	if (!NT_SUCCESS(result))
		return result;

	FspFileSystemReadDirectoryBuffer(&node->dir_buffer, marker, buf, len, transferred);
	return STATUS_SUCCESS;
}

static FSP_FILE_SYSTEM_INTERFACE g_interface = [] {
	FSP_FILE_SYSTEM_INTERFACE i;
	memset(&i, 0, sizeof(i));
	i.GetVolumeInfo = GetVolumeInfo;
	i.GetSecurityByName = GetSecurityByName;
	i.Create = Create;
	i.Open = Open;
	i.Overwrite = Overwrite;
	i.Cleanup = Cleanup;
	i.Close = Close;
	i.Read = Read;
	i.Write = Write;
	i.Flush = Flush;
	i.GetFileInfo = GetFileInfo;
	i.SetBasicInfo = SetBasicInfo;
	i.SetFileSize = SetFileSize;
	i.CanDelete = CanDelete;
	i.Rename = Rename;
	i.GetSecurity = GetSecurity;
	i.ReadDirectory = ReadDirectory;
	return i;
}();

NTSTATUS ApfsFs::Start(const wchar_t *mount_point)
{
	const VolumeInfo &info = m_vol->Info();
	FSP_FSCTL_VOLUME_PARAMS vp;
	memset(&vp, 0, sizeof(vp));
	vp.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
	vp.SectorSize = 4096;
	vp.SectorsPerAllocationUnit = static_cast<UINT16>(std::max<uint32_t>(1, m_block_size / 4096));
	vp.MaxComponentLength = 255;
	vp.VolumeCreationTime = NsToFileTime(info.last_mod_ns);
	vp.VolumeSerialNumber = static_cast<UINT32>(std::hash<std::string>()(info.uuid));
	// read-only: metadata never changes, cache it forever
	vp.FileInfoTimeout = m_vol->Writable() ? 1000 : 0xFFFFFFFF;
	vp.CaseSensitiveSearch = info.case_sensitive ? 1 : 0;
	vp.CasePreservedNames = 1;
	vp.UnicodeOnDisk = 1;
	vp.PersistentAcls = 1;
	vp.ReadOnlyVolume = m_vol->Writable() ? 0 : 1;
	vp.PostCleanupWhenModifiedOnly = 1;
	vp.UmFileContextIsUserContext2 = 1;
	wcscpy_s(vp.FileSystemName, L"APFS");

	NTSTATUS r = FspFileSystemCreate(const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME), &vp, &g_interface, &m_fs);
	if (!NT_SUCCESS(r))
		return r;
	m_fs->UserContext = this;
	if (m_opts.debug)
	{
		FspDebugLogSetHandle(GetStdHandle(STD_ERROR_HANDLE));
		FspFileSystemSetDebugLog(m_fs, static_cast<UINT32>(-1));
	}

	r = FspFileSystemSetMountPoint(m_fs, const_cast<PWSTR>(mount_point));
	if (NT_SUCCESS(r))
		r = FspFileSystemStartDispatcher(m_fs, 0);
	if (!NT_SUCCESS(r))
	{
		FspFileSystemDelete(m_fs);
		m_fs = nullptr;
	}
	return r;
}

void ApfsFs::Stop()
{
	if (!m_fs)
		return;
	FspFileSystemStopDispatcher(m_fs);
	FspFileSystemDelete(m_fs);
	m_fs = nullptr;
}

std::wstring ApfsFs::MountPoint() const
{
	if (!m_fs)
		return std::wstring();
	PWSTR mp = FspFileSystemMountPoint(m_fs);
	return mp ? std::wstring(mp) : std::wstring();
}

bool LoadWinFsp()
{
	return NT_SUCCESS(FspLoad(nullptr));
}

} // namespace winapfs
