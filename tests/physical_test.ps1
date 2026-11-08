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

. "$PSScriptRoot\vhd_helpers.ps1"

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
