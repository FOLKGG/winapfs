# winapfs — APFS (Mac) disks on Windows

winapfs reads and (experimentally) writes Apple's APFS file system on Windows.
APFS volumes appear as ordinary drive letters in Explorer: files, folders,
compressed files, extended attributes (as alternate data streams), symbolic
links, timestamps and Mac file names all work.

It builds on two GPL projects:
- [linux-apfs-rw](https://github.com/linux-apfs/linux-apfs-rw), the Linux APFS
  driver, running in user mode on Windows (the engine that reads and writes);
- [apfs-fuse](https://github.com/sgan81/apfs-fuse) (ApfsLib), used for
  encrypted volumes and as an independent checker in the tests;

plus [apfsprogs](https://github.com/linux-apfs/apfsprogs)' `apfsck` as
`winapfs-fsck`.

## Requirements

- Windows 10 or 11 (x64)
- [WinFsp](https://winfsp.dev) 2.x

## Use

From an administrator terminal (raw disk access needs it):

```
winapfs list                         # APFS disks, partitions and volumes
winapfs mount-all                    # mount every volume read-only (Ctrl+C unmounts)
winapfs mount \\.\PhysicalDrive2 X: --volume "Macintosh HD - Data"
winapfs mount-all --ask              # ask for passwords of encrypted volumes
```

Disk images (`.img`, `.dmg` raw images) work without administrator rights:
`winapfs mount disk.img *`.

### Automatic mounting

```
winapfs service install              # as administrator
```

The service mounts every APFS volume read-only when a disk is connected, at a
drive letter it remembers per volume, and unmounts cleanly on "Safely Remove
Hardware", unplugging and shutdown. Settings live in `HKLM\SOFTWARE\winapfs`
(see `winapfs service`). `winapfs service uninstall` removes it.

### Writing (experimental)

`--rw` mounts read-write. It is refused for disks that were not ejected
cleanly from a Mac (crashed, hibernated, unplugged), and for macOS system,
Time Machine, sealed, encrypted and Fusion volumes. Writing to physical disks
additionally needs `--allow-disk-writes` while write support is being
validated. **Keep a backup.**

## Building

Visual Studio 2022 Build Tools (C++), CMake ≥ 3.20 and WinFsp (with its
developer files):

```
build.cmd Release
ctest --test-dir build -C Release --output-on-failure
```

The tests need the test images (see `testdata/generator`).

## License

winapfs as a whole is distributed under the GNU General Public License,
version 2 (GPL-2.0-only), because linux-apfs-rw is GPL-2.0-only. Bundled
components keep their own permissive licences (zlib, bzip2, lzfse,
nlohmann/json).

winapfs uses WinFsp under its FLOSS exception:

> **WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos**
> https://github.com/winfsp/winfsp
