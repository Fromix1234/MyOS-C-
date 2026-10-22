# ===========================================================================
#  build.ps1 - build the whole OS: kernel (ring 0) + shell (ring 3) + disk
# ===========================================================================
#  Architecture (v2.0, Zen/Linux-like):
#    - kernel.bin (ring 0): hardware + syscalls only, no GUI;
#    - shell.bin  (ring 3): the whole desktop/console/apps, runs from MyFS;
#    - demo.bin   (ring 3): test program started by the 'ring3' command.
#
#  Steps:
#    0. user_demo.asm -> user_demo.bin  (ring 3 program at 0x500000)
#    1. shell/shell.cpp -> shell.bin    (ring 3 GUI at 0x400000)
#    2. kernel/kernel.cpp -> kernel.bin (2 passes learn KERNEL_SECTORS)
#    3. boot.asm / vbr.asm / stage2.asm -> boot sector, VBR, mode switch
#    4. disk.img: MBR + raw boot chain + P1 with MyFS (shell.bin, demo.bin)
#
#  NOTE: messages are in English on purpose - Cyrillic in .ps1 breaks
#  PowerShell 5.1 encoding. The real code (.asm/.cpp) has Russian comments.
# ===========================================================================

$NASM = "C:\Users\User\AppData\Local\bin\NASM\nasm.exe"
$QEMU = "C:\Program Files\qemu\qemu-system-i386.exe"
$BIN  = "C:\Users\User\AppData\Local\Programs\w64devkit\w64devkit\bin"
$GXX  = "g++"
$RUSTC = "$env:USERPROFILE\.cargo\bin\rustc.exe"
$P    = "D:\C++\MyOS"
Set-Location $P

$CXXFLAGS = @("-m32","-ffreestanding","-fno-exceptions","-fno-rtti","-fno-pie",
              "-fno-stack-protector","-nostdlib","-nostartfiles","-mno-sse",
              "-mno-mmx","-mno-80387","-fno-asynchronous-unwind-tables",
              "-Os","-fno-strict-aliasing")

function Remove-Junk($obj, $clean) {
    & "$BIN\objcopy.exe" --remove-section=.comment --remove-section=.note --remove-section=.note.GNU-stack $obj $clean
}

# ---------------------------------------------------------------------------
Write-Host "=== 0. Building ring 3 program (user_demo.asm) ===" -ForegroundColor Cyan
& $NASM -f bin "$P\user_demo.asm" -o "$P\user_demo.bin"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR user_demo.asm" -ForegroundColor Red; exit 1 }
$demo = [System.IO.File]::ReadAllBytes("$P\user_demo.bin")
Write-Host "    user_demo.bin = $($demo.Length) bytes (ring 3 program, org 0x500000)" -ForegroundColor Yellow

# ---------------------------------------------------------------------------
Write-Host "=== 1. Building the GUI shell (shell/shell.cpp -> shell.bin) ===" -ForegroundColor Cyan
# The shell is a SEPARATE binary (the Linux way): it lives on MyFS (shell.bin),
# the kernel loads it to 4 MB and starts it as a ring 3 task. It talks to the
# kernel ONLY through syscalls (int 0x80) - see common/myos.h.
& $GXX @CXXFLAGS -c "$P\shell\shell.cpp" -o "$P\shell\shell.o"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR compiling shell" -ForegroundColor Red; exit 1 }
if (-not (Test-Path "$P\shell\shell.o")) { Write-Host "ERROR compiling shell" -ForegroundColor Red; exit 1 }
Remove-Junk "$P\shell\shell.o" "$P\shell\shell_clean.o"
# --image-base 0: base 0x400000 IS the load address; otherwise the PE image
# base collides with it and objcopy pads the binary from address 0 (4 MB!).
& "$BIN\ld.exe" -m i386pe --image-base 0 -T "$P\shell\shell.ld" -o "$P\shell\shell.exe" "$P\shell\shell_clean.o"
if (-not (Test-Path "$P\shell\shell.exe")) { Write-Host "ERROR linking shell" -ForegroundColor Red; exit 1 }
& "$BIN\objcopy.exe" -O binary "$P\shell\shell.exe" "$P\shell\shell.bin"
$shell = [System.IO.File]::ReadAllBytes("$P\shell\shell.bin")
if ($shell.Length -gt 1048576) { Write-Host "ERROR: shell.bin is bigger than 1 MB (snapshot zone)" -ForegroundColor Red; exit 1 }
# shell_main must sit at the VERY start of the image: the kernel jumps to 0x400000
$nmShell = & "$BIN\nm.exe" "$P\shell\shell.exe" | Select-String "T _shell_main" | Select-Object -First 1
if (-not $nmShell) { Write-Host "ERROR: shell_main not found" -ForegroundColor Red; exit 1 }
$shellEntry = ($nmShell.ToString().Trim() -split "\s+")[0]
if ($shellEntry.ToLower() -ne "00400000") { Write-Host "WARN: shell_main at 0x$shellEntry, expected 0x00400000" -ForegroundColor Yellow }
Write-Host "    shell.bin = $($shell.Length) bytes (entry 0x$shellEntry)" -ForegroundColor Yellow

# ---------------------------------------------------------------------------
Write-Host "=== 2. Compiling C++ kernel (kernel/kernel.cpp) ===" -ForegroundColor Cyan
# Rust-компоненты ядра (kernel-rs): no_std staticlib в COFF под
# i686-pc-windows-gnu. Линкуются тем же MinGW-линкером, что и C++-ядро;
# граница языков — C ABI. Собирается один раз (от проходов KERNEL_SECTORS
# не зависит).
if (Test-Path $RUSTC) {
    Write-Host "=== 1b. Compiling Rust kernel component (kernel-rs) ===" -ForegroundColor Cyan
    & $RUSTC --edition 2021 --target i686-pc-windows-gnu --crate-type staticlib -O -C panic=abort "$P\kernel-rs\rs_probe.rs" -o "$P\kernel-rs\librs.a"
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR compiling kernel-rs (rustc)" -ForegroundColor Red; exit 1 }
    if (-not (Test-Path "$P\kernel-rs\librs.a")) { Write-Host "ERROR: kernel-rs/librs.a not created" -ForegroundColor Red; exit 1 }
} else {
    Write-Host "WARN: rustc not found - Rust components disabled" -ForegroundColor Yellow
}
# Compile TWICE: the first pass learns the kernel size, the second pass bakes
# that number into the kernel as KERNEL_SECTORS (the 'install' command copies
# the kernel by sectors and must know how many). The loop recalculates until
# the value stops changing (normally exactly 2 passes).
$sectors = 16
for ($pass = 0; $pass -lt 3; $pass++) {
    Remove-Item -Force "$P\kernel.o" -ErrorAction SilentlyContinue
    & $GXX @CXXFLAGS "-DKERNEL_SECTORS=$sectors" -c "$P\kernel\kernel.cpp" -o "$P\kernel.o"
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR compiling kernel.cpp" -ForegroundColor Red; exit 1 }
    if (-not (Test-Path "$P\kernel.o")) { Write-Host "ERROR compiling kernel.cpp" -ForegroundColor Red; exit 1 }
    Remove-Junk "$P\kernel.o" "$P\kernel_clean.o"
    Write-Host "=== 2. Linking kernel (kernel.ld) ===" -ForegroundColor Cyan
    & "$BIN\ld.exe" -m i386pe -T "$P\kernel.ld" -o "$P\kernel.exe" "$P\kernel_clean.o" "$P\kernel-rs\librs.a"
    if (-not (Test-Path "$P\kernel.exe")) { Write-Host "ERROR linking" -ForegroundColor Red; exit 1 }
    & "$BIN\objcopy.exe" -O binary "$P\kernel.exe" "$P\kernel.bin"
    $kernelSize = (Get-Item "$P\kernel.bin").Length
    $newSectors = [int][Math]::Ceiling($kernelSize / 512) + 1
    if ($newSectors -eq $sectors) { break }
    $sectors = $newSectors
}
Write-Host "    kernel.bin = $kernelSize bytes = $sectors sectors" -ForegroundColor Yellow
if (($sectors + 3) -gt 192) { Write-Host "ERROR: kernel does not fit the boot area (need $sectors+3, PART_RESERVED=192)" -ForegroundColor Red; exit 1 }

# Extract the real entry point address of kernel_main
$nmLine = & "$BIN\nm.exe" "$P\kernel.exe" | Select-String "T _kernel_main" | Select-Object -First 1
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
# Assemble stage2 with the EXACT kernel entry address and sector count
& $NASM -f bin "-dKERNEL_ENTRY=$entryAddr" "-dKERNEL_SECTORS=$sectors" "$P\stage2.asm" -o "$P\stage2.bin"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR stage2.asm" -ForegroundColor Red; exit 1 }

# Check sector boundaries (critical!)
if ((Get-Item "$P\boot.bin").Length -ne 512)    { Write-Host "ERROR: boot.bin is not 512 bytes" -ForegroundColor Red; exit 1 }
if ((Get-Item "$P\vbr.bin").Length -ne 512)     { Write-Host "ERROR: vbr.bin is not 512 bytes" -ForegroundColor Red; exit 1 }
if ((Get-Item "$P\stage2.bin").Length -gt 1024) { Write-Host "ERROR: stage2.bin larger than 1024 bytes!" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
Write-Host "=== 4. Building disk image (disk.img) ===" -ForegroundColor Cyan
# Raw layout: [sector 0: MBR][sector 1: VBR][sectors 2-3: stage2][sector 4+: kernel]
# The system is ALSO pre-installed into partition P1 (LBA 2048):
#   P1+0: VBR (copy with base LBA 2048 baked at offset 504)
#   P1+1..P1+2: stage2 (2 sectors), P1+3...: kernel
#   P1+192: MyFS (superblock, file table, data: shell.bin then demo.bin)
# On boot the MBR finds the ACTIVE partition (flag 0x80) and chainloads its VBR,
# so a freshly built image boots FROM THE PARTITION, not from raw sectors.
$boot   = [System.IO.File]::ReadAllBytes("$P\boot.bin")
$vbr    = [System.IO.File]::ReadAllBytes("$P\vbr.bin")
$stage2 = [System.IO.File]::ReadAllBytes("$P\stage2.bin")
$kernel = [System.IO.File]::ReadAllBytes("$P\kernel.bin")
$img = New-Object System.Collections.Generic.List[byte]
$img.AddRange($boot)
$img.AddRange($vbr)
# Pad stage2 with ZEROS to exactly 1024 bytes = 2 sectors (else kernel shifts!)
$stage2Padded = New-Object byte[] 1024
[System.Array]::Copy($stage2, $stage2Padded, $stage2.Length)
$img.AddRange($stage2Padded)
$img.AddRange($kernel)
# Build the final image as a 32 MB HARD DISK (65536 sectors * 512 = 33554432).
# It is a real IDE disk for QEMU: in protected mode the kernel reads it through
# the ATA controller (ports 0x1F0..0x1F7) - see the "part" command.
# Padding to a bigger size only appends zeros, so the layout at the start
# (sector 0 MBR, sector 1 VBR, sectors 2-3 stage2, sector 4+ kernel) does not change.
$diskSize = 33554432
$raw   = $img.ToArray()
$final = New-Object byte[] $diskSize
[System.Array]::Copy($raw, $final, $raw.Length)

# --- MBR PARTITION TABLE (sector 0, offset 446 = 0x1BE) ---
# 4 entries of 16 bytes. Layout: [0]=boot flag, [4]=type, [8..11]=start LBA,
# [12..15]=sector count (all little-endian). This is how a real disk stores
# its partitions - our "part" command reads and shows them.
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
# Three 8 MB partitions (16384 sectors each), 1 MB aligned (start at 2048)
Set-PartEntry $final 0 0x80 0x83 2048  16384   # P1: bootable, MyOS
Set-PartEntry $final 1 0x00 0x83 18432 16384   # P2: MyOS
Set-PartEntry $final 2 0x00 0x83 34816 16384   # P3: MyOS

# --- little-endian helpers (MyFS layout must match kernel/k3_myfs.cpp) ---
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

# --- PRE-INSTALL the boot zone into P1 (same as 'install 1 yes' at runtime) ---
# VBR copy with the partition base LBA baked into its field at offset 504
$vbrP1 = New-Object byte[] 512
[System.Array]::Copy($vbr, $vbrP1, 512)
Set-Le32 $vbrP1 504 2048
[System.Array]::Copy($vbrP1,        0, $final, 2048 * 512, 512)            # P1+0: VBR
[System.Array]::Copy($stage2Padded, 0, $final, 2049 * 512, 1024)           # P1+1..2: stage2
[System.Array]::Copy($kernel,       0, $final, 2051 * 512, $kernel.Length) # P1+3..: kernel

# --- MyFS on P1: superblock (26 packed bytes) + file table + data ----------
# Superblock: magic[4] @0, version u16 @4, total_sectors u32 @6,
# table_lba u32 @10, data_lba u32 @14, data_ptr u32 @18,
# file_count u16 @22, files_max u16 @24. Table: 8 entries x 64 bytes
# (name[56], start u32 @56, size u32 @60).
$P1LBA     = 2048
$sbLba     = $P1LBA + 192                     # 2240: superblock
$tabLba    = $sbLba + 1                       # 2241: file table
$dataLba   = $sbLba + 2                       # 2242: data area (shell.bin...)
$shellSecs = [int][Math]::Ceiling($shell.Length / 512)
$demoStart = $dataLba + $shellSecs
$demoSecs  = [int][Math]::Ceiling($demo.Length / 512)
$dataPtr   = $demoStart + $demoSecs           # first free data sector

$sb = New-Object byte[] 512
$sb[0] = [byte][char]'M'; $sb[1] = [byte][char]'Y'
$sb[2] = [byte][char]'F'; $sb[3] = [byte][char]'S'
Set-Le16 $sb 4  1                             # version = 1
Set-Le32 $sb 6  16384                         # total_sectors (P1 size)
Set-Le32 $sb 10 $tabLba                       # table_lba
Set-Le32 $sb 14 $dataLba                      # data_lba
Set-Le32 $sb 18 $dataPtr                      # data_ptr (free)
Set-Le16 $sb 22 2                             # file_count (shell.bin, demo.bin)
Set-Le16 $sb 24 8                             # files_max
[System.Array]::Copy($sb, 0, $final, $sbLba * 512, 512)

$tab = New-Object byte[] 512                  # 8 entries x 64 bytes
Set-Name $tab 0  "shell.bin"; Set-Le32 $tab 56  $dataLba;   Set-Le32 $tab 60  $shell.Length
Set-Name $tab 64 "demo.bin";  Set-Le32 $tab 120 $demoStart; Set-Le32 $tab 124 $demo.Length
[System.Array]::Copy($tab, 0, $final, $tabLba * 512, 512)

[System.Array]::Copy($shell, 0, $final, $dataLba * 512, $shell.Length)    # shell.bin data
[System.Array]::Copy($demo,  0, $final, $demoStart * 512, $demo.Length)   # demo.bin data
Write-Host "    MyFS on P1: shell.bin @ $dataLba ($shellSecs sec), demo.bin @ $demoStart ($demoSecs sec)" -ForegroundColor Yellow

[System.IO.File]::WriteAllBytes("$P\disk.img", $final)
Write-Host "    disk.img = $($final.Length) bytes (32 MB HDD with MBR table)" -ForegroundColor Green

if ($args[0] -eq "-norun") { Write-Host "Done (no run)."; exit 0 }

Write-Host ""
Write-Host "=== 5. Running in QEMU ===" -ForegroundColor Cyan
Write-Host "Close the QEMU window to stop the kernel."
& $QEMU -drive "format=raw,file=$P\disk.img"
