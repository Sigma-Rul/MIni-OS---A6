@echo off
REM Menjalankan 6S di QEMU (Windows). Ubah path jika QEMU diinstal di folder lain.
set QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
%QEMU% -kernel 6s.elf
pause
