# winapfs - crash test (W3)
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Crashes apfsrw-writetest at evenly spread device writes during a churn phase
# that follows a synced set of writes, in two ways:
#   kill   the process dies; everything written before stays (a program crash)
#   power  writes since the last flush are lost except a random subset (a power
#          cut with a volatile disk cache: checks that the barriers are right)
# After each crash:
#   1. winapfs-fsck is clean (the newest checkpoint on disk is consistent)
#   2. the synced state reads back exactly through ApfsLib and linux-apfs-rw
#   3. linux-apfs-rw mounts read-write again, writes and unmounts
#   4. winapfs-fsck -c is clean (now unmounted cleanly) and step 2 still holds

param(
	[Parameter(Mandatory)] [string]$Writetest,
	[Parameter(Mandatory)] [string]$Fsck,
	[Parameter(Mandatory)] [string]$Manifest,
	[Parameter(Mandatory)] [string]$Image,
	[Parameter(Mandatory)] [string]$Work,
	[string]$Volume = '',
	[int]$Points = 6,
	[string[]]$Modes = @('kill', 'power'),
	# for checking the checker: crashes with -Modes nobarrier must break something
	[switch]$ExpectBroken
)

$ErrorActionPreference = 'Continue'
$model = "$Work.model.json"

function Run([string]$exe, [string[]]$a) {
	$out = & $exe @a 2>&1 | ForEach-Object { "$_" }
	[pscustomobject]@{ Code = $LASTEXITCODE; Out = ($out -join "`n") }
}

$vol = @()
if ($Volume) { $vol = @($Volume) }

$r = Run $Writetest (@('--crash', 'count', '0', '0', $Manifest, $Image, $Work, $model) + $vol)
if ($r.Code -ne 0 -or $r.Out -notmatch 'crash phase: (\d+) writes') {
	Write-Output $r.Out
	Write-Output "FAIL: counting run (exit $($r.Code))"
	exit 1
}
$total = [int64]$Matches[1]
Write-Output "churn phase: $total device writes"

$failures = 0
$crashes = 0
foreach ($how in $Modes) {
	for ($k = 1; $k -le $Points; $k++) {
		$at = [int64][math]::Floor($total * $k / ($Points + 1))
		$tag = "$how at write $at"
		$steps = @(
			@{ Name = 'crash';        Exe = $Writetest; Args = @('--crash', $how, "$at", "$k", $Manifest, $Image, $Work, $model) + $vol; Ok = @(0, 99) },
			@{ Name = 'fsck';         Exe = $Fsck;      Args = @($Work);                                                           Ok = @(0) },
			@{ Name = 'verify';       Exe = $Writetest; Args = @('--verify-crash', $Manifest, $Work, $model) + $vol;               Ok = @(0) },
			@{ Name = 'recover';      Exe = $Writetest; Args = @('--recover', $Work) + $vol;                                       Ok = @(0) },
			@{ Name = 'fsck -c';      Exe = $Fsck;      Args = @('-c', $Work);                                                     Ok = @(0) },
			@{ Name = 'verify again'; Exe = $Writetest; Args = @('--verify-crash', $Manifest, $Work, $model) + $vol;               Ok = @(0) }
		)
		$crashed = $false
		$bad = $null
		foreach ($s in $steps) {
			$r = Run $s.Exe $s.Args
			if ($s.Name -eq 'crash' -and $r.Code -eq 99) { $crashed = $true }
			if ($s.Ok -notcontains $r.Code) { $bad = $s.Name; break }
		}
		if ($bad) {
			$failures++
			Write-Output "FAIL $tag ($bad, exit $($r.Code)):"
			Write-Output $r.Out
			Copy-Item $Work "$Work.$how.$at.failed" -Force
		} else {
			if ($crashed) { $crashes++ }
			Write-Output ("ok   {0}{1}" -f $tag, $(if ($crashed) { '' } else { ' (finished before the crash point)' }))
		}
	}
}
Write-Output "$crashes crashes recovered, $failures failures"
if ($ExpectBroken) {
	if ($failures) { Write-Output "the damage was detected, as expected"; exit 0 }
	Write-Output "FAIL: crashes on a disk that ignores flushes went unnoticed"
	exit 1
}
if ($failures -or $crashes -eq 0) { exit 1 }
exit 0
