# After wizard install to P2: fresh boot must show "Boot: P2 (lba=18432)"
# (VESA 1024x768; PNG via shared ppm2png.ps1 helper)
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4560

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

$w.WriteLine('sendkey f3'); Start-Sleep -Milliseconds 2500
foreach ($k in @('i','n','f','o','ret')) {
    $w.WriteLine('sendkey ' + $k)
    Start-Sleep -Milliseconds 1100
}

$w.WriteLine("screendump $d\bootp2.ppm")
Start-Sleep -Milliseconds 900
$w.WriteLine('quit')
Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
Convert-Ppm $d 'bootp2'
Write-Output 'done'
