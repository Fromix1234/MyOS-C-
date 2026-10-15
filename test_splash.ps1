# Splash screen check: shot during the splash and after it.
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4571

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
$w = New-Object System.IO.StreamWriter($st); $w.AutoFlush = $true

Start-Sleep -Milliseconds 700
$w.WriteLine("screendump $d\splash.ppm"); Start-Sleep -Milliseconds 600
$w.WriteLine("screendump $d\splash2.ppm"); Start-Sleep -Milliseconds 800
Start-Sleep -Seconds 2
$w.WriteLine("screendump $d\splash3.ppm"); Start-Sleep -Milliseconds 900
$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
Convert-Ppm $d 'splash'; Convert-Ppm $d 'splash2'; Convert-Ppm $d 'splash3'
Write-Output 'done'
