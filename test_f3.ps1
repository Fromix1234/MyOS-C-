# Тест докрутки CMD: time, mem, backspace, история (Up)
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4475

Start-Process -FilePath $qemu -ArgumentList @(
    '-drive', "format=raw,file=$d\disk.img",
    '-monitor', "tcp:127.0.0.1:$port,server,nowait"
) | Out-Null

Start-Sleep -Seconds 4

$c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $port)
$st = $c.GetStream()
$w = New-Object System.IO.StreamWriter($st)
$w.AutoFlush = $true
Start-Sleep -Milliseconds 1000

function Type-Keys([string[]]$keys) {
    foreach ($k in $keys) {
        $w.WriteLine('sendkey ' + $k)
        Start-Sleep -Milliseconds 150
    }
    Start-Sleep -Milliseconds 700
}

# 1) Открыть консоль
$w.WriteLine('sendkey f3')
Start-Sleep -Milliseconds 900

# 2) backspace-тест: мусор "zz", стираем его, потом настоящая команда.
#    Если backspace работает -> время; если нет -> "Unknown: zztime"
Type-Keys @('z','z','backspace','backspace')
# 3) time
Type-Keys @('t','i','m','e','ret')
# 4) mem
Type-Keys @('m','e','m','ret')
# 5) Up -> вспомнить последнюю команду ("mem"), оставить на экране
$w.WriteLine('sendkey up')
Start-Sleep -Milliseconds 700

$w.WriteLine("screendump $d\screen_gfx.ppm")
Start-Sleep -Milliseconds 700
$w.WriteLine('quit')
Start-Sleep -Milliseconds 500
$c.Close()

# PPM -> PNG
Add-Type -AssemblyName System.Drawing
$bytes = [System.IO.File]::ReadAllBytes("$d\screen_gfx.ppm")
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
$bmp.Save("$d\screen_gfx.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output 'done'
