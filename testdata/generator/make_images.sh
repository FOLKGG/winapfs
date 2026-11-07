#!/usr/bin/env bash
# Builds APFS test images for winapfs. macOS only, no sudo.
# Output: images/*.img (raw, GPT-partitioned), images/basic.container.img (bare
# container), one manifest JSON per volume, and SHA256SUMS. Then run ./verify.sh.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PY="${PYTHON:-python3}"
TOOL="$HERE/tool.py"
OUT="$HERE/images"
PASS="winapfs-test"
WORK="$(mktemp -d -t winapfs)"
DISK=""

cleanup() {
  [[ -n "$DISK" ]] && hdiutil detach -force "$DISK" >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

rm -rf "$OUT"; mkdir -p "$OUT" "$WORK/mnt"

create() {  # name size fs volname
  echo "== $1"
  hdiutil create -quiet -size "$2" -fs "$3" -volname "$4" -layout GPTSPUD "$WORK/$1.dmg"
  local out; out=$(hdiutil attach -nobrowse -owners on -mountroot "$WORK/mnt" "$WORK/$1.dmg" 2>/dev/null)
  DISK=$(awk 'NR==1{print $1}' <<<"$out")
  CONT=$(awk 'toupper($2) ~ /^EF57347C-0000-11AA-AA11/ {print $1; exit}' <<<"$out")
  [[ -n "$CONT" ]] || { echo "could not find APFS container in:"; echo "$out"; exit 1; }
}

add_volume() {  # fs volname [extra diskutil args...]
  local fs=$1 vol=$2; shift 2
  # custom -mountpoint needs root, so let diskutil pick one and link to it
  local out dev mp
  out=$(diskutil apfs addVolume "$CONT" "$fs" "$vol" "$@")
  dev=$(grep -oE 'disk[0-9]+s[0-9]+' <<<"$out" | tail -1)
  mp=$(diskutil info -plist "$dev" | plutil -extract MountPoint raw -)
  [[ -d "$mp" ]] || { echo "volume $vol not mounted:"; echo "$out"; exit 1; }
  ln -s "$mp" "$WORK/mnt/$vol"
}

mp() { (cd "$WORK/mnt/$1" && pwd -P); }
populate() { "$PY" "$TOOL" populate "$(mp "$1")" "$2"; }

finish() {  # name
  local name=$1
  sync
  hdiutil detach -quiet "$DISK"; DISK=""
  hdiutil convert -quiet "$WORK/$name.dmg" -format UDTO -o "$WORK/$name"
  mv "$WORK/$name.cdr" "$OUT/$name.img"
  rm -f "$WORK/$name.dmg"
}

create basic 128m APFS Basic
populate Basic basic
finish basic

create compressed 64m APFS Compressed
populate Compressed compressed
finish compressed

create links 32m APFS Links
populate Links links
finish links

create casesensitive 32m "Case-sensitive APFS" CaseSens
populate CaseSens casesensitive
finish casesensitive

create multivol 1600m APFS VolA   # APFS allows one volume per 512 MiB of container
add_volume "Case-sensitive APFS" VolB
add_volume APFS VolC -quota 16m
for v in VolA VolB VolC; do populate "$v" "small:$v"; done
finish multivol

create encrypted 600m APFS Public
add_volume APFS Secret -passphrase "$PASS" -passphraseHint "winapfs test password"
populate Public small:Public
populate Secret small:Secret
finish encrypted

"$PY" "$TOOL" extract "$OUT/basic.img" "$OUT/basic.container.img"

(cd "$OUT" && shasum -a 256 *.img > SHA256SUMS)
# manifests come from a read-only mount of the finished raw image, so they show
# exactly what is in the .img (true ownership, xattrs macOS added at unmount)
"$HERE/verify.sh" --record
echo "done:"; ls -lh "$OUT"
