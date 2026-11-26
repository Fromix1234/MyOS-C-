# DreamOS 1.0.0 — Release Readiness

**Status:** Planning / BIOS baseline verified (see §7)  
**Target:** DreamOS 1.0.0 Complete  
**Boot targets:** Legacy BIOS + UEFI  
**Architecture target:** x86-64 (release requirement; current repository state must be verified before implementation)

This document records the initial repository inspection and defines the first release-readiness work. It is not a claim that DreamOS 1.0.0 is already implemented or tested.

## 1. Initial repository observations

Inspected the `main` branch on 2026-11-26.

- The repository default branch is `main`.
- The README identifies the project as pre-release and describes version 3.2.
- The documented VirtualBox setup explicitly disables EFI and uses Legacy BIOS.
- The documented build scripts target 32-bit x86: `build.sh` uses `-m32`, `elf_i386`, and the Rust target `i686-unknown-linux-gnu`.
- The current boot chain is documented and implemented as BIOS MBR -> active partition VBR -> stage2 -> kernel.
- The ISO instructions describe an El Torito image using 1.44 MB floppy emulation.
- The repository contains both C++ and Rust kernel code, a shell, disk/ISO build scripts, and a large set of root-level PowerShell test scripts.
- The README describes Rust kernel functionality and a modular Aqua UI, but the actual release capabilities still need to be verified by building and running the current source.

## 2. Release-critical gaps to investigate

These are investigation items, not yet confirmed defects:

1. **UEFI boot path:** no UEFI-specific EFI application or EFI System Partition workflow is evident in the inspected root-level boot chain. UEFI support must be designed and implemented, not assumed.
2. **x86-64 target:** current build scripts visibly target i686 / 32-bit x86. The intended architecture for 1.0.0 must be reconciled with the x86-64 release target.
3. **Build reproducibility:** both Windows and Linux/macOS build paths need clean-build verification and documented toolchain versions.
4. **Test organization:** root-level `test_*.ps1` scripts need classification. Preserve useful regression tests; move or remove only confirmed obsolete experiments.
5. **Release artifacts:** ISO creation, boot mode, installation flow, checksums, and artifact naming need a repeatable release pipeline.
6. **Documentation consistency:** README instructions, version labels, architecture claims, and boot support must match tested behavior.

## 3. Work packages and acceptance criteria

### R0 — Baseline and inventory
- Record the current commit and branch heads.
- Inventory source modules, build scripts, tests, and generated artifacts.
- Classify each test as regression, diagnostic, experiment, or obsolete.
- Acceptance: every tracked source/test file has an understood role; no files are deleted without evidence.

### R1 — Reproducible BIOS build
- Build from a clean checkout using the documented toolchain.
- Run the resulting image in a BIOS virtual machine.
- Acceptance: clean build succeeds; image boots; logs and exact toolchain versions are recorded.

### R2 — UEFI boot support
- Define a UEFI boot architecture and EFI System Partition layout.
- Implement an x86-64 EFI loader or an explicitly selected UEFI-compatible bootloader.
- Pass the firmware memory map and boot information to the kernel.
- Acceptance: UEFI VM boots the same supported kernel build and reaches a documented success state.

### R3 — Architecture decision and x86-64 migration
- Decide whether 1.0.0 is x86-64-only or whether 32-bit BIOS support is retained as a separate target.
- Align bootloader, kernel, linker scripts, ABI, drivers, and build scripts.
- Acceptance: architecture matrix is documented; every advertised target has a clean build and boot test.

### R4 — Kernel, drivers, and storage
- Define a minimum supported hardware profile.
- Test memory management, interrupts, input, framebuffer, storage, and filesystem operations.
- Acceptance: required tests pass; filesystem read/write survives reboot; unsupported hardware is documented.

### R5 — Desktop and system applications
- Verify shell startup, window management, input, terminal, file operations, and system tools.
- Acceptance: all advertised core desktop workflows pass repeatable tests.

### R6 — Installer
- Implement and test safe target-disk selection, partitioning, installation, bootloader setup, and first boot.
- Acceptance: installation succeeds in BIOS and UEFI test VMs; destructive actions require explicit confirmation; installed system boots without installation media.

### R7 — Release candidate and publication
- Freeze features, fix release blockers, build clean artifacts, generate SHA-256 checksums, and publish release notes.
- Acceptance: zero open release-blocking Critical/Major defects; all required test scenarios pass; artifacts and checksums are independently verified.

## 4. Release gates

DreamOS 1.0.0 must not be marked Stable until all of the following are true:

- [ ] Clean reproducible build for every advertised boot target.
- [ ] Legacy BIOS boot verified.
- [ ] UEFI boot verified.
- [ ] Architecture and hardware support matrix published.
- [ ] Kernel and storage regression tests pass.
- [ ] Desktop acceptance tests pass.
- [ ] Installer tested in both boot modes.
- [ ] Installed system boots without installation media.
- [ ] No open release-blocking Critical or Major defects.
- [ ] README, installation guide, troubleshooting guide, and release notes match the tested build.
- [ ] SHA-256 checksums published for release artifacts.

## 5. Safety rules for repository cleanup

- Do not delete files merely because their names contain `test`.
- Do not rewrite the boot chain or kernel before capturing a reproducible baseline.
- Keep release work on a dedicated branch and merge only reviewed, tested changes.
- Record every removed or replaced file and the reason.
- Never publish an untested build as Stable.

## 6. Immediate next actions

1. Complete the recursive source/test inventory.
2. Inspect the full BIOS and ISO build pipeline.
3. Run a clean build in an available toolchain environment.
4. Inspect the Rust kernel target, linker script, and entry ABI.
5. Draft the UEFI architecture before changing the existing BIOS path.

## 7. Baseline verification — 2026-11-26

The release branch was consolidated before any code changes so that all
subsequent work starts from one tested point.

### 7.1 Branch consolidation

- Base: `release/1.0.0-readiness` at `4306c9e` (`main` + this document).
- Merged `release/dreamos-stable` (39 commits removing legacy test scripts).
- Merged `migration/dreamos-full` (`d74ceeb`, DreamOS Glass palettes in
  `shell/s0_prelude.cpp` and `shell/s1_ui.cpp`).
- Added UTF-8 BOM to `build.ps1`, `build_iso.ps1`, `font_gen.ps1`, and
  `ppm2png.ps1`. Without the BOM, Windows PowerShell 5.1 decodes Cyrillic
  UTF-8 as the system ANSI code page and fails to parse the scripts.
- Consolidated head: `a74f86b`.

### 7.2 Reference build environment

| Tool | Version |
| :--- | :--- |
| Windows | 10.0.26200 |
| GCC / g++ (w64devkit) | 16.2.0 |
| GNU ld | 2.47.20260726 |
| NASM | 3.02 |
| Python | 3.11.9 |
| Rust / Cargo | 1.99.1 |
| VirtualBox | 7.2.22 r175245 |

NASM was installed at `%LOCALAPPDATA%\bin\NASM` but was not on `PATH`;
the build was run with that directory prepended to `PATH`.

### 7.3 Verified BIOS build and boot

- Command: `powershell -ExecutionPolicy Bypass -File build.ps1 -NoRun`.
- Result: build completed; `kernel.bin` = 70,860 bytes (140 sectors);
  `disk.img` = 33,554,432 bytes (32 MiB).
- Boot test: VirtualBox 7.2.22, BIOS firmware, 128 MiB RAM, IDE HDD.
- Observed: desktop reached (`MyOS 3.2 pre-release`, Night Ridge wallpaper,
  menu bar, clock, dock with Terminal/Files/Snake/Settings/About icons).
- Not yet verified: ISO build, Linux/macOS build, Rust kernel build,
  UEFI boot, installer.

### 7.4 Test inventory status

`release/dreamos-stable` removed 39 legacy `test_*.ps1` and `mcp/*` scripts
without the classification required by R0. They remain recoverable from git
history (`e9a1cde` and earlier), but the release branch currently ships with
no automated regression tests. R0 is therefore **not complete**: the removed
scripts still need to be classified as regression, diagnostic, experiment, or
obsolete, and any regression tests worth keeping must be restored under a
documented test layout.

### 7.5 Updated work-package status

- **R0:** in progress — branch baseline recorded; test classification pending.
- **R1:** partially verified — Windows BIOS build and boot pass; clean-build
  repeatability, ISO, and non-Windows paths pending.
- **R2–R7:** unchanged.
