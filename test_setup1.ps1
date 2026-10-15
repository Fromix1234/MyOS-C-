# Setup wizard test: list, invalid input, cancel, retry, install to P2
# (VESA 1024x768; PNG via shared ppm2png.ps1 helper)
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4555

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
Start-Sleep -Milliseconds 1500

function Send($k) { $w.WriteLine('sendkey ' + $k); Start-Sleep -Milliseconds 1200 }
function Shot($name) { $w.WriteLine("screendump $d\$name.ppm"); Start-Sleep -Milliseconds 900 }

Send 'f3'; Start-Sleep -Milliseconds 2000

# setup  (listing)
foreach ($k in @('s','e','t','u','p','ret')) { Send $k }
Shot 'wiz1'

# 9 + Enter (invalid), then empty Enter (cancel)
Send '9'; Send 'ret'
Send 'ret'
Shot 'wiz2'

# setup again -> install to 2
foreach ($k in @('s','e','t','u','p','ret')) { Send $k }
Send '2'; Send 'ret'
Shot 'wiz3'

# yes -> perform install
foreach ($k in @('y','e','s','ret')) { Send $k }
Start-Sleep -Milliseconds 2000
Shot 'wiz4'

$w.WriteLine('quit')
Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($name in @('wiz1','wiz2','wiz3','wiz4')) { Convert-Ppm $d $name }
Write-Output 'done'
