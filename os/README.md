# Sura OS

This directory contains the bootable QEMU/OVMF integration image for Sura's
experimental freestanding x86-64 target.

`sura_os.sura` currently performs these operations in a virtual machine:

- starts as a UEFI x86-64 image generated directly from Sura source
- records the GOP framebuffer and allocates a 64 MiB-bounded backbuffer
- initializes QEMU's modern VirtIO GPU PCI capabilities and control queue,
  mirrors the completed GOP surface to a 2D scanout, and keeps GOP as a
  startup fallback
- initializes QEMU's Intel HDA controller and output codec, transfers a
  bounded 48-kHz signed-16 stereo startup slice through one BDL, and keeps
  booting when the device is absent
- discovers conventional ACPI S5 and FADT reset registers before leaving
  Boot Services, then uses them for shell and desktop power actions
- initializes the COM1 serial port
- obtains the UEFI memory map and calls `ExitBootServices`
- renders pixels, lines, rectangles, embedded 16x16 raster image icons, and
  5x7 bitmap text
- presents a double-buffered 1280x800-tested desktop with overlapping Terminal
  and System Information windows, desktop icons, and a taskbar
- enumerates QEMU USB boot keyboard and mouse devices through xHCI, keeps
  interrupt-IN transfers posted, and retains the polling i8042 PS/2 path as a
  fallback with IntelliMouse four-byte wheel negotiation
- converts keyboard press, release, repeat, scan-code, Unicode, and modifier
  state into `KeyEvent` records and routes them through a fixed 64-entry
  kernel-owned input queue
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
  windows; System Information snapshot validation, Terminal command-line
  editing and command recognition, and File Explorer, Text Editor, and
  Calculator state transitions execute at CPL 3 through bounded mailboxes
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
  HTTP/1.1 responses framed by `Content-Length`, chunked transfer coding, or
  connection close; gzip content coding is decoded before HTML processing
- tokenizes a bounded HTML body, lays out `h1`, `p`, `br`, and `a` content with
  distinct heading, paragraph, and link styles, and renders it in the movable
  `TEXT BROWSER` window
- reads a bounded CSS subset for `body`, `div`, `h1`, and `a` selectors,
  applying `background`, `background-color`, and `color` with three- or
  six-digit hexadecimal values
- accepts a hostname, `http://` URL, or `https://` URL in the active browser
  address bar, selects that field by pointer or F6, performs a new DNS and TCP
  request when Enter is pressed, and follows at most five absolute,
  protocol-relative, or root-relative redirects
- scrolls DOM boxes with mouse wheel, arrows, Page Up/Down, Home, and End;
  resolves clicked anchor boxes to their owning `<a>` node; and repeats held
  printable, navigation, and UTF-8 Backspace keys after a bounded delay
- initializes the bitmap physical-page allocator from conventional memory
- allocates, writes, reads, and releases one physical page
- emits deterministic boot, memory, and kernel-ready markers
- runs a COM1 command shell with ACPI `shutdown` and `reboot`; QEMU's
  `isa-debug-exit` remains an unsupported-firmware fallback

This is the first graphical desktop milestone, not a complete desktop
operating system. The normal VM uses polling xHCI USB keyboard and mouse input;
PS/2 remains an executed fallback and COM1 remains available for diagnostics
and automation. The mouse pointer moves, and the left button focuses, raises,
drags, resizes, closes, and reopens managed windows.
The Start menu, desktop icons, and taskbar buttons activate their matching
windows. Title-bar controls and F8/F9/F10/F11 provide resize, minimize,
maximize, fullscreen, and restore. The Start menu also exposes the ACPI
shutdown action.
System Information, Terminal, File Explorer, Text Editor, Calculator, a
Window Server, and a Browser request validator run as seven persistent CPL-3
processes in one single-CPU
`UserProcessScheduler`. Each
has a separate `ProcessAddressSpace` root, event queue, kernel stack,
executable mapping, mailbox, guarded user stack, and page-table pages. The
Window Server has a distinct CR3, a bounded damage compositor, and a writable
shared mapping of the real desktop backbuffer. The kernel wakes the requested
worker and normally waits for it to block again. Full-size built-in app
surfaces and complete user-space composition are not implemented.
The 128 MiB QEMU data disk keeps its original FAT32 partition for
`SETTINGS.CFG`, `DESKTOP.CFG`, and migration compatibility, and adds a type
`0x7f` partition at LBA 131072 for native SuraFS. Existing 64 MiB images are
extended in place without changing the FAT32 bytes after the MBR. The
graphical File Explorer mounts SuraFS through the common VFS, renders UTF-8
names with the Hangul font, enters `/문서`, and opens `/문서/메모.txt` in Text
Editor. Text Editor saves the selected SuraFS file, and the next boot mounts
the same generation and reloads the saved bytes.

SuraFS remains deliberately bounded. The graphical format uses 32 nodes,
64-byte UTF-8 name slots, 4096-byte per-file data slots, and alternating
whole-bank copy-on-write commits with CRC32 validation.
`tools/sura_surafs_qemu_gate.ps1` corrupts the newest bank and verifies
fallback to the prior generation. `tools/sura_surafs_gui_qemu_gate.ps1`
drives the graphical editor and explorer, requires the exact 43-byte
`Welcome to Sura OS Notes.sura notes\n한글 ` UTF-8 payload, boots the preserved
disk again, checks the restored length and weighted checksum reported by the
OS, verifies file/folder creation, Korean naming, rename, recycle-bin transfer,
and an exact `copy.sura` Save As duplicate with syntax-color activation, and
requires the complete SuraFS
partition hash and generation to stay unchanged across that remount-only
second boot.

The shared FAT32 library separately supports short-name creation, cluster
allocation/free, resize, partial I/O, rename, deletion, and checked VFAT long
names. `tools/sura_fat32_mutation_qemu_gate.ps1` executes those operations,
while `fat32_vfs.sura` separately covers `/문서/메모.txt` through the common
VFS. Those FAT32 mutation APIs are not the graphical desktop's primary
document store.

The network path is deliberately bounded. It obtains IPv4 configuration
through DHCP, polls VirtIO queues, performs DNS A-record queries, and uses
fixed-size packet buffers plus a 2-MiB response buffer. Browser input supports
ASCII hostnames and both `http://` and `https://` URLs with paths. A hostname
without a scheme uses HTTPS; plain HTTP requires an explicit `http://`. The HTTPS
profile is TLS 1.3 with `TLS_AES_128_GCM_SHA256`, X25519,
RSA-PSS-RSAE-SHA256 CertificateVerify, RSA/SHA-256 or RSA/SHA-384 certificate
chains, HTTP/1.1 ALPN, certificate validity and hostname checks, and six pinned
trust anchors: ISRG Root X1, DigiCert Global Root G2, GlobalSign Root CA - R3,
Amazon Root CA 1, USERTrust RSA Certification Authority, and Microsoft RSA
Root Certificate Authority 2017. `tools/sura_trust_store_generate.ps1` emits
indexed DER, length, and SHA-256 accessors, and the HTTPS initializer accepts
between one and sixteen generated roots without another per-root TLS-code
change. Only the six roots listed above are currently shipped, so this is not
a broad operating-system root store. `tools/sura_sha384_qemu_gate.ps1`
executes SHA-384 known-answer and padding-boundary tests after
`ExitBootServices`. The trust-store gate verifies each root fingerprint,
policy, and self-signature, including RSA/SHA-384 roots, and rejects a
one-byte signature mutation for every root. The QEMU screenshot gate has
executed a direct `suralang.site` HTTPS navigation and received and
rendered its encrypted response. Requests advertise
`Accept-Encoding: gzip, identity`. The shared
HTTP/HTTPS response path decodes bounded gzip streams containing stored,
fixed-Huffman, or dynamic-Huffman DEFLATE blocks and verifies CRC32 and ISIZE.
Other content codings are rejected. The response framer rejects
ambiguous `Transfer-Encoding` plus `Content-Length`, duplicate framing headers,
unsupported transfer codings, malformed chunk sizes, and incomplete bodies.
Separate freestanding PNG and baseline JPEG libraries now decode bounded image
bytes into RGBA8 and have post-`ExitBootServices` QEMU pixel gates. They are
connected to the Text Browser's bounded two-slot `<img>` resource path.
The freestanding HTML DOM and CSS computed-style foundations also have
post-`ExitBootServices` QEMU gates. The CSS subset applies tag, class, ID, and
inline rules with source-order/specificity cascade; inherited color/font
values; display; pixel and bounded relative width/height; min/max dimensions;
margin, padding, border width including compound `border` and directional
border shorthand widths; and box-sizing. Supported relative values are width
percentages, viewport width/height, and `calc(<percent> - <px>)` for widths. A
horizontal `margin:auto` value distributes remaining width and centers boxes
when both sides are automatic. A separate box-layout foundation computes content/border/outer
geometry, vertical block flow, basic inline flow and wrapping, and hidden
subtree removal. It lays out relative, absolute, and fixed boxes with signed
pixel offsets; absolute/fixed boxes do not consume normal block, flex, or grid
flow space. The Text Browser uses these modules with bounded
1,024-node/attribute/style/box storage, collects inline CSS, fetches one
root-relative same-host stylesheet, and paints visible backgrounds, borders,
and UTF-8 text. The browser now opens in a work-area-sized window instead of
the earlier 720-by-420 test window. Its relaxed stylesheet parser accepts bounded
compound, descendant, and direct-child selectors and skips unsupported at-rules
and selectors. It also resolves up to 32 global `:root` custom properties into
matching `var(--name)` uses, accepts 4/8-digit hex colors with alpha discarded,
and extracts the final supported hex color from a compound background value.
The complete QEMU screenshot gate fetches, decodes, lays out, and paints a
large PNG from live `suralang.site`; it requires PNG decode, image-render, and
CSS-variable and positioned-style markers. Flex supports bounded direction, wrapping, gaps, main-axis
distribution and cross-axis placement. Grid accepts up to 16 explicit or
`repeat(count, ...)` equal-width columns with gaps. These are not complete
flex/grid algorithms. A bounded `clamp(px, vw, px)` font-size value and
unitless decimal line-height are resolved for box geometry. General
variable-size web-font rendering remains unimplemented. Advanced text layout
remains unimplemented. Sticky positioning, z-index/stacking contexts,
transforms, complete containing-block behavior, and overflow clipping remain
unimplemented.
Text/password inputs, checkboxes, buttons, pointer focus, and bounded mutable
form values are connected to the DOM box hit-test path. The allocation-free
`browser_form.sura` layer serializes successful named controls as
`application/x-www-form-urlencoded`, excludes disabled controls and unchecked
checkboxes, uses `on` for a checked checkbox without a value, and supports
Enter or submit-button activation. Default/GET forms append the encoded payload
to the action URL; POST forms send it as an HTTP/1.1 request body. Actions are
the current URL, an absolute HTTP(S) URL, or a root-relative URL. POST preserves
its body across 307/308 redirects and changes to GET after 301/302/303. The
dedicated form-state gate verifies UTF-8 editing, encoding, capacity failure,
and successful-control selection, while the complete screenshot gate executes
and renders the built-in `sura.local/forms` POST flow. Radio groups, textarea,
select, file upload, multipart bodies, validation, cookies/sessions, and the
complete HTML form model are not implemented.
The browser also has an allocation-free JavaScript foundation. `browser_js.sura`
executes fixed-capacity bytecode with undefined/null/boolean/integer/string
values, globals, arithmetic, comparisons, absolute branches, bounded host
calls, stack checks, and an instruction limit. `browser_js_source.sura`
compiles a fail-closed source subset containing `let`/`const`/`var`, assignment
to declared globals, literals, unary and binary expressions, blocks, and
`if`/`else`. Inline `<script>` text is collected and executed in the graphical
browser; script/style contents are parsed as raw text so a JavaScript `<`
operator is not mistaken for an HTML tag. The built-in form page executes a
real inline script. A bounded DOM host implements the exact
`document.getElementById(id).textContent = text` statement. Elements with an
inline `onclick` attribute can execute that subset on a pointer click and
redraw the changed text overlay. The complete screenshot gate requires
`SURA_OS_BROWSER_JS_OK`, `SURA_OS_BROWSER_JS_PAGE_OK`, and
`SURA_OS_BROWSER_JS_CLICK_OK`.

This is not general browser JavaScript. There are no floating-point JavaScript
numbers, arrays, objects, functions, closures, exceptions, promises, modules,
external scripts, general objects/properties, `addEventListener`, timers,
network/storage Web APIs, or general DOM mutation model. Only the exact
ID-based `textContent` mutation and inline `onclick` path above are supported.
An `onclick` handler currently consumes the pointer click before default link
or form activation.
Unsupported scripts are isolated from page rendering and recorded as a bounded
script error.

The browser also has an allocation-free bounded WebAssembly MVP integer
runtime in `browser_wasm.sura`. It parses custom, type, function, export, and
code sections; executes direct calls, structured blocks/loops/if/else,
branches, locals, i32/i64 constants, integer comparisons, and the implemented
integer arithmetic opcodes; and enforces fixed call-depth and instruction
budgets. The graphical browser detects an HTTP or HTTPS response body beginning
with the WebAssembly magic bytes, invokes its zero-argument exported `run`
function, accepts an i32 or i64 result, and renders that result as an HTML page.
The internal binary fixture `sura.local/webassembly` returns 42. The complete
screenshot gate requires `SURA_OS_BROWSER_WASM_OK` and
`SURA_OS_BROWSER_WASM_PAGE_OK`, then writes
`build/os/SuraOS-browser-webassembly.ppm`.

This is not general WebAssembly or the browser WebAssembly JavaScript API.
Imports, tables, linear memory, globals, start functions, elements, data,
floating point, references, exceptions, threads, SIMD, WASI, JIT compilation,
`WebAssembly.instantiate`, and JavaScript fetch/instantiate integration are not
implemented. Validation is a bounded structural and runtime type check rather
than the complete WebAssembly specification validator.

The dedicated execution gates are:

```powershell
.\tools\sura_browser_js_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe

.\tools\sura_browser_js_source_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe

.\tools\sura_browser_js_dom_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe

.\tools\sura_browser_wasm_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

This is not a general TLS stack:
it does not provide IPv6, TCP
retransmission, congestion control, out-of-order reassembly, IDNA, certificate
revocation, a broad root store, TLS session resumption, cookies, complete
JavaScript or WebAssembly, multipart/file form submission, broad CSS
selector/property coverage, complete CSS layout, or complete flex/grid
behavior.

The repository has freestanding TLS 1.3 and X.509 foundations in
`sha256.sura`, `hkdf_sha256.sura`, `aes128_gcm.sura`, `x25519.sura`,
`tls13_record.sura`, `tls13_handshake.sura`, `tls13_hello.sura`,
`tls13_messages.sura`, `tls13_verify.sura`, `entropy_x86.sura`,
`rsa_public.sura`, `rsa_sha256.sura`, `der.sura`, `x509.sura`, and
`x509_chain.sura`. The following gate executes them after `ExitBootServices`
and checks RFC/NIST known answers, X25519 shared-secret agreement, encrypted
TLS 1.3 records, handshake and application key schedules, Finished
verification, strict handshake-message parsing, RDRAND-backed entropy, RSA
public operations, RSA-PSS and PKCS#1 SHA-256 verification, DER parsing, X.509
chain/hostname/time validation, and modified-tag rejection:

```powershell
.\tools\sura_tls_crypto_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe

.\tools\sura_trust_store_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The trust-store gate executes after `ExitBootServices`, walks every generated
root through the indexed accessors, verifies its pinned SHA-256 digest, parses
it as an RSA CA, verifies its SHA-256 or SHA-384 self-signature, rejects a
one-byte signature mutation, and requires `SURA_TRUST_STORE_OK`. These
isolated gates alone
do not prove an Internet connection. The separate
full-desktop screenshot gate executes the TCP 443 and Text Browser integration
against `suralang.site` and requires `SURA_OS_BROWSER_HTTPS_OK`. The current
profile is intentionally limited to the algorithms and trust anchor listed
above. It has no revocation check or session resumption, and the table-based
AES path is not claimed to be cache-side-channel hardened.

The freestanding text-input library composes modern two-set Korean and encodes
Unicode scalars as UTF-8. Text Editor sends those bytes through its CPL-3
mailbox. Common desktop UI, Text Editor, File Explorer, Calculator, and Text
Browser render distinct lowercase ASCII and modern precomposed Hangul with a
proportional 16-pixel, 2-bit antialiased atlas generated from Noto Sans CJK KR.
The source font is pinned by SHA-256 in `tools/sura_ui_font_generate.ps1`; its
SIL Open Font License 1.1 is included in `third_party/noto-cjk/OFL.txt`.
The QEMU text-input gate checks the exact physical-key sequence
`dkssudgktpdy` → `안녕하세요`. The desktop screenshot gate enters Korean
through the emulated keyboard and requires `SURA_OS_KOREAN_INPUT_OK`. It
captures the rendered editor and `INPUT: KO` indicator in
`build/os/SuraOS-korean-input.ppm`. The same gate enters `안녕하세요` in
Terminal and requires `SURA_OS_TERMINAL_KOREAN_INPUT_OK`. Terminal stores
Unicode code points in its visible cell buffer and renders them with the same
UI font; built-in command names remain ASCII. The atlas covers printable ASCII
and modern precomposed Hangul, not all Unicode, complex shaping, or Hanja
conversion. Text Browser accepts and renders Korean in
URL paths and percent-encodes their UTF-8 bytes for HTTP. The graphical gate
captures the Korean address bar in `build/os/SuraOS-browser-korean.ppm`. Its
live network assertion enters explicit `http://example.org`, proving that the
request hostname is not fixed to the boot-time `example.com`. The same gate
enters scheme-less `suralang.site`, which selects HTTPS by default, validates
its certificate, receives its encrypted HTTP response, and requires
`SURA_OS_BROWSER_HTTPS_OK`. DNS host names remain ASCII until IDNA is added;
page text preserves valid UTF-8 and uses the same atlas where glyphs exist.

The English-US/Korean-two-set preference can be toggled from any active window
with Right Alt or the Hangul key, or by clicking the keyboard row in System
Information. It is saved in `SETTINGS.CFG`. The two-boot gate verifies the
same FAT32 disk restores Korean mode:

```powershell
.\tools\sura_input_layout_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
```

File Explorer shows the mounted SuraFS document volume and supports bounded
click and arrow-key navigation into UTF-8 directories and back to their
parent. F3 or `Ctrl+Shift+N` creates a folder, F4 or `Ctrl+N` creates a file,
F2 performs inline UTF-8 rename, Delete confirms a move to `/휴지통`, and
Delete inside the recycle bin permanently removes the selected tree. Explorer
uses `Ctrl+C`, `Ctrl+X`, and `Ctrl+V` for file or complete directory-tree
copy/move. Paste keeps the original name when it is free and otherwise chooses
` - Copy`, ` - Copy 2`, and later conflict suffixes; copying a directory into
its own descendant is rejected. Explorer enumerates up to the 32-node SuraFS
capacity and follows arrow, Home, and End selection through a six-row viewport
and scrollbar. Text Editor
opens the selected SuraFS file into a 4096-byte buffer, accepts LF-normalized
Enter and UTF-8 code-point Backspace/Delete, tracks a byte-boundary cursor and
selection anchor, highlights selected text, wraps long lines, follows the
cursor viewport, and autosaves its complete contents through one atomic VFS
replace and one SuraFS generation commit. `Ctrl+A`, `Ctrl+C`, `Ctrl+X`, and
`Ctrl+V` select, copy, cut, and paste; Shift+Left/Right and Shift+Home/End
extend a selection. `Ctrl+F` selects the next byte-exact UTF-8 match, and
`Ctrl+H` performs a bounded replacement through the same checked Ring-3 edit
path. The edit itself runs through an 8288-byte Ring-3 mailbox with separate
bounded document and insertion-payload regions.
The first SuraFS initialization also creates a 62-byte `/문서/main.sura`
starter when that path is absent. A lower-case `.sura` active path switches
the title to `SURA CODE EDITOR` and lexically colors recognized Sura keywords,
quoted strings, `#` line comments, and numbers. Highlighting is a bounded
render-time byte map; it does not alter file contents and is not a parser,
diagnostic engine, or completion system.
`Ctrl+Shift+S` opens a UTF-8 Save As
field in the current document directory; it refuses silent overwrite and
switches the active document only after creation and the complete write both
succeed. `Ctrl+S` forces a save of the active file. The legacy FAT32
`DOCS/README.TXT` and fixed
`NOTES.TXT` path remain as a fallback when the SuraFS partition cannot mount.
Calculator displays the complete expression and result, accepts keyboard
digits, `+ - * / =`, Backspace, and `C`, and exposes the same operations
through a clickable 4x4 keypad. QEMU
verifies SuraFS directory traversal, folder creation and recycle-bin transfer,
file creation and rename, `한글.sura` creation through Korean composition,
an exact 43-byte UTF-8 file copy, cut/paste into `movebox`, recursive
`movebox - Copy` duplication, editor select/copy/cut/paste with exact content
restoration, editor input, exact `copy.sura` Save As contents and
`SURA_OS_EDITOR_SYNTAX_OK`,
the disk write, a second boot with the exact saved nodes and bytes, and the
keyboard result `50 - 31 = 19` plus the graphical-keypad result
`7 + 5 = 12`. File
Explorer, Text Editor, Calculator, Terminal, and System Information rendering
remains in the kernel, and VFS/SuraFS operations remain kernel operations.
Their selection, editing, calculator, command-line, and system-snapshot
state-transition functions are copied to process-owned read-only executable
pages. The graphical boot also starts a Browser request validator, creating
seven workers once in a shared
`UserProcessScheduler`. Each starts in persistent mode, blocks on syscall 5,
receives a kernel event through checked syscall 2, processes its mailbox, and
blocks again. Only each worker's mailbox, event page, and guarded stack are
writable from CPL 3. Each process CR3 shares kernel mappings with U/S cleared
and reserves a different lower-half PML4 slot for user pages. VFS navigation,
SuraFS autosave, terminal
rendering, memory inspection, clear, and shutdown run only after the matching
worker has returned to the kernel root. The Browser worker receives a private
384-byte mailbox with a copied URL snapshot, validates bounded scheme, host,
and path bytes, and returns explicit network, storage, and device
capabilities. External hosts receive only network access; `sura.local` receives
no network access. Initial requests, form actions, and redirect targets cross
this boundary. Address-bar navigation sends an uncached DNS query and polls it
incrementally from the desktop loop. Keyboard and mouse input continue during
that DNS wait, and editing the address cancels the stale snapshot. TCP
SYN/SYN-ACK is a second begin/poll desktop-loop stage; input and F6 cancellation
remain live, and dotted-decimal IPv4 literals bypass DNS. For the first HTTPS
request, ClientHello transmission and TLS record reception are a third
begin/poll desktop-loop stage. The authenticated connection is consumed once
by the HTTPS request continuation. Response reception and decoding, additional
stylesheet/image TLS connections, DOM, layout, rendering, and link/form
navigation still run on a synchronous kernel call stack. Their network polling
loops cooperatively service desktop input, and QEMU verifies pointer movement
during a live HTTPS response fetch. F6 directly cancels a pending DNS, TCP, or
TLS stage. During the later synchronous response/resource phase it records a
cancellation request, lets the shared network stack unwind, and only then
restores the old document and address-field focus. Other nested Browser address
edits, content clicks, and second requests remain rejected while shared
HTTP/TLS buffers are active.

The freestanding libraries also contain scheduler, interrupt, user-process,
checked IPC/process-syscall, ELF64, PCI/PCIe, ACPI, block, partition, FAT32,
AHCI, NVMe, xHCI, modern VirtIO PCI, and VirtIO GPU 2D building blocks.
AHCI, MBR partition discovery, FAT32, VirtIO-net,
Ethernet/ARP/IPv4/UDP/DNS/TCP/HTTP, framebuffer, PS/2, desktop, and application
code are executed by the current boot image. A
fixed-capacity window-manager foundation also implements focus, z-order, hit
testing, title-bar drag, resize, minimize, maximize, fullscreen, close, and
desktop-bound clamping. `window_server.sura` and `ui.sura` provide app-owned
surface composition, damage rectangles, monitor/DPI records, clipboard,
theme, common controls, and accessibility metadata. Their executed QEMU gate
checks exact composition pixels and failed-owner cleanup.
The separate Ring 3 QEMU gate executes an IRETQ transition to CPL 3, checks the
saved CS through a DPL-3 `INT 0x80`, returns with IRETQ, and re-enters the kernel
through `SYSCALL`. The desktop boot path additionally executes System
Information, Terminal, File Explorer, Calculator, and Text Editor state
transitions at CPL 3. `SURA_OS_SYSTEM_CR3_OK`,
`SURA_OS_TERMINAL_CR3_OK`, `SURA_OS_FILES_CR3_OK`,
`SURA_OS_CALCULATOR_CR3_OK`, and
`SURA_OS_EDITOR_CR3_OK` are emitted after the scheduler restores the kernel
root and the kernel validates the matching process address space. Their
`RING3_OK` markers prove completed mailbox round trips.
`SURA_OS_BROWSER_RING3_READY`, `SURA_OS_BROWSER_RING3_OK`, and
`SURA_OS_BROWSER_CR3_OK` prove the Browser request boundary.
`SURA_OS_USER_SCHEDULER_READY` proves the seven queues, process records,
per-process kernel stacks, syscall table, page-fault gate, and APIC kernel-slice
gate were initialized. `SURA_OS_USER_PROCESSES_PERSISTENT_OK` is emitted only
after real GUI requests leave all seven workers blocked with their original
saved frame, CR3 root, and kernel stack. Separate executed scheduler gates also
verify timer preemption of a non-yielding loop, IPC delivery, and user-fault
isolation. The kernel-slice gate additionally terminates that non-yielding
process with exit code 137 and reaps its address space. In the graphical
dispatcher, a faulted or exited worker is disabled and its matching window is
closed instead of ending the shell; `SURA_OS_USER_PROCESS_ISOLATED` is the
isolation marker. The kernel then reaps the failed address space, clears and
rebuilds that app's worker and event queue, creates a new process ID and CR3,
and primes the replacement process back into its blocked event wait. A window
that was visible before the fault is reopened after recovery.
`SURA_OS_USER_PROCESS_RESTARTED` reports successful reconstruction. The
`faultapp`, `hangapp`, and `faultbrowser` are hidden regression-only diagnostic commands; the
normal Terminal `help` output does not advertise them. The full-desktop
regression sends `faultapp` after its normal Calculator checks,
deliberately faults the blocked Calculator's saved user RIP, requires fault
isolation and reconstruction, sends a real `C` event through the replacement
Calculator, and requires `SURA_OS_USER_PROCESS_RESTART_EVENT_OK`. It then
requires `status` from the still-running Terminal. The same regression sends
`hangapp`, enters an intentional non-yielding Calculator loop, injects mouse
movement after `SURA_OS_USER_PROCESS_HANG_STARTED`, requires APIC kernel slices
to advance that input, and queues a System Information request at the same
time. `SURA_OS_USER_PROCESSES_CONCURRENT_OK` records that both processes were
runnable, and `SURA_OS_USER_PROCESS_BACKGROUND_OK` is emitted only after
System Information completes under its own CR3 while Calculator is still
hung. The watchdog then terminates and rebuilds Calculator and requires
`SURA_OS_USER_PROCESS_HANG_RECOVERED` plus another successful Terminal
`status` before shutdown. Ordinary UI calls still wait for their own result;
there is not yet a user-facing background job API. ELF64 execution and several
interrupt foundations are not connected to the desktop boot path. NVMe is
connected as the preferred data-disk backend: the OS initializes admin and
I/O queues, identifies namespace 1, and mounts the same FAT32 and SuraFS
partitions through its `BlockDevice`; AHCI remains the fallback. The dedicated
NVMe gate also verifies an 8-KiB write both through device readback and in the
host image. xHCI is connected: the normal VM reserves a 64-KiB DMA
arena, enumerates directly attached boot keyboard and mouse devices, keeps one
interrupt-IN transfer posted for each, converts reports into `KeyEvent` and
`PointerEvent`, and routes them through the desktop handlers. The VM gate
injects USB Shift and mouse movement and requires
`SURA_OS_XHCI_INPUT_READY`, `SURA_OS_KEYBOARD_OK`, and `SURA_OS_MOUSE_OK`.
Separate keyboard and mouse xHCI gates verify Address Device, descriptor
transfers, Configure Endpoint, raw reports, common-event conversion, and
Disable Slot. This remains polling and QEMU-only; hubs, arbitrary device order,
hot-plug, non-boot HID, IRQ/MSI, and physical-hardware proof are not complete.

The VirtIO GPU path is independently executed by
`tools/sura_virtio_gpu_qemu_gate.ps1`. That gate configures the modern
`1af4:1050` PCI capability transport, creates and attaches a 640x480
`B8G8R8X8_UNORM` resource, selects scanout zero, performs transfer and flush,
and validates five exact RGB pixels from a QMP screenshot. The normal OS keeps
GOP visible and mirrors full frames or cursor-damage rectangles into the
VirtIO resource. It emits `SURA_OS_VIRTIO_GPU_READY`; running
`tools/sura_os_vm.ps1 -DisableVirtioGpu` requires
`SURA_OS_VIRTIO_GPU_FALLBACK` while the same desktop continues on GOP. This
does not implement 3D, virgl/Venus, multiple scanouts, a hardware cursor,
interrupts, hot-plug recovery, or physical GPU support.

The Intel HDA path is independently executed by
`tools/sura_hda_qemu_gate.ps1`. It resets the PCI/MMIO controller, discovers
the codec, Audio Function Group, output converter, and pin, configures one
48-kHz signed-16 stereo stream, and transfers a one-entry BDL. The wrapper
captures QEMU's WAV backend and verifies the PCM format plus observed
`-12000` and `+12000` samples. The normal OS reserves 48 DMA pages before
`ExitBootServices`, transfers a bounded startup slice, and emits
`SURA_OS_HDA_AUDIO_READY`. Running `tools/sura_os_vm.ps1 -DisableHda`
requires `SURA_OS_HDA_AUDIO_UNAVAILABLE` while the same desktop continues.
The automated VM uses a silent audio backend. This remains polling,
fixed-format, QEMU-only output without an application mixer, capture path,
interrupts, CORB/RIRB, or physical-hardware proof.

The ACPI power path is independently executed by
`tools/sura_power_shutdown_qemu_gate.ps1` and
`tools/sura_power_reset_qemu_gate.ps1`. It checksum-validates the firmware
tables, parses the DSDT `_S5` package, uses the FADT PM1 control descriptions
for power-off, and uses `RESET_REG`/`RESET_VALUE` for reset. The complete OS
emits `SURA_OS_ACPI_POWER_READY`, and the normal VM requires
`SURA_OS_ACPI_POWER_OFF_ARMED` before QEMU exits with code 0.
`tools/sura_os_reboot_qemu_gate.ps1` sends `reboot` through the persistent
CPL-3 Terminal worker and requires `SURA_OS_ACPI_RESET_ARMED`. The legacy
QEMU debug-exit path is retained only as a fallback when the firmware path is
unavailable. This implementation does not include a general AML interpreter,
`_PTS`, hardware-reduced ACPI sleep registers, suspend/wake, battery or
thermal policy, or physical-hardware proof.

Run the executed Ring 3 and syscall gate:

```powershell
.\tools\sura_ring3_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
```

Run the checked process-to-process IPC syscall gate:

```powershell
.\tools\sura_process_syscall_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
```

Run the executed blocking event-wait and wakeup gate:

```powershell
.\tools\sura_process_wait_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
```

Run the executed preemptive multi-process gate:

```powershell
.\tools\sura_user_process_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
```

Run the non-yielding process to Ring-0 input-loop timer gate:

```powershell
.\tools\sura_user_process_kernel_slice_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
```

Run the real calculator worker as one persistent event-driven process:

```powershell
.\tools\sura_persistent_calculator_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
```

Run all six real desktop workers under one persistent process scheduler:

```powershell
.\tools\sura_persistent_desktop_apps_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
```

Run every freestanding process-foundation check:

```powershell
.\tools\sura_os_foundation_verify.ps1 -Engine .\build\SuraLanguage_user.exe
```

Run the dedicated USB keyboard and mouse gates:

```powershell
.\tools\sura_xhci_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
.\tools\sura_xhci_mouse_qemu_gate.ps1 -Engine .\build\SuraLanguage_user.exe
```

Build and run the VM test from the repository root:

```powershell
.\tools\sura_os_vm.ps1 -Engine .\build\SuraLanguage_os_next.exe
```

Start an interactive serial shell:

```powershell
.\tools\sura_os_vm.ps1 -Engine .\build\SuraLanguage_os_next.exe -Interactive
```

Interactive mode opens the graphical QEMU window. Keep the PowerShell window
focused when entering serial-shell commands. For terminal-only automation:

```powershell
.\tools\sura_os_vm.ps1 -Engine .\build\SuraLanguage_os_next.exe `
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
.\tools\sura_os_screenshot.ps1 -Engine .\build\SuraLanguage_os_next.exe
```

Run the exact graphical SuraFS save and two-boot restore gate:

```powershell
.\tools\sura_surafs_gui_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The final capture is written to `build\os\SuraOS-desktop.ppm`, the dragged
overlapping-window state to `build\os\SuraOS-windows.ppm`, the open Start menu
to `build\os\SuraOS-start-menu.ppm`, and the live `suralang.site` HTTPS browser
state to `build\os\SuraOS-browser.ppm`. The bounded WebAssembly result page is
written to `build\os\SuraOS-browser-webassembly.ppm`. The capture tool attaches
QEMU xHCI USB keyboard and mouse devices and uses QMP to focus, drag, close,
and reopen System Information from the taskbar, opens
Start, opens and exercises the three built-in apps, enters `DOCS` in File
Explorer, writes `build\os\SuraOS-apps.ppm`, activates the browser, types
`http://example.org/`, performs a second live navigation, verifies the complete
direct `suralang.site` HTTPS path, external CSS fetch, DOM box conversion, and
painting through `SURA_OS_BROWSER_HTTPS_OK`,
`SURA_OS_BROWSER_EXTERNAL_CSS_OK`, `SURA_OS_BROWSER_DOM_BOX_OK`, and
`SURA_OS_BROWSER_DOM_RENDER_OK`, captures that rendered state, clicks a live
same-document anchor, injects a PS/2 wheel event, holds Page Down, and holds
Backspace over the UTF-8 address while requiring
`SURA_OS_BROWSER_LINK_OK`, `SURA_OS_BROWSER_WHEEL_OK`,
`SURA_OS_BROWSER_SCROLL_OK`, `SURA_OS_KEY_REPEAT_OK`, and
`SURA_OS_BROWSER_BACKSPACE_REPEAT_OK`. Before `example.org`, it starts a real
DNS query for a reserved invalid TLD, presses F6 while that query is pending,
cancels the stale request, and requires
`SURA_OS_BROWSER_NAV_ASYNC_BEGIN`, `SURA_OS_BROWSER_NAV_ASYNC_INPUT_OK`, and
`SURA_OS_BROWSER_NAV_ASYNC_DONE` after the replacement live navigation. It then
starts a TCP connection to the RFC 5737 TEST-NET address `203.0.113.1`, presses
F6 while SYN-ACK is pending, and requires `SURA_OS_BROWSER_NAV_TCP_BEGIN`,
`SURA_OS_BROWSER_NAV_TCP_INPUT_OK`, and a later
`SURA_OS_BROWSER_NAV_TCP_DONE` from the replacement live navigation. The
scheme-less `suralang.site` navigation must emit
`SURA_OS_BROWSER_NAV_TLS_BEGIN` and `SURA_OS_BROWSER_NAV_TLS_DONE` while the
authenticated handshake advances from the desktop loop. During the later live
HTTPS response phase it first presses F6 during active shared-buffer work,
requires `SURA_OS_BROWSER_NAV_FETCH_CANCEL_REQUESTED` and
`SURA_OS_BROWSER_NAV_FETCH_CANCELLED_OK`, retries the same address, injects a
mouse movement, and requires
`SURA_OS_BROWSER_NAV_FETCH_BEGIN` and
`SURA_OS_BROWSER_NAV_FETCH_INPUT_OK` before completion. It then activates
Terminal, verifies the
desktop/window/app/terminal/storage/DHCP/network/DNS/TCP/HTTP/browser markers,
the xHCI input-ready marker, the JavaScript and WebAssembly execution markers,
and then shuts the VM down
normally. The test normally works on a disposable data-disk copy. Preserve
that modified copy for a second-boot persistence check with:

```powershell
.\tools\sura_os_screenshot.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe `
  -DataDiskOutput .\build\os\SuraData-persistence-test.img
```

Use `-DataDisk <path>` to capture a previously saved disk, and
`-SkipInputVerification` when only its restored desktop is needed.

The shell supports `help`, `status`, `mem`, `about`, `clear`, `shutdown`, and
`reboot`. Use `shutdown` for ACPI S5 or `reboot` for the FADT reset path. The
non-interactive VM test checks the normal commands and ACPI shutdown; a
separate complete-OS gate checks the Ring-3 `reboot` route.

The script uses QEMU with TCG emulation and an EDK2 x86-64 firmware image. It
does not modify firmware boot entries or boot the host computer. Interactive
mode connects COM1 to an ephemeral loopback-only TCP port so PowerShell handles
line input normally; it does not expose the shell on an external interface.

## Doom Ring-3 port

`os/doom` contains a freestanding Doomgeneric port. Its 64-bit static ELF is
linked into PML4 slot 2, loaded into a dedicated `ProcessAddressSpace`, and
executed at CPL 3. The Sura kernel gate supplies monotonic millisecond ticks,
polling PS/2 Set-1 keyboard input, checked 640x400 GOP presentation, serial
diagnostics, and process exit through software-interrupt syscalls 100-104.
The shareware `doom1.wad` is embedded in the ELF by the build.

The normal Sura desktop launcher now builds and embeds the same ELF
automatically. Start the graphical desktop, then click the `DOOM` icon in the
left desktop column or on the taskbar:

```powershell
.\tools\sura_os_vm.ps1 -Interactive
```

Doom opens as window 7 and runs in the spare slot of the existing Ring-3
desktop scheduler. Keyboard events from either xHCI USB or PS/2 are forwarded
through the shared input layer. Arrow keys move, Ctrl fires, Space uses, Esc
opens the Doom menu, and F12 closes only Doom and returns to the Sura desktop.
The port starts shareware E1M1 directly (`-warp 1 1`), so Doom's unattended
attract-mode demo no longer looks like automatic player movement. Pass
`-DisableDoom` to `sura_os_vm.ps1` when an OS image without the embedded game
is wanted.

Run the end-to-end desktop proof, including icon click, first E1M1 frame, xHCI
F12 input, and return to the desktop scheduler, with:

```powershell
.\tools\sura_os_doom_desktop_gate.ps1
```

The proof writes `build\os\SuraOS-Doom.ppm` and
`build\os\SuraOS-Doom.serial.log`.

Build the ELF and EFI/disk images and run the automated QEMU proof:

```powershell
.\tools\sura_doom_qemu_gate.ps1
```

The proof requires Doom's Ring-3 startup and first rendered frame, captures
`build\doom\SuraDoom.ppm`, injects F12 through QEMU's i8042 keyboard, and
requires `SURA_DOOM_PLAYABLE` plus QEMU debug-exit code 33.

Start a playable graphical VM with:

```powershell
.\tools\sura_doom_qemu_gate.ps1 -Interactive
```

Use the arrow keys to move, Ctrl to fire, Space to use, Esc for the Doom menu,
and F12 to close the VM cleanly. The current port is keyboard-only, has no
audio, and discards configuration and save-file writes. It runs from a
temporary ASCII-only disk path so repositories stored below a Korean Windows
path remain usable.
