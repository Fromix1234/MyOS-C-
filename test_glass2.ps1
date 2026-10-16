# Visual re-check after the fill_rect fix: installer labels, selected row, glass stacking.
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4578

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

$script:mxc = 512; $script:myc = 384                    # курсор ядра после загрузки — в центре
function Shot($n) { $w.WriteLine("screendump $d\$n.ppm"); Start-Sleep -Milliseconds 900 }
function Send($k) { $w.WriteLine("sendkey $k"); Start-Sleep -Milliseconds 350 }
function Kbd($s) { foreach ($ch in $s.ToCharArray()) { if ($ch -eq ' ') { Send 'spc' } elseif ($ch -eq '.') { Send 'dot' } elseif ($ch -eq '/') { Send 'slash' } else { Send "$ch" } } }
function RawMove($dx, $dy) {
    $n = [Math]::Ceiling([Math]::Max([Math]::Abs($dx), [Math]::Abs($dy)) / 40.0)
    if ($n -lt 1) { $n = 1 }
    $px = 0; $py = 0                            # cumulative step targets
    for ($i = 0; $i -lt $n; $i++) {
        $cx = [int][Math]::Round(($i + 1) * $dx / $n)
        $cy = [int][Math]::Round(($i + 1) * $dy / $n)
        $sx = $cx - $px; $sy = $cy - $py; $px = $cx; $py = $cy
        if ($sx -eq 0 -and $sy -eq 0) { continue }
        $w.WriteLine("mouse_move $sx $sy"); Start-Sleep -Milliseconds 80
    }
    $script:mxc += $dx; $script:myc += $dy
}
function MMoveTo($x, $y) { RawMove ($x - $script:mxc) ($y - $script:myc) }
function Click($x, $y) {
    MMoveTo $x $y
    $w.WriteLine('mouse_button 1'); Start-Sleep -Milliseconds 250
    $w.WriteLine('mouse_button 0'); Start-Sleep -Milliseconds 450
}

Start-Sleep -Seconds 3
Shot 'gx0'                                   # desktop

Send 'f3'                                    # console
Start-Sleep -Milliseconds 700
Kbd 'info'                                   # откуда загрузились (после установки в P2)
Send 'ret'
Start-Sleep -Milliseconds 900
Shot 'gx1'                                   # info: Boot should be P2

Kbd 'setup'                                  # graphical installer
Send 'ret'
Start-Sleep -Milliseconds 900
Shot 'gx2'                                   # list: P2 must show ACTIVE

Click 338 188                                # P3 row (row index 2) with the mouse
Shot 'gx3'                                   # selected row: white bar + BLACK text

Click 687 371                                # Install -> plan (no writing)
Shot 'gx4'                                   # confirm + highlighted Install button
Send 'esc'                                   # back to the list
Start-Sleep -Milliseconds 500
Send 'esc'                                   # close the installer
Start-Sleep -Milliseconds 700
Send 'f3'                                    # close the console
Start-Sleep -Milliseconds 700

Click 308 715                                # Terminal
Start-Sleep -Milliseconds 900
Click 580 715                                # Code
Start-Sleep -Milliseconds 900
Click 648 715                                # Game
Start-Sleep -Milliseconds 900
Click 376 715                                # Files
Start-Sleep -Milliseconds 1200
Shot 'gx5'                                   # windows stacked: glass over glass

$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($n in @('gx0','gx1','gx2','gx3','gx4','gx5')) { Convert-Ppm $d $n }
Write-Output 'done'
