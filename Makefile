# Windows uses native PowerShell/CMake. Legacy POSIX targets remain available.
ifeq ($(OS),Windows_NT)
.DEFAULT_GOAL := all
.PHONY: all cli test check release release-windows release-windows-portable golden-update
all test check:
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build-windows.ps1 -Action Test
cli release release-windows:
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build-windows.ps1 -Action Build
release-windows-portable:
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build-windows.ps1 -Action Package
golden-update:
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/build-windows.ps1 -Action GoldenUpdate
else
include Makefile.posix
endif