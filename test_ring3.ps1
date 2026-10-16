# Test: ring 3 (user mode) - run the user program from the console
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4580

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

function Shot($n) { $w.WriteLine("screendump $d\$n.ppm"); Start-Sleep -Milliseconds 800 }
function Send($k) { $w.WriteLine("sendkey $k"); Start-Sleep -Milliseconds 350 }
function Kbd($s) { foreach ($ch in $s.ToCharArray()) { if ($ch -eq ' ') { Send 'spc' } else { Send "$ch" } } }

Start-Sleep -Seconds 3
Send 'f3'                                    # open the console
Start-Sleep -Milliseconds 700
Kbd 'ring3'
Send 'ret'
Start-Sleep -Milliseconds 1500
Shot 'r0'                                    # user program running: pulsing square + messages

Send 'a'                                     # any key -> the program reports it
Start-Sleep -Milliseconds 800
Shot 'r1'

Send 'q'                                     # q -> SYS_EXIT, back to the kernel
Start-Sleep -Milliseconds 1200
Shot 'r2'                                    # console again, desktop redrawn

$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($n in @('r0','r1','r2')) { Convert-Ppm $d $n }
Write-Output 'done'
