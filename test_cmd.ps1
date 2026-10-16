# Console commands: code / play launch apps; MyFS still works (format/write/ls).
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4574

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

function Shot($n) { $w.WriteLine("screendump $d\$n.ppm"); Start-Sleep -Milliseconds 900 }
function Send($k) { $w.WriteLine("sendkey $k"); Start-Sleep -Milliseconds 1100 }
function Kbd($s) { foreach ($ch in $s.ToCharArray()) { if ($ch -eq ' ') { Send 'spc' } elseif ($ch -eq '.') { Send 'dot' } elseif ($ch -eq '/') { Send 'slash' } else { Send "$ch" } } }

Start-Sleep -Seconds 3

Send 'f3'                                  # консоль
Kbd 'help'
Send 'ret'
Start-Sleep -Milliseconds 700
Shot 'c_help'                              # в списке есть play и code
Kbd 'code'
Send 'ret'
Start-Sleep -Milliseconds 800
Shot 'c_code'                              # редактор по команде code
Send 'esc'
Start-Sleep -Seconds 1

# После Esc фокус сам возвращается в консоль (она всё ещё открыта) - печатаем туда
Kbd 'play'
Send 'ret'
Start-Sleep -Milliseconds 3400             # ждём статичный GAME OVER (змейка врезалась)
Shot 'c_play'                              # игра по команде play
Send 'esc'
Start-Sleep -Seconds 1

Kbd 'format 3'
Send 'ret'
Start-Sleep -Seconds 2
Kbd 'write 3 hello world'
Send 'ret'
Start-Sleep -Milliseconds 800
Kbd 'ls 3'
Send 'ret'
Start-Sleep -Milliseconds 800
Shot 'c_ls'                                # MyFS работает
$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($n in @('c_help','c_code','c_play','c_ls')) { Convert-Ppm $d $n }
Write-Output 'done'
