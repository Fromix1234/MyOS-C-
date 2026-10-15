# Dock apps test: Files (list + viewer), Settings (clock toggle), Tasks.
# Prepares MyFS files in P3 via console first, then walks the apps with screenshots.
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4566

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

# --- 0. Prepare MyFS test files on P3 (via console) ---
Key 'esc'                                    # warm-up (often lost)
Key 'f3'                                     # open console
TypeText 'format 3'; Key 'ret'
TypeText 'write 3 readme hello MyOS'; Key 'ret'
TypeText 'write 3 notes test file'; Key 'ret'
TypeText 'ls 3'; Key 'ret'
Shot 'apps_console'
Key 'f3'                                     # close console

# --- 1. Files: open, switch P1 -> P2 -> P3, view a file ---
Key 'right'                                  # dock: Terminal -> Files
Key 'ret'                                    # open Files
Key 'right'                                  # P1 -> P2
Key 'right'                                  # P2 -> P3
Shot 'apps_files'
Key 'ret'                                    # open readme in viewer
Shot 'apps_view'
Key 'esc'                                    # back to list
Key 'esc'                                    # close Files

# --- 2. Settings: open, toggle to 12-hour clock ---
Key 'right'                                  # dock: Files -> Settings
Key 'ret'                                    # open Settings
Shot 'apps_set'
Key 'right'                                  # toggle Clock 24h -> 12h
Shot 'apps_set12'
Key 'esc'                                    # close Settings

# --- 3. Tasks: open, let Uptime tick ---
Key 'right'                                  # dock: Settings -> Tasks
Key 'ret'                                    # open Task Manager
Start-Sleep -Milliseconds 2500
Shot 'apps_tasks'
Key 'esc'                                    # close Tasks

# --- 4. About (last dock slot) still works ---
Key 'right'                                  # dock: Tasks -> About
Key 'ret'                                    # open About
Shot 'apps_about'
Key 'esc'

$w.WriteLine('quit')
Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($name in @('apps_console','apps_files','apps_view','apps_set','apps_set12','apps_tasks','apps_about')) {
    Convert-Ppm $d $name
}
Write-Output 'done'
