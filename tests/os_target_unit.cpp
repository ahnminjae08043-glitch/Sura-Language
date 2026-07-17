#include "../os_target.hpp"

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

    auto entry_body = std::make_unique<SuraBlock>(1);
    entry_body->body.push_back(std::make_unique<GlobalDeclStmt>(
        std::vector<std::string>{"boot_counter"}, 1));
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
        auto size = std::make_unique<CallExpr>("sizeof", 1);
        size->args.push_back(std::make_unique<Ident>("PciHeader", 1));
        entry_body->body.push_back(std::make_unique<AssignStmt>(
            "header_size", std::move(size), 1));
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
    assert(contains_bytes(result.image, {0x0f, 0xa2})); // cpuid
    assert(contains_bytes(result.image,
                          {0xf0, 0x48, 0x0f, 0xc1, 0x01})); // lock xadd

    const uint32_t pe = read_u32(result.image, 0x3c);
    assert(result.image.at(pe) == 'P' && result.image.at(pe + 1) == 'E');
    assert(read_u16(result.image, pe + 4) == 0x8664);
    assert(read_u16(result.image, pe + 6) == 3);
    const size_t optional = pe + 4 + 20;
    assert(read_u16(result.image, optional) == 0x20b);
    assert(read_u16(result.image, optional + 68) == 10);
    assert(read_u32(result.image, optional + 16) >= 0x1000);

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
