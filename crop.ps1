# crop.ps1: cut a region out of a PNG screenshot and scale it up (for UI checks).
# Usage: powershell -File .\crop.ps1 -src gl3.png -dst crop1.png -x 130 -y 60 -w 650 -h 350 -scale 2
param([string]$src, [string]$dst, [int]$x, [int]$y, [int]$w, [int]$h, [int]$scale = 2)
Add-Type -AssemblyName System.Drawing
$d = 'D:\C++\MyOS'
$img = [System.Drawing.Image]::FromFile("$d\$src")
$bmp = New-Object System.Drawing.Bitmap (($w * $scale), ($h * $scale))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$rectD = New-Object System.Drawing.Rectangle 0, 0, ($w * $scale), ($h * $scale)
$rectS = New-Object System.Drawing.Rectangle $x, $y, $w, $h
$g.DrawImage($img, $rectD, $rectS, [System.Drawing.GraphicsUnit]::Pixel)
$bmp.Save("$d\$dst", [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $img.Dispose()
Write-Output "saved $dst"
