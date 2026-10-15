# Files app as a real file manager: format, create, rename, delete, view - all via GUI.
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4570

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
function TypeText($s) {
    foreach ($ch in $s.ToCharArray()) {
        if ($ch -eq ' ') { Key 'spc'; continue }
        if ($ch -cmatch '[A-Z]') { Key ('shift-' + $ch.ToString().ToLower()); continue }
        Key ([string]$ch)
    }
}

Key 'esc'                                  # warm-up
Key 'right'; Key 'ret'                     # Files -> open (P1: Not MyFS)
Key 'right'; Key 'right'                   # -> P3
Shot 'f01_notmyfs'
Key 'f'                                    # format confirm
Shot 'f02_fmtconf'
Key 'y'                                    # format
Shot 'f03_formatted'
Key 'n'; TypeText 'todox'; Key 'backspace' # name with backspace test
Shot 'f04_name'
Key 'ret'; TypeText 'buy milk'             # text
Shot 'f05_text'
Key 'ret'                                  # create
Shot 'f06_created'
Key 'n'; TypeText 'Note'; Key 'ret'        # second file (shift test)
TypeText 'test file'; Key 'ret'
Shot 'f07_two'
Key 'r'; TypeText 'list'; Key 'ret'        # rename selected (Note) -> list
Shot 'f08_renamed'
Key 'd'                                    # delete confirm
Shot 'f09_delconf'
Key 'y'                                    # delete
Shot 'f10_deleted'
Key 'ret'                                  # view todo
Shot 'f11_view'
Key 'esc'; Key 'esc'                       # back, close -> desktop
Key 'f3'                                   # console: cross-check
TypeText 'ls 3'; Key 'ret'
Shot 'f12_console'

$w.WriteLine('quit')
Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
$names = @('f01_notmyfs','f02_fmtconf','f03_formatted','f04_name','f05_text','f06_created',
           'f07_two','f08_renamed','f09_delconf','f10_deleted','f11_view','f12_console')
foreach ($name in $names) { Convert-Ppm $d $name }
Write-Output 'done'
