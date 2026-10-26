# ------------------------------------------------------------
# build_iso.ps1 — собрать загрузочный ISO из готового disk.img
# ------------------------------------------------------------

# Путь к проекту (по умолчанию текущий каталог)
$P = (Get-Location).Path

# Убедимся, что уже есть готовый disk.img (собранный build.ps1)
$diskPath = Join-Path $P 'disk.img'
if (-not (Test-Path $diskPath)) {
    Write-Host "ERROR: $diskPath не найден. Сначала запустите .\build.ps1" -ForegroundColor Red
    exit 1
}

# Создаём boot.img – образ floppy (1,44 MiB = 2880 секторов × 512 байт)
$bootImgPath = Join-Path $P 'boot.img'
$sectorSize = 512
$floppySectors = 2880               # 1 44 MiB
$bytesToCopy = $sectorSize * $floppySectors

$diskBytes = [IO.File]::ReadAllBytes($diskPath)
if ($diskBytes.Length -lt $bytesToCopy) {
    Write-Host "ERROR: disk.img слишком маленький (< $bytesToCopy байт)" -ForegroundColor Red
    exit 1
}
$bootBytes = $diskBytes[0..($bytesToCopy-1)]
[IO.File]::WriteAllBytes($bootImgPath, $bootBytes)

Write-Host "Создан $bootImgPath (1 44 MiB)" -ForegroundColor Green

# Формируем ISO
$isoPath = Join-Path $P 'MyOS.iso'

# 3а) Попытка использовать oscdimg (Windows ADK)
if (Get-Command oscdimg -ErrorAction SilentlyContinue) {
    Write-Host "Создаём ISO при помощи oscdimg…" -ForegroundColor Cyan
    oscdimg -b $bootImgPath -no-emul-boot -boot-load-size 4 -boot-info-table -o $isoPath $P
    if ($LASTEXITCODE -eq 0) {
        Write-Host "\nISO успешно создан: $isoPath" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "oscdimg завершился с ошибкой (code $LASTEXITCODE). Попробуем mkisofs…" -ForegroundColor Yellow
    }
}

# 3б) Попытка использовать mkisofs (genisoimage)
if (Get-Command mkisofs -ErrorAction SilentlyContinue) {
    Write-Host "Создаём ISO при помощи mkisofs…" -ForegroundColor Cyan
    mkisofs -b boot.img -no-emul-boot -boot-load-size 4 -boot-info-table -o $isoPath $P
    if ($LASTEXITCODE -eq 0) {
        Write-Host "\nISO успешно создан: $isoPath" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "mkisofs завершился с ошибкой (code $LASTEXITCODE). Попробуем xorriso…" -ForegroundColor Yellow
    }
}

# 3в) Попытка использовать xorriso (Linux/WSL)
if (Get-Command xorriso -ErrorAction SilentlyContinue) {
    Write-Host "Создаём ISO при помощи xorriso…" -ForegroundColor Cyan
    xorriso -as mkisofs -b boot.img -no-emul-boot -boot-load-size 4 -boot-info-table -o $isoPath $P
    if ($LASTEXITCODE -eq 0) {
        Write-Host "\nISO успешно создан: $isoPath" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "xorriso завершился с ошибкой (code $LASTEXITCODE)." -ForegroundColor Yellow
    }
}

Write-Host "\nERROR: Не найдено ни одной программы для создания ISO (oscdimg, mkisofs, xorriso)." -ForegroundColor Red
Write-Host "Установите одну из них (см. комментарии в скрипте) и запустите скрипт заново." -ForegroundColor Red
exit 1
