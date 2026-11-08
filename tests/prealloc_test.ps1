# winapfs - the Windows write pattern (size first, then data), through a --rw mount
# SPDX-License-Identifier: GPL-2.0-or-later
#
# CopyFile and Explorer set a file's final size before writing it. The mount
# keeps that growth pending, so the data goes to the file's tail and the
# engine merges it into large extents (growing the APFS file first made one
# extent per 4 KiB block). Checks:
#   1. the size is visible at once and the unwritten part reads as zeros
#   2. the data reads back exactly after a remount
#   3. the 8 MiB file is a few extents, not ~2048 (KSHIM_IOSTATS piece count)
#   4. a file grown to 1 MiB with 100 bytes written ends up 1 MiB, zero tail
#   5. winapfs-fsck is clean

param(
	[Parameter(Mandatory)] [string]$Winapfs,
	[Parameter(Mandatory)] [string]$Fsck,
	[Parameter(Mandatory)] [string]$Image,
	[Parameter(Mandatory)] [string]$Work,
	[string]$Volume = '',
	[string]$Mount = '',     # internal: the phase run under --run
	[string]$Phase = ''
)

$ErrorActionPreference = 'Stop'
$size = 8MB

function Pattern([int]$n) {
	$cycle = New-Object byte[] 251
	for ($i = 0; $i -lt 251; $i++) { $cycle[$i] = [byte](($i * 31 + 7) % 251) }
	$b = New-Object byte[] $n
	for ($off = 0; $off -lt $n; $off += 251) { [Array]::Copy($cycle, 0, $b, $off, [Math]::Min(251, $n - $off)) }
	return , $b   # the comma keeps PowerShell from unrolling the array
}

function Hash([byte[]]$b) {
	return [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($b))
}

if ($Phase -eq 'write') {
	$p = "$Mount\pre.bin"
	$fs = [IO.File]::Open($p, 'CreateNew', 'ReadWrite', 'ReadWrite')
	$fs.SetLength($size)
	if ((Get-Item $p).Length -ne $size) { Write-Output "FAIL: size not visible after SetLength"; exit 1 }
	$fs.Position = 4MB
	$z = New-Object byte[] 4096
	[void]$fs.Read($z, 0, $z.Length)
	if ((Hash $z) -ne (Hash (New-Object byte[] 4096))) { Write-Output "FAIL: pending growth does not read as zeros"; exit 1 }
	$fs.Position = 0
	$data = Pattern $size
	for ($off = 0; $off -lt $size; $off += 65536) { $fs.Write($data, $off, 65536) }
	$fs.Close()

	$q = "$Mount\short.bin"
	$fs = [IO.File]::Open($q, 'CreateNew', 'ReadWrite', 'ReadWrite')
	$fs.SetLength(1MB)
	$fs.Write((Pattern 100), 0, 100)
	$fs.Close()
	exit 0
}

if ($Phase -eq 'verify') {
	$got = [IO.File]::ReadAllBytes("$Mount\pre.bin")
	if ($got.Length -ne $size -or (Hash $got) -ne (Hash (Pattern $size))) { Write-Output "FAIL: pre.bin contents differ"; exit 1 }
	$s = [IO.File]::ReadAllBytes("$Mount\short.bin")
	if ($s.Length -ne 1MB) { Write-Output "FAIL: short.bin is $($s.Length) bytes, expected 1 MiB"; exit 1 }
	$want = New-Object byte[] 1MB
	[Array]::Copy((Pattern 100), 0, $want, 0, 100)
	if ((Hash $s) -ne (Hash $want)) { Write-Output "FAIL: short.bin is not 100 bytes of data and a zero tail"; exit 1 }
	exit 0
}

# driver (Continue: winapfs's messages on stderr are not errors)
$ErrorActionPreference = 'Continue'
$self = $MyInvocation.MyCommand.Path
$vol = @()
if ($Volume) { $vol = @('--volume', $Volume) }
Copy-Item $Image $Work -Force
$run = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$self`" -Winapfs x -Fsck x -Image x -Work x -Mount %MOUNT% -Phase"

$out = & $Winapfs mount $Work * @vol --rw --run "$run write" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { $out; Write-Output "FAIL: write phase (exit $LASTEXITCODE)"; exit 1 }

$env:KSHIM_IOSTATS = '1'
$out = & $Winapfs mount $Work * @vol --run "$run verify" 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
Remove-Item Env:KSHIM_IOSTATS
if ($code -ne 0) { $out; Write-Output "FAIL: verify phase (exit $code)"; exit 1 }
$m = $out | Select-String 'readahead saw (\d+) extent pieces'
if (-not $m) { $out; Write-Output "FAIL: no piece count"; exit 1 }
$pieces = [int]$m.Matches[0].Groups[1].Value
# 8 MiB in 1 MiB readahead batches is >= 8 pieces; one extent per block would be ~2048
if ($pieces -gt 64) { Write-Output "FAIL: $pieces extent pieces read: the file is fragmented"; exit 1 }

& $Fsck $Work
if ($LASTEXITCODE -ne 0) { Write-Output "FAIL: fsck"; exit 1 }
Write-Output "OK: size-first writes read back, $pieces extent pieces, fsck clean"
exit 0
