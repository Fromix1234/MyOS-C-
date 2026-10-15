# Desktop test: menubar+dock screenshot, arrow navigation, About window, Esc, F3 console
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4545

Start-Process -FilePath $qemu -ArgumentList @(
    '-drive', "format=raw,file=$d\disk.img",
    '-monitor', "tcp:127.0.0.1:$port,server,nowait"
) | Out-Null

Start-Sleep -Seconds 2

$c = $null
for ($i = 0; $i -lt 20; $i++) {
    try { $c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $port); break }
    catch { Start-Sleep -Milliseconds 1000 }
}
if (-not $c) { Write-Output 'no monitor'; exit 1 }
$st = $c.GetStream()
$w = New-Object System.IO.StreamWriter($st)
$w.AutoFlush = $true
Start-Sleep -Milliseconds 1500

function Shot($name) {
    $w.WriteLine("screendump $d\$name.ppm")
    Start-Sleep -Milliseconds 700
}

# 1. boot desktop (Terminal selected)
Shot 'st1'

# 2. Right x2 (idempotent: clamps at last icon) -> About selected
$w.WriteLine('sendkey right'); Start-Sleep -Milliseconds 1000
$w.WriteLine('sendkey right'); Start-Sleep -Milliseconds 1000
Shot 'st2'

# 3. Enter -> About window opens
$w.WriteLine('sendkey ret'); Start-Sleep -Milliseconds 1000
Shot 'st3'

# 4. Esc -> back to desktop
$w.WriteLine('sendkey esc'); Start-Sleep -Milliseconds 1000
Shot 'st4'

# 5. Left x2 -> Terminal, F3 -> console, info
$w.WriteLine('sendkey left'); Start-Sleep -Milliseconds 1000
$w.WriteLine('sendkey left'); Start-Sleep -Milliseconds 1000
$w.WriteLine('sendkey f3');   Start-Sleep -Milliseconds 1500
foreach ($k in @('i','n','f','o','ret')) {
    $w.WriteLine('sendkey ' + $k)
    Start-Sleep -Milliseconds 1000
}
Shot 'st5'

$w.WriteLine('quit')
Start-Sleep -Milliseconds 500
$c.Close()

# PPM -> PNG for all shots
Add-Type -AssemblyName System.Drawing
foreach ($name in @('st1','st2','st3','st4','st5')) {
    $bytes = [System.IO.File]::ReadAllBytes("$d\$name.ppm")
    $nl = 0; $hE = 0
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 10) { $nl++; if ($nl -eq 3) { $hE = $i + 1; break } }
    }
    $t = [System.Text.Encoding]::ASCII.GetString($bytes[0..($hE-1)])
    $ln = ($t.Trim() -split "`n")
    $dim = ($ln[1]).Trim() -split ' '
    $w2 = [int]$dim[0]; $h2 = [int]$dim[1]
    $bmp = New-Object System.Drawing.Bitmap($w2, $h2)
    for ($y = 0; $y -lt $h2; $y++) {
        for ($x = 0; $x -lt $w2; $x++) {
            $idx = $hE + ($y*$w2 + $x)*3
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($bytes[$idx], $bytes[$idx+1], $bytes[$idx+2]))
        }
    }
    $bmp.Save("$d\$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
Write-Output 'done'
