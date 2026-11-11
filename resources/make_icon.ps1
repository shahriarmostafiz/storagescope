# Generates resources\app.ico (PNG frames 256/48/32/16) with System.Drawing. Run: powershell -File make_icon.ps1
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot "app.ico"

function New-Frame([int]$S) {
  $bmp = New-Object System.Drawing.Bitmap 256, 256, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'; $g.Clear([System.Drawing.Color]::Transparent)
  function RR($x,$y,$w,$h,$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x,$y,2*$r,2*$r,180,90); $p.AddArc($x+$w-2*$r,$y,2*$r,2*$r,270,90)
    $p.AddArc($x+$w-2*$r,$y+$h-2*$r,2*$r,2*$r,0,90); $p.AddArc($x,$y+$h-2*$r,2*$r,2*$r,90,90); $p.CloseFigure(); $p }
  # glass body
  $body = RR 14 14 228 228 58
  $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Point]::new(0,0)), ([System.Drawing.Point]::new(256,256)), ([System.Drawing.Color]::FromArgb(255,120,240,200)), ([System.Drawing.Color]::FromArgb(255,0,112,96))
  $g.FillPath($bg, $body)
  # sheen (top half)
  $sheenPath = RR 20 20 216 120 52
  $sheen = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Point]::new(0,20)), ([System.Drawing.Point]::new(0,140)), ([System.Drawing.Color]::FromArgb(120,255,255,255)), ([System.Drawing.Color]::FromArgb(0,255,255,255))
  $g.FillPath($sheen, $sheenPath)
  # usage ring
  $pen1 = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(245,255,255,255)), 26
  $pen1.StartCap = 'Round'; $pen1.EndCap = 'Round'
  $pen2 = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(85,255,255,255)), 26
  $g.DrawArc($pen2, 66, 66, 124, 124, 0, 360)
  $g.DrawArc($pen1, 66, 66, 124, 124, -90, 250)
  # centre dot
  $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(230,255,255,255))), 116, 116, 24, 24)
  # rim light
  $rim = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150,255,255,255)), 3
  $g.DrawPath($rim, $body)
  $g.Dispose()
  if ($S -eq 256) { return $bmp }
  $r = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g2 = [System.Drawing.Graphics]::FromImage($r); $g2.InterpolationMode = 'HighQualityBicubic'; $g2.PixelOffsetMode = 'HighQuality'
  $g2.DrawImage($bmp, 0, 0, $S, $S); $g2.Dispose(); $bmp.Dispose(); return $r
}

$sizes = 256, 48, 32, 16
$pngs = foreach ($s in $sizes) { $b = New-Frame $s; $ms = New-Object System.IO.MemoryStream; $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose(); ,$ms.ToArray() }
$fs = [System.IO.File]::Create($out); $w = New-Object System.IO.BinaryWriter $fs
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]; $dim = if ($s -ge 256) { 0 } else { $s }
  $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
  $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
  $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Close(); $fs.Close()
"wrote $out ($((Get-Item $out).Length) bytes)"
