# VESA test: resolution check + desktop + About + console info, fast PPM->PNG
$qemu = 'C:\Program Files\qemu\qemu-system-i386.exe'
$d = 'D:\C++\MyOS'
$port = 4561

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

function Shot($name) {
    $w.WriteLine("screendump $d\$name.ppm")
    Start-Sleep -Milliseconds 900
}

# warm-up key (first key after boot is often lost), then desktop shot
$w.WriteLine('sendkey esc'); Start-Sleep -Milliseconds 1000
Shot 'vesa1'

# Right x2 (idempotent, clamps) -> About selected; Enter opens About
$w.WriteLine('sendkey right'); Start-Sleep -Milliseconds 1000
$w.WriteLine('sendkey right'); Start-Sleep -Milliseconds 1000
$w.WriteLine('sendkey ret');   Start-Sleep -Milliseconds 1200
Shot 'vesa2'

# Esc closes About; Left x2 -> Terminal; F3 opens console; run info + mem
$w.WriteLine('sendkey esc');  Start-Sleep -Milliseconds 1000
$w.WriteLine('sendkey left'); Start-Sleep -Milliseconds 1000
$w.WriteLine('sendkey left'); Start-Sleep -Milliseconds 1000
$w.WriteLine('sendkey f3');   Start-Sleep -Milliseconds 1800
foreach ($k in @('i','n','f','o','ret','m','e','m','ret')) {
    $w.WriteLine('sendkey ' + $k)
    Start-Sleep -Milliseconds 250
}
Start-Sleep -Milliseconds 800
Shot 'vesa3'

$w.WriteLine('quit')
Start-Sleep -Milliseconds 500
$c.Close()

# --- PPM -> PNG (быстро: C#-помощник; каналы DIB идут B,G,R, поэтому меняем R<->B) ---
Add-Type -AssemblyName System.Drawing
$cs = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
public static class PpmTool {
    public static void ToPng(string ppmPath, string pngPath) {
        byte[] b = File.ReadAllBytes(ppmPath);
        int nl = 0, hE = 0;
        for (int i = 0; i < b.Length; i++) { if (b[i] == 10) { nl++; if (nl == 3) { hE = i + 1; break; } } }
        string header = System.Text.Encoding.ASCII.GetString(b, 0, hE);
        string[] parts = header.Trim().Split('\n');
        string[] dim = parts[1].Trim().Split(' ');
        int w = int.Parse(dim[0]), h = int.Parse(dim[1]);
        using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb)) {
            Rectangle rect = new Rectangle(0, 0, w, h);
            BitmapData data = bmp.LockBits(rect, ImageLockMode.WriteOnly, bmp.PixelFormat);
            byte[] row = new byte[w * 3];
            long ptr = data.Scan0.ToInt64();
            for (int y = 0; y < h; y++) {
                int src = hE + y * w * 3;
                for (int x = 0; x < w; x++) {
                    row[x * 3]     = b[src + x * 3 + 2]; // B
                    row[x * 3 + 1] = b[src + x * 3 + 1]; // G
                    row[x * 3 + 2] = b[src + x * 3];     // R
                }
                Marshal.Copy(row, 0, new IntPtr(ptr + y * data.Stride), w * 3);
            }
            bmp.UnlockBits(data);
            bmp.Save(pngPath, ImageFormat.Png);
        }
    }
}
'@
Add-Type -TypeDefinition $cs -ReferencedAssemblies System.Drawing

foreach ($name in @('vesa1','vesa2','vesa3')) {
    [PpmTool]::ToPng("$d\$name.ppm", "$d\$name.png")
    $img = [System.Drawing.Image]::FromFile("$d\$name.png")
    Write-Output ("$name : " + $img.Width + "x" + $img.Height)
    $img.Dispose()
}
Write-Output 'done'
