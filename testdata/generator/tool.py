#!/usr/bin/env python3
"""winapfs test-data helper (macOS only).

  tool.py populate <mountpoint> <profile>   fill a mounted APFS volume
  tool.py manifest <mountpoint> <out.json>  record ground truth as macOS sees it
  tool.py extract  <gpt.img> <container.img> copy the APFS partition out of a GPT image
"""
import ctypes
import ctypes.util
import hashlib
import json
import os
import random
import stat
import struct
import subprocess
import sys
import tempfile
import unicodedata

# ---------------------------------------------------------------- xattrs (libc)
libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
XATTR_NOFOLLOW = 0x0001
libc.listxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
libc.listxattr.restype = ctypes.c_ssize_t
libc.getxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p,
                          ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
libc.getxattr.restype = ctypes.c_ssize_t
libc.setxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p,
                          ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int]
libc.setxattr.restype = ctypes.c_int


def _check(rc, path):
    if rc < 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e), path)
    return rc


def listxattr(path):
    p = os.fsencode(path)
    n = _check(libc.listxattr(p, None, 0, XATTR_NOFOLLOW), path)
    if n == 0:
        return []
    buf = ctypes.create_string_buffer(n)
    n = _check(libc.listxattr(p, buf, n, XATTR_NOFOLLOW), path)
    return [x.decode() for x in buf.raw[:n].split(b"\0") if x]


def getxattr(path, name):
    p, k = os.fsencode(path), name.encode()
    n = _check(libc.getxattr(p, k, None, 0, 0, XATTR_NOFOLLOW), path)
    buf = ctypes.create_string_buffer(max(n, 1))
    n = _check(libc.getxattr(p, k, buf, n, 0, XATTR_NOFOLLOW), path)
    return buf.raw[:n]


def setxattr(path, name, value):
    _check(libc.setxattr(os.fsencode(path), name.encode(), value, len(value), 0, XATTR_NOFOLLOW), path)


# ---------------------------------------------------------------- populate
T_2001 = 985_392_000          # 2001-03-24 00:00:00 UTC
T_2038 = 2_147_483_648        # one second past 32-bit time_t
T_1969 = -86_400              # pre-epoch


def rnd(seed, n):
    return random.Random(seed).randbytes(n)


def write(path, data, mode=None, mtime=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    if mode is not None:
        os.chmod(path, mode)
    if mtime is not None:
        os.utime(path, (mtime, mtime))


def text_blob(lines, seed):
    r = random.Random(seed)
    words = "apfs volume container btree extent inode snapshot checkpoint omap keybag".split()
    return "".join(" ".join(r.choice(words) for _ in range(12)) + "\n" for _ in range(lines)).encode()


def populate_basic(m):
    write(f"{m}/empty.txt", b"")
    write(f"{m}/hello.txt", b"Hello, APFS!\n")
    for n in (1, 4095, 4096, 4097, 65536, 1_048_577):
        write(f"{m}/sizes/size_{n}.bin", rnd(n, n))
    write(f"{m}/sizes/random_20MiB.bin", rnd(20, 20 * 1024 * 1024))

    # sparse: 32 MiB hole, then 4 KiB of data
    with open(f"{m}/sparse_32MiB.bin", "wb") as f:
        f.seek(32 * 1024 * 1024)
        f.write(rnd(99, 4096))

    deep = m
    for i in range(24):
        deep = f"{deep}/level{i:02d}"
    write(f"{deep}/bottom.txt", b"deepest file\n")

    # large directory -> multi-level catalog B-tree
    for i in range(3000):
        write(f"{m}/bigdir/file_{i:04d}.txt", f"file {i}\n".encode())

    names = {
        "nfc_café.txt": b"NFC precomposed e-acute\n",
        "nfd_café.txt": b"NFD e + combining acute\n",
        "日本語.txt": b"Japanese\n",
        "Ελληνικά.txt": b"Greek\n",
        "emoji_\U0001F642.txt": b"astral-plane char (UTF-16 surrogate pair)\n",
        "with spaces.txt": b"spaces\n",
        # legal on APFS, illegal or special on Windows
        "colon:name.txt": b"colon\n",
        'win_illegal_<>|"*?.txt': b"chars Windows forbids\n",
        "back\\slash.txt": b"backslash\n",
        "trailing dot.": b"trailing dot\n",
        "trailing space ": b"trailing space\n",
        "CON.txt": b"reserved DOS device name\n",
        "nul": b"reserved DOS device name\n",
        "x" * 255: b"255-byte name\n",
    }
    for name, data in names.items():
        write(f"{m}/names/{name}", data)

    write(f"{m}/perms/private_0600.txt", b"private\n", mode=0o600)
    write(f"{m}/perms/script_0755.sh", b"#!/bin/sh\necho hi\n", mode=0o755)
    write(f"{m}/perms/readonly_0444.txt", b"readonly\n", mode=0o444)

    write(f"{m}/times/2001.txt", b"2001\n", mtime=T_2001)
    write(f"{m}/times/after_2038.txt", b"2038\n", mtime=T_2038)
    write(f"{m}/times/pre_epoch.txt", b"1969\n", mtime=T_1969)
    write(f"{m}/times/nanoseconds.txt", b"ns\n")
    os.utime(f"{m}/times/nanoseconds.txt", ns=(1_600_000_000_123_456_789,) * 2)

    write(f"{m}/.hidden_dotfile", b"dotfile\n")
    write(f"{m}/hidden_flag.txt", b"UF_HIDDEN\n")
    os.chflags(f"{m}/hidden_flag.txt", stat.UF_HIDDEN)

    # history: forces newer checkpoints / omap versions over older data
    write(f"{m}/history/overwritten.txt", b"version 1\n" * 1000)
    os.sync()
    write(f"{m}/history/overwritten.txt", b"version 2\n")
    write(f"{m}/history/deleted.txt", b"should not be visible\n")
    os.sync()
    os.remove(f"{m}/history/deleted.txt")
    write(f"{m}/history/old_name.txt", b"renamed\n")
    os.rename(f"{m}/history/old_name.txt", f"{m}/history/new_name.txt")
    with open(f"{m}/history/appended.txt", "ab") as f:
        f.write(b"first\n")
    os.sync()
    with open(f"{m}/history/appended.txt", "ab") as f:
        f.write(b"second\n")


def populate_compressed(m):
    src = tempfile.mkdtemp(prefix="winapfs-src-")
    write(f"{src}/tiny.txt", b"tiny compressible " * 8)                 # too small: ditto leaves it plain
    write(f"{src}/text_64KiB.txt", text_blob(900, 1))
    write(f"{src}/text_2MiB.txt", text_blob(28_000, 2))                  # compressed data in resource fork
    write(f"{src}/zeros_8MiB.bin", b"\0" * (8 * 1024 * 1024))
    write(f"{src}/random_1MiB.bin", rnd(3, 1024 * 1024))                 # incompressible: stays plain
    write(f"{src}/sub/nested.txt", text_blob(5000, 4))
    os.makedirs(f"{m}/compressed", exist_ok=True)
    subprocess.run(["ditto", "--hfsCompression", src, f"{m}/compressed"], check=True)
    subprocess.run(["rm", "-rf", src], check=True)
    write(f"{m}/uncompressed_control.txt", text_blob(900, 1))           # same bytes as text_64KiB, not compressed
    # ditto won't compress small files, so build the inline form by hand
    os.makedirs(f"{m}/inline", exist_ok=True)
    decmpfs_zlib_inline(f"{m}/inline/zlib_16KiB_As.txt", b"A" * 16384)
    decmpfs_zlib_inline(f"{m}/inline/zlib_text_8KiB.txt", text_blob(110, 5)[:8192])
    decmpfs_zlib_inline(f"{m}/inline/stored_raw_100B.txt", rnd(6, 100), raw=True)


def decmpfs_zlib_inline(path, data, raw=False):
    """decmpfs type 3: zlib data inline in com.apple.decmpfs. raw=True uses the
    0xFF-prefixed 'stored uncompressed' variant. Data fork is left empty."""
    import zlib
    payload = (b"\xff" + data) if raw else zlib.compress(data, 9)
    assert len(payload) < 3800, "too big for inline"
    with open(path, "wb"):
        pass
    setxattr(path, "com.apple.decmpfs", struct.pack("<4sIQ", b"fpmc", 3, len(data)) + payload)
    os.chflags(path, stat.UF_COMPRESSED)
    with open(path, "rb") as f:
        assert f.read() == data, f"{path}: kernel decompression mismatch"


def populate_links(m):
    write(f"{m}/target.txt", b"symlink target\n")
    os.makedirs(f"{m}/dir", exist_ok=True)
    write(f"{m}/dir/inside.txt", b"inside dir\n")
    os.symlink("target.txt", f"{m}/link_relative")
    os.symlink("/Volumes/Links/target.txt", f"{m}/link_absolute")
    os.symlink("does/not/exist", f"{m}/link_dangling")
    os.symlink("dir", f"{m}/link_to_dir")
    os.symlink("../target.txt", f"{m}/dir/link_parent")
    os.symlink("link_loop_b", f"{m}/link_loop_a")
    os.symlink("link_loop_a", f"{m}/link_loop_b")
    os.symlink("t" * 1000, f"{m}/link_long_target")

    write(f"{m}/hard/original.txt", b"hard linked\n")
    os.makedirs(f"{m}/hard/other_dir", exist_ok=True)
    os.link(f"{m}/hard/original.txt", f"{m}/hard/second_name.txt")
    os.link(f"{m}/hard/original.txt", f"{m}/hard/other_dir/third_name.txt")

    write(f"{m}/xattr/small.txt", b"has xattrs\n")
    setxattr(f"{m}/xattr/small.txt", "user.comment", b"short inline value")
    setxattr(f"{m}/xattr/small.txt", "user.binary", bytes(range(256)))
    setxattr(f"{m}/xattr/small.txt", "user.empty", b"")
    write(f"{m}/xattr/large.txt", b"large xattr\n")
    setxattr(f"{m}/xattr/large.txt", "user.big", rnd(7, 64 * 1024))   # > 3804 B: stored as data stream
    write(f"{m}/xattr/rsrc.txt", b"has a resource fork\n")
    with open(f"{m}/xattr/rsrc.txt/..namedfork/rsrc", "wb") as f:
        f.write(rnd(8, 10_000))
    write(f"{m}/xattr/finderinfo.txt", b"finder info\n")
    setxattr(f"{m}/xattr/finderinfo.txt", "com.apple.FinderInfo", b"TEXTttxt" + b"\0" * 24)
    os.makedirs(f"{m}/xattr/dir_with_xattr", exist_ok=True)
    setxattr(f"{m}/xattr/dir_with_xattr", "user.on_dir", b"directory xattr")

    write(f"{m}/flags/immutable.txt", b"uchg\n")
    os.chflags(f"{m}/flags/immutable.txt", stat.UF_IMMUTABLE)
    write(f"{m}/flags/nodump.txt", b"nodump\n")
    os.chflags(f"{m}/flags/nodump.txt", stat.UF_NODUMP)

    os.mkfifo(f"{m}/special_fifo")


def populate_casesensitive(m):
    write(f"{m}/readme.txt", b"lower\n")
    write(f"{m}/README.txt", b"UPPER\n")
    write(f"{m}/ReadMe.txt", b"Mixed\n")
    write(f"{m}/Dir/a.txt", b"Dir/a\n")
    write(f"{m}/dir/a.txt", b"dir/a\n")
    write(f"{m}/École.txt", b"capital E-acute\n")
    write(f"{m}/école.txt", b"small e-acute\n")
    write(f"{m}/straße.txt", b"sharp s\n")
    write(f"{m}/STRASSE.txt", b"SS\n")


def populate_small(m, label):
    write(f"{m}/volume.txt", f"this is volume {label}\n".encode())
    write(f"{m}/data/{label}.bin", rnd(sum(label.encode()), 100_000))


PROFILES = {
    "basic": populate_basic,
    "compressed": populate_compressed,
    "links": populate_links,
    "casesensitive": populate_casesensitive,
}


# ---------------------------------------------------------------- manifest
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def manifest(mount, out):
    case_sensitive = is_case_sensitive(mount)  # probe before walking so it isn't recorded
    vfs = os.statvfs(mount)
    entries = []
    for root, dirs, files in os.walk(mount):
        dirs.sort()
        rel_root = os.path.relpath(root, mount)
        if rel_root.split(os.sep)[0] in (".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems"):
            dirs[:] = []
            continue
        for name in [""] + sorted(dirs) + sorted(files):
            if name == "" and rel_root != ".":
                continue  # each dir is recorded once, from its parent
            p = os.path.join(root, name) if name else root
            rel = os.path.relpath(p, mount)
            if rel.split(os.sep)[0] in (".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems"):
                continue
            st = os.lstat(p)
            e = {
                "path": "/" if rel == "." else "/" + rel,
                "path_utf8_hex": rel.encode().hex(),
                "type": ("dir" if stat.S_ISDIR(st.st_mode) else "symlink" if stat.S_ISLNK(st.st_mode)
                         else "fifo" if stat.S_ISFIFO(st.st_mode) else "file"),
                "inode": st.st_ino,
                "nlink": st.st_nlink,
                "mode": oct(stat.S_IMODE(st.st_mode)),
                "uid": st.st_uid,
                "gid": st.st_gid,
                "flags": st.st_flags,
                "compressed": bool(st.st_flags & stat.UF_COMPRESSED),
                "mtime_ns": st.st_mtime_ns,
                "ctime_ns": st.st_ctime_ns,
                "atime_ns": st.st_atime_ns,
                "birthtime": st.st_birthtime,
            }
            if e["type"] == "file":
                e["size"] = st.st_size
                e["allocated_bytes"] = st.st_blocks * 512
                e["sha256"] = sha256_file(p)
            elif e["type"] == "symlink":
                e["target"] = os.readlink(p)
            xs = {}
            for x in listxattr(p):
                v = getxattr(p, x)
                xs[x] = {"size": len(v), "sha256": hashlib.sha256(v).hexdigest()}
            if xs:
                e["xattrs"] = xs
            entries.append(e)
    doc = {
        "volume_mountpoint": mount,
        "block_size": vfs.f_frsize,
        "case_sensitive": case_sensitive,
        "entry_count": len(entries),
        "entries": entries,
    }
    with open(out, "w") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False)


def is_case_sensitive(mount):
    # read-only safe: look up an existing name with its case swapped
    for name in sorted(os.listdir(mount)):
        swapped = name.swapcase()
        if swapped != name:
            a, b = os.path.join(mount, name), os.path.join(mount, swapped)
            return not (os.path.lexists(b) and os.lstat(a).st_ino == os.lstat(b).st_ino)
    raise RuntimeError(f"no cased name in {mount} to probe")


# ---------------------------------------------------------------- extract
APFS_GUID = bytes.fromhex("ef57347c0000aa11aa1100306543ecac")  # 7C3457EF-0000-11AA-AA11-00306543ECAC, on-disk order


def extract(src, dst):
    with open(src, "rb") as f:
        f.seek(512)
        hdr = f.read(92)
        assert hdr[:8] == b"EFI PART", "no GPT header"
        ent_lba, n_ent, ent_size = struct.unpack_from("<QII", hdr, 72)
        f.seek(ent_lba * 512)
        for i in range(n_ent):
            ent = f.read(ent_size)
            if ent[:16] == APFS_GUID:
                first, last = struct.unpack_from("<QQ", ent, 32)
                break
        else:
            raise SystemExit("no APFS partition")
        f.seek(first * 512)
        remaining = (last - first + 1) * 512
        with open(dst, "wb") as o:
            while remaining:
                chunk = f.read(min(remaining, 1 << 20))
                o.write(chunk)
                remaining -= len(chunk)
    with open(dst, "rb") as o:
        o.seek(32)
        assert o.read(4) == b"NXSB", "container superblock magic missing"
    print(f"{dst}: APFS container at byte offset {first * 512} of {os.path.basename(src)}")


VOLATILE = {"atime_ns"}


def compare(expected, actual):
    """Exit non-zero if two manifests differ (ignoring atime and mountpoint)."""
    e, a = (json.load(open(x)) for x in (expected, actual))
    ee = {x["path"]: {k: v for k, v in x.items() if k not in VOLATILE} for x in e["entries"]}
    aa = {x["path"]: {k: v for k, v in x.items() if k not in VOLATILE} for x in a["entries"]}
    problems = [f"missing: {p}" for p in ee.keys() - aa.keys()]
    problems += [f"extra: {p}" for p in aa.keys() - ee.keys()]
    for p in ee.keys() & aa.keys():
        for k in ee[p].keys() | aa[p].keys():
            if ee[p].get(k) != aa[p].get(k):
                problems.append(f"{p}: {k} {ee[p].get(k)!r} != {aa[p].get(k)!r}")
    if e["case_sensitive"] != a["case_sensitive"]:
        problems.append("case_sensitive differs")
    print(f"{os.path.basename(expected)}: {len(ee)} entries, {len(problems)} differences")
    for x in problems[:20]:
        print("  ", x)
    if problems:
        raise SystemExit(1)


if __name__ == "__main__":
    cmd, *a = sys.argv[1:]
    if cmd == "populate":
        mount, profile = a
        if profile.startswith("small:"):
            populate_small(mount, profile.split(":", 1)[1])
        else:
            PROFILES[profile](mount)
    elif cmd == "manifest":
        manifest(*a)
    elif cmd == "extract":
        extract(*a)
    elif cmd == "compare":
        compare(*a)
    else:
        raise SystemExit(__doc__)
