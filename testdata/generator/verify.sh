#!/usr/bin/env bash
# Mounts each raw image read-only with macOS's own APFS driver and checks it
# against its manifests (or, with --record, writes them). macOS only, no sudo.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PY="${PYTHON:-python3}"
TOOL="$HERE/tool.py"
IMG="$HERE/images"
PASS="winapfs-test"
WORK="$(mktemp -d -t winapfs-verify)"
DISK=""
cleanup() {
  [[ -n "$DISK" ]] && hdiutil detach -force "$DISK" >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

RECORD=0; [[ "${1:-}" == "--record" ]] && RECORD=1
(( RECORD )) || (cd "$IMG" && shasum -a 256 -c SHA256SUMS)

check() {  # image-file manifest-prefix volume...
  local img=$1 prefix=$2 out m vol dev mp; shift 2
  echo "== $img"
  out=$(hdiutil attach -readonly -nobrowse -owners on -mountroot "$WORK" \
        -imagekey diskimage-class=CRawDiskImage "$IMG/$img" 2>/dev/null)
  DISK=$(awk 'NR==1{print $1}' <<<"$out")
  for vol in "$@"; do
    m="$IMG/$prefix.$vol.manifest.json"
    mp="$WORK/$vol"
    if [[ ! -d "$mp" ]]; then  # locked encrypted volume
      dev=$(diskutil list "$DISK" >/dev/null; diskutil info -plist "$vol" | plutil -extract DeviceIdentifier raw -)
      diskutil apfs unlockVolume "$dev" -passphrase "$PASS" -nomount >/dev/null
      diskutil mount readOnly "$dev" >/dev/null
      mp=$(diskutil info -plist "$dev" | plutil -extract MountPoint raw -)
      echo "   unlocked $vol ($dev) with the test passphrase"
    fi
    if (( RECORD )); then
      "$PY" "$TOOL" manifest "$mp" "$m"; echo "   recorded $(basename "$m")"
    else
      "$PY" "$TOOL" manifest "$mp" "$WORK/actual.json"
      "$PY" "$TOOL" compare "$m" "$WORK/actual.json"
    fi
  done
  hdiutil detach -quiet "$DISK"; DISK=""
}

check basic.img basic Basic
check compressed.img compressed Compressed
check links.img links Links
check casesensitive.img casesensitive CaseSens
check multivol.img multivol VolA VolB VolC
check encrypted.img encrypted Public Secret
(( RECORD )) && exit 0
check basic.container.img basic Basic
echo "all images verified"
