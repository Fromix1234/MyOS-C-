# ===========================================================================
#  build.ps1 - build the whole OS: kernel (ring 0) + shell (ring 3) + disk
# ===========================================================================
#  Architecture (v3.0, Zen/Linux-like):
#    - kernel.bin (ring 0): Rust kernel-rs (default: C++ kernel v2.1 reference);
#    - shell.bin  (ring 3): the whole desktop/console/apps, runs from MyFS;
#    - demo.bin   (ring 3): test program started by the 'ring3' command.
# ===========================================================================

# АБСОЛЮТНЫЕ ПУТИ ДЛЯ ГАРАНТИРОВАННОГО ЗАПУСКА
$NASM = "C:\Users\fRoMiXpc\AppData\Local\bin\NASM\nasm.exe"
$BIN  = "C:\Users\fRoMiXpc\msys64\msys64\mingw32\bin"
$GXX  = "$BIN\g++.exe"
$P    = "C:\Users\fRoMiXpc\MyOS-C-"
Set-Location $P

# Проверка наличия инструментов перед началом
if (-not (Test-Path $NASM)) { Write-Host "ERROR: NASM not found at $NASM" -ForegroundColor Red; exit 1 }
if (-not (Test-Path "$BIN\ld.exe")) {
    Write-Host "ERROR: MinGW-w64 i686 not found at $BIN" -ForegroundColor Red
    Write-Host "Install it in MSYS2: pacman -S mingw-w64-i686-gcc" -ForegroundColor Yellow
    exit 1
}
# cc1plus.exe лежит отдельно от g++.exe и ищет свои DLL в PATH.
$env:Path = "$BIN;$env:Path"

$CXXFLAGS = @("-m32","-ffreestanding","-fno-exceptions","-fno-rtti","-fno-pie",
              "-fno-stack-protector","-nostdlib","-nostartfiles","-mno-sse",
              "-mno-mmx","-mno-80387","-fno-asynchronous-unwind-tables",
              "-Os","-fno-strict-aliasing")

function Remove-Junk($obj, $clean) {
    $tool = if ($BIN) { "$BIN\objcopy.exe" } else { "objcopy" }
    & $tool --remove-section=.comment --remove-section=.note --remove-section=.note.GNU-stack $obj $clean
}

# ---------------------------------------------------------------------------
Write-Host "=== 0. Building ring 3 program (user_demo.asm) ===" -ForegroundColor Cyan
& $NASM -f bin "$P\user_demo.asm" -o "$P\user_demo.bin"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR user_demo.asm" -ForegroundColor Red; exit 1 }
$demo = [System.IO.File]::ReadAllBytes("$P\user_demo.bin")
Write-Host "    user_demo.bin = $($demo.Length) bytes (ring 3 program, org 0x500000)" -ForegroundColor Yellow

# ---------------------------------------------------------------------------
Write-Host "=== 1. Building the GUI shell (shell/shell.cpp -> shell.bin) ===" -ForegroundColor Cyan
& $GXX @CXXFLAGS -c "$P\shell\shell.cpp" -o "$P\shell\shell.o"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR compiling shell" -ForegroundColor Red; exit 1 }
if (-not (Test-Path "$P\shell\shell.o")) { Write-Host "ERROR compiling shell" -ForegroundColor Red; exit 1 }
Remove-Junk "$P\shell\shell.o" "$P\shell\shell_clean.o"

$ldTool = if ($BIN) { "$BIN\ld.exe" } else { "ld" }
& $ldTool -m i386pe --image-base 0 -T "$P\shell\shell.ld" -o "$P\shell\shell.exe" "$P\shell\shell_clean.o"
if (-not (Test-Path "$P\shell\shell.exe")) { Write-Host "ERROR linking shell" -ForegroundColor Red; exit 1 }

$objcopyTool = if ($BIN) { "$BIN\objcopy.exe" } else { "objcopy" }
& $objcopyTool -O binary "$P\shell\shell.exe" "$P\shell\shell.bin"
$shell = [System.IO.File]::ReadAllBytes("$P\shell\shell.bin")
if ($shell.Length -gt 1048576) { Write-Host "ERROR: shell.bin is bigger than 1 MB (snapshot zone)" -ForegroundColor Red; exit 1 }
$nmTool = if ($BIN) { "$BIN\nm.exe" } else { "nm" }
$nmShell = & $nmTool "$P\shell\shell.exe" | Select-String "T _shell_main" | Select-Object -First 1
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
    Write-Host "ERROR: shell.bin too big for the 1.44 MB ISO boot image ($blobSecs sectors)" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
$UseRust = ($args -contains "-rust")
if ($UseRust) {
    Write-Host "=== 2. Compiling Rust kernel (kernel-rs -> kernel.bin) ===" -ForegroundColor Cyan
    $CARGO = "$env:USERPROFILE\.cargo\bin\cargo.exe"
    if (-not (Test-Path $CARGO)) { Write-Host "ERROR: cargo not found" -ForegroundColor Red; exit 1 }
    $env:RUSTFLAGS = "-Zfunction-sections=yes"
    & $CARGO +nightly-x86_64-pc-windows-gnu build --release --target i686-pc-windows-gnu -Z build-std=core --manifest-path "$P\kernel-rs\Cargo.toml"
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR compiling Rust kernel" -ForegroundColor Red; exit 1 }
    $rustLib = "$P\kernel-rs\target\i686-pc-windows-gnu\release\libmyos_kernel_rs.a"
    if (-not (Test-Path $rustLib)) { Write-Host "ERROR: Rust staticlib not found" -ForegroundColor Red; exit 1 }
    $ldTool = if ($BIN) { "$BIN\ld.exe" } else { "ld" }
    & $ldTool -m i386pe --image-base 0 --gc-sections -T "$P\kernel-rs\kernel.ld" -o "$P\kernel.exe" "$rustLib"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$P\kernel.exe")) { Write-Host "ERROR linking Rust kernel" -ForegroundColor Red; exit 1 }
    $objcopyTool = if ($BIN) { "$BIN\objcopy.exe" } else { "objcopy" }
    & $objcopyTool -O binary "$P\kernel.exe" "$P\kernel.bin"
    $kernelSize = (Get-Item "$P\kernel.bin").Length
    $sectors = [int][Math]::Ceiling($kernelSize / 512) + 1
} else {
    Write-Host "=== 2. Compiling C++ kernel (kernel/kernel.cpp) ===" -ForegroundColor Cyan
    $sectors = 16
    for ($pass = 0; $pass -lt 3; $pass++) {
        Remove-Item -Force "$P\kernel.o" -ErrorAction SilentlyContinue
        & $GXX @CXXFLAGS "-DKERNEL_SECTORS=$sectors" -c "$P\kernel\kernel.cpp" -o "$P\kernel.o"
        if ($LASTEXITCODE -ne 0) { Write-Host "ERROR compiling kernel.cpp" -ForegroundColor Red; exit 1 }
        if (-not (Test-Path "$P\kernel.o")) { Write-Host "ERROR compiling kernel.cpp" -ForegroundColor Red; exit 1 }
        Remove-Junk "$P\kernel.o" "$P\kernel_clean.o"
        $ldTool = if ($BIN) { "$BIN\ld.exe" } else { "ld" }
        & $ldTool -m i386pe --image-base 0 -T "$P\kernel.ld" -o "$P\kernel.exe" "$P\kernel_clean.o"
        if ($LASTEXITCODE -ne 0) { Write-Host "ERROR linking" -ForegroundColor Red; exit 1 }
        $objcopyTool = if ($BIN) { "$BIN\objcopy.exe" } else { "objcopy" }
        & $objcopyTool -O binary "$P\kernel.exe" "$P\kernel.bin"
        $kernelSize = (Get-Item "$P\kernel.bin").Length
        $newSectors = [int][Math]::Ceiling($kernelSize / 512) + 1
        if ($newSectors -eq $sectors) { break }
        $sectors = $newSectors
    }
}
Write-Host "    kernel.bin = $kernelSize bytes = $sectors sectors" -ForegroundColor Yellow
if (($sectors + 3) -gt 192) { Write-Host "ERROR: kernel does not fit the boot area (need $sectors+3, PART_RESERVED=192)" -ForegroundColor Red; exit 1 }

$nmTool = if ($BIN) { "$BIN\nm.exe" } else { "nm" }
$nmLine = & $nmTool "$P\kernel.exe" | Select-String "T _kernel_main" | Select-Object -First 1
if (-not $nmLine) { Write-Host "ERROR: kernel_main not found" -ForegroundColor Red; exit 1 }
$entryHex = ($nmLine.ToString().Trim() -split "\s+")[0]
$entryAddr = "0x" + $entryHex
Write-Host "    kernel_main entry point = $entryAddr" -ForegroundColor Yellow

# ---------------------------------------------------------------------------
Write-Host "=== 3. Assembling bootloader (boot.asm + vbr.asm + stage2.asm) ===" -ForegroundColor Cyan
& $NASM -f bin "$P\boot.asm" -o "$P\boot.bin"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR boot.asm" -ForegroundColor Red; exit 1 }
& $NASM -f bin "$P\vbr.asm" -o "$P\vbr.bin"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR vbr.asm" -ForegroundColor Red; exit 1 }
& $NASM -f bin "-dKERNEL_ENTRY=$entryAddr" "-dKERNEL_SECTORS=$sectors" "-dSHELL_BLOB_LBA=$blobLba" "-dSHELL_BLOB_SECTORS=$blobSecs" "-dSHELL_BLOB_BYTES=$($shell.Length)" "$P\stage2.asm" -o "$P\stage2.bin"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR stage2.asm" -ForegroundColor Red; exit 1 }

if ((Get-Item "$P\boot.bin").Length -ne 512)    { Write-Host "ERROR: boot.bin is not 512 bytes" -ForegroundColor Red; exit 1 }
if ((Get-Item "$P\vbr.bin").Length -ne 512)     { Write-Host "ERROR: vbr.bin is not 512 bytes" -ForegroundColor Red; exit 1 }
if ((Get-Item "$P\stage2.bin").Length -gt 1024) { Write-Host "ERROR: stage2.bin larger than 1024 bytes!" -ForegroundColor Red; exit 1 }

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
    Write-Host "ERROR: shell blob LBA overlaps MyFS data area (blob=$blobLba data=$dataPtr)" -ForegroundColor Red
    exit 1
}
[System.Array]::Copy($shell, 0, $final, $blobLba * 512, $shell.Length)
try {
    [System.IO.File]::WriteAllBytes("$P\disk.img", $final)
} catch {
    Write-Host "ERROR: cannot write disk.img" -ForegroundColor Red
    exit 1
}
Write-Host "    disk.img = $($final.Length) bytes (32 MB HDD with MBR table)" -ForegroundColor Green
if ($args -contains "-norun") { Write-Host "Done (no run)."; exit 0 }

Write-Host "`nDone! disk.img created. Now load it into Oracle VM VirtualBox." -ForegroundColor Green
