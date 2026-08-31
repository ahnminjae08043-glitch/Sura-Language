#include "../os_target.hpp"
#include "../uefi_disk.hpp"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>

static uint16_t read_u16(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint16_t>(bytes.at(offset)) |
           static_cast<uint16_t>(bytes.at(offset + 1) << 8);
}

static uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes.at(offset)) |
           (static_cast<uint32_t>(bytes.at(offset + 1)) << 8) |
           (static_cast<uint32_t>(bytes.at(offset + 2)) << 16) |
           (static_cast<uint32_t>(bytes.at(offset + 3)) << 24);
}

static uint64_t read_u64(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint64_t>(read_u32(bytes, offset)) |
           (static_cast<uint64_t>(read_u32(bytes, offset + 4)) << 32);
}

static ExprPtr method_call(const std::string& module, const std::string& method,
                           std::vector<ExprPtr> args = {}) {
    auto call = std::make_unique<MethodCallExpr>(
        std::make_unique<Ident>(module, 1), method, 1);
    call->args = std::move(args);
    return call;
}

static TypeAnnot scalar_type(const std::string& name) {
    TypeAnnot type;
    type.present = true;
    type.kind = SType::NUMBER;
    type.source_name = name;
    return type;
}

static bool contains_bytes(const std::vector<uint8_t>& image,
                           std::initializer_list<uint8_t> needle) {
    return std::search(image.begin(), image.end(),
                       needle.begin(), needle.end()) != image.end();
}

static size_t count_bytes(const std::vector<uint8_t>& image,
                          std::initializer_list<uint8_t> needle) {
    size_t count = 0;
    auto cursor = image.begin();
    while (cursor != image.end()) {
        cursor = std::search(cursor, image.end(), needle.begin(), needle.end());
        if (cursor == image.end()) break;
        ++count;
        ++cursor;
    }
    return count;
}

int main(int argc, char** argv) {
    auto root = std::make_unique<SuraBlock>(1);

    auto pci = std::make_unique<ClassDef>("PciHeader", std::string(), 1);
    pci->value_struct = true;
    pci->packed_layout = true;
    pci->add_field("vendor_id", std::make_unique<NilLit>(1), false,
                   scalar_type("u16"));
    pci->add_field("device_id", std::make_unique<NilLit>(1), false,
                   scalar_type("u16"));
    pci->add_field("command", std::make_unique<NilLit>(1), false,
                   scalar_type("u16"));
    root->body.push_back(std::move(pci));

    auto counter = std::make_unique<AssignStmt>(
        "boot_counter", std::make_unique<NumLit>(1, 1), 1);
    counter->type_annot = scalar_type("u64");
    root->body.push_back(std::move(counter));
    auto saved_context = std::make_unique<AssignStmt>(
        "saved_context_rsp", std::make_unique<NumLit>(0, 1), 1);
    saved_context->type_annot = scalar_type("u64");
    root->body.push_back(std::move(saved_context));

    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("PciHeader", 1));
        root->body.push_back(std::make_unique<AssignStmt>(
            "pci_header", method_call("static", "struct", std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(4096, 1));
        args.push_back(std::make_unique<NumLit>(4096, 1));
        root->body.push_back(std::make_unique<AssignStmt>(
            "page_buffer", method_call("static", "zero", std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(4096, 1));
        args.push_back(std::make_unique<NumLit>(16, 1));
        root->body.push_back(std::make_unique<AssignStmt>(
            "idt_table", method_call("static", "zero", std::move(args)), 1));
    }
    const auto add_static_zero =
        [&](const std::string& name, uint64_t size, uint64_t alignment) {
            std::vector<ExprPtr> args;
            args.push_back(std::make_unique<NumLit>(
                static_cast<double>(size), 1));
            args.push_back(std::make_unique<NumLit>(
                static_cast<double>(alignment), 1));
            root->body.push_back(std::make_unique<AssignStmt>(
                name, method_call("static", "zero", std::move(args)), 1));
        };
    add_static_zero("gdt_table", 128, 16);
    add_static_zero("tss_state", 104, 16);
    add_static_zero("kernel_stack", 4096, 16);
    add_static_zero("fpu_state", 4096, 64);
    add_static_zero("percpu_state", 256, 64);
    add_static_zero("task_stack", 4096, 16);

    auto helper_body = std::make_unique<SuraBlock>(1);
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<StrLit>("Sura UEFI", 1));
        helper_body->body.push_back(std::make_unique<ExprStmt>(
            method_call("uefi", "write", std::move(args)), 1));
    }
    helper_body->body.push_back(
        std::make_unique<ReturnStmt>(std::make_unique<NumLit>(7, 1), 1));
    auto helper = std::make_unique<FuncDef>(
        "banner", std::vector<std::string>{}, std::move(helper_body), 1);
    root->body.push_back(std::move(helper));

    auto low_body = std::make_unique<SuraBlock>(1);
    low_body->body.push_back(std::make_unique<AssignStmt>(
        "framebuffer", method_call("uefi", "gop_framebuffer"), 1));
    low_body->body.push_back(std::make_unique<ExprStmt>(
        method_call("cpu", "disable_interrupts"), 1));
    low_body->body.push_back(std::make_unique<ExprStmt>(
        method_call("cpu", "halt"), 1));
    low_body->body.push_back(
        std::make_unique<ReturnStmt>(std::make_unique<NumLit>(0, 1), 1));
    auto low = std::make_unique<FuncDef>(
        "low_level_probe", std::vector<std::string>{}, std::move(low_body), 1);
    root->body.push_back(std::move(low));

    auto task_body = std::make_unique<SuraBlock>(1);
    task_body->body.push_back(std::make_unique<ReturnStmt>(
        std::make_unique<Ident>("argument", 1), 1));
    auto task = std::make_unique<FuncDef>(
        "task_entry", std::vector<std::string>{"argument"},
        std::move(task_body), 1);
    task->param_types.push_back(scalar_type("u64"));
    root->body.push_back(std::move(task));

    auto task_exit_body = std::make_unique<SuraBlock>(1);
    task_exit_body->body.push_back(
        std::make_unique<ReturnStmt>(nullptr, 1));
    auto task_exit = std::make_unique<FuncDef>(
        "task_exit", std::vector<std::string>{"result"},
        std::move(task_exit_body), 1);
    task_exit->param_types.push_back(scalar_type("u64"));
    root->body.push_back(std::move(task_exit));

    auto fast_handler_body = std::make_unique<SuraBlock>(1);
    fast_handler_body->body.push_back(std::make_unique<ReturnStmt>(
        std::make_unique<NumLit>(0, 1), 1));
    auto fast_handler = std::make_unique<FuncDef>(
        "fast_syscall_handler", std::vector<std::string>{"frame"},
        std::move(fast_handler_body), 1);
    fast_handler->param_types.push_back(scalar_type("ptr"));
    root->body.push_back(std::move(fast_handler));

    auto fast_bad_body = std::make_unique<SuraBlock>(1);
    fast_bad_body->body.push_back(std::make_unique<ReturnStmt>(
        std::make_unique<NumLit>(0, 1), 1));
    auto fast_bad = std::make_unique<FuncDef>(
        "fast_bad_return", std::vector<std::string>{"frame"},
        std::move(fast_bad_body), 1);
    fast_bad->param_types.push_back(scalar_type("ptr"));
    root->body.push_back(std::move(fast_bad));

    auto irq_body = std::make_unique<SuraBlock>(1);
    {
        auto address = std::make_unique<CallExpr>("addr_of", 1);
        address->args.push_back(std::make_unique<Ident>("boot_counter", 1));
        std::vector<ExprPtr> args;
        args.push_back(std::move(address));
        args.push_back(std::make_unique<NumLit>(1, 1));
        irq_body->body.push_back(std::make_unique<ExprStmt>(
            method_call("atomic", "fetch_add64", std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("frame", 1));
        irq_body->body.push_back(std::make_unique<AssignStmt>(
            "preempt_frame_valid",
            method_call("preempt", "frame_valid", std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("frame", 1));
        irq_body->body.push_back(std::make_unique<AssignStmt>(
            "preempt_resumed",
            method_call("preempt", "resume", std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("frame", 1));
        irq_body->body.push_back(std::make_unique<AssignStmt>(
            "user_frame_valid",
            method_call("user", "frame_valid", std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("frame", 1));
        irq_body->body.push_back(std::make_unique<AssignStmt>(
            "user_resumed",
            method_call("user", "resume", std::move(args)), 1));
    }
    irq_body->body.push_back(
        std::make_unique<ReturnStmt>(nullptr, 1));
    auto irq = std::make_unique<FuncDef>(
        "timer_interrupt", std::vector<std::string>{"frame"},
        std::move(irq_body), 1);
    irq->abi = "interrupt";
    irq->param_types.push_back(scalar_type("ptr"));
    root->body.push_back(std::move(irq));

    auto fault_body = std::make_unique<SuraBlock>(1);
    fault_body->body.push_back(
        std::make_unique<ReturnStmt>(nullptr, 1));
    auto fault = std::make_unique<FuncDef>(
        "page_fault_interrupt", std::vector<std::string>{"frame"},
        std::move(fault_body), 1);
    fault->abi = "interrupt_error";
    fault->param_types.push_back(scalar_type("ptr"));
    root->body.push_back(std::move(fault));

    auto entry_body = std::make_unique<SuraBlock>(1);
    entry_body->body.push_back(std::make_unique<GlobalDeclStmt>(
        std::vector<std::string>{"boot_counter"}, 1));
    entry_body->body.push_back(std::make_unique<AssignStmt>(
        "short_circuit_and",
        std::make_unique<BinOp>(
            "and", std::make_unique<NumLit>(0, 1),
            std::make_unique<NumLit>(static_cast<double>(0x11223344), 1), 1),
        1));
    entry_body->body.push_back(std::make_unique<AssignStmt>(
        "short_circuit_or",
        std::make_unique<BinOp>(
            "or", std::make_unique<NumLit>(7, 1),
            std::make_unique<NumLit>(static_cast<double>(0x55667788), 1), 1),
        1));
    entry_body->body.push_back(std::make_unique<InPlaceStmt>(
        "boot_counter", "+", std::make_unique<NumLit>(1, 1), 1));
    auto header = std::make_unique<AssignStmt>(
        "header", std::make_unique<Ident>("pci_header", 1), 1);
    header->type_annot = scalar_type("ptr[PciHeader]");
    entry_body->body.push_back(std::move(header));
    entry_body->body.push_back(std::make_unique<DotAssignStmt>(
        "header", "command", std::make_unique<NumLit>(7, 1), 1));
    entry_body->body.push_back(std::make_unique<AssignStmt>(
        "vendor", std::make_unique<DotAccess>(
                      std::make_unique<Ident>("header", 1), "vendor_id", 1),
        1));
    {
        std::vector<ExprPtr> address_args;
        address_args.push_back(std::make_unique<Ident>("boot_counter", 1));
        std::vector<ExprPtr> atomic_args;
        atomic_args.push_back(std::make_unique<CallExpr>("addr_of", 1));
        static_cast<CallExpr*>(atomic_args.back().get())->args =
            std::move(address_args);
        atomic_args.push_back(std::make_unique<NumLit>(1, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "previous", method_call("atomic", "fetch_add64",
                                      std::move(atomic_args)), 1));
    }
    {
        std::vector<ExprPtr> cpuid_args;
        cpuid_args.push_back(std::make_unique<NumLit>(0, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "cpuid_max", method_call("cpu", "cpuid_eax",
                                      std::move(cpuid_args)), 1));
    }
    {
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "hardware_random", method_call("cpu", "rdrand", {}), 1));
    }
    {
        auto size = std::make_unique<CallExpr>("sizeof", 1);
        size->args.push_back(std::make_unique<Ident>("PciHeader", 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "header_size", std::move(size), 1));
    }
    {
        auto handler = std::make_unique<CallExpr>("addr_of", 1);
        handler->args.push_back(
            std::make_unique<Ident>("timer_interrupt", 1));
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("idt_table", 1));
        args.push_back(std::make_unique<NumLit>(32, 1));
        args.push_back(std::move(handler));
        args.push_back(std::make_unique<NumLit>(8, 1));
        args.push_back(std::make_unique<NumLit>(0, 1));
        args.push_back(std::make_unique<NumLit>(142, 1));
        entry_body->body.push_back(std::make_unique<ExprStmt>(
            method_call("cpu", "idt_set_gate", std::move(args)), 1));
    }
    const auto add_cpu_statement =
        [&](const std::string& method, std::vector<ExprPtr> args = {}) {
            entry_body->body.push_back(std::make_unique<ExprStmt>(
                method_call("cpu", method, std::move(args)), 1));
        };
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("tss_state", 1));
        args.push_back(std::make_unique<NumLit>(0, 1));
        args.push_back(std::make_unique<Ident>("kernel_stack", 1));
        add_cpu_statement("tss_set_rsp", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("tss_state", 1));
        args.push_back(std::make_unique<NumLit>(1, 1));
        args.push_back(std::make_unique<Ident>("kernel_stack", 1));
        add_cpu_statement("tss_set_ist", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("tss_state", 1));
        args.push_back(std::make_unique<NumLit>(104, 1));
        add_cpu_statement("tss_set_iomap", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("gdt_table", 1));
        args.push_back(std::make_unique<NumLit>(3, 1));
        args.push_back(std::make_unique<Ident>("tss_state", 1));
        args.push_back(std::make_unique<NumLit>(103, 1));
        add_cpu_statement("gdt_set_tss", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("gdt_table", 1));
        args.push_back(std::make_unique<NumLit>(128, 1));
        add_cpu_statement("load_gdt", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(8, 1));
        args.push_back(std::make_unique<NumLit>(16, 1));
        add_cpu_statement("reload_segments", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("idt_table", 1));
        args.push_back(std::make_unique<NumLit>(4096, 1));
        add_cpu_statement("load_idt", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(24, 1));
        add_cpu_statement("load_task_register", std::move(args));
    }
    {
        auto read_tr = method_call("cpu", "read_task_register");
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "task_selector", std::move(read_tr), 1));
    }
    add_cpu_statement("fninit");
    add_cpu_statement("clts");
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("fpu_state", 1));
        add_cpu_statement("fxsave", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("fpu_state", 1));
        add_cpu_statement("fxrstor", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(0, 1));
        args.push_back(std::make_unique<NumLit>(7, 1));
        add_cpu_statement("xsetbv", std::move(args));
    }
    for (const std::string& method :
         std::vector<std::string>{"xsave", "xrstor"}) {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("fpu_state", 1));
        args.push_back(std::make_unique<NumLit>(7, 1));
        add_cpu_statement(method, std::move(args));
    }
    add_cpu_statement("stac");
    add_cpu_statement("clac");
    add_cpu_statement("swapgs");
    add_cpu_statement("wbinvd");
    const auto add_module_statement =
        [&](const std::string& module, const std::string& method,
            std::vector<ExprPtr> args = {}) {
            entry_body->body.push_back(std::make_unique<ExprStmt>(
                method_call(module, method, std::move(args)), 1));
        };
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("percpu_state", 1));
        add_module_statement("percpu", "set_base", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("percpu_state", 1));
        add_module_statement("percpu", "set_kernel_base", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(0, 1));
        args.push_back(method_call("apic", "current_id"));
        add_module_statement("percpu", "write64", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(0, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "percpu_value", method_call("percpu", "read64", std::move(args)), 1));
    }
    for (const std::string& method :
         std::vector<std::string>{"mode", "base", "current_id", "icr_busy"}) {
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "apic_" + method, method_call("apic", method), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(32, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "apic_id_register",
            method_call("apic", "read", std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(240, 1));
        args.push_back(std::make_unique<NumLit>(511, 1));
        add_module_statement("apic", "write", std::move(args));
    }
    add_module_statement("apic", "eoi");
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(1, 1));
        args.push_back(std::make_unique<NumLit>(64, 1));
        add_module_statement("apic", "send_ipi", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(1, 1));
        add_module_statement("apic", "send_init", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(1, 1));
        args.push_back(std::make_unique<NumLit>(32768, 1));
        add_module_statement("apic", "send_startup", std::move(args));
    }
    for (const std::string& method :
         std::vector<std::string>{"pml4_index", "pdpt_index", "pd_index",
                                  "pt_index", "offset", "is_canonical48"}) {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(0x12345678, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "paging_" + method,
            method_call("paging", method, std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("page_buffer", 1));
        args.push_back(std::make_unique<NumLit>(3, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "page_entry", method_call("paging", "entry", std::move(args)), 1));
    }
    for (const std::string& method :
         std::vector<std::string>{"entry_address", "entry_flags",
                                  "present", "large"}) {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("page_entry", 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "paging_" + method,
            method_call("paging", method, std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("page_buffer", 1));
        args.push_back(std::make_unique<NumLit>(1, 1));
        args.push_back(std::make_unique<Ident>("page_entry", 1));
        add_module_statement("paging", "write", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("page_buffer", 1));
        args.push_back(std::make_unique<NumLit>(1, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "paging_read", method_call("paging", "read", std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("page_buffer", 1));
        args.push_back(std::make_unique<NumLit>(2, 1));
        args.push_back(std::make_unique<Ident>("page_buffer", 1));
        args.push_back(std::make_unique<NumLit>(3, 1));
        add_module_statement("paging", "map", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("page_buffer", 1));
        args.push_back(std::make_unique<NumLit>(2, 1));
        add_module_statement("paging", "clear", std::move(args));
    }
    entry_body->body.push_back(std::make_unique<AssignStmt>(
        "paging_root", method_call("paging", "root"), 1));
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<Ident>("page_buffer", 1));
        add_module_statement("paging", "activate", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(0x12345678, 1));
        add_module_statement("paging", "invalidate", std::move(args));
    }
    add_module_statement("paging", "flush");
    entry_body->body.push_back(std::make_unique<AssignStmt>(
        "context_frame_size", method_call("context", "frame_size"), 1));
    entry_body->body.push_back(std::make_unique<AssignStmt>(
        "preempt_frame_size", method_call("preempt", "frame_size"), 1));
    {
        std::vector<ExprPtr> stack_args;
        stack_args.push_back(std::make_unique<Ident>("task_stack", 1));
        stack_args.push_back(std::make_unique<NumLit>(4096, 1));

        auto entry_address = std::make_unique<CallExpr>("addr_of", 1);
        entry_address->args.push_back(std::make_unique<Ident>("task_entry", 1));
        auto exit_address = std::make_unique<CallExpr>("addr_of", 1);
        exit_address->args.push_back(std::make_unique<Ident>("task_exit", 1));

        std::vector<ExprPtr> args;
        args.push_back(method_call("ptr", "add", std::move(stack_args)));
        args.push_back(std::move(entry_address));
        args.push_back(std::make_unique<NumLit>(123, 1));
        args.push_back(std::move(exit_address));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "initial_context_rsp",
            method_call("context", "init", std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> stack_args;
        stack_args.push_back(std::make_unique<Ident>("task_stack", 1));
        stack_args.push_back(std::make_unique<NumLit>(4096, 1));

        auto entry_address = std::make_unique<CallExpr>("addr_of", 1);
        entry_address->args.push_back(std::make_unique<Ident>("task_entry", 1));
        auto exit_address = std::make_unique<CallExpr>("addr_of", 1);
        exit_address->args.push_back(std::make_unique<Ident>("task_exit", 1));

        std::vector<ExprPtr> args;
        args.push_back(method_call("ptr", "add", std::move(stack_args)));
        args.push_back(std::move(entry_address));
        args.push_back(std::make_unique<NumLit>(456, 1));
        args.push_back(std::move(exit_address));
        args.push_back(std::make_unique<NumLit>(8, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "initial_preempt_frame",
            method_call("preempt", "init", std::move(args)), 1));
    }
    {
        auto saved_address = std::make_unique<CallExpr>("addr_of", 1);
        saved_address->args.push_back(
            std::make_unique<Ident>("saved_context_rsp", 1));
        std::vector<ExprPtr> args;
        args.push_back(std::move(saved_address));
        args.push_back(std::make_unique<Ident>("initial_context_rsp", 1));
        add_module_statement("context", "switch", std::move(args));
    }
    {
        auto task_address = std::make_unique<CallExpr>("addr_of", 1);
        task_address->args.push_back(
            std::make_unique<Ident>("task_entry", 1));
        std::vector<ExprPtr> args;
        args.push_back(std::move(task_address));
        args.push_back(std::make_unique<NumLit>(41, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "indirect_result",
            method_call("call", "indirect", std::move(args)), 1));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(128, 1));
        args.push_back(std::make_unique<NumLit>(7, 1));
        for (uint64_t value = 1; value <= 5; ++value) {
            args.push_back(std::make_unique<NumLit>(
                static_cast<double>(value), 1));
        }
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "syscall_result",
            method_call("syscall", "invoke", std::move(args)), 1));
    }
    {
        auto dispatch = std::make_unique<CallExpr>("addr_of", 1);
        dispatch->args.push_back(
            std::make_unique<Ident>("fast_syscall_handler", 1));
        auto bad_return = std::make_unique<CallExpr>("addr_of", 1);
        bad_return->args.push_back(
            std::make_unique<Ident>("fast_bad_return", 1));
        std::vector<ExprPtr> args;
        args.push_back(std::move(dispatch));
        args.push_back(std::move(bad_return));
        args.push_back(std::make_unique<NumLit>(8, 1));
        args.push_back(std::make_unique<NumLit>(35, 1));
        args.push_back(std::make_unique<NumLit>(292608, 1));
        args.push_back(std::make_unique<NumLit>(0, 1));
        args.push_back(std::make_unique<NumLit>(8, 1));
        add_module_statement(
            "syscall", "fast_configure", std::move(args));
    }
    {
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<NumLit>(7, 1));
        for (uint64_t value = 1; value <= 5; ++value) {
            args.push_back(std::make_unique<NumLit>(
                static_cast<double>(value), 1));
        }
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "fast_syscall_result",
            method_call("syscall", "fast", std::move(args)), 1));
    }
    {
        auto task_address = std::make_unique<CallExpr>("addr_of", 1);
        task_address->args.push_back(
            std::make_unique<Ident>("task_entry", 1));
        std::vector<ExprPtr> args;
        args.push_back(std::move(task_address));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "user_address_valid",
            method_call("user", "is_address", std::move(args)), 1));
    }
    entry_body->body.push_back(std::make_unique<AssignStmt>(
        "user_frame_bytes", method_call("user", "frame_size"), 1));
    {
        std::vector<ExprPtr> kernel_stack_args;
        kernel_stack_args.push_back(std::make_unique<Ident>("kernel_stack", 1));
        kernel_stack_args.push_back(std::make_unique<NumLit>(4096, 1));
        auto task_address = std::make_unique<CallExpr>("addr_of", 1);
        task_address->args.push_back(
            std::make_unique<Ident>("task_entry", 1));
        std::vector<ExprPtr> user_stack_args;
        user_stack_args.push_back(std::make_unique<Ident>("task_stack", 1));
        user_stack_args.push_back(std::make_unique<NumLit>(4088, 1));
        std::vector<ExprPtr> args;
        args.push_back(method_call("ptr", "add", std::move(kernel_stack_args)));
        args.push_back(std::move(task_address));
        args.push_back(method_call("ptr", "add", std::move(user_stack_args)));
        args.push_back(std::make_unique<NumLit>(456, 1));
        args.push_back(std::make_unique<NumLit>(35, 1));
        args.push_back(std::make_unique<NumLit>(27, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "initial_user_frame",
            method_call("user", "frame_init", std::move(args)), 1));
    }
    {
        auto task_address = std::make_unique<CallExpr>("addr_of", 1);
        task_address->args.push_back(
            std::make_unique<Ident>("task_entry", 1));
        std::vector<ExprPtr> stack_args;
        stack_args.push_back(std::make_unique<Ident>("task_stack", 1));
        stack_args.push_back(std::make_unique<NumLit>(4088, 1));
        std::vector<ExprPtr> args;
        args.push_back(std::move(task_address));
        args.push_back(method_call("ptr", "add", std::move(stack_args)));
        args.push_back(std::make_unique<NumLit>(123, 1));
        args.push_back(std::make_unique<NumLit>(35, 1));
        args.push_back(std::make_unique<NumLit>(27, 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "user_entered",
            method_call("user", "enter", std::move(args)), 1));
    }
    {
        auto maximum_call = std::make_unique<CallExpr>("u64", 1);
        maximum_call->args.push_back(
            std::make_unique<StrLit>("0xffffffffffffffff", 1));
        auto maximum = std::make_unique<AssignStmt>(
            "unsigned_maximum", std::move(maximum_call), 1);
        maximum->type_annot = scalar_type("u64");
        entry_body->body.push_back(std::move(maximum));

        auto unsigned_divide = std::make_unique<AssignStmt>(
            "unsigned_divide",
            std::make_unique<BinOp>(
                "/", std::make_unique<Ident>("unsigned_maximum", 1),
                std::make_unique<NumLit>(255, 1), 1),
            1);
        unsigned_divide->type_annot = scalar_type("u64");
        entry_body->body.push_back(std::move(unsigned_divide));

        auto unsigned_compare = std::make_unique<AssignStmt>(
            "unsigned_compare",
            std::make_unique<BinOp>(
                ">", std::make_unique<Ident>("unsigned_maximum", 1),
                std::make_unique<NumLit>(7, 1), 1),
            1);
        unsigned_compare->type_annot = scalar_type("bool");
        entry_body->body.push_back(std::move(unsigned_compare));

        auto unsigned_shift = std::make_unique<AssignStmt>(
            "unsigned_shift",
            std::make_unique<BinOp>(
                ">>", std::make_unique<Ident>("unsigned_maximum", 1),
                std::make_unique<NumLit>(63, 1), 1),
            1);
        unsigned_shift->type_annot = scalar_type("u64");
        entry_body->body.push_back(std::move(unsigned_shift));

        auto negative = std::make_unique<AssignStmt>(
            "signed_negative", std::make_unique<NumLit>(-8, 1), 1);
        negative->type_annot = scalar_type("i64");
        entry_body->body.push_back(std::move(negative));

        auto signed_divide = std::make_unique<AssignStmt>(
            "signed_divide",
            std::make_unique<BinOp>(
                "/", std::make_unique<Ident>("signed_negative", 1),
                std::make_unique<NumLit>(2, 1), 1),
            1);
        signed_divide->type_annot = scalar_type("i64");
        entry_body->body.push_back(std::move(signed_divide));

        auto signed_compare = std::make_unique<AssignStmt>(
            "signed_compare",
            std::make_unique<BinOp>(
                "<", std::make_unique<Ident>("signed_negative", 1),
                std::make_unique<NumLit>(0, 1), 1),
            1);
        signed_compare->type_annot = scalar_type("bool");
        entry_body->body.push_back(std::move(signed_compare));

        auto signed_shift = std::make_unique<AssignStmt>(
            "signed_shift",
            std::make_unique<BinOp>(
                ">>", std::make_unique<Ident>("signed_negative", 1),
                std::make_unique<NumLit>(2, 1), 1),
            1);
        signed_shift->type_annot = scalar_type("i64");
        entry_body->body.push_back(std::move(signed_shift));
    }
    entry_body->body.push_back(std::make_unique<ExprStmt>(
        method_call("uefi", "clear"), 1));
    entry_body->body.push_back(std::make_unique<ExprStmt>(
        std::make_unique<CallExpr>("banner", 1), 1));
    entry_body->body.push_back(
        std::make_unique<ReturnStmt>(std::make_unique<NumLit>(0, 1), 1));
    auto entry = std::make_unique<FuncDef>(
        "efi_main", std::vector<std::string>{"image", "system"},
        std::move(entry_body), 1);
    root->body.push_back(std::move(entry));

    SuraOsCompileResult result = sura_compile_uefi_x64(root.get());
    assert(result.target == "uefi-x86_64");
    assert(result.entry_function == "efi_main");
    assert(result.machine_code_bytes > 128);
    assert(result.data_bytes >= 8192);
    assert(result.image.size() >= 10240);
    assert(result.image[0] == 'M' && result.image[1] == 'Z');
    assert(contains_bytes(result.image,
                          {0x31, 0xd2, 0x49, 0xf7, 0xf2})); // unsigned div
    assert(contains_bytes(result.image,
                          {0x48, 0x99, 0x49, 0xf7, 0xfa})); // signed idiv
    assert(contains_bytes(result.image,
                          {0x48, 0x39, 0xc1, 0x0f, 0x97, 0xc0})); // unsigned >
    assert(contains_bytes(result.image,
                          {0x48, 0x39, 0xc1, 0x0f, 0x9c, 0xc0})); // signed <
    assert(contains_bytes(result.image,
                          {0x48, 0xd3, 0xe8})); // unsigned shift right
    assert(contains_bytes(result.image,
                          {0x48, 0xd3, 0xf8})); // signed shift right
    assert(contains_bytes(result.image,
                          {0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x48, 0x85, 0xc0, 0x0f, 0x84,
                           0x0a, 0x00, 0x00, 0x00, 0x48, 0xb8,
                           0x44, 0x33, 0x22, 0x11})); // short-circuit and
    assert(contains_bytes(result.image,
                          {0x48, 0xb8, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x48, 0x85, 0xc0, 0x0f, 0x85,
                           0x0a, 0x00, 0x00, 0x00, 0x48, 0xb8,
                           0x88, 0x77, 0x66, 0x55})); // short-circuit or
    assert(contains_bytes(result.image, {0x0f, 0xa2})); // cpuid
    assert(contains_bytes(result.image,
                          {0x48, 0x0f, 0xc7, 0xf0, 0x72, 0x02,
                           0x31, 0xc0})); // rdrand, zero on CF=0
    assert(contains_bytes(result.image,
                          {0xf0, 0x48, 0x0f, 0xc1, 0x01})); // lock xadd
    assert(count_bytes(result.image, {0x48, 0xcf}) >= 3); // iretq
    assert(contains_bytes(result.image,
                          {0xfa, 0xf6, 0x44, 0x24, 0x08, 0x01,
                           0x0f, 0x84})); // conditional SWAPGS, no-error entry
    assert(contains_bytes(result.image,
                          {0xfa, 0xf6, 0x44, 0x24, 0x10, 0x01,
                           0x0f, 0x84})); // conditional SWAPGS, error entry
    assert(contains_bytes(result.image,
                          {0xf6, 0x44, 0x24, 0x08, 0x02,
                           0x0f, 0x84})); // require both RPL bits
    assert(count_bytes(result.image,
                       {0x0f, 0xae, 0xe8}) >= 3); // serialized SWAPGS paths
    assert(contains_bytes(result.image,
                          {0x66, 0x45, 0x89, 0x1a})); // IDT offset low
    assert(contains_bytes(result.image,
                          {0x6a, 0x00, 0x50, 0x51, 0x52, 0x53})); // no-error wrapper
    assert(contains_bytes(result.image,
                          {0x41, 0xc6, 0x42, 0x05, 0x89})); // TSS descriptor
    assert(contains_bytes(result.image, {0x0f, 0x01, 0x14, 0x24})); // lgdt
    assert(contains_bytes(result.image, {0x0f, 0x01, 0x1c, 0x24})); // lidt
    assert(contains_bytes(result.image, {0x0f, 0x00, 0xd8})); // ltr
    assert(contains_bytes(result.image, {0x0f, 0x00, 0xc8})); // str
    assert(contains_bytes(result.image, {0x48, 0xcb})); // far return
    assert(contains_bytes(result.image, {0x0f, 0x01, 0xf8})); // swapgs
    assert(contains_bytes(result.image, {0x48, 0x0f, 0xae, 0x00})); // fxsave64
    assert(contains_bytes(result.image, {0x49, 0x0f, 0xae, 0x23})); // xsave64
    assert(contains_bytes(result.image, {0x0f, 0x01, 0xd1})); // xsetbv
    assert(contains_bytes(result.image, {0x65, 0x48, 0x8b, 0x00})); // GS read
    assert(contains_bytes(result.image, {0x65, 0x48, 0x89, 0x01})); // GS write
    assert(contains_bytes(result.image,
                          {0xb9, 0x30, 0x08, 0x00, 0x00, 0x0f, 0x30})); // x2 ICR
    assert(contains_bytes(result.image,
                          {0x41, 0x89, 0x82, 0x10, 0x03, 0x00, 0x00})); // xAPIC ICR high
    assert(contains_bytes(result.image,
                          {0x48, 0xc1, 0xe8, 0x0c, 0x25, 0xff, 0x00,
                           0x00, 0x00, 0x0d, 0x00, 0x06, 0x00, 0x00})); // SIPI vector
    assert(contains_bytes(result.image,
                          {0x48, 0xc1, 0xe8, 0x27,
                           0x25, 0xff, 0x01, 0x00, 0x00})); // PML4 index
    assert(contains_bytes(result.image,
                          {0x48, 0xba, 0x00, 0xf0, 0xff, 0xff, 0xff,
                           0xff, 0x0f, 0x00, 0x48, 0x21, 0xd0})); // entry mask
    assert(contains_bytes(result.image,
                          {0x48, 0x01, 0xd1, 0x48, 0x89, 0x01})); // PTE write
    assert(contains_bytes(result.image, {0x0f, 0x01, 0x38})); // invlpg
    assert(contains_bytes(result.image,
                          {0x0f, 0x20, 0xd8, 0x0f, 0x22, 0xd8})); // CR3 flush
    assert(contains_bytes(result.image,
                          {0x53, 0x55, 0x57, 0x56, 0x41, 0x54, 0x41,
                           0x55, 0x41, 0x56, 0x41, 0x57,
                           0x48, 0x89, 0x21, 0x48, 0x89, 0xd4})); // context save/switch
    assert(contains_bytes(result.image,
                          {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41,
                           0x5c, 0x5e, 0x5f, 0x5d, 0x5b, 0xc3})); // context restore
    assert(contains_bytes(result.image,
                          {0xfc, 0x4c, 0x89, 0xe9, 0x48, 0x83, 0xec,
                           0x20, 0x41, 0xff, 0xd4})); // task bootstrap entry
    assert(contains_bytes(result.image,
                          {0x48, 0x83, 0xe0, 0xf0,
                           0x48, 0x83, 0xe8, 0x48})); // initial 72-byte frame
    assert(contains_bytes(result.image,
                          {0x48, 0x83, 0xe0, 0xf0,
                           0x48, 0x2d, 0x98, 0x00, 0x00, 0x00})); // preempt frame
    assert(contains_bytes(result.image,
                          {0x48, 0x83, 0xe0, 0xf0,
                           0x48, 0x3d, 0xa8, 0x00, 0x00, 0x00,
                           0x0f, 0x82})); // checked 168-byte user frame
    assert(contains_bytes(result.image,
                          {0x49, 0x89, 0x82, 0x98, 0x00, 0x00, 0x00})); // user RSP
    assert(contains_bytes(result.image,
                          {0xf7, 0x81, 0x90, 0x00, 0x00, 0x00,
                           0x00, 0x70, 0x02, 0x00})); // reject IOPL/NT/VM
    assert(contains_bytes(result.image,
                          {0xfa, 0x4c, 0x89, 0xd4,
                           0x41, 0x5f, 0x41, 0x5e})); // checked frame resume
    assert(contains_bytes(result.image,
                          {0x48, 0x83, 0xc4, 0x08,
                           0x0f, 0x01, 0xf8,
                           0x0f, 0xae, 0xe8,
                           0x48, 0xcf})); // checked ring-3 resume
    assert(contains_bytes(result.image,
                          {0x41, 0xff, 0xd3})); // indirect Win64 call
    assert(contains_bytes(result.image,
                          {0xcd, 0x80, 0x5e, 0x5f})); // int 0x80 and restore
    assert(contains_bytes(result.image,
                          {0x48, 0x8b, 0xbd})); // syscall argument 0 -> rdi
    assert(contains_bytes(result.image,
                          {0x48, 0x8b, 0xb5})); // syscall argument 1 -> rsi
    assert(contains_bytes(result.image,
                          {0x0f, 0x05, 0x5e, 0x5f})); // syscall and restore
    assert(contains_bytes(result.image,
                          {0xb9, 0x82, 0x00, 0x00, 0xc0,
                           0x0f, 0x30})); // IA32_LSTAR
    assert(contains_bytes(result.image,
                          {0x65, 0x48, 0x89, 0x24, 0x25,
                           0x08, 0x00, 0x00, 0x00})); // save user RSP
    assert(contains_bytes(result.image,
                          {0x65, 0x48, 0x8b, 0x24, 0x25,
                           0x00, 0x00, 0x00, 0x00})); // load kernel RSP
    assert(contains_bytes(result.image,
                          {0x48, 0x0f, 0x07})); // sysretq
    assert(contains_bytes(result.image,
                          {0x68, 0x1b, 0x00, 0x00, 0x00,
                           0x41, 0x57,
                           0x68, 0x02, 0x02, 0x00, 0x00,
                           0x68, 0x23, 0x00, 0x00, 0x00,
                           0x41, 0x54,
                           0xfa,
                           0x0f, 0x01, 0xf8,
                           0x48, 0xcf})); // ring-3 IRET frame

    const uint32_t pe = read_u32(result.image, 0x3c);
    assert(result.image.at(pe) == 'P' && result.image.at(pe + 1) == 'E');
    assert(read_u16(result.image, pe + 4) == 0x8664);
    assert(read_u16(result.image, pe + 6) == 3);
    const size_t optional = pe + 4 + 20;
    assert(read_u16(result.image, optional) == 0x20b);
    assert(read_u16(result.image, optional + 68) == 10);
    assert(read_u32(result.image, optional + 16) >= 0x1000);

    SuraUefiDiskResult disk = sura_build_uefi_disk_image(result.image);
    assert(disk.image.size() >= 64U * 1024U * 1024U);
    assert(disk.image[510] == 0x55 && disk.image[511] == 0xaa);
    assert(disk.image[446 + 4] == 0xee);
    assert(std::memcmp(disk.image.data() + 512, "EFI PART", 8) == 0);
    const uint64_t partition_lba = read_u64(disk.image, 2 * 512 + 32);
    assert(partition_lba == 2048);
    const size_t partition = static_cast<size_t>(partition_lba * 512);
    assert(std::memcmp(disk.image.data() + partition + 82, "FAT32   ", 8) == 0);
    assert(disk.image[partition + 510] == 0x55 &&
           disk.image[partition + 511] == 0xaa);
    const uint32_t reserved = read_u16(disk.image, partition + 14);
    const uint32_t fats = disk.image[partition + 16];
    const uint32_t fat_sectors = read_u32(disk.image, partition + 36);
    const uint64_t data_lba =
        partition_lba + reserved + static_cast<uint64_t>(fats) * fat_sectors;
    const auto cluster_offset = [&](uint32_t cluster) {
        return static_cast<size_t>((data_lba + cluster - 2) * 512);
    };
    assert(std::memcmp(disk.image.data() + cluster_offset(2),
                       "EFI        ", 11) == 0);
    assert(std::memcmp(disk.image.data() + cluster_offset(3) + 64,
                       "BOOT       ", 11) == 0);
    assert(std::memcmp(disk.image.data() + cluster_offset(4) + 64,
                       "BOOTX64 EFI", 11) == 0);
    assert(read_u32(disk.image, cluster_offset(4) + 64 + 28) ==
           result.image.size());
    assert(std::equal(result.image.begin(), result.image.end(),
                      disk.image.begin() +
                          static_cast<std::ptrdiff_t>(cluster_offset(5))));
    assert(std::memcmp(disk.image.data() + cluster_offset(2) + 32,
                       "NOTES   TXT", 11) == 0);
    assert(std::memcmp(disk.image.data() + cluster_offset(2) + 64,
                       "SETTINGSCFG", 11) == 0);
    assert(std::memcmp(disk.image.data() + cluster_offset(2) + 96,
                       "DESKTOP CFG", 11) == 0);
    assert(std::memcmp(disk.image.data() + cluster_offset(2) + 128,
                       "DOCS       ", 11) == 0);
    assert(std::memcmp(
               disk.image.data() + cluster_offset(disk.notes_first_cluster),
               "Welcome to Sura OS Notes.", 25) == 0);
    assert(std::memcmp(
               disk.image.data() + cluster_offset(disk.settings_first_cluster),
               "version=1\r\naccent=orange\r\n", 26) == 0);
    assert(std::memcmp(
               disk.image.data() + cluster_offset(disk.desktop_first_cluster),
               "version=1\r\nwallpaper=default\r\n", 30) == 0);
    const uint32_t docs_cluster = disk.notes_first_cluster + 3;
    assert(std::memcmp(disk.image.data() + cluster_offset(docs_cluster) + 64,
                       "README  TXT", 11) == 0);

    if (argc > 1) {
        std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(result.image.data()),
                  static_cast<std::streamsize>(result.image.size()));
        assert(out.good());
    }

    std::cout << "os_target_unit: PASS ("
              << result.machine_code_bytes << " code bytes, "
              << result.image.size() << " image bytes)\n";
    return 0;
}
