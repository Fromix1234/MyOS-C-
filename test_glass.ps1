# Glass + dock icons + graphical installer: screenshots, real install, reboot check.
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4577

function Start-Q($p) {
    Start-Process -FilePath $qemu -ArgumentList @(
        '-drive', "format=raw,file=$d\disk.img",
        '-monitor', "tcp:127.0.0.1:$p,server,nowait"
    ) | Out-Null
    Start-Sleep -Seconds 2
    $cc = $null
    for ($i = 0; $i -lt 20; $i++) {
        try { $cc = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $p); break }
        catch { Start-Sleep -Milliseconds 1000 }
    }
    return $cc
}

$c = Start-Q $port
if (-not $c) { Write-Output 'no monitor'; exit 1 }
$st = $c.GetStream()
$w = New-Object System.IO.StreamWriter($st); $w.AutoFlush = $true

$script:mxc = 512; $script:myc = 384
function Shot($n) { $w.WriteLine("screendump $d\$n.ppm"); Start-Sleep -Milliseconds 900 }
function Send($k) { $w.WriteLine("sendkey $k"); Start-Sleep -Milliseconds 350 }
function Kbd($s) { foreach ($ch in $s.ToCharArray()) { if ($ch -eq ' ') { Send 'spc' } elseif ($ch -eq '.') { Send 'dot' } elseif ($ch -eq '/') { Send 'slash' } else { Send "$ch" } } }
function RawMove($dx, $dy) {
    $n = [Math]::Ceiling([Math]::Max([Math]::Abs($dx), [Math]::Abs($dy)) / 40.0)
    if ($n -lt 1) { $n = 1 }
    for ($i = 0; $i -lt $n; $i++) {
        $sx = [int][Math]::Round($dx / $n); $sy = [int][Math]::Round($dy / $n)
        if ($sx -eq 0 -and $sy -eq 0) { continue }
        $w.WriteLine("mouse_move $sx $sy"); Start-Sleep -Milliseconds 80
    }
    $script:mxc += $dx; $script:myc += $dy
}
function MMoveTo($x, $y) { RawMove ($x - $script:mxc) ($y - $script:myc) }
function Click($x, $y) {
    MMoveTo $x $y
    $w.WriteLine('mouse_button 1'); Start-Sleep -Milliseconds 250
    $w.WriteLine('mouse_button 0'); Start-Sleep -Milliseconds 400
}

Start-Sleep -Seconds 3
Shot 'gl0'                                   # desktop: glass dock + new icons + gradient

Send 'f3'                                    # console
Start-Sleep -Milliseconds 700
Kbd 'format 3'                               # MyFS on P3 (check it survives the install)
Send 'ret'
Start-Sleep -Seconds 2
Kbd 'write 3 keep my files'
Send 'ret'
Start-Sleep -Milliseconds 900
Kbd 'setup'                                  # graphical installer
Send 'ret'
Start-Sleep -Milliseconds 900
Shot 'gl1'                                   # installer window: partition list

Click 338 158                                # P2 row with the mouse
Shot 'gl2'                                   # P2 selected (white bar)

Click 687 371                                # Install -> show the plan
Shot 'gl3'                                   # confirm step (plan, nothing written yet)

Click 687 371                                # Install again -> WRITE
Start-Sleep -Seconds 3
Shot 'gl4'                                   # done: P2 ACTIVE

# --- reboot: MyOS must boot from P2 now, and MyFS on P3 must be intact ---
$w.WriteLine('quit'); Start-Sleep -Milliseconds 800
$c.Close()
Start-Sleep -Seconds 2
$c = Start-Q $port
if (-not $c) { Write-Output 'no monitor (reboot)'; exit 1 }
$st = $c.GetStream()
$w = New-Object System.IO.StreamWriter($st); $w.AutoFlush = $true
$script:mxc = 512; $script:myc = 384                    # курсор ядра после перезагрузки — в центре
Start-Sleep -Seconds 3

Shot 'gl5'                                   # booted from P2: desktop
Send 'f3'
Start-Sleep -Milliseconds 700
Kbd 'ls 3'
Send 'ret'
Start-Sleep -Milliseconds 900
Shot 'gl6'                                   # MyFS on P3 survived the install
Kbd 'setup'
Send 'ret'
Start-Sleep -Milliseconds 900
Shot 'gl7'                                   # installer: P2 shown as ACTIVE
Send 'esc'                                   # close installer
Start-Sleep -Milliseconds 700
Send 'f3'                                    # close console
Start-Sleep -Milliseconds 700

Click 398 740                                # Terminal
Start-Sleep -Milliseconds 800
Click 550 740                                # Code
Start-Sleep -Milliseconds 800
Click 588 740                                # Game
Start-Sleep -Milliseconds 800
Click 436 740                                # Files
Start-Sleep -Milliseconds 1000
Shot 'gl8'                                   # windows stacked: glass shows the window below

$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($n in @('gl0','gl1','gl2','gl3','gl4','gl5','gl6','gl7','gl8')) { Convert-Ppm $d $n }
Write-Output 'done'
