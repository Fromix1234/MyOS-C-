<div align="center">

# 🖥️ DreamOS
### *The Future of Minimalist Operating Systems*

![DreamOS Banner](<banner_DreamOS_Монтажная область 1.jpg>)

</div>

**Pre-release · v3.2** · x86 · Legacy BIOS

A minimal operating system built from scratch — loader, kernel, filesystem and graphical shell.

---

## Overview

DreamOS uses a microkernel-inspired split: Ring 0 kernel (C++) provides drivers, syscalls and storage; Ring 3 shell (C++) provides the GUI. The boot chain is BIOS MBR → active partition VBR → stage2 → kernel → user tasks.

- **Boot**: Legacy BIOS only — no EFI/UEFI support yet
- **Architecture**: 32‑bit x86
- **Filesystem**: MyFS (custom, 8 files max, sector‑based)
- **UI**: “Liquid Glass” effect with modular windows

---

## Quick Start (VirtualBox)

### Pre-built images

- `DreamOS.vdi` — 32 MB hard‑disk image
- `DreamOS.iso` — bootable El Torito ISO (1.44 MB floppy emulation)

**VDI method**

1. Create VM: type **Other**, version **Other/32‑bit**, RAM **128 MB**
2. **Motherboard** → EFI **disabled**
3. **Storage** → IDE → add `DreamOS.vdi` as hard disk
4. Start the VM

**ISO method**

1. Create VM without hard disk (or keep existing)
2. **Storage** → IDE → DVD drive → select `DreamOS.iso`
3. **Boot Order** → optical drive first
4. Start the VM — stage2 loads the shell from the ISO

### Build from sources

```powershell
# Windows (PowerShell 5.1)
powershell -ExecutionPolicy Bypass -File build.ps1
powershell -ExecutionPolicy Bypass -File build_iso.ps1

# Linux / macOS
./build.sh              # kernel + shell + disk.img
./build.sh -r           # Rust kernel (nightly, i686-unknown-linux-gnu)
python3 tools/mkiso.py disk.img -o DreamOS.iso
VBoxManage convertfromraw disk.img DreamOS.vdi --format VDI
```

---

## Building blocks

| Component | Language / Tool | Role |
|-----------|----------------|------|
| Bootloader | NASM | MBR → VBR → stage2 |
| Kernel | C++ (GCC `-m32`) | Memory, syscalls, ATA, MyFS |
| Shell | C++ | Window manager, compositor, apps |
| Filesystem | MyFS | Custom, 8 files, sector‑based |
| Rust kernel | nightly i686‑unknown‑linux-gnu | Experimental |

---

## Development

- Source tree: `shell/`, `kernel/`, `boot/`, `docs/`
- Build scripts: `build.ps1`, `build_iso.ps1` (Windows); `build.sh` (Linux/macOS)
- Test scripts (legacy): see git history — most were removed in `release/dreamos-stable`

---

## Contact

Project maintained by [FROMIX](https://github.com/Fromix1234).

© 2026 DreamOS Project. All rights reserved.