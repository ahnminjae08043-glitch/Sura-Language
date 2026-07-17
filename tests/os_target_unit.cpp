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

int main(int argc, char** argv) {
    auto root = std::make_unique<SuraBlock>(1);

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
    assert(result.machine_code_bytes > 32);
    assert(result.image.size() >= 2048);
    assert(result.image[0] == 'M' && result.image[1] == 'Z');

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
