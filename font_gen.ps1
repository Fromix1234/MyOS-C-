# Генератор растрового шрифта 8x16 для ядра (формат C-массива).
# Каждый символ рисуется в ШИРОКИЙ холст (без обрезки), затем глиф
# аккуратно укладывается в ячейку 8x16 по общей базовой линии.
Add-Type -AssemblyName System.Drawing

$font     = New-Object System.Drawing.Font("Consolas", 10)  # моноширинный, ~7.3px шаг
$Xstart   = 4          # где стоит "перо" (левая граница знакоместа)
$Ytop     = 4          # верх строки при рендере
$big      = 48         # размер большого холста (чтобы ничего не обрезалось)
$cellW    = 8
$cellH    = 16
$baseTgt  = 12         # куда кладём базовую линию в ячейке (строка 12)

function Get-Grid($font, $ch) {
    $bmp = New-Object System.Drawing.Bitmap($big, $big)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Black)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
    $g.DrawString($ch, $font, [System.Drawing.Brushes]::White, $Xstart, $Ytop)
    $g.Dispose()
    $grid = New-Object 'bool[,]' $big, $big
    for ($y = 0; $y -lt $big; $y++) {
        for ($x = 0; $x -lt $big; $x++) {
            if ($bmp.GetPixel($x, $y).R -gt 110) { $grid[$x, $y] = $true }
        }
    }
    $bmp.Dispose()
    return ,$grid          # запятая = НЕ разворачивать двумерный массив в конвейере
}

# 1) Базовая линия = низ буквы "A" (она стоит на базовой линии)
$ga = Get-Grid $font "A"
$baselineSrc = -1
for ($y = $big - 1; $y -ge 0; $y--) {
    for ($x = 0; $x -lt $big; $x++) { if ($ga[$x, $y]) { $baselineSrc = $y; break } }
    if ($baselineSrc -ge 0) { break }
}

# 2) Собираем глифы ASCII 32..126
$out = New-Object System.Collections.Generic.List[string]
$maxRight = 0
$worst = ""
foreach ($code in 32..126) {
    $grid = Get-Grid $font ([string][char]$code)
    $rows = New-Object int[] $cellH   # заполнено нулями

    $minx = 1e9; $maxx = -1; $miny = 1e9; $maxy = -1
    for ($y = 0; $y -lt $big; $y++) {
        for ($x = 0; $x -lt $big; $x++) {
            if ($grid[$x, $y]) {
                if ($x -lt $minx) { $minx = $x }
                if ($x -gt $maxx) { $maxx = $x }
                if ($y -lt $miny) { $miny = $y }
                if ($y -gt $maxy) { $maxy = $y }
            }
        }
    }

    if ($maxx -ge 0) {
        $gw = $maxx - $minx + 1                     # ширина глифа в пикселях
        if ($gw -gt $maxRight) { $maxRight = $gw; $worst = "code=$code w=$gw" }
        for ($sy = $miny; $sy -le $maxy; $sy++) {
            $cy = $sy - $baselineSrc + $baseTgt          # перенос по базовой линии
            if ($cy -lt 0 -or $cy -ge $cellH) { continue }
            for ($sx = $minx; $sx -le $maxx; $sx++) {
                if ($grid[$sx, $sy]) {
                    $cx = $sx - $minx                    # левый край глифа -> колонка 0
                    if ($cx -ge 0 -and $cx -lt $cellW) {
                        $rows[$cy] = $rows[$cy] -bor (0x80 -shr $cx)  # бит7 = левый пиксель
                    }
                }
            }
        }
    }

    $hex = ($rows | ForEach-Object { '0x{0:X2}' -f $_ }) -join ', '
    $out.Add(('    {' + $hex + '},  // ' + $code))
}

# Пишем готовый фрагмент C-кода, который ядро подключит через #include
$wrap = @("const unsigned char font8x16[95][16] = {") + $out + @("};")
$wrap | Out-File -Encoding ASCII "D:\C++\MyOS\font_data.inc"
Write-Output "OK glyphs=$($out.Count) baselineSrc=$baselineSrc maxWidth=$maxRight (must be <= $cellW) worst=[$worst]"
