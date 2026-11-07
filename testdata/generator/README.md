# winapfs test data

Raw APFS images for developing and testing **winapfs**, a read-only APFS driver for Windows. They were made on macOS with Apple's own APFS driver, and every image was re-mounted read-only and checked against its manifest (`./verify.sh`).

Copy `images/` to `testdata\` in the winapfs repo.

## Images

All `.img` files are raw byte-for-byte disk images (no DMG wrapper). They're GPT-partitioned, with the APFS container in the partition of type `7C3457EF-0000-11AA-AA11-00306543ECAC`, at byte offset 20480.

| Image | Container | Volumes | Exercises |
|---|---|---|---|
| `basic.container.img` | bare, starts at byte 0 | Basic | **Start here (milestone 1).** Same contents as `basic.img` without the partition table. |
| `basic.img` | GPT | Basic (case-insensitive) | 3068 entries: empty/odd-sized/20 MiB files, a sparse file (32 MiB hole + 4 KiB), 24-level nesting, a 3000-file directory (multi-level catalog B-tree), NFC vs NFD names, CJK/Greek/emoji names, **names that are illegal on Windows** (`colon:name.txt`, `<>\|"*?`, backslash, trailing dot/space, `CON.txt`, `nul`), a 255-byte name, modes 0600/0755/0444, timestamps before 1970 / in 2001 / after 2038 / with nanoseconds, a dotfile and a `UF_HIDDEN` file, plus `history/` (overwritten, deleted, renamed, appended across separate syncs, so older checkpoints/object versions exist on disk) |
| `compressed.img` | GPT | Compressed | decmpfs: **LZVN in resource fork** (type 8, from `ditto --hfsCompression`) and **zlib inline in the xattr** (type 3, including the `0xFF` stored-raw variant). Also an incompressible file left plain, a small file ditto skipped, and an uncompressed control file with the same bytes as `text_64KiB.txt`. |
| `links.img` | GPT | Links | Symlinks: relative, absolute, dangling, to a dir, `../`, a loop, a 1000-char target. 3 hard links across directories (same inode, nlink 3). Xattrs: small, binary, empty, 64 KiB (stored as a data stream), on a directory. Resource fork, FinderInfo, `UF_IMMUTABLE`, `UF_NODUMP`, a FIFO. |
| `casesensitive.img` | GPT | CaseSens (case-sensitive) | `readme.txt` / `README.txt` / `ReadMe.txt`, `Dir/` vs `dir/`, `École` vs `école`, `straße` vs `STRASSE` all coexist. On Windows these collide, so the driver needs a strategy. |
| `multivol.img` | GPT, 1.6 GB | VolA, VolB (case-sensitive), VolC (16 MB quota) | Several volumes in one container (APFS allows one volume per 512 MiB, hence the size; it's mostly zeros) |
| `encrypted.img` | GPT, 600 MB | Public, Secret (**encrypted**) | Software encryption (AES-XTS, keybag). Passphrase: **`winapfs-test`**, hint `winapfs test password`. Secret's plaintext doesn't appear anywhere in the raw image. |

Not covered: snapshots (creating them needs root/entitlements), LZFSE and zlib-in-resource-fork compression, Fusion drives, sealed system volumes, T2/Apple-silicon hardware encryption.

## Manifests

There's one `<image>.<Volume>.manifest.json` per volume, read from a read-only mount of the final `.img`. Each entry is one path:

- `path`: POSIX path from the volume root. `path_utf8_hex` has the exact on-disk bytes, which matters for NFD names.
- `type`: `file`, `dir`, `symlink` or `fifo`.
- `size`, `allocated_bytes`, `sha256` for files. `target` for symlinks.
- `inode`, `nlink`, `mode`, `uid`, `gid`, `flags` (BSD `st_flags`; `compressed` = `UF_COMPRESSED`).
- `mtime_ns`, `ctime_ns`, `atime_ns`, `birthtime`.
- `xattrs`: name → `{size, sha256}`, as the macOS VFS lists them. The kernel hides `com.apple.decmpfs` and the compressed files' resource forks, and shows a resource fork as `com.apple.ResourceFork`.

The manifests skip `.fseventsd`, `.Spotlight-V100`, `.Trashes` and `.TemporaryItems`, which macOS creates at mount time. The driver will still see them.

Verify a mount by walking the drive, hashing each file, and comparing paths, types, sizes and sha256 against the manifest. Then compare the other fields as each feature lands. `atime_ns` changes on read, so ignore it.

`SHA256SUMS` covers the `.img` files, so you can check the transfer (`certutil -hashfile <file> SHA256` on Windows).

## Regenerating (macOS, no sudo)

```bash
./make_images.sh   # builds images/, records manifests from the finished .img files
./verify.sh        # re-mounts each .img read-only and compares against the manifests
```

`tool.py` does the populating (per-profile file trees), manifest writing, GPT→bare container extraction, and manifest comparison.
