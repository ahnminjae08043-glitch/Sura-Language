# Sura OS development

Sura has an experimental freestanding `uefi-x86_64` target. It emits a
PE32+ EFI application directly from `.sura` source. The output does not embed
the Sura VM, garbage collector, Windows API, C runtime, assembler, or linker.

This is an early systems subset, not a complete general-purpose OS SDK or an
operating system. The files in `examples/os` are compiler feature tests. They
do not contain a kernel, scheduler, filesystem, or device drivers. Test
generated images in a virtual machine before using physical hardware.

## Build

```powershell
.\SuraLanguage.exe --target uefi-x86_64 `
  --out BOOTX64.EFI examples\os\hello_uefi.sura
```

Place the file at `EFI\BOOT\BOOTX64.EFI` on a FAT-formatted UEFI image.
Unsigned development images normally require Secure Boot to be disabled.

The entry function is selected in this order: `efi_main`, `kernel_main`,
`main`. If none exists, top-level statements become the EFI entry body.
UEFI passes the image handle and system-table pointer in the first two
parameters.

```sura
func efi_main(image_handle: u64, system_table: ptr) -> u64 do
  uefi.write("Hello from Sura")
  uefi.newline()
  return 0
end
```

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

The GOP helpers use firmware graphics initialization, so a basic framebuffer
does not require a vendor-specific NVIDIA, AMD, or Intel driver. Accelerated
3D still requires a separate GPU driver.

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
- `cpu.load_gdt(address)`, `cpu.load_idt(address)`
- `cpu.invalidate_page(address)`
- `cpu.cpuid_eax/ebx/ecx/edx(leaf, subleaf?)`
- `cpu.rdtsc()`, `cpu.rdtscp()`, `cpu.xgetbv(index)`

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
`interrupt_error` is for vectors 8, 10–14, 17, 21, 29, and 30. The compiler
adds a synthetic zero for the first form, saves all general-purpose registers,
clears the direction flag for calls, aligns the handler stack, restores the
registers, discards the real or synthetic error code, and emits `iretq`.
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
User-mode entry also still needs a `swapgs` policy and validated kernel stack.

## Current lowering boundary

The backend currently lowers fixed-width locals and globals, concrete struct
layouts, typed pointer fields, functions with up to six exact arguments,
integer arithmetic and comparisons, `if`, `while`, `repeat`, `break`,
`continue`, calls, and returns. Nested calls use independent argument storage.
Strings are supported for firmware console output and static data.

Still required for a complete self-hosted OS environment:

- multiprocessor discovery and application-processor startup
- TSS/IST management, FPU/SIMD interrupt state, and user/kernel `swapgs` policy
- page-table, allocator, scheduler, syscall, and driver libraries
- FAT reader and boot-image builder
- x86-64 ELF/raw-kernel output in addition to UEFI PE32+
- ARM64 freestanding backend
- source-level freestanding debugger and emulator boot gate
