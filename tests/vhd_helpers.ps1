# VHD helpers for the elevated tests: wrap a raw disk image as a fixed VHD
# (the data plus a 512-byte footer), which Windows attaches as a real disk.
# SPDX-License-Identifier: GPL-2.0-or-later

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

