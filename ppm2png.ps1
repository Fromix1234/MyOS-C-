# Shared helper: PPM (P6) -> PNG with correct channel order.
# DIB-память GDI+ идёт как B,G,R, а PPM — R,G,B: небольшая C#-программа
# быстро конвертирует и меняет каналы (SetPixel в PowerShell слишком медленный
# для 1024x768). Подключается так:  . "$d\ppm2png.ps1"
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

function Convert-Ppm($dir, $name) {
    [PpmTool]::ToPng("$dir\$name.ppm", "$dir\$name.png")
    Write-Output "converted $name"
}
