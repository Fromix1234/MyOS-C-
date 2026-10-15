# MyC control flow test: types a program with while + if/else + comparison, compiles & runs.
# Expected RESULT = 107 (then-branch 7, then else-branch +100).
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4575

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

# named keys accepted by QEMU 11:
#   spc, semicolon, equal, minus, slash
#   shift-9 -> ( , shift-0 -> ) , shift-bracket_left -> { , shift-bracket_right -> }
#   shift-dot -> > , shift-comma -> < , shift-equal -> + , shift-8 -> *
$map = @{
    ' ' = 'spc';   ';' = 'semicolon'; '=' = 'equal';      '<' = 'shift-comma'; '>' = 'shift-dot';
    '{' = 'shift-bracket_left'; '}' = 'shift-bracket_right';
    '(' = 'shift-9'; ')' = 'shift-0';
    '+' = 'shift-equal'; '-' = 'minus'; '*' = 'shift-8';  '/' = 'slash';
}
function Kbd($ch) {
    if ($ch -eq "`n") { Send 'ret'; return }
    $code = [int][char]$ch
    if (($code -ge 97 -and $code -le 122) -or ($code -ge 48 -and $code -le 57)) { Send "$ch"; return }
    if ($map.ContainsKey("$ch")) { Send $map["$ch"]; return }
    Write-Output "UNMAPPED '$ch' ($code)"
}
function TypeText($s) { foreach ($ch in $s.ToCharArray()) { Kbd $ch } }

Start-Sleep -Seconds 2
1..4 | ForEach-Object { Send 'right' }         # dock: Terminal -> Code (index 4)
Send 'ret'
Start-Sleep -Milliseconds 800
Shot 'l_demo'                                  # example with while

$prog = "int s = 0;`n" +
        "int n = 2;`n" +
        "while (n > 0) {`n" +
        " s = s + n;`n" +
        " n = n - 1;`n" +
        "}`n" +
        "if (s == 3) {`n" +
        " s = 7;`n" +
        "} else {`n" +
        " s = 9;`n" +
        "}`n" +
        "if (1 < 0) {`n" +
        " s = 555;`n" +
        "} else {`n" +
        " s = s + 100;`n" +
        "}`n" +
        "return s;`n"
TypeText $prog
Shot 'l_typed'
Send 'f5'
Shot 'l_compiled'
Send 'f6'
Shot 'l_run'                                   # expect RESULT = 107
Send 'esc'
Start-Sleep -Seconds 1
Shot 'l_back'
$w.WriteLine('quit'); Start-Sleep -Milliseconds 500
$c.Close()

. "$d\ppm2png.ps1"
foreach ($n in @('l_demo','l_typed','l_compiled','l_run','l_back')) { Convert-Ppm $d $n }
Write-Output 'done'
