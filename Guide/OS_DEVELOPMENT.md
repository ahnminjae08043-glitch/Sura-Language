# Sura OS development

Sura has an experimental freestanding `uefi-x86_64` target. It emits a
PE32+ EFI application directly from `.sura` source. The output does not embed
the Sura VM, garbage collector, Windows API, C runtime, assembler, or linker.

This is an early systems subset, not a complete general-purpose OS SDK yet.
Test generated images in a virtual machine before using physical hardware.

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
- `cpu.disable_interrupts()`, `cpu.enable_interrupts()`, `cpu.iret()`
- `cpu.read_cr0/cr2/cr3/cr4()`
- `cpu.write_cr0/cr3/cr4(value)`
- `cpu.read_flags()`, `cpu.write_flags(value)`
- `cpu.read_msr(index)`, `cpu.write_msr(index, value)`
- `cpu.load_gdt(address)`, `cpu.load_idt(address)`
- `cpu.invalidate_page(address)`

These operations are privileged. They are only recognized by the
freestanding target and are not added to normal hosted Sura execution.

## Current lowering boundary

The backend currently lowers fixed-width locals, functions with up to six
arguments, integer arithmetic and comparisons, `if`, `while`, `repeat`,
`break`, `continue`, calls, and returns. Strings are supported for firmware
console output as compile-time literals.

Still required for a complete self-hosted OS environment:

- static data declarations and structured pointer types
- interrupt-function declarations and saved-register frames
- atomic operations and multiprocessor startup
- page-table, allocator, scheduler, syscall, and driver libraries
- FAT reader and boot-image builder
- x86-64 ELF/raw-kernel output in addition to UEFI PE32+
- ARM64 freestanding backend
- source-level freestanding debugger and emulator boot gate
