// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Volume on the linux-apfs-rw engine (apfsrw API). The engine works with
// looked-up nodes (dentries); the Volume interface works with inode numbers,
// so every node we see is kept in an inode -> node map until unmount.

#include "Apfs.h"

#include <apfsrw.h>

#include <mutex>
#include <unordered_map>

namespace winapfs {

namespace {

FileType TypeFromMode(uint32_t mode)
{
	switch (mode & 0170000)
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

FileType TypeFromDtype(unsigned dt)
{
	switch (dt)
	{
	case APFSRW_DT_REG: return FileType::File;
	case APFSRW_DT_DIR: return FileType::Dir;
	case APFSRW_DT_LNK: return FileType::Symlink;
	case APFSRW_DT_FIFO: return FileType::Fifo;
	case APFSRW_DT_CHR: return FileType::CharDev;
	case APFSRW_DT_BLK: return FileType::BlockDev;
	case APFSRW_DT_SOCK: return FileType::Socket;
	default: return FileType::Unknown;
	}
}

class RwReader : public FileReader
{
public:
	RwReader(apfsrw_file *f, bool serialize) : m_file(f), m_serialize(serialize) {}
	~RwReader() override { apfsrw_release(m_file); }

	uint64_t Size() const override { return apfsrw_file_size(m_file); }   // live: files can grow

	bool Read(uint64_t off, void *buf, size_t len, size_t &got) override
	{
		// the driver keeps decompression state per open file
		std::unique_lock<std::mutex> lock(m_mutex, std::defer_lock);
		if (m_serialize)
			lock.lock();
		return apfsrw_read(m_file, off, buf, len, &got) == 0;
	}

private:
	apfsrw_file *m_file;
	bool m_serialize;
	std::mutex m_mutex;
};

class RwVolume : public Volume
{
public:
	RwVolume(apfsrw_mount *m, const VolumeInfo &info) : Volume(info), m_mount(m)
	{
		apfsrw_node *root = nullptr;
		apfsrw_root(m_mount, &root);
		m_nodes[kRootInode] = root;
		m_parent[kRootInode] = kRootParent;
	}

	~RwVolume() override
	{
		for (auto &kv : m_nodes)
			apfsrw_put(kv.second);
		m_nodes.clear();
		apfsrw_unmount(m_mount);
	}

	const char *EngineName() const override { return "linux-apfs-rw"; }

	bool GetSpace(VolumeSpace &space) override
	{
		apfsrw_statfs s;
		if (apfsrw_get_statfs(m_mount, &s))
			return false;
		space.block_size = static_cast<uint32_t>(s.block_size);
		space.total_bytes = s.blocks * s.block_size;
		space.free_bytes = s.blocks_avail * s.block_size;
		return true;
	}

	bool GetStat(uint64_t inode, Stat &st) override
	{
		apfsrw_node *n = Get(inode);
		apfsrw_stat s;
		st = Stat();
		if (inode == kRootParent)
		{
			st.inode = kRootParent;
			st.type = FileType::Dir;
			st.mode = 0040755;
			return true;
		}
		if (!n || apfsrw_getattr(n, &s))
			return false;
		st.inode = s.ino;
		st.parent = Parent(inode);
		st.mode = static_cast<uint16_t>(s.mode);
		st.type = TypeFromMode(s.mode);
		st.uid = s.uid;
		st.gid = s.gid;
		st.bsd_flags = s.bsd_flags;
		st.nlink = s.nlink;
		st.size = s.size;
		st.alloc_size = s.blocks * 512;
		st.create_ns = s.btime_ns;
		st.mod_ns = s.mtime_ns;
		st.change_ns = s.ctime_ns;
		st.access_ns = s.atime_ns;
		st.compressed = (s.bsd_flags & kUF_COMPRESSED) != 0;
		st.rdev = s.rdev;
		return true;
	}

	bool Lookup(uint64_t parent, const std::string &name, DirEntry &out) override
	{
		apfsrw_node *p = Get(parent);
		if (!p)
			return false;
		char stored[1024];
		apfsrw_node *child = nullptr;
		if (apfsrw_lookup_stored(p, name.data(), name.size(), &child, stored, sizeof(stored)))
			return false;
		apfsrw_stat s;
		if (apfsrw_getattr(child, &s))
		{
			apfsrw_put(child);
			return false;
		}
		Remember(s.ino, child, parent);
		out.name = stored;
		out.inode = s.ino;
		out.type = TypeFromMode(s.mode);
		return true;
	}

	bool ListDir(uint64_t dir_inode, std::vector<DirEntry> &entries) override
	{
		apfsrw_node *dir = Get(dir_inode);
		entries.clear();
		if (!dir)
			return false;
		struct Ctx
		{
			std::vector<DirEntry> *out;
		} ctx{ &entries };
		int err = apfsrw_readdir(dir, [](void *c, const char *name, size_t len, uint64_t ino, unsigned dtype) {
			DirEntry e;
			e.name.assign(name, len);
			e.inode = ino;
			e.type = TypeFromDtype(dtype);
			static_cast<Ctx *>(c)->out->push_back(std::move(e));
			return 0;
		}, &ctx);
		if (err)
			return false;
		// callers stat the entries next; make every child addressable by inode
		for (const auto &e : entries)
		{
			if (Get(e.inode))
				continue;
			apfsrw_node *child = nullptr;
			if (apfsrw_lookup(dir, e.name.data(), e.name.size(), &child) == 0)
				Remember(e.inode, child, dir_inode);
		}
		return true;
	}

	std::unique_ptr<FileReader> OpenFile(const Stat &st, std::string &err) override
	{
		apfsrw_node *n = Get(st.inode);
		apfsrw_file *f = nullptr;
		if (st.type != FileType::File)
		{
			err = "not a regular file";
			return nullptr;
		}
		int rc = n ? apfsrw_open(n, false, &f) : -2;
		if (rc)
		{
			err = "open failed (errno " + std::to_string(-rc) + ")";
			return nullptr;
		}
		return std::unique_ptr<FileReader>(new RwReader(f, st.compressed));
	}

	bool ListXattrs(uint64_t inode, const Stat &st, std::vector<std::string> &names) override
	{
		apfsrw_node *n = Get(inode);
		names.clear();
		if (!n)
			return false;
		long size = apfsrw_listxattr(n, nullptr, 0);
		if (size <= 0)
			return true;
		std::vector<char> buf(size);
		size = apfsrw_listxattr(n, buf.data(), buf.size());
		for (long p = 0; p < size; p += static_cast<long>(strlen(buf.data() + p)) + 1)
		{
			std::string name = buf.data() + p;
			if (name == "com.apple.decmpfs" || name.compare(0, 13, "com.apple.fs.") == 0)
				continue;
			if (name == "com.apple.ResourceFork")
			{
				uint64_t rsize = 0;
				if (st.compressed || !GetXattrSize(inode, name, rsize) || rsize == 0)
					continue;
			}
			names.push_back(std::move(name));
		}
		return true;
	}

	bool GetXattr(uint64_t inode, const std::string &name, std::vector<uint8_t> &data) override
	{
		apfsrw_node *n = Get(inode);
		if (!n)
			return false;
		long size = apfsrw_getxattr(n, name.c_str(), nullptr, 0);
		if (size < 0)
			return false;
		data.resize(size);
		size = apfsrw_getxattr(n, name.c_str(), data.data(), data.size());
		if (size < 0)
			return false;
		data.resize(size);
		return true;
	}

	bool GetXattrSize(uint64_t inode, const std::string &name, uint64_t &size) override
	{
		apfsrw_node *n = Get(inode);
		long s = n ? apfsrw_getxattr(n, name.c_str(), nullptr, 0) : -1;
		if (s < 0)
			return false;
		size = static_cast<uint64_t>(s);
		return true;
	}

	bool ReadXattr(uint64_t inode, const std::string &name, uint64_t off, void *buf, size_t len, size_t &got) override
	{
		std::vector<uint8_t> data;
		got = 0;
		if (!GetXattr(inode, name, data))
			return false;
		if (off < data.size())
		{
			got = static_cast<size_t>(std::min<uint64_t>(len, data.size() - off));
			memcpy(buf, data.data() + off, got);
		}
		return true;
	}

	bool ReadLink(uint64_t inode, std::string &target) override
	{
		apfsrw_node *n = Get(inode);
		if (!n)
			return false;
		std::vector<char> buf(4096);
		int len = apfsrw_readlink(n, buf.data(), buf.size());
		if (len < 0)
			return false;
		if (static_cast<size_t>(len) >= buf.size())
		{
			buf.resize(len + 1);
			len = apfsrw_readlink(n, buf.data(), buf.size());
		}
		target.assign(buf.data(), len);
		return true;
	}

	// ---------------------------------------------------------------- writing

	bool Writable() const override { return apfsrw_is_readwrite(m_mount); }

	int Create(uint64_t parent, const std::string &name, bool dir, uint32_t mode, DirEntry &out) override
	{
		apfsrw_node *p = Get(parent), *n = nullptr;
		if (!p)
			return -2;
		int err = dir ? apfsrw_mkdir(p, name.data(), name.size(), mode, &n) : apfsrw_create(p, name.data(), name.size(), mode, &n);
		if (err)
			return err;
		apfsrw_stat s;
		err = apfsrw_getattr(n, &s);
		if (err)
		{
			apfsrw_put(n);
			return err;
		}
		Remember(s.ino, n, parent);
		out.name = name;
		out.inode = s.ino;
		out.type = dir ? FileType::Dir : FileType::File;
		return 0;
	}

	int Remove(uint64_t parent, const std::string &name, bool dir) override
	{
		apfsrw_node *p = Get(parent);
		if (!p)
			return -2;
		uint64_t victim = InodeOf(p, name);
		int err = dir ? apfsrw_rmdir(p, name.data(), name.size()) : apfsrw_unlink(p, name.data(), name.size());
		if (!err && victim)
			ForgetIfUnlinked(victim);
		return err;
	}

	int Rename(uint64_t old_parent, const std::string &old_name, uint64_t new_parent, const std::string &new_name, bool replace) override
	{
		apfsrw_node *op = Get(old_parent), *np = Get(new_parent);
		if (!op || !np)
			return -2;
		uint64_t moved = InodeOf(op, old_name), replaced = InodeOf(np, new_name);
		int err = apfsrw_rename(op, old_name.data(), old_name.size(), np, new_name.data(), new_name.size(),
			replace ? 0 : APFSRW_RENAME_NOREPLACE);
		if (err)
			return err;
		if (moved)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_parent[moved] = new_parent;
		}
		if (replaced && replaced != moved)
			ForgetIfUnlinked(replaced);
		return 0;
	}

	int Write(uint64_t inode, uint64_t off, const void *buf, size_t len, size_t &written) override
	{
		apfsrw_node *n = Get(inode);
		apfsrw_file *f = nullptr;
		written = 0;
		if (!n)
			return -2;
		int err = apfsrw_open(n, true, &f);
		if (err)
			return err;
		err = apfsrw_write(f, off, buf, len, &written);
		apfsrw_release(f);
		return err;
	}

	int SetSize(uint64_t inode, uint64_t size) override
	{
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_SIZE;
		a.size = size;
		return Setattr(inode, a);
	}

	int SetTimes(uint64_t inode, int64_t atime_ns, int64_t mtime_ns) override
	{
		apfsrw_setattr_t a = {};
		if (atime_ns != INT64_MIN)
		{
			a.mask |= APFSRW_SET_ATIME;
			a.atime_ns = atime_ns;
		}
		if (mtime_ns != INT64_MIN)
		{
			a.mask |= APFSRW_SET_MTIME;
			a.mtime_ns = mtime_ns;
		}
		return a.mask ? Setattr(inode, a) : 0;
	}

	int SetMode(uint64_t inode, uint32_t mode) override
	{
		apfsrw_setattr_t a = {};
		a.mask = APFSRW_SET_MODE;
		a.mode = mode;
		return Setattr(inode, a);
	}

	int Sync() override
	{
		return apfsrw_sync(m_mount);
	}

private:
	int Setattr(uint64_t inode, const apfsrw_setattr_t &a)
	{
		apfsrw_node *n = Get(inode);
		return n ? apfsrw_setattr(n, &a) : -2;
	}

	// Inode number behind a name, or 0.
	uint64_t InodeOf(apfsrw_node *dir, const std::string &name)
	{
		apfsrw_node *n = nullptr;
		apfsrw_stat s;
		uint64_t ino = 0;
		if (apfsrw_lookup(dir, name.data(), name.size(), &n) == 0)
		{
			if (apfsrw_getattr(n, &s) == 0)
				ino = s.ino;
			apfsrw_put(n);
		}
		return ino;
	}

	// Drop our node for an inode that lost its last name, so the engine can
	// evict it (that is what frees its blocks).
	void ForgetIfUnlinked(uint64_t inode)
	{
		apfsrw_node *n = Get(inode);
		apfsrw_stat s;
		if (!n || (apfsrw_getattr(n, &s) == 0 && s.nlink > 0))
			return;
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_nodes.find(inode);
		if (it != m_nodes.end())
		{
			apfsrw_put(it->second);
			m_nodes.erase(it);
			m_parent.erase(inode);
		}
	}

	// Borrowed pointer; the map holds the reference until unmount.
	apfsrw_node *Get(uint64_t inode)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_nodes.find(inode);
		return it == m_nodes.end() ? nullptr : it->second;
	}

	uint64_t Parent(uint64_t inode)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_parent.find(inode);
		return it == m_parent.end() ? 0 : it->second;
	}

	// Takes ownership of node's reference.
	void Remember(uint64_t inode, apfsrw_node *node, uint64_t parent)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_nodes.find(inode);
		if (it == m_nodes.end())
			m_nodes[inode] = node;
		else
			apfsrw_put(node);
		m_parent[inode] = parent;
	}

	apfsrw_mount *m_mount;
	std::mutex m_mutex;
	std::unordered_map<uint64_t, apfsrw_node *> m_nodes;
	std::unordered_map<uint64_t, uint64_t> m_parent;
};

} // namespace

std::unique_ptr<Volume> OpenRwVolume(const Container &c, unsigned fsid, bool readwrite, std::string &err)
{
	const VolumeInfo *info = nullptr;
	for (const auto &v : c.Volumes())
		if (v.fsid == fsid)
			info = &v;
	if (!info)
	{
		err = "no volume " + std::to_string(fsid);
		return nullptr;
	}
	if (info->encrypted)
	{
		err = "the linux-apfs-rw engine cannot open encrypted volumes";
		return nullptr;
	}
	apfsrw_mount *m = nullptr;
	int rc = apfsrw_mount_volume(Utf8ToWide(c.Path()).c_str(), c.PartitionOffset(), c.PartitionSize(), fsid, readwrite, nullptr, &m);
	if (rc)
	{
		err = "linux-apfs-rw could not mount " + info->name + " (errno " + std::to_string(-rc) + ")";
		return nullptr;
	}
	return std::unique_ptr<Volume>(new RwVolume(m, *info));
}

} // namespace winapfs
