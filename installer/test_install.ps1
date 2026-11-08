# winapfs - install / upgrade / uninstall test of the MSI (needs an elevated prompt)
# SPDX-License-Identifier: GPL-2.0-or-later
#
#   powershell -ExecutionPolicy Bypass -File installer\test_install.ps1 [-Log file]
#
# 1. installs dist\winapfs-<version>-x64.msi silently: files, running service,
#    PATH entry, event source
# 2. the installed service mounts a VHD copy of basic.img and lets go of it on
#    detach
# 3. major upgrade to a rebuilt 0.0.1-higher package: one product entry, the
#    service runs the new files
# 4. uninstall: service, files and PATH entry gone; HKLM\SOFTWARE\winapfs kept
# Leaves nothing installed (also on failure). testdata is never attached.

param([string]$Log)
$ErrorActionPreference = 'Continue'
if ($Log) { Start-Transcript -Path $Log -Force | Out-Null }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
	Write-Host "test_install.ps1 needs an elevated (administrator) prompt"
	exit 2
}

$root = (Resolve-Path "$PSScriptRoot\..").Path
. "$root\tests\vhd_helpers.ps1"
$m = Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(winapfs VERSION ([0-9]+)\.([0-9]+)\.([0-9]+)'
$v1 = "$($m.Matches[0].Groups[1].Value).$($m.Matches[0].Groups[2].Value).$($m.Matches[0].Groups[3].Value)"
$v2 = "$($m.Matches[0].Groups[1].Value).$($m.Matches[0].Groups[2].Value).$([int]$m.Matches[0].Groups[3].Value + 1)"
$dir = "$root\build\installer-test"
New-Item -ItemType Directory -Force $dir | Out-Null
$msi1 = "$dir\winapfs-$v1-x64.msi"
$msi2 = "$dir\winapfs-$v2-x64.msi"
$inst = "$env:ProgramFiles\winapfs"
$vhd = "$dir\inst-basic.vhd"

function Step([string]$what) { Write-Host "== $what" }
function Check([bool]$ok, [string]$what) { if (-not $ok) { throw "FAILED: $what" }; Write-Host "   ok: $what" }
function Products { Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue | Where-Object DisplayName -like 'winapfs*' }
function MachinePath { [Environment]::GetEnvironmentVariable('PATH', 'Machine') }
function Find-Basic {
	foreach ($d in [IO.DriveInfo]::GetDrives()) {
		try { if ($d.IsReady -and $d.VolumeLabel -eq 'Basic' -and (Test-Path "$($d.Name)hello.txt")) { return $d.Name } } catch { }
	}
	return $null
}

$rc = 1
$attached = $false
try {
	Check (-not (Products)) "winapfs is not installed yet"
	Step "building $v1 and $v2 packages"
	& powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\build.ps1" -Out $dir -Version $v1 | Out-Host
	& powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\build.ps1" -Out $dir -Version $v2 | Out-Host
	Check ((Test-Path $msi1) -and (Test-Path $msi2)) "both packages built"

	Step "install $v1"
	$p = Start-Process msiexec -ArgumentList "/i `"$msi1`" /qn /l*v `"$dir\install.log`"" -Wait -PassThru
	Check ($p.ExitCode -eq 0) "msiexec exit code $($p.ExitCode)"
	foreach ($f in 'winapfs.exe', 'winapfs-cli.exe', 'winapfs-fsck.exe', 'README.md', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt') { Check (Test-Path "$inst\$f") "installed $f" }
	$svc = Get-Service winapfs -ErrorAction SilentlyContinue
	Check ($svc -and $svc.Status -eq 'Running') "service running"
	Check ((Get-CimInstance Win32_Service -Filter "Name='winapfs'").PathName -like "*$inst\winapfs.exe*") "service runs the installed exe"
	Check ((MachinePath) -like "*$inst*") "install folder on the system PATH"
	Check (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\winapfs') "event log source"
	Check (@(Products).Count -eq 1 -and (Products).DisplayVersion -eq $v1) "one product entry, version $v1"

	Step "the installed service mounts a disk"
	Check (-not (Find-Basic)) "no Basic volume mounted beforehand"
	Remove-Item $vhd -ErrorAction SilentlyContinue
	New-FixedVhd "$root\testdata\basic.img" $vhd
	Mount-DiskImage -ImagePath $vhd -Access ReadOnly -NoDriveLetter | Out-Null
	$attached = $true
	$drive = $null
	for ($i = 0; $i -lt 60 -and -not $drive; $i++) { Start-Sleep -Milliseconds 500; $drive = Find-Basic }
	Check ([bool]$drive) "volume mounted at $drive"
	Dismount-DiskImage -ImagePath $vhd | Out-Null
	$attached = $false
	$gone = $false
	for ($i = 0; $i -lt 40 -and -not $gone; $i++) { Start-Sleep -Milliseconds 500; $gone = -not (Find-Basic) }
	Check $gone "drive gone after detach"

	Step "upgrade to $v2"
	$p = Start-Process msiexec -ArgumentList "/i `"$msi2`" /qn /l*v `"$dir\upgrade.log`"" -Wait -PassThru
	Check ($p.ExitCode -eq 0) "msiexec exit code $($p.ExitCode)"
	Check (@(Products).Count -eq 1 -and (Products).DisplayVersion -eq $v2) "one product entry, version $v2"
	$svc = Get-Service winapfs -ErrorAction SilentlyContinue
	Check ($svc -and $svc.Status -eq 'Running') "service running after the upgrade"

	Step "uninstall"
	$code = (Products).PSChildName
	$p = Start-Process msiexec -ArgumentList "/x $code /qn /l*v `"$dir\uninstall.log`"" -Wait -PassThru
	Check ($p.ExitCode -eq 0) "msiexec exit code $($p.ExitCode)"
	Check (-not (Get-Service winapfs -ErrorAction SilentlyContinue)) "service removed"
	Check (-not (Test-Path "$inst\winapfs.exe")) "files removed"
	Check (-not ((MachinePath) -like "*$inst*")) "PATH entry removed"
	Check (-not (Products)) "no product entry left"
	Check (Test-Path 'HKLM:\SOFTWARE\winapfs') "settings kept (HKLM\SOFTWARE\winapfs)"
	$rc = 0
}
catch {
	Write-Host $_
}
finally {
	if ($attached) { Dismount-DiskImage -ImagePath $vhd | Out-Null }
	foreach ($p in @(Products)) { Start-Process msiexec -ArgumentList "/x $($p.PSChildName) /qn" -Wait | Out-Null }
	Get-ChildItem 'HKLM:\SOFTWARE\winapfs\Volumes' -ErrorAction SilentlyContinue |
		Where-Object { (Get-ItemProperty $_.PSPath).Name -eq 'Basic' } | Remove-Item -Recurse -Force
	if ($Log) { Stop-Transcript | Out-Null }
}
if ($rc -eq 0) { Write-Host "installer test: OK" }
exit $rc
