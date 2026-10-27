# ===========================================================================
#  build_iso.ps1 - create a bootable El Torito ISO without external tools
# ===========================================================================
#  Layout:
#    0..15   system area (zero)
#    16      Primary Volume Descriptor (ISO 9660)
#    17      Boot Record Volume Descriptor (El Torito)
#    18      Boot Catalog
#    19      Volume Descriptor Set Terminator
#    20      root directory
#    21      README.TXT
#    22      Type-L path table
#    23      Type-M path table
#    32..    boot image: first 1.44 MB of disk.img (720 x 2048-byte sectors)
#
#  The boot image is a 1.44 MB floppy image.  BIOS floppy emulation gives the
#  MyOS MBR a real int 13h drive, so the existing MBR -> VBR -> stage2 -> kernel
#  chain works unchanged.  Stage2 also preloads shell.bin from LBA 2560 (inside
#  this 1.44 MB image), so the kernel does not need an ATA/CD driver.
# ===========================================================================

$ErrorActionPreference = "Stop"
$P = (Get-Location).Path
$diskPath = Join-Path $P 'disk.img'
$bootPath = Join-Path $P 'boot.img'
$isoPath  = Join-Path $P 'MyOS.iso'

if (-not (Test-Path $diskPath)) {
    Write-Host "ERROR: $diskPath not found. Run .\build.ps1 first." -ForegroundColor Red
    exit 1
}

$SECTOR = 2048
$FLOPPY_BYTES = 1474560                 # 1.44 MB = 2880 x 512
$FLOPPY_SECTORS = [int]($FLOPPY_BYTES / $SECTOR)   # 720

$diskBytes = [IO.File]::ReadAllBytes($diskPath)
if ($diskBytes.Length -lt $FLOPPY_BYTES) {
    Write-Host "ERROR: disk.img is smaller than 1.44 MB." -ForegroundColor Red
    exit 1
}
$bootBytes = New-Object byte[] $FLOPPY_BYTES
[Array]::Copy($diskBytes, 0, $bootBytes, 0, $FLOPPY_BYTES)
[IO.File]::WriteAllBytes($bootPath, $bootBytes)
Write-Host "boot.img = $FLOPPY_BYTES bytes (1.44 MB floppy image)" -ForegroundColor Green

# --- ISO layout constants --------------------------------------------------
$PVD_LBA     = 16
$BRVD_LBA    = 17
$CATALOG_LBA = 18
$TERM_LBA    = 19
$ROOT_LBA    = 20
$README_LBA  = 21
$PATHL_LBA   = 22
$PATHM_LBA   = 23
$BOOT_LBA    = 32
$TOTAL_SECTORS = $BOOT_LBA + $FLOPPY_SECTORS   # 752 sectors = 1,540,096 bytes

$iso = New-Object byte[] ($TOTAL_SECTORS * $SECTOR)

# --- little/big endian helpers --------------------------------------------
function Put-LE16([byte[]]$a, [int]$o, [int]$v) {
    $a[$o] = [byte]($v -band 0xFF)
    $a[$o + 1] = [byte](($v -shr 8) -band 0xFF)
}
function Put-BE16([byte[]]$a, [int]$o, [int]$v) {
    $a[$o] = [byte](($v -shr 8) -band 0xFF)
    $a[$o + 1] = [byte]($v -band 0xFF)
}
function Put-LE32([byte[]]$a, [int]$o, [long]$v) {
    $a[$o]     = [byte]($v -band 0xFF)
    $a[$o + 1] = [byte](($v -shr 8) -band 0xFF)
    $a[$o + 2] = [byte](($v -shr 16) -band 0xFF)
    $a[$o + 3] = [byte](($v -shr 24) -band 0xFF)
}
function Put-BE32([byte[]]$a, [int]$o, [long]$v) {
    $a[$o]     = [byte](($v -shr 24) -band 0xFF)
    $a[$o + 1] = [byte](($v -shr 16) -band 0xFF)
    $a[$o + 2] = [byte](($v -shr 8) -band 0xFF)
    $a[$o + 3] = [byte]($v -band 0xFF)
}
function Put-Both16([byte[]]$a, [int]$o, [int]$v) {
    Put-LE16 $a $o $v
    Put-BE16 $a ($o + 2) $v
}
function Put-Both32([byte[]]$a, [int]$o, [long]$v) {
    Put-LE32 $a $o $v
    Put-BE32 $a ($o + 4) $v
}
function Put-Ascii([byte[]]$a, [int]$o, [string]$s) {
    for ($i = 0; $i -lt $s.Length; $i++) { $a[$o + $i] = [byte][char]$s[$i] }
}
function Fill-Spaces([byte[]]$a, [int]$o, [int]$n) {
    for ($i = 0; $i -lt $n; $i++) { $a[$o + $i] = 0x20 }
}
function Make-DirRecord([int]$extent, [int]$size, [bool]$isDir, [byte[]]$id) {
    $idLen = $id.Length
    $len = 33 + $idLen
    if (($len % 2) -ne 0) { $len++ }
    $r = New-Object byte[] $len
    $r[0] = [byte]$len
    $r[1] = 0
    Put-Both32 $r 2 $extent
    Put-Both32 $r 10 $size
    # recording date/time: 2026-10-27 12:00:00 GMT+0
    $r[18] = 126; $r[19] = 10; $r[20] = 27
    $r[21] = 12; $r[22] = 0; $r[23] = 0; $r[24] = 0
    $r[25] = if ($isDir) { 0x02 } else { 0x00 }
    $r[26] = 0; $r[27] = 0
    Put-Both16 $r 28 1
    $r[32] = [byte]$idLen
    [Array]::Copy($id, 0, $r, 33, $idLen)
    return $r
}
function Put-Sector([byte[]]$src, [int]$lba) {
    [Array]::Copy($src, 0, $iso, $lba * $SECTOR, $SECTOR)
}

Write-Host "Building ISO9660 + El Torito image..." -ForegroundColor Cyan

# --- Primary Volume Descriptor --------------------------------------------
$pvd = New-Object byte[] $SECTOR
$pvd[0] = 1
Put-Ascii $pvd 1 "CD001"
$pvd[6] = 1
Fill-Spaces $pvd 8 32
Put-Ascii $pvd 8 "MYOS"
Fill-Spaces $pvd 40 32
Put-Ascii $pvd 40 "MYOS_V31_PRERELEASE"
Put-Both32 $pvd 72 $TOTAL_SECTORS
Put-Both16 $pvd 80 1
Put-Both16 $pvd 88 1
Put-Both16 $pvd 96 $SECTOR
$pathTableSize = 10
Put-Both32 $pvd 104 $pathTableSize
Put-LE32 $pvd 112 $PATHL_LBA
Put-LE32 $pvd 116 0
Put-LE32 $pvd 120 $PATHM_LBA
Put-LE32 $pvd 124 0
# root directory record (34 bytes)
$rootRec = Make-DirRecord $ROOT_LBA $SECTOR $true ([byte[]](0))
[Array]::Copy($rootRec, 0, $pvd, 128, $rootRec.Length)
Fill-Spaces $pvd 190 128
Fill-Spaces $pvd 318 128
Fill-Spaces $pvd 446 128
Fill-Spaces $pvd 574 128
Fill-Spaces $pvd 702 37
Fill-Spaces $pvd 739 37
Fill-Spaces $pvd 776 37
Put-Ascii $pvd 318 "CEO FROMIX"
Put-Ascii $pvd 446 "MyOS project"
Put-Ascii $pvd 574 "MyOS 3.1 pre-release"
$date = "2026102712000000"
Put-Ascii $pvd 813 $date; $pvd[829] = 0
Put-Ascii $pvd 830 $date; $pvd[846] = 0
for ($i = 0; $i -lt 16; $i++) { $pvd[847 + $i] = [byte][char]'0' }
$pvd[863] = 0
for ($i = 0; $i -lt 16; $i++) { $pvd[864 + $i] = [byte][char]'0' }
$pvd[880] = 0
$pvd[881] = 1
Put-Sector $pvd $PVD_LBA

# --- Boot Record Volume Descriptor (El Torito) ----------------------------
$brvd = New-Object byte[] $SECTOR
$brvd[0] = 0
Put-Ascii $brvd 1 "CD001"
$brvd[6] = 1
Put-Ascii $brvd 7 "EL TORITO SPECIFICATION"   # zero padding follows
Put-LE32 $brvd 71 $CATALOG_LBA
Put-Sector $brvd $BRVD_LBA

# --- Volume Descriptor Set Terminator -------------------------------------
$term = New-Object byte[] $SECTOR
$term[0] = 255
Put-Ascii $term 1 "CD001"
$term[6] = 1
Put-Sector $term $TERM_LBA

# --- Boot Catalog ----------------------------------------------------------
$cat = New-Object byte[] $SECTOR
# validation entry
$cat[0] = 1
$cat[1] = 0
$cat[2] = 0
$cat[3] = 0
Put-Ascii $cat 4 "MyOS 3.1 pre-release"
$sum = 0
for ($i = 0; $i -lt 32; $i += 2) {
    $sum += $cat[$i] + ($cat[$i + 1] -shl 8)
}
$checksum = (0x10000 - ($sum -band 0xFFFF)) -band 0xFFFF
Put-LE16 $cat 28 $checksum
$cat[30] = 0x55
$cat[31] = 0xAA
# initial/default entry: 1.44 MB floppy emulation
$cat[32] = 0x88
$cat[33] = 0x02
Put-LE16 $cat 34 0
$cat[36] = 0
$cat[37] = 0
Put-LE16 $cat 38 1
Put-LE32 $cat 40 $BOOT_LBA
Put-Sector $cat $CATALOG_LBA

# --- Root directory --------------------------------------------------------
$readmeText = @"
MyOS 3.1 pre-release
Design by CEO FROMIX
You can be on the list of developers.
WARNING!!! Project now in pre-release

Bootable ISO: El Torito 1.44 MB floppy emulation.
The hard-disk image (MyOS.vdi) is shipped separately in the release.
"@
$readmeBytes = [Text.Encoding]::ASCII.GetBytes($readmeText)
$root = New-Object byte[] $SECTOR
$entries = New-Object System.Collections.Generic.List[byte]
$entries.AddRange([byte[]](Make-DirRecord $ROOT_LBA $SECTOR $true ([byte[]](0))))
$entries.AddRange([byte[]](Make-DirRecord $ROOT_LBA $SECTOR $true ([byte[]](1))))
$entries.AddRange([byte[]](Make-DirRecord $README_LBA $readmeBytes.Length $false ([Text.Encoding]::ASCII.GetBytes("README.TXT;1"))))
$entries.AddRange([byte[]](Make-DirRecord $BOOT_LBA $FLOPPY_BYTES $false ([Text.Encoding]::ASCII.GetBytes("BOOT.IMG;1"))))
$entryBytes = $entries.ToArray()
[Array]::Copy($entryBytes, 0, $root, 0, $entryBytes.Length)
Put-Sector $root $ROOT_LBA

# --- README ----------------------------------------------------------------
$readmeSector = New-Object byte[] $SECTOR
[Array]::Copy($readmeBytes, 0, $readmeSector, 0, $readmeBytes.Length)
Put-Sector $readmeSector $README_LBA

# --- Path tables -----------------------------------------------------------
$pathL = New-Object byte[] $SECTOR
$pathL[0] = 1
$pathL[1] = 0
Put-LE32 $pathL 2 $ROOT_LBA
Put-LE16 $pathL 6 1
$pathL[8] = 0
Put-Sector $pathL $PATHL_LBA

$pathM = New-Object byte[] $SECTOR
$pathM[0] = 1
$pathM[1] = 0
Put-BE32 $pathM 2 $ROOT_LBA
Put-BE16 $pathM 6 1
$pathM[8] = 0
Put-Sector $pathM $PATHM_LBA

# --- Boot image at a fixed LBA --------------------------------------------
[Array]::Copy($bootBytes, 0, $iso, $BOOT_LBA * $SECTOR, $FLOPPY_BYTES)

[IO.File]::WriteAllBytes($isoPath, $iso)
$hash = (Get-FileHash $isoPath -Algorithm SHA256).Hash
Write-Host "ISO created: $isoPath ($($iso.Length) bytes, $TOTAL_SECTORS sectors)" -ForegroundColor Green
Write-Host "SHA256: $hash" -ForegroundColor Gray
Write-Host "Boot image: El Torito 1.44 MB floppy emulation at LBA $BOOT_LBA" -ForegroundColor Gray
