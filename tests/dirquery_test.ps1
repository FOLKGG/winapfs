# winapfs - single-name directory queries (GetDirInfoByName), through a --rw mount
# SPDX-License-Identifier: GPL-2.0-or-later
#
# After each create, Windows components (filter drivers, Explorer) query the
# directory for that one name. Without GetDirInfoByName WinFsp answered each
# with a full listing, so copying many files into one folder slowed down
# quadratically (user report 2026-11-17: long copies sank to almost 0).
# Checks:
#   1. creating 400 files in one directory causes few full listings
#      (WINAPFS_DIRSTATS count; was one per file)
#   2. an exact-name query returns the stored name (case-insensitive volume:
#      any case finds it; case-sensitive: only the exact case)
#   3. a missing name finds nothing; dotfiles come back hidden; a name with
#      a mapped character (U+F03F = '?') is found
#   4. winapfs-fsck is clean

param(
	[Parameter(Mandatory)] [string]$Winapfs,
	[Parameter(Mandatory)] [string]$Fsck,
	[Parameter(Mandatory)] [string]$Image,
	[Parameter(Mandatory)] [string]$Work,
	[string]$Volume = '',
	[switch]$CaseSensitive,
	[string]$Mount = '',     # internal: the phase run under --run
	[string]$Phase = ''
)

$ErrorActionPreference = 'Stop'
$count = 400

function Fail([string]$m) { Write-Output "FAIL: $m"; exit 1 }

# FindFirstFile with no wildcard: one QueryDirectory for that name
function Query([string]$dir, [string]$name) {
	return @([IO.DirectoryInfo]::new($dir).GetFileSystemInfos($name))
}

if ($Phase -eq 'run') {
	$d = "$Mount\many"
	New-Item -ItemType Directory $d | Out-Null
	$src = [IO.Path]::GetTempFileName()
	[IO.File]::WriteAllBytes($src, (New-Object byte[] 4096))
	for ($i = 0; $i -lt $count; $i++) { [IO.File]::Copy($src, "$d\File$i.txt") }
	Remove-Item $src
	[IO.File]::WriteAllText("$d\.hidden", 'x')
	$odd = "q$([char]0xF03F)x"
	[IO.File]::WriteAllText("$d\$odd", 'x')

	$r = Query $d 'File7.txt'
	if ($r.Count -ne 1 -or $r[0].Name -cne 'File7.txt') { Fail "exact query for File7.txt: $($r.Name)" }
	$r = Query $d 'FILE7.TXT'
	if ($CaseSensitive) {
		if ($r.Count -ne 0) { Fail "case-sensitive volume found FILE7.TXT as $($r.Name)" }
	} elseif ($r.Count -ne 1 -or $r[0].Name -cne 'File7.txt') {
		Fail "case-insensitive query FILE7.TXT returned '$($r.Name)', expected the stored File7.txt"
	}
	if ((Query $d 'nothere.txt').Count -ne 0) { Fail "a missing name was found" }
	$r = Query $d '.hidden'
	if ($r.Count -ne 1 -or -not ($r[0].Attributes -band [IO.FileAttributes]::Hidden)) { Fail ".hidden is not found or not hidden" }
	$r = Query $d $odd
	if ($r.Count -ne 1 -or $r[0].Name -cne $odd) { Fail "name with a mapped character not found" }
	$r = Query $d 'many'
	if ($r.Count -ne 0) { Fail "a file query matched the directory itself" }
	# a full listing still sees everything
	$all = @([IO.Directory]::EnumerateFileSystemEntries($d))
	if ($all.Count -ne $count + 2) { Fail "listing has $($all.Count) entries, expected $($count + 2)" }
	exit 0
}

# driver (Continue: winapfs's messages on stderr are not errors)
$ErrorActionPreference = 'Continue'
$self = $MyInvocation.MyCommand.Path
$vol = @()
if ($Volume) { $vol = @('--volume', $Volume) }
$cs = ''
if ($CaseSensitive) { $cs = '-CaseSensitive' }
Copy-Item $Image $Work -Force
$run = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$self`" -Winapfs x -Fsck x -Image x -Work x $cs -Mount %MOUNT% -Phase run"

$env:WINAPFS_DIRSTATS = '1'
$out = & $Winapfs mount $Work * @vol --rw --run $run 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
Remove-Item Env:WINAPFS_DIRSTATS
if ($code -ne 0) { $out; Fail "mount phase (exit $code)" }
$listings = 0
foreach ($m in ($out | Select-String 'directory listings: (\d+) by pid')) { $listings += [int]$m.Matches[0].Groups[1].Value }
if ($listings -gt 50) { $out; Fail "$listings full directory listings for $count creates (single-name queries are listing the directory)" }

$sw = [Diagnostics.Stopwatch]::StartNew()
$why = ''
while ($sw.ElapsedMilliseconds -lt 10000) {
	try { [IO.File]::Open($Work, 'Open', 'Read', 'ReadWrite').Close(); break } catch { $why = $_.Exception.InnerException.Message; Start-Sleep -Milliseconds 20 }
}
if ($why) { Write-Output "note: the image could not be opened for $($sw.ElapsedMilliseconds) ms after unmount: $why" }
& $Fsck $Work
if ($LASTEXITCODE -ne 0) { Fail "fsck" }
Write-Output "OK: $count creates, $listings full listings, exact-name queries correct, fsck clean"
exit 0
