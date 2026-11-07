// winapfs - read-only APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Thin, thread-safe layer over ApfsLib used by the CLI, the WinFsp front-end
// and the tests. All names and paths are UTF-8 exactly as stored on disk.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Device;
class ApfsContainer;
class ApfsVolume;

namespace winapfs {

constexpr uint64_t kRootInode = 2;      // ROOT_DIR_INO_NUM
constexpr uint64_t kRootParent = 1;     // ROOT_DIR_PARENT

// APFS_GPT_PARTITION_UUID, 7C3457EF-0000-11AA-AA11-00306543ECAC
struct PartitionInfo
{
	int index = -1;             // GPT entry index
	uint64_t offset = 0;        // bytes
	uint64_t size = 0;          // bytes
	std::string name;           // GPT partition name (UTF-8)
};

struct VolumeInfo
{
	unsigned fsid = 0;          // index in nx_fs_oid[]
	std::string name;
	std::string uuid;
	uint16_t role = 0;
	bool encrypted = false;     // needs a key (FileVault / software encryption)
	bool case_sensitive = false;
	bool normalization_insensitive = false;
	bool sealed = false;
	uint64_t bytes_used = 0;
	uint64_t quota_bytes = 0;
	uint64_t reserve_bytes = 0;
	int64_t last_mod_ns = 0;
	std::string password_hint;  // encrypted volumes only, if the keybag has one
};

enum class FileType { Unknown, File, Dir, Symlink, Fifo, CharDev, BlockDev, Socket };

const char *FileTypeName(FileType t);

struct Stat
{
	uint64_t inode = 0;
	uint64_t parent = 0;
	uint64_t private_id = 0;    // data stream object id
	FileType type = FileType::Unknown;
	uint16_t mode = 0;          // full st_mode incl. S_IFMT
	uint32_t uid = 0;
	uint32_t gid = 0;
	uint32_t bsd_flags = 0;
	uint64_t internal_flags = 0;
	uint64_t nlink = 1;         // files: link count; dirs: 2 + subdirs is not stored, so number of children
	uint64_t size = 0;          // logical size (uncompressed for compressed files)
	uint64_t alloc_size = 0;
	int64_t create_ns = 0;      // nanoseconds since 1970-01-01 UTC
	int64_t mod_ns = 0;
	int64_t change_ns = 0;
	int64_t access_ns = 0;
	bool compressed = false;    // UF_COMPRESSED (decmpfs)
	uint32_t rdev = 0;
};

struct DirEntry
{
	std::string name;
	uint64_t inode = 0;
	FileType type = FileType::Unknown;
};

// BSD flags
constexpr uint32_t kUF_NODUMP = 0x00000001;
constexpr uint32_t kUF_IMMUTABLE = 0x00000002;
constexpr uint32_t kUF_APPEND = 0x00000004;
constexpr uint32_t kUF_COMPRESSED = 0x00000020;
constexpr uint32_t kUF_HIDDEN = 0x00008000;
constexpr uint32_t kSF_IMMUTABLE = 0x00020000;

class Volume;

// Random-access reader over a file's logical (decompressed) contents.
class FileReader
{
public:
	virtual ~FileReader() = default;
	virtual uint64_t Size() const = 0;
	// Reads up to len bytes at off; stops at EOF. Returns false on I/O or
	// decompression error.
	virtual bool Read(uint64_t off, void *buf, size_t len, size_t &got) = 0;
};

class Container
{
public:
	~Container();

	// path: an image file (bare container or GPT disk image) or a raw device
	// such as \\.\PhysicalDrive1. partition = -1 picks the first APFS GPT
	// partition, or the whole device when there is no GPT.
	static std::unique_ptr<Container> Open(const std::string &path, std::string &err, int partition = -1);

	// Lists APFS partitions on a GPT disk or image. Empty if no GPT.
	static bool FindApfsPartitions(const std::string &path, std::vector<PartitionInfo> &parts, std::string &err);

	const std::vector<VolumeInfo> &Volumes() const { return m_volumes; }
	const VolumeInfo *FindVolume(const std::string &name_or_index) const;

	// passphrase may be a volume password or a personal recovery key.
	std::unique_ptr<Volume> OpenVolume(unsigned fsid, const std::string &passphrase, std::string &err);

	uint32_t BlockSize() const;
	uint64_t BlockCount() const;
	uint64_t FreeBlocks() const;
	const std::string &Path() const { return m_path; }
	uint64_t PartitionOffset() const { return m_offset; }

private:
	Container() = default;

	std::string m_path;
	uint64_t m_offset = 0;
	uint64_t m_size = 0;
	std::unique_ptr<Device> m_dev;
	std::unique_ptr<ApfsContainer> m_nx;
	std::vector<VolumeInfo> m_volumes;
	std::mutex m_open_mutex;  // GetVolume stores the passphrase in the container
};

class Volume
{
public:
	~Volume();

	const VolumeInfo &Info() const { return m_info; }
	Container &GetContainer() const { return m_container; }

	bool GetStat(uint64_t inode, Stat &st);

	// One path component, matched the way APFS does on this volume
	// (case/normalization-insensitive where the volume says so).
	bool Lookup(uint64_t parent, const std::string &name, DirEntry &out);

	// "/a/b/c" (or "" / "/" for the root). Does not follow symlinks.
	bool ResolvePath(const std::string &path, uint64_t &inode);

	bool ListDir(uint64_t dir_inode, std::vector<DirEntry> &entries);

	std::unique_ptr<FileReader> OpenFile(const Stat &st, std::string &err);

	// Extended attributes as macOS userland sees them: the decmpfs xattr,
	// a compressed file's resource fork and com.apple.fs.* are hidden.
	bool ListXattrs(uint64_t inode, const Stat &st, std::vector<std::string> &names);
	bool GetXattr(uint64_t inode, const std::string &name, std::vector<uint8_t> &data);
	bool GetXattrSize(uint64_t inode, const std::string &name, uint64_t &size);
	// Ranged read of an xattr value; stream-backed xattrs are read in place.
	bool ReadXattr(uint64_t inode, const std::string &name, uint64_t off, void *buf, size_t len, size_t &got);
	bool ReadLink(uint64_t inode, std::string &target);

	// Raw data-stream read (no decompression), used by FileReader.
	bool ReadStream(uint64_t private_id, uint64_t off, void *buf, size_t len);

	ApfsVolume &Raw() { return *m_vol; }

private:
	friend class Container;
	Volume(Container &c, std::unique_ptr<ApfsVolume> v, const VolumeInfo &info);

	Container &m_container;
	std::unique_ptr<ApfsVolume> m_vol;
	VolumeInfo m_info;
};

struct PhysicalDrive
{
	std::string path;           // \\.\PhysicalDriveN
	uint64_t size = 0;
	bool access_denied = false; // raw disk access needs an elevated process
	bool has_gpt = false;
	std::vector<PartitionInfo> apfs;
};

// Probes \\.\PhysicalDrive0..63 and lists their APFS partitions.
std::vector<PhysicalDrive> EnumeratePhysicalDrives();

// Encoding helpers (Windows)
std::wstring Utf8ToWide(const std::string &s);
std::string WideToUtf8(const std::wstring &s);

} // namespace winapfs
