# Generates src\neonnet.ico (PNG-compressed entries, 16..256 px) with System.Drawing.
param([string]$Out = "$PSScriptRoot\..\src\neonnet.ico")
Add-Type -AssemblyName System.Drawing

function New-IconPng([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.Clear([System.Drawing.Color]::Transparent)
    $c = [Math]::Max(2, [int]($s * 0.22))
    $m = [Math]::Max(0.5, $s / 64.0)
    $pts = @(
        (New-Object System.Drawing.PointF $m, $m),
        (New-Object System.Drawing.PointF ($s - $c), $m),
        (New-Object System.Drawing.PointF ($s - $m), $c),
        (New-Object System.Drawing.PointF ($s - $m), ($s - $m)),
        (New-Object System.Drawing.PointF $c, ($s - $m)),
        (New-Object System.Drawing.PointF $m, ($s - $c))
    )
    $bg = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0x12, 0x06, 0x2c))
    $g.FillPolygon($bg, $pts)
    # sun stripe at the bottom
    $sun = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0xff, 0x9a, 0x3c))
    if ($s -ge 32) { $g.FillRectangle($sun, [float]($s * 0.18), [float]($s * 0.78), [float]($s * 0.5), [float]([Math]::Max(1, $s * 0.05))) }
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 0xff, 0x2e, 0x97)), ([float]([Math]::Max(1, $s / 16.0)))
    $g.DrawPolygon($pen, $pts)
    $fs = [float]($s * 0.68)
    $font = New-Object System.Drawing.Font 'Impact', $fs, ([System.Drawing.FontStyle]::Italic), ([System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = 'Center'; $sf.LineAlignment = 'Center'
    $rect = New-Object System.Drawing.RectangleF 0, ([float](-$s * 0.03)), $s, $s
    $off = [float]([Math]::Max(1, $s / 24.0))
    $shadow = New-Object System.Drawing.RectangleF $off, ([float](-$s * 0.03 + $off)), $s, $s
    $g.DrawString('N', $font, (New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0xff, 0x2e, 0x97))), $shadow, $sf)
    $g.DrawString('N', $font, (New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0x00, 0xe5, 0xff))), $rect, $sf)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$imgs = $sizes | ForEach-Object { ,(New-IconPng $_) }
$fsOut = [System.IO.File]::Create((Resolve-Path -LiteralPath (Split-Path $Out)).Path + '\' + (Split-Path $Out -Leaf))
$bw = New-Object System.IO.BinaryWriter $fsOut
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = $imgs[$i]
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$d.Length); $bw.Write([UInt32]$offset)
    $offset += $d.Length
}
foreach ($d in $imgs) { $bw.Write($d) }
$bw.Close()
Write-Host "icon written: $Out"
