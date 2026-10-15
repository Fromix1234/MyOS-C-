# Game steering + Code save to MyFS (needs a formatted partition).
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4573

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
function Fast($k) { $w.WriteLine("sendkey $k"); Start-Sleep -Milliseconds 250 }

Start-Sleep -Seconds 3        # пропустить заставку

# --- 1. Отформатировать P3 через консоль ---
Send 'f3'
1..2 | ForEach-Object { Start-Sleep -Milliseconds 400 }
foreach ($k in @('f','o','r','m','a','t','spc','3')) { Send $k }
Send 'ret'
Start-Sleep -Seconds 2
Send 'f3'
Start-Sleep -Seconds 1

# --- 2. Редактор кода: сохранить в P3 ---
1..4 | ForEach-Object { Send 'right' }
Send 'ret'
Start-Sleep -Milliseconds 800
Send 'f7'; Send 'f7'                       # раздел P1 -> P3
Send 'f2'                                  # открыть диалог сохранения
Start-Sleep -Milliseconds 400
foreach ($k in @('m','y','dot','m','y','c')) { Send $k }
Shot 's_name'                              # "Save as: my.myc_"
Send 'ret'
Start-Sleep -Milliseconds 900
Shot 's_saved'                             # DISK P3 my.myc ... + "Saved to MyFS"
Send 'esc'
Start-Sleep -Seconds 1

# --- 3. Управление змейкой ---
Send 'right'                               # док: Code -> Game
Send 'ret'
Fast 'up'
Start-Sleep -Milliseconds 900
Shot 's_game_up'                           # змейка идёт вверх
Fast 'left'
Start-Sleep -Milliseconds 900
Shot 's_game_left'                         # затем влево
Send 'esc'
Start-Sleep -Seconds 1
Shot 's_back'
$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($n in @('s_name','s_saved','s_game_up','s_game_left','s_back')) { Convert-Ppm $d $n }
Write-Output 'done'
