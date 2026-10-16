# Splash check: the VESA splash lives ~1.4 s and QEMU boot timing varies.
# So we take a dense burst of frames (200 ms step, ~6 s span): the splash
# is guaranteed to be visible in ~7 consecutive frames.
# The kernel also prints "splash-exit: key=... wait=...ms" to COM1 (serial):
# we use it to verify the full splash duration automatically.
# NOTE: keep this script ASCII-only (PowerShell 5.1 reads .ps1 as ANSI).
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4571

Remove-Item "$d\_serial.log" -Force -ErrorAction SilentlyContinue
Start-Process -FilePath $qemu -ArgumentList @(
    '-drive', "format=raw,file=$d\disk.img",
    '-monitor', "tcp:127.0.0.1:$port,server,nowait",
    '-serial', "file:$d\_serial.log"
) | Out-Null
Start-Sleep -Milliseconds 200
$c = $null
for ($i = 0; $i -lt 20; $i++) {
    try { $c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $port); break }
    catch { Start-Sleep -Milliseconds 500 }
}
if (-not $c) { Write-Output 'no monitor'; exit 1 }
$st = $c.GetStream()
$w = New-Object System.IO.StreamWriter($st); $w.AutoFlush = $true

# Frame burst: splash_00..splash_29 (0..6 s from connect) + desktop frame.
for ($i = 0; $i -lt 30; $i++) {
    $n = 'splash_{0:d2}' -f $i
    $w.WriteLine("screendump $d\$n.ppm")
    Start-Sleep -Milliseconds 200
}
$w.WriteLine("screendump $d\splash_desk.ppm"); Start-Sleep -Milliseconds 600
$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
for ($i = 0; $i -lt 30; $i++) { Convert-Ppm $d ('splash_{0:d2}' -f $i) }
Convert-Ppm $d 'splash_desk'

Write-Output '--- serial ---'
Select-String -Path "$d\_serial.log" -Pattern 'boot:|splash-exit' | ForEach-Object { $_.Line }
$m = Select-String -Path "$d\_serial.log" -Pattern 'splash-exit: key=none wait=(\d+)ms'
if ($m -and [int]$m.Matches[0].Groups[1].Value -ge 1300) {
    Write-Output 'SPLASH OK: full duration reached'
} else {
    Write-Output 'SPLASH CHECK: exited early -- see splash-exit line above'
}
Write-Output 'Frames: splash_00..splash_29.png (splash ~1.4 s), splash_desk.png'
Write-Output 'done'
