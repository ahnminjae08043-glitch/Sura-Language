# Sura OS development

Sura has an experimental freestanding `uefi-x86_64` target. It emits a
PE32+ EFI application directly from `.sura` source. The output does not embed
the Sura VM, garbage collector, Windows API, C runtime, assembler, or linker.

This is an early systems subset, not a complete general-purpose OS SDK or an
operating system. The files in `examples/os` are compiler feature tests. They
do not form a complete kernel, filesystem, or device-driver stack. Test
generated images in a virtual machine before using physical hardware.

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
- `cpu.load_gdt/load_idt(descriptor_address)`
- `cpu.load_gdt/load_idt(table, byte_size)`
- `cpu.reload_segments(code_selector, data_selector)`
- `cpu.load_task_register(selector)`, `cpu.read_task_register()`
- `cpu.invalidate_page(address)`
- `cpu.cpuid_eax/ebx/ecx/edx(leaf, subleaf?)`
- `cpu.rdtsc()`, `cpu.rdtscp()`, `cpu.xgetbv(index)`, `cpu.xsetbv(index, value)`
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
instructions. `stac/clac` require SMAP support and `swapgs` is only a
primitive; entry code must still enforce when it is safe to exchange GS bases.

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

These helpers send commands but do not invent the required timing or state
machine. Startup code must discover enabled processors from ACPI MADT, exclude
the BSP, wait for `icr_busy()` to clear, send INIT, observe the architecture's
delay requirements, send SIPI as required, and wait on an atomic per-AP ready
flag. A real-mode-to-long-mode trampoline and AP entry contract are still
required before `send_startup` can start Sura code.

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

## Indirect calls and software-interrupt syscalls

`call.indirect(function, argument...)` performs a Win64-compatible indirect
call to a function address with at most five integer or pointer arguments.
The target must follow the same freestanding Sura calling convention.

`syscall.invoke(vector, number, argument...)` emits `int vector`. The vector
must be a compile-time integer from 32 through 255. The syscall number is
placed in RAX and up to five arguments use RDI, RSI, RDX, R10, and R8. This is
a software-interrupt ABI; it is not the x86-64 `SYSCALL/SYSRET` instruction
pair.

`stdlib/freestanding/syscall.sura` provides a fixed-size handler table and the
`software_syscall_dispatch` interrupt handler. The dispatcher returns its
result through the saved RAX field. The caller must install a matching IDT
gate and owns every security-sensitive policy: CPL/DPL setup, user-pointer
validation, copy-in/copy-out, per-process permissions, synchronization,
address-space selection, and fault recovery. The compile-only
`examples/os/syscall_features.sura` emits both sides without entering user
mode or starting an OS.

## Current lowering boundary

The backend currently lowers fixed-width locals and globals, concrete struct
layouts, typed pointer fields, functions with up to six exact arguments,
integer arithmetic and comparisons, `if`, `while`, `repeat`, `break`,
`continue`, calls, and returns. Nested calls use independent argument storage.
Strings are supported for firmware console output and static data. Nested
relative imports are flattened into the same freestanding compilation unit.

Still required for a complete self-hosted OS environment:

- ACPI MADT processor discovery, AP trampoline, and complete AP startup
- automatic per-CPU TSS/IST allocation and FPU/SIMD context-switch policy
- user/kernel entry validation and `swapgs` policy
- synchronized/NUMA physical-memory policy, automatic intermediate page-table
  allocation/reclamation, virtual address-space policy, and remote TLB shootdown
- preemptive/SMP scheduling, `SYSCALL/SYSRET` and user-entry policy, and driver libraries
- FAT reader and persistent filesystem writer
- x86-64 ELF/raw-kernel output in addition to UEFI PE32+
- ARM64 freestanding backend
- source-level freestanding debugger and emulator boot gate
