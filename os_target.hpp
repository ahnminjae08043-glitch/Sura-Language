#ifndef SURA_OS_TARGET_H
#define SURA_OS_TARGET_H

#include "ast.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Freestanding Sura backend.
//
// This backend deliberately does not depend on the Sura VM, GC, C runtime, or
// an external assembler/linker. It lowers a systems-oriented Sura subset to a
// position-independent PE32+ EFI application whose entry point follows the
// Microsoft x64 ABI required by UEFI.
//
// The first public target is `uefi-x86_64`. It is intentionally separate from
// the hosted VM/JIT pipeline: privileged memory/CPU intrinsics must never gain
// accidental meaning in an ordinary Sura process.

struct SuraOsCompileError : std::runtime_error {
    int line;
    explicit SuraOsCompileError(const std::string& message, int source_line = 0)
        : std::runtime_error(message), line(source_line) {}
};

struct SuraOsCompileResult {
    std::vector<uint8_t> image;
    std::string target;
    std::string entry_function;
    size_t machine_code_bytes = 0;
    size_t data_bytes = 0;
};

namespace SuraOsTarget {

inline uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

inline void put_u16(std::vector<uint8_t>& out, size_t offset, uint16_t value) {
    if (out.size() < offset + 2) out.resize(offset + 2, 0);
    out[offset] = static_cast<uint8_t>(value);
    out[offset + 1] = static_cast<uint8_t>(value >> 8);
}

inline void put_u32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
    if (out.size() < offset + 4) out.resize(offset + 4, 0);
    for (unsigned i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

inline void put_u64(std::vector<uint8_t>& out, size_t offset, uint64_t value) {
    if (out.size() < offset + 8) out.resize(offset + 8, 0);
    for (unsigned i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

inline std::vector<uint16_t> utf8_to_utf16(const std::string& text) {
    std::vector<uint16_t> out;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0xfffd;
        const uint8_t first = static_cast<uint8_t>(text[i]);
        size_t count = 1;
        if (first < 0x80) {
            cp = first;
        } else if ((first & 0xe0) == 0xc0 && i + 1 < text.size()) {
            cp = first & 0x1f;
            count = 2;
        } else if ((first & 0xf0) == 0xe0 && i + 2 < text.size()) {
            cp = first & 0x0f;
            count = 3;
        } else if ((first & 0xf8) == 0xf0 && i + 3 < text.size()) {
            cp = first & 0x07;
            count = 4;
        }
        bool valid = count > 1;
        for (size_t j = 1; j < count && valid; ++j) {
            const uint8_t ch = static_cast<uint8_t>(text[i + j]);
            if ((ch & 0xc0) != 0x80) valid = false;
            else cp = (cp << 6) | (ch & 0x3f);
        }
        if (!valid && count > 1) {
            cp = 0xfffd;
            count = 1;
        }
        i += count;
        if (cp <= 0xffff) {
            if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
            out.push_back(static_cast<uint16_t>(cp));
        } else if (cp <= 0x10ffff) {
            cp -= 0x10000;
            out.push_back(static_cast<uint16_t>(0xd800 + (cp >> 10)));
            out.push_back(static_cast<uint16_t>(0xdc00 + (cp & 0x3ff)));
        } else {
            out.push_back(0xfffd);
        }
    }
    return out;
}

class X64Emitter {
public:
    std::vector<uint8_t> code;

    void b(uint8_t value) { code.push_back(value); }
    void bytes(std::initializer_list<uint8_t> values) {
        code.insert(code.end(), values.begin(), values.end());
    }
    void d(uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) b(static_cast<uint8_t>(value >> (i * 8)));
    }
    void q(uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) b(static_cast<uint8_t>(value >> (i * 8)));
    }
    size_t pos() const { return code.size(); }

    size_t rel32(std::initializer_list<uint8_t> prefix) {
        bytes(prefix);
        const size_t at = pos();
        d(0);
        return at;
    }

    void patch_rel32(size_t displacement_offset, size_t target_offset) {
        const int64_t delta = static_cast<int64_t>(target_offset) -
                              static_cast<int64_t>(displacement_offset + 4);
        if (delta < std::numeric_limits<int32_t>::min() ||
            delta > std::numeric_limits<int32_t>::max()) {
            throw SuraOsCompileError("x86-64 relative branch is out of range");
        }
        const uint32_t encoded = static_cast<uint32_t>(static_cast<int32_t>(delta));
        for (unsigned i = 0; i < 4; ++i) {
            code[displacement_offset + i] =
                static_cast<uint8_t>(encoded >> (i * 8));
        }
    }

    void mov_rax_imm(uint64_t value) {
        bytes({0x48, 0xb8});
        q(value);
    }
    void mov_rax_rbp(int32_t disp) {
        bytes({0x48, 0x8b, 0x85});
        d(static_cast<uint32_t>(disp));
    }
    void mov_rbp_rax(int32_t disp) {
        bytes({0x48, 0x89, 0x85});
        d(static_cast<uint32_t>(disp));
    }
    void mov_rbp_reg(int32_t disp, unsigned reg) {
        switch (reg) {
            case 1: bytes({0x48, 0x89, 0x8d}); break; // rcx
            case 2: bytes({0x48, 0x89, 0x95}); break; // rdx
            case 8: bytes({0x4c, 0x89, 0x85}); break; // r8
            case 9: bytes({0x4c, 0x89, 0x8d}); break; // r9
            default: throw SuraOsCompileError("unsupported x64 parameter register");
        }
        d(static_cast<uint32_t>(disp));
    }
    void mov_reg_rbp(unsigned reg, int32_t disp) {
        switch (reg) {
            case 1: bytes({0x48, 0x8b, 0x8d}); break;
            case 2: bytes({0x48, 0x8b, 0x95}); break;
            case 8: bytes({0x4c, 0x8b, 0x85}); break;
            case 9: bytes({0x4c, 0x8b, 0x8d}); break;
            default: throw SuraOsCompileError("unsupported x64 parameter register");
        }
        d(static_cast<uint32_t>(disp));
    }
};

struct DataPatch {
    size_t displacement_offset;
    size_t data_offset;
};

struct CallPatch {
    size_t displacement_offset;
    std::string function;
    int line;
};

struct LoopPatchState {
    size_t continue_target = 0;
    std::vector<size_t> breaks;
    std::vector<size_t> continues;
};

class UefiX64Compiler {
    static constexpr uint32_t text_rva = 0x1000;
    static constexpr size_t image_handle_data_offset = 0;
    static constexpr size_t system_table_data_offset = 8;

    X64Emitter x;
    std::vector<uint8_t> data = std::vector<uint8_t>(16, 0);
    std::vector<DataPatch> data_patches;
    std::vector<CallPatch> call_patches;
    std::unordered_map<std::string, size_t> function_offsets;
    std::unordered_map<std::string, const FuncDef*> functions;
    std::vector<LoopPatchState> loops;
    std::vector<size_t> return_patches;
    std::unordered_map<std::string, int32_t> slots;
    std::unordered_map<const Stmt*, int32_t> repeat_slots;
    std::vector<int32_t> scratch_slots;
    size_t gop_guid_data_offset = std::numeric_limits<size_t>::max();
    size_t next_slot = 0;
    uint32_t frame_size = 0;
    const FuncDef* current_function = nullptr;
    bool current_is_entry = false;

    [[noreturn]] static void fail(const Node* node, const std::string& message) {
        throw SuraOsCompileError(message, node ? node->line : 0);
    }

    int32_t allocate_slot(const std::string& name) {
        auto found = slots.find(name);
        if (found != slots.end()) return found->second;
        const int32_t disp = -static_cast<int32_t>((++next_slot) * 8);
        slots.emplace(name, disp);
        return disp;
    }

    int32_t require_slot(const Ident* ident) const {
        auto found = slots.find(ident->name);
        if (found == slots.end()) {
            fail(ident, "unknown freestanding local '" + ident->name + "'");
        }
        return found->second;
    }

    void scan_locals(const SuraBlock* block) {
        if (!block) return;
        for (const auto& holder : block->body) {
            const Stmt* stmt = holder.get();
            if (!stmt) continue;
            switch (stmt->kind) {
                case NK::ASSIGN:
                    allocate_slot(static_cast<const AssignStmt*>(stmt)->name);
                    break;
                case NK::IF: {
                    auto* value = static_cast<const IfStmt*>(stmt);
                    scan_locals(value->then_block.get());
                    scan_locals(value->else_block.get());
                    break;
                }
                case NK::WHILE:
                    scan_locals(static_cast<const WhileStmt*>(stmt)->body.get());
                    break;
                case NK::REPEAT: {
                    repeat_slots[stmt] = allocate_slot(
                        "__sura_repeat_" + std::to_string(repeat_slots.size()));
                    scan_locals(static_cast<const RepeatStmt*>(stmt)->body.get());
                    break;
                }
                default:
                    break;
            }
        }
    }

    size_t add_utf16_string(const std::string& value) {
        if (data.size() & 1U) data.push_back(0);
        const size_t offset = data.size();
        for (uint16_t unit : utf8_to_utf16(value)) {
            data.push_back(static_cast<uint8_t>(unit));
            data.push_back(static_cast<uint8_t>(unit >> 8));
        }
        data.push_back(0);
        data.push_back(0);
        return offset;
    }

    size_t add_data_bytes(std::initializer_list<uint8_t> bytes,
                          size_t alignment = 1) {
        while (data.size() % alignment) data.push_back(0);
        const size_t offset = data.size();
        data.insert(data.end(), bytes.begin(), bytes.end());
        return offset;
    }

    size_t ensure_gop_guid() {
        if (gop_guid_data_offset != std::numeric_limits<size_t>::max()) {
            return gop_guid_data_offset;
        }
        // EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID
        // 9042a9de-23dc-4a38-96fb-7aded080516a, encoded in EFI_GUID layout.
        gop_guid_data_offset = add_data_bytes(
            {0xde, 0xa9, 0x42, 0x90, 0xdc, 0x23, 0x38, 0x4a,
             0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a},
            8);
        return gop_guid_data_offset;
    }

    size_t rip_data_disp(std::initializer_list<uint8_t> prefix,
                         size_t data_offset) {
        const size_t at = x.rel32(prefix);
        data_patches.push_back({at, data_offset});
        return at;
    }

    void load_system_table_rax() {
        rip_data_disp({0x48, 0x8b, 0x05}, system_table_data_offset);
    }

    void load_image_handle_rax() {
        rip_data_disp({0x48, 0x8b, 0x05}, image_handle_data_offset);
    }

    void address_of_data_rax(size_t data_offset) {
        rip_data_disp({0x48, 0x8d, 0x05}, data_offset);
    }

    static bool integral_literal(const Expr* expr, uint64_t& out) {
        if (!expr || expr->kind != NK::NUM_LIT) return false;
        const double value = static_cast<const NumLit*>(expr)->value;
        if (!std::isfinite(value) || std::floor(value) != value ||
            value < -9007199254740991.0 || value > 9007199254740991.0) {
            return false;
        }
        out = static_cast<uint64_t>(static_cast<int64_t>(value));
        return true;
    }

    static bool parse_integer_text(const std::string& text, uint64_t& out) {
        if (text.empty()) return false;
        size_t index = 0;
        unsigned base = 10;
        if (text.size() > 2 && text[0] == '0' &&
            (text[1] == 'x' || text[1] == 'X')) {
            base = 16;
            index = 2;
        } else if (text.size() > 2 && text[0] == '0' &&
                   (text[1] == 'b' || text[1] == 'B')) {
            base = 2;
            index = 2;
        }
        uint64_t value = 0;
        bool digit_seen = false;
        for (; index < text.size(); ++index) {
            const char ch = text[index];
            if (ch == '_') continue;
            unsigned digit = 99;
            if (ch >= '0' && ch <= '9') digit = static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') digit = 10U + static_cast<unsigned>(ch - 'a');
            else if (ch >= 'A' && ch <= 'F') digit = 10U + static_cast<unsigned>(ch - 'A');
            if (digit >= base ||
                value > (std::numeric_limits<uint64_t>::max() - digit) / base) {
                return false;
            }
            value = value * base + digit;
            digit_seen = true;
        }
        if (!digit_seen) return false;
        out = value;
        return true;
    }

    bool flatten_call(const Expr* expr, std::string& name,
                      const std::vector<ExprPtr>*& args) const {
        if (expr->kind == NK::CALL) {
            auto* call = static_cast<const CallExpr*>(expr);
            name = call->name;
            args = &call->args;
            return true;
        }
        if (expr->kind == NK::METHOD_CALL) {
            auto* call = static_cast<const MethodCallExpr*>(expr);
            if (call->obj && call->obj->kind == NK::IDENT) {
                name = static_cast<const Ident*>(call->obj.get())->name + "." + call->method;
                args = &call->args;
                return true;
            }
        }
        return false;
    }

    static std::string canonical_intrinsic(std::string name) {
        std::replace(name.begin(), name.end(), '.', '_');
        return name;
    }

    void compile_expr(const Expr* expr) {
        if (!expr) {
            x.bytes({0x31, 0xc0}); // xor eax,eax
            return;
        }
        switch (expr->kind) {
            case NK::NUM_LIT: {
                uint64_t value = 0;
                if (!integral_literal(expr, value)) {
                    fail(expr, "freestanding numeric literals must be exact integers; "
                               "use u64(\"0x...\") for full-width constants");
                }
                x.mov_rax_imm(value);
                return;
            }
            case NK::BOOL_LIT:
                x.mov_rax_imm(static_cast<const BoolLit*>(expr)->value ? 1 : 0);
                return;
            case NK::NIL_LIT:
                x.bytes({0x31, 0xc0});
                return;
            case NK::IDENT:
                x.mov_rax_rbp(require_slot(static_cast<const Ident*>(expr)));
                return;
            case NK::UNARY_OP: {
                auto* unary = static_cast<const UnaryOp*>(expr);
                compile_expr(unary->operand.get());
                if (unary->op == "-") x.bytes({0x48, 0xf7, 0xd8});
                else if (unary->op == "~") x.bytes({0x48, 0xf7, 0xd0});
                else if (unary->op == "not" || unary->op == "!") {
                    x.bytes({0x48, 0x85, 0xc0, 0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xc0});
                } else {
                    fail(expr, "unsupported freestanding unary operator '" + unary->op + "'");
                }
                return;
            }
            case NK::BIN_OP: {
                auto* binary = static_cast<const BinOp*>(expr);
                compile_expr(binary->left.get());
                x.b(0x50); // push rax
                compile_expr(binary->right.get());
                x.b(0x59); // pop rcx (left)
                const std::string& op = binary->op;
                if (op == "+") x.bytes({0x48, 0x01, 0xc1, 0x48, 0x89, 0xc8});
                else if (op == "-") x.bytes({0x48, 0x29, 0xc1, 0x48, 0x89, 0xc8});
                else if (op == "*") x.bytes({0x48, 0x0f, 0xaf, 0xc8, 0x48, 0x89, 0xc8});
                else if (op == "&" || op == "and") x.bytes({0x48, 0x21, 0xc1, 0x48, 0x89, 0xc8});
                else if (op == "|" || op == "or") x.bytes({0x48, 0x09, 0xc1, 0x48, 0x89, 0xc8});
                else if (op == "^") x.bytes({0x48, 0x31, 0xc1, 0x48, 0x89, 0xc8});
                else if (op == "/" || op == "%") {
                    x.bytes({0x49, 0x89, 0xc2, 0x48, 0x89, 0xc8, 0x48, 0x99,
                             0x49, 0xf7, 0xfa});
                    if (op == "%") x.bytes({0x48, 0x89, 0xd0});
                } else if (op == "<<" || op == ">>") {
                    x.bytes({0x49, 0x89, 0xca, 0x48, 0x89, 0xc1, 0x4c, 0x89, 0xd0});
                    if (op == "<<") x.bytes({0x48, 0xd3, 0xe0});
                    else x.bytes({0x48, 0xd3, 0xe8});
                } else {
                    x.bytes({0x48, 0x39, 0xc1}); // cmp rcx,rax
                    if (op == "==") x.bytes({0x0f, 0x94, 0xc0});
                    else if (op == "!=") x.bytes({0x0f, 0x95, 0xc0});
                    else if (op == "<") x.bytes({0x0f, 0x9c, 0xc0});
                    else if (op == "<=") x.bytes({0x0f, 0x9e, 0xc0});
                    else if (op == ">") x.bytes({0x0f, 0x9f, 0xc0});
                    else if (op == ">=") x.bytes({0x0f, 0x9d, 0xc0});
                    else fail(expr, "unsupported freestanding binary operator '" + op + "'");
                    x.bytes({0x0f, 0xb6, 0xc0});
                }
                return;
            }
            case NK::CALL:
            case NK::METHOD_CALL:
                compile_call_expr(expr);
                return;
            default:
                fail(expr, "expression is not available in the freestanding OS target");
        }
    }

    void save_call_args(const std::vector<ExprPtr>& args) {
        if (args.size() > scratch_slots.size()) {
            fail(current_function, "freestanding calls support at most " +
                 std::to_string(scratch_slots.size()) + " arguments");
        }
        for (size_t i = 0; i < args.size(); ++i) {
            compile_expr(args[i].get());
            x.mov_rbp_rax(scratch_slots[i]);
        }
    }

    void load_call_args(size_t count) {
        if (count > 0) x.mov_reg_rbp(1, scratch_slots[0]);
        if (count > 1) x.mov_reg_rbp(2, scratch_slots[1]);
        if (count > 2) x.mov_reg_rbp(8, scratch_slots[2]);
        if (count > 3) x.mov_reg_rbp(9, scratch_slots[3]);
        for (size_t i = 4; i < count; ++i) {
            x.mov_rax_rbp(scratch_slots[i]);
            x.bytes({0x48, 0x89, 0x84, 0x24});
            x.d(static_cast<uint32_t>(32 + (i - 4) * 8));
        }
    }

    void compile_uefi_service(const Expr* origin, const std::vector<ExprPtr>& args,
                              uint32_t table_offset, uint32_t function_offset,
                              size_t min_args, size_t max_args) {
        if (args.size() < min_args || args.size() > max_args) {
            fail(origin, "UEFI intrinsic expects " + std::to_string(min_args) +
                         (min_args == max_args ? "" : ".." + std::to_string(max_args)) +
                         " argument(s)");
        }
        save_call_args(args);
        load_system_table_rax();
        x.bytes({0x48, 0x8b, 0x80});
        x.d(table_offset);
        x.bytes({0x4c, 0x8b, 0x98});
        x.d(function_offset);
        load_call_args(args.size());
        x.bytes({0x41, 0xff, 0xd3}); // call r11
    }

    void compile_call_expr(const Expr* origin) {
        std::string raw_name;
        const std::vector<ExprPtr>* args_ptr = nullptr;
        if (!flatten_call(origin, raw_name, args_ptr)) {
            fail(origin, "unsupported freestanding call target");
        }
        const std::string name = canonical_intrinsic(raw_name);
        const auto& args = *args_ptr;

        if ((name == "u64" || name == "ptr" || name == "usize") &&
            args.size() == 1 && args[0]->kind == NK::STR_LIT) {
            uint64_t value = 0;
            if (!parse_integer_text(static_cast<const StrLit*>(args[0].get())->value, value)) {
                fail(origin, name + "() requires a decimal, hexadecimal, or binary integer string");
            }
            x.mov_rax_imm(value);
            return;
        }

        if (name == "addr_of") {
            if (args.size() != 1 || args[0]->kind != NK::IDENT) {
                fail(origin, "addr_of() requires one local variable name");
            }
            const int32_t disp =
                require_slot(static_cast<const Ident*>(args[0].get()));
            x.bytes({0x48, 0x8d, 0x85});
            x.d(static_cast<uint32_t>(disp));
            return;
        }

        if (name == "uefi_system_table") {
            if (!args.empty()) fail(origin, "uefi.system_table() takes no arguments");
            load_system_table_rax();
            return;
        }
        if (name == "uefi_image_handle") {
            if (!args.empty()) fail(origin, "uefi.image_handle() takes no arguments");
            load_image_handle_rax();
            return;
        }

        const auto memory_read = [&](unsigned width) {
            if (args.size() != 1) fail(origin, "memory read expects one address");
            compile_expr(args[0].get());
            if (width == 8) x.bytes({0x0f, 0xb6, 0x00});
            else if (width == 16) x.bytes({0x0f, 0xb7, 0x00});
            else if (width == 32) x.bytes({0x8b, 0x00});
            else x.bytes({0x48, 0x8b, 0x00});
        };
        if (name == "mem_read8") { memory_read(8); return; }
        if (name == "mem_read16") { memory_read(16); return; }
        if (name == "mem_read32") { memory_read(32); return; }
        if (name == "mem_read64") { memory_read(64); return; }

        const auto port_read = [&](unsigned width) {
            if (args.size() != 1) fail(origin, "port input expects one port");
            compile_expr(args[0].get());
            x.bytes({0x66, 0x89, 0xc2, 0x31, 0xc0}); // mov dx,ax; xor eax,eax
            if (width == 8) x.b(0xec);
            else if (width == 16) x.bytes({0x66, 0xed});
            else x.b(0xed);
        };
        if (name == "io_in8") { port_read(8); return; }
        if (name == "io_in16") { port_read(16); return; }
        if (name == "io_in32") { port_read(32); return; }

        if (name == "cpu_read_cr0") { x.bytes({0x0f, 0x20, 0xc0}); return; }
        if (name == "cpu_read_cr2") { x.bytes({0x0f, 0x20, 0xd0}); return; }
        if (name == "cpu_read_cr3") { x.bytes({0x0f, 0x20, 0xd8}); return; }
        if (name == "cpu_read_cr4") { x.bytes({0x0f, 0x20, 0xe0}); return; }
        if (name == "cpu_read_flags") { x.bytes({0x9c, 0x58}); return; }

        if (name == "cpu_read_msr") {
            if (args.size() != 1) fail(origin, "cpu.read_msr() expects an MSR index");
            compile_expr(args[0].get());
            x.bytes({0x89, 0xc1, 0x0f, 0x32, 0x48, 0xc1, 0xe2, 0x20,
                     0x48, 0x09, 0xd0});
            return;
        }

        if (name == "uefi_get_memory_map") {
            compile_uefi_service(origin, args, 96, 56, 5, 5);
            return;
        }
        if (name == "uefi_allocate_pages") {
            compile_uefi_service(origin, args, 96, 40, 4, 4);
            return;
        }
        if (name == "uefi_free_pages") {
            compile_uefi_service(origin, args, 96, 48, 2, 2);
            return;
        }
        if (name == "uefi_allocate_pool") {
            compile_uefi_service(origin, args, 96, 64, 3, 3);
            return;
        }
        if (name == "uefi_free_pool") {
            compile_uefi_service(origin, args, 96, 72, 1, 1);
            return;
        }
        if (name == "uefi_locate_protocol") {
            compile_uefi_service(origin, args, 96, 320, 3, 3);
            return;
        }
        if (name == "uefi_exit_boot_services") {
            if (args.size() != 1) fail(origin, "uefi.exit_boot_services() expects a map key");
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[1]);
            load_image_handle_rax();
            x.mov_rbp_rax(scratch_slots[0]);
            load_system_table_rax();
            x.bytes({0x48, 0x8b, 0x40, 0x60, 0x4c, 0x8b, 0x98});
            x.d(232);
            load_call_args(2);
            x.bytes({0x41, 0xff, 0xd3});
            return;
        }

        if (name == "uefi_gop_framebuffer" ||
            name == "uefi_gop_framebuffer_size" ||
            name == "uefi_gop_width" || name == "uefi_gop_height" ||
            name == "uefi_gop_stride" || name == "uefi_gop_pixel_format") {
            if (!args.empty()) fail(origin, raw_name + "() takes no arguments");

            // LocateProtocol(&GOP_GUID, nil, &gop). The last scratch slot is
            // stable addressable storage for the returned interface pointer.
            x.bytes({0x31, 0xc0});
            x.mov_rbp_rax(scratch_slots[5]);
            address_of_data_rax(ensure_gop_guid());
            x.mov_rbp_rax(scratch_slots[0]);
            x.bytes({0x31, 0xc0});
            x.mov_rbp_rax(scratch_slots[1]);
            x.bytes({0x48, 0x8d, 0x85});
            x.d(static_cast<uint32_t>(scratch_slots[5]));
            x.mov_rbp_rax(scratch_slots[2]);

            load_system_table_rax();
            x.bytes({0x48, 0x8b, 0x40, 0x60, 0x4c, 0x8b, 0x98});
            x.d(320);
            load_call_args(3);
            x.bytes({0x41, 0xff, 0xd3, 0x48, 0x85, 0xc0});
            const size_t status_failure = x.rel32({0x0f, 0x85});
            x.mov_rax_rbp(scratch_slots[5]);
            x.bytes({0x48, 0x85, 0xc0});
            const size_t null_failure = x.rel32({0x0f, 0x84});

            x.bytes({0x48, 0x8b, 0x40, 0x18}); // GOP->Mode
            x.bytes({0x48, 0x85, 0xc0});
            const size_t mode_failure = x.rel32({0x0f, 0x84});
            if (name == "uefi_gop_framebuffer") {
                x.bytes({0x48, 0x8b, 0x40, 0x18});
            } else if (name == "uefi_gop_framebuffer_size") {
                x.bytes({0x48, 0x8b, 0x40, 0x20});
            } else {
                x.bytes({0x48, 0x8b, 0x40, 0x08}); // Mode->Info
                x.bytes({0x48, 0x85, 0xc0});
                const size_t info_failure = x.rel32({0x0f, 0x84});
                if (name == "uefi_gop_width") x.bytes({0x8b, 0x40, 0x04});
                else if (name == "uefi_gop_height") x.bytes({0x8b, 0x40, 0x08});
                else if (name == "uefi_gop_pixel_format") x.bytes({0x8b, 0x40, 0x0c});
                else x.bytes({0x8b, 0x40, 0x20});
                const size_t success = x.rel32({0xe9});
                const size_t failure = x.pos();
                x.patch_rel32(info_failure, failure);
                x.bytes({0x31, 0xc0});
                x.patch_rel32(success, x.pos());
                const size_t end = x.pos();
                x.patch_rel32(status_failure, failure);
                x.patch_rel32(null_failure, failure);
                x.patch_rel32(mode_failure, failure);
                (void)end;
                return;
            }
            const size_t success = x.rel32({0xe9});
            const size_t failure = x.pos();
            x.patch_rel32(status_failure, failure);
            x.patch_rel32(null_failure, failure);
            x.patch_rel32(mode_failure, failure);
            x.bytes({0x31, 0xc0});
            x.patch_rel32(success, x.pos());
            return;
        }

        auto user = functions.find(raw_name);
        if (user != functions.end()) {
            save_call_args(args);
            load_call_args(args.size());
            const size_t patch = x.rel32({0xe8});
            call_patches.push_back({patch, raw_name, origin->line});
            return;
        }
        fail(origin, "unknown freestanding intrinsic or function '" + raw_name + "'");
    }

    void call_conout(uint32_t function_offset) {
        load_system_table_rax();
        x.bytes({0x48, 0x8b, 0x48, 0x40}); // rcx = SystemTable->ConOut
        x.bytes({0x48, 0x8b, 0x81});
        x.d(function_offset);
        x.bytes({0xff, 0xd0});
    }

    void emit_uefi_write(const Expr* origin, const std::vector<ExprPtr>& args) {
        if (args.size() != 1 || args[0]->kind != NK::STR_LIT) {
            fail(origin, "uefi.write() currently requires one string literal");
        }
        const size_t string_offset =
            add_utf16_string(static_cast<const StrLit*>(args[0].get())->value);
        load_system_table_rax();
        x.bytes({0x48, 0x8b, 0x48, 0x40});
        x.bytes({0x48, 0x8b, 0x41, 0x08});
        rip_data_disp({0x48, 0x8d, 0x15}, string_offset);
        x.bytes({0xff, 0xd0});
    }

    bool compile_intrinsic_statement(const Expr* expr) {
        std::string raw_name;
        const std::vector<ExprPtr>* args_ptr = nullptr;
        if (!flatten_call(expr, raw_name, args_ptr)) return false;
        const std::string name = canonical_intrinsic(raw_name);
        const auto& args = *args_ptr;

        if (name == "uefi_write") {
            emit_uefi_write(expr, args);
            return true;
        }
        if (name == "uefi_newline") {
            if (!args.empty()) fail(expr, "uefi.newline() takes no arguments");
            std::vector<ExprPtr> synthetic;
            const size_t string_offset = add_utf16_string("\r\n");
            load_system_table_rax();
            x.bytes({0x48, 0x8b, 0x48, 0x40, 0x48, 0x8b, 0x41, 0x08});
            rip_data_disp({0x48, 0x8d, 0x15}, string_offset);
            x.bytes({0xff, 0xd0});
            return true;
        }
        if (name == "uefi_clear") {
            if (!args.empty()) fail(expr, "uefi.clear() takes no arguments");
            call_conout(48);
            return true;
        }
        if (name == "uefi_set_color") {
            if (args.size() != 2) fail(expr, "uefi.set_color(foreground, background) expects two values");
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[0]);
            compile_expr(args[1].get());
            x.bytes({0x48, 0xc1, 0xe0, 0x04});
            x.mov_rax_rbp(scratch_slots[0]);
            // Re-evaluate background without losing foreground.
            compile_expr(args[1].get());
            x.bytes({0x48, 0xc1, 0xe0, 0x04});
            x.bytes({0x48, 0x0b, 0x85});
            x.d(static_cast<uint32_t>(scratch_slots[0]));
            x.bytes({0x48, 0x89, 0xc2});
            load_system_table_rax();
            x.bytes({0x48, 0x8b, 0x48, 0x40, 0x48, 0x8b, 0x41, 0x28,
                     0xff, 0xd0});
            return true;
        }
        if (name == "uefi_stall") {
            compile_uefi_service(expr, args, 96, 248, 1, 1);
            return true;
        }
        if (name == "uefi_shutdown") {
            if (!args.empty()) fail(expr, "uefi.shutdown() takes no arguments");
            load_system_table_rax();
            x.bytes({0x48, 0x8b, 0x40, 0x58, 0x4c, 0x8b, 0x58, 0x68,
                     0xb9, 0x02, 0x00, 0x00, 0x00, 0x31, 0xd2,
                     0x45, 0x31, 0xc0, 0x45, 0x31, 0xc9, 0x41, 0xff, 0xd3,
                     0x0f, 0x0b});
            return true;
        }

        const auto memory_write = [&](unsigned width) {
            if (args.size() != 2) fail(expr, "memory write expects address and value");
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[0]);
            compile_expr(args[1].get());
            x.mov_reg_rbp(1, scratch_slots[0]);
            if (width == 8) x.bytes({0x88, 0x01});
            else if (width == 16) x.bytes({0x66, 0x89, 0x01});
            else if (width == 32) x.bytes({0x89, 0x01});
            else x.bytes({0x48, 0x89, 0x01});
        };
        if (name == "mem_write8") { memory_write(8); return true; }
        if (name == "mem_write16") { memory_write(16); return true; }
        if (name == "mem_write32") { memory_write(32); return true; }
        if (name == "mem_write64") { memory_write(64); return true; }

        const auto port_write = [&](unsigned width) {
            if (args.size() != 2) fail(expr, "port output expects port and value");
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[0]);
            compile_expr(args[1].get());
            x.mov_reg_rbp(2, scratch_slots[0]);
            if (width == 8) x.b(0xee);
            else if (width == 16) x.bytes({0x66, 0xef});
            else x.b(0xef);
        };
        if (name == "io_out8") { port_write(8); return true; }
        if (name == "io_out16") { port_write(16); return true; }
        if (name == "io_out32") { port_write(32); return true; }

        if (name == "cpu_halt") {
            if (!args.empty()) fail(expr, "cpu.halt() takes no arguments");
            x.b(0xf4);
            return true;
        }
        if (name == "cpu_pause") {
            if (!args.empty()) fail(expr, "cpu.pause() takes no arguments");
            x.bytes({0xf3, 0x90});
            return true;
        }
        if (name == "cpu_disable_interrupts") {
            if (!args.empty()) fail(expr, "cpu.disable_interrupts() takes no arguments");
            x.b(0xfa);
            return true;
        }
        if (name == "cpu_enable_interrupts") {
            if (!args.empty()) fail(expr, "cpu.enable_interrupts() takes no arguments");
            x.b(0xfb);
            return true;
        }
        if (name == "cpu_iret") {
            if (!args.empty()) fail(expr, "cpu.iret() takes no arguments");
            x.bytes({0x48, 0xcf});
            return true;
        }
        if (name == "cpu_load_gdt" || name == "cpu_load_idt" ||
            name == "cpu_invalidate_page") {
            if (args.size() != 1) fail(expr, name + "() expects one address");
            compile_expr(args[0].get());
            if (name == "cpu_load_gdt") x.bytes({0x0f, 0x01, 0x10});
            else if (name == "cpu_load_idt") x.bytes({0x0f, 0x01, 0x18});
            else x.bytes({0x0f, 0x01, 0x38});
            return true;
        }
        if (name == "cpu_write_cr0" || name == "cpu_write_cr3" ||
            name == "cpu_write_cr4" || name == "cpu_write_flags") {
            if (args.size() != 1) fail(expr, name + "() expects one value");
            compile_expr(args[0].get());
            if (name == "cpu_write_cr0") x.bytes({0x0f, 0x22, 0xc0});
            else if (name == "cpu_write_cr3") x.bytes({0x0f, 0x22, 0xd8});
            else if (name == "cpu_write_cr4") x.bytes({0x0f, 0x22, 0xe0});
            else x.bytes({0x50, 0x9d});
            return true;
        }
        if (name == "cpu_write_msr") {
            if (args.size() != 2) fail(expr, "cpu.write_msr(index, value) expects two values");
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[0]);
            compile_expr(args[1].get());
            x.bytes({0x48, 0x89, 0xc2, 0x48, 0xc1, 0xea, 0x20});
            x.mov_reg_rbp(1, scratch_slots[0]);
            x.bytes({0x0f, 0x30});
            return true;
        }
        return false;
    }

    void compile_block(const SuraBlock* block) {
        if (!block) return;
        for (const auto& stmt : block->body) compile_stmt(stmt.get());
    }

    void compile_stmt(const Stmt* stmt) {
        if (!stmt) return;
        switch (stmt->kind) {
            case NK::ASSIGN: {
                auto* assignment = static_cast<const AssignStmt*>(stmt);
                compile_expr(assignment->value.get());
                x.mov_rbp_rax(slots.at(assignment->name));
                return;
            }
            case NK::IN_PLACE: {
                auto* update = static_cast<const InPlaceStmt*>(stmt);
                auto found = slots.find(update->name);
                if (found == slots.end()) fail(stmt, "unknown freestanding local '" + update->name + "'");
                x.mov_rax_rbp(found->second);
                x.b(0x50);
                compile_expr(update->value.get());
                x.b(0x59);
                if (update->op == "+") x.bytes({0x48, 0x01, 0xc1});
                else if (update->op == "-") x.bytes({0x48, 0x29, 0xc1});
                else if (update->op == "&") x.bytes({0x48, 0x21, 0xc1});
                else if (update->op == "|") x.bytes({0x48, 0x09, 0xc1});
                else if (update->op == "^") x.bytes({0x48, 0x31, 0xc1});
                else fail(stmt, "unsupported freestanding in-place operator");
                x.bytes({0x48, 0x89, 0xc8});
                x.mov_rbp_rax(found->second);
                return;
            }
            case NK::EXPR_STMT: {
                const Expr* expr = static_cast<const ExprStmt*>(stmt)->expr.get();
                if (!compile_intrinsic_statement(expr)) compile_expr(expr);
                return;
            }
            case NK::CMD: {
                auto* command = static_cast<const CmdStmt*>(stmt);
                if (command->cmd != "print" && command->cmd != "print_n" &&
                    command->cmd != "print_no_nl") {
                    fail(stmt, "command '" + command->cmd + "' is hosted-only");
                }
                for (const auto& arg : command->args) {
                    if (arg->kind != NK::STR_LIT) {
                        fail(arg.get(), "freestanding print currently accepts string literals");
                    }
                    const size_t offset =
                        add_utf16_string(static_cast<const StrLit*>(arg.get())->value);
                    load_system_table_rax();
                    x.bytes({0x48, 0x8b, 0x48, 0x40, 0x48, 0x8b, 0x41, 0x08});
                    rip_data_disp({0x48, 0x8d, 0x15}, offset);
                    x.bytes({0xff, 0xd0});
                }
                if (command->cmd != "print_no_nl") {
                    const size_t offset = add_utf16_string("\r\n");
                    load_system_table_rax();
                    x.bytes({0x48, 0x8b, 0x48, 0x40, 0x48, 0x8b, 0x41, 0x08});
                    rip_data_disp({0x48, 0x8d, 0x15}, offset);
                    x.bytes({0xff, 0xd0});
                }
                return;
            }
            case NK::IF: {
                auto* branch = static_cast<const IfStmt*>(stmt);
                compile_expr(branch->cond.get());
                x.bytes({0x48, 0x85, 0xc0});
                const size_t to_else = x.rel32({0x0f, 0x84});
                compile_block(branch->then_block.get());
                const size_t to_end = x.rel32({0xe9});
                x.patch_rel32(to_else, x.pos());
                compile_block(branch->else_block.get());
                x.patch_rel32(to_end, x.pos());
                return;
            }
            case NK::WHILE: {
                auto* loop = static_cast<const WhileStmt*>(stmt);
                const size_t start = x.pos();
                compile_expr(loop->cond.get());
                x.bytes({0x48, 0x85, 0xc0});
                const size_t to_end = x.rel32({0x0f, 0x84});
                loops.push_back({start, {}, {}});
                compile_block(loop->body.get());
                for (size_t patch : loops.back().continues) x.patch_rel32(patch, start);
                const size_t back = x.rel32({0xe9});
                x.patch_rel32(back, start);
                const size_t end = x.pos();
                x.patch_rel32(to_end, end);
                for (size_t patch : loops.back().breaks) x.patch_rel32(patch, end);
                loops.pop_back();
                return;
            }
            case NK::REPEAT: {
                auto* loop = static_cast<const RepeatStmt*>(stmt);
                const int32_t slot = repeat_slots.at(stmt);
                compile_expr(loop->count.get());
                x.mov_rbp_rax(slot);
                const size_t start = x.pos();
                x.mov_rax_rbp(slot);
                x.bytes({0x48, 0x85, 0xc0});
                const size_t to_end = x.rel32({0x0f, 0x8e}); // jle
                loops.push_back({0, {}, {}});
                compile_block(loop->body.get());
                const size_t decrement = x.pos();
                loops.back().continue_target = decrement;
                for (size_t patch : loops.back().continues) x.patch_rel32(patch, decrement);
                x.bytes({0x48, 0xff, 0x8d});
                x.d(static_cast<uint32_t>(slot));
                const size_t back = x.rel32({0xe9});
                x.patch_rel32(back, start);
                const size_t end = x.pos();
                x.patch_rel32(to_end, end);
                for (size_t patch : loops.back().breaks) x.patch_rel32(patch, end);
                loops.pop_back();
                return;
            }
            case NK::BREAK:
                if (loops.empty()) fail(stmt, "break is outside a freestanding loop");
                loops.back().breaks.push_back(x.rel32({0xe9}));
                return;
            case NK::CONTINUE:
                if (loops.empty()) fail(stmt, "continue is outside a freestanding loop");
                loops.back().continues.push_back(x.rel32({0xe9}));
                return;
            case NK::RETURN: {
                auto* ret = static_cast<const ReturnStmt*>(stmt);
                compile_expr(ret->value.get());
                return_patches.push_back(x.rel32({0xe9}));
                return;
            }
            case NK::FUNC_DEF:
            case NK::USE:
            case NK::IMPORT:
            case NK::GLOBAL_DECL:
                return;
            default:
                fail(stmt, "statement is not available in the freestanding OS target");
        }
    }

    void compile_function(const std::string& name, const FuncDef* function,
                          const SuraBlock* body, bool is_entry) {
        while (x.pos() & 15U) x.b(0x90);
        function_offsets[name] = x.pos();
        current_function = function;
        current_is_entry = is_entry;
        slots.clear();
        repeat_slots.clear();
        scratch_slots.clear();
        return_patches.clear();
        next_slot = 0;

        if (function) {
            if (function->params.size() > 6) {
                fail(function, "freestanding functions support at most six parameters");
            }
            for (const std::string& param : function->params) allocate_slot(param);
        }
        scan_locals(body);
        for (unsigned i = 0; i < 6; ++i) {
            scratch_slots.push_back(allocate_slot("__sura_arg_" + std::to_string(i)));
        }

        // 32 bytes of Win64 shadow space, 16 bytes for outgoing stack args,
        // then all fixed-width locals. RBP-relative locals therefore never
        // overlap the caller-owned argument area at RSP+32.
        frame_size = align_up_u32(
            static_cast<uint32_t>(next_slot * 8 + 32 + 16), 16);

        x.bytes({0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec});
        x.d(frame_size);

        if (is_entry) {
            rip_data_disp({0x48, 0x89, 0x0d}, image_handle_data_offset);
            rip_data_disp({0x48, 0x89, 0x15}, system_table_data_offset);
        }

        if (function) {
            for (size_t i = 0; i < function->params.size(); ++i) {
                const int32_t disp = slots.at(function->params[i]);
                if (i == 0) x.mov_rbp_reg(disp, 1);
                else if (i == 1) x.mov_rbp_reg(disp, 2);
                else if (i == 2) x.mov_rbp_reg(disp, 8);
                else if (i == 3) x.mov_rbp_reg(disp, 9);
                else {
                    x.bytes({0x48, 0x8b, 0x85});
                    x.d(static_cast<uint32_t>(48 + (i - 4) * 8));
                    x.mov_rbp_rax(disp);
                }
            }
        }

        compile_block(body);
        x.bytes({0x31, 0xc0}); // implicit EFI_SUCCESS / integer zero
        const size_t epilogue = x.pos();
        for (size_t patch : return_patches) x.patch_rel32(patch, epilogue);
        x.bytes({0xc9, 0xc3}); // leave; ret
    }

    std::vector<uint8_t> build_pe(uint32_t entry_rva, uint32_t data_rva,
                                  uint32_t reloc_rva) {
        constexpr uint32_t file_alignment = 0x200;
        constexpr uint32_t section_alignment = 0x1000;
        constexpr uint32_t headers_size = 0x200;
        const uint32_t text_raw_size =
            align_up_u32(static_cast<uint32_t>(x.code.size()), file_alignment);
        const uint32_t data_raw_size =
            align_up_u32(static_cast<uint32_t>(data.size()), file_alignment);
        const uint32_t reloc_raw_size = file_alignment;
        const uint32_t text_raw = headers_size;
        const uint32_t data_raw = text_raw + text_raw_size;
        const uint32_t reloc_raw = data_raw + data_raw_size;
        const uint32_t image_size =
            align_up_u32(reloc_rva + 12, section_alignment);

        std::vector<uint8_t> pe(reloc_raw + reloc_raw_size, 0);
        pe[0] = 'M'; pe[1] = 'Z';
        put_u32(pe, 0x3c, 0x80);
        const char stub[] = "This program is a Sura UEFI image.\r\n$";
        std::memcpy(pe.data() + 0x40, stub,
                    std::min(sizeof(stub) - 1, static_cast<size_t>(0x3c)));

        size_t o = 0x80;
        pe[o++] = 'P'; pe[o++] = 'E'; pe[o++] = 0; pe[o++] = 0;
        put_u16(pe, o, 0x8664); o += 2; // IMAGE_FILE_MACHINE_AMD64
        put_u16(pe, o, 3); o += 2;
        put_u32(pe, o, 0); o += 4;
        put_u32(pe, o, 0); o += 4;
        put_u32(pe, o, 0); o += 4;
        put_u16(pe, o, 240); o += 2;
        put_u16(pe, o, 0x0022); o += 2; // executable, large-address-aware

        const size_t optional = o;
        put_u16(pe, o, 0x20b); o += 2; // PE32+
        pe[o++] = 1; pe[o++] = 0;
        put_u32(pe, o, text_raw_size); o += 4;
        put_u32(pe, o, data_raw_size + reloc_raw_size); o += 4;
        put_u32(pe, o, 0); o += 4;
        put_u32(pe, o, entry_rva); o += 4;
        put_u32(pe, o, text_rva); o += 4;
        put_u64(pe, o, 0x0000000000400000ULL); o += 8;
        put_u32(pe, o, section_alignment); o += 4;
        put_u32(pe, o, file_alignment); o += 4;
        put_u16(pe, o, 2); o += 2; put_u16(pe, o, 0); o += 2;
        put_u16(pe, o, 0); o += 2; put_u16(pe, o, 0); o += 2;
        put_u16(pe, o, 2); o += 2; put_u16(pe, o, 0); o += 2;
        put_u32(pe, o, 0); o += 4;
        put_u32(pe, o, image_size); o += 4;
        put_u32(pe, o, headers_size); o += 4;
        put_u32(pe, o, 0); o += 4;
        put_u16(pe, o, 10); o += 2; // EFI application
        put_u16(pe, o, 0x0100); o += 2; // NX compatible
        put_u64(pe, o, 0x100000); o += 8;
        put_u64(pe, o, 0x1000); o += 8;
        put_u64(pe, o, 0x100000); o += 8;
        put_u64(pe, o, 0x1000); o += 8;
        put_u32(pe, o, 0); o += 4;
        put_u32(pe, o, 16); o += 4;
        // Data directory #5: base relocations. Other directories remain zero.
        put_u32(pe, optional + 112 + 5 * 8, reloc_rva);
        put_u32(pe, optional + 112 + 5 * 8 + 4, 12);
        o = optional + 240;

        auto section = [&](const char* name, uint32_t virtual_size,
                           uint32_t virtual_address, uint32_t raw_size,
                           uint32_t raw_address, uint32_t characteristics) {
            for (unsigned i = 0; i < 8 && name[i]; ++i) pe[o + i] = name[i];
            o += 8;
            put_u32(pe, o, virtual_size); o += 4;
            put_u32(pe, o, virtual_address); o += 4;
            put_u32(pe, o, raw_size); o += 4;
            put_u32(pe, o, raw_address); o += 4;
            put_u32(pe, o, 0); o += 4;
            put_u32(pe, o, 0); o += 4;
            put_u16(pe, o, 0); o += 2;
            put_u16(pe, o, 0); o += 2;
            put_u32(pe, o, characteristics); o += 4;
        };
        section(".text", static_cast<uint32_t>(x.code.size()), text_rva,
                text_raw_size, text_raw, 0x60000020);
        section(".data", static_cast<uint32_t>(data.size()), data_rva,
                data_raw_size, data_raw, 0xc0000040);
        section(".reloc", 12, reloc_rva, reloc_raw_size, reloc_raw, 0x42000040);

        std::copy(x.code.begin(), x.code.end(), pe.begin() + text_raw);
        std::copy(data.begin(), data.end(), pe.begin() + data_raw);
        put_u32(pe, reloc_raw, text_rva);
        put_u32(pe, reloc_raw + 4, 12);
        put_u16(pe, reloc_raw + 8, 0);  // IMAGE_REL_BASED_ABSOLUTE
        put_u16(pe, reloc_raw + 10, 0);
        return pe;
    }

public:
    SuraOsCompileResult compile(const SuraBlock* root) {
        if (!root) throw SuraOsCompileError("missing Sura AST for OS target");
        functions.clear();
        for (const auto& stmt : root->body) {
            if (stmt && stmt->kind == NK::FUNC_DEF) {
                auto* function = static_cast<const FuncDef*>(stmt.get());
                if (!functions.emplace(function->name, function).second) {
                    fail(function, "duplicate freestanding function '" + function->name + "'");
                }
            }
        }

        std::string entry_name;
        const FuncDef* entry = nullptr;
        for (const char* candidate : {"efi_main", "kernel_main", "main"}) {
            auto found = functions.find(candidate);
            if (found != functions.end()) {
                entry_name = candidate;
                entry = found->second;
                break;
            }
        }
        if (!entry) {
            entry_name = "efi_main";
            compile_function(entry_name, nullptr, root, true);
        } else {
            compile_function(entry_name, entry, entry->body.get(), true);
            std::vector<std::string> names;
            names.reserve(functions.size());
            for (const auto& item : functions) {
                if (item.first != entry_name) names.push_back(item.first);
            }
            std::sort(names.begin(), names.end());
            for (const std::string& name : names) {
                compile_function(name, functions.at(name),
                                 functions.at(name)->body.get(), false);
            }
        }

        for (const CallPatch& patch : call_patches) {
            auto target = function_offsets.find(patch.function);
            if (target == function_offsets.end()) {
                throw SuraOsCompileError(
                    "unresolved freestanding function '" + patch.function + "'",
                    patch.line);
            }
            x.patch_rel32(patch.displacement_offset, target->second);
        }

        const uint32_t data_rva = align_up_u32(
            text_rva + static_cast<uint32_t>(x.code.size()), 0x1000);
        const uint32_t reloc_rva = align_up_u32(
            data_rva + static_cast<uint32_t>(data.size()), 0x1000);
        for (const DataPatch& patch : data_patches) {
            const int64_t target = static_cast<int64_t>(data_rva + patch.data_offset);
            const int64_t next = static_cast<int64_t>(
                text_rva + patch.displacement_offset + 4);
            const int64_t delta = target - next;
            if (delta < std::numeric_limits<int32_t>::min() ||
                delta > std::numeric_limits<int32_t>::max()) {
                throw SuraOsCompileError("UEFI data reference is out of range");
            }
            const uint32_t encoded =
                static_cast<uint32_t>(static_cast<int32_t>(delta));
            for (unsigned i = 0; i < 4; ++i) {
                x.code[patch.displacement_offset + i] =
                    static_cast<uint8_t>(encoded >> (i * 8));
            }
        }

        const uint32_t entry_rva =
            text_rva + static_cast<uint32_t>(function_offsets.at(entry_name));
        SuraOsCompileResult result;
        result.target = "uefi-x86_64";
        result.entry_function = entry_name;
        result.machine_code_bytes = x.code.size();
        result.data_bytes = data.size();
        result.image = build_pe(entry_rva, data_rva, reloc_rva);
        return result;
    }
};

} // namespace SuraOsTarget

inline SuraOsCompileResult sura_compile_uefi_x64(const SuraBlock* root) {
    return SuraOsTarget::UefiX64Compiler{}.compile(root);
}

#endif
