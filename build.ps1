# ===========================================================================
#  build.ps1 - build the whole OS: C++ kernel + bootloader + disk image
# ===========================================================================
#  Steps:
#    1. kernel.cpp -> kernel.o -> kernel.exe (PE) -> kernel.bin
#       (also extracts the real entry point address of kernel_main)
#    2. boot.asm   -> boot.bin   (sector 1: boot sector)
#    3. stage2.asm -> stage2.bin (sectors 2-3: VESA video, loads kernel)
#    4. Concatenate into disk.img (the "virtual USB stick")
#
#  NOTE: messages are in English on purpose - Cyrillic in .ps1 breaks
#  PowerShell 5.1 encoding. The real code (.asm/.cpp) has Russian comments.
# ===========================================================================

$NASM = "C:\Users\User\AppData\Local\bin\NASM\nasm.exe"
$QEMU = "C:\Program Files\qemu\qemu-system-i386.exe"
$BIN  = "C:\Users\User\AppData\Local\Programs\w64devkit\w64devkit\bin"
$GXX  = "g++"
$P    = "D:\C++\MyOS"
Set-Location $P

$CXXFLAGS = @("-m32","-ffreestanding","-fno-exceptions","-fno-rtti","-fno-pie",
              "-fno-stack-protector","-nostdlib","-nostartfiles","-mno-sse",
              "-mno-mmx","-mno-80387","-fno-asynchronous-unwind-tables")

Write-Host "=== 1. Compiling C++ kernel (kernel.cpp) ===" -ForegroundColor Cyan
# Compile TWICE: the first pass learns the kernel size, the second pass bakes
# that number into the kernel as KERNEL_SECTORS (the 'install' command copies
# the kernel by sectors and must know how many). The loop recalculates until
# the value stops changing (normally exactly 2 passes).
$sectors = 16
for ($pass = 0; $pass -lt 3; $pass++) {
    & $GXX @CXXFLAGS "-DKERNEL_SECTORS=$sectors" -c "$P\kernel.cpp" -o "$P\kernel.o"
    if (-not (Test-Path "$P\kernel.o")) { Write-Host "ERROR compiling kernel.cpp" -ForegroundColor Red; exit 1 }

    # Remove junk sections (.comment/.note) that MinGW prepends
    & "$BIN\objcopy.exe" --remove-section=.comment --remove-section=.note --remove-section=.note.GNU-stack "$P\kernel.o" "$P\kernel_clean.o"

    Write-Host "=== 2. Linking kernel (kernel.ld) ===" -ForegroundColor Cyan
    & "$BIN\ld.exe" -m i386pe -T "$P\kernel.ld" -o "$P\kernel.exe" "$P\kernel_clean.o"
    if (-not (Test-Path "$P\kernel.exe")) { Write-Host "ERROR linking" -ForegroundColor Red; exit 1 }

    # Flat binary (no PE headers) - this is what we load into memory
    & "$BIN\objcopy.exe" -O binary "$P\kernel.exe" "$P\kernel.bin"
    $kernelSize = (Get-Item "$P\kernel.bin").Length
    $newSectors = [int][Math]::Ceiling($kernelSize / 512) + 1
    if ($newSectors -eq $sectors) { break }
    $sectors = $newSectors
}
Write-Host "    kernel.bin = $kernelSize bytes = $sectors sectors" -ForegroundColor Yellow

# Extract the real entry point address of kernel_main
$nmLine = & "$BIN\nm.exe" "$P\kernel.exe" | Select-String "T _kernel_main" | Select-Object -First 1
if (-not $nmLine) { Write-Host "ERROR: kernel_main not found" -ForegroundColor Red; exit 1 }
$entryHex = ($nmLine.ToString().Trim() -split "\s+")[0]
$entryAddr = "0x" + $entryHex
Write-Host "    kernel_main entry point = $entryAddr" -ForegroundColor Yellow

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

Write-Host "=== 4. Building disk image (disk.img) ===" -ForegroundColor Cyan
# Raw layout: [sector 1: MBR][sector 2: VBR][sectors 3-4: stage2][sectors 5+: kernel]
# The system is ALSO pre-installed into partition P1 (LBA 2048):
#   P1+0: VBR (copy with base LBA 2048 baked at offset 504)
#   P1+1..P1+2: stage2 (2 sectors), P1+3...: kernel
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
# (sector 1 MBR, sector 2 VBR, sectors 3-4 stage2, sectors 5+ kernel) does not change.
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

# --- PRE-INSTALL the system into P1 (same as 'install 1 yes' at runtime) ---
function Set-Le32($arr, $off, $val) {
    $arr[$off]     = [byte]($val -band 0xFF)
    $arr[$off + 1] = [byte](($val -shr 8) -band 0xFF)
    $arr[$off + 2] = [byte](($val -shr 16) -band 0xFF)
    $arr[$off + 3] = [byte](($val -shr 24) -band 0xFF)
}
# VBR copy with the partition base LBA baked into its field at offset 504
$vbrP1 = New-Object byte[] 512
[System.Array]::Copy($vbr, $vbrP1, 512)
Set-Le32 $vbrP1 504 2048
[System.Array]::Copy($vbrP1,        0, $final, 2048 * 512, 512)            # P1+0: VBR
[System.Array]::Copy($stage2Padded, 0, $final, 2049 * 512, 1024)           # P1+1..2: stage2
[System.Array]::Copy($kernel,       0, $final, 2051 * 512, $kernel.Length) # P1+3..: kernel

[System.IO.File]::WriteAllBytes("$P\disk.img", $final)
Write-Host "    disk.img = $($final.Length) bytes (32 MB HDD with MBR table)" -ForegroundColor Green

if ($args[0] -eq "-norun") { Write-Host "Done (no run)."; exit 0 }

Write-Host ""
Write-Host "=== 5. Running in QEMU ===" -ForegroundColor Cyan
Write-Host "Close the QEMU window to stop the kernel."
& $QEMU -drive "format=raw,file=$P\disk.img"
