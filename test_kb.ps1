# Тест драйвера клавиатуры: шлём нажатия через монитор QEMU, делаем скриншот
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4460

Start-Process -FilePath $qemu -ArgumentList @(
    '-drive', "format=raw,file=$d\disk.img",
    '-monitor', "tcp:127.0.0.1:$port,server,nowait"
) | Out-Null

Start-Sleep -Seconds 3

$c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $port)
$st = $c.GetStream()
$w = New-Object System.IO.StreamWriter($st)
$w.AutoFlush = $true

# Печатаем "hello world"
foreach ($k in @('h','e','l','l','o','space','w','o','r','l','d')) {
    $w.WriteLine('sendkey ' + $k)
    Start-Sleep -Milliseconds 120
}
Start-Sleep -Milliseconds 500

$w.WriteLine("screendump $d\screen7.ppm")
Start-Sleep -Milliseconds 500
$w.WriteLine('quit')
Start-Sleep -Milliseconds 500
$c.Close()

# PPM -> PNG
Add-Type -AssemblyName System.Drawing
$bytes = [System.IO.File]::ReadAllBytes("$d\screen7.ppm")
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
$bmp.Save("$d\screen7.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output 'done'
