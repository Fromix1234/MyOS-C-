# Host-side verification of the boot-from-partition layout (reads disk.img directly)
# Raw layout: [LBA0: MBR][LBA1: VBR][LBA2-3: stage2 (2 sectors)][LBA4+: kernel]
# Partition boot area: +0 VBR, +1..+2 stage2, +3.. kernel
# P1 installed at LBA 2048, P2 installed at LBA 18432 (base field at +504)
$d = 'D:\C++\MyOS'
$b = [System.IO.File]::ReadAllBytes("$d\disk.img")
$ksize = (Get-Item "$d\kernel.bin").Length
$csec = [int][Math]::Ceiling($ksize / 512) + 1
Write-Output "kernel.bin = $ksize bytes -> install copies $csec sectors"

Write-Output ("MBR P1 flag = 0x{0:X2}  (0x80 = active)" -f $b[446])
Write-Output ("MBR P2 flag = 0x{0:X2}  (0x80 = active)" -f $b[462])
Write-Output ("raw MBR sig = {0:X2}{1:X2}" -f $b[511], $b[510])
Write-Output ("raw VBR @LBA1: base={0} sig={1:X2}{2:X2}" -f [BitConverter]::ToUInt32($b, 512+504), $b[512+511], $b[512+510])

foreach ($lba in 2048, 18432) {
    $o = $lba * 512
    $base = [BitConverter]::ToUInt32($b, $o + 504)
    $sig = "{0:X2}{1:X2}" -f $b[$o + 511], $b[$o + 510]
    Write-Output ("Installed @$lba : base={0} sig={1}" -f $base, $sig)

    $vc = $true
    for ($i = 0; $i -lt 504; $i++) { if ($b[512 + $i] -ne $b[$o + $i]) { $vc = $false; break } }
    Write-Output "    VBR code == raw VBR : $vc"

    $s2 = $true
    for ($i = 0; $i -lt 1024; $i++) { if ($b[1024 + $i] -ne $b[$o + 512 + $i]) { $s2 = $false; break } }
    Write-Output "    stage2 (1024 B) == raw stage2 : $s2"

    $k = $true
    for ($i = 0; $i -lt $csec * 512; $i++) { if ($b[2048 + $i] -ne $b[$o + 1536 + $i]) { $k = $false; break } }
    Write-Output "    kernel == raw kernel : $k"
}
