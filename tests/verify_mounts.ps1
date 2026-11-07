# Runs inside `winapfs mount-all ... --run`: verifies every mounted volume
# against its manifest. The volume name comes from the manifest file name
# (<image>.<Volume>.manifest.json) and its mount point from WINAPFS_MOUNT_<Volume>.
param(
    [Parameter(Mandatory)] [string] $Verify,
    [Parameter(Mandatory)] [string] $Manifests,   # comma-separated (-File cannot pass arrays)
    [string] $Fields = "type,size,sha256"
)

$failed = 0
foreach ($m in ($Manifests -split ",")) {
    $vol = ([IO.Path]::GetFileName($m) -split '\.')[1]
    $mp = [Environment]::GetEnvironmentVariable("WINAPFS_MOUNT_$vol")
    if (-not $mp) {
        Write-Host "volume $vol was not mounted"
        $failed++
        continue
    }
    & $Verify $m --dir $mp --fields $Fields
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
exit $failed
