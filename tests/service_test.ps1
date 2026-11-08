# winapfs - the auto-mount logic of the service, without administrator rights
# SPDX-License-Identifier: GPL-2.0-or-later
#
# `winapfs service debug` runs the service's AutoMounter in the console, on
# image copies instead of physical disks, with settings in
# HKCU\Software\winapfs-test (WINAPFS_CONFIG_HKCU=1). Two runs:
#   1. every volume of basic.img and multivol.img is mounted; letters and
#      names are remembered per volume UUID
#   2. after setting VolC Disabled, a new DriveLetter for Basic and ReadWrite
#      for VolA: VolC is not mounted, Basic is at the new letter, VolA/VolB keep
#      their letters, VolA is writable (a file is written); then fsck -c

param(
	[Parameter(Mandatory)] [string]$Winapfs,
	[Parameter(Mandatory)] [string]$Fsck,
	[Parameter(Mandatory)] [string]$Testdata,
	[Parameter(Mandatory)] [string]$WorkDir,
	[string]$Phase = '',
	[string]$Out = ''
)

if ($Phase -eq 'record') {
	# runs under the service: record WINAPFS_MOUNT_* and check the mounts
	$m = @{}
	Get-ChildItem Env: | Where-Object Name -like 'WINAPFS_MOUNT_*' | ForEach-Object { $m[$_.Name.Substring(14)] = $_.Value }
	foreach ($v in $m.Keys) { if (-not (Test-Path "$($m[$v])\")) { Write-Output "FAIL: $v at $($m[$v]) is not reachable"; exit 1 } }
	if ($m.ContainsKey('Basic') -and -not (Test-Path "$($m['Basic'])\hello.txt")) { Write-Output "FAIL: Basic has no hello.txt"; exit 1 }
	if ($m.ContainsKey('VolA')) {
		try { [IO.File]::WriteAllText("$($m['VolA'])\from-service.txt", 'written by the service test'); $m['VolA_written'] = '1' } catch { $m['VolA_written'] = '0' }
	}
	$m | ConvertTo-Json | Set-Content $Out
	exit 0
}

$ErrorActionPreference = 'Continue'
$self = $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$basic = "$WorkDir\svc-basic.img"
$multi = "$WorkDir\svc-multivol.img"
Copy-Item "$Testdata\basic.img" $basic -Force
Copy-Item "$Testdata\multivol.img" $multi -Force
$key = 'HKCU:\Software\winapfs-test'
Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue
$env:WINAPFS_CONFIG_HKCU = '1'

function Run([string]$out) {
	$cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$self`" -Winapfs x -Fsck x -Testdata x -WorkDir x -Phase record -Out `"$out`""
	$log = & $Winapfs service debug --image $basic --image $multi --run $cmd 2>&1 | ForEach-Object { "$_" }
	$code = $LASTEXITCODE
	$log | ForEach-Object { Write-Host "  | $_" }
	if ($code -ne 0) { Write-Output "FAIL: service debug exited $code"; exit 1 }
	# a read-only volume next to a read-write one must share its engine container
	if ($log -match 'falling back') { Write-Output "FAIL: a volume fell back to a second reader of the container"; exit 1 }
	return (Get-Content $out -Raw | ConvertFrom-Json)
}

$r1 = Run "$WorkDir\svc-run1.json"
foreach ($v in 'Basic', 'VolA', 'VolB', 'VolC') { if (-not $r1.$v) { Write-Output "FAIL: run 1 did not mount $v"; exit 1 } }
if ($r1.VolA_written -ne '0') { Write-Output "FAIL: VolA was writable without ReadWrite"; exit 1 }

# settings for run 2, found by the remembered names
$vols = @{}
Get-ChildItem "$key\Volumes" | ForEach-Object { $vols[(Get-ItemProperty $_.PSPath).Name] = $_.PSPath }
foreach ($v in 'Basic', 'VolA', 'VolB', 'VolC') { if (-not $vols[$v]) { Write-Output "FAIL: no remembered settings for $v"; exit 1 } }
Set-ItemProperty $vols['VolC'] -Name Disabled -Value 1 -Type DWord
Set-ItemProperty $vols['VolA'] -Name ReadWrite -Value 1 -Type DWord
$used = @($r1.Basic, $r1.VolA, $r1.VolB, $r1.VolC) | ForEach-Object { $_.Substring(0, 1) }
$free = [char[]](68..90) | Where-Object { $used -notcontains [string]$_ -and -not (Test-Path "$($_):\") } | Select-Object -First 1
Set-ItemProperty $vols['Basic'] -Name DriveLetter -Value "$($free):"

$r2 = Run "$WorkDir\svc-run2.json"
if ($r2.VolC) { Write-Output "FAIL: VolC is disabled but was mounted"; exit 1 }
if ($r2.Basic -ne "$($free):") { Write-Output "FAIL: Basic at $($r2.Basic), configured $($free):"; exit 1 }
if ($r2.VolA -ne $r1.VolA -or $r2.VolB -ne $r1.VolB) { Write-Output "FAIL: letters not remembered ($($r1.VolA)/$($r1.VolB) -> $($r2.VolA)/$($r2.VolB))"; exit 1 }
if ($r2.VolA_written -ne '1') { Write-Output "FAIL: VolA is ReadWrite but could not be written"; exit 1 }

& $Fsck -c $multi
if ($LASTEXITCODE -ne 0) { Write-Output "FAIL: fsck after the service wrote to VolA"; exit 1 }
Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue
Write-Output "OK: auto-mount, remembered letters, Disabled, DriveLetter, ReadWrite; fsck clean"
exit 0
