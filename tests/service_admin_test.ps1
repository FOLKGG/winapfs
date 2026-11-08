# winapfs - the real auto-mount service, end to end (needs an elevated prompt)
# SPDX-License-Identifier: GPL-2.0-or-later
#
#   powershell -ExecutionPolicy Bypass -File tests\service_admin_test.ps1 [-Build build] [-Log file]
#
# Installs the service from the build directory, attaches a copy of basic.img
# as a VHD (a real \\.\PhysicalDriveN), waits for the service to mount its
# volume at a drive letter every session can see, reads a file from it, then
# detaches the VHD (Windows asks the service to let go of the disk: handle
# notification, query-remove) and checks that the drive disappears. Finally
# uninstalls the service. testdata is never attached; settings for the test
# volume are removed afterwards.

param(
	[string] $Build = "$PSScriptRoot\..\build",
	[string] $Config = "Release",
	[string] $Log
)
$ErrorActionPreference = 'Continue'
if ($Log) { Start-Transcript -Path $Log -Force | Out-Null }

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
	Write-Host "service_admin_test.ps1 needs an elevated (administrator) prompt"
	exit 2
}

$root = Resolve-Path "$PSScriptRoot\.."
$winapfs = (Resolve-Path "$Build\src\mount\$Config\winapfs.exe").Path
$vhdDir = "$Build\vhd"
New-Item -ItemType Directory -Force $vhdDir | Out-Null
$vhd = "$vhdDir\svc-basic.vhd"

. "$PSScriptRoot\vhd_helpers.ps1"

function Find-Basic {
	foreach ($d in [IO.DriveInfo]::GetDrives()) {
		try { if ($d.IsReady -and $d.VolumeLabel -eq 'Basic' -and (Test-Path "$($d.Name)hello.txt")) { return $d.Name } } catch { }
	}
	return $null
}

$rc = 1
$attached = $false
try {
	if (Find-Basic) { throw "a volume labelled Basic is already mounted; unmount it first" }
	Remove-Item $vhd -ErrorAction SilentlyContinue
	New-FixedVhd "$root\testdata\basic.img" $vhd

	& $winapfs service install
	if ($LASTEXITCODE -ne 0) { throw "service install failed" }
	& $winapfs service status

	Mount-DiskImage -ImagePath $vhd -Access ReadOnly -NoDriveLetter | Out-Null
	$attached = $true
	Write-Host "attached $vhd as \\.\PhysicalDrive$((Get-DiskImage -ImagePath $vhd).Number); waiting for the service"
	$drive = $null
	for ($i = 0; $i -lt 60 -and -not $drive; $i++) { Start-Sleep -Milliseconds 500; $drive = Find-Basic }
	if (-not $drive) { throw "the service did not mount the volume within 30 s" }
	Write-Host "mounted at $drive; hello.txt says: $((Get-Content "$($drive)hello.txt" -Raw).Trim())"

	Dismount-DiskImage -ImagePath $vhd | Out-Null
	$attached = $false
	Write-Host "detached; waiting for the drive to go"
	$gone = $false
	for ($i = 0; $i -lt 40 -and -not $gone; $i++) { Start-Sleep -Milliseconds 500; $gone = -not (Find-Basic) }
	if (-not $gone) { throw "the drive is still there after the disk was detached" }
	Write-Host "drive gone after detach: ok"
	$rc = 0
}
catch {
	Write-Host "FAIL: $_"
}
finally {
	if ($attached) { Dismount-DiskImage -ImagePath $vhd | Out-Null }
	# the events first: uninstall removes the event source and its message table
	Write-Host "recent service events:"
	try {
		Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'winapfs' } -MaxEvents 12 -ErrorAction Stop |
			Sort-Object TimeCreated | ForEach-Object { Write-Host "  $($_.TimeCreated.ToString('HH:mm:ss'))  $($_.Message)" }
	} catch { Write-Host "  (none readable: $($_.Exception.Message))" }
	& $winapfs service uninstall
	# forget the test volume's settings
	Get-ChildItem 'HKLM:\SOFTWARE\winapfs\Volumes' -ErrorAction SilentlyContinue |
		Where-Object { (Get-ItemProperty $_.PSPath).Name -eq 'Basic' } | Remove-Item -Recurse -Force
	if ($Log) { Stop-Transcript | Out-Null }
}
if ($rc -eq 0) { Write-Host "service test: OK" }
exit $rc
