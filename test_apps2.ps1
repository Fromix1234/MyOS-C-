# Granular diagnostic: one screenshot after EVERY single key press.
# Boot -> esc(f3 open/close warmup) -> then step-by-step dock navigation.
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4567

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
Start-Sleep -Milliseconds 2500

function Key($k) { $w.WriteLine("sendkey $k"); Start-Sleep -Milliseconds 1200 }
function Shot($name) { $w.WriteLine("screendump $d\$name.ppm"); Start-Sleep -Milliseconds 900 }

Key 'esc'          # warm-up (often lost after boot)
Shot 'g00_desktop'
Key 'f3'           # open console
Shot 'g01_console_open'
Key 'f3'           # close console
Shot 'g02_console_closed'
Key 'right'        # dock: expect Files
Shot 'g03_right1'
Key 'right'        # dock: expect Settings
Shot 'g04_right2'
Key 'right'        # dock: expect Tasks
Shot 'g05_right3'
Key 'right'        # dock: expect About
Shot 'g06_right4'
Key 'left'         # back to Tasks
Shot 'g07_left'
Key 'ret'          # open Tasks
Shot 'g08_tasks_open'
Key 'esc'          # close
Shot 'g09_closed'
Key 'right'        # About again
Key 'ret'          # open About
Shot 'g10_about_open'
Key 'esc'
Shot 'g11_final'

$w.WriteLine('quit')
Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($name in @('g00_desktop','g01_console_open','g02_console_closed','g03_right1','g04_right2',
                    'g05_right3','g06_right4','g07_left','g08_tasks_open','g09_closed',
                    'g10_about_open','g11_final')) {
    Convert-Ppm $d $name
}
Write-Output 'done'
