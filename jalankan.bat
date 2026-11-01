@echo off
REM Menjalankan A6 di QEMU (Windows). Ubah path jika QEMU diinstal di folder lain.
set QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
%QEMU% -kernel a6.elf
pause
