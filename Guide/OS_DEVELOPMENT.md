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
The generic interrupt wrapper does not execute `swapgs`; a kernel that exposes
an IDT gate to ring 3 must supply its own privilege-transition entry policy.
The dedicated fast-syscall helper described below has a separate checked
`swapgs` and per-CPU stack contract.

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
primitive outside the generated fast-syscall helper; other entry paths must
still enforce when it is safe to exchange GS bases.

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

These operations do not create user page tables or mark pages U/S, load an
executable, validate syscall pointers, copy data across the privilege
boundary, isolate kernel mappings, mitigate speculative `swapgs` paths, save
FPU/SIMD state, or define process fault and exit policy. The kernel must keep
kernel code/data supervisor-only and provide those policies.
`examples/os/user_mode_features.sura` is a compile and machine-code feature
test; ring-3 execution still needs QEMU or hardware verification.

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
Transfers are accepted only inside the registered DMA window. The current
command path uses one command slot and one PRDT entry, so one request is at
most 4 MiB and 65,535 logical sectors; larger operations must be split by the
caller. A controller without 64-bit addressing is rejected when any DMA range
crosses 4 GiB.

This is a polling foundation, not a complete production storage stack. It
does not implement interrupts, NCQ, multiple outstanding commands, ATAPI,
port multipliers, hot-plug, TRIM, COMRESET/error recovery, power management,
IOMMU mapping. `examples/os/ahci_features.sura` checks command-header,
H2D FIS, and PRDT construction and retains discovery/initialization paths for
compilation. The generated machine code is checked, but no AHCI command has
yet been executed in QEMU or on hardware.

`stdlib/freestanding/nvme.sura` supplies a separate polling NVMe
NVM-command-set path. It discovers the PCI class and BAR, configures a
caller-owned admin queue, identifies the controller and a selected namespace,
creates I/O queue pair 1, builds read/write/flush commands, consumes
phase-tagged completion entries, and exposes the namespace as a `BlockDevice`.
The current PRP builder supports PRP1 plus one direct PRP2 page, so a request
can cover at most two 4-KiB controller pages and may be smaller when it starts
inside a page. Queue depth is limited to 64, queue memory must fit in one page,
the controller must support a 4-KiB minimum memory page, and formatted LBAs
with separate metadata are rejected.

The NVMe path is also synchronous and polling. It does not implement MSI/MSI-X,
multiple I/O queue pairs, concurrent commands, PRP lists, SGLs, namespace-list
selection, controller shutdown, abort/reset recovery, asynchronous events,
IOMMU mapping, or zoned namespaces. `examples/os/nvme_features.sura` checks SQ
entry and cross-page PRP construction. The image gate compiles and inspects
that path; it has not submitted a command to an emulated or physical NVMe
controller.

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
directory, read a complete regular file, and overwrite an existing regular
file without changing its size. The overwrite path preserves bytes outside a
partial final sector and flushes the device.

The FAT32 writer deliberately does not allocate or free clusters. It cannot
create, resize, delete, or rename files, and it does not support long file
names, subdirectory path parsing, FAT mirroring repair, FSInfo updates,
journaling, or concurrent access. A malformed or cyclic cluster chain is
bounded by the volume cluster count and fails.

`stdlib/freestanding/vfs.sura` supplies a fixed-capacity mount table and file
dispatch layer. Paths are caller-owned UTF-8 byte buffers with explicit
lengths. It accepts canonical absolute paths only: embedded NUL, a trailing or
repeated separator, and `.` or `..` segments are rejected rather than
normalized. It rejects duplicate mount points, resolves the longest matching
mount prefix, and dispatches open, read, write, seek, flush, close, and
filesystem sync through checked callbacks. All mount, filesystem, file, and
path storage is supplied by the kernel; the library allocates nothing and
does not provide internal locking.

`examples/os/gpt_features.sura`, `examples/os/partition_features.sura`,
`examples/os/fat32_features.sura`, and `examples/os/vfs_features.sura` force
these paths through the UEFI x86-64 backend. The GPT example constructs a
RAM-backed table and checks its CRC and entry lookup; the partition example
checks the legacy fallback. The smoke gate verifies their generated PE32+
images and embedded feature markers. This is compile and image verification,
not proof that disk I/O has run in QEMU or on hardware.

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

## Current lowering boundary

The backend currently lowers fixed-width locals and globals, concrete struct
layouts, typed pointer fields, functions with up to six exact arguments,
integer arithmetic and comparisons, `if`, `while`, `repeat`, `break`,
`continue`, calls, and returns. Nested calls use independent argument storage.
Strings are supported for firmware console output and static data. Nested
relative imports are flattened into the same freestanding compilation unit.

Still required for a complete self-hosted OS environment:

- executed AP-startup coverage and complete per-AP descriptor, extended-state,
  scheduler-join, failure-recovery, and temporary-mapping lifecycle
- automatic per-CPU TSS/IST allocation and FPU/SIMD context-switch policy
- synchronized/NUMA physical-memory policy, automatic intermediate page-table
  allocation/reclamation, virtual address-space policy, and remote TLB shootdown
- SMP run queues, load balancing, user-mode preemption, and executed
  timer/context-switch verification
- per-process address spaces and executable loading, user-pointer copy-in/out,
  process fault/exit policy, KPTI, and speculative-entry hardening
- PCI/PCIe resource allocation, bridge configuration, MSI/MSI-X, network,
  USB, graphics, audio, and other device-specific drivers
- partition creation/resizing, extended MBR chains, GPT repair, and a full
  persistent filesystem writer with allocation, creation, resizing, deletion,
  long names, recovery, and locking
- x86-64 ELF/raw-kernel output in addition to UEFI PE32+
- ARM64 freestanding backend
- source-level freestanding debugger and executed CI VM boot coverage
