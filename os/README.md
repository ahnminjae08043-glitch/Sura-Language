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
- repairs only the old cursor rectangle during ordinary pointer movement
  instead of presenting the complete framebuffer for every mouse packet
- keeps a readable 46x14 terminal history with wrapping, scrolling, and `clear`
- manages overlapping Terminal and System Information windows with focus,
  z-order, title-bar dragging, close-button input, and desktop bounds
- opens a Start menu and reopens closed windows from persistent taskbar buttons
- reads the CMOS RTC and renders an `HH:MM` taskbar clock
- runs kernel-owned File Explorer, Text Editor, and Calculator windows
- initializes QEMU's ICH9 AHCI controller after `ExitBootServices`, identifies
  the second SATA disk, reads its MBR, and mounts its FAT32 partition
- lists FAT32 root entries in File Explorer and loads and overwrites the fixed
  `NOTES.TXT` file from Text Editor
- persists the selected file and each managed window's position, visibility,
  z-order, and active state in `SETTINGS.CFG` and `DESKTOP.CFG`
- initializes the bitmap physical-page allocator from conventional memory
- allocates, writes, reads, and releases one physical page
- emits deterministic boot, memory, and kernel-ready markers
- runs a COM1 command shell and exits through QEMU's `isa-debug-exit` device

This is the first graphical desktop milestone, not a complete desktop
operating system. The terminal accepts PS/2 keyboard input in QEMU and COM1
remains available for diagnostics and automation. The mouse pointer moves, but
the left button now focuses, raises, drags, closes, and reopens managed windows.
The Start menu and taskbar buttons activate their matching windows. Resize,
minimize, and maximize are not available. The Start menu also exposes the
QEMU shutdown action.
There is no separate application process, network stack, or browser yet.
The current filesystem is a deliberately bounded FAT32 implementation: it
mounts one fixed QEMU data disk, reads short 8.3 names, and overwrites existing
fixed-length files. Creating, deleting, growing, renaming, or allocating files
is not implemented.

File Explorer shows the mounted data disk's root entries. Text Editor edits the
512-byte `NOTES.TXT` record and autosaves it through AHCI. Calculator accepts
keyboard digits, `+ - * / =`, Backspace, and `C`. QEMU verifies editor input,
the disk write, and the result of `50 - 8 = 42`; these remain kernel-owned
applications, not Ring 3 processes.

The freestanding libraries also contain compile-verified scheduler, interrupt,
user-process, ELF64, PCI/PCIe, ACPI, block, partition, FAT32, AHCI, and NVMe
building blocks. AHCI, MBR partition discovery, FAT32, framebuffer, PS/2,
desktop, and application code are executed by the current boot image. A
fixed-capacity window-manager foundation also implements
focus, z-order, hit testing, title-bar drag, close, and desktop-bound clamping.
Scheduler, Ring 3, ELF64, NVMe, and several interrupt foundations remain
compile-verified libraries and are not yet connected to this desktop boot path.

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

Interactive mode attaches `build\os\SuraData.img` directly, so Text Editor and
desktop-state changes survive the next run. Recreate a clean data disk with:

```powershell
.\tools\sura_os_data_disk.ps1 -Path .\build\os\SuraData.img -Force
```

Capture the actual QEMU framebuffer after the desktop marker:

```powershell
.\tools\sura_os_screenshot.ps1 -Engine .\build\SuraLanguage_user.exe
```

The final capture is written to `build\os\SuraOS-desktop.ppm`, the dragged
overlapping-window state to `build\os\SuraOS-windows.ppm`, and the open Start
menu to `build\os\SuraOS-start-menu.ppm`. The capture tool uses QEMU QMP,
focuses, drags, closes, and reopens System Information from the taskbar, opens
Start, opens and exercises the three built-in apps, writes
`build\os\SuraOS-apps.ppm`, activates Terminal, verifies the
desktop/window/app/terminal/storage markers, and then shuts the VM down
normally. The test normally works on a disposable data-disk copy. Preserve
that modified copy for a second-boot persistence check with:

```powershell
.\tools\sura_os_screenshot.ps1 `
  -Engine .\build\SuraLanguage_user.exe `
  -DataDiskOutput .\build\os\SuraData-persistence-test.img
```

Use `-DataDisk <path>` to capture a previously saved disk, and
`-SkipInputVerification` when only its restored desktop is needed.

The shell supports `help`, `status`, `mem`, `about`, `clear`, and `shutdown`. Use
`shutdown` to close QEMU normally. The non-interactive VM test sends `status`,
`mem`, and `shutdown` through COM1 and checks their output.

The script uses QEMU with TCG emulation and an EDK2 x86-64 firmware image. It
does not modify firmware boot entries or boot the host computer. Interactive
mode connects COM1 to an ephemeral loopback-only TCP port so PowerShell handles
line input normally; it does not expose the shell on an external interface.
