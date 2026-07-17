# Sura OS

This directory contains the bootable QEMU/OVMF integration image for Sura's
experimental freestanding x86-64 target.

`sura_os.sura` currently performs these operations in a virtual machine:

- starts as a UEFI x86-64 image generated directly from Sura source
- records the GOP framebuffer and draws a header after firmware services end
- initializes the COM1 serial port
- obtains the UEFI memory map and calls `ExitBootServices`
- initializes the bitmap physical-page allocator from conventional memory
- allocates, writes, reads, and releases one physical page
- emits deterministic boot, memory, and kernel-ready markers
- exits through QEMU's `isa-debug-exit` test device

It is a minimal kernel integration test, not a desktop operating system. The
freestanding libraries also contain compile-verified scheduler, interrupt,
user-process, ELF64, PCI/PCIe, ACPI, block, partition, FAT32, AHCI, and NVMe
building blocks. Those subsystems are not all executed by this boot image yet.

Build and run the VM test from the repository root:

```powershell
.\tools\sura_os_vm.ps1 -Engine .\build\SuraLanguage_user.exe
```

Start an interactive serial shell:

```powershell
.\tools\sura_os_vm.ps1 -Engine .\build\SuraLanguage_user.exe -Interactive
```

The shell supports `help`, `status`, `mem`, `about`, and `shutdown`. Use
`shutdown` to close QEMU normally. The non-interactive VM test sends `status`,
`mem`, and `shutdown` through COM1 and checks their output.

The script uses QEMU with TCG emulation and an EDK2 x86-64 firmware image. It
does not modify firmware boot entries or boot the host computer. Interactive
mode connects COM1 to an ephemeral loopback-only TCP port so PowerShell handles
line input normally; it does not expose the shell on an external interface.
