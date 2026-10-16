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

Click 308 715                                # Terminal (dock icon 0)
Start-Sleep -Milliseconds 900
Click 376 715                                # Files (icon 1) on top of Terminal
Start-Sleep -Milliseconds 900
Shot 'mt1'                                   # Files over the Terminal
Click 308 715                                # Terminal again - raise, text must stay
Start-Sleep -Milliseconds 900
Shot 'mt2'                                   # Terminal on top, its text intact
Click 648 715                                # Game (icon 5)
Start-Sleep -Milliseconds 900
Click 580 715                                # Code (icon 4)
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
