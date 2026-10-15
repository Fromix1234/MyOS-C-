@echo off
REM ===========================================================================
REM  run.bat - build and run the OS in QEMU (just double-click this file)
REM ===========================================================================
REM  This simply calls build.ps1 which does EVERYTHING:
REM    - compiles the C++ kernel
REM    - finds the kernel entry point automatically
REM    - assembles boot.asm + stage2.asm
REM    - builds disk.img (boot + stage2 + kernel)
REM    - launches QEMU
REM
REM  To stop: close the QEMU window.
REM ===========================================================================

powershell -ExecutionPolicy Bypass -File "D:\C++\MyOS\build.ps1"

pause
