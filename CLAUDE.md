# winapfs — notes for working on this repo

Read-only APFS driver for Windows. GPL-2.0-or-later (vendored apfs-fuse is GPL-2+).
C++17, MSVC (VS 2022 Build Tools), CMake ≥ 3.20, WinFsp 2.1 native API.

## Build and test

```
build.cmd [Release|Debug]          # vcvars64 + cmake -G "Visual Studio 17 2022" -A x64 + build
"C:\Program Files\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```

`cmake`/`ctest` are not on PATH in this environment; use the full path. Run
`build.cmd` with an absolute path from PowerShell (the shell's cwd drifts).

Tests (all in `tests/CMakeLists.txt`):
- `image.*` — `winapfs-verify --image`: walks each volume through the core
  library and compares **every** manifest field (`--fields all`).
- `mount.*` — `winapfs mount <img> * --run "winapfs-verify --dir %MOUNT% ..."`:
  mounts through WinFsp at a free drive letter and walks it with Win32
  (FindFirstFile / CreateFile / ReadFile), comparing against the manifest.
  `MOUNT_FIELDS` lists what is checked through the mount; grow it as features land.
- `cli.*` — smoke tests for `winapfs-cli`.

Mount tests share `RESOURCE_LOCK winfsp_mount`. Mounting a drive letter does
not need admin. Windows Defender scans each file opened on the mount
(~3 ms/file), so `mount.basic*` (3000+ files) takes ~12 s. That's environmental.

## Test data

`testdata/` = images + manifests from the user's macOS generator
(`testdata/generator/`, see its README). `*.img` are gitignored (2.7 GB);
manifests and `SHA256SUMS` are tracked. Encrypted volume passphrase:
`winapfs-test`. Manifest quirks handled in the verifier:
- skips `.fseventsd`, `.Spotlight-V100`, `.Trashes`, `.TemporaryItems`
- stored uid/gid 99 (macOS `_unknown`, shown as the viewing user) matches anything
- `nlink` is not compared for directories (APFS stores child count instead)

## Layout

```
third_party/apfs-fuse/   ApfsLib + Crypto + lzfse, vendored from sgan81/apfs-fuse
                         @ 66b86bd (2023-03-12), trimmed; patches listed below
third_party/zlib/        zlib 1.3.1 (gz* files dropped)
third_party/bzip2/       bzip2 1.0.8 (DMG images only)
third_party/nlohmann/    json.hpp 3.11.3 (tests only)
src/core/winapfs/        winapfs_core: thread-safe layer over ApfsLib
  Apfs.h/.cpp            Container (image/GPT/raw disk), Volume (stat, lookup,
                         readdir, xattrs, readlink), FileReader
  Compression.*          chunked decmpfs reader (64 KiB blocks on demand, LRU 8)
  Gpt.*                  own GPT parser: all APFS partitions, 512/4096 sectors, CRCs
  Names.*                APFS UTF-8 <-> Windows UTF-16 name mapping
src/cli/                 winapfs-cli: info | list | cat | xattr
src/mount/               winapfs: WinFsp front-end (ApfsFs) + `mount` command
tests/verify.cpp         winapfs-verify: manifest checker (image or mounted dir)
```

## Architecture

- **Device** (ApfsLib): `DeviceWinFile` (image files) and `DeviceWinPhys`
  (raw disks) were rewritten: UTF-8 paths, positional `ReadFile` with an
  OVERLAPPED offset (thread-safe), raw disk reads go through an aligned
  bounce buffer (sector alignment), size via IOCTL_DISK_GET_LENGTH_INFO.
- **Thread safety**: WinFsp calls in from several threads. ApfsLib's B-tree
  node cache is mutex-guarded, AES-XTS only reads its key schedule, and
  `ApfsDir` holds a scratch buffer, so the core makes a fresh `ApfsDir` per
  call. `Container::OpenVolume` is serialized (GetVolume stores the
  passphrase in the container).
- **Names**: Windows-illegal characters (0x01–0x1F `" * : < > ? \ |`) map to
  U+F000+c, and a trailing space or dot maps to U+F020/U+F02E (the Cygwin/WSL
  convention, so it round-trips). Invalid UTF-8 bytes map to U+DC80+b. Unicode
  normalization is not changed: Windows sees the stored bytes, and lookups use
  APFS's own hashing (case/normalization-insensitive per volume flags).
- **Case**: `CaseSensitiveSearch` follows the volume. On case-insensitive
  volumes, Open returns the stored name as the NormalizedName.
- **Mount**: `ApfsFs` caches directory path → inode. Metadata is cached forever
  (`FileInfoTimeout = ~0`) since nothing changes. Fixed SD
  `O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;WD)`; `ReadOnlyVolume = 1`.
  WinFsp requires `Create`/`Overwrite` callbacks even when read-only; they
  return STATUS_MEDIA_WRITE_PROTECTED. winfsp-x64.dll is delay-loaded via
  `FspLoad`.
- **Timestamps**: APFS ns since 1970 (pre-1970 stored as negative int64) →
  FILETIME with floor division.

## Patches to vendored ApfsLib (all marked `// winapfs:`)

- DeviceWinFile/DeviceWinPhys: rewritten (see above).
- Util.h: `__attribute__((format))` → `APFS_PRINTF_ATTR` (MSVC).
- Util.cpp: Windows no-echo `GetPassword`.
- ApfsVolume.cpp: `Init`/`MountSnapshot` never prompt for a password on
  stdin; they fail and the caller asks.
- ApfsDir.cpp: `ReadFile` treats offsets with no covering extent as sparse
  holes (upstream returned false for a leading hole and underflowed `cur_size`
  past the last extent). `LookupName` returns the stored name and the drec
  flags (type). Defined the missing `XAttr` constructors.
- CMake: SYSTEM includes; WIN32_LEAN_AND_MEAN is not exported (WinFsp needs
  bcrypt.h's PNTSTATUS).

## Direction change: read-write (decided 2026-11-07)

The user changed the goal from read-only to proper read-write. Decisions:
- **Engine**: port linux-apfs-rw (`fs/apfs`, GPL-2) to user mode on MSVC
  over a small Linux-kernel-API shim. It becomes the one engine for both
  reading and writing. ApfsLib stays only as an independent reader in the
  tests (an oracle that cross-checks what the new engine wrote).
- **Checkers**: port apfsprogs' `apfsck` as `winapfs-fsck`. Every write test
  ends with a clean fsck plus an ApfsLib re-read. The user runs macOS
  `fsck_apfs` on images written by winapfs; that is the gate before physical
  disk writes are enabled by default.
- **Safety**: writes only with an explicit `--rw`. Refuse to write when the
  container is unclean, the Mac is hibernated, or the volume is sealed,
  encrypted (until W6) or a Fusion drive. Raw disks opened for write only when
  `--rw` is given.

Read-write milestones (after M1–M3):
- W1 engine port, with all current read tests passing on it
- W2 winapfs-fsck
- W3 basic writes on images (create/write/truncate/delete/rename/mkdir, times,
  attributes) and a crash test (kill mid-write → previous checkpoint is clean)
- W4 writing the fidelity features (xattrs/ADS, symlinks, hard links, names)
- W5 opt-in physical-disk writes with the refusals above
- W6 encrypted RW, snapshots, installer, auto-mount service

### W1 status: engine reads, not yet behind the mount

linux-apfs-rw @ 949677e (2026-11-06) is vendored in `third_party/linux-apfs-rw`
(its bundled lzfse removed: the engine uses Apple's lzfse from apfs-fuse, which
is MSVC-clean). It builds with MSVC as C23 (`/std:clatest`, for `__typeof__`)
on the kernel shim in `src/kshim/`:

```
src/kshim/include/kshim.h     types, byte order, bitops, atomics, locks (SRWLOCK),
                              lists, printk (%pV/%pg), memory, time, crc32c
src/kshim/include/kshim_fs.h  VFS/bdev/buffer-head/page-cache structs, 6.12 op tables
src/kshim/include/linux/*.h   one-line stubs that include kshim.h
kshim_core.c      primitives, zlib_inflate_blob (raw deflate!), mount-option parser
kshim_work.c      one worker thread for (delayed) work: transaction commits, orphans
kshim_bdev.c      device registry "kshim:N" -> Win32 handle + partition range,
                  positional overlapped I/O, FlushFileBuffers as the barrier
kshim_pagecache.c folios (one 4 KiB page, page-aligned), per-mapping hash, buffer
                  heads on folios, synchronous submit_bh, mpage read, block_write_*
                  writeback, truncation, reclaim of clean folios above 64K folios
kshim_vfs.c       inode cache (iget5_locked & co), transient dentries, sget/super
                  lifecycle, sync_filesystem (write_inode, sync_fs 0, flush, sync_fs 1)
apfsrw_api.c      C API (src/kshim/api/apfsrw.h): mount/unmount/sync/statfs, lookup,
                  getattr (+ BSD flags from APFS_I), readdir, open/read, readlink, xattrs
```

We present Linux 6.12 (`LINUX_VERSION_CODE`), so the driver uses its old mount
API (`apfs_mount` + `sget`), folio aops and `mnt_idmap` signatures. Crash-safety
contract: `filemap_write_and_wait(bd_mapping)` writes every dirty block-device
buffer and then flushes, and the checkpoint superblock is written between two of
those barriers (`apfs_checkpoint_end`).

Tests: `rwengine.*` runs `winapfs-verify --engine rw --fields all` on every
volume; all pass. Secret is refused (the engine has no encryption, WILL_FAIL).
`apfsrw-smoke` mounts and lists a volume.

Patches to vendored linux-apfs-rw (all marked `winapfs:`), needed only for MSVC:
- apfs.h: `#pragma pack(1)` around `#include "apfs_raw.h"` (no `__packed` suffix).
- apfs_raw.h: `kl_entries` is `u8[]` on MSVC (array of structs ending in a
  flexible array; unused by the driver).
- unicode.c: explicit sizes for the forward-declared static tables (4016, 3404,
  2944, 1521, 3520; verified exact: N-1 fails with "too many initializers").
- ~60 lines of `void *` arithmetic rewritten as `(void *)((char *)p + off)` in
  btree.c, extents.c, inode.c, node.c, spaceman.c, super.c, xattr.c, xfield.c.
  Regenerate the list with `git diff` against upstream before syncing.

Next for W1: an engine interface in the WinFsp front-end (rw engine for
unencrypted volumes, ApfsLib for encrypted), and the `mount.*` tests on it.
Then W2 (apfsck port) and W3 writes.

## Milestones (read path)

1. ✅ Repo skeleton, ApfsLib on MSVC, `winapfs-cli list|cat|info|xattr`.
   All 10 volumes pass `--fields all` through the library.
2. ✅ `winapfs mount <image> X:`: open, read, readdir, file info, volume info,
   fixed SD. Win32 checksum walk passes on every volume except links.img
   (symlinks/FIFO, milestone 4).
3. ✅ Physical disks: `winapfs list` / `mount-all` scan \\.\PhysicalDrive0..63
   for GPT APFS partitions. Passed end to end (elevated) with the test images
   attached as read-only VHDs (`tests/physical_test.ps1`, needs admin; it
   builds fixed VHDs by appending a footer). DeviceWinPhys now has a 64 MiB
   chunk cache and an overlapped handle; the first VHD run, before that, took
   ~11 min for Basic. The cached version passes the non-admin `rawdev.*` /
   `mount_rawdev.*` tests (\\.\C:\...\file.img routes through DeviceWinPhys)
   but has not been re-timed on a real disk.
4. Fidelity: compression (done at library level), xattrs as ADS, symlinks
   as reparse points, hard links, timestamps, case, normalization.
5. FileVault: password prompt (CLI done), recovery key.
6. Snapshots, installer, auto-mount service.

## Known gaps

- Symlinks show up as 0-byte files and FIFOs as files on the mount (milestone 4).
- Reserved DOS names (`nul`, `CON.txt`) are passed through unchanged; Win32
  path parsing may turn them into devices unless accessed via `\\?\`.
- Hard links appear as independent files (same IndexNumber); WinFsp has no
  hard-link count (`HardLinks` is unimplemented).
- Fusion drives (two devices) are not exposed by the front-ends.
- kshim: unreferenced inodes stay cached until unmount (no inode reclaim yet);
  ioctl `unsigned long arg` is 32-bit on Windows, so pointer ioctls must not go
  through the driver's ioctl handlers; `pgoff_t` is 32-bit (16 TiB file limit).
- Build helpers: `build_target.cmd <target>` builds one target;
  `summarize_errors.sh <target>` builds and groups MSVC errors by kind.
- Vendored upstream files have CRLF line endings (Git for Windows autocrlf):
  use the Edit tool, not perl/sed with `\n`, on files under third_party/.
- No sealed-volume (signed system volume) testing; ApfsLib reads the fext tree.
