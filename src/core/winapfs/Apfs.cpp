// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Apfs.h"
#include "Compression.h"
#include "Gpt.h"

#include <ApfsLib/ApfsContainer.h>
#include <ApfsLib/ApfsDir.h>
#include <ApfsLib/ApfsVolume.h>
#include <ApfsLib/Device.h>
#include <ApfsLib/DiskStruct.h>
#include <ApfsLib/Decmpfs.h>
#include <ApfsLib/Util.h>

#include <Windows.h>

#include <cstring>
#include <iomanip>
#include <sstream>

namespace winapfs {

const char *FileTypeName(FileType t)
{
	switch (t)
	{
	case FileType::File: return "file";
	case FileType::Dir: return "dir";
	case FileType::Symlink: return "symlink";
	case FileType::Fifo: return "fifo";
	case FileType::CharDev: return "chardev";
	case FileType::BlockDev: return "blockdev";
	case FileType::Socket: return "socket";
	default: return "unknown";
	}
}

static FileType TypeFromMode(uint16_t mode)
{
	switch (mode & MODE_S_IFMT)
	{
	case 0100000: return FileType::File;
	case 0040000: return FileType::Dir;
	case 0120000: return FileType::Symlink;
	case 0010000: return FileType::Fifo;
	case 0020000: return FileType::CharDev;
	case 0060000: return FileType::BlockDev;
	case 0140000: return FileType::Socket;
	default: return FileType::Unknown;
	}
}

static FileType TypeFromDrecFlags(uint16_t flags)
{
	switch (flags & DREC_TYPE_MASK)
	{
	case 8: return FileType::File;
	case 4: return FileType::Dir;
	case 10: return FileType::Symlink;
	case 1: return FileType::Fifo;
	case 2: return FileType::CharDev;
	case 6: return FileType::BlockDev;
	case 12: return FileType::Socket;
	default: return FileType::Unknown;
	}
}

std::wstring Utf8ToWide(const std::string &s)
{
	if (s.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
	return w;
}

std::string WideToUtf8(const std::wstring &w)
{
	if (w.empty())
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
	return s;
}

// ---------------------------------------------------------------- Container

Container::~Container()
{
	m_nx.reset();
	if (m_dev)
		m_dev->Close();
}

bool Container::FindApfsPartitions(const std::string &path, std::vector<PartitionInfo> &parts, std::string &err)
{
	std::unique_ptr<Device> dev(Device::OpenDevice(path.c_str()));
	if (!dev)
	{
		err = "cannot open " + path;
		return false;
	}
	parts.clear();
	bool ok = ReadGptApfsPartitions(*dev, parts);
	dev->Close();
	(void)ok;
	return true;
}

std::unique_ptr<Container> Container::Open(const std::string &path, std::string &err, int partition)
{
	std::unique_ptr<Container> c(new Container());
	c->m_path = path;
	c->m_dev.reset(Device::OpenDevice(path.c_str()));
	if (!c->m_dev)
	{
		err = "cannot open " + path + " (error " + std::to_string(GetLastError()) + ")";
		return nullptr;
	}

	std::vector<PartitionInfo> parts;
	ReadGptApfsPartitions(*c->m_dev, parts);
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
			err = "no APFS partition with index " + std::to_string(partition);
			return nullptr;
		}
		c->m_offset = p->offset;
		c->m_size = p->size;
	}
	else
	{
		if (partition >= 0)
		{
			err = "no GPT partition table on " + path;
			return nullptr;
		}
		c->m_offset = 0;
		c->m_size = c->m_dev->GetSize();
	}

	c->m_nx.reset(new ApfsContainer(c->m_dev.get(), c->m_offset, c->m_size));
	if (!c->m_nx->Init())
	{
		err = "no valid APFS container found in " + path;
		return nullptr;
	}

	for (unsigned fsid = 0; fsid < NX_MAX_FILE_SYSTEMS; fsid++)
	{
		apfs_superblock_t sb;
		if (!c->m_nx->GetVolumeInfo(fsid, sb))
			continue;
		VolumeInfo vi;
		vi.fsid = fsid;
		vi.name.assign(reinterpret_cast<const char *>(sb.apfs_volname), strnlen(reinterpret_cast<const char *>(sb.apfs_volname), sizeof(sb.apfs_volname)));
		vi.uuid = uuidstr(sb.apfs_vol_uuid);
		vi.role = sb.apfs_role;
		vi.encrypted = (sb.apfs_fs_flags & APFS_FS_CRYPTOFLAGS) != APFS_FS_UNENCRYPTED && !c->m_nx->IsUnencrypted();
		vi.case_sensitive = (sb.apfs_incompatible_features & APFS_INCOMPAT_CASE_INSENSITIVE) == 0;
		vi.normalization_insensitive = (sb.apfs_incompatible_features & APFS_INCOMPAT_NORMALIZATION_INSENSITIVE) != 0;
		vi.sealed = (sb.apfs_incompatible_features & APFS_INCOMPAT_SEALED_VOLUME) != 0;
		vi.bytes_used = sb.apfs_fs_alloc_count * static_cast<uint64_t>(c->m_nx->GetBlocksize());
		vi.quota_bytes = sb.apfs_fs_quota_block_count * static_cast<uint64_t>(c->m_nx->GetBlocksize());
		vi.reserve_bytes = sb.apfs_fs_reserve_block_count * static_cast<uint64_t>(c->m_nx->GetBlocksize());
		vi.last_mod_ns = static_cast<int64_t>(static_cast<uint64_t>(sb.apfs_last_mod_time));
		if (vi.encrypted)
			c->m_nx->GetPasswordHint(vi.password_hint, sb.apfs_vol_uuid);
		c->m_volumes.push_back(vi);
	}

	return c;
}

std::vector<PhysicalDrive> EnumeratePhysicalDrives()
{
	std::vector<PhysicalDrive> drives;
	for (int n = 0; n < 64; n++)
	{
		PhysicalDrive d;
		d.path = "\\\\.\\PhysicalDrive" + std::to_string(n);
		std::unique_ptr<Device> dev(Device::OpenDevice(d.path.c_str()));
		if (!dev)
		{
			DWORD err = GetLastError();
			if (err == ERROR_ACCESS_DENIED)
			{
				d.access_denied = true;
				drives.push_back(d);
			}
			continue;
		}
		d.size = dev->GetSize();
		d.has_gpt = ReadGptApfsPartitions(*dev, d.apfs);
		dev->Close();
		drives.push_back(d);
	}
	return drives;
}

const VolumeInfo *Container::FindVolume(const std::string &key) const
{
	for (const auto &v : m_volumes)
		if (v.name == key)
			return &v;
	char *end = nullptr;
	unsigned long idx = strtoul(key.c_str(), &end, 10);
	if (!key.empty() && end && *end == 0)
		for (const auto &v : m_volumes)
			if (v.fsid == idx)
				return &v;
	return nullptr;
}

std::unique_ptr<Volume> Container::OpenVolume(unsigned fsid, const std::string &passphrase, std::string &err)
{
	const VolumeInfo *info = nullptr;
	for (const auto &v : m_volumes)
		if (v.fsid == fsid)
			info = &v;
	if (!info)
	{
		err = "no volume " + std::to_string(fsid);
		return nullptr;
	}

	std::lock_guard<std::mutex> lock(m_open_mutex);
	std::unique_ptr<ApfsVolume> vol(m_nx->GetVolume(fsid, passphrase));
	if (!vol)
	{
		if (info->encrypted)
			err = passphrase.empty() ? "volume is encrypted; a password is required" : "wrong password for encrypted volume";
		else
			err = "cannot load volume " + info->name;
		return nullptr;
	}
	return std::unique_ptr<Volume>(new Volume(*this, std::move(vol), *info));
}

uint32_t Container::BlockSize() const { return m_nx->GetBlocksize(); }
uint64_t Container::BlockCount() const { return m_nx->GetBlockCount(); }
uint64_t Container::FreeBlocks() const { return m_nx->GetFreeBlocks(); }

// ---------------------------------------------------------------- Volume

Volume::Volume(Container &c, std::unique_ptr<ApfsVolume> v, const VolumeInfo &info)
	: m_container(c), m_vol(std::move(v)), m_info(info)
{
}

Volume::~Volume() = default;

bool Volume::GetStat(uint64_t inode, Stat &st)
{
	ApfsDir dir(*m_vol);
	ApfsDir::Inode rec;

	st = Stat();
	if (inode == kRootParent)
	{
		st.inode = kRootParent;
		st.type = FileType::Dir;
		st.mode = 0040755;
		return true;
	}

	if (!dir.GetInode(rec, inode))
		return false;

	st.inode = inode;
	st.parent = rec.parent_id;
	st.private_id = rec.private_id;
	st.mode = rec.mode;
	st.type = TypeFromMode(rec.mode);
	st.uid = rec.owner;
	st.gid = rec.group;
	st.bsd_flags = rec.bsd_flags;
	st.internal_flags = rec.internal_flags;
	st.nlink = rec.nchildren_nlink;
	st.create_ns = static_cast<int64_t>(rec.create_time);
	st.mod_ns = static_cast<int64_t>(rec.mod_time);
	st.change_ns = static_cast<int64_t>(rec.change_time);
	st.access_ns = static_cast<int64_t>(rec.access_time);
	st.compressed = (rec.bsd_flags & kUF_COMPRESSED) != 0;
	if (rec.optional_present_flags & ApfsDir::Inode::INO_HAS_RDEV)
		st.rdev = rec.rdev;
	if (rec.optional_present_flags & ApfsDir::Inode::INO_HAS_DSTREAM)
	{
		st.size = rec.ds_size;
		st.alloc_size = rec.ds_alloced_size;
	}

	if (st.type == FileType::File && st.compressed)
	{
		if (rec.internal_flags & INODE_HAS_UNCOMPRESSED_SIZE)
		{
			st.size = rec.uncompressed_size;
		}
		else
		{
			std::vector<uint8_t> hdr;
			if (dir.GetAttribute(hdr, inode, "com.apple.decmpfs") && hdr.size() >= sizeof(CompressionHeader))
				st.size = reinterpret_cast<const CompressionHeader *>(hdr.data())->size;
		}
	}

	return true;
}

bool Volume::Lookup(uint64_t parent, const std::string &name, DirEntry &out)
{
	ApfsDir dir(*m_vol);
	ApfsDir::DirRec rec;

	if (name.empty() || name.size() > 1000)
		return false;
	if (!dir.LookupName(rec, parent, name.c_str()))
		return false;
	out.name = rec.name;
	out.inode = rec.file_id;
	out.type = TypeFromDrecFlags(rec.flags);
	return true;
}

bool Volume::ResolvePath(const std::string &path, uint64_t &inode)
{
	uint64_t cur = kRootInode;
	size_t pos = 0;

	while (pos < path.size())
	{
		size_t next = path.find('/', pos);
		if (next == std::string::npos)
			next = path.size();
		if (next > pos)
		{
			DirEntry e;
			if (!Lookup(cur, path.substr(pos, next - pos), e))
				return false;
			cur = e.inode;
		}
		pos = next + 1;
	}

	inode = cur;
	return true;
}

bool Volume::ListDir(uint64_t dir_inode, std::vector<DirEntry> &entries)
{
	ApfsDir dir(*m_vol);
	std::vector<ApfsDir::DirRec> recs;

	entries.clear();
	if (!dir.ListDirectory(recs, dir_inode))
		return false;
	entries.reserve(recs.size());
	for (const auto &r : recs)
	{
		DirEntry e;
		e.name = r.name;
		e.inode = r.file_id;
		e.type = TypeFromDrecFlags(r.flags);
		entries.push_back(std::move(e));
	}
	return true;
}

bool Volume::ReadStream(uint64_t private_id, uint64_t off, void *buf, size_t len)
{
	ApfsDir dir(*m_vol);
	return dir.ReadFile(buf, private_id, off, len);
}

namespace {

class PlainReader : public FileReader
{
public:
	PlainReader(Volume &v, uint64_t private_id, uint64_t size) : m_vol(v), m_id(private_id), m_size(size) {}

	uint64_t Size() const override { return m_size; }

	bool Read(uint64_t off, void *buf, size_t len, size_t &got) override
	{
		got = 0;
		if (off >= m_size)
			return true;
		if (len > m_size - off)
			len = static_cast<size_t>(m_size - off);
		if (len == 0)
			return true;
		if (!m_vol.ReadStream(m_id, off, buf, len))
			return false;
		got = len;
		return true;
	}

private:
	Volume &m_vol;
	uint64_t m_id;
	uint64_t m_size;
};

} // namespace

std::unique_ptr<FileReader> Volume::OpenFile(const Stat &st, std::string &err)
{
	if (st.type != FileType::File)
	{
		err = "not a regular file";
		return nullptr;
	}
	if (st.compressed)
		return OpenCompressedFile(*this, st, err);
	return std::unique_ptr<FileReader>(new PlainReader(*this, st.private_id, st.size));
}

bool Volume::ListXattrs(uint64_t inode, const Stat &st, std::vector<std::string> &names)
{
	ApfsDir dir(*m_vol);
	std::vector<std::string> all;

	names.clear();
	if (!dir.ListAttributes(all, inode))
		return true;  // no xattrs is not an error
	for (auto &n : all)
	{
		if (n == "com.apple.decmpfs")
			continue;
		if (n.compare(0, 13, "com.apple.fs.") == 0)
			continue;
		if (n == "com.apple.ResourceFork")
		{
			// macOS hides the resource fork of compressed files and empty ones
			uint64_t size = 0;
			if (st.compressed || !GetXattrSize(inode, n, size) || size == 0)
				continue;
		}
		names.push_back(std::move(n));
	}
	return true;
}

bool Volume::GetXattr(uint64_t inode, const std::string &name, std::vector<uint8_t> &data)
{
	ApfsDir dir(*m_vol);
	return dir.GetAttribute(data, inode, name.c_str());
}

bool Volume::GetXattrSize(uint64_t inode, const std::string &name, uint64_t &size)
{
	ApfsDir dir(*m_vol);
	ApfsDir::XAttr xa;
	if (!dir.GetAttributeInfo(xa, inode, name.c_str()))
		return false;
	size = (xa.flags & XATTR_DATA_STREAM) ? static_cast<uint64_t>(xa.xstrm.dstream.size) : xa.xdata_len;
	return true;
}

bool Volume::ReadXattr(uint64_t inode, const std::string &name, uint64_t off, void *buf, size_t len, size_t &got)
{
	ApfsDir dir(*m_vol);
	ApfsDir::XAttr xa;
	got = 0;
	if (!dir.GetAttributeInfo(xa, inode, name.c_str()))
		return false;
	if (xa.flags & XATTR_DATA_STREAM)
	{
		uint64_t size = xa.xstrm.dstream.size;
		if (off >= size)
			return true;
		size_t n = static_cast<size_t>(std::min<uint64_t>(len, size - off));
		if (n && !dir.ReadFile(buf, xa.xstrm.xattr_obj_id, off, n))
			return false;
		got = n;
		return true;
	}
	std::vector<uint8_t> data;
	if (!dir.GetAttribute(data, inode, name.c_str()))
		return false;
	if (off >= data.size())
		return true;
	got = static_cast<size_t>(std::min<uint64_t>(len, data.size() - off));
	memcpy(buf, data.data() + off, got);
	return true;
}

bool Volume::ReadLink(uint64_t inode, std::string &target)
{
	std::vector<uint8_t> data;
	if (!GetXattr(inode, "com.apple.fs.symlink", data))
		return false;
	// stored NUL-terminated
	size_t n = data.size();
	while (n > 0 && data[n - 1] == 0)
		n--;
	target.assign(reinterpret_cast<const char *>(data.data()), n);
	return true;
}

} // namespace winapfs
