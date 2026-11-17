# winapfs - render the logo (assets/logo.svg) as the Windows icon and PNGs
# SPDX-License-Identifier: CC0-1.0
#
#   powershell -ExecutionPolicy Bypass -File assets\make_icon.ps1
#
# Writes assets\winapfs.ico (16-256 px, PNG entries) and assets\logo-256.png.
# GDI+ draws the same shapes as logo.svg (512-unit grid). At 32 px and below a
# simplified mark is used (no arrow or status light, bolder bars, the tile
# filling the square) so it stays legible in the notification area.

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

function Rounded([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
	$p = New-Object System.Drawing.Drawing2D.GraphicsPath
	$d = 2 * $r
	$p.AddArc($x, $y, $d, $d, 180, 90)
	$p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
	$p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
	$p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
	$p.CloseFigure()
	return $p
}

function Color([string]$hex, [int]$a = 255) {
	$c = [System.Drawing.ColorTranslator]::FromHtml($hex)
	return [System.Drawing.Color]::FromArgb($a, $c.R, $c.G, $c.B)
}

function Render([int]$size) {
	$bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$g.SmoothingMode = 'AntiAlias'
	$g.PixelOffsetMode = 'HighQuality'
	$g.Clear([System.Drawing.Color]::Transparent)
	$s = $size / 512.0
	$g.ScaleTransform($s, $s)
	$small = $size -le 32

	# tile with a diagonal gradient
	if ($small) { $tile = Rounded 0 0 512 512 104 } else { $tile = Rounded 16 16 480 480 112 }
	$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(16, 16)), ([System.Drawing.PointF]::new(496, 496)), (Color '#16a3e0'), (Color '#5b4fe0')
	$g.FillPath($bg, $tile)
	if (-not $small) {
		$shine = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 16)), ([System.Drawing.PointF]::new(0, 256)), (Color '#ffffff' 46), (Color '#ffffff' 0)
		$g.SetClip($tile)
		$g.FillRectangle($shine, 16, 16, 480, 240)
		$g.ResetClip()
	}

	$white = New-Object System.Drawing.SolidBrush (Color '#ffffff')
	$c1 = New-Object System.Drawing.SolidBrush (Color '#16a3e0')
	$c2 = New-Object System.Drawing.SolidBrush (Color '#3a7ae0')
	$c3 = New-Object System.Drawing.SolidBrush (Color '#5b4fe0')
	if ($small) {
		# drive with three bold volume bars
		$g.FillPath($white, (Rounded 72 80 368 352 56))
		$g.FillPath($c1, (Rounded 120 132 272 72 22))
		$g.FillPath($c2, (Rounded 120 220 272 72 22))
		$g.FillPath($c3, (Rounded 120 308 180 72 22))
	} else {
		$g.FillPath($white, (Rounded 104 112 304 288 44))
		$g.FillPath($c1, (Rounded 140 152 232 44 14))
		$g.FillPath($c2, (Rounded 140 212 232 44 14))
		$g.FillPath($c3, (Rounded 140 272 148 44 14))
		$g.FillEllipse((New-Object System.Drawing.SolidBrush (Color '#22c55e')), 326, 280, 28, 28)
		$pen = New-Object System.Drawing.Pen (Color '#5b4fe0'), 16
		$pen.StartCap = 'Round'; $pen.EndCap = 'Round'; $pen.LineJoin = 'Round'
		$g.DrawLine($pen, 152, 356, 328, 356)
		$g.DrawLines($pen, [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(308, 336), [System.Drawing.PointF]::new(334, 356), [System.Drawing.PointF]::new(308, 376)))
		$g.DrawLines($pen, [System.Drawing.PointF[]]@([System.Drawing.PointF]::new(178, 336), [System.Drawing.PointF]::new(152, 356), [System.Drawing.PointF]::new(178, 376)))
	}
	$g.Dispose()
	return $bmp
}

function PngBytes($bmp) {
	$ms = New-Object System.IO.MemoryStream
	$bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
	return , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$images = @()
foreach ($sz in $sizes) {
	$bmp = Render $sz
	$images += , (PngBytes $bmp)
	if ($sz -eq 256) { $bmp.Save("$PSScriptRoot\logo-256.png", [System.Drawing.Imaging.ImageFormat]::Png) }
	$bmp.Dispose()
}

# ICO: ICONDIR, one ICONDIRENTRY per size, then the PNG data (Vista and later)
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
	$dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
	$w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
	$w.Write([uint16]1); $w.Write([uint16]32)
	$w.Write([uint32]$images[$i].Length); $w.Write([uint32]$offset)
	$offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
[System.IO.File]::WriteAllBytes("$PSScriptRoot\winapfs.ico", $out.ToArray())
Write-Output "wrote $PSScriptRoot\winapfs.ico ($($sizes -join ', ') px) and logo-256.png"

# Installer artwork (WiX UI): the dialog bitmap (493x312; the left 164 px show
# beside the welcome/licence and finish text) and the top banner (493x58).
function Bmp24([int]$w, [int]$h) {
	return New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
}
$logo = Render 256

$dlg = Bmp24 493 312
$g = [System.Drawing.Graphics]::FromImage($dlg)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'
$g.Clear([System.Drawing.Color]::White)
$side = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 0)), ([System.Drawing.PointF]::new(164, 312)), (Color '#16a3e0'), (Color '#5b4fe0')
$g.FillRectangle($side, 0, 0, 164, 312)
$g.DrawImage($logo, 22, 70, 120, 120)
$font = New-Object System.Drawing.Font 'Segoe UI Semibold', 15
$fmt = New-Object System.Drawing.StringFormat
$fmt.Alignment = 'Center'
$g.TextRenderingHint = 'AntiAliasGridFit'
$g.DrawString('winapfs', $font, [System.Drawing.Brushes]::White, [System.Drawing.RectangleF]::new(0, 200, 164, 32), $fmt)
$g.Dispose()
$dlg.Save("$PSScriptRoot\installer-dialog.bmp", [System.Drawing.Imaging.ImageFormat]::Bmp)

$ban = Bmp24 493 58
$g = [System.Drawing.Graphics]::FromImage($ban)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'
$g.Clear([System.Drawing.Color]::White)
$g.DrawImage($logo, 437, 5, 48, 48)
$g.Dispose()
$ban.Save("$PSScriptRoot\installer-banner.bmp", [System.Drawing.Imaging.ImageFormat]::Bmp)
$logo.Dispose()
Write-Output "wrote installer-dialog.bmp and installer-banner.bmp"
