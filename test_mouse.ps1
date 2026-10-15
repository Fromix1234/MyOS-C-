# Cursor + multitasking check: several windows, raising, dragging, closing.
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4576

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

$script:mxc = 512; $script:myc = 384

function Shot($name) { $w.WriteLine("screendump $d\$name.ppm"); Start-Sleep -Milliseconds 900 }
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
function DragTo($x1, $y1, $x2, $y2) {
    MMoveTo $x1 $y1
    $w.WriteLine('mouse_button 1'); Start-Sleep -Milliseconds 300
    MMoveTo $x2 $y2
    Start-Sleep -Milliseconds 250
    $w.WriteLine('mouse_button 0'); Start-Sleep -Milliseconds 400
}

Start-Sleep -Seconds 2
Shot 'mt0'                                   # desktop + cursor in the centre

Click 398 740                                # Terminal (dock icon 0)
Start-Sleep -Milliseconds 900
Click 436 740                                # Files (icon 1) on top of Terminal
Start-Sleep -Milliseconds 900
Shot 'mt1'                                   # Files over the Terminal
Click 398 740                                # Terminal again - raise, text must stay
Start-Sleep -Milliseconds 900
Shot 'mt2'                                   # Terminal on top, its text intact
Click 588 740                                # Game (icon 5)
Start-Sleep -Milliseconds 900
Click 550 740                                # Code (icon 4)
Start-Sleep -Milliseconds 1200
Shot 'mt3'                                   # four windows at once

DragTo 400 153 600 400                       # drag the Code window by its title bar
Shot 'mt4'                                   # Code moved to (400,198)
Click 411 207                                # its red button - close
Start-Sleep -Milliseconds 900
Shot 'mt5'                                   # Code gone, others remain

$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($n in @('mt0','mt1','mt2','mt3','mt4','mt5')) { Convert-Ppm $d $n }
Write-Output 'done'
