# Splash + Snake + Code editor: boot, play a bit, then compile & run MyC code.
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4572

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
function Send($k) { $w.WriteLine("sendkey $k"); Start-Sleep -Milliseconds 1200 }

Shot 'i_logo'                                  # заставка
Start-Sleep -Seconds 2
Shot 'i_desktop'                               # рабочий стол
1..5 | ForEach-Object { Send 'right' }         # док: Terminal..Game
Send 'ret'
Start-Sleep -Milliseconds 800
Shot 'i_game'                                  # игра
Send 'up'; Send 'right'
Start-Sleep -Milliseconds 1500
Shot 'i_game2'
Send 'esc'
Start-Sleep -Seconds 1
Send 'left'                                    # док: Game -> Code
Send 'ret'
Start-Sleep -Milliseconds 800
Shot 'i_code'                                  # редактор с примером
foreach ($k in @('r','e','t','u','r','n','spc','a','semicolon')) { Send $k }
Shot 'i_typed'                                 # набрали return a;
Send 'f5'
Shot 'i_compiled'                              # компиляция
Send 'f6'
Shot 'i_run'                                   # запуск: регистры и результат
Send 'esc'
Start-Sleep -Seconds 1
Shot 'i_back'                                  # назад на рабочий стол
$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($n in @('i_logo','i_desktop','i_game','i_game2','i_code','i_typed','i_compiled','i_run','i_back')) {
    Convert-Ppm $d $n
}
Write-Output 'done'
