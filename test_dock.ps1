# Debug: where does it freeze when the mouse approaches the dock?
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4579

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
function Shot($n) { $w.WriteLine("screendump $d\$n.ppm"); Start-Sleep -Milliseconds 800 }
function RawMove($dx, $dy) {
    $n = [Math]::Ceiling([Math]::Max([Math]::Abs($dx), [Math]::Abs($dy)) / 40.0)
    if ($n -lt 1) { $n = 1 }
    $px = 0; $py = 0                            # cumulative step targets
    for ($i = 0; $i -lt $n; $i++) {
        $cx = [int][Math]::Round(($i + 1) * $dx / $n)
        $cy = [int][Math]::Round(($i + 1) * $dy / $n)
        $sx = $cx - $px; $sy = $cy - $py; $px = $cx; $py = $cy
        if ($sx -eq 0 -and $sy -eq 0) { continue }
        $w.WriteLine("mouse_move $sx $sy"); Start-Sleep -Milliseconds 150
    }
    $script:mxc += $dx; $script:myc += $dy
}
function MMoveTo($x, $y) { RawMove ($x - $script:mxc) ($y - $script:myc) }

Start-Sleep -Seconds 3
Shot 'd0'                                        # desktop, cursor in the center

MMoveTo 308 660                                  # just above the dock
Shot 'd1'                                        # still alive?

MMoveTo 308 715                                  # now ON the dock (icon 0)
Shot 'd2'                                        # alive? hover redraw?

$w.WriteLine('mouse_button 1'); Start-Sleep -Milliseconds 300
$w.WriteLine('mouse_button 0'); Start-Sleep -Milliseconds 600
Shot 'd3'                                        # click on Terminal -> console must open

MMoveTo 700 400                                  # away from the dock
Shot 'd4'                                        # still alive?

MMoveTo 376 715                                  # Files icon
$w.WriteLine('mouse_button 1'); Start-Sleep -Milliseconds 300
$w.WriteLine('mouse_button 0'); Start-Sleep -Milliseconds 600
Shot 'd5'                                        # Files window too

$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($n in @('d0','d1','d2','d3','d4','d5')) { Convert-Ppm $d $n }
Write-Output 'done'
