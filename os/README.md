# Sura OS

This directory contains the bootable QEMU/OVMF integration image for Sura's
experimental freestanding x86-64 target.

`sura_os.sura` currently performs these operations in a virtual machine:

- starts as a UEFI x86-64 image generated directly from Sura source
- records the GOP framebuffer and allocates a 64 MiB-bounded backbuffer
- initializes the COM1 serial port
- obtains the UEFI memory map and calls `ExitBootServices`
- renders pixels, lines, rectangles, embedded 16x16 raster image icons, and
  5x7 bitmap text
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
- runs kernel-rendered File Explorer, Text Editor, Calculator, and Text Browser
  windows; Text Editor and Calculator state transitions execute at CPL 3
  through bounded mailboxes
- initializes QEMU's ICH9 AHCI controller after `ExitBootServices`, identifies
  the second SATA disk, reads its MBR, and mounts its FAT32 partition
- lists FAT32 root entries, enters the generated `DOCS` subdirectory, shows
  the current path and parent navigation in File Explorer, and loads and
  overwrites the fixed `NOTES.TXT` file from Text Editor
- persists the selected file and each managed window's position, visibility,
  z-order, and active state in `SETTINGS.CFG` and `DESKTOP.CFG`
- initializes a legacy-compatible VirtIO-net PCI device with caller-owned
  split virtqueues and DMA receive/transmit buffers
- completes DHCP Discover, Offer, Request, and ACK with QEMU user networking,
  then uses the leased IPv4 address, subnet mask, gateway, and DNS server
- exchanges real Ethernet ARP packets, builds and validates IPv4 headers,
  sends UDP DNS queries, completes TCP three-way handshakes, and receives
  HTTP/1.0 responses
- tokenizes a bounded HTML body, lays out `h1`, `p`, `br`, and `a` content with
  distinct heading, paragraph, and link styles, and renders it in the movable
  `TEXT BROWSER` window
- reads a bounded CSS subset for `body`, `div`, `h1`, and `a` selectors,
  applying `background`, `background-color`, and `color` with three- or
  six-digit hexadecimal values
- accepts a hostname or `http://` URL in the active browser address bar and
  performs a new DNS, TCP, and HTTP request when Enter is pressed
- initializes the bitmap physical-page allocator from conventional memory
- allocates, writes, reads, and releases one physical page
- emits deterministic boot, memory, and kernel-ready markers
- runs a COM1 command shell and exits through QEMU's `isa-debug-exit` device

This is the first graphical desktop milestone, not a complete desktop
operating system. The terminal accepts PS/2 keyboard input in QEMU and COM1
remains available for diagnostics and automation. The mouse pointer moves, but
the left button now focuses, raises, drags, closes, and reopens managed windows.
The Start menu, desktop icons, and taskbar buttons activate their matching
windows. Resize,
minimize, and maximize are not available. The Start menu also exposes the
QEMU shutdown action.
There is no scheduled or preemptive application process yet. Text Editor and
Calculator logic run at CPL 3 in separate `ProcessAddressSpace` roots with
dedicated executable, mailbox, guarded-stack, and page-table pages, then return
synchronously after each input.
The current filesystem is a deliberately bounded FAT32 implementation: it
mounts one fixed QEMU data disk, reads short 8.3 names, and overwrites existing
fixed-length files. Creating, deleting, growing, renaming, or allocating files
is not implemented.

The network path is also deliberately bounded. It obtains IPv4 configuration
through DHCP, polls VirtIO queues, performs DNS A-record queries and HTTP/1.0
GET requests, and uses fixed-size packet and response buffers. Browser input
supports hostnames and `http://` URLs with paths. It does not yet provide IPv6,
TCP retransmission or congestion control, TLS/HTTPS, ports other than HTTP 80,
cookies, JavaScript, nested DOM layout, images, forms, or general CSS cascade,
box-model, flex, and grid layout.

File Explorer shows the mounted data disk's root entries and supports bounded
click navigation into short-name FAT32 directories and back to their parent.
The default image includes `DOCS/README.TXT`. Text Editor edits the 512-byte
`NOTES.TXT` record and autosaves it through AHCI. Calculator accepts keyboard
digits, `+ - * / =`, Backspace, and `C`. QEMU verifies directory traversal,
editor input, the disk write, and the result of `50 - 31 = 19`. File Explorer,
and Text Browser logic remains in the kernel. Text Editor and Calculator
rendering also remains in the kernel, while each state-transition function is
copied to process-owned read-only executable pages and entered at CPL 3 for
each key input. Only each worker's mailbox and guarded stack are writable from
CPL 3. Each process CR3 shares kernel mappings with U/S cleared and reserves a
different lower-half PML4 slot for user pages. FAT32 autosave runs only after
the Text Editor worker has returned to the kernel root.

The freestanding libraries also contain scheduler, interrupt, user-process,
ELF64, PCI/PCIe, ACPI, block, partition, FAT32, AHCI, and NVMe building blocks.
AHCI, MBR partition discovery, FAT32, VirtIO-net,
Ethernet/ARP/IPv4/UDP/DNS/TCP/HTTP, framebuffer, PS/2, desktop, and application
code are executed by the current boot image. A
fixed-capacity window-manager foundation also implements
focus, z-order, hit testing, title-bar drag, close, and desktop-bound clamping.
The separate Ring 3 QEMU gate executes an IRETQ transition to CPL 3, checks the
saved CS through a DPL-3 `INT 0x80`, returns with IRETQ, and re-enters the kernel
through `SYSCALL`. The desktop boot path additionally executes Calculator state
and Text Editor state transitions at CPL 3. `SURA_OS_CALCULATOR_CR3_OK` and
`SURA_OS_EDITOR_CR3_OK` are emitted only after the interrupt handler observes
the matching worker CR3 and switches back to the distinct kernel root.
`SURA_OS_CALCULATOR_RING3_OK` and `SURA_OS_EDITOR_RING3_OK` prove completed
mailbox round trips. These synchronous address-space switches do not yet use
the compile-verified `UserProcessScheduler`; ELF64 execution, timer preemption,
NVMe, and several interrupt foundations are not connected to the desktop boot
path.

Run the executed Ring 3 and syscall gate:

```powershell
.\tools\sura_ring3_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
```

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

Interactive mode runs the data disk from a temporary ASCII-only path so QEMU
works when the repository path contains Korean characters. A normal
`shutdown` copies the modified disk back to `build\os\SuraData.img`, so Text
Editor and desktop-state changes survive the next run. Recreate a clean data
disk with:

```powershell
.\tools\sura_os_data_disk.ps1 -Path .\build\os\SuraData.img -Force
```

Capture the actual QEMU framebuffer after the desktop marker:

```powershell
.\tools\sura_os_screenshot.ps1 -Engine .\build\SuraLanguage_user.exe
```

The final capture is written to `build\os\SuraOS-desktop.ppm`, the dragged
overlapping-window state to `build\os\SuraOS-windows.ppm`, the open Start menu
to `build\os\SuraOS-start-menu.ppm`, and the foreground HTTP text browser to
`build\os\SuraOS-browser.ppm`. The capture tool uses QEMU QMP,
focuses, drags, closes, and reopens System Information from the taskbar, opens
Start, opens and exercises the three built-in apps, enters `DOCS` in File
Explorer, writes `build\os\SuraOS-apps.ppm`, activates the browser, types
`example.com/`,
performs a second live navigation, activates Terminal, verifies the
desktop/window/app/terminal/storage/DHCP/network/DNS/TCP/HTTP/browser markers,
and then shuts the VM down
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
