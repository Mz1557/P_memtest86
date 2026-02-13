UEFI MemTest Prototype Build Guide

1) Prerequisites
- EDK2 workspace configured
- GCC5 toolchain (or VS toolchain on Windows)
- UEFI x64 target platform

2) Place source
- Copy this folder into your EDK2 workspace package, e.g.
  edk2/MyPkg/UefiMemtestApp/

3) Add module to DSC
- In your platform DSC, add INF entry:
  MyPkg/UefiMemtestApp/UefiMemtestApp.inf

4) Build example
- Linux example commands:
  cd edk2
  source edksetup.sh
  build -a X64 -t GCC5 -p MyPkg/MyPkg.dsc -m MyPkg/UefiMemtestApp/UefiMemtestApp.inf

5) Output
- EFI binary path is typically:
  Build/<PlatformName>/X64/UefiMemtestApp.efi

6) USB layout (UEFI default boot path)
- Format USB as FAT32
- Copy binary to:
  EFI/BOOT/BOOTX64.EFI

7) Boot notes
- If Secure Boot is enabled, unsigned EFI binaries may be blocked.
- Disable Secure Boot or sign the binary with enrolled key.

8) Current limitations of this prototype
- Sweeps EfiConventionalMemory ranges only (not firmware-reserved/runtime regions).
- Pattern is checkerboard fill/verify only.
- No IMC register parsing / SPD decode / real rowhammer adjacency engine yet.
