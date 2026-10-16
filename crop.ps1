param([string]$src, [int]$x, [int]$y, [int]$w, [int]$h, [string]$dst, [int]$scale = 2)
Add-Type -AssemblyName System.Drawing
$b = [System.Drawing.Bitmap]::FromFile($src)
$r = New-Object System.Drawing.Rectangle($x, $y, $w, $h)
$c = $b.Clone($r, $b.PixelFormat)
$s = New-Object System.Drawing.Bitmap($c, ($w * $scale), ($h * $scale))
$s.Save($dst)
$s.Dispose(); $c.Dispose(); $b.Dispose()
Write-Output "cropped $dst"
