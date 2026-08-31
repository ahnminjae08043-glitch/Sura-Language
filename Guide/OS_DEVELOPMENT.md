# Sura OS development

Sura has an experimental freestanding `uefi-x86_64` target. It emits a
PE32+ EFI application directly from `.sura` source. The output does not embed
the Sura VM, garbage collector, Windows API, C runtime, assembler, or linker.

`os/sura_os.sura` is an executed QEMU/OVMF kernel integration image with the
first graphical desktop milestone. It records the GOP framebuffer, allocates a
backbuffer, initializes COM1, obtains the UEFI memory map, exits Boot Services,
renders a desktop entirely from freestanding Sura code, presents it to GOP
memory, and executes a physical-page allocate/write/read/free self-check before
emitting `SURA_OS_KERNEL_READY`.

It is not a complete general-purpose operating system. In the normal VM
configuration, the rendered desktop enumerates a directly attached USB boot
keyboard and boot mouse through xHCI and polls their interrupt-IN rings. The
i8042 PS/2 path remains an executed fallback. COM1 remains available for
diagnostics and automation.
Six windows can be focused, raised, dragged, resized, minimized, maximized,
switched to fullscreen, restored, closed, and reopened. Title-bar controls and
the F8/F9/F10/F11 QEMU input paths cover resize, minimize, maximize, and
fullscreen state. The Start menu activates applications and shutdown.
The executed image mounts a FAT32 data disk, saves notes and desktop state,
obtains IPv4 configuration with DHCP over VirtIO-net, performs ARP, DNS, TCP,
HTTP/1.1, and HTTPS, and displays a bounded HTML layout from an entered URL.
It accepts ASCII hostnames, follows at most five redirects, and supports a
narrow fail-closed TLS 1.3 profile: `TLS_AES_128_GCM_SHA256`, X25519,
RSA-PSS-RSAE-SHA256 CertificateVerify, RSA/SHA-256 or RSA/SHA-384 certificate
chains, HTTP/1.1 ALPN, hostname and validity-time checks, and six pinned trust
anchors: ISRG Root X1, DigiCert Global Root G2, GlobalSign Root CA - R3,
Amazon Root CA 1, USERTrust RSA Certification Authority, and Microsoft RSA
Root Certificate Authority 2017.
This is not a broad operating-system root store. Separate
post-`ExitBootServices` QEMU execution verifies the
cryptographic and X.509 primitives, and the full desktop gate executes the
direct `suralang.site` HTTPS path through encrypted response rendering.
Plain HTTP and decrypted HTTPS responses share the same bounded framer for
`Content-Length`, `Transfer-Encoding: chunked` with extensions and trailers,
and connection-close bodies. Requests send `Accept-Encoding: gzip, identity`.
The shared response path removes chunk framing before decoding bounded gzip
content, supports stored, fixed-Huffman, and dynamic-Huffman DEFLATE blocks,
and verifies the gzip CRC32 and ISIZE trailer. Other content codings fail
closed.
The browser distinguishes heading, paragraph, and link runs and applies a
small CSS color subset (`body`, `div`, `h1`, `a`; `background`,
`background-color`, and `color`; `#RGB` and `#RRGGBB`).
System Information snapshot validation, Terminal command-line editing and
command recognition, and File Explorer, Text Editor, and Calculator state
transitions execute at CPL 3 in distinct `ProcessAddressSpace` roots through
dedicated user code, stack, and mailbox pages, while window rendering, input
routing, privileged terminal commands, FAT32 traversal, and file persistence
remain in the kernel.
The five application workers plus a Window Server worker and Browser request
validator are seven persistent scheduled processes. The Window Server has its
own CR3 and a shared mapping of
the real desktop backbuffer, while the kernel still drives one requested
worker at a time and normally waits for it to block again. There is no
user-facing background-job model or complete user-space compositor,
IPv6, complete TCP reliability, a general TLS/root-store implementation, or a
general HTML/CSS/JavaScript browser. Most files in `examples/os` remain compiler
feature tests and do not form a complete user environment or device-driver
stack. The dedicated `ring3_qemu_gate.sura` separately verifies one CPL-3
entry and both software-interrupt and fast-syscall paths in QEMU. Test
generated images in a virtual machine before considering physical hardware.

## Build

```powershell
.\SuraLanguage.exe --target uefi-x86_64 `
  --out BOOTX64.EFI examples\os\hello_uefi.sura

.\SuraLanguage.exe --target uefi-x86_64 `
  --out BOOTX64.EFI --disk-image sura-os.img `
  examples\os\hello_uefi.sura
```

Place the file at `EFI\BOOT\BOOTX64.EFI` on a FAT-formatted UEFI image.
The second form also creates a deterministic disk image with a protective
MBR, primary and backup GPT, a FAT32 EFI System Partition, and the generated
payload at `EFI\BOOT\BOOTX64.EFI`. The standalone `.efi` is retained so it can
also be copied to an existing ESP or inspected separately. `--out` and
`--disk-image` must name different files.

Unsigned development images normally require Secure Boot to be disabled.
The image builder does not sign the EFI payload, install firmware variables,
or create Secure Boot keys.

An automated QEMU/OVMF gate is available:

```powershell
.\tools\sura_qemu_boot_gate.ps1 -Engine .\SuraLanguage.exe

# Compile and inspect the gate image without launching QEMU:
.\tools\sura_qemu_boot_gate.ps1 -Engine .\SuraLanguage.exe -CompileOnly
```

The full form requires `qemu-system-x86_64` and OVMF/EDK2 firmware, which can
also be supplied with `-Qemu` and `-Firmware`. It boots the generated GPT/FAT32
disk, waits for `SURA_EXIT_BOOT_SERVICES_OK` on COM1, and requires the expected
`isa-debug-exit` status. A compile-only pass proves image construction and
marker retention, not that firmware executed the image.

The TLS 1.3 cryptographic and X.509 foundation has a separate executed gate:

```powershell
.\tools\sura_tls_crypto_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe

# Build and inspect the EFI image without claiming execution:
.\tools\sura_tls_crypto_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe -CompileOnly
```

The executed form first leaves UEFI Boot Services and then checks the RFC 6234
SHA-256 and HMAC-SHA256 vectors, RFC 5869 HKDF test case 1, RFC 8448
TLS 1.3 label, complete ClientHello-plus-ServerHello handshake key schedule,
traffic key, IV, and Finished-key values, AES-128 and AES-128-GCM known
answers, RFC 7748 X25519 direct and Alice/Bob shared-secret vectors, an exact
RFC 8448 encrypted TLS 1.3 record, deterministic RSA public and signature
verification vectors, DER and X.509 chain vectors, and QEMU-emulated RDRAND
output. The ClientHello builder emits SNI,
TLS 1.3, X25519, signature-scheme, AES-128-GCM, and HTTP/1.1 ALPN offers. The
ServerHello parser checks lengths, the selected version/cipher/group, session
ID, duplicate required extensions, HelloRetryRequest, and an all-zero share.
The gate also rejects a modified GCM tag without writing unauthenticated
plaintext. Success requires
`SURA_SHA256_OK`, `SURA_SHA384_OK`, `SURA_HMAC_SHA256_OK`,
`SURA_HKDF_SHA256_OK`,
`SURA_TLS13_LABEL_OK`, `SURA_AES128_OK`, `SURA_AES128_GCM_OK`,
`SURA_X25519_OK`, `SURA_TLS13_RECORD_OK`,
`SURA_TLS13_KEY_SCHEDULE_OK`, `SURA_TLS13_HELLO_OK`,
`SURA_ENTROPY_X86_OK`, `SURA_RSA_PUBLIC_OK`,
`SURA_RSA_SHA256_VERIFY_OK`, `SURA_DER_READER_OK`,
`SURA_X509_CHAIN_OK`, `SURA_TLS13_APPLICATION_OK`,
`SURA_TLS13_MESSAGES_OK`, and
`SURA_TLS_CRYPTO_OK`, with QEMU exit code 33.

The generated trust store has a separate post-`ExitBootServices` execution
gate:

```powershell
.\tools\sura_trust_store_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

`tools/sura_trust_store_generate.ps1` pins each downloaded DER length and
SHA-256 fingerprint and emits indexed DER, byte-count, and fingerprint
accessors. The HTTPS initializer accepts one through sixteen generated roots
and fills the X.509 pointer table in a loop. The gate walks the same accessors,
checks every digest, parses every root, validates CA/key-usage/time/RSA
properties, verifies each SHA-256 or SHA-384 RSA self-signature, rejects a
one-byte signature mutation, and requires `SURA_TRUST_STORE_OK`.

These isolated crypto and trust-store gates do not prove an Internet
connection. The
full-desktop screenshot gate separately executes TCP 443, the TLS 1.3
handshake, certificate-chain/hostname/time validation, encrypted HTTP, and
Text Browser integration against `suralang.site`, and requires
`SURA_OS_BROWSER_HTTPS_OK`. The implemented profile has six pinned trust
anchors—ISRG Root X1, DigiCert Global Root G2, GlobalSign Root CA - R3,
Amazon Root CA 1, USERTrust RSA Certification Authority, and Microsoft RSA
Root Certificate Authority 2017—with no
revocation check or session resumption, and no general algorithm negotiation.
The current table-based AES path is not claimed to be cache-side-channel
hardened.

HTTP/1.1 response framing has a separate post-`ExitBootServices` execution
gate. It checks partial and complete `Content-Length` bodies, case-insensitive
headers, chunk extensions, trailers, in-place dechunking, connection-close
bodies, no-body status codes, and rejection of conflicting, duplicate,
malformed, and incomplete framing:

```powershell
.\tools\sura_http1_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The gzip/DEFLATE path has a separate post-`ExitBootServices` execution gate.
It checks CRC32, stored, fixed-Huffman, and dynamic-Huffman blocks, overlapping
LZ77 copies, malformed/truncated input, output bounds, and the complete
HTTP `Content-Encoding: gzip` finalization path:

```powershell
.\tools\sura_gzip_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The freestanding PNG foundation decodes non-interlaced 8-bit grayscale,
grayscale-alpha, RGB, and RGBA images into RGBA8. It validates chunk CRCs,
concatenates consecutive IDAT chunks, checks the zlib Adler-32 trailer, and
reconstructs PNG filters 0 through 4. Its QEMU gate executes a two-IDAT RGB
image containing all five filter types and checks every output pixel:

```powershell
.\tools\sura_png_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

Palette images, sub-8-bit/16-bit samples, Adam7 interlacing, and color
management are not implemented. The complete desktop gate separately executes
same-host browser `<img>` fetch, decode, layout, and painting.

The freestanding JPEG foundation decodes 8-bit sequential baseline Huffman
images into RGBA8. It accepts grayscale and three-component YCbCr with 1x1
through 2x2 sampling factors, covering 4:4:4, 4:2:2, and 4:2:0. The decoder
parses bounded quantization and Huffman tables, handles entropy byte stuffing,
dequantizes coefficients, performs an integer inverse DCT, upsamples chroma,
and converts YCbCr to RGB:

```powershell
.\tools\sura_jpeg_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The executed gate checks an AC-bearing 4:4:4 color gradient, a 4:2:0 image
with four luminance blocks per MCU, a grayscale image, exact dimensions and
alpha, bounded color error, malformed input, output capacity, and workspace
capacity. Progressive JPEG, arithmetic coding, nonzero restart intervals,
CMYK/YCCK, 12-bit samples, and ICC/color-profile processing are not
implemented. The browser connects this decoder to its bounded `<img>` path.

The fixed-capacity HTML DOM foundation stores element and UTF-8 text nodes in
caller-owned arrays with parent, first-child, last-child, and next-sibling
links. Attribute values remain bounded slices of the immutable response bytes.
It recognizes the browser's structural, text, link, image, form, and metadata
tags; maps common attributes including `class`, `href`, `src`, `alt`, `style`,
`width`, `height`, and the form attributes `type`, `name`, `value`, `action`,
`method`, `placeholder`, `for`, `checked`, and `disabled`; accepts quoted and unquoted values; skips comments and
declarations; and performs bounded ancestor recovery for closing tags:

```powershell
.\tools\sura_html_dom_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The gate verifies an exact element/text tree, Korean UTF-8 slices, image and
link attributes, a login-shaped `form`/`label`/`input`/`button` tree and its
attributes, void elements, comment removal, malformed-comment rejection, and
node/attribute capacity failures. Editable page-control state and form
submission are the next layer and are not claimed by this DOM gate. The graphical Text Browser now copies a
bounded response body into a 1,024-node/1,024-attribute DOM, computes styles,
builds boxes, and paints visible backgrounds, borders, and UTF-8 text. It keeps
the older flat tokenizer as a fallback when this bounded path rejects a page.
The browser scans `<img>` nodes, reserves two bounded image slots, and performs
same-host PNG or baseline-JPEG fetch/decode/layout. The complete QEMU screenshot
gate fetches a large PNG from live `suralang.site`, decodes it, paints it, and
requires `SURA_OS_BROWSER_IMAGE_PNG_OK` plus
`SURA_OS_BROWSER_IMAGE_RENDER_OK`. The same live gate also covers a maximum
16,384-byte TLS application-data fragment, including the extra TLS inner
content-type byte.

The fixed-capacity CSS computed-style foundation parses tag, `.class`, `#id`,
compound, descendant, and direct-child rules and applies them to the DOM tree.
It supports `display`, foreground
and background colors, width/height, min/max dimensions,
margin/padding/border-width longhands and one-to-four-value shorthands, width
extraction from compound `border` and directional border shorthands, font size,
line height, and content-box or border-box sizing. Widths accept integer pixels,
percentages, viewport width, and the bounded `calc(<percent> - <px>)` form;
heights accept integer pixels and viewport height. Positioning accepts
`static`, `relative`, `absolute`, and `fixed` with signed pixel or `auto`
top/right/bottom/left offsets. Font size additionally
preserves the bounded `clamp(<px>, <vw>, <px>)` form until the viewport width
is known, and line height accepts a non-negative decimal multiplier. The cascade uses
tag/class/id specificity and source order;
inline `style` declarations override stylesheet rules. Color, font size, and
line height inherit through the DOM:

```powershell
.\tools\sura_css_style_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The executed gate checks exact computed values for tag, class, ID, and inline
rules; source-order and specificity behavior; inherited text colors;
`display:none`; four-side and compound border shorthands; malformed input; and
fixed-capacity failure. A separate relaxed entry point used by the browser skips at-rules and
unsupported selectors and retains supported selectors from comma lists. It
matches bounded compounds such as `div#hero` and `.foo.bar`, descendants such
as `.page #hero`, and direct children such as `.page > #hero`. Before parsing,
the browser collects up to 32 declarations from one `:root` block and resolves
matching `var(--name)` uses in place. The live QEMU gate requires
`SURA_OS_BROWSER_CSS_VARIABLES_OK` for the production stylesheet. Hex colors
accept 3/4/6/8 digits; the alpha nibble/byte is currently discarded, and a
compound `background` value uses its last supported hex color. Scoped custom
properties, fallback/cycle handling, attribute/pseudo/sibling selectors,
`!important`, named colors, percentage heights, general `calc()`, `em`/`rem`,
vertical auto-margin behavior, media queries, and general variable-size web-font
rendering are not implemented. `sticky`, `z-index`, stacking contexts,
transforms, and complete CSS containing-block rules are also not implemented. Horizontal
`margin:auto` participates in remaining-width distribution. The browser collects inline `style` elements and
can fetch one root-relative, same-host stylesheet.

The bounded CSS box foundation converts those computed styles into border-box
and content-box coordinates. It applies margin, border, and padding edges,
vertical block flow, inline element/text flow, width-based line wrapping,
explicit/automatic height, `content-box`/`border-box`, and complete
`display:none` subtree removal:

```powershell
.\tools\sura_css_box_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The executed gate checks exact `x`, `y`, width, height, content, and outer
geometry for nested root/html/body/div/text boxes, a fixed-height border-box,
hidden content, box-capacity rejection, and a 40-pixel inline wrapping case
containing text, `span`, and following text. The style gate also verifies the
minimum, viewport-preferred, and maximum branches of font-size `clamp()` and a
decimal line-height multiplier. Text width is currently an
approximation derived from Unicode scalar count and font size; it does not
perform glyph-atlas measurement, word breaking, shaping, bidirectional layout,
baseline alignment, floats, overflow clipping, margin collapsing, or table
layout. Relative boxes keep their normal-flow slot and shift visually.
Absolute boxes use the nearest positioned ancestor, fixed boxes use the
viewport, and both are removed from block/flex/grid flow. Right and bottom
offsets require a known containing dimension and positioned size. The live
gate requires `SURA_OS_BROWSER_CSS_POSITION_OK`. Overflow clipping, sticky
positioning, stacking contexts, and z-order remain unimplemented.
`display:flex` supports row/column direction, nowrap/wrap, pixel
row/column gaps, start/center/end/space-between/space-around/space-evenly main
axis placement, and start/center/end bounded cross-axis placement.
`display:grid` accepts up to 16 explicit top-level column tokens or
`repeat(<count>, ...)`, applies pixel row/column gaps, and places children in
equal-width cells. Horizontal auto margins distribute remaining container
width, including centered `margin: 0 auto` boxes. Flex grow/shrink/basis,
ordering, reverse directions,
align-content, per-item alignment, intrinsic track sizing, named lines,
auto-fit/auto-fill, spanning, and the complete CSS algorithms are not
implemented. Layout uses one bounded global state object and is not reentrant.
The Text Browser paints a bounded visible subset per desktop frame:
up to 64 colored backgrounds, 32 text boxes, 256 UTF-8 bytes per text node, and
2,048 UTF-8 text bytes in total. The byte limits preserve complete UTF-8
sequences.

Editable browser controls and form serialization have their own allocation-free
layer and post-`ExitBootServices` execution gate:

```powershell
.\tools\sura_browser_form_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

It verifies initial and mutable text/password values, UTF-8 Backspace,
checkbox toggling, disabled-control rejection, control-to-form ancestry,
uppercase percent encoding, spaces as `+`, exclusion of disabled and unchecked
controls, the default `on` checkbox value, exact query bytes, and bounded-output
failure. The graphical browser sends default/GET forms as an encoded action
query and POST forms as an HTTP/1.1
`application/x-www-form-urlencoded` body. It supports current, absolute
HTTP(S), and root-relative actions; 307/308 preserve POST while 301/302/303
continue as GET. The complete screenshot gate focuses and edits
`sura.local/forms`, submits it through the real POST request builder, requires
`SURA_OS_BROWSER_FORM_SUBMIT_OK`, and captures
`build/os/SuraOS-browser-submitted.ppm`. Radio groups, textarea, select, file
upload, multipart encoding, validation, cookies/sessions, and the complete HTML
form model are not implemented.

The first browser JavaScript execution layer is allocation-free and has three
post-`ExitBootServices` QEMU gates:

```powershell
.\tools\sura_browser_js_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe

.\tools\sura_browser_js_source_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe

.\tools\sura_browser_js_dom_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

`browser_js.sura` provides caller-owned value, instruction, program, stack,
global, and scratch storage. Its VM executes undefined/null/boolean/integer/
string constants, global reads and writes, arithmetic, comparisons, unary
operators, absolute branches, a bounded indirect host-call ABI, and duplicate/
pop operations. It rejects stack underflow/overflow, invalid constants,
globals, jumps and opcodes, type errors, division by zero, integer division/
negation overflow, host failures, and exhausted instruction budgets. The core
gate executes a taken branch and an indirect host call, verifies exact globals,
then exercises the invalid-opcode, stack, constant, global, jump, argument, and
infinite-loop failure paths.

`browser_js_source.sura` compiles a bounded fail-closed JavaScript source
subset: integer and unescaped UTF-8 string literals, booleans, null,
undefined, `let`/`const`/`var`, assignment to declared globals, unary
`!`/`-`, integer arithmetic, equality and ordering, parentheses, blocks,
`if`/`else`, and line/block comments. The source gate compiles and executes
both branches, assignment, strict equality syntax, and rejects unknown names,
unsupported string escapes, and insufficient output capacity.

`browser_js_dom.sura` adds one fixed DOM-host operation:
`document.getElementById(id).textContent = text`. The DOM gate executes that
source through the VM, records one bounded mutation overlay, and verifies that
the renderer replaces the first descendant text node while suppressing later
descendant text nodes.

The graphical browser collects same-document inline `<script>` text into a
16-KiB source buffer, compiles at most 512 instructions/128 constants/128
bindings, executes with a 64-value stack and an 8,192-instruction budget, and
isolates unsupported-script errors from page rendering. HTML `<script>` and
`<style>` contents use raw-text scanning, including JavaScript `<` operators.
The built-in `sura.local/forms` page executes an inline assignment script.
The `sura.local/javascript` page has an inline `onclick` button whose exact
ID-based `textContent` assignment is hit-tested, executed, and redrawn. The
full screenshot gate requires `SURA_OS_BROWSER_JS_OK`,
`SURA_OS_BROWSER_JS_PAGE_OK`, and `SURA_OS_BROWSER_JS_CLICK_OK`.

This layer is not a general ECMAScript or Web API implementation. JavaScript
floating-point numbers, escapes, arrays, objects, functions, closures,
exceptions, promises, modules, external scripts, general DOM mutation,
`addEventListener`, timers, fetch, storage, and other Web APIs are not
implemented. The only event path is a parsed inline `onclick` attribute, and
the only DOM write is the exact ID-based `textContent` statement above. An
`onclick` handler currently consumes the pointer click before default link or
form activation.

The first browser WebAssembly layer is also allocation-free and bounded:

```powershell
.\tools\sura_browser_wasm_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

`browser_wasm.sura` parses WebAssembly MVP custom, type, function, export, and
code sections into caller-owned fixed-capacity tables. It executes i32/i64
parameters, locals and results, direct calls, blocks, loops, if/else,
`br`/`br_if`, return, local access, integer constants, comparisons, and the
implemented integer arithmetic opcodes. Call depth and instruction count are
bounded. The gate executes arithmetic, a direct call, both if branches, and an
infinite loop stopped at its exact budget; it also rejects a bad magic header,
an unsupported memory section, malformed LEB128, an invalid branch depth, and
insufficient table capacity.

The graphical browser detects a fetched HTTP or HTTPS response body beginning
with the WebAssembly magic bytes, invokes its zero-argument `run` export,
accepts an i32 or i64 result, and renders the value as HTML. The binary fixture
`sura.local/webassembly` returns 42. The full screenshot gate requires
`SURA_OS_BROWSER_WASM_OK` and `SURA_OS_BROWSER_WASM_PAGE_OK` and writes
`build/os/SuraOS-browser-webassembly.ppm`.

This is not a complete WebAssembly implementation. Imports, tables, linear
memory, globals, start functions, elements, data, floating point, references,
exceptions, threads, SIMD, WASI, JIT compilation, the JavaScript
`WebAssembly` object, fetch/instantiate integration, and complete
specification validation are not implemented.

The Ring 3 transition and syscall path has a separate executed gate:

```powershell
.\tools\sura_ring3_qemu_gate.ps1 -Engine .\SuraLanguage.exe

# Build and inspect without claiming execution:
.\tools\sura_ring3_qemu_gate.ps1 -Engine .\SuraLanguage.exe -CompileOnly
```

The executed form leaves Boot Services, installs its own GDT, TSS, and IDT,
splits OVMF's 2-MiB identity leaf into 4-KiB leaves when necessary, marks only
one code page and four stack pages as user accessible, and enters selector 35
at CPL 3. A DPL-3 `INT 0x80` handler requires saved CS 35, returns the sum
through IRETQ, and the user code then reaches a kernel-stack dispatcher through
`SYSCALL`. Success requires `SURA_RING3_READY` and
`SURA_RING3_CPL3_SYSCALL_OK` with QEMU exit code 33. This proves the transition
and syscall plumbing, not a scheduled or isolated desktop application process.

The minimal OS integration has a separate build-and-run gate:

```powershell
.\tools\sura_os_vm.ps1 -Engine .\SuraLanguage.exe

# Generate build\os\SuraOS.efi and SuraOS.img without launching QEMU:
.\tools\sura_os_vm.ps1 -Engine .\SuraLanguage.exe -CompileOnly

# Attach the current terminal to the post-ExitBootServices COM1 shell:
.\tools\sura_os_vm.ps1 -Engine .\SuraLanguage.exe -Interactive

# Keep interactive COM1 but suppress the graphical QEMU window:
.\tools\sura_os_vm.ps1 -Engine .\SuraLanguage.exe `
  -Interactive -HeadlessInteractive

# Capture the actual QEMU framebuffer after the desktop is ready:
.\tools\sura_os_screenshot.ps1 -Engine .\build\SuraLanguage_os_next.exe
```

The executed form uses QEMU TCG rather than host hardware. Success requires
the desktop, FAT32 storage, DHCP, DNS, TCP, HTTP, and browser markers; the
post-self-check shell must answer `status` and `mem`, accept `shutdown`, emit
`SURA_OS_SHUTDOWN`, arm ACPI S5, and make QEMU exit with code 0. Interactive
mode supports `help`, `status`, `mem`, `about`, `clear`, `shutdown`, and
`reboot`. It uses an ephemeral
`127.0.0.1` TCP bridge for COM1 so PowerShell retains normal line editing; the
listener is not bound to an external interface. The screenshot gate uses a
second loopback-only QMP connection, types a browser address, verifies a live
explicit-HTTP navigation to `example.org`, then verifies that scheme-less
`suralang.site` selects HTTPS and completes certificate validation,
encrypted response, same-host external stylesheet fetch, DOM box conversion,
and painting through `SURA_OS_BROWSER_HTTPS_OK`,
`SURA_OS_BROWSER_EXTERNAL_CSS_OK`, `SURA_OS_BROWSER_DOM_BOX_OK`, and
`SURA_OS_BROWSER_DOM_RENDER_OK`. It also clicks a live DOM anchor, injects a
PS/2 IntelliMouse wheel event, holds Page Down to exercise viewport scrolling
and device-independent key repeat, and holds Backspace over a populated UTF-8
address. Those paths require `SURA_OS_BROWSER_LINK_OK`,
`SURA_OS_BROWSER_WHEEL_OK`, `SURA_OS_BROWSER_SCROLL_OK`,
`SURA_OS_KEY_REPEAT_OK`, and `SURA_OS_BROWSER_BACKSPACE_REPEAT_OK`, and
writes `build/os/SuraOS-desktop.ppm` and
`build/os/SuraOS-browser.ppm`. The same run loads the
`sura.local/webassembly` binary fixture, executes `run()`, verifies result 42,
and writes `build/os/SuraOS-browser-webassembly.ppm`. The screenshot QEMU
instance includes xHCI USB keyboard and mouse devices and requires
`SURA_OS_XHCI_INPUT_READY`; PS/2 remains a fallback. A failed input dispatch is
recorded and recovered instead of terminating the kernel loop. It also edits
Text Editor, verifies `Ctrl+F`
and bounded `Ctrl+H` replacement, saves `copy.sura`, requires the `.sura`
syntax-color path, opens Calculator, sends keyboard
`50 - 31 = 19`, clicks the 4x4 keypad for `7 + 5 = 12`, requires
`SURA_OS_CALCULATOR_KEYPAD_RESULT_OK`,
`SURA_OS_CALCULATOR_RING3_READY`, `SURA_OS_CALCULATOR_RING3_OK`, and
`SURA_OS_CALCULATOR_CR3_OK`, and requires
the corresponding `SURA_OS_EDITOR_RING3_READY`, `RING3_OK`, and `CR3_OK`
markers plus the File Explorer `RING3_READY`, `RING3_OK`, and `CR3_OK` markers
and Terminal `RING3_READY`, `RING3_OK`, and `CR3_OK` markers before continuing
plus System Information `RING3_READY`, `RING3_OK`, and `CR3_OK` markers before
continuing the remaining desktop regression. The graphical boot also requires
`SURA_OS_USER_SCHEDULER_READY`, then starts each worker once with its own event
queue and kernel stack. Requests wake the blocked process through kernel IPC;
the worker receives through checked syscall 2 and blocks again through syscall
5. `SURA_OS_USER_PROCESSES_PERSISTENT_OK` is emitted only after all seven real
GUI paths preserve the same saved user frame, CR3 root, and kernel stack across
requests. Each worker's user code is read-only/executable, mailbox, event page,
and guarded stack are writable/NX, and shared kernel PML4 entries remain
supervisor-only. Neither gate modifies host firmware variables or boot entries.

The entry function is selected in this order: `efi_main`, `kernel_main`,
`main`. If none exists, top-level statements become the EFI entry body.
UEFI passes the image handle and system-table pointer in the first two
parameters.

```sura
func efi_main(image_handle: u64, system_table: ptr) -> u64 do
  uefi.write("Hello from Sura")
  uefi.newline()
  return u64(0)
end
```

## Freestanding source modules

Freestanding programs can split compiler, kernel-library, and driver code
across files with the existing import syntax:

```sura
import "memory/page_tables.sura"
import "drivers/pci.sura"
```

Relative paths are resolved from the file that contains each import, so nested
modules do not depend on the process working directory. The freestanding
loader parses modules into one compilation unit, includes each normalized path
once, and rejects circular imports and missing files before machine-code
generation. Imported definitions currently share one global namespace;
namespace isolation and per-module visibility are not implemented yet.

## Freestanding scalar types

The parser accepts `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`,
`isize`, `usize`, and `ptr`. The hosted VM currently represents these aliases
as Sura numbers. The freestanding backend uses 64-bit integer registers and
applies the requested width at memory and port-I/O boundaries.

Use `u64("0xffff800000000000")`, `usize("0x...")`, or `ptr("0x...")` for
integer constants that cannot be represented exactly by the normal numeric
literal format. `addr_of(local)` returns the address of a fixed-width local.
It also accepts a top-level static name.

## Static data and globals

Top-level assignments in the freestanding target are compile-time static
declarations. Scalar initializers must be compile-time integers.

```sura
boot_count: u64 is 0
page is static.zero(4096, 4096)
signature is static.bytes([83, 85, 82, 65], 16)
name is static.utf8("sura-device")
wide_name is static.utf16("Sura")
table is static.u64([0, 1, 2, 3])
```

Available initializers are `static.zero(size, alignment?)`,
`static.bytes/u8/u16/u32/u64(array, alignment?)`, `static.utf8(text)`,
`static.utf16(text)`, and `static.struct(Type, count?)`. Static byte strings
are null terminated. Static objects live in the writable PE `.data` section.
Their names evaluate to addresses.

A function must use the existing Sura `global` declaration before assigning
to a mutable top-level scalar:

```sura
counter: u64 is 0

func increment() -> u64 do
  global counter
  counter += 1
  return counter
end
```

Static buffers and tables are modified through typed fields, `mem.write*`, or
atomic operations rather than by assigning a new address to their names.

## Memory-layout structs and typed pointers

Typed fields give `struct` a concrete freestanding memory layout. Natural
layout aligns each field to its width. Add `packed` when a hardware or firmware
format has no padding.

```sura
struct PciConfigHeader packed do
  vendor_id: u16
  device_id: u16
  command: u16
end

header_storage is static.struct(PciConfigHeader)

func probe() -> u64 do
  header: ptr[PciConfigHeader] is header_storage
  header.command is 7
  return header.vendor_id
end
```

Field loads use the declared signedness and width; field stores write exactly
that width. Embedded structs have a layout but cannot yet be loaded or assigned
as one scalar value. Use a `ptr[NestedStruct]` field for an address.

Freestanding comparisons, division, remainder, and right shift also follow the
declared signedness of their operands. `u64` and pointer-sized unsigned values
use unsigned conditions, `div`, and logical right shift; signed values use
signed conditions, `idiv`, and arithmetic right shift.
`tools/sura_integer_semantics_qemu_gate.ps1` executes boundary cases including
`0xffffffffffffffff > 7`, unsigned division and remainder, the high-bit logical
shift, and negative signed division, remainder, comparison, and shift before
emitting `SURA_INTEGER_SEMANTICS_OK`.

- `sizeof(StructName)`, `alignof(StructName)`
- `offset_of(StructName, field)`
- `ptr.add(address, byte_offset)`
- `ptr.index(address, index, element_size)`
- `ptr.field(address, StructName, field)`
- `ptr.align_up/down(value, alignment)`, `ptr.is_aligned(value, alignment)`

## UEFI services

- `uefi.write(text_literal)`, `uefi.newline()`, `uefi.clear()`
- `uefi.set_color(foreground, background)`, `uefi.stall(microseconds)`
- `uefi.shutdown()`
- `uefi.image_handle()`, `uefi.system_table()`
- `uefi.allocate_pages(type, memory_type, pages, address_ptr)`
- `uefi.free_pages(address, pages)`
- `uefi.get_memory_map(size_ptr, map_ptr, key_ptr, descriptor_size_ptr, version_ptr)`
- `uefi.allocate_pool(memory_type, size, buffer_ptr)`, `uefi.free_pool(buffer)`
- `uefi.locate_protocol(guid_ptr, registration, interface_ptr)`
- `uefi.exit_boot_services(map_key)`
- `uefi.gop_framebuffer()`, `uefi.gop_framebuffer_size()`
- `uefi.gop_width()`, `uefi.gop_height()`, `uefi.gop_stride()`
- `uefi.gop_pixel_format()`

The GOP helpers use firmware graphics initialization, so the fallback
framebuffer does not require a vendor-specific NVIDIA, AMD, or Intel driver.
The QEMU path also has a separate VirtIO GPU 2D scanout driver described
below. Accelerated 3D still requires a 3D command API and host/physical GPU
driver stack.

## Framebuffer desktop libraries

`stdlib/freestanding/framebuffer.sura` provides checked 32-bit GOP surface
operations:

- RGB/BGR pixel conversion for GOP formats 0 and 1
- clipped pixel, filled rectangle, outline, horizontal and vertical line
  operations
- integer Bresenham lines and a basic application-icon primitive
- a bounded 64 MiB surface contract
- unrolled 64-byte double-buffer presentation and a sampled framebuffer hash

`stdlib/freestanding/font5x7.sura` provides the fixed-width terminal bitmap
font with distinct lowercase ASCII, digits, and punctuation.
`stdlib/freestanding/font_ui.sura` provides proportional 16-pixel,
two-bit-antialiased printable ASCII and modern precomposed Hangul for windows,
menus, editors, and the browser. Text supports caller-selected colors; the
terminal intentionally retains fixed-width cells.

The caller owns all surface memory. `fb_surface_init` validates dimensions,
stride, format, and the 64 MiB size bound, but it cannot discover the actual
allocation length behind an arbitrary pointer. The caller must provide at
least `stride * height * 4` writable bytes.

The current OS allocates its backbuffer with UEFI page allocation before
capturing the final memory map. After `ExitBootServices`, it renders the
desktop to that buffer and presents it to the GOP surface. When the VirtIO GPU
path is active, the completed GOP front surface is also mirrored into the
VirtIO resource; ordinary cursor movement copies and transfers only the
combined old/new cursor damage rectangle. The QEMU gate observes
`SURA_OS_DESKTOP_OK`; `tools/sura_os_screenshot.ps1` additionally captures the
actual GOP pixels through QMP.

## VirtIO GPU 2D scanout

`stdlib/freestanding/virtio_pci.sura` implements the modern VirtIO PCI
transport used by QEMU's `1af4:1050` GPU function. It walks vendor-specific
PCI capabilities, maps the common, notify, ISR, and device configuration
regions from their BAR-relative offsets, negotiates `VIRTIO_F_VERSION_1`, and
configures control queue zero with caller-owned descriptor, available, and
used areas.

`stdlib/freestanding/virtio_gpu.sura` uses that transport to execute:

- `GET_DISPLAY_INFO`
- `RESOURCE_CREATE_2D` with `B8G8R8X8_UNORM`
- `RESOURCE_ATTACH_BACKING`
- `SET_SCANOUT`
- `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`
- `RESOURCE_UNREF` during the standalone gate

The desktop keeps GOP as the primary safe surface. If the modern GPU function,
capabilities, negotiated version, queue, reported mode, or BGR byte layout do
not match the bounded path, startup emits `SURA_OS_VIRTIO_GPU_FALLBACK` and
continues on GOP. A successful secondary scanout emits
`SURA_OS_VIRTIO_GPU_READY`. Run the same full OS gate without the GPU to
exercise the fallback:

```powershell
.\tools\sura_os_vm.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe `
  -DisableVirtioGpu
```

`tools/sura_virtio_gpu_qemu_gate.ps1` is the direct device proof. It boots
with GOP disabled, creates a 640x480 resource, transfers a deterministic
four-color image, captures the VirtIO display through QMP, and checks five
exact RGB samples before releasing the resource:

```powershell
.\tools\sura_virtio_gpu_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

This is a polling QEMU 2D driver, not general GPU acceleration. It has no
interrupt/MSI-X delivery, cursor queue, EDID handling, hot-plug/config-change
recovery, multiple active scanouts, resource blobs, virgl/Venus, 3D command
submission, IOMMU support, or physical-hardware proof.

## Intel HDA PCM output

`stdlib/freestanding/hda.sura` provides the first executed audio-output path.
It discovers a PCI multimedia-audio function, enables MMIO and bus mastering,
resets the controller through GCTL, discovers a codec through STATESTS, and
uses the immediate-command registers to enumerate the root nodes, Audio
Function Group, Audio Output converter, and output Pin Complex.

The current stream contract is deliberately fixed and explicit:

- 48 kHz, signed 16-bit little-endian, stereo PCM
- stream tag 1 and the controller's first output stream descriptor
- one caller-owned 16-byte BDL entry
- one caller-owned, page-aligned 192000-byte sample region
- one contiguous 48-page identity-mapped DMA allocation containing both
  control data and samples
- polling start, LPIB progress, and stop with bounded waits

The direct executed gate generates one second of deterministic bipolar sample
data and captures the QEMU audio backend as a WAV file. The PowerShell wrapper
parses the file rather than trusting a serial marker: it requires PCM,
48 kHz, two channels, 16 bits, a substantial data payload, and both negative
and positive samples. Run it with:

```powershell
.\tools\sura_hda_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The full OS reserves the DMA area before `ExitBootServices`, transfers a
bounded 125-ms startup slice, stops the stream, and leaves the initialized
controller available for later services. A successful path emits
`SURA_OS_HDA_AUDIO_READY`. The normal automated VM uses QEMU's silent backend,
so the proof does not play an audible tone on the host. The same desktop can
be booted without the HDA device to require the safe fallback marker:

```powershell
.\tools\sura_os_vm.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe `
  -DisableHda
```

This is not yet a general application audio system. It has no IRQ/MSI
delivery, CORB/RIRB command ring, mixer, multiple client streams, resampling,
input capture, jack sensing, unsolicited codec events, power management,
hot-plug recovery, IOMMU mapping, or physical-hardware proof.

## ACPI power-off and reset

`stdlib/freestanding/power.sura` implements the conventional ACPI fixed-
hardware paths used by the current x86-64 image. Before `ExitBootServices`,
the OS checksum-validates the RSDP, FADT, and DSDT, reads the PM1 control
register descriptions, parses a static `_S5` package, and records the FADT
`RESET_REG` and `RESET_VALUE`. After boot it emits
`SURA_OS_ACPI_POWER_READY` only when both shutdown and reset are available.

The `shutdown` shell command and Start-menu shutdown action enable ACPI mode
when necessary and write `SLP_TYP` plus `SLP_EN` to PM1a and optional PM1b.
The `reboot` shell command writes the FADT reset value through its Generic
Address Structure. Unsupported firmware falls back to QEMU's
`isa-debug-exit`; that fallback is retained for deterministic diagnostics and
is not the normal QEMU path.

The direct gates and the complete-OS reboot gate are:

```powershell
.\tools\sura_power_shutdown_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
.\tools\sura_power_reset_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
.\tools\sura_os_reboot_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The two direct gates require QEMU exit code 0 after the ACPI write. The
complete OS gate sends `reboot` through the persistent CPL-3 Terminal worker,
returns to the kernel power service, requires
`SURA_OS_ACPI_RESET_ARMED`, and exits QEMU through the firmware reset path.
The normal OS VM gate similarly requires
`SURA_OS_ACPI_POWER_OFF_ARMED` before S5 power-off.

This is not a general AML or platform power-management implementation. It
does not execute `_PTS` or other AML methods, support hardware-reduced ACPI
sleep-control registers, suspend states, wake sources, battery telemetry,
thermal policy, CPU performance states, or physical-hardware validation. The
static AML parser only accepts the bounded `_S5` package forms needed by the
executed OVMF path.

`os/icons.sura` supplies seven embedded 16x16 palette-index raster images for
the logo, Terminal, File Explorer/folders, System Information, Text Editor,
Calculator, and Browser. The desktop, window title bars, Start menu, taskbar,
and File Explorer rows scale those image assets instead of using placeholder
square-and-line icons.

`examples/os/framebuffer_features.sura` independently compiles the surface,
pixel, rectangle, line, icon, font, hash, and presentation paths into an EFI
feature image. The OS VM gate is the executed proof for the same libraries.

## USB and PS/2 desktop input

`stdlib/freestanding/ps2.sura` provides a polling Intel 8042 path for the
current x86-64 desktop milestone:

- controller input/output readiness waits with fixed spin limits
- translated Set-1 keyboard decoding, left/right Shift, Caps Lock, printable
  ASCII, Enter, Backspace, and extended-key tracking
- standard three-byte relative mouse packet assembly plus IntelliMouse
  four-byte wheel negotiation, signed movement/wheel deltas, bounds, and
  button transition state
- keyboard and auxiliary-device enable commands used by QEMU's emulated
  controller

The OS combines COM1 and device input in one polling shell loop. When
`os/input.sura` successfully enumerates the QEMU xHCI keyboard and mouse, it
consumes USB reports and leaves PS/2 initialized as a fallback. Printable
keys, Backspace, and Enter update the graphical terminal. Mouse packets move a
software pointer. xHCI readiness emits `SURA_OS_XHCI_INPUT_READY`; the first
controller, keyboard, and mouse events emit
`SURA_OS_PS2_READY`, `SURA_OS_KEYBOARD_OK`, and `SURA_OS_MOUSE_OK`. Shift and
the first mouse-button press additionally emit `SURA_OS_SHIFT_OK` and
`SURA_OS_MOUSE_CLICK_OK`. Browser wheel delivery emits
`SURA_OS_BROWSER_WHEEL_OK`.

`examples/os/ps2_features.sura` checks scan-code and packet decoding in
generated EFI code. `tools/sura_os_screenshot.ps1` uses QMP to type `status`
through QEMU's emulated PS/2 keyboard, moves the emulated mouse, requires the
Shift/keyboard/move/click markers and `kernel: ready`, and captures the
resulting framebuffer.

`stdlib/freestanding/key_event.sura` defines the common physical-key event:
scan code, normalized key position, Unicode scalar, press/release/repeat state,
Shift/Ctrl/Alt/Caps/Hangul modifiers, and sequence number. The PS/2 decoder
produces those events from translated Set-1 bytes, including dedicated Korean
106-key Hangul/Hanja make codes.

`stdlib/freestanding/text_input.sura` consumes `KeyEvent` values, supports
English/Hangul mode, and maps Korean from physical key positions so Caps Lock
does not alter two-set composition. It composes modern initial/medial/final
jamo including compound vowels and finals, splits a final before a following
vowel, exposes a live composition code point, handles composition Backspace,
and writes checked one- through four-byte UTF-8.
`tools/sura_text_input_qemu_gate.ps1` executes Set-1 press/release/repeat and
Shift/Ctrl/Alt/Caps events, the exact `dkssudgktpdy` → `안녕하세요`
physical-key sequence, compound-vowel/final deletion, and exact UTF-8 bytes
before requiring `SURA_KEY_EVENT_OK`, `SURA_TEXT_INPUT_ANNYEONGHASEYO_OK`,
and `SURA_TEXT_INPUT_OK`.

The desktop converts every decoded USB or PS/2 keyboard transition into `KeyEvent`,
serializes it through a 64-entry kernel-owned `IpcQueue`, and only then routes
the dequeued event to the active window. Queue initialization and round-trip
encoding emit `SURA_OS_INPUT_EVENT_OK`. This is a kernel polling queue, not
process IPC delivery or an interrupt-driven device queue.

Text Editor and Terminal connect this layer to their CPL-3 mailboxes. Right
Alt or the dedicated Hangul key switches input mode, committed scalars are
packed as UTF-8, composition-aware Backspace stays in the input method, and
`stdlib/freestanding/font_hangul.sura` provides UTF-8 decoding, common Unicode
display normalization, and a geometric fallback. Common desktop UI and Text
Editor use `stdlib/freestanding/font_ui.sura`, whose proportional 16-pixel,
2-bit antialiased atlas contains printable ASCII and every modern precomposed
Hangul syllable. `tools/sura_ui_font_generate.ps1` generates the atlas from a
SHA-256-pinned Noto Sans CJK KR OTF; the required SIL Open Font License 1.1 copy
is in `third_party/noto-cjk/OFL.txt`. The QMP screenshot gate types Korean
through the emulated keyboard, requires `SURA_OS_KOREAN_INPUT_OK`, and captures
the rendered editor in `build/os/SuraOS-korean-input.ppm` with its `INPUT: KO`
indicator. It also enters the exact `dkssudgktpdy` → `안녕하세요` sequence in
Terminal and requires `SURA_OS_TERMINAL_KOREAN_INPUT_OK`. Terminal stores
Unicode code points in its visible cell buffer and draws them with the same UI
font; its built-in command names remain ASCII. Text Browser uses the same
composition and antialiased atlas in its address bar. A pointer click or F6
selects that field, and the next printable key replaces the current address.
DNS host names remain
ASCII until IDNA is implemented, while
UTF-8 path bytes are converted to uppercase percent encoding before the HTTP
request. The screenshot gate enters Korean text in the address bar, requires
`SURA_OS_BROWSER_KOREAN_INPUT_OK`, and captures
`build/os/SuraOS-browser-korean.ppm`. Its separate ASCII
`http://example.org` navigation performs the live HTTP-response check and
proves the request host is
not fixed to the boot-time `example.com`. The same gate separately requires
the live `suralang.site` HTTPS marker; the Korean-input capture does not
require another live request. Valid UTF-8 page text is preserved and rendered
through the atlas where a glyph exists. DNS names require ASCII until IDNA
exists, and the atlas does not cover Hanja, all Unicode scripts, or complex
text shaping.

The English-US/Korean-two-set preference is global rather than tied to one
application. Right Alt or the dedicated Hangul key changes it from any active
window, and the keyboard row in System Information can be clicked to make the
same change. `SETTINGS.CFG` stores the selected mode. Run
`tools/sura_input_layout_qemu_gate.ps1` to execute one QMP input boot, preserve
the modified FAT32 disk, boot that exact disk again, and require
`SURA_OS_INPUT_LAYOUT_RESTORED`.

The current input path still has no IRQ/MSI delivery, scheduled-process event
delivery, layouts beyond English-US and Korean two-set, Hanja candidate
conversion, USB hubs, hot-plug recovery, arbitrary HID report-descriptor
 parsing, or five-button support. The USB report converter preserves an
 optional wheel byte, while the PS/2 fallback negotiates QEMU's IntelliMouse
 device ID and decodes its fourth wheel byte. Pointer clicks are dispatched to
 the window manager.

## Graphical terminal

`stdlib/freestanding/text_terminal.sura` provides a fixed-capacity ASCII cell
buffer for graphical consoles. It supports:

- caller-selected columns and rows, capped at 256x128
- printable input, Backspace, newline, wrapping, and upward scrolling
- complete clear with cursor reset
- bounded C-string and unsigned-decimal output
- direct 5x7 framebuffer drawing

Sura OS uses a 46x14 instance with the 5x7 font at 2x scale inside the terminal
window. COM1 and decoded keyboard input from xHCI or the PS/2 fallback update
the same command buffer and graphical history. `help`, `status`,
`mem`, `about`, unknown-command output, the prompt, and command text remain on
screen and scroll together. `clear` clears the cell buffer before drawing the
next prompt. `shutdown` remains available from both input paths.

`examples/os/text_terminal_features.sura` verifies wrap, scroll, numeric
output, framebuffer drawing, and clear in a generated EFI image.
`tools/sura_os_screenshot.ps1` additionally fills the terminal through
QEMU PS/2 input, requires `SURA_OS_TERMINAL_SCROLL_OK`, runs `clear`, requires
`SURA_OS_CLEAR_OK`, then leaves a visible `status` result in the captured
framebuffer.

## Window manager foundation

`stdlib/freestanding/window_manager.sura` provides fixed-capacity desktop
window metadata without allocating memory. It manages:

- visible and active state, z-order, focus, and top-window hit testing
- title-bar dragging, bottom-right resizing, and close-button hit testing
- normal, minimized, maximized, and fullscreen state with geometry restore
- clamping windows to the desktop area above the taskbar
- at most 64 caller-owned window records

Rendering and application contents remain caller-owned. The module can resize,
minimize, maximize, enter fullscreen, restore, close, and show a registered
window. Taskbar drawing and process creation are outside this module.

`examples/os/window_manager_features.sura` checks overlapping-window
selection, focus changes, z-order, drag offsets, interactive resize,
minimize/show, maximize/restore, fullscreen/restore, screen-bound clamping,
close, and activation of the next top window. `tools/sura_uefi_target_smoke.ps1`
compiles that example and validates its generated EFI image and diagnostic.
The executed `os/sura_os.sura` desktop connects the manager to six kernel-owned
windows: Terminal, System Information, File Explorer, Text Editor, Calculator,
and Browser. PS/2 left-button input changes focus and z-order, drags title
bars, and closes a window. Keyboard input is routed according to the active
application. `tools/sura_os_screenshot.ps1` drives those actions through QEMU,
requires the focus, drag, resize, minimize, maximize, fullscreen, close, and
restore markers, and captures the dragged overlapping windows before
closing the information window. It then reopens that window from the taskbar,
requires `SURA_OS_WINDOW_REOPEN_OK`, opens Start, requires
`SURA_OS_START_MENU_OK`, captures the menu, and selects Terminal.

`stdlib/freestanding/window_server.sura` adds caller-owned application
surfaces, z-order composition into a separate output buffer, bounded damage
rectangles, failed-owner surface removal, a shared clipboard, dark/light theme
state, 100–300% scale settings, and a two-monitor virtual-desktop model.
`stdlib/freestanding/ui.sura` supplies reusable button, text-input, list, menu,
and checkbox roles, focus order, hit testing, theme colors, app-surface
drawing, and accessible role/name metadata.
`tools/sura_window_server_qemu_gate.ps1` executes exact pixel composition,
damage, owner removal, clipboard, dark theme, 150% scale, two monitor records,
common UI, and all window-state transitions.

The graphical OS also starts `os/user_window_server.sura` as a sixth persistent
Ring-3 process. It has a distinct CR3 and kernel stack, executes a bounded
damage compositor, and receives a writable shared mapping of the real desktop
backbuffer. The full QEMU gate requires
`SURA_OS_WINDOW_SERVER_RING3_READY`, `SURA_OS_WINDOW_SERVER_CR3_OK`,
`SURA_OS_WINDOW_SERVER_SHARED_BUFFER_OK`, and
`SURA_OS_WINDOW_SERVER_RING3_OK`. The shared-buffer check reads one real
backbuffer pixel, writes a test value, verifies it, and restores the original
value from Ring 3.

This is an intermediate migration, not the completed Window Server design.
The six built-in applications still render their window contents in ring 0;
their full-size app-owned surfaces are not yet composed by the Ring-3 server.
Window animations, drag and drop, a system accessibility service, and real
multi-output presentation are also not implemented.

## Desktop shell

The executed desktop draws persistent taskbar buttons for all six applications
with an active-window underline. The Start button toggles a bounded menu above
the taskbar. Start, desktop, and taskbar entries focus and raise a visible
window or reopen a closed one. The default browser window is hidden until
activated, reducing initial overlap. A Shut Down menu action follows the same
QEMU-exit path as the shell command.

`stdlib/freestanding/desktop_shell.sura` owns the fixed-layout hit testing and
Start open state. It converts pointer presses into bounded actions without
performing rendering or window changes itself.

`stdlib/freestanding/rtc.sura` reads the PC CMOS clock only after its update
window closes, requires two matching samples, and converts BCD and 12-hour
formats. The OS renders `HH:MM` on the taskbar, refreshes it when the sampled
second changes, and emits `SURA_OS_RTC_OK` after a valid QEMU RTC read.
`examples/os/desktop_shell_features.sura` and `examples/os/rtc_features.sura`
provide compile/image coverage for these modules.

The desktop shell is still kernel-owned fixed UI. It has no general
application-window protocol, notifications, settings store, or
user-configurable launcher entries.

## Desktop applications and Ring 3 workers

`stdlib/freestanding/desktop_apps.sura` supplies allocation-free state models
for the first built-in applications:

- File Explorer selection over at most 32 caller-provided entries
- Text Editor input in a caller-owned buffer, including an explicit UTF-8
  cursor and selection anchor, Enter, Backspace, range deletion, and bounded
  selection copy
- unsigned-integer Calculator input for `+`, `-`, `*`, `/`, `=`, and `C`,
  with bounded values, division/overflow errors, a full expression display,
  keyboard input, and a clickable 4x4 keypad

The executed OS registers File Explorer, Text Editor, Calculator, and Browser
windows alongside Terminal and System Information. Their image icons, Start
entries, and persistent taskbar buttons focus or reopen them. File Explorer
mounts the type-`0x7f` SuraFS document partition through VFS, renders `/문서`,
opens `/문서/메모.txt`, and returns through its synthetic parent row. QEMU
input creates a folder, moves it to `/휴지통`, creates and renames a file,
creates `한글.sura` through the Korean input method, appends ASCII, Enter, and
`한글 ` through the editor, completes `Ctrl+F` next-match selection and
bounded `Ctrl+H` replacement, evaluates keyboard `50 - 31 = 19`, and clicks
the keypad for `7 + 5 = 12`. The gate requires `SURA_OS_FILES_APP_OK`,
`SURA_OS_DIRECTORY_OK`, `SURA_OS_SURAFS_CREATE_OK`,
`SURA_OS_SURAFS_RENAME_OK`, `SURA_OS_SURAFS_TRASH_OK`,
`SURA_OS_SURAFS_SAVE_AS_OK`, `SURA_OS_EDITOR_INPUT_OK`,
`SURA_OS_EDITOR_FIND_OK`, `SURA_OS_EDITOR_REPLACE_OK`,
`SURA_OS_EDITOR_SYNTAX_OK`,
`SURA_OS_CALCULATOR_RESULT_OK`, and
`SURA_OS_CALCULATOR_KEYPAD_RESULT_OK`, then captures
`build/os/SuraOS-apps.ppm`.

Text Editor uses a 4096-byte state buffer and an 8288-byte, three-page Ring-3
mailbox containing a 96-byte checked header, the document, and one bounded
insertion payload. Enter is stored as LF, Backspace and Delete preserve UTF-8
code-point boundaries, and insertion replaces the selected range. The
framebuffer text box highlights selections, draws the cursor at its byte
boundary, wraps long lines, and follows the cursor viewport. `Ctrl+A`,
`Ctrl+C`, `Ctrl+X`, and `Ctrl+V` operate on the editor clipboard; Shift with
Left/Right or Home/End extends the selection. `Ctrl+F` opens a bounded UTF-8
find field and selects the next byte-exact match. `Ctrl+H` adds a replacement
field and sends the selected replacement through the same checked Ring-3 edit
mailbox. SuraFS atomically replaces and reloads the selected file. File Explorer
offers `Ctrl+N` or F4 for a file, `Ctrl+Shift+N` or F3 for a folder, F2 for
inline UTF-8 rename, Delete with confirmation for recycle-bin transfer, arrow
selection, Home/End, and Enter to open. It enumerates up to all 32 formatted
SuraFS nodes and follows selection through a six-row viewport and scrollbar.
Deleting inside `/휴지통` removes the selected tree permanently. Text Editor
provides `Ctrl+S` and a `Ctrl+Shift+S` UTF-8
Save As field. Save As keeps the current directory, rejects an existing
different target instead of silently overwriting it, rolls back a failed new
file write, and changes the active path only after the full document is
stored. The document directory also receives a non-destructive 62-byte
`main.sura` starter file when it does not already exist. Opening a lower-case
`.sura` path, or saving the active document with that suffix, changes the
window to `SURA CODE EDITOR` and builds a bounded byte-category map for Sura
keywords, quoted strings, line comments, and numbers. The UTF-8 renderer
applies those colors without changing document bytes, cursor offsets, or the
selection range. This is lexical highlighting, not parser-backed diagnostics
or code completion.

File
Explorer, Text Editor, Calculator, and Terminal UI and input routing remain in
the kernel, but their state models in `os/user_file_explorer.sura`,
`os/user_text_editor.sura`, and `os/user_calculator.sura` are copied to
dedicated read-only executable user pages. Terminal command-line editing and
command recognition use the same boundary through `os/user_terminal.sura`.
Each dispatch enters selector 35 at CPL 3, reads and updates one bounded
mailbox, invokes DPL-3 vector `0x80`, and returns to a clean kernel continuation
stack. Only after File Explorer returns does ring 0 perform VFS/SuraFS
traversal and settings persistence. Only after Text Editor returns does ring 0
atomically replace and autosave the active SuraFS file. Terminal rendering, memory
inspection, `clear`, and `shutdown` occur
only after its worker returns.

System Information activation copies a kernel-gathered framebuffer, free-page,
storage, and network snapshot to `os/user_system_info.sura`. The worker checks
the bounded fields and derives display-safe free-memory and pixel counts before
returning. Hardware reads and System Information rendering remain in ring 0.

All seven workers use `os/user_worker.sura` and have separate physical PML4 roots:
existing kernel mappings are copied into supervisor-only PML4 entries, while a
different lower-half slot contains process-owned W^X code, a writable/NX
mailbox, and a four-page writable/NX stack preceded by an unmapped guard page.
The QEMU gate requires System Information, Terminal, File Explorer, Calculator,
Editor, Window Server, and Browser request-validator execution and CR3 markers.
`os/user_browser.sura` receives a private 384-byte mailbox containing a copied
URL snapshot, validates bounded scheme/host/path bytes, grants network
capability only to external hosts, and denies storage and device capabilities.
Initial requests, form actions, and redirect targets are authorized through
that worker. Uncached address-bar DNS starts as a bounded request and is polled
incrementally from the desktop loop; keyboard and mouse events continue during
that wait, and address editing cancels a stale URL snapshot. TCP connect is a
second begin/poll state: the desktop loop advances SYN/SYN-ACK in bounded
budgets, accepts input and F6 cancellation while pending, and consumes the
completed connection once in the HTTP or HTTPS continuation. Dotted-decimal
IPv4 literals bypass DNS. Scheme-less hostnames select HTTPS; explicit
`http://` remains available. For the first HTTPS request, ClientHello
transmission and TLS record reception form a third begin/poll state driven by
the desktop loop. The gate requires `SURA_OS_BROWSER_NAV_TLS_BEGIN` and
`SURA_OS_BROWSER_NAV_TLS_DONE` for live `suralang.site`. Response reception and
decoding, later stylesheet/image TLS connections, DOM, layout, rendering, plus
link and form navigation still run on a synchronous kernel call stack. Bounded
polling loops cooperatively service desktop input, so pointer movement and
other window activity continue; the full QEMU gate injects movement during the
live response phase and requires `SURA_OS_BROWSER_NAV_FETCH_INPUT_OK`. F6
directly cancels pending DNS, TCP, or TLS state. During the synchronous
response/resource phase it records a cancellation request. The next
cooperative poll unwinds the network call stack, preserves the old document,
focuses the address field, and emits
`SURA_OS_BROWSER_NAV_FETCH_CANCEL_REQUESTED` followed by
`SURA_OS_BROWSER_NAV_FETCH_CANCELLED_OK`. The full QEMU gate cancels one live
fetch and then completes a second navigation to the same HTTPS site. Because
the HTTP/TLS/DOM buffers are shared, other nested Browser address edits,
content clicks, and second requests remain blocked until the current fetch
returns or completes cancellation.
The seven workers are persistent scheduled
`UserProcess` instances in one `UserProcessScheduler`; the kernel dispatcher
wakes one requested worker and normally waits for it to block again. They are
built-in code mappings rather than ELF-loaded applications.

## Kernel intrinsics

Raw memory:

- `mem.read8/16/32/64(address)`
- `mem.write8/16/32/64(address, value)`

Port I/O:

- `io.in8/16/32(port)`
- `io.out8/16/32(port, value)`

CPU control:

- `cpu.halt()`, `cpu.pause()`
- `cpu.disable_interrupts()`, `cpu.enable_interrupts()`
- `cpu.read_cr0/cr2/cr3/cr4()`
- `cpu.write_cr0/cr3/cr4(value)`
- `cpu.read_flags()`, `cpu.write_flags(value)`
- `cpu.read_msr(index)`, `cpu.write_msr(index, value)`
- `cpu.load_gdt/load_idt(descriptor_address)`
- `cpu.load_gdt/load_idt(table, byte_size)`
- `cpu.reload_segments(code_selector, data_selector)`
- `cpu.load_task_register(selector)`, `cpu.read_task_register()`
- `cpu.invalidate_page(address)`
- `cpu.cpuid_eax/ebx/ecx/edx(leaf, subleaf?)`
- `cpu.rdtsc()`, `cpu.rdtscp()`, `cpu.rdrand()`
- `cpu.xgetbv(index)`, `cpu.xsetbv(index, value)`
- `cpu.swapgs()`, `cpu.stac()`, `cpu.clac()`, `cpu.wbinvd()`
- `cpu.fninit()`, `cpu.clts()`
- `cpu.fxsave/fxrstor(area)`, `cpu.xsave/xrstor(area, state_mask)`

Atomic operations:

- `atomic.load8/16/32/64(address)`
- `atomic.store8/16/32/64(address, value)`
- `atomic.exchange8/16/32/64(address, value)`
- `atomic.compare_exchange8/16/32/64(address, expected, desired)`
- `atomic.fetch_add8/16/32/64(address, value)`
- `atomic.fetch_sub8/16/32/64(address, value)`
- `atomic.fence()`, `atomic.load_fence()`, `atomic.store_fence()`

The unsuffixed atomic names operate on 64-bit values. Exchange,
compare-exchange, fetch-add, and fetch-sub return the previous memory value.
Stores use an implicitly locked `xchg`; read-modify-write operations use locked
x86-64 instructions. These semantics are for the current x86-64 target and
will require an explicit portable memory-order model before an ARM64 target.

These operations are privileged. They are only recognized by the
freestanding target and are not added to normal hosted Sura execution.

## Interrupt functions

A top-level function can select one of two x86-64 interrupt ABIs after its
return type:

```sura
func timer(frame: ptr[InterruptFrame]) interrupt do
  atomic.fetch_add64(addr_of(timer_ticks), 1)
  return
end

func page_fault(frame: ptr[InterruptFrame]) interrupt_error do
  global last_fault_error
  last_fault_error is frame.error_code
  return
end
```

`interrupt` is for vectors whose hardware frame has no error code.
`interrupt_error` is for vectors 8, 10-14, 17, 21, 29, and 30. The compiler
disables maskable interrupts, checks the saved CS privilege level without
clobbering a general-purpose register, conditionally executes `swapgs` for a
ring-3 entry, and serializes that decision with `lfence`. It then adds a
synthetic zero for the first form, saves all general-purpose registers, clears
the direction flag for calls, and aligns the handler stack. The return path
restores the registers, discards the real or synthetic error code, conditionally
returns to the user GS base, and emits `iretq`.
Interrupt functions require exactly one typed pointer parameter and cannot be
called as normal functions.

The saved frame begins at the pointer passed to the handler:

| Offset | Field |
| ---: | --- |
| 0..56 | `r15, r14, r13, r12, r11, r10, r9, r8` |
| 64..112 | `rdi, rsi, rbp, rbx, rdx, rcx, rax` |
| 120 | normalized error code |
| 128, 136, 144 | hardware `rip`, `cs`, `rflags` |
| 152, 160 | hardware old `rsp`, `ss` only when privilege level changed |

For a same-privilege interrupt, `frame + 152` is the interrupted stack
address; offsets 152 and 160 are not hardware fields that may be read as
stored values. A ring transition supplies actual old `rsp` and `ss` values.

Install a gate with:

```sura
cpu.idt_set_gate(idt, 32, addr_of(timer), 8, 0, 142)
cpu.idt_set_gate(idt, 14, addr_of(page_fault), 8, 0, 142)
```

The arguments are IDT base, vector, handler, code selector, IST index, and
attributes. Vector, selector, IST, and attributes are compile-time integers.
The compiler requires a direct `addr_of(interrupt_function)` and checks whether
the selected vector requires an error-code ABI. This helper writes one 16-byte
gate; it does not configure a TSS/IST stack, load IDTR, enable interrupts, or
send an APIC end-of-interrupt.

The current wrapper saves integer registers only. Kernel code must add an
FPU/SIMD state policy before handlers use floating-point or vector operations.
The saved-CS `swapgs` path covers ordinary ring-3 interrupt and exception
entry. It is not a paranoid NMI/MCE entry path: a non-maskable event can arrive
after a user-to-kernel transition but before the outer wrapper exchanges GS,
while its own saved CS reports ring 0. Do not expose those vectors through the
generic wrapper until the kernel adds a separate NMI-safe entry contract. The
dedicated fast-syscall helper described below has its own checked `swapgs` and
per-CPU stack contract.

## TSS, IST, and extended CPU state

The backend can build and activate the x86-64 structures needed for
per-CPU kernel and interrupt stacks:

```sura
gdt is static.zero(128, 16)
tss is static.zero(104, 16)
kernel_stack is static.zero(16384, 16)
fault_stack is static.zero(16384, 16)

cpu.tss_set_rsp(tss, 0, ptr.add(kernel_stack, 16384))
cpu.tss_set_ist(tss, 1, ptr.add(fault_stack, 16384))
cpu.tss_set_iomap(tss, 104)
cpu.gdt_set_tss(gdt, 3, tss, 103)
cpu.load_gdt(gdt, 128)
cpu.reload_segments(8, 16)
cpu.load_task_register(24)
```

`cpu.gdt_set_tss` writes a present, ring-0, available 64-bit TSS descriptor
occupying two GDT entries. Its index and limit are compile-time integers. When
the GDT and TSS are named static objects, the compiler rejects descriptors
that exceed either object. `cpu.tss_set_rsp` accepts privilege level 0-2;
`cpu.tss_set_ist` accepts IST index 1-7. A 104-byte TSS with I/O-map offset 104
contains no usable I/O permission bitmap.

The two-argument `load_gdt/load_idt` forms construct the 10-byte pseudo
descriptor on the current stack and execute `lgdt/lidt`. The one-argument
forms remain available when code has already built a pseudo descriptor.
`reload_segments` performs a far return to reload `CS`, then loads `DS`, `ES`,
and `SS`. It deliberately does not modify `FS` or `GS`.

FPU/SIMD state policy remains explicit:

```sura
cpu.fninit()
cpu.fxsave(fx_area)
cpu.fxrstor(fx_area)
cpu.xsetbv(0, enabled_xcr0_bits)
cpu.xsave(xsave_area, enabled_xcr0_bits)
cpu.xrstor(xsave_area, enabled_xcr0_bits)
```

Kernel code must check CPUID support, configure CR0/CR4 and XCR0 in the
architecturally required order, obtain the required XSAVE area size from
CPUID leaf `0xD`, and provide correctly aligned storage before using these
instructions. `stac/clac` require SMAP support. Direct `cpu.swapgs()` remains a
raw primitive; code outside compiler-generated interrupt and fast-syscall
entry must still enforce when it is safe to exchange GS bases.

## Per-CPU storage and local APIC primitives

GS-relative fixed-width access is available for CPU-local state:

```sura
percpu.set_base(cpu_state)
percpu.set_kernel_base(cpu_state)
percpu.write64(0, cpu_index)
index: u64 is percpu.read64(0)
field_address: ptr is percpu.address(0)
```

- `percpu.base()`, `percpu.kernel_base()`
- `percpu.set_base(address)`, `percpu.set_kernel_base(address)`
- `percpu.address(byte_offset)`
- `percpu.read8/16/32/64(byte_offset)`
- `percpu.write8/16/32/64(byte_offset, value)`

The base helpers access `IA32_GS_BASE` and `IA32_KERNEL_GS_BASE`. GS-relative
reads and writes do not perform bounds checks; the kernel owns each allocation
and must keep it alive while that CPU can access it. These are primitives for
a later per-CPU allocator, not an allocator themselves.

The `apic` intrinsics select xAPIC MMIO or x2APIC MSRs from
`IA32_APIC_BASE` at run time:

- `apic.mode()` returns 0 for disabled, 1 for xAPIC, or 2 for x2APIC
- `apic.base()`, `apic.current_id()`
- `apic.read(offset)`, `apic.write(offset, value)`
- `apic.icr_busy()`, `apic.eoi()`
- `apic.send_ipi(destination, command)`
- `apic.send_init(destination)`
- `apic.send_startup(destination, trampoline_physical_address)`

APIC register offsets are compile-time integers from `0x20` through `0x3F0`
and must be 16-byte aligned. `send_startup` derives the SIPI vector from a
4-KiB-aligned physical address below 1 MiB; constant addresses are checked by
the compiler. A run-time address remains the caller's responsibility.
xAPIC destinations are limited to the low 8-bit APIC ID, while x2APIC accepts
a 32-bit destination.

These helpers send commands but do not discover processors or calibrate a
timer. Startup code must discover enabled processors from ACPI MADT and
exclude the BSP.

## Application-processor startup

`stdlib/freestanding/ap_startup.sura` provides a checked 230-byte x86-64 AP
trampoline and a bounded INIT/SIPI sequence. The trampoline starts in 16-bit
real mode, enables PAE and `EFER.LME`, installs its small GDT, loads a
caller-supplied PML4, enters long mode, switches to a 16-byte-aligned stack,
publishes an atomic ready flag, and calls a normal one-argument Sura function.

```sura
import "../../stdlib/freestanding/ap_startup.sura"

config: ptr[ApStartupConfig] is ap_config
config.destination is mapped_low_page
config.physical_address is 32768
config.pml4_physical is kernel_pml4_physical
config.stack_top is ap_stack_top
config.entry is addr_of(secondary_processor_main)
config.argument is ap_index
config.ready_flag is ap_ready_flag

started: bool is ap_start(config, apic_id, tsc_ticks_per_us, 100000, true)
```

The trampoline physical address must be a 4-KiB-aligned page from `0x1000`
through `0xFF000`. `config.destination` must be a writable virtual alias of
that same physical page. The caller-supplied PML4 must identity-map the
trampoline and map the stack, entry function, ready flag, and data used by the
secondary entry. `pml4_physical` must fit in 32 bits because the trampoline
loads CR3 before long mode. `ap_start` waits for ICR idle, sends INIT, waits
10 ms, sends SIPI, waits 200 µs, optionally sends a second SIPI, and polls the
ready flag with caller-supplied TSC calibration and bounded timeouts.

The library does not calibrate TSC, allocate the low page or per-AP state,
choose processors, install a per-AP GDT/TSS/IDT, initialize FPU/SIMD state,
join the AP to a scheduler, recover a failed AP, support CPU hotplug, or tear
down temporary identity mappings. It targets modern integrated local APIC
startup and does not implement the older discrete 82489DX INIT-deassert
sequence. `examples/os/ap_startup_features.sura` is compile-only.
`tools/sura_ap_startup_smoke.ps1` verifies the exact assembled 230-byte
template inside the generated EFI image; actual AP execution still needs QEMU
or hardware verification.

## x86-64 paging primitives

The `paging` intrinsics build and inspect four-level, 4-KiB x86-64 page-table
entries without depending on the hosted runtime:

```sura
virtual_address: u64 is u64("0xffff800000001000")
pml4_slot: u64 is paging.pml4_index(virtual_address)
pdpt_slot: u64 is paging.pdpt_index(virtual_address)
pd_slot: u64 is paging.pd_index(virtual_address)
pt_slot: u64 is paging.pt_index(virtual_address)

entry: u64 is paging.entry(physical_page, 3) # present | writable
paging.write(page_table, pt_slot, entry)
paging.invalidate(virtual_address)
```

- `paging.pml4_index/pdpt_index/pd_index/pt_index(address)`
- `paging.offset(address)`, `paging.is_canonical48(address)`
- `paging.entry(physical_address, flags)`
- `paging.entry_address(entry)`, `paging.entry_flags(entry)`
- `paging.present(entry)`, `paging.large(entry)`
- `paging.read(table, index)`, `paging.write(table, index, entry)`
- `paging.map(table, index, physical_address, flags)`
- `paging.clear(table, index)`
- `paging.root()`, `paging.activate(root)`
- `paging.invalidate(address)`, `paging.flush()`

Constant page-table indexes outside 0..511 and constant unaligned physical
addresses are rejected. A run-time index is masked to nine bits. Entry and
root addresses are masked to a 52-bit, 4-KiB-aligned physical address;
run-time callers must check alignment before calling if truncation should be
an error. `paging.entry` preserves the low 12 flag bits and high flag/software
bits 52..63, including NX. The kernel must enable EFER.NXE before using NX.

`paging.activate`, `paging.invalidate`, and `paging.flush` are privileged.
`paging.activate` deliberately accepts only a root address and clears PCID and
CR3 no-flush bits. Kernels that implement PCID policy can use
`cpu.write_cr3(value)` directly. These primitives do not allocate intermediate
tables, walk an arbitrary address space, perform TLB shootdowns on other CPUs,
or choose a kernel/user virtual-memory layout.

## Freestanding memory libraries

The source libraries under `stdlib/freestanding` build on the paging and
memory intrinsics without adding a hosted runtime:

```sura
import "../../stdlib/freestanding/physical_memory.sura"
import "../../stdlib/freestanding/virtual_memory.sura"
```

`physical_memory.sura` defines `PhysicalAllocator` and a bitmap allocator:

- `pmem_reset(state, bitmap, bitmap_bytes, base, page_count)`
- `pmem_init_from_uefi(state, bitmap, bitmap_bytes, map, map_size, descriptor_size)`
- `pmem_reserve_range(state, start_address, page_count)`
- `pmem_release_range(state, start_address, page_count)`
- `pmem_alloc(state)`, `pmem_alloc_contiguous(state, count, alignment_pages)`
- `pmem_free(state, address)`, `pmem_is_used(state, address)`
- `pmem_available_pages(state)`

A set bitmap bit means allocated or reserved. Initialization starts with every
page reserved and releases only UEFI `EfiConventionalMemory` descriptors
(type 7) covered by the caller-provided bitmap. Page zero is never released
when the allocator base is zero, because address zero is the allocation
failure result. The library clamps descriptors beyond bitmap coverage instead
of pretending that untracked memory is safe.

The bitmap allocator is not internally synchronized. SMP code must serialize
mutations or give each CPU disjoint ownership. It also does not yet implement
NUMA zones, DMA address classes, or a policy for reclaiming other UEFI memory
types after their firmware lifetime has ended.

The final memory map must be acquired after all boot-service allocations.
Call `uefi.exit_boot_services(map_key)` immediately after that successful map.
Do not allocate directly from conventional memory while firmware boot services
still own it. If `ExitBootServices` rejects a stale key, acquire a fresh map
and retry. After it succeeds, do not call firmware console or other boot
services and do not return to the firmware entry caller.

`virtual_memory.sura` provides conflict-checked four-level helpers:

- `vmem_walk_pte(root, virtual_address)`
- `vmem_translate(root, virtual_address)` with 4-KiB, 2-MiB, and 1-GiB leaves
- `vmem_link_4k(root, virtual_address, pdpt, pd, pt, flags)`
- `vmem_map_4k`, `vmem_unmap_4k`, `vmem_protect_4k`
- `vmem_mapping_flags`, `vmem_is_mapped`

`vmem_link_4k` requires caller-owned, 4-KiB-aligned, identity-accessible table
pages and refuses to replace an existing table with a different one.
`vmem_map_4k` refuses to overwrite a present leaf. Mapping changes invalidate
the local CPU entry only; an SMP kernel must send a TLB-shootdown IPI and wait
for acknowledgements before reclaiming a page visible to another CPU.

`examples/os/memory_kernel.sura` shows the complete ordering: memory-map retry,
`ExitBootServices`, allocator initialization, allocation, table linking,
mapping, translation verification, unmapping, freeing, and a non-returning
post-firmware halt path.

## Process address spaces and static ELF64 loading

`stdlib/freestanding/process_memory.sura` builds isolated lower-half user
address spaces on the physical and virtual memory libraries. The kernel
supplies a `ProcessSpaceBuffers` object containing fixed-capacity arrays for
owned physical pages and `ProcessMapping` records. `process_space_init`
allocates a new PML4, leaves its lower half empty, and copies PML4 entries
256..511 from the supplied kernel root with the U/S bit cleared. Passing zero
as the kernel root creates an address space without shared kernel mappings,
which is useful for the executable feature test but is not sufficient for a
real ring-3 transition.

The public operations are:

- `process_space_init(state, allocator, kernel_root, buffers)`
- `process_space_map_page(state, virtual_page, writable, executable)`
- `process_space_protect_page` and `process_space_unmap_page`
- `process_space_map_stack(state, top, pages, guard_pages, result)`
- `process_copy_to_user` and `process_copy_from_user`
- `process_space_activate`, `process_space_rollback`, and
  `process_space_destroy`
- `process_nx_supported()` and `process_enable_nx()`

Mapping accepts nonzero 4-KiB-aligned addresses below
`0x0000800000000000`. It automatically allocates the missing PDPT, PD, and PT
pages, refuses large or non-present nonzero conflicts, creates U/S mappings,
and rejects writable-plus-executable leaves. Non-executable pages carry NX;
the kernel must call `process_enable_nx` before activating an address space
that contains them. Unmapping releases the data page and prunes empty
intermediate tables. Mapping changes invalidate only the local CPU.

The copy helpers validate the complete user range and every recorded PTE
before copying any byte. They copy through the identity-accessible physical
page recorded by the address space instead of dereferencing an untrusted user
virtual address in kernel mode. `process_copy_to_user` also requires writable
PTEs. These checks are stable only while the caller prevents concurrent
mapping changes. They do not replace a process fault policy, ownership checks
for higher-level kernel objects, or remote TLB shootdown.

`process_space_map_stack` maps writable NX stack pages, leaves the requested
guard pages unmapped, and returns a stack pointer congruent to 8 modulo 16 for
the current Sura user-entry ABI. `process_space_destroy` refuses to free the
currently active CR3 and preflights every owned page before releasing the
address space.

`stdlib/freestanding/elf64.sura` loads a deliberately bounded static
x86-64 ELF64 subset into a `ProcessAddressSpace`. It accepts little-endian
System V or Linux `ET_EXEC` images and checks:

- ELF64 magic, class, data encoding, ABI version, x86-64 machine, header
  sizes, zero x86-64 flags, and at most 128 program headers
- every program-header table and file range before pointer arithmetic
- sorted readable `PT_LOAD` segments, `p_filesz <= p_memsz`, power-of-two
  alignment, and file/virtual page-offset congruence
- nonzero lower-half memory ranges, no page-overlapping load segments, no
  W+X segment, and an entry point inside an executable load segment
- rejection of `PT_INTERP`, `PT_DYNAMIC`, `PT_SHLIB`, `PT_TLS`, and an
  executable `PT_GNU_STACK`

The loader first validates the complete image, maps zeroed writable NX pages,
copies file bytes, leaves the `p_memsz - p_filesz` tail zeroed, and then applies
the final read/write/execute permissions. A failure rolls back only mappings
created by that load attempt. Two caller-owned `Elf64Segment` scratch objects
make the parser reentrant when each concurrent load has separate buffers.

`examples/os/process_elf_features.sura` constructs a static ELF64 image,
checks malformed-size and W+X rejection, loads code and zero-filled memory,
checks safe copy permissions, creates a guarded user stack, rejects a W+X
mapping, destroys the address space, and verifies that all physical pages were
returned. The UEFI smoke gate currently compiles this executable self-check
and verifies its image; QEMU/OVMF or hardware execution has not yet been
recorded.

This subset does not implement `ET_DYN`, ASLR, relocations, an ELF
interpreter, dynamic linking, TLS, demand paging, copy-on-write, shared memory,
memory-mapped files, PCID, KPTI, or remote TLB shootdown. The separate
`user_process.sura` layer described below adds a bounded single-CPU
fault/exit/preemption policy; it does not change the ELF loader's format
limits.

## Cooperative context primitives

The backend emits a small Win64-compatible integer context switch only when
the source uses the `context` module:

- `context.frame_size()` returns 72 bytes
- `context.init(stack_top, entry, argument, exit_handler)` returns initial RSP
- `context.switch(saved_rsp_address, next_rsp)` saves the current RSP and resumes another context

`context.init` aligns the supplied stack top to 16 bytes and builds a frame
for `r15, r14, r13, r12, rsi, rdi, rbp, rbx` plus a bootstrap return address.
The bootstrap calls `entry(argument)`. If `entry` returns and `exit_handler`
is nonzero, it calls `exit_handler(result)`; returning from the exit handler
or omitting it ends in `cli; hlt`.

The switch follows call-preserved register rules, so volatile general
registers are intentionally not preserved. It does not save RFLAGS,
FPU/SIMD state, CR3, FS/GS bases, debug registers, or interrupt state. A
scheduler must handle those policies separately, keep interrupts/preemption
safe around queue mutations, and never reclaim a running task stack.

## Cooperative scheduler library

`stdlib/freestanding/scheduler.sura` builds a single-CPU cooperative scheduler
on the context primitives. It provides:

- `scheduler_init` and `scheduler_create`
- round-robin `scheduler_yield`
- explicit `scheduler_tick` and tick-based `scheduler_sleep`
- `scheduler_block_current` and `scheduler_wake`
- `scheduler_join`, result lookup, and `scheduler_reap`

Task state and stacks are supplied by the caller. The initial context occupies
slot zero, created tasks use at least 4096-byte, 16-byte-aligned stacks, and a
finished stack remains owned by the caller until the task is reaped.
`examples/os/scheduler_features.sura` emits the creation, switching, joining,
and reaping paths inside a non-executing feature block. It deliberately does
not exit UEFI boot services or start an OS.

This scheduler is deliberately cooperative and single-CPU. It does not
preempt from an interrupt frame, synchronize queues between processors, save
FPU/SIMD state, or program a hardware timer. A kernel using sleep must call
`scheduler_tick` from its chosen timer policy, and queue operations must not
race an interrupt or another processor.

## Kernel preemption and local-APIC timers

The freestanding backend can create and validate the 152-byte same-privilege
interrupt frame used to start or resume a ring-0 task:

- `preempt.frame_size()`
- `preempt.init(stack_top, entry, argument, exit_handler, code_selector)`
- `preempt.frame_valid(frame)`
- `preempt.resume(frame)`
- `interrupt.invoke(vector)`

`preempt.init` creates the same integer-register, normalized-error,
RIP/CS/RFLAGS layout produced by an `interrupt` function, with a bootstrap
that calls `entry(argument)` and then `exit_handler(result)`. It validates
canonical addresses, a 16-byte-aligned stack top, and a nonzero ring-0 code
selector. A constant invalid selector is rejected during compilation; a
run-time selector makes `preempt.init` return zero.

`preempt.resume` is accepted only inside an `interrupt` or `interrupt_error`
function. It validates the frame, disables maskable interrupts for the final
switch, restores all integer registers, discards the normalized error code,
and executes `iretq`. A valid resume does not return. The current validator
accepts same-privilege ring-0 frames only; it deliberately rejects an
interrupt frame captured from ring 3 because user-entry `swapgs`, address
space, and process-state policy are not part of this scheduler.

`stdlib/freestanding/preempt.sura` builds a single-CPU round-robin scheduler on
these intrinsics. It supports task creation, timer-driven selection, voluntary
yield through reserved vector 129, sleep, block, wake, join, exit, and reap.
Slot zero represents the boot/idle task and cannot be slept or blocked, so
there is always a ring-0 fallback. The timer handler acknowledges the local
APIC before switching frames. The caller must install vector 129 and the
chosen timer vector as ring-0 interrupt gates.

`stdlib/freestanding/timer.sura` provides:

- local-APIC one-shot and periodic initial-count programming
- mask, unmask, stop, and current-count access
- bounded PIT channel-2 calibration
- conversion from a measured PIT interval to a requested timer frequency
- CPUID-checked TSC-deadline programming and cancellation

The calibration loop has a caller-supplied bound and restores port `0x61`.
The kernel still owns vector allocation, IDT installation, timer-frequency
policy, per-CPU programming, and interrupt-controller setup.

`examples/os/preemptive_timer_features.sura` compiles the scheduler, PIT/APIC
timer paths, checked frame resume, and voluntary software interrupt without
executing them as a UEFI program. The generated machine-code paths are covered
by `tools/sura_uefi_target_smoke.ps1`, but an actual preemptive switch has not
yet been executed in QEMU or on hardware. This foundation does not save
FPU/SIMD, CR3, FS/GS bases, debug registers, or process state; it has no
priority policy, SMP queue locking, load balancing, or user-mode preemption.

## Indirect calls, ring 3, and syscalls

`call.indirect(function, argument...)` performs a Win64-compatible indirect
call to a function address with at most five integer or pointer arguments.
The target must follow the same freestanding Sura calling convention.

`syscall.invoke(vector, number, argument...)` emits `int vector`. The vector
must be a compile-time integer from 32 through 255. The syscall number is
placed in RAX and up to five arguments use RDI, RSI, RDX, R10, and R8. This is
a software-interrupt compatibility ABI.

`stdlib/freestanding/syscall.sura` provides a fixed-size handler table and the
`software_syscall_dispatch` interrupt handler. The dispatcher returns its
result through the saved RAX field. The caller must install a matching IDT
gate and owns every security-sensitive policy: CPL/DPL setup, user-pointer
validation, copy-in/copy-out, per-process permissions, synchronization,
address-space selection, and fault recovery. The compile-only
`examples/os/syscall_features.sura` emits both sides without entering user
mode or starting an OS.

The x86-64 fast path uses the same number and argument registers:

```sura
percpu.set_base(fast_syscall_cpu_state)
percpu.set_kernel_base(user_gs_state)
syscall.fast_configure(addr_of(fast_syscall_dispatch_active), addr_of(fast_syscall_bad_return), 8, 35, 292608, 0, 8)

result: u64 is syscall.fast(7, argument, 2, 3, 4, 5)
```

`syscall.fast_configure(dispatch, bad_return, kernel_cs, user_cs, flags_mask,
kernel_rsp_offset, user_rsp_offset)` may appear exactly once. It enables
`EFER.SCE` and configures `IA32_STAR`, `IA32_LSTAR`, and `IA32_FMASK`.
Selectors, the mask, and the two GS offsets are compile-time values. The
compiler requires ring-0 kernel CS, ring-3 user CS, distinct aligned GS
offsets, and an FMASK that clears TF, IF, DF, IOPL, NT, and AC on entry.
The user SS used by `SYSRETQ` is `user_cs - 8`.

The generated entry helper executes `swapgs`, saves user RSP through GS,
loads the per-CPU kernel RSP, creates a `FastSyscallFrame`, and calls the
configured dispatcher. Before `SYSRETQ`, it requires nonzero lower-half
canonical user RIP and RSP, sanitizes RFLAGS, restores integer registers and
the user stack, and executes `swapgs` again. An invalid return calls
`bad_return(frame)`; that function must terminate or schedule away the current
process and must not return. The library's default implementation disables
interrupts and halts, so a real kernel should replace it.

The saved fast frame is:

| Offset | Field |
| ---: | --- |
| 0..112 | `r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax` |
| 120 | user `rsp` |

Ring-3 entry is explicit:

```sura
if user.is_address(user_entry) and user.is_address(user_stack_top) then
  entered: bool is user.enter(user_entry, user_stack_top, argument, 35, 27)
end
```

`user.enter` requires nonzero lower-half canonical entry and stack addresses,
a stack pointer congruent to 8 modulo 16 for the Sura function-entry ABI, and
ring-3 CS/SS selectors. It places the argument in RCX, constructs an IRET
frame with RFLAGS `0x202`, disables maskable interrupts for the final
transition, executes `swapgs`, and enters with `IRETQ`. Success does not
return; `IRETQ` restores the requested user IF state.

Saved ring-3 contexts use a separate 168-byte frame:

- `user.frame_size()` returns 168
- `user.frame_init(kernel_stack_top, entry, user_rsp, argument, 35, 27)`
- `user.frame_valid(frame)`
- `user.resume(frame)`

The first 152 bytes match the normalized interrupt layout. Offsets 152 and 160
hold the required user RSP and SS. `user.frame_init` aligns the kernel-stack
top, reserves and clears the frame, places the argument in saved RCX, and
creates an initial RIP/CS/RFLAGS/RSP/SS state with RFLAGS `0x202`. It requires
lower-half entry and user-stack addresses and a user stack congruent to 8
modulo 16. CS and SS are compile-time, nonzero 16-bit selectors with RPL 3.

`user.frame_valid` checks the frame pointer, lower-half RIP and RSP, ring-3
selectors, required RFLAGS bit 1, and rejects nonzero IOPL, NT, VM, or upper
RFLAGS bits. It assumes the frame points to trusted mapped kernel memory.
`user.resume` is accepted only inside an `interrupt` or `interrupt_error`
function. A valid resume disables maskable interrupts, restores the saved
integer state, executes `swapgs` and `lfence`, and returns with `iretq`; it does
not return to the handler.

`stdlib/freestanding/user_process.sura` combines these frames with
`ProcessAddressSpace` in a fixed-capacity, single-CPU lifecycle:

- checked process creation from an already loaded address space
- round-robin timer selection and DPL-3 voluntary yield on vector 130
- CR3 activation, per-process TSS RSP0, and user GS-base switching
- optional per-process kernel-owned event queues
- event-queue wait with a saved IRET frame and atomic wake-on-post
- blocked/wake counters and runnable-count invariant checks
- non-returning process exit
- ring-3 page-fault termination with saved CR2 and hardware error code
- exit/fault status lookup and address-space destruction during reap
- a ring-0 idle frame when no process is runnable

`stdlib/freestanding/ipc.sura` supplies the event transport used by that
process boundary. It implements caller-owned fixed-capacity FIFO queues,
nonzero sequence numbers, full-queue rejection with a saturating dropped
counter, kernel-authenticated endpoint delivery, and receiver ownership
checks. `tools/sura_ipc_qemu_gate.ps1` executes queue saturation, sender/target assignment,
FIFO receive, and ownership rejection before emitting `SURA_IPC_QUEUE_OK`.
The queue is intentionally non-blocking and the kernel must serialize queue
operations on SMP.

`stdlib/freestanding/process_syscall.sura` adds the process-aware system-call
boundary:

- syscall 0 returns the authenticated current process ID
- syscall 1 copies an `IpcMessage` from checked user memory, replaces forged
  source and target fields, and performs non-blocking delivery
- syscall 2 preflights a writable user destination, peeks the next event,
  copies it to user memory, and only then consumes it
- syscall 3 yields using the saved software-interrupt IRET frame
- syscall 4 marks the current process exited and schedules away without
  returning to that process
- syscall 5 returns immediately when an event is queued, or blocks the current
  process on an empty queue until a later post wakes it

Calls 0 through 2 can use the generic table or fast-SYSCALL dispatcher. Yield,
exit, and wait require `process_software_syscall_dispatch` on a DPL-3 interrupt
gate because a fast-SYSCALL frame cannot currently be suspended and resumed.
`tools/sura_process_syscall_qemu_gate.ps1` executes a real checked
`ProcessAddressSpace`, forged-identity rejection, process-to-process queue
delivery, safe copy-out, empty-queue result, and invalid-address rejection,
then emits `SURA_PROCESS_SYSCALL_OK`.

`tools/sura_process_wait_qemu_gate.ps1` executes the blocking path between two
real CPL-3 processes with separate CR3 roots. Process A enters syscall 5 with
an empty event queue and becomes blocked. Process B copies an `IpcMessage` from
its checked user mapping, sends it to A, atomically wakes A, and exits. A then
resumes after the suspended wait frame, receives the event into its own user
mapping, and exits. The saved Ring-0 frame resumes only after both process
lifecycles complete. The gate emits:

- `SURA_PROCESS_WAIT_BLOCK_OK`
- `SURA_PROCESS_WAIT_WAKE_OK`
- `SURA_PROCESS_WAIT_RECEIVE_OK`
- `SURA_PROCESS_WAIT_EXIT_OK`
- `SURA_PROCESS_WAIT_CR3_OK`

`tools/sura_user_process_qemu_gate.ps1` executes the scheduler itself after
`ExitBootServices`. Process A reports once and then spins forever without
yielding. A periodic local-APIC interrupt must preempt it so process B can run
under a different CR3, send an IPC event, and deliberately fault on address
zero. The gate succeeds only after the kernel isolates B's page fault, resumes
A, receives the authenticated event, observes both distinct process roots, and
handles another timer interrupt. It emits:

- `SURA_USER_PROCESS_PREEMPT_OK`
- `SURA_USER_PROCESS_IPC_OK`
- `SURA_USER_PROCESS_FAULT_ISOLATED`
- `SURA_USER_PROCESS_CR3_ISOLATED`

`user_process_select_kernel` and
`user_process_kernel_slice_timer_interrupt` provide the opposite scheduling
direction needed by a desktop kernel loop: a timer saves a still-runnable user
frame and resumes the saved Ring-0 service frame. The kernel can poll and route
input, then invoke vector 130 to resume the next runnable user process. A
separate last-user cursor preserves round-robin order across those kernel
slices. `tools/sura_user_process_kernel_slice_qemu_gate.ps1` executes a CPL-3
program that spins forever without yielding, requires two timer returns to the
same Ring-0 input continuation, and reschedules the process between them. It
emits:

- `SURA_USER_PROCESS_KERNEL_SLICE_OK`
- `SURA_USER_PROCESS_INPUT_PROGRESS_OK`
- `SURA_USER_PROCESS_RESUME_OK`
- `SURA_USER_PROCESS_TERMINATE_OK`

After the second timer return, the gate calls
`user_process_terminate(..., 137)` while the non-yielding process is no longer
the active CPU context, verifies the runnable count and exit code, and reaps
its address space.

The calculator worker in `os/user_calculator.sura` has a backward-compatible
persistent entry mode. A plain mailbox argument retains the current desktop's
single-request vector-128 return. Setting argument bit 0 makes the same copied
position-independent program mask the flag, wait on syscall 5, receive into a
separate writable event page through syscall 2, process its mailbox, and wait
again without recreating its process, stack, or address space.
`tools/sura_persistent_calculator_qemu_gate.ps1` executes that actual
calculator worker, sends four kernel events for `2 + 3 =`, and requires result
5 from one process after five separate execution slices. It emits:

- `SURA_PERSISTENT_CALCULATOR_READY`
- `SURA_PERSISTENT_CALCULATOR_EVENTS_OK`
- `SURA_PERSISTENT_CALCULATOR_RESULT_OK`
- `SURA_PERSISTENT_CALCULATOR_SAME_PROCESS_OK`
- `SURA_PERSISTENT_CALCULATOR_CR3_OK`

Calculator, Text Editor, File Explorer, Terminal, System Information, and
Window Server all
expose the same persistent entry contract. `OsUserWorkerConfig` accepts a
page-aligned `event_address`; the worker host maps and validates that page
writable and NX, separately from the mailbox. The graphical boot maps those
six event pages and selects persistent mode by setting argument bit 0.

`tools/sura_persistent_desktop_apps_qemu_gate.ps1` copies the six actual OS
worker programs into six independent `ProcessAddressSpace` roots, supplies
six event queues and kernel stacks, and starts them once in a shared
`UserProcessScheduler`. All six first block on empty queues. The kernel then
wakes each process separately and verifies the real mailbox result:

- Calculator accepts `7`
- Text Editor inserts `abc`
- File Explorer selects entry 2
- Terminal appends `h`
- System Information derives memory and pixel totals
- Window Server composes two app surfaces into a damaged output region

Every worker must return to blocked state with the same process ID, CR3, saved
frame, and kernel stack. The gate emits:

- `SURA_PERSISTENT_DESKTOP_APPS_READY`
- `SURA_PERSISTENT_DESKTOP_CALCULATOR_OK`
- `SURA_PERSISTENT_DESKTOP_EDITOR_OK`
- `SURA_PERSISTENT_DESKTOP_FILES_OK`
- `SURA_PERSISTENT_DESKTOP_TERMINAL_OK`
- `SURA_PERSISTENT_DESKTOP_SYSTEM_OK`
- `SURA_PERSISTENT_DESKTOP_WINDOW_SERVER_OK`
- `SURA_PERSISTENT_DESKTOP_SAME_PROCESS_OK`
- `SURA_PERSISTENT_DESKTOP_CR3_OK`

The graphical desktop now uses the same model. It creates seven processes
once, installs the process syscall, page-fault, yield, and APIC kernel-slice
gates, and primes every worker into blocked state. Kernel code can post events
to more than one blocked worker before entering the scheduler. Those workers
then run round-robin under separate CR3 roots, while each APIC kernel slice
returns control to the desktop input loop. The graphical regression requires
`SURA_OS_USER_SCHEDULER_READY` and
`SURA_OS_USER_PROCESSES_PERSISTENT_OK` in addition to every app result and CR3
marker. Ordinary UI handlers still wait for their own request to finish, and
the built-in app renderers remain in ring 0, so this is not yet a general
user-facing job API or a complete user-space compositor.

If the installed page-fault handler marks a graphical worker faulted, or a
worker exits, the dispatcher no longer unwinds the kernel shell. It disables
only that worker, closes its matching window, reports
`SURA_OS_USER_PROCESS_ISOLATED`, and keeps the desktop loop running. It then
reaps the failed address space, rebuilds that app's worker and event queue,
creates a replacement with a new process ID and CR3, primes it into its blocked
event wait, and reopens the window when it was previously visible. The common
recovery path applies to all seven graphical workers. The regression-only
`faultbrowser` command faults the blocked Browser worker, requires the generic
fault/isolation/restart markers, then requires
`SURA_OS_BROWSER_PROCESS_ISOLATED_OK` after the replacement worker authorizes a
real request under its new process ID and CR3.

The standalone process gate deliberately executes a page fault in its second
process. The full graphical regression separately sends the `faultapp`
diagnostic after all normal app checks: it changes the blocked Calculator's
saved user RIP to an unmapped address, requires
`SURA_OS_USER_PROCESS_FAULT`, `SURA_OS_USER_PROCESS_ISOLATED`, and
`SURA_OS_USER_PROCESS_RESTARTED`, then sends a real `C` event through the new
Calculator process and requires `SURA_OS_USER_PROCESS_RESTART_EVENT_OK`. It
finally sends `status` through the still-running persistent Terminal and
requires `kernel: ready`.

The same graphical regression then sends `hangapp`. Calculator enters an
intentional non-yielding CPL-3 loop after
`SURA_OS_USER_PROCESS_HANG_STARTED`. QMP injects a mouse movement while the
APIC kernel-slice timer repeatedly returns to the desktop service
continuation. The gate requires `SURA_OS_USER_PROCESS_WATCHDOG`,
`SURA_OS_USER_PROCESS_HANG_INPUT_OK`, and
`SURA_OS_USER_PROCESSES_CONCURRENT_OK`. The kernel also queues a real System
Information request before the hang and requires
`SURA_OS_USER_PROCESS_BACKGROUND_OK` only after that second process has
completed and blocked again. Calculator is then reconstructed and must emit
`SURA_OS_USER_PROCESS_HANG_RECOVERED`. A final Terminal `status` must still
print `kernel: ready`.

Run the complete process-foundation verification with:

```powershell
.\tools\sura_os_foundation_verify.ps1 -Engine .\build\SuraLanguage_user.exe
```

The scheduler requires the standard Sura GDT layout (kernel code 8, user data
27, user code 35). Its metadata, TSS, code, current and target kernel stacks,
and all code used during a switch must live in supervisor-only mappings shared
by every process PML4. Production kernels normally place those mappings in the
higher half; the UEFI execution gate shares its loaded low identity-map slot
with U/S cleared. The kernel must install the timer gate, the vector-130 DPL-3
gate, and the vector-14 error-code gate. Scheduling is suppressed while a
runnable process is executing trusted ring-0 code; exit can schedule away
through a nested ring-0 vector-130 interrupt. A kernel-driven desktop may use
the kernel-slice timer handler instead of the pure user-to-user round-robin
timer handler. The graphical desktop installs that handler on vector 48 and
returns to its saved Ring-0 service continuation when a user slice reaches the
APIC timer boundary. It currently services one pending mouse packet between
long slices; keyboard routing remains deferred until the active desktop request
completes.

The generic handler table still does not infer argument types or validate
arbitrary syscall pointers; the process IPC boundary above performs its own
specific validation. This layer does not save FPU/SIMD or debug state, block
and later resume a fast-syscall frame, provide signals, priorities, SMP
locking, PCID, KPTI, remote TLB shootdown, or the NMI-safe entry path noted
above. `process_copy_to_user` and `process_copy_from_user` remain the required
checked data-transfer primitives. Both
`examples/os/user_mode_features.sura` and
`examples/os/user_process_features.sura` are compile and machine-code feature
tests. `examples/os/ring3_qemu_gate.sura` separately executes one checked
CPL-3 entry, DPL-3 software interrupt return, and fast SYSCALL kernel entry in
QEMU. `examples/os/user_process_qemu_gate.sura` separately executes two
`UserProcessScheduler` processes, APIC timer preemption, IPC delivery, distinct
CR3 roots, and page-fault isolation. The desktop System Information, Terminal,
File Explorer, Text Editor, and Calculator use the same checked IRETQ and
interrupt foundation for fixed mailbox workers and switch to distinct
`ProcessAddressSpace` roots before IRETQ. They return to the kernel root from
the DPL-3 interrupt handler, but do not yet run as scheduled processes or load
ELF applications.

## PCI configuration-space foundation

`stdlib/freestanding/pci.sura` implements legacy PCI configuration mechanism
1 through I/O ports `0xCF8` and `0xCFC`. It provides:

- BDF construction and validation
- 8-, 16-, and 32-bit configuration reads and writes
- device probing and vendor/device or class matching
- bounded capability-list traversal
- BAR type and address decoding
- PCI command-register enable and disable helpers

The search functions account for the multifunction bit on function zero.
`examples/os/pci_features.sura` emits each path in a non-executing feature
block.

The two configuration ports are global shared state, so a kernel must
serialize each complete address/data transaction across interrupts and CPUs.
This legacy library does not itself size BARs, configure MSI/MSI-X, allocate
resources, or provide a device-specific driver. A kernel must also validate
BARs and disable conflicting decode before changing device resources.

`stdlib/freestanding/pcie.sura` adds PCI Express ECAM access. It discovers and
checksum-validates ACPI MCFG, requires enough caller-owned storage for every
allocation record, rejects malformed, overlapping, or overflowing segment/bus
ranges, and computes checked 4-KiB function addresses. It provides aligned
8/16/32-bit access, segment-aware enumeration, standard and extended
capability traversal, BAR decoding, and command-register enablement.

The kernel must keep the firmware ACPI tables mapped while parsing and map
each accepted ECAM physical range as uncached MMIO before configuration
access. Writes still require kernel-level serialization and device-specific
state control. This layer does not size or allocate BARs, configure bridges,
MSI/MSI-X, SR-IOV, ACS/IOMMU policy, or hot-plug.
`examples/os/pcie_features.sura` constructs and validates an MCFG record and
checks an ECAM address. Actual configuration-space MMIO remains unexecuted in
the current gate.

## Block-device foundation

`stdlib/freestanding/block.sura` defines a fixed-width synchronous block-device
contract. A device records its context, logical-sector size and count, buffer
alignment, read-only state, and read/write/flush callbacks. Every public
request checks the device shape, nonzero count, LBA range without wrapping,
transfer-size multiplication, and buffer alignment before making an indirect
call. Callbacks return zero on success; the public helpers return `bool`.

The library includes two adapters:

- a caller-owned RAM disk with bounded byte copies and optional read-only mode
- a UEFI Block I/O adapter with checked native structure offsets, a snapshotted
  media identifier and block size, media-change rejection, and firmware
  read/write/flush calls

```sura
device: ptr[BlockDevice] is block_device
context: ptr[RamBlockContext] is ram_context
if not ram_block_bind(device, context, storage, 32768, 512, 0) then return 1
if not block_write(device, 1, 8, buffer) then return 2
if not block_read(device, 1, 8, buffer) then return 3
```

`examples/os/block_features.sura` contains a RAM-disk write/read/flush
self-check and retains the UEFI adapter path for compilation. The generated
machine code, diagnostic, and UEFI Block I/O GUID are checked by
`tools/sura_uefi_target_smoke.ps1`. This is currently compile and image
verification: the self-check has not yet been executed in QEMU or on hardware.

The UEFI adapter is a boot-stage adapter. Its protocol and media pointers stop
being usable after successful `ExitBootServices`; a kernel must not retain this
device for post-boot I/O. `uefi_block_locate` binds the first matching protocol
only and does not enumerate handles, select a partition, perform asynchronous
I/O, or recover a changed removable medium automatically. Rebinding refreshes
the media snapshot. The caller owns every device, context, and buffer and must
serialize requests.

## Native AHCI SATA and NVMe foundations

`stdlib/freestanding/ahci.sura` provides a polling AHCI 1.x SATA path that can
remain usable after `ExitBootServices`. It includes PCI class discovery,
ABAR extraction and PCI memory/bus-master enablement, BIOS/OS ownership
handoff, HBA reset, implemented-port and active-SATA discovery, command-engine
stop/start, caller-owned command-list/FIS/table programming, ATA IDENTIFY,
48-bit DMA read/write, cache flush, and a `BlockDevice` adapter.

The kernel must map ABAR as uncached MMIO and supply stable, physically
contiguous command and data buffers with both virtual and physical addresses.
The command path uses one command slot and one PRDT entry. Buffers outside the
registered DMA window are copied through that window in bounded,
sector-aligned chunks, which lets SuraFS use its own static bank buffer. A
controller without 64-bit addressing is rejected when any DMA range crosses
4 GiB.

This is a polling foundation, not a complete production storage stack. It
does not implement interrupts, NCQ, multiple outstanding commands, ATAPI,
port multipliers, hot-plug, TRIM, COMRESET/error recovery, power management,
IOMMU mapping. `examples/os/ahci_features.sura` checks command-header,
H2D FIS, and PRDT construction. The graphical QEMU OS additionally discovers
ICH9 AHCI, identifies the second SATA disk, reads and writes the FAT32 and
SuraFS partitions through the staged DMA path, flushes, and remounts the
preserved image. No AHCI command has been tested on physical hardware.

`stdlib/freestanding/nvme.sura` supplies a separate polling NVMe
NVM-command-set path. It discovers the PCI class and BAR, configures a
caller-owned admin queue, identifies the controller and a selected namespace,
creates I/O queue pair 1, builds read/write/flush commands, consumes
phase-tagged completion entries, and exposes the namespace as a `BlockDevice`.
The current PRP builder supports PRP1 plus one direct PRP2 page, so a request
can cover at most two 4-KiB controller pages and may be smaller when it starts
inside a page. Larger block requests and arbitrary filesystem buffers are
staged through the registered DMA window in bounded chunks. Queue depth is
limited to 64, queue memory must fit in one page, the controller must support
a 4-KiB minimum memory page, and formatted LBAs with separate metadata are
rejected.

`tools/sura_nvme_qemu_gate.ps1` attaches a 64-MiB QEMU NVMe namespace,
initializes the admin and I/O queues after `ExitBootServices`, executes
controller and namespace Identify, verifies a known sector, writes and flushes
an 8-KiB PRP1/PRP2 transfer, reads it back through the driver, and then checks
the same 8192 bytes in the host disk image. The complete OS gate
`tools/sura_os_nvme_qemu_gate.ps1` attaches the normal FAT32+SuraFS data image
only as NVMe and requires `SURA_OS_STORAGE_NVME_READY`, settings load, SuraFS
mount, and ACPI shutdown. The desktop selects NVMe first and retains the
executed AHCI path as fallback.

```powershell
.\tools\sura_nvme_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
.\tools\sura_os_nvme_qemu_gate.ps1 `
  -Engine .\build\SuraLanguage_os_next.exe
```

The NVMe path is also synchronous and polling. It does not implement MSI/MSI-X,
multiple I/O queue pairs, concurrent commands, PRP lists, SGLs, namespace-list
selection, controller shutdown, abort/reset recovery, asynchronous events,
IOMMU mapping, or zoned namespaces. `examples/os/nvme_features.sura` checks SQ
entry and cross-page PRP construction. The executed gates prove the QEMU path,
not operation on a physical NVMe controller.

## Native xHCI USB controller foundation

`stdlib/freestanding/xhci.sura` provides a polling xHCI controller foundation
that remains usable after `ExitBootServices`. It discovers PCI class
0x0c/0x03/0x30, extracts and enables the MMIO BAR, parses the capability
registers, performs the legacy BIOS/OS ownership handoff, stops and resets the
controller, and configures the DCBAA, command ring, event ring, ERST, runtime
interrupter registers, and maximum enabled slots.

The current DMA contract uses a caller-owned, physically contiguous,
page-aligned 16-KiB region: one page each for the DCBAA, command ring, event
ring, and ERST. The command and event rings track cycle state, use a Link TRB,
ring doorbell zero, poll Command Completion Events, and update ERDP. Controllers
that require scratchpad buffers are currently rejected.

Each USB device additionally uses a caller-owned, contiguous 24-KiB DMA region
for the output and input contexts, endpoint-0 ring, descriptor buffer,
interrupt-IN ring, and report buffer. The desktop reserves one 64-KiB arena:
16 KiB for the controller plus 24 KiB each for keyboard and mouse. The polling
enumeration path resets a
root port, enables a slot, issues Address Device, reads the Device and
Configuration descriptors through endpoint 0, finds a boot-protocol HID
keyboard or mouse interface, configures its interrupt-IN endpoint, sends
SET_CONFIGURATION and SET_PROTOCOL, and posts one Normal TRB for a report.

`tools/sura_xhci_qemu_gate.ps1` boots
`examples/os/xhci_qemu_gate.sura` with QEMU's `qemu-xhci`, `usb-kbd`, and
`usb-tablet` devices. After `ExitBootServices`, the gate observes both ports,
enumerates the directly attached boot keyboard, and uses QMP to inject an A
key. The driver completes the interrupt-IN transfer and verifies the eight-byte
boot report `00 00 04 00 00 00 00 00` before disabling the slot. This is
executed proof of the PCI/MMIO, reset/run, command/event ring, endpoint-0
control-transfer, Configure Endpoint, and boot-keyboard report paths on that
QEMU device. It has not been tested on physical USB hardware.

`tools/sura_xhci_mouse_qemu_gate.ps1` separately selects the second connected
port, enumerates QEMU `usb-mouse`, configures its boot-mouse interrupt
endpoint, injects movement, and verifies both the raw report and the resulting
`PointerEvent`. The keyboard gate likewise verifies conversion of the injected
A report into `KeyEvent`.

The normal `tools/sura_os_vm.ps1` boot attaches both USB devices. The OS
enumerates both slots, keeps one pending interrupt-IN TRB per device, converts
completed reports through `usb_hid.sura`, and dispatches them through the same
desktop key and pointer handlers used by PS/2. Its non-interactive gate injects
USB Shift and mouse movement and requires `SURA_OS_XHCI_INPUT_READY`,
`SURA_OS_KEYBOARD_OK`, and `SURA_OS_MOUSE_OK`.

The desktop currently assumes the first connected root-port device is a boot
keyboard and the second is a boot mouse. Hubs and route strings, arbitrary
device order, device removal and hot-plug recovery, arbitrary HID report
descriptors, non-boot HID devices, interrupt/MSI operation, isochronous
transfers, power management, physical-hardware proof, and scratchpad
allocation remain to be implemented.

## FAT32 and virtual filesystem foundation

`stdlib/freestanding/gpt.sura` validates a primary or backup GPT header,
including its CRC32, usable-LBA bounds, disk GUID, entry geometry, and the
CRC32 of the complete partition-entry array. It copies even sector-spanning
entries into a caller-owned entry buffer and supports indexed traversal and
type-GUID lookup. `stdlib/freestanding/partition.sura` adds the four legacy
MBR primary entries and a unified EFI System Partition lookup. A valid GPT is
authoritative; the type-0xEF MBR fallback is used only when neither GPT header
can be validated. Extended MBR/EBR chains, hybrid-disk reconciliation, GPT
repair, partition creation, and resizing are not implemented.

`stdlib/freestanding/fat32.sura` mounts a FAT32 volume through a
`BlockDevice`. Mounting checks the boot signature, sector geometry,
power-of-two cluster size, FAT32 cluster-count range, FAT version, root
cluster, arithmetic overflow, and volume bounds before accepting the volume.
It can walk a cluster chain, look up an uppercase space-padded 8.3 name in a
directory, list directory entries, read a complete regular file, and
overwrite an existing regular file without changing its size. Its writable
foundation also updates every FAT copy, allocates and zeroes clusters, frees
bounded cluster chains, creates short-name files and directories, initializes
`.` and `..`, grows or shrinks regular files, performs exact partial
reads/writes, renames same-directory short-name entries, rejects nonempty
directory deletion, and deletes entries and their cluster chains. Extending a
file zeroes every newly visible byte, including growth that stays within the
same allocated cluster count.

The same module reads and writes VFAT long-file-name entry groups. The caller
supplies UTF-8 input/output, 255-unit input UTF-16, 260-unit decoded UTF-16,
and 11-byte alias work buffers through `Fat32LongName`. Creation strictly
decodes UTF-8, rejects FAT-forbidden characters and invalid scalar values,
generates a collision-checked uppercase 8.3 alias, writes LFN entries before
publishing the short entry, and supports names up to 255 UTF-16 code units.
Lookup verifies ordinal order, entry type, zero cluster field, short-alias
checksum, UTF-16 terminator/padding, and surrogate pairs. Listing falls back
to the short alias when an LFN chain is malformed. Long-name deletion marks
the published short entry first and then its preceding LFN slots; rename
publishes a replacement group before retiring the old group.

`tools/sura_fat32_mutation_qemu_gate.ps1` builds and boots a minimum-size
FAT32 RAM disk through the production `BlockDevice` path. It checks both FAT
copies while a file grows to three clusters, writes across sector and cluster
boundaries, shrinks and regrows the file, verifies truncated bytes cannot
reappear, renames the file, creates and removes a nonempty directory tree,
mounts the unchanged disk through fresh `Fat32Volume` objects, reads the
persisted bytes, deletes the file, and confirms its complete chain is free.
The same gate creates `문서/메모.txt`, resolves alias collisions, renames it to
`긴 한글 메모 파일.sura`, remounts and verifies its bytes, rejects deletion of
the nonempty Korean directory, and removes the complete tree. It also creates,
lists, remounts, and deletes a maximum 255-code-unit name whose 20 LFN entries
force directory-cluster expansion. Finally it corrupts an LFN checksum and
requires listing to expose only the valid short alias. Success requires
`SURA_FAT32_REMOUNT_OK`, `SURA_FAT32_LFN_UTF8_OK`,
`SURA_FAT32_LFN_CORRUPT_OK`, and `SURA_FAT32_MUTATION_OK` from the
QEMU-executed image.

The FAT32 library has no Unicode normalization or general Unicode case
folding; exact UTF-16 names are compared with ASCII-only case insensitivity.
It also lacks a general string path parser, cross-directory moves, FAT
mirroring repair, FSInfo free-count updates, timestamps, permissions,
journaling, recovery after an interrupted metadata sequence, and concurrent
access. Allocation scans the FAT from cluster 2 and can leak a cluster after a
failed device write. The graphical Explorer and Text Editor now use the
SuraFS-backed VFS rather than these FAT32 mutation and LFN functions. FAT32
remains mounted for settings, desktop state, and first-run legacy-note
migration. A malformed or cyclic FAT32 cluster chain is bounded by the volume
cluster count and fails.

`stdlib/freestanding/vfs.sura` supplies a fixed-capacity mount table and file
dispatch layer. Paths are caller-owned UTF-8 byte buffers with explicit
lengths. It accepts canonical absolute paths only: embedded NUL, a trailing or
repeated separator, and `.` or `..` segments are rejected rather than
normalized. It rejects duplicate mount points, resolves the longest matching
mount prefix, and dispatches open, read, write, append, seek, resize, flush,
close, stat, file or directory creation, directory listing, rename, removal,
and filesystem sync through checked callbacks. Rename is deliberately
restricted to one mounted filesystem. All mount, filesystem, file, and path
storage is supplied by the kernel; the library allocates nothing and does not
provide internal locking.

`stdlib/freestanding/fat32_vfs.sura` adapts the writable FAT32/LFN operations
to every `VfsFileSystem` callback. It resolves canonical relative UTF-8 paths
component by component, exposes root/file/directory stat and listing, creates
files or directories, opens files with create/truncate/append semantics,
automatically grows files for writes, supports explicit resize and flush,
renames within one directory, and removes regular files or empty directories.
The caller supplies a mounted `Fat32Volume`, one sector buffer, LFN work
buffers, one scratch entry, and a fixed array of `Fat32VfsHandle` records.
Open handles are exclusive per directory entry to prevent stale cluster and
size metadata from a second writer.

The FAT32 mutation QEMU gate mounts this adapter at `/`, creates
`/문서/메모.txt`, writes through `VfsFile`, verifies the exclusive-open rule,
stats and lists both Korean components, renames the file to
`/문서/기록.sura`, appends and reads through the VFS callbacks, rejects
nonempty directory deletion, removes the tree, remounts the same FAT bytes,
and confirms the path is absent. Success additionally requires
`SURA_FAT32_VFS_UTF8_OK` and `SURA_FAT32_VFS_REMOUNT_OK`.

The adapter is synchronous and uses one shared sector/LFN workspace, so its
caller must serialize operations. Nonempty recursive deletion,
cross-directory moves, advisory/shared locks, permissions, timestamps,
asynchronous I/O, and cache coherence are not implemented.

`stdlib/freestanding/memfs.sura` is a writable, hierarchical, volatile VFS
backend. The caller supplies fixed node storage, one fixed UTF-8 name slot per
node, and one fixed data slot per file. It supports file and directory
creation, exact read/write, append, resize, stat, directory listing,
whole-file atomic replace, cross-directory rename, and recursive or
nonrecursive removal. Component
validation rejects NUL, separators, dot segments, truncated sequences,
overlong encodings, surrogate values, and Unicode values above U+10FFFF.

`tools/sura_memfs_qemu_gate.ps1` executes those operations in a QEMU-booted
UEFI image. Its test creates `/문서/메모.txt`, writes and reads exact bytes,
replaces the complete file with one generation change, grows the file, lists
the Korean name, renames it to `/문서/기록.txt`, appends,
copies the complete directory tree through the allocation-free bounded
`vfs_copy_tree` workspace, verifies the copied 21-byte file, rejects a copy
into the source tree's own descendant,
creates `/새.sura` through the open-create flag, truncates it through a second
open, checks nonrecursive removal rejection, and recursively deletes the tree
before emitting `SURA_MEMFS_UTF8_OK`, `SURA_MEMFS_COPY_TREE_OK`, and
`SURA_MEMFS_MUTATION_OK`.

This memfs is not persistent when used by itself. SuraFS now uses it as the
in-memory node/data model for the graphical document volume; it is not exposed
as a separate volatile desktop mount. It does not allocate dynamically,
enforce ownership or permissions, provide
sparse extents or links, synchronize concurrent callers, journal changes, or
recover after a crash.

`stdlib/freestanding/surafs.sura` adds the first persistent native SuraFS
format. It stores the fixed-capacity memfs node, UTF-8 name, and file-data
slots in two complete on-disk banks. A mutation writes the inactive bank,
flushes it, publishes that bank's superblock, and flushes again. Each
superblock has a header CRC32 and records the CRC32 of the complete bank.
Mount validates both copies and chooses the newest intact generation.

`tools/sura_surafs_qemu_gate.ps1` formats a RAM-backed block device in a UEFI
image, creates Korean directory and file names, writes `OLD`, renames the
file, and commits `NEW`. It then deliberately overwrites the newest bank
header, remounts through a fresh SuraFS state, requires fallback to the prior
generation containing `OLD`, and verifies that append, listing, and recursive
deletion can be committed after recovery. Success requires
`SURA_SURAFS_RECOVERY_OK` and `SURA_SURAFS_MUTATION_OK`.

The QEMU graphical OS now stores user documents in a second type-`0x7f`
partition. File Explorer renders the UTF-8 `/문서` directory and opens
`/문서/메모.txt`; Text Editor replaces the selected file through one VFS
operation and therefore publishes one SuraFS generation per save. Existing
64 MiB data disks are extended to 128 MiB while retaining the original FAT32
partition for settings, desktop state, and migration compatibility.
`tools/sura_surafs_gui_qemu_gate.ps1` drives those GUI paths, inspects the
persisted Korean tree, verifies `temp` was moved under `/휴지통`, verifies
`agent.sura` became `/문서/renamed.sura`, verifies `/문서/한글.sura` was
created through graphical Korean input, and drives Explorer `Ctrl+C`,
`Ctrl+X`, and `Ctrl+V`. It verifies an exact 43-byte Korean-named document
copy is moved into `/문서/movebox`, then verifies recursive
`/문서/movebox - Copy` contains an independent exact-byte copy. It also
verifies `/문서/copy.sura` contains an
exact Save As copy of the 43-byte multiline ASCII-plus-Korean UTF-8 editor
payload, and checks that navigation scratch paths do not replace the active
editor path. It boots the preserved image again,
requires
`SURA_OS_SURAFS_EDITOR_RESTORED bytes=43 checksum=103782`, and requires the
SuraFS partition SHA-256 and generation to stay unchanged on that remount-only
boot. The same gate creates enough entries to cross the six-row viewport,
requires `SURA_OS_FILES_SCROLL_OK`, and inspects all extra nodes directly.
The Explorer clipboard path additionally requires `SURA_OS_FILES_COPY_READY`,
`SURA_OS_FILES_CUT_READY`, and `SURA_OS_FILES_PASTE_OK`. The editor then
selects the complete 43-byte UTF-8 document, copies it, cuts it through the
Ring-3 range-deletion path, pastes it through one bounded Ring-3 payload, and
still produces the exact persisted document and Save As copy. That path
requires `SURA_OS_EDITOR_SELECTION_OK`, `SURA_OS_EDITOR_COPY_OK`,
`SURA_OS_EDITOR_CUT_OK`, `SURA_OS_EDITOR_PASTE_OK`, and
`SURA_OS_EDITOR_SYNTAX_OK`.
The screenshot driver waits for each asynchronous Ring-3 editor
completion before sending a dependent composition key.

This is a persistent recovery foundation, not the final scalable filesystem.
It commits a whole fixed-size bank for every mutation and fixes node count,
name-slot size, and per-file capacity at format time. The graphical format
currently uses 32 nodes, 64-byte name slots, and 4096-byte data slots. It has
no extent
allocator, sparse files, permissions enforcement, ownership, links, internal
locking, encryption, compression, online resizing, bad-block handling, or
background scrub. CRC32 detects accidental corruption but does not
authenticate data. It has been executed with an AHCI-backed QEMU disk, not
installed or tested on a physical disk.

`examples/os/gpt_features.sura`, `examples/os/partition_features.sura`,
`examples/os/fat32_features.sura`, and `examples/os/vfs_features.sura` force
their compile paths through the UEFI x86-64 backend. The GPT example
constructs a RAM-backed table and checks its CRC and entry lookup; the
partition example checks the legacy fallback. The general smoke gate verifies
their generated PE32+ images and embedded feature markers. FAT32 mutation has
the stronger executed QEMU gate described above; GPT and partition mutation
still have compile/image verification only and no physical-hardware proof.

## Serial diagnostics and VM boot marker

`stdlib/freestanding/serial.sura` provides polling access to a
16550-compatible UART:

- initialization with a 16-bit baud divisor
- bounded transmit-ready and receive-ready polling
- byte reads and writes
- fixed-length and bounded null-terminated buffer writes

Every wait has a caller-supplied spin limit, so an absent UART does not force
an infinite loop. The library does not discover UARTs through ACPI, configure
interrupt-driven receive, or provide buffering and locking.

`examples/os/qemu_boot_gate.sura` initializes COM1, leaves UEFI boot services,
writes `SURA_EXIT_BOOT_SERVICES_OK`, and exits a QEMU instance configured with
`isa-debug-exit`. That debug-exit port is a VM test mechanism, not a portable
hardware shutdown interface.

`examples/os/ring3_qemu_gate.sura` uses the same VM-only exit mechanism after
the interrupt frame proves saved CS 35 and the subsequent fast syscall reaches
its ring-0 dispatcher. Its page-fault handler prints the setup stage, CR2,
error code, and RIP before failing, which keeps permission failures observable.

## ACPI MADT discovery

`stdlib/freestanding/acpi.sura` discovers the ACPI 2.0 or 1.0 RSDP from the
UEFI configuration table, validates RSDP and SDT checksums and bounded
lengths, prefers XSDT with RSDT fallback, and locates MADT (`APIC`). Its MADT
parser records:

- local APIC and x2APIC processor entries, flags, and ACPI UIDs
- usable processor count from enabled/online-capable flags
- I/O APIC identifiers, MMIO addresses, and GSI bases
- interrupt source overrides
- 64-bit local APIC address overrides

The caller supplies fixed processor, I/O APIC, and override buffers. Capacity
overflow sets `AcpiMadtInfo.truncated`; malformed entry lengths fail instead
of continuing past the table. Firmware configuration-table pages must remain
mapped while parsing. This module does not parse every ACPI table, configure
interrupt routing, allocate per-CPU state, or itself start an application
processor. Pair its processor records with
`stdlib/freestanding/ap_startup.sura`. `examples/os/acpi_features.sura` is a
non-executing compile feature test.

`stdlib/freestanding/ioapic.sura` turns the discovered I/O APIC and interrupt
source override records into fixed physical-destination redirection entries.
It validates I/O APIC identity and redirection count, resolves ISA IRQs with
ACPI polarity/trigger semantics, rejects missing or duplicate override data,
selects exactly one controller for a GSI, and programs a new route while it is
masked before applying the requested final mask state. The caller must map
MMIO uncached and serialize the shared IOREGSEL/IOWIN pair.

The current route builder supports fixed delivery to an 8-bit physical APIC
ID. It does not provide logical/lowest-priority delivery, x2APIC interrupt
remapping, NMI routes, interrupt-remapping hardware, or automatic vector
allocation. `examples/os/ioapic_features.sura` checks ISA override decoding
and the exact 64-bit redirection entry; MMIO programming has not been executed
in the current gate.

## Current lowering boundary

The backend currently lowers fixed-width locals and globals, concrete struct
layouts, typed pointer fields, functions with up to six exact arguments,
integer arithmetic and comparisons, `if`, `while`, `repeat`, `break`,
`continue`, calls, and returns. Nested calls use independent argument storage.
`and` and `or` short-circuit and return the selected operand, matching hosted
Sura; pointer guards therefore do not evaluate a guarded field access after
the left side has already decided the result.
Strings are supported for firmware console output and static data. Nested
relative imports are flattened into the same freestanding compilation unit.

Still required for a complete self-hosted OS environment:

- executed AP-startup coverage and complete per-AP descriptor, extended-state,
  scheduler-join, failure-recovery, and temporary-mapping lifecycle
- automatic per-CPU TSS/IST allocation and FPU/SIMD context-switch policy
- synchronized/NUMA physical-memory policy, a complete virtual address-space
  policy, shared mappings, PCID, and remote TLB shootdown
- SMP run queues, load balancing, process priorities/blocking, and FPU/SIMD
  process state
- dynamic/PIE ELF loading, relocations, TLS, demand paging, copy-on-write,
  signals, KPTI, NMI-safe entry, and broader speculative-entry hardening
- PCI/PCIe resource allocation, bridge configuration, MSI/MSI-X, production
  network drivers, USB hub/hot-plug/general-HID support, accelerated graphics,
  a multi-client audio mixer and capture service, physical audio-driver proof,
  and other production device-specific drivers
- partition creation/resizing, extended MBR chains, GPT repair, and a full
  persistent filesystem writer with allocation, creation, resizing, deletion,
  long names, recovery, and locking
- x86-64 ELF/raw-kernel output in addition to UEFI PE32+
- ARM64 freestanding backend
- source-level freestanding debugger and executed CI VM boot coverage
