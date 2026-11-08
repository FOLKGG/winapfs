# Milestone 3 end-to-end test against real \\.\PhysicalDriveN devices.
#
# Wraps the GPT test images as fixed VHDs (raw data + 512-byte footer),
# attaches them read-only (no drive letters; Windows doesn't know APFS), then
# runs `winapfs mount-all` with no source arguments, so it has to find the
# APFS partitions itself by scanning every physical disk, and verifies every
# mounted volume against its manifest. Needs an elevated prompt.
#
#   powershell -ExecutionPolicy Bypass -File tests\physical_test.ps1 [-Build build] [-Log file] [-Write]
#
# -Write (W5) instead attaches a fresh copy of basic.img read-write and checks
# physical-disk writes: --rw without --allow-disk-writes is refused; the Win32
# write test runs through \\.\PhysicalDriveN (Windows' RAW volume on the
# partition gets locked); after detaching, both engines verify the result and
# winapfs-fsck -c is clean. The originals in testdata are never attached rw.
param(
    [string] $Build = "$PSScriptRoot\..\build",
    [string] $Config = "Release",
    [string] $Log,
    [switch] $Write
)
$ErrorActionPreference = "Stop"
if ($Log) { Start-Transcript -Path $Log -Force | Out-Null }

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "physical_test.ps1 needs an elevated (administrator) prompt"
    exit 2
}

$root = Resolve-Path "$PSScriptRoot\.."
$td = "$root\testdata"
$winapfs = Resolve-Path "$Build\src\mount\$Config\winapfs.exe"
$verify = Resolve-Path "$Build\tests\$Config\winapfs-verify.exe"
$vhdDir = "$Build\vhd"
New-Item -ItemType Directory -Force $vhdDir | Out-Null

function Write-BE([byte[]] $buf, [int] $off, [UInt64] $value, [int] $len) {
    for ($i = $len - 1; $i -ge 0; $i--) { $buf[$off + $i] = [byte]($value -band 0xFF); $value = $value -shr 8 }
}

# VHD spec appendix: CHS geometry for a disk of $size bytes
function Get-Geometry([UInt64] $size) {
    $total = [Math]::Min([UInt64]($size / 512), [UInt64](65535 * 16 * 255))
    if ($total -ge 65535 * 16 * 63) { $spt = 255; $heads = 16; $cth = [UInt64]($total / $spt) }
    else {
        $spt = 17; $cth = [UInt64]([Math]::Floor($total / $spt))
        $heads = [Math]::Max([UInt64][Math]::Floor(($cth + 1023) / 1024), 4)
        if ($cth -ge $heads * 1024 -or $heads -gt 16) { $spt = 31; $heads = 16; $cth = [UInt64][Math]::Floor($total / $spt) }
        if ($cth -ge $heads * 1024) { $spt = 63; $heads = 16; $cth = [UInt64][Math]::Floor($total / $spt) }
    }
    $cyl = [UInt64][Math]::Floor($cth / $heads)
    return @($cyl, $heads, $spt)
}

function New-FixedVhd([string] $img, [string] $vhd) {
    if ((Test-Path $vhd) -and (Get-Item $vhd).Length -eq (Get-Item $img).Length + 512) { return }
    Copy-Item $img $vhd -Force
    $size = [UInt64](Get-Item $img).Length
    $f = New-Object byte[] 512
    [Text.Encoding]::ASCII.GetBytes("conectix").CopyTo($f, 0)
    Write-BE $f 8 2 4                                   # features
    Write-BE $f 12 0x00010000 4                         # version
    Write-BE $f 16 ([UInt64]::MaxValue) 8               # data offset (fixed disk)
    Write-BE $f 24 ([UInt64]([DateTime]::UtcNow - [DateTime]'2000-01-01').TotalSeconds) 4
    [Text.Encoding]::ASCII.GetBytes("wapf").CopyTo($f, 28)
    Write-BE $f 32 0x00010000 4
    [Text.Encoding]::ASCII.GetBytes("Wi2k").CopyTo($f, 36)
    Write-BE $f 40 $size 8                              # original size
    Write-BE $f 48 $size 8                              # current size
    $g = Get-Geometry $size
    Write-BE $f 56 $g[0] 2; $f[58] = [byte]$g[1]; $f[59] = [byte]$g[2]
    Write-BE $f 60 2 4                                  # disk type: fixed
    [Guid]::NewGuid().ToByteArray().CopyTo($f, 68)
    $sum = [UInt32]0
    foreach ($b in $f) { $sum = [UInt32](($sum + $b) -band 0xFFFFFFFF) }
    Write-BE $f 64 ([UInt32](-bnot $sum -band 0xFFFFFFFF)) 4
    $s = [IO.File]::Open($vhd, 'Append'); $s.Write($f, 0, 512); $s.Close()
}

if ($Write) {
    $writetest = Resolve-Path "$Build\tests\$Config\apfsrw-writetest.exe"
    $fsck = Resolve-Path "$Build\src\fsck\$Config\winapfs-fsck.exe"
    $vhd = "$vhdDir\w5-basic.vhd"
    $model = "$vhdDir\w5-basic.model.json"
    $manifest = "$td\basic.Basic.manifest.json"
    Remove-Item $vhd, $model -ErrorAction SilentlyContinue
    New-FixedVhd "$td\basic.img" $vhd          # a copy: testdata stays pristine
    $rc = 1
    $attachedRw = $false
    try {
        Mount-DiskImage -ImagePath $vhd -Access ReadWrite -NoDriveLetter | Out-Null
        $attachedRw = $true
        $disk = "\\.\PhysicalDrive$((Get-DiskImage -ImagePath $vhd).Number)"
        Write-Host "== $vhd attached read-write as $disk"

        $ErrorActionPreference = "Continue"     # winapfs reports on stderr
        & $winapfs mount $disk * --rw --run "cmd /c exit 0"
        if ($LASTEXITCODE -eq 0) { throw "--rw on a physical disk was not refused without --allow-disk-writes" }
        Write-Host "   refused without --allow-disk-writes: ok"

        & $winapfs mount $disk * --rw --allow-disk-writes --volume Basic --run "`"$writetest`" --win32 %MOUNT% `"$manifest`" `"$vhd`" `"$model`" Basic"
        if ($LASTEXITCODE -ne 0) { throw "writes through $disk failed (exit $LASTEXITCODE)" }
        Write-Host "   Win32 writes through ${disk}: ok"
    }
    finally {
        if ($attachedRw) { Dismount-DiskImage -ImagePath $vhd | Out-Null }
    }
    & $writetest --verify $manifest $vhd $model Basic
    if ($LASTEXITCODE -ne 0) { Write-Host "verify failed"; exit 1 }
    & $fsck -c $vhd
    if ($LASTEXITCODE -ne 0) { Write-Host "fsck failed"; exit 1 }
    Write-Host "physical-disk write test: OK"
    if ($Log) { Stop-Transcript | Out-Null }
    exit 0
}

$images = @("basic", "multivol", "encrypted", "compressed")
$attached = @()
$rc = 1
try {
    foreach ($name in $images) {
        $vhd = "$vhdDir\$name.vhd"
        Write-Host "== $name.img -> $vhd"
        New-FixedVhd "$td\$name.img" $vhd
        $di = Mount-DiskImage -ImagePath $vhd -Access ReadOnly -NoDriveLetter -PassThru
        $attached += $vhd
        $di = Get-DiskImage -ImagePath $vhd
        Write-Host "   attached as \\.\PhysicalDrive$($di.Number)"
    }

    & $winapfs list
    $manifests = (Get-ChildItem "$td\basic.*.manifest.json", "$td\multivol.*.manifest.json",
        "$td\encrypted.*.manifest.json", "$td\compressed.*.manifest.json" | ForEach-Object FullName) -join ","
    & $winapfs mount-all --password winapfs-test --run "powershell -NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\verify_mounts.ps1`" -Verify `"$verify`" -Manifests $manifests"
    $rc = $LASTEXITCODE
    Write-Host "mount-all exit code: $rc"
}
finally {
    foreach ($vhd in $attached) { Dismount-DiskImage -ImagePath $vhd | Out-Null }
    if ($Log) { Stop-Transcript | Out-Null }
}
exit $rc
