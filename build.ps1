# ===========================================================================
#  build.ps1 — сборка ОС целиком: ядро (ring 0) + оболочка (ring 3) + диск
# ===========================================================================
#  Архитектура (v3, Zen/Linux-like):
#    - kernel.bin (ring 0): Rust kernel-rs (флаг -Rust) или C++ kernel/ (по умолчанию);
#    - shell.bin  (ring 3): весь рабочий стол/консоль/приложения, живёт на MyFS;
#    - demo.bin   (ring 3): тестовая программа, запускается командой 'ring3'.
#
#  Использование:
#      .\build.ps1               # C++-ядро + оболочка + disk.img
#      .\build.ps1 -Rust         # Rust-ядро (нужен nightly + build-std)
#      .\build.ps1 -NoRun        # только собрать, без напоминания про VirtualBox
#      .\build.ps1 -Clean        # сначала удалить прошлые артефакты
#
#  Пути к инструментам ищутся автоматически (PATH, MSYS2, winget-каталоги)
#  и могут быть заданы вручную переменными окружения:
#      MYOS_NASM, MYOS_GXX, MYOS_LD, MYOS_OBJCOPY, MYOS_NM, MYOS_CARGO
# ===========================================================================
param(
    [switch]$Rust,
    [switch]$NoRun,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$P = $PSScriptRoot                     # каталог проекта (где лежит этот скрипт)
Set-Location $P

# ---------------------------------------------------------------------------
#  Поиск инструментов: переменная окружения -> PATH -> типовые места MSYS2
# ---------------------------------------------------------------------------
function Resolve-Tool {
    param([string]$Name, [string]$EnvName, [string[]]$Candidates, [string]$Hint)

    $custom = [Environment]::GetEnvironmentVariable($EnvName)
    if ($custom) {
        if (Test-Path $custom) { return $custom }
        Write-Host "ERROR: $EnvName=$custom — файл не найден" -ForegroundColor Red
        exit 1
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($c in $Candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    Write-Host "ERROR: не найден $Name. $Hint" -ForegroundColor Red
    Write-Host "       Можно задать путь вручную: `$env:$EnvName = 'C:\...\$Name'" -ForegroundColor Yellow
    exit 1
}

$msysCandidates = @(
    "C:\msys64\mingw32\bin",
    "C:\msys64\msys64\mingw32\bin",
    "C:\msys64\usr\bin"
)
function First-Existing {
    param([string]$Leaf)
    foreach ($d in $msysCandidates) {
        $f = Join-Path $d $Leaf
        if (Test-Path $f) { return $f }
    }
    return $null
}

$NASM    = Resolve-Tool "nasm.exe"    "MYOS_NASM"    @((First-Existing "nasm.exe"))              "Установите NASM (winget install NASM.NASM)."
$GXX     = Resolve-Tool "g++.exe"     "MYOS_GXX"     @((First-Existing "g++.exe"))               "Нужен i686 MinGW-w64: pacman -S mingw-w64-i686-gcc (MSYS2)."
$LD      = Resolve-Tool "ld.exe"      "MYOS_LD"      @((First-Existing "ld.exe"))                "Нужен binutils i686 (идёт вместе с mingw-w64-i686-gcc)."
$OBJCOPY = Resolve-Tool "objcopy.exe" "MYOS_OBJCOPY" @((First-Existing "objcopy.exe"))           "Нужен binutils i686."
$NM      = Resolve-Tool "nm.exe"      "MYOS_NM"      @((First-Existing "nm.exe"))                "Нужен binutils i686."
$BIN     = Split-Path $GXX -Parent
# cc1plus.exe живёт рядом с g++.exe и ищет свои DLL в PATH.
$env:Path = "$BIN;$env:Path"

Write-Host "tools: nasm=$NASM" -ForegroundColor DarkGray
Write-Host "       gcc =$GXX" -ForegroundColor DarkGray

if ($Clean) {
    Write-Host "=== -Clean: удаляю прошлые артефакты ===" -ForegroundColor Cyan
    foreach ($f in @("boot.bin", "vbr.bin", "stage2.bin", "kernel.bin", "kernel.exe",
                     "kernel.o", "kernel_clean.o", "disk.img", "user_demo.bin",
                     "shell\shell.bin", "shell\shell.exe", "shell\shell.o",
                     "shell\shell_clean.o")) {
        Remove-Item -Force (Join-Path $P $f) -ErrorAction SilentlyContinue
    }
}

# Любая внешняя команда обязана завершиться успешно — иначе сборка «прошла»,
# а в диск уехал прошлый бинарник (именно так ловились «загадочные» баги).
function Invoke-Tool {
    param([string]$What, [string]$Exe, [string[]]$ToolArgs)
    & $Exe @ToolArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: $What (код $LASTEXITCODE)" -ForegroundColor Red
        exit 1
    }
}
function Assert-File {
    param([string]$Path, [string]$What)
    if (-not (Test-Path $Path)) {
        Write-Host "ERROR: $What — файл не создан: $Path" -ForegroundColor Red
        exit 1
    }
}

$CXXFLAGS = @("-m32", "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-pie",
              "-fno-stack-protector", "-nostdlib", "-nostartfiles", "-mno-sse",
              "-mno-mmx", "-mno-80387", "-fno-asynchronous-unwind-tables",
              "-Os", "-fno-strict-aliasing", "-Wall", "-Wextra",
              "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable")

function Remove-Junk {
    param([string]$Obj, [string]$Clean)
    Invoke-Tool "objcopy --remove-section" $OBJCOPY @(
        "--remove-section=.comment", "--remove-section=.note",
        "--remove-section=.note.GNU-stack", $Obj, $Clean)
}

# ---------------------------------------------------------------------------
Write-Host "=== 0. Building ring 3 program (user_demo.asm) ===" -ForegroundColor Cyan
Invoke-Tool "nasm user_demo.asm" $NASM @("-f", "bin", "$P\user_demo.asm", "-o", "$P\user_demo.bin")
$demo = [System.IO.File]::ReadAllBytes("$P\user_demo.bin")
Write-Host "    user_demo.bin = $($demo.Length) bytes (ring 3 program, org 0x500000)" -ForegroundColor Yellow

# ---------------------------------------------------------------------------
Write-Host "=== 1. Building the GUI shell (shell/shell.cpp -> shell.bin) ===" -ForegroundColor Cyan
Invoke-Tool "compiling shell" $GXX (@($CXXFLAGS) + @("-c", "$P\shell\shell.cpp", "-o", "$P\shell\shell.o"))
Remove-Junk "$P\shell\shell.o" "$P\shell\shell_clean.o"
Invoke-Tool "linking shell" $LD @("-m", "i386pe", "--image-base", "0",
    "-T", "$P\shell\shell.ld", "-o", "$P\shell\shell.exe", "$P\shell\shell_clean.o")
Invoke-Tool "objcopy shell" $OBJCOPY @("-O", "binary", "$P\shell\shell.exe", "$P\shell\shell.bin")
$shell = [System.IO.File]::ReadAllBytes("$P\shell\shell.bin")
if ($shell.Length -gt 1048576) { Write-Host "ERROR: shell.bin больше 1 МБ (зона снимка)" -ForegroundColor Red; exit 1 }
$nmShell = & $NM "$P\shell\shell.exe" | Select-String "T _shell_main" | Select-Object -First 1
if (-not $nmShell) { Write-Host "ERROR: shell_main not found" -ForegroundColor Red; exit 1 }
$shellEntry = ($nmShell.ToString().Trim() -split "\s+")[0]
if ($shellEntry.ToLower() -ne "00400000") { Write-Host "WARN: shell_main at 0x$shellEntry, expected 0x00400000" -ForegroundColor Yellow }
Write-Host "    shell.bin = $($shell.Length) bytes (entry 0x$shellEntry)" -ForegroundColor Yellow

# Образ оболочки для boot.img (ISO): кладём после MyFS, но внутри первых
# 2880 секторов (1.44 МБ) — чтобы El Torito-флоппи его видел.
$shellSecs = [int][Math]::Ceiling($shell.Length / 512)
$blobLba   = 2560
$blobSecs  = $shellSecs
if (($blobLba + $blobSecs) -gt 2880) {
    Write-Host "ERROR: shell.bin не влезает в 1.44 МБ ISO-образа ($blobSecs секторов)" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
if ($Rust) {
    Write-Host "=== 2. Compiling Rust kernel (kernel-rs -> kernel.bin) ===" -ForegroundColor Cyan
    $CARGO = Resolve-Tool "cargo.exe" "MYOS_CARGO" @("$env:USERPROFILE\.cargo\bin\cargo.exe") "Установите Rust (rustup) + nightly."
    $env:RUSTFLAGS = "-Zfunction-sections=yes"
    Invoke-Tool "cargo build" $CARGO @(
        "+nightly-x86_64-pc-windows-gnu", "build", "--release",
        "--target", "i686-pc-windows-gnu", "-Z", "build-std=core",
        "--manifest-path", "$P\kernel-rs\Cargo.toml")
    $rustLib = "$P\kernel-rs\target\i686-pc-windows-gnu\release\libmyos_kernel_rs.a"
    Assert-File $rustLib "Rust staticlib"
    Invoke-Tool "linking Rust kernel" $LD @("-m", "i386pe", "--image-base", "0", "--gc-sections",
        "-T", "$P\kernel-rs\kernel.ld", "-o", "$P\kernel.exe", $rustLib)
    Invoke-Tool "objcopy Rust kernel" $OBJCOPY @("-O", "binary", "$P\kernel.exe", "$P\kernel.bin")
    Assert-File "$P\kernel.bin" "kernel.bin"
    $kernelSize = (Get-Item "$P\kernel.bin").Length
    $sectors = [int][Math]::Ceiling($kernelSize / 512) + 1
} else {
    Write-Host "=== 2. Compiling C++ kernel (kernel/kernel.cpp) ===" -ForegroundColor Cyan
    $sectors = 16
    for ($pass = 0; $pass -lt 3; $pass++) {
        Remove-Item -Force "$P\kernel.o" -ErrorAction SilentlyContinue
        Invoke-Tool "compiling kernel.cpp" $GXX (@($CXXFLAGS) + @("-DKERNEL_SECTORS=$sectors",
            "-c", "$P\kernel\kernel.cpp", "-o", "$P\kernel.o"))
        Remove-Junk "$P\kernel.o" "$P\kernel_clean.o"
        Invoke-Tool "linking kernel" $LD @("-m", "i386pe", "--image-base", "0",
            "-T", "$P\kernel.ld", "-o", "$P\kernel.exe", "$P\kernel_clean.o")
        Invoke-Tool "objcopy kernel" $OBJCOPY @("-O", "binary", "$P\kernel.exe", "$P\kernel.bin")
        Assert-File "$P\kernel.bin" "kernel.bin"
        $kernelSize = (Get-Item "$P\kernel.bin").Length
        $newSectors = [int][Math]::Ceiling($kernelSize / 512) + 1
        if ($newSectors -eq $sectors) { break }
        $sectors = $newSectors
    }
}
Write-Host "    kernel.bin = $kernelSize bytes = $sectors sectors" -ForegroundColor Yellow
if (($sectors + 3) -gt 192) {
    Write-Host "ERROR: ядро не влезает в загрузочную зону (нужно $sectors+3, PART_RESERVED=192)" -ForegroundColor Red
    exit 1
}

$nmLine = & $NM "$P\kernel.exe" | Select-String "T _kernel_main" | Select-Object -First 1
if (-not $nmLine) { Write-Host "ERROR: kernel_main not found" -ForegroundColor Red; exit 1 }
$entryHex = ($nmLine.ToString().Trim() -split "\s+")[0]
$entryAddr = "0x" + $entryHex
Write-Host "    kernel_main entry point = $entryAddr" -ForegroundColor Yellow

# ---------------------------------------------------------------------------
Write-Host "=== 3. Assembling bootloader (boot.asm + vbr.asm + stage2.asm) ===" -ForegroundColor Cyan
Invoke-Tool "nasm boot.asm" $NASM @("-f", "bin", "$P\boot.asm", "-o", "$P\boot.bin")
Invoke-Tool "nasm vbr.asm" $NASM @("-f", "bin", "$P\vbr.asm", "-o", "$P\vbr.bin")
Invoke-Tool "nasm stage2.asm" $NASM @("-f", "bin",
    "-dKERNEL_ENTRY=$entryAddr", "-dKERNEL_SECTORS=$sectors",
    "-dSHELL_BLOB_LBA=$blobLba", "-dSHELL_BLOB_SECTORS=$blobSecs",
    "-dSHELL_BLOB_BYTES=$($shell.Length)", "$P\stage2.asm", "-o", "$P\stage2.bin")

if ((Get-Item "$P\boot.bin").Length -ne 512)    { Write-Host "ERROR: boot.bin не 512 байт" -ForegroundColor Red; exit 1 }
if ((Get-Item "$P\vbr.bin").Length -ne 512)     { Write-Host "ERROR: vbr.bin не 512 байт" -ForegroundColor Red; exit 1 }
if ((Get-Item "$P\stage2.bin").Length -gt 1024) { Write-Host "ERROR: stage2.bin больше 1024 байт!" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
Write-Host "=== 4. Building disk image (disk.img) ===" -ForegroundColor Cyan
$boot   = [System.IO.File]::ReadAllBytes("$P\boot.bin")
$vbr    = [System.IO.File]::ReadAllBytes("$P\vbr.bin")
$stage2 = [System.IO.File]::ReadAllBytes("$P\stage2.bin")
$kernel = [System.IO.File]::ReadAllBytes("$P\kernel.bin")
$img = New-Object System.Collections.Generic.List[byte]
$img.AddRange($boot)
$img.AddRange($vbr)
$stage2Padded = New-Object byte[] 1024
[System.Array]::Copy($stage2, $stage2Padded, $stage2.Length)
$img.AddRange($stage2Padded)
$img.AddRange($kernel)
$diskSize = 33554432
$raw   = $img.ToArray()
$final = New-Object byte[] $diskSize
[System.Array]::Copy($raw, $final, $raw.Length)

function Set-PartEntry($img, $idx, $flag, $type, $lba, $sectors) {
    $off = 446 + $idx * 16
    $img[$off]      = [byte]$flag
    $img[$off + 4]  = [byte]$type
    $img[$off + 8]  = [byte]($lba -band 0xFF)
    $img[$off + 9]  = [byte](($lba -shr 8) -band 0xFF)
    $img[$off + 10] = [byte](($lba -shr 16) -band 0xFF)
    $img[$off + 11] = [byte](($lba -shr 24) -band 0xFF)
    $img[$off + 12] = [byte]($sectors -band 0xFF)
    $img[$off + 13] = [byte](($sectors -shr 8) -band 0xFF)
    $img[$off + 14] = [byte](($sectors -shr 16) -band 0xFF)
    $img[$off + 15] = [byte](($sectors -shr 24) -band 0xFF)
}
$P1LBA = 2048
Set-PartEntry $final 0 0x80 0x83 $P1LBA 16384
Set-PartEntry $final 1 0x00 0x83 18432 16384
Set-PartEntry $final 2 0x00 0x83 34816 16384

# --- Загрузочная зона ВНУТРИ раздела P1 --------------------------------
# MBR читает VBR не с сектора 1 диска, а с начала активного раздела.
# Поэтому VBR/stage2/ядро дублируются в P1: +0, +1..+2, +3...
$vbrPart = [byte[]]$vbr.Clone()
$vbrPart[504] = [byte]($P1LBA -band 0xFF)
$vbrPart[505] = [byte](($P1LBA -shr 8) -band 0xFF)
$vbrPart[506] = [byte](($P1LBA -shr 16) -band 0xFF)
$vbrPart[507] = [byte](($P1LBA -shr 24) -band 0xFF)
[System.Array]::Copy($vbrPart,       0, $final, $P1LBA * 512, 512)
[System.Array]::Copy($stage2Padded,  0, $final, ($P1LBA + 1) * 512, 1024)
[System.Array]::Copy($kernel,        0, $final, ($P1LBA + 3) * 512, $kernel.Length)

function Set-Le16($arr, $off, $val) {
    $arr[$off]     = [byte]($val -band 0xFF)
    $arr[$off + 1] = [byte](($val -shr 8) -band 0xFF)
}
function Set-Le32($arr, $off, $val) {
    $arr[$off]     = [byte]($val -band 0xFF)
    $arr[$off + 1] = [byte](($val -shr 8) -band 0xFF)
    $arr[$off + 2] = [byte](($val -shr 16) -band 0xFF)
    $arr[$off + 3] = [byte](($val -shr 24) -band 0xFF)
}
function Set-Name($arr, $off, $s) {
    for ($i = 0; $i -lt $s.Length; $i++) { $arr[$off + $i] = [byte][char]$s[$i] }
}
$sbLba     = $P1LBA + 192
$tabLba    = $sbLba + 1
$dataLba   = $sbLba + 2
$demoStart = $dataLba + $shellSecs
$demoSecs  = [int][Math]::Ceiling($demo.Length / 512)
$dataPtr   = $demoStart + $demoSecs
$sb = New-Object byte[] 512
$sb[0] = [byte][char]'M'; $sb[1] = [byte][char]'Y'; $sb[2] = [byte][char]'F'; $sb[3] = [byte][char]'S'
Set-Le16 $sb 4  1
Set-Le32 $sb 6  16384
Set-Le32 $sb 10 $tabLba
Set-Le32 $sb 14 $dataLba
Set-Le32 $sb 18 $dataPtr
Set-Le16 $sb 22 2
Set-Le16 $sb 24 8
[System.Array]::Copy($sb, 0, $final, $sbLba * 512, 512)
$tab = New-Object byte[] 512
Set-Name $tab 0  "shell.bin"; Set-Le32 $tab 56  $dataLba;   Set-Le32 $tab 60  $shell.Length
Set-Name $tab 64 "demo.bin";  Set-Le32 $tab 120 $demoStart; Set-Le32 $tab 124 $demo.Length
[System.Array]::Copy($tab, 0, $final, $tabLba * 512, 512)
[System.Array]::Copy($shell, 0, $final, $dataLba * 512, $shell.Length)
[System.Array]::Copy($demo,  0, $final, $demoStart * 512, $demo.Length)
# Копия shell.bin для ISO-загрузки (stage2 читает её при DL=0x00).
if ($blobLba -lt $dataPtr) {
    Write-Host "ERROR: shell blob LBA пересекается с данными MyFS (blob=$blobLba data=$dataPtr)" -ForegroundColor Red
    exit 1
}
[System.Array]::Copy($shell, 0, $final, $blobLba * 512, $shell.Length)
try {
    [System.IO.File]::WriteAllBytes("$P\disk.img", $final)
} catch {
    Write-Host "ERROR: не удалось записать disk.img" -ForegroundColor Red
    exit 1
}
Write-Host "    disk.img = $($final.Length) bytes (32 MB HDD with MBR table)" -ForegroundColor Green
if ($NoRun) { Write-Host "Done (no run)."; exit 0 }

Write-Host "`nDone! disk.img created. Now load it into Oracle VM VirtualBox." -ForegroundColor Green
