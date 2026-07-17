# Sura OS

This directory contains the bootable QEMU/OVMF integration image for Sura's
experimental freestanding x86-64 target.

`sura_os.sura` currently performs these operations in a virtual machine:

- starts as a UEFI x86-64 image generated directly from Sura source
- records the GOP framebuffer and allocates a 64 MiB-bounded backbuffer
- initializes the COM1 serial port
- obtains the UEFI memory map and calls `ExitBootServices`
- renders pixels, lines, rectangles, icons, and 5x7 bitmap text
- presents a double-buffered 1280x800-tested desktop with overlapping Terminal
  and System Information windows, desktop icons, and a taskbar
- polls the emulated i8042 controller for translated PS/2 keyboard scan codes
  and three-byte mouse packets
- accepts commands in the active graphical terminal and moves a software
  pointer
- keeps a readable 46x14 terminal history with wrapping, scrolling, and `clear`
- manages overlapping Terminal and System Information windows with focus,
  z-order, title-bar dragging, close-button input, and desktop bounds
- initializes the bitmap physical-page allocator from conventional memory
- allocates, writes, reads, and releases one physical page
- emits deterministic boot, memory, and kernel-ready markers
- runs a COM1 command shell and exits through QEMU's `isa-debug-exit` device

This is the first graphical desktop milestone, not a complete desktop
operating system. The terminal accepts PS/2 keyboard input in QEMU and COM1
remains available for diagnostics and automation. The mouse pointer moves, but
the left button now focuses, raises, drags, and closes managed windows.
Resize, minimize, maximize, reopen, and taskbar-button input are not available.
There is no application process, persistent desktop filesystem, network stack,
or browser yet.

The freestanding libraries also contain compile-verified scheduler, interrupt,
user-process, ELF64, PCI/PCIe, ACPI, block, partition, FAT32, AHCI, and NVMe
building blocks. A fixed-capacity window-manager foundation also implements
focus, z-order, hit testing, title-bar drag, close, and desktop-bound clamping.
The boot image executes that window manager for its two current windows. The
other listed subsystems are not all executed by this boot image.

Build and run the VM test from the repository root:

```powershell
.\tools\sura_os_vm.ps1 -Engine .\build\SuraLanguage_user.exe
```

Start an interactive serial shell:

```powershell
.\tools\sura_os_vm.ps1 -Engine .\build\SuraLanguage_user.exe -Interactive
```

Interactive mode opens the graphical QEMU window. Keep the PowerShell window
focused when entering serial-shell commands. For terminal-only automation:

```powershell
.\tools\sura_os_vm.ps1 -Engine .\build\SuraLanguage_user.exe `
  -Interactive -HeadlessInteractive
```

Capture the actual QEMU framebuffer after the desktop marker:

```powershell
.\tools\sura_os_screenshot.ps1 -Engine .\build\SuraLanguage_user.exe
```

The final capture is written to `build\os\SuraOS-desktop.ppm`, and the dragged
overlapping-window state is written to `build\os\SuraOS-windows.ppm`. The
capture tool uses QEMU QMP, requires the desktop, window-manager, and PS/2
ready markers, types through the emulated keyboard, focuses and drags the
System Information window, captures it, closes it, verifies the window and
terminal markers, and then shuts the VM down normally.

The shell supports `help`, `status`, `mem`, `about`, `clear`, and `shutdown`. Use
`shutdown` to close QEMU normally. The non-interactive VM test sends `status`,
`mem`, and `shutdown` through COM1 and checks their output.

The script uses QEMU with TCG emulation and an EDK2 x86-64 firmware image. It
does not modify firmware boot entries or boot the host computer. Interactive
mode connects COM1 to an ephemeral loopback-only TCP port so PowerShell handles
line input normally; it does not expose the shell on an external interface.
