# Third-party notices

winapfs includes or uses the following software. The full licence texts are
in the paths listed; the installer also ships them all in
`THIRD-PARTY-NOTICES.txt`.

The combined program is distributed under the GNU General Public License,
version 2 only ([LICENSE](LICENSE)), the licence of linux-apfs-rw. Every
component below is compatible with that.

## Included in the source and the binaries

### linux-apfs-rw
- Upstream: https://github.com/linux-apfs/linux-apfs-rw, commit `949677e`
- Copyright (C) Ernesto A. Fernández and contributors
- Licence: GPL-2.0-only, [third_party/linux-apfs-rw/LICENSE](third_party/linux-apfs-rw/LICENSE)
- Local changes, all marked `winapfs:` in the source: MSVC portability
  (`void *` arithmetic, packed structs, table sizes); a stored-name lookup
  helper; fixes marked `winapfs (upstream candidate)`: inode flags that
  mirror extended attributes (`xattr.c`), and per-volume writable copies when
  several volumes join one container transaction (`transaction.c`). Its
  bundled lzfse was removed in favour of Apple's below.

### apfsprogs (apfsck)
- Upstream: https://github.com/linux-apfs/apfsprogs, commit `1bd1c1c`
- Copyright (C) Ernesto A. Fernández and contributors
- Licence: GPL-2.0, [third_party/apfsprogs/LICENSE](third_party/apfsprogs/LICENSE)
- Used as `winapfs-fsck`. Local changes (marked `winapfs`): MSVC
  portability, a POSIX compatibility layer for Win32, and fixes marked
  `winapfs (upstream candidate)`: padded keybag entries, LZVN/LZFSE
  support, incompressible resource forks, DER keybag entries.

### apfs-fuse (ApfsLib, Crypto)
- Upstream: https://github.com/sgan81/apfs-fuse, commit `66b86bd` (2023-03-12), trimmed
- Copyright (C) 2017 Simon Gander
- Licence: GPL-2.0-or-later, [third_party/apfs-fuse/LICENSE](third_party/apfs-fuse/LICENSE)
- Local changes, marked `winapfs:`: rewritten Windows device layer, no
  password prompts on stdin, block-zero transaction id, sparse-hole reads,
  case-insensitive name comparison fix, stored names in lookups.

### LZFSE
- Upstream: https://github.com/lzfse/lzfse (bundled with apfs-fuse)
- Copyright (c) 2015-2016, Apple Inc. All rights reserved.
- Licence: BSD-3-Clause, [third_party/apfs-fuse/3rdparty/lzfse/LICENSE](third_party/apfs-fuse/3rdparty/lzfse/LICENSE)

### zlib 1.3.1
- Upstream: https://zlib.net
- Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
- Licence: zlib, [third_party/zlib/LICENSE](third_party/zlib/LICENSE). The
  `gz*` files were left out.

### bzip2 1.0.8
- Upstream: https://sourceware.org/bzip2/
- Copyright (C) 1996-2019 Julian R Seward
- Licence: bzip2 (BSD-style), [third_party/bzip2/LICENSE](third_party/bzip2/LICENSE)

## Tests only (not in the binaries)

### nlohmann/json 3.11.3
- Upstream: https://github.com/nlohmann/json
- Copyright (c) 2013-2022 Niels Lohmann
- Licence: MIT, [third_party/nlohmann/LICENSE.MIT](third_party/nlohmann/LICENSE.MIT)

## Used, not included

### WinFsp
- https://github.com/winfsp/winfsp. Installed separately; winapfs loads
  `winfsp-x64.dll` from the WinFsp installation.
- Licence: GPL-3.0 with a FLOSS exception that lets free software under an
  OSI/FSF licence (GPL-2.0 qualifies) use it, provided the notice below is
  shown in the program's user interface and documentation (it is: `winapfs
  --help`, the tray's About box, the installer's licence page and this
  repository's README) and nothing proprietary is linked or bundled:

  > **WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos**
  > https://github.com/winfsp/winfsp

### WiX Toolset 5
- https://wixtoolset.org, MS-RL. Build tool for the installer; not part of
  winapfs's source.

## winapfs's own artwork

The logo, [assets/logo.svg](assets/logo.svg), was made for winapfs and is
dedicated to the public domain under CC0 1.0.

## Trademarks

APFS, Apple, Mac, macOS and FileVault are trademarks of Apple Inc. Windows is
a trademark of Microsoft Corporation. winapfs is not affiliated with or
endorsed by either.
