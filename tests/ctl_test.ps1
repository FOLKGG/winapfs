# winapfs - the service's control pipe through `winapfs ctl` (no administrator needed)
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs `winapfs service debug` on image copies (settings in HKCU, control pipe
# \\.\pipe\winapfs-debug) and drives it with winapfs ctl: list, unmount and
# mount, set ReadWrite (then write a file) and Disabled, eject, unlocking an
# encrypted volume (wrong password, then the right one), unknown volumes.
# Afterwards winapfs-fsck -c on the written image.

param(
	[Parameter(Mandatory)] [string]$Winapfs,
	[string]$Fsck = '',
	[string]$Testdata = '',
	[string]$WorkDir = '',
	[string]$Phase = ''
)
$ErrorActionPreference = 'Continue'

function Ctl([string[]]$a) {
	$out = & $Winapfs ctl @a 2>&1 | ForEach-Object { "$_" }
	return [pscustomobject]@{ Code = $LASTEXITCODE; Out = ($out -join "`n") }
}
function Vols {
	$r = Ctl @('list')
	$v = @{}
	foreach ($l in ($r.Out -split "`n")) {
		# name letter state mode uuid disk
		if ($l -match '^(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+([0-9A-F-]{36})') { $v[$Matches[1]] = [pscustomobject]@{ Letter = $Matches[2]; State = $Matches[3]; Mode = $Matches[4] } }
	}
	return $v
}
function Check([bool]$ok, [string]$what) { if (-not $ok) { Write-Output "FAIL: $what"; exit 1 }; Write-Output "   ok: $what" }

if ($Phase -eq 'inner') {
	$v = Vols
	foreach ($n in 'Basic', 'VolA', 'VolB', 'VolC', 'Public') { Check ($v[$n] -and $v[$n].State -eq 'mounted') "$n mounted" }
	Check ($v['Secret'] -and $v['Secret'].State -eq 'locked') "Secret is locked (encrypted)"

	Check ((Ctl @('unmount', 'VolC')).Code -eq 0) "unmount VolC"
	$v = Vols
	Check ($v['VolC'].State -eq 'unmounted' -and $v['VolC'].Letter -eq '-') "VolC shows as unmounted"
	Check ((Ctl @('rescan')).Code -eq 0 -and (Vols)['VolC'].State -eq 'unmounted') "a rescan leaves it unmounted"
	Check ((Ctl @('mount', 'VolC')).Code -eq 0 -and (Vols)['VolC'].State -eq 'mounted') "mount VolC again"

	Check ((Ctl @('set', 'VolA', 'ReadWrite', '1')).Code -eq 0) "set VolA ReadWrite 1"
	$v = Vols
	Check ($v['VolA'].State -eq 'mounted' -and $v['VolA'].Mode -eq 'rw') "VolA remounted read-write"
	Check ($v['VolB'].Mode -eq 'ro') "VolB stays read-only"
	try { [IO.File]::WriteAllText("$($v['VolA'].Letter)\from-ctl.txt", 'written via winapfs ctl'); $w = $true } catch { $w = $false }
	Check $w "a file written to VolA"

	Check ((Ctl @('set', 'VolB', 'Disabled', '1')).Code -eq 0 -and (Vols)['VolB'].State -eq 'disabled') "VolB disabled (unmounted)"
	Check ((Ctl @('set', 'VolB', 'Disabled', '0')).Code -eq 0 -and (Vols)['VolB'].State -eq 'mounted') "VolB enabled (mounted)"

	$r = Ctl @('mount', 'Secret', '--password', 'wrong-password')
	Check ($r.Code -ne 0 -and (Vols)['Secret'].State -eq 'locked') "a wrong password is refused"
	$r = Ctl @('mount', 'Secret', '--password', 'winapfs-test')
	$v = Vols
	Check ($r.Code -eq 0 -and $v['Secret'].State -eq 'mounted' -and $v['Secret'].Mode -eq 'ro') "Secret unlocked and mounted (read-only)"
	Check (Test-Path "$($v['Secret'].Letter)\") "Secret is reachable at $($v['Secret'].Letter)"

	Check ((Ctl @('eject', 'Basic')).Code -eq 0) "eject Basic"
	Check (-not (Vols).ContainsKey('Basic')) "Basic is gone from the list"
	Ctl @('rescan') | Out-Null
	Check (-not (Vols).ContainsKey('Basic')) "and stays gone after a rescan"

	Check ((Ctl @('mount', 'NoSuchVolume')).Code -ne 0) "an unknown volume is an error"
	Check ((Ctl @('set', 'VolA', 'Bogus', '1')).Code -ne 0) "an unknown setting is an error"
	exit 0
}

# driver
$self = $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$imgs = @()
foreach ($i in 'basic', 'multivol', 'encrypted') {
	Copy-Item "$Testdata\$i.img" "$WorkDir\ctl-$i.img" -Force
	$imgs += @('--image', "$WorkDir\ctl-$i.img")
}
$key = 'HKCU:\Software\winapfs-test'
Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue
$env:WINAPFS_CONFIG_HKCU = '1'
$inner = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$self`" -Winapfs `"$Winapfs`" -Phase inner"
& $Winapfs service debug @imgs --run $inner 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -notmatch 'space manager' }
$code = $LASTEXITCODE
Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue
if ($code -ne 0) { Write-Output "FAIL: control test (exit $code)"; exit 1 }
& $Fsck -c "$WorkDir\ctl-multivol.img"
if ($LASTEXITCODE -ne 0) { Write-Output "FAIL: fsck after writing through the service"; exit 1 }
Write-Output "OK: control pipe (list, mount, unmount, set, eject, unlock); fsck clean"
exit 0
