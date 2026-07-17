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
#include <unordered_set>
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
            case 10: bytes({0x4c, 0x8b, 0x95}); break;
            case 11: bytes({0x4c, 0x8b, 0x9d}); break;
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

struct FunctionAddressPatch {
    size_t displacement_offset;
    std::string function;
    int line;
};

struct LoopPatchState {
    size_t continue_target = 0;
    std::vector<size_t> breaks;
    std::vector<size_t> continues;
};

struct FreestandingFieldLayout {
    size_t offset = 0;
    size_t size = 0;
    size_t alignment = 1;
    TypeAnnot type;
};

struct FreestandingStructLayout {
    size_t size = 0;
    size_t alignment = 1;
    bool packed = false;
    std::unordered_map<std::string, FreestandingFieldLayout> fields;
};

struct FreestandingGlobal {
    size_t data_offset = 0;
    size_t size = 0;
    unsigned scalar_width = 0;
    bool address_value = false;
    bool mutable_scalar = false;
    TypeAnnot type;
};

class UefiX64Compiler {
    static constexpr uint32_t text_rva = 0x1000;
    static constexpr size_t image_handle_data_offset = 0;
    static constexpr size_t system_table_data_offset = 8;
    static constexpr size_t temporary_slot_count = 16;
    static constexpr size_t argument_bank_width = 6;
    static constexpr size_t argument_bank_count = 8;

    X64Emitter x;
    std::vector<uint8_t> data = std::vector<uint8_t>(16, 0);
    std::vector<DataPatch> data_patches;
    std::vector<CallPatch> call_patches;
    std::vector<FunctionAddressPatch> function_address_patches;
    std::unordered_map<std::string, size_t> function_offsets;
    std::unordered_map<std::string, const FuncDef*> functions;
    std::unordered_map<std::string, const ClassDef*> struct_defs;
    std::unordered_map<std::string, FreestandingStructLayout> struct_layouts;
    std::unordered_set<std::string> layouts_in_progress;
    std::unordered_map<std::string, FreestandingGlobal> globals;
    std::unordered_set<const Stmt*> top_level_global_statements;
    std::vector<LoopPatchState> loops;
    std::vector<size_t> return_patches;
    std::unordered_map<std::string, int32_t> slots;
    std::unordered_map<std::string, TypeAnnot> local_types;
    std::unordered_set<std::string> function_globals;
    std::unordered_map<const Stmt*, int32_t> repeat_slots;
    std::vector<int32_t> scratch_slots;
    size_t gop_guid_data_offset = std::numeric_limits<size_t>::max();
    size_t next_slot = 0;
    size_t temporary_depth = 0;
    size_t call_argument_depth = 0;
    uint32_t frame_size = 0;
    bool context_helpers_used = false;
    bool preempt_helpers_used = false;
    bool user_frame_helpers_used = false;
    bool fast_syscall_helper_used = false;
    std::string fast_syscall_handler;
    std::string fast_syscall_bad_return_handler;
    uint32_t fast_syscall_kernel_rsp_offset = 0;
    uint32_t fast_syscall_user_rsp_offset = 0;
    const FuncDef* current_function = nullptr;
    bool current_is_entry = false;

    [[noreturn]] static void fail(const Node* node, const std::string& message) {
        throw SuraOsCompileError(message, node ? node->line : 0);
    }

    static size_t align_up_size(size_t value, size_t alignment) {
        return (value + alignment - 1U) & ~(alignment - 1U);
    }

    static bool is_power_of_two(size_t value) {
        return value && (value & (value - 1U)) == 0;
    }

    static std::string annotated_type_name(const TypeAnnot& type) {
        if (!type.source_name.empty()) return type.source_name;
        if (!type.class_name.empty()) return type.class_name;
        switch (type.kind) {
            case SType::BOOL: return "bool";
            case SType::NUMBER: return "u64";
            default: return {};
        }
    }

    static bool pointer_pointee(const TypeAnnot& type, std::string& pointee) {
        const std::string name = annotated_type_name(type);
        constexpr const char prefix[] = "ptr[";
        if (name.size() <= sizeof(prefix) ||
            name.compare(0, sizeof(prefix) - 1, prefix) != 0 ||
            name.back() != ']') {
            return false;
        }
        pointee = name.substr(sizeof(prefix) - 1,
                              name.size() - sizeof(prefix));
        return !pointee.empty();
    }

    static bool signed_scalar_type(const TypeAnnot& type) {
        const std::string name = annotated_type_name(type);
        return name == "i8" || name == "i16" || name == "i32" ||
               name == "i64" || name == "isize";
    }

    bool primitive_type_layout(const TypeAnnot& type, size_t& size,
                               size_t& alignment) const {
        const std::string name = annotated_type_name(type);
        if (name == "i8" || name == "u8" || name == "bool") {
            size = alignment = 1;
            return true;
        }
        if (name == "i16" || name == "u16") {
            size = alignment = 2;
            return true;
        }
        if (name == "i32" || name == "u32") {
            size = alignment = 4;
            return true;
        }
        if (name == "i64" || name == "u64" || name == "isize" ||
            name == "usize" || name == "ptr" ||
            name.rfind("ptr[", 0) == 0) {
            size = alignment = 8;
            return true;
        }
        return false;
    }

    const FreestandingStructLayout& ensure_struct_layout(
        const std::string& name, const Node* origin) {
        auto ready = struct_layouts.find(name);
        if (ready != struct_layouts.end()) return ready->second;
        auto definition = struct_defs.find(name);
        if (definition == struct_defs.end()) {
            fail(origin, "unknown freestanding struct type '" + name + "'");
        }
        if (!layouts_in_progress.insert(name).second) {
            fail(origin, "freestanding struct '" + name +
                         "' contains itself by value; use ptr[" + name + "]");
        }

        const ClassDef* def = definition->second;
        FreestandingStructLayout layout;
        layout.packed = def->packed_layout;
        size_t cursor = 0;
        for (const std::string& field_name : def->field_order) {
            auto type_it = def->field_types.find(field_name);
            if (type_it == def->field_types.end() || !type_it->second.present) {
                layouts_in_progress.erase(name);
                fail(def, "freestanding struct field '" + name + "." +
                          field_name + "' requires an explicit type");
            }
            const TypeAnnot& field_type = type_it->second;
            size_t field_size = 0;
            size_t field_alignment = 1;
            if (!primitive_type_layout(field_type, field_size, field_alignment)) {
                const std::string nested = annotated_type_name(field_type);
                const auto& nested_layout = ensure_struct_layout(nested, def);
                field_size = nested_layout.size;
                field_alignment = nested_layout.alignment;
            }
            if (layout.packed) field_alignment = 1;
            cursor = align_up_size(cursor, field_alignment);
            layout.fields.emplace(
                field_name,
                FreestandingFieldLayout{cursor, field_size, field_alignment,
                                         field_type});
            cursor += field_size;
            layout.alignment = std::max(layout.alignment, field_alignment);
        }
        if (cursor == 0) {
            layouts_in_progress.erase(name);
            fail(def, "freestanding struct '" + name +
                      "' must contain at least one typed field");
        }
        layout.size = align_up_size(cursor, layout.alignment);
        layouts_in_progress.erase(name);
        auto inserted = struct_layouts.emplace(name, std::move(layout));
        return inserted.first->second;
    }

    const FreestandingFieldLayout& require_field(
        const std::string& struct_name, const std::string& field_name,
        const Node* origin) {
        const auto& layout = ensure_struct_layout(struct_name, origin);
        auto field = layout.fields.find(field_name);
        if (field == layout.fields.end()) {
            fail(origin, "freestanding struct '" + struct_name +
                         "' has no field '" + field_name + "'");
        }
        return field->second;
    }

    size_t append_zero_data(size_t size, size_t alignment,
                            const Node* origin) {
        if (!is_power_of_two(alignment) || alignment > (1U << 20)) {
            fail(origin, "static-data alignment must be a power of two no "
                         "greater than 1048576");
        }
        if (size == 0 || size > (64U << 20) ||
            data.size() > (64U << 20) - size) {
            fail(origin, "a static object must contain 1..67108864 bytes and "
                         "the total static-data limit is 64 MiB");
        }
        while (data.size() % alignment) data.push_back(0);
        const size_t offset = data.size();
        data.resize(data.size() + size, 0);
        return offset;
    }

    static void write_data_integer(std::vector<uint8_t>& destination,
                                   size_t offset, unsigned width,
                                   uint64_t value) {
        for (unsigned i = 0; i < width; ++i) {
            destination.at(offset + i) =
                static_cast<uint8_t>(value >> (i * 8));
        }
    }

    int32_t allocate_slot(const std::string& name,
                          const TypeAnnot* type = nullptr) {
        if (type && type->present) local_types[name] = *type;
        auto found = slots.find(name);
        if (found != slots.end()) return found->second;
        const int32_t disp = -static_cast<int32_t>((++next_slot) * 8);
        slots.emplace(name, disp);
        return disp;
    }

    size_t reserve_temporaries(size_t count, const Node* origin) {
        if (count > temporary_slot_count ||
            temporary_depth > temporary_slot_count - count) {
            fail(origin, "freestanding intrinsic expressions are nested too deeply");
        }
        const size_t base = temporary_depth;
        temporary_depth += count;
        return base;
    }

    void release_temporaries(size_t count) {
        temporary_depth -= count;
    }

    int32_t require_slot(const Ident* ident) const {
        auto found = slots.find(ident->name);
        if (found == slots.end()) {
            fail(ident, "unknown freestanding local '" + ident->name + "'");
        }
        return found->second;
    }

    void scan_global_declarations(const SuraBlock* block) {
        if (!block) return;
        for (const auto& holder : block->body) {
            const Stmt* stmt = holder.get();
            if (!stmt) continue;
            switch (stmt->kind) {
                case NK::GLOBAL_DECL: {
                    auto* declaration =
                        static_cast<const GlobalDeclStmt*>(stmt);
                    for (const std::string& name : declaration->names) {
                        if (globals.find(name) == globals.end()) {
                            fail(stmt, "global '" + name +
                                       "' has no top-level static declaration");
                        }
                        function_globals.insert(name);
                    }
                    break;
                }
                case NK::IF: {
                    auto* value = static_cast<const IfStmt*>(stmt);
                    scan_global_declarations(value->then_block.get());
                    scan_global_declarations(value->else_block.get());
                    break;
                }
                case NK::WHILE:
                    scan_global_declarations(
                        static_cast<const WhileStmt*>(stmt)->body.get());
                    break;
                case NK::REPEAT:
                    scan_global_declarations(
                        static_cast<const RepeatStmt*>(stmt)->body.get());
                    break;
                default:
                    break;
            }
        }
    }

    void scan_locals(const SuraBlock* block) {
        if (!block) return;
        for (const auto& holder : block->body) {
            const Stmt* stmt = holder.get();
            if (!stmt) continue;
            switch (stmt->kind) {
                case NK::ASSIGN: {
                    auto* assignment = static_cast<const AssignStmt*>(stmt);
                    if (function_globals.find(assignment->name) ==
                        function_globals.end()) {
                        allocate_slot(assignment->name,
                                      &assignment->type_annot);
                    }
                    break;
                }
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

    void load_memory_rax(unsigned width, bool is_signed = false) {
        if (width == 8) {
            if (is_signed) x.bytes({0x48, 0x0f, 0xbe, 0x00});
            else x.bytes({0x0f, 0xb6, 0x00});
        } else if (width == 16) {
            if (is_signed) x.bytes({0x48, 0x0f, 0xbf, 0x00});
            else x.bytes({0x0f, 0xb7, 0x00});
        } else if (width == 32) {
            if (is_signed) x.bytes({0x48, 0x63, 0x00});
            else x.bytes({0x8b, 0x00});
        } else if (width == 64) {
            x.bytes({0x48, 0x8b, 0x00});
        } else {
            throw SuraOsCompileError("unsupported freestanding memory width");
        }
    }

    void store_rax_to_rcx(unsigned width) {
        if (width == 8) x.bytes({0x88, 0x01});
        else if (width == 16) x.bytes({0x66, 0x89, 0x01});
        else if (width == 32) x.bytes({0x89, 0x01});
        else if (width == 64) x.bytes({0x48, 0x89, 0x01});
        else throw SuraOsCompileError("unsupported freestanding memory width");
    }

    void load_global_rax(const std::string& name, const Node* origin) {
        auto found = globals.find(name);
        if (found == globals.end()) {
            fail(origin, "unknown freestanding global '" + name + "'");
        }
        const FreestandingGlobal& global = found->second;
        if (global.address_value) {
            address_of_data_rax(global.data_offset);
            return;
        }
        const bool is_signed = signed_scalar_type(global.type);
        if (global.scalar_width == 8) {
            rip_data_disp(is_signed
                              ? std::initializer_list<uint8_t>{
                                    0x48, 0x0f, 0xbe, 0x05}
                              : std::initializer_list<uint8_t>{
                                    0x0f, 0xb6, 0x05},
                          global.data_offset);
        } else if (global.scalar_width == 16) {
            rip_data_disp(is_signed
                              ? std::initializer_list<uint8_t>{
                                    0x48, 0x0f, 0xbf, 0x05}
                              : std::initializer_list<uint8_t>{
                                    0x0f, 0xb7, 0x05},
                          global.data_offset);
        } else if (global.scalar_width == 32) {
            rip_data_disp(is_signed
                              ? std::initializer_list<uint8_t>{
                                    0x48, 0x63, 0x05}
                              : std::initializer_list<uint8_t>{
                                    0x8b, 0x05},
                          global.data_offset);
        } else if (global.scalar_width == 64) {
            rip_data_disp({0x48, 0x8b, 0x05}, global.data_offset);
        } else {
            fail(origin, "global '" + name + "' has an invalid scalar width");
        }
    }

    void store_global_rax(const std::string& name, const Node* origin) {
        auto found = globals.find(name);
        if (found == globals.end()) {
            fail(origin, "unknown freestanding global '" + name + "'");
        }
        const FreestandingGlobal& global = found->second;
        if (!global.mutable_scalar || global.address_value) {
            fail(origin, "static object '" + name +
                         "' is an address; modify its memory through mem or a "
                         "typed pointer");
        }
        if (global.scalar_width == 8) {
            rip_data_disp({0x88, 0x05}, global.data_offset);
        } else if (global.scalar_width == 16) {
            rip_data_disp({0x66, 0x89, 0x05}, global.data_offset);
        } else if (global.scalar_width == 32) {
            rip_data_disp({0x89, 0x05}, global.data_offset);
        } else if (global.scalar_width == 64) {
            rip_data_disp({0x48, 0x89, 0x05}, global.data_offset);
        } else {
            fail(origin, "global '" + name + "' has an invalid scalar width");
        }
    }

    void address_of_named_rax(const Ident* ident) {
        auto local = slots.find(ident->name);
        if (local != slots.end()) {
            x.bytes({0x48, 0x8d, 0x85});
            x.d(static_cast<uint32_t>(local->second));
            return;
        }
        auto global = globals.find(ident->name);
        if (global != globals.end()) {
            address_of_data_rax(global->second.data_offset);
            return;
        }
        if (functions.find(ident->name) != functions.end()) {
            const size_t displacement =
                x.rel32({0x48, 0x8d, 0x05}); // lea rax,[rip+handler]
            function_address_patches.push_back(
                {displacement, ident->name, ident->line});
            return;
        }
        fail(ident, "unknown freestanding local, global, or function '" +
                    ident->name + "'");
    }

    const TypeAnnot& identifier_type(const Ident* ident) const {
        auto local = local_types.find(ident->name);
        if (local != local_types.end()) return local->second;
        auto global = globals.find(ident->name);
        if (global != globals.end()) return global->second.type;
        fail(ident, "unknown typed freestanding value '" + ident->name + "'");
    }

    const FreestandingFieldLayout& typed_pointer_field(
        const Ident* pointer, const std::string& field_name,
        const Node* origin) {
        std::string struct_name;
        if (!pointer_pointee(identifier_type(pointer), struct_name)) {
            fail(origin, "'" + pointer->name +
                         "' must have a ptr[StructName] annotation for field access");
        }
        return require_field(struct_name, field_name, origin);
    }

    const FreestandingFieldLayout& compile_field_address(
        const DotAccess* access) {
        if (!access->obj || access->obj->kind != NK::IDENT) {
            fail(access, "freestanding structured field access currently "
                         "requires a named ptr[StructName] value");
        }
        auto* pointer = static_cast<const Ident*>(access->obj.get());
        const auto& field = typed_pointer_field(pointer, access->prop, access);
        compile_expr(pointer);
        if (field.offset) {
            x.bytes({0x48, 0x05});
            x.d(static_cast<uint32_t>(field.offset));
        }
        return field;
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

    bool constant_integer(const Expr* expr, uint64_t& out) {
        if (!expr) return false;
        if (integral_literal(expr, out)) return true;
        if (expr->kind == NK::BOOL_LIT) {
            out = static_cast<const BoolLit*>(expr)->value ? 1 : 0;
            return true;
        }
        if (expr->kind == NK::NIL_LIT) {
            out = 0;
            return true;
        }
        if (expr->kind == NK::UNARY_OP) {
            auto* unary = static_cast<const UnaryOp*>(expr);
            uint64_t value = 0;
            if (!constant_integer(unary->operand.get(), value)) return false;
            if (unary->op == "-") out = 0 - value;
            else if (unary->op == "~") out = ~value;
            else if (unary->op == "not" || unary->op == "!") out = !value;
            else return false;
            return true;
        }
        if (expr->kind == NK::BIN_OP) {
            auto* binary = static_cast<const BinOp*>(expr);
            uint64_t left = 0;
            uint64_t right = 0;
            if (!constant_integer(binary->left.get(), left) ||
                !constant_integer(binary->right.get(), right)) {
                return false;
            }
            const std::string& op = binary->op;
            if (op == "+") out = left + right;
            else if (op == "-") out = left - right;
            else if (op == "*") out = left * right;
            else if (op == "/" && right) out = left / right;
            else if (op == "%" && right) out = left % right;
            else if (op == "&" || op == "and") out = left & right;
            else if (op == "|" || op == "or") out = left | right;
            else if (op == "^") out = left ^ right;
            else if (op == "<<" && right < 64) out = left << right;
            else if (op == ">>" && right < 64) out = left >> right;
            else if (op == "==") out = left == right;
            else if (op == "!=") out = left != right;
            else if (op == "<") out = static_cast<int64_t>(left) <
                                      static_cast<int64_t>(right);
            else if (op == "<=") out = static_cast<int64_t>(left) <=
                                       static_cast<int64_t>(right);
            else if (op == ">") out = static_cast<int64_t>(left) >
                                      static_cast<int64_t>(right);
            else if (op == ">=") out = static_cast<int64_t>(left) >=
                                       static_cast<int64_t>(right);
            else return false;
            return true;
        }

        std::string raw_name;
        const std::vector<ExprPtr>* args_ptr = nullptr;
        if (!flatten_call(expr, raw_name, args_ptr)) return false;
        const std::string name = canonical_intrinsic(raw_name);
        const auto& args = *args_ptr;
        if ((name == "u64" || name == "ptr" || name == "usize") &&
            args.size() == 1 && args[0]->kind == NK::STR_LIT) {
            return parse_integer_text(
                static_cast<const StrLit*>(args[0].get())->value, out);
        }
        if ((name == "sizeof" || name == "alignof") && args.size() == 1 &&
            args[0]->kind == NK::IDENT) {
            const std::string type_name =
                static_cast<const Ident*>(args[0].get())->name;
            const auto& layout = ensure_struct_layout(type_name, expr);
            out = name == "sizeof" ? layout.size : layout.alignment;
            return true;
        }
        if (name == "offset_of" && args.size() == 2 &&
            args[0]->kind == NK::IDENT && args[1]->kind == NK::IDENT) {
            const std::string type_name =
                static_cast<const Ident*>(args[0].get())->name;
            const std::string field_name =
                static_cast<const Ident*>(args[1].get())->name;
            out = require_field(type_name, field_name, expr).offset;
            return true;
        }
        return false;
    }

    uint64_t require_constant_integer(const Expr* expr,
                                      const std::string& description) {
        uint64_t value = 0;
        if (!constant_integer(expr, value)) {
            fail(expr, description + " must be a compile-time integer");
        }
        return value;
    }

    static TypeAnnot inferred_type(const std::string& name) {
        TypeAnnot type;
        type.present = true;
        type.kind = SType::NUMBER;
        type.source_name = name;
        return type;
    }

    FreestandingGlobal build_static_initializer(const AssignStmt* assignment,
                                                 const std::string& name,
                                                 const std::vector<ExprPtr>& args,
                                                 const Expr* origin) {
        FreestandingGlobal global;
        global.address_value = true;
        global.mutable_scalar = false;
        global.type = assignment->type_annot;

        const auto alignment_argument =
            [&](size_t index, size_t fallback) -> size_t {
                if (args.size() <= index) return fallback;
                const uint64_t value = require_constant_integer(
                    args[index].get(), "static-data alignment");
                if (value > std::numeric_limits<size_t>::max()) {
                    fail(args[index].get(), "static-data alignment is too large");
                }
                return static_cast<size_t>(value);
            };

        if (name == "static_zero") {
            if (args.empty() || args.size() > 2) {
                fail(origin, "static.zero(size, alignment?) expects one or two arguments");
            }
            const uint64_t requested =
                require_constant_integer(args[0].get(), "static.zero() size");
            if (requested > std::numeric_limits<size_t>::max()) {
                fail(args[0].get(), "static.zero() size is too large");
            }
            global.size = static_cast<size_t>(requested);
            global.data_offset = append_zero_data(
                global.size, alignment_argument(1, 1), origin);
            return global;
        }

        if (name == "static_struct") {
            if (args.empty() || args.size() > 2 ||
                args[0]->kind != NK::IDENT) {
                fail(origin, "static.struct(Type, count?) expects a struct type name");
            }
            const std::string type_name =
                static_cast<const Ident*>(args[0].get())->name;
            const auto& layout = ensure_struct_layout(type_name, origin);
            const uint64_t count = args.size() == 2
                ? require_constant_integer(args[1].get(), "static.struct() count")
                : 1;
            if (!count || count > std::numeric_limits<size_t>::max() /
                                     layout.size) {
                fail(origin, "static.struct() count is out of range");
            }
            global.size = layout.size * static_cast<size_t>(count);
            global.data_offset =
                append_zero_data(global.size, layout.alignment, origin);
            if (!global.type.present) {
                global.type = inferred_type("ptr[" + type_name + "]");
            }
            return global;
        }

        if (name == "static_utf8") {
            if (args.size() != 1 || args[0]->kind != NK::STR_LIT) {
                fail(origin, "static.utf8() expects one string literal");
            }
            const std::string& text =
                static_cast<const StrLit*>(args[0].get())->value;
            global.size = text.size() + 1;
            global.data_offset = append_zero_data(global.size, 1, origin);
            std::copy(text.begin(), text.end(),
                      data.begin() + static_cast<std::ptrdiff_t>(global.data_offset));
            return global;
        }

        if (name == "static_utf16") {
            if (args.size() != 1 || args[0]->kind != NK::STR_LIT) {
                fail(origin, "static.utf16() expects one string literal");
            }
            const auto units = utf8_to_utf16(
                static_cast<const StrLit*>(args[0].get())->value);
            global.size = (units.size() + 1) * 2;
            global.data_offset = append_zero_data(global.size, 2, origin);
            for (size_t i = 0; i < units.size(); ++i) {
                write_data_integer(data, global.data_offset + i * 2, 2,
                                   units[i]);
            }
            return global;
        }

        unsigned element_width = 0;
        if (name == "static_u8" || name == "static_bytes") element_width = 1;
        else if (name == "static_u16") element_width = 2;
        else if (name == "static_u32") element_width = 4;
        else if (name == "static_u64") element_width = 8;
        if (element_width) {
            if (args.empty() || args.size() > 2 ||
                args[0]->kind != NK::ARRAY_LIT) {
                fail(origin, raw_name_for_error(name) +
                             " expects an array literal and optional alignment");
            }
            auto* array = static_cast<const ArrayLit*>(args[0].get());
            if (array->elements.empty()) {
                fail(origin, "static arrays must contain at least one element");
            }
            if (array->elements.size() >
                (64U << 20) / element_width) {
                fail(origin, "static array is too large");
            }
            global.size = array->elements.size() * element_width;
            global.data_offset = append_zero_data(
                global.size, alignment_argument(1, element_width), origin);
            const uint64_t maximum =
                element_width == 8
                    ? std::numeric_limits<uint64_t>::max()
                    : ((uint64_t{1} << (element_width * 8)) - 1U);
            for (size_t i = 0; i < array->elements.size(); ++i) {
                const uint64_t value = require_constant_integer(
                    array->elements[i].get(), "static array element");
                if (value > maximum) {
                    fail(array->elements[i].get(),
                         "static array element does not fit its width");
                }
                write_data_integer(data,
                                   global.data_offset + i * element_width,
                                   element_width, value);
            }
            return global;
        }

        fail(origin, "unknown static-data initializer '" + name + "'");
    }

    static std::string raw_name_for_error(const std::string& canonical) {
        std::string result = canonical;
        std::replace(result.begin(), result.end(), '_', '.');
        return result + "()";
    }

    void build_top_level_declarations(const SuraBlock* root) {
        globals.clear();
        top_level_global_statements.clear();
        for (const auto& holder : root->body) {
            if (!holder || holder->kind != NK::ASSIGN) continue;
            auto* assignment = static_cast<const AssignStmt*>(holder.get());
            if (globals.find(assignment->name) != globals.end()) {
                fail(assignment, "duplicate top-level static declaration '" +
                                 assignment->name + "'");
            }

            FreestandingGlobal global;
            std::string raw_name;
            const std::vector<ExprPtr>* args = nullptr;
            if (flatten_call(assignment->value.get(), raw_name, args) &&
                canonical_intrinsic(raw_name).rfind("static_", 0) == 0) {
                global = build_static_initializer(
                    assignment, canonical_intrinsic(raw_name), *args,
                    assignment->value.get());
            } else {
                global.type = assignment->type_annot;
                if (!global.type.present) {
                    global.type = inferred_type(
                        assignment->value &&
                                assignment->value->kind == NK::BOOL_LIT
                            ? "bool"
                            : "u64");
                }
                size_t scalar_size = 0;
                size_t scalar_alignment = 0;
                if (!primitive_type_layout(global.type, scalar_size,
                                           scalar_alignment) ||
                    scalar_size > 8) {
                    fail(assignment, "top-level scalar '" + assignment->name +
                                     "' requires a fixed-width scalar or pointer type");
                }
                const uint64_t value = require_constant_integer(
                    assignment->value.get(),
                    "top-level initializer for '" + assignment->name + "'");
                if (scalar_size < 8) {
                    const unsigned bits =
                        static_cast<unsigned>(scalar_size * 8);
                    bool fits = false;
                    if (signed_scalar_type(global.type)) {
                        const int64_t signed_value =
                            static_cast<int64_t>(value);
                        const int64_t minimum =
                            -(int64_t{1} << (bits - 1));
                        const int64_t maximum =
                            (int64_t{1} << (bits - 1)) - 1;
                        fits = signed_value >= minimum &&
                               signed_value <= maximum;
                    } else {
                        fits = value <=
                               ((uint64_t{1} << bits) - 1U);
                    }
                    if (!fits) {
                        fail(assignment->value.get(),
                             "top-level initializer does not fit '" +
                                 annotated_type_name(global.type) + "'");
                    }
                }
                global.size = scalar_size;
                global.scalar_width = static_cast<unsigned>(scalar_size * 8);
                global.address_value = false;
                global.mutable_scalar = true;
                global.data_offset =
                    append_zero_data(scalar_size, scalar_alignment, assignment);
                write_data_integer(data, global.data_offset,
                                   static_cast<unsigned>(scalar_size), value);
            }
            globals.emplace(assignment->name, std::move(global));
            top_level_global_statements.insert(holder.get());
        }
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
            case NK::IDENT: {
                auto* ident = static_cast<const Ident*>(expr);
                auto local = slots.find(ident->name);
                if (local != slots.end()) {
                    x.mov_rax_rbp(local->second);
                } else {
                    load_global_rax(ident->name, ident);
                }
                return;
            }
            case NK::DOT_ACCESS: {
                const auto& field =
                    compile_field_address(static_cast<const DotAccess*>(expr));
                if (field.size != 1 && field.size != 2 &&
                    field.size != 4 && field.size != 8) {
                    fail(expr, "embedded struct fields cannot be loaded as a "
                               "single scalar; use a ptr[NestedStruct] field");
                }
                load_memory_rax(static_cast<unsigned>(field.size * 8),
                                signed_scalar_type(field.type));
                return;
            }
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
                // Match hosted Sura semantics: logical operators short-circuit
                // and return the selected operand instead of a normalized bool.
                // This is also required for checked pointer guards such as
                // `state == 0 or state.field == 0`.
                if (binary->op == "and") {
                    compile_expr(binary->left.get());
                    x.bytes({0x48, 0x85, 0xc0});
                    const size_t finished = x.rel32({0x0f, 0x84});
                    compile_expr(binary->right.get());
                    x.patch_rel32(finished, x.pos());
                    return;
                }
                if (binary->op == "or") {
                    compile_expr(binary->left.get());
                    x.bytes({0x48, 0x85, 0xc0});
                    const size_t finished = x.rel32({0x0f, 0x85});
                    compile_expr(binary->right.get());
                    x.patch_rel32(finished, x.pos());
                    return;
                }
                compile_expr(binary->left.get());
                // Keep a spill slot plus alignment padding so a function call
                // inside the right operand still sees the Win64-required
                // 16-byte aligned stack.
                x.bytes({0x50, 0x48, 0x83, 0xec, 0x08});
                compile_expr(binary->right.get());
                x.bytes({0x48, 0x83, 0xc4, 0x08, 0x59});
                const std::string& op = binary->op;
                if (op == "+") x.bytes({0x48, 0x01, 0xc1, 0x48, 0x89, 0xc8});
                else if (op == "-") x.bytes({0x48, 0x29, 0xc1, 0x48, 0x89, 0xc8});
                else if (op == "*") x.bytes({0x48, 0x0f, 0xaf, 0xc8, 0x48, 0x89, 0xc8});
                else if (op == "&") x.bytes({0x48, 0x21, 0xc1, 0x48, 0x89, 0xc8});
                else if (op == "|") x.bytes({0x48, 0x09, 0xc1, 0x48, 0x89, 0xc8});
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

    size_t save_call_args_from(const std::vector<ExprPtr>& args,
                               size_t first) {
        if (first > args.size()) {
            fail(current_function, "invalid freestanding argument range");
        }
        const size_t count = args.size() - first;
        if (count > argument_bank_width) {
            fail(current_function, "freestanding calls support at most " +
                 std::to_string(argument_bank_width) + " arguments");
        }
        if (call_argument_depth >= argument_bank_count) {
            fail(current_function,
                 "freestanding call expressions are nested more than eight levels");
        }
        const size_t base =
            temporary_slot_count +
            call_argument_depth * argument_bank_width;
        ++call_argument_depth;
        for (size_t i = 0; i < count; ++i) {
            compile_expr(args[first + i].get());
            x.mov_rbp_rax(scratch_slots[base + i]);
        }
        --call_argument_depth;
        return base;
    }

    size_t save_call_args(const std::vector<ExprPtr>& args) {
        return save_call_args_from(args, 0);
    }

    void load_call_args(size_t count, size_t base = 0) {
        if (count > 0) x.mov_reg_rbp(1, scratch_slots[base]);
        if (count > 1) x.mov_reg_rbp(2, scratch_slots[base + 1]);
        if (count > 2) x.mov_reg_rbp(8, scratch_slots[base + 2]);
        if (count > 3) x.mov_reg_rbp(9, scratch_slots[base + 3]);
        for (size_t i = 4; i < count; ++i) {
            x.mov_rax_rbp(scratch_slots[base + i]);
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
        const size_t argument_base = save_call_args(args);
        load_system_table_rax();
        x.bytes({0x48, 0x8b, 0x80});
        x.d(table_offset);
        x.bytes({0x4c, 0x8b, 0x98});
        x.d(function_offset);
        load_call_args(args.size(), argument_base);
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

        if (name == "sizeof" || name == "alignof" ||
            name == "offset_of") {
            uint64_t value = 0;
            if (!constant_integer(origin, value)) {
                fail(origin, raw_name +
                             "() requires compile-time struct and field names");
            }
            x.mov_rax_imm(value);
            return;
        }

        if (name == "addr_of") {
            if (args.size() != 1 || args[0]->kind != NK::IDENT) {
                fail(origin, "addr_of() requires one local or static global name");
            }
            address_of_named_rax(static_cast<const Ident*>(args[0].get()));
            return;
        }

        if (name == "call_indirect") {
            if (args.empty() || args.size() > 6) {
                fail(origin, "call.indirect(function, argument...) expects a "
                             "function address and at most five arguments");
            }
            const size_t argument_base = save_call_args(args);
            x.mov_reg_rbp(11, scratch_slots[argument_base]);
            load_call_args(args.size() - 1, argument_base + 1);
            x.bytes({0x41, 0xff, 0xd3}); // call r11
            return;
        }

        if (name == "syscall_invoke") {
            if (args.size() < 2 || args.size() > 7) {
                fail(origin, "syscall.invoke(vector, number, argument...) "
                             "expects a vector, number, and at most five "
                             "arguments");
            }
            const uint64_t vector =
                require_constant_integer(args[0].get(), "system-call vector");
            if (vector < 32 || vector > 255) {
                fail(args[0].get(), "system-call vector must be 32..255");
            }
            const size_t argument_base = save_call_args_from(args, 1);

            // Keep the Win64 nonvolatile registers used by the syscall
            // convention intact for the surrounding Sura function.
            x.bytes({0x57, 0x56}); // push rdi; push rsi
            x.mov_rax_rbp(scratch_slots[argument_base]); // syscall number

            if (args.size() > 2) {
                x.bytes({0x48, 0x8b, 0xbd}); // rdi = argument 0
                x.d(static_cast<uint32_t>(scratch_slots[argument_base + 1]));
            } else {
                x.bytes({0x31, 0xff});
            }
            if (args.size() > 3) {
                x.bytes({0x48, 0x8b, 0xb5}); // rsi = argument 1
                x.d(static_cast<uint32_t>(scratch_slots[argument_base + 2]));
            } else {
                x.bytes({0x31, 0xf6});
            }
            if (args.size() > 4) {
                x.mov_reg_rbp(2, scratch_slots[argument_base + 3]);
            } else {
                x.bytes({0x31, 0xd2});
            }
            if (args.size() > 5) {
                x.mov_reg_rbp(10, scratch_slots[argument_base + 4]);
            } else {
                x.bytes({0x45, 0x31, 0xd2});
            }
            if (args.size() > 6) {
                x.mov_reg_rbp(8, scratch_slots[argument_base + 5]);
            } else {
                x.bytes({0x45, 0x31, 0xc0});
            }

            x.bytes({0xcd, static_cast<uint8_t>(vector),
                     0x5e, 0x5f}); // int vector; pop rsi; pop rdi
            return;
        }

        if (name == "syscall_fast") {
            if (args.empty() || args.size() > 6) {
                fail(origin, "syscall.fast(number, argument...) expects a "
                             "number and at most five arguments");
            }
            const size_t argument_base = save_call_args(args);
            x.bytes({0x57, 0x56}); // preserve Win64 nonvolatile rdi/rsi
            x.mov_rax_rbp(scratch_slots[argument_base]);

            if (args.size() > 1) {
                x.bytes({0x48, 0x8b, 0xbd});
                x.d(static_cast<uint32_t>(scratch_slots[argument_base + 1]));
            } else {
                x.bytes({0x31, 0xff});
            }
            if (args.size() > 2) {
                x.bytes({0x48, 0x8b, 0xb5});
                x.d(static_cast<uint32_t>(scratch_slots[argument_base + 2]));
            } else {
                x.bytes({0x31, 0xf6});
            }
            if (args.size() > 3) {
                x.mov_reg_rbp(2, scratch_slots[argument_base + 3]);
            } else {
                x.bytes({0x31, 0xd2});
            }
            if (args.size() > 4) {
                x.mov_reg_rbp(10, scratch_slots[argument_base + 4]);
            } else {
                x.bytes({0x45, 0x31, 0xd2});
            }
            if (args.size() > 5) {
                x.mov_reg_rbp(8, scratch_slots[argument_base + 5]);
            } else {
                x.bytes({0x45, 0x31, 0xc0});
            }
            x.bytes({0x0f, 0x05, 0x5e, 0x5f}); // syscall; restore rsi/rdi
            return;
        }

        if (name == "user_is_address") {
            if (args.size() != 1) {
                fail(origin, "user.is_address(address) expects one value");
            }
            compile_expr(args[0].get());
            x.bytes({0x48, 0x89, 0xc2, // mov rdx,rax
                     0x31, 0xc0,       // false by default
                     0x48, 0x85, 0xd2});
            const size_t finished = x.rel32({0x0f, 0x84});
            x.bytes({0x48, 0xc1, 0xea, 0x2f,
                     0x0f, 0x94, 0xc0,
                     0x0f, 0xb6, 0xc0});
            x.patch_rel32(finished, x.pos());
            return;
        }

        if (name == "user_frame_size") {
            if (!args.empty()) {
                fail(origin, "user.frame_size() expects no values");
            }
            x.mov_rax_imm(168);
            return;
        }

        if (name == "user_frame_init") {
            if (args.size() != 6) {
                fail(origin, "user.frame_init(kernel_stack_top, entry, "
                             "user_stack_pointer, argument, code_selector, "
                             "stack_selector) expects six values");
            }
            const uint64_t code_selector =
                require_constant_integer(args[4].get(), "user code selector");
            const uint64_t stack_selector =
                require_constant_integer(args[5].get(), "user stack selector");
            if (code_selector > 0xffff || (code_selector & 3) != 3 ||
                code_selector < 3) {
                fail(args[4].get(), "user code selector must be a nonzero "
                                    "16-bit selector with RPL 3");
            }
            if (stack_selector > 0xffff || (stack_selector & 3) != 3 ||
                stack_selector < 3) {
                fail(args[5].get(), "user stack selector must be a nonzero "
                                    "16-bit selector with RPL 3");
            }
            uint64_t constant_top = 0;
            if (constant_integer(args[0].get(), constant_top) &&
                constant_top < 168) {
                fail(args[0].get(), "user frame kernel stack must provide at "
                                    "least 168 bytes");
            }

            const size_t argument_base = save_call_args(args);
            std::vector<size_t> invalid;
            const auto validate_canonical_rax = [&]() {
                x.bytes({0x48, 0x89, 0xc2,
                         0x48, 0xc1, 0xea, 0x2f,
                         0x48, 0x85, 0xd2});
                const size_t canonical = x.rel32({0x0f, 0x84});
                x.bytes({0x48, 0x81, 0xfa,
                         0xff, 0xff, 0x01, 0x00});
                invalid.push_back(x.rel32({0x0f, 0x85}));
                x.patch_rel32(canonical, x.pos());
            };
            const auto validate_low_user_address = [&](size_t slot) {
                    x.mov_rax_rbp(scratch_slots[slot]);
                    x.bytes({0x48, 0x85, 0xc0});
                    invalid.push_back(x.rel32({0x0f, 0x84}));
                    x.bytes({0x48, 0x89, 0xc2,
                             0x48, 0xc1, 0xea, 0x2f,
                             0x48, 0x85, 0xd2});
                    invalid.push_back(x.rel32({0x0f, 0x85}));
            };

            x.mov_rax_rbp(scratch_slots[argument_base]);
            x.bytes({0x48, 0x85, 0xc0});
            invalid.push_back(x.rel32({0x0f, 0x84}));
            validate_canonical_rax();
            x.bytes({0x48, 0x83, 0xe0, 0xf0,
                     0x48, 0x3d, 0xa8, 0x00, 0x00, 0x00});
            invalid.push_back(x.rel32({0x0f, 0x82}));
            x.bytes({0x48, 0x2d, 0xa8, 0x00, 0x00, 0x00});
            validate_canonical_rax();
            x.bytes({0x49, 0x89, 0xc2}); // r10 = normalized frame

            validate_low_user_address(argument_base + 1);
            validate_low_user_address(argument_base + 2);
            x.mov_rax_rbp(scratch_slots[argument_base + 2]);
            x.bytes({0x48, 0x83, 0xe0, 0x0f,
                     0x48, 0x83, 0xf8, 0x08});
            invalid.push_back(x.rel32({0x0f, 0x85}));

            // Clear all 21 qwords before installing the initial user state.
            x.bytes({0x31, 0xc0});
            for (uint32_t offset = 0; offset < 168; offset += 8) {
                if (offset == 0) {
                    x.bytes({0x49, 0x89, 0x02});
                } else if (offset <= 0x7f) {
                    x.bytes({0x49, 0x89, 0x42,
                             static_cast<uint8_t>(offset)});
                } else {
                    x.bytes({0x49, 0x89, 0x82});
                    x.d(offset);
                }
            }
            x.mov_rax_rbp(scratch_slots[argument_base + 3]);
            x.bytes({0x49, 0x89, 0x42, 0x68}); // RCX = argument
            x.mov_rax_rbp(scratch_slots[argument_base + 1]);
            x.bytes({0x49, 0x89, 0x82, 0x80, 0x00, 0x00, 0x00});
            x.mov_rax_imm(code_selector);
            x.bytes({0x49, 0x89, 0x82, 0x88, 0x00, 0x00, 0x00});
            x.mov_rax_imm(0x202);
            x.bytes({0x49, 0x89, 0x82, 0x90, 0x00, 0x00, 0x00});
            x.mov_rax_rbp(scratch_slots[argument_base + 2]);
            x.bytes({0x49, 0x89, 0x82, 0x98, 0x00, 0x00, 0x00});
            x.mov_rax_imm(stack_selector);
            x.bytes({0x49, 0x89, 0x82, 0xa0, 0x00, 0x00, 0x00});
            x.bytes({0x4c, 0x89, 0xd0}); // return frame
            const size_t finished = x.rel32({0xe9});

            const size_t invalid_target = x.pos();
            for (size_t patch : invalid) {
                x.patch_rel32(patch, invalid_target);
            }
            x.bytes({0x31, 0xc0});
            x.patch_rel32(finished, x.pos());
            return;
        }

        if (name == "user_frame_valid") {
            if (args.size() != 1) {
                fail(origin, "user.frame_valid(frame) expects one value");
            }
            user_frame_helpers_used = true;
            const size_t argument_base = save_call_args(args);
            load_call_args(1, argument_base);
            const size_t patch = x.rel32({0xe8});
            call_patches.push_back(
                {patch, "__sura_user_frame_valid", origin->line});
            return;
        }

        if (name == "user_resume") {
            if (args.size() != 1) {
                fail(origin, "user.resume(frame) expects one value");
            }
            if (!current_function ||
                (current_function->abi != "interrupt" &&
                 current_function->abi != "interrupt_error")) {
                fail(origin, "user.resume() is only available inside an "
                             "interrupt or interrupt_error function");
            }
            user_frame_helpers_used = true;
            const size_t argument_base = save_call_args(args);
            load_call_args(1, argument_base);
            const size_t patch = x.rel32({0xe8});
            call_patches.push_back(
                {patch, "__sura_user_resume", origin->line});
            return;
        }

        if (name == "user_enter") {
            if (args.size() != 5) {
                fail(origin, "user.enter(entry, stack_pointer, argument, "
                             "code_selector, stack_selector) expects five "
                             "values");
            }
            const uint64_t code_selector =
                require_constant_integer(args[3].get(), "user code selector");
            const uint64_t stack_selector =
                require_constant_integer(args[4].get(), "user stack selector");
            if (code_selector > 0xffff || (code_selector & 3) != 3 ||
                code_selector < 3) {
                fail(args[3].get(), "user code selector must be a nonzero "
                                    "16-bit selector with RPL 3");
            }
            if (stack_selector > 0xffff || (stack_selector & 3) != 3 ||
                stack_selector < 3) {
                fail(args[4].get(), "user stack selector must be a nonzero "
                                    "16-bit selector with RPL 3");
            }
            const size_t argument_base = save_call_args(args);

            // Preserve nonvolatile registers if validation fails and the
            // intrinsic returns false. Successful entry never returns here.
            x.bytes({0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
            x.bytes({0x4c, 0x8b, 0xa5});
            x.d(static_cast<uint32_t>(scratch_slots[argument_base]));
            x.bytes({0x4c, 0x8b, 0xbd});
            x.d(static_cast<uint32_t>(scratch_slots[argument_base + 1]));
            x.bytes({0x4c, 0x8b, 0xad});
            x.d(static_cast<uint32_t>(scratch_slots[argument_base + 2]));

            std::vector<size_t> invalid;
            const auto validate_low_user_address =
                [&](std::initializer_list<uint8_t> move_to_rax) {
                    x.bytes(move_to_rax);
                    x.bytes({0x48, 0x85, 0xc0});
                    invalid.push_back(x.rel32({0x0f, 0x84}));
                    x.bytes({0x48, 0x89, 0xc2,
                             0x48, 0xc1, 0xea, 0x2f,
                             0x48, 0x85, 0xd2});
                    invalid.push_back(x.rel32({0x0f, 0x85}));
                };
            validate_low_user_address({0x4c, 0x89, 0xe0}); // r12 entry
            validate_low_user_address({0x4c, 0x89, 0xf8}); // r15 stack
            x.bytes({0x4c, 0x89, 0xf8,
                     0x48, 0x83, 0xe0, 0x0f,
                     0x48, 0x83, 0xf8, 0x08});
            invalid.push_back(x.rel32({0x0f, 0x85}));

            x.bytes({0x4c, 0x89, 0xe9}); // rcx = user argument
            x.b(0x68);
            x.d(static_cast<uint32_t>(stack_selector));
            x.bytes({0x41, 0x57}); // user RSP
            x.b(0x68);
            x.d(0x202);            // reserved bit + IF
            x.b(0x68);
            x.d(static_cast<uint32_t>(code_selector));
            x.bytes({0x41, 0x54,             // user RIP
                     0xfa,                   // close the maskable IRQ window
                     0x0f, 0x01, 0xf8,       // swapgs
                     0x48, 0xcf});            // iretq restores user IF

            const size_t invalid_target = x.pos();
            for (size_t patch : invalid) {
                x.patch_rel32(patch, invalid_target);
            }
            x.bytes({0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c,
                     0x31, 0xc0});
            return;
        }

        if (name.rfind("static_", 0) == 0) {
            fail(origin, raw_name +
                         "() may only initialize a top-level static declaration");
        }

        if (name == "ptr_add") {
            if (args.size() != 2) {
                fail(origin, "ptr.add(address, byte_offset) expects two values");
            }
            const size_t temporary = reserve_temporaries(1, origin);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            x.bytes({0x48, 0x01, 0xc8}); // add rax,rcx
            release_temporaries(1);
            return;
        }
        if (name == "ptr_index") {
            if (args.size() != 3) {
                fail(origin, "ptr.index(address, index, element_size) expects three values");
            }
            const size_t temporary = reserve_temporaries(2, origin);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.mov_rbp_rax(scratch_slots[temporary + 1]);
            compile_expr(args[2].get());
            x.mov_reg_rbp(1, scratch_slots[temporary + 1]);
            x.bytes({0x48, 0x0f, 0xaf, 0xc8}); // imul rcx,rax
            x.mov_rax_rbp(scratch_slots[temporary]);
            x.bytes({0x48, 0x01, 0xc8});
            release_temporaries(2);
            return;
        }
        if (name == "ptr_field") {
            if (args.size() != 3 || args[1]->kind != NK::IDENT ||
                args[2]->kind != NK::IDENT) {
                fail(origin, "ptr.field(address, StructType, field) expects "
                             "an address and two names");
            }
            const std::string type_name =
                static_cast<const Ident*>(args[1].get())->name;
            const std::string field_name =
                static_cast<const Ident*>(args[2].get())->name;
            const size_t offset =
                require_field(type_name, field_name, origin).offset;
            compile_expr(args[0].get());
            if (offset) {
                x.bytes({0x48, 0x05});
                x.d(static_cast<uint32_t>(offset));
            }
            return;
        }
        if (name == "ptr_align_up" || name == "ptr_align_down" ||
            name == "ptr_is_aligned") {
            if (args.size() != 2) {
                fail(origin, raw_name + "(value, alignment) expects two values");
            }
            const size_t temporary = reserve_temporaries(1, origin);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            if (name == "ptr_align_down") {
                x.bytes({0x48, 0xf7, 0xd8}); // neg rax
                x.mov_reg_rbp(1, scratch_slots[temporary]);
                x.bytes({0x48, 0x21, 0xc8}); // and rax,rcx
            } else if (name == "ptr_align_up") {
                x.bytes({0x48, 0x89, 0xc2, 0x48, 0xff, 0xca});
                x.mov_reg_rbp(1, scratch_slots[temporary]);
                x.bytes({0x48, 0x01, 0xd1, 0x48, 0xf7, 0xd8,
                         0x48, 0x21, 0xc8});
            } else {
                x.bytes({0x48, 0xff, 0xc8}); // dec rax
                x.mov_reg_rbp(1, scratch_slots[temporary]);
                x.bytes({0x48, 0x85, 0xc8, 0x0f, 0x94, 0xc0,
                         0x0f, 0xb6, 0xc0});
            }
            release_temporaries(1);
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
            load_memory_rax(width);
        };
        if (name == "mem_read8") { memory_read(8); return; }
        if (name == "mem_read16") { memory_read(16); return; }
        if (name == "mem_read32") { memory_read(32); return; }
        if (name == "mem_read64") { memory_read(64); return; }

        const auto atomic_load = [&](unsigned width) {
            if (args.size() != 1) {
                fail(origin, "atomic load expects one address");
            }
            compile_expr(args[0].get());
            load_memory_rax(width);
        };
        if (name == "atomic_load" || name == "atomic_load64") {
            atomic_load(64); return;
        }
        if (name == "atomic_load8") { atomic_load(8); return; }
        if (name == "atomic_load16") { atomic_load(16); return; }
        if (name == "atomic_load32") { atomic_load(32); return; }

        const auto atomic_exchange = [&](unsigned width) {
            if (args.size() != 2) {
                fail(origin, "atomic exchange expects address and value");
            }
            const size_t temporary = reserve_temporaries(1, origin);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            if (width == 8) {
                x.bytes({0x86, 0x01, 0x0f, 0xb6, 0xc0});
            } else if (width == 16) {
                x.bytes({0x66, 0x87, 0x01, 0x0f, 0xb7, 0xc0});
            } else if (width == 32) {
                x.bytes({0x87, 0x01});
            } else {
                x.bytes({0x48, 0x87, 0x01});
            }
            release_temporaries(1);
        };
        if (name == "atomic_exchange" || name == "atomic_exchange64") {
            atomic_exchange(64); return;
        }
        if (name == "atomic_exchange8") { atomic_exchange(8); return; }
        if (name == "atomic_exchange16") { atomic_exchange(16); return; }
        if (name == "atomic_exchange32") { atomic_exchange(32); return; }

        const auto atomic_compare_exchange = [&](unsigned width) {
            if (args.size() != 3) {
                fail(origin, "atomic compare_exchange expects address, "
                             "expected, and desired");
            }
            const size_t temporary = reserve_temporaries(2, origin);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.mov_rbp_rax(scratch_slots[temporary + 1]);
            compile_expr(args[2].get());
            x.bytes({0x48, 0x89, 0xc2}); // rdx = desired
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            x.mov_rax_rbp(scratch_slots[temporary + 1]); // rax = expected
            if (width == 8) {
                x.bytes({0xf0, 0x0f, 0xb0, 0x11, 0x0f, 0xb6, 0xc0});
            } else if (width == 16) {
                x.bytes({0x66, 0xf0, 0x0f, 0xb1, 0x11,
                         0x0f, 0xb7, 0xc0});
            } else if (width == 32) {
                x.bytes({0xf0, 0x0f, 0xb1, 0x11});
            } else {
                x.bytes({0xf0, 0x48, 0x0f, 0xb1, 0x11});
            }
            release_temporaries(2);
        };
        if (name == "atomic_compare_exchange" ||
            name == "atomic_compare_exchange64") {
            atomic_compare_exchange(64); return;
        }
        if (name == "atomic_compare_exchange8") {
            atomic_compare_exchange(8); return;
        }
        if (name == "atomic_compare_exchange16") {
            atomic_compare_exchange(16); return;
        }
        if (name == "atomic_compare_exchange32") {
            atomic_compare_exchange(32); return;
        }

        const auto atomic_fetch_add = [&](unsigned width, bool subtract) {
            if (args.size() != 2) {
                fail(origin, "atomic fetch_add/fetch_sub expects address and value");
            }
            const size_t temporary = reserve_temporaries(1, origin);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            if (subtract) x.bytes({0x48, 0xf7, 0xd8});
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            if (width == 8) {
                x.bytes({0xf0, 0x0f, 0xc0, 0x01, 0x0f, 0xb6, 0xc0});
            } else if (width == 16) {
                x.bytes({0x66, 0xf0, 0x0f, 0xc1, 0x01,
                         0x0f, 0xb7, 0xc0});
            } else if (width == 32) {
                x.bytes({0xf0, 0x0f, 0xc1, 0x01});
            } else {
                x.bytes({0xf0, 0x48, 0x0f, 0xc1, 0x01});
            }
            release_temporaries(1);
        };
        if (name == "atomic_fetch_add" || name == "atomic_fetch_add64") {
            atomic_fetch_add(64, false); return;
        }
        if (name == "atomic_fetch_add8") {
            atomic_fetch_add(8, false); return;
        }
        if (name == "atomic_fetch_add16") {
            atomic_fetch_add(16, false); return;
        }
        if (name == "atomic_fetch_add32") {
            atomic_fetch_add(32, false); return;
        }
        if (name == "atomic_fetch_sub" || name == "atomic_fetch_sub64") {
            atomic_fetch_add(64, true); return;
        }
        if (name == "atomic_fetch_sub8") {
            atomic_fetch_add(8, true); return;
        }
        if (name == "atomic_fetch_sub16") {
            atomic_fetch_add(16, true); return;
        }
        if (name == "atomic_fetch_sub32") {
            atomic_fetch_add(32, true); return;
        }

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

        if (name == "cpu_read_cr0" || name == "cpu_read_cr2" ||
            name == "cpu_read_cr3" || name == "cpu_read_cr4" ||
            name == "cpu_read_flags" ||
            name == "cpu_read_task_register") {
            if (!args.empty()) fail(origin, raw_name + "() takes no arguments");
            if (name == "cpu_read_cr0") x.bytes({0x0f, 0x20, 0xc0});
            else if (name == "cpu_read_cr2") x.bytes({0x0f, 0x20, 0xd0});
            else if (name == "cpu_read_cr3") x.bytes({0x0f, 0x20, 0xd8});
            else if (name == "cpu_read_cr4") x.bytes({0x0f, 0x20, 0xe0});
            else if (name == "cpu_read_flags") x.bytes({0x9c, 0x58});
            else x.bytes({0x31, 0xc0, 0x0f, 0x00, 0xc8}); // xor eax,eax; str ax
            return;
        }
        if (name == "cpu_rdtsc" || name == "cpu_rdtscp") {
            if (!args.empty()) fail(origin, raw_name + "() takes no arguments");
            if (name == "cpu_rdtsc") x.bytes({0x0f, 0x31});
            else x.bytes({0x0f, 0x01, 0xf9});
            x.bytes({0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0});
            return;
        }
        if (name == "cpu_xgetbv") {
            if (args.size() != 1) {
                fail(origin, "cpu.xgetbv(index) expects one value");
            }
            compile_expr(args[0].get());
            x.bytes({0x89, 0xc1, 0x0f, 0x01, 0xd0,
                     0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0});
            return;
        }
        if (name == "cpu_cpuid_eax" || name == "cpu_cpuid_ebx" ||
            name == "cpu_cpuid_ecx" || name == "cpu_cpuid_edx") {
            if (args.empty() || args.size() > 2) {
                fail(origin, raw_name + "(leaf, subleaf?) expects one or two values");
            }
            const size_t temporary = reserve_temporaries(1, origin);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            if (args.size() == 2) compile_expr(args[1].get());
            else x.bytes({0x31, 0xc0});
            x.bytes({0x89, 0xc1}); // ecx = subleaf
            x.mov_rax_rbp(scratch_slots[temporary]);
            x.bytes({0x53, 0x0f, 0xa2}); // preserve nonvolatile rbx; cpuid
            if (name == "cpu_cpuid_ebx") x.bytes({0x89, 0xd8});
            else if (name == "cpu_cpuid_ecx") x.bytes({0x89, 0xc8});
            else if (name == "cpu_cpuid_edx") x.bytes({0x89, 0xd0});
            x.b(0x5b);
            release_temporaries(1);
            return;
        }

        if (name == "cpu_read_msr") {
            if (args.size() != 1) fail(origin, "cpu.read_msr() expects an MSR index");
            compile_expr(args[0].get());
            x.bytes({0x89, 0xc1, 0x0f, 0x32, 0x48, 0xc1, 0xe2, 0x20,
                     0x48, 0x09, 0xd0});
            return;
        }

        if (name == "percpu_base" ||
            name == "percpu_kernel_base") {
            if (!args.empty()) fail(origin, raw_name + "() takes no arguments");
            x.b(0xb9);
            x.d(name == "percpu_base" ? 0xc0000101U : 0xc0000102U);
            x.bytes({0x0f, 0x32, 0x48, 0xc1, 0xe2, 0x20,
                     0x48, 0x09, 0xd0});
            return;
        }
        if (name == "percpu_address") {
            if (args.size() != 1) {
                fail(origin, "percpu.address(offset) expects one value");
            }
            const size_t temporary = reserve_temporaries(1, origin);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            x.b(0xb9);
            x.d(0xc0000101U);
            x.bytes({0x0f, 0x32, 0x48, 0xc1, 0xe2, 0x20,
                     0x48, 0x09, 0xd0});
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            x.bytes({0x48, 0x01, 0xc8});
            release_temporaries(1);
            return;
        }
        const auto percpu_read = [&](unsigned width) {
            if (args.size() != 1) {
                fail(origin, "percpu read expects one byte offset");
            }
            compile_expr(args[0].get());
            if (width == 8) x.bytes({0x65, 0x0f, 0xb6, 0x00});
            else if (width == 16) x.bytes({0x65, 0x0f, 0xb7, 0x00});
            else if (width == 32) x.bytes({0x65, 0x8b, 0x00});
            else x.bytes({0x65, 0x48, 0x8b, 0x00});
        };
        if (name == "percpu_read8") { percpu_read(8); return; }
        if (name == "percpu_read16") { percpu_read(16); return; }
        if (name == "percpu_read32") { percpu_read(32); return; }
        if (name == "percpu_read64") { percpu_read(64); return; }

        if (name == "apic_mode") {
            if (!args.empty()) fail(origin, "apic.mode() takes no arguments");
            x.bytes({0xb9, 0x1b, 0x00, 0x00, 0x00, 0x0f, 0x32,
                     0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0,
                     0x48, 0xc1, 0xe8, 0x0a, 0x48, 0x83, 0xe0, 0x03,
                     0x48, 0x85, 0xc0});
            const size_t disabled = x.rel32({0x0f, 0x84});
            x.bytes({0x48, 0xff, 0xc8}); // enabled: 2 -> xAPIC 1, 3 -> x2APIC 2
            x.patch_rel32(disabled, x.pos());
            return;
        }
        if (name == "apic_base") {
            if (!args.empty()) fail(origin, "apic.base() takes no arguments");
            x.bytes({0xb9, 0x1b, 0x00, 0x00, 0x00, 0x0f, 0x32,
                     0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0,
                     0x48, 0x25, 0x00, 0xf0, 0xff, 0xff});
            return;
        }
        if (name == "apic_read" || name == "apic_current_id" ||
            name == "apic_icr_busy") {
            uint64_t offset = 0;
            if (name == "apic_read") {
                if (args.size() != 1) {
                    fail(origin, "apic.read(register_offset) expects one value");
                }
                offset = require_constant_integer(
                    args[0].get(), "APIC register offset");
            } else {
                if (!args.empty()) fail(origin, raw_name + "() takes no arguments");
                offset = name == "apic_current_id" ? 0x20 : 0x300;
            }
            if (offset < 0x20 || offset > 0x3f0 || (offset & 0x0f)) {
                fail(origin, "APIC register offset must be 0x20..0x3f0 "
                             "and 16-byte aligned");
            }

            const size_t temporary = reserve_temporaries(1, origin);
            x.bytes({0xb9, 0x1b, 0x00, 0x00, 0x00, 0x0f, 0x32,
                     0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0});
            x.mov_rbp_rax(scratch_slots[temporary]);
            x.bytes({0x48, 0xa9, 0x00, 0x04, 0x00, 0x00});
            const size_t use_xapic = x.rel32({0x0f, 0x84});

            x.b(0xb9);
            x.d(static_cast<uint32_t>(0x800 + offset / 16));
            x.bytes({0x0f, 0x32, 0x48, 0xc1, 0xe2, 0x20,
                     0x48, 0x09, 0xd0});
            if (name == "apic_icr_busy") {
                x.bytes({0x48, 0xc1, 0xe8, 0x0c, 0x48, 0x83, 0xe0, 0x01});
            }
            const size_t finished = x.rel32({0xe9});

            x.patch_rel32(use_xapic, x.pos());
            x.mov_rax_rbp(scratch_slots[temporary]);
            x.bytes({0x48, 0x25, 0x00, 0xf0, 0xff, 0xff,
                     0x48, 0x05});
            x.d(static_cast<uint32_t>(offset));
            x.bytes({0x8b, 0x00});
            if (name == "apic_current_id") {
                x.bytes({0xc1, 0xe8, 0x18});
            } else if (name == "apic_icr_busy") {
                x.bytes({0xc1, 0xe8, 0x0c, 0x83, 0xe0, 0x01});
            }
            x.patch_rel32(finished, x.pos());
            release_temporaries(1);
            return;
        }

        if (name == "paging_pml4_index" ||
            name == "paging_pdpt_index" ||
            name == "paging_pd_index" ||
            name == "paging_pt_index" ||
            name == "paging_offset") {
            if (args.size() != 1) {
                fail(origin, raw_name + "(address) expects one value");
            }
            compile_expr(args[0].get());
            unsigned shift = 0;
            if (name == "paging_pml4_index") shift = 39;
            else if (name == "paging_pdpt_index") shift = 30;
            else if (name == "paging_pd_index") shift = 21;
            else if (name == "paging_pt_index") shift = 12;
            if (shift) {
                x.bytes({0x48, 0xc1, 0xe8});
                x.b(static_cast<uint8_t>(shift));
                x.bytes({0x25, 0xff, 0x01, 0x00, 0x00});
            } else {
                x.bytes({0x25, 0xff, 0x0f, 0x00, 0x00});
            }
            return;
        }
        if (name == "paging_is_canonical48") {
            if (args.size() != 1) {
                fail(origin, "paging.is_canonical48(address) expects one value");
            }
            compile_expr(args[0].get());
            x.bytes({0x48, 0x89, 0xc1,
                     0x48, 0xc1, 0xe0, 0x10,
                     0x48, 0xc1, 0xf8, 0x10,
                     0x48, 0x39, 0xc1,
                     0x0f, 0x94, 0xc0,
                     0x0f, 0xb6, 0xc0});
            return;
        }
        if (name == "paging_entry" ||
            name == "paging_entry_address" ||
            name == "paging_entry_flags" ||
            name == "paging_present" ||
            name == "paging_large") {
            const size_t expected = name == "paging_entry" ? 2 : 1;
            if (args.size() != expected) {
                fail(origin, raw_name +
                             (expected == 2
                                  ? "(physical_address, flags) expects two values"
                                  : "(entry) expects one value"));
            }
            if (name == "paging_entry") {
                uint64_t constant_address = 0;
                if (constant_integer(args[0].get(), constant_address) &&
                    (constant_address & 0xfffU)) {
                    fail(args[0].get(), "paging.entry() physical address must "
                                        "be 4 KiB aligned");
                }
                const size_t temporary = reserve_temporaries(1, origin);
                compile_expr(args[0].get());
                x.bytes({0x48, 0xba});
                x.q(0x000ffffffffff000ULL);
                x.bytes({0x48, 0x21, 0xd0});
                x.mov_rbp_rax(scratch_slots[temporary]);
                compile_expr(args[1].get());
                x.bytes({0x48, 0xba});
                x.q(0xfff0000000000fffULL);
                x.bytes({0x48, 0x21, 0xd0});
                x.mov_reg_rbp(1, scratch_slots[temporary]);
                x.bytes({0x48, 0x09, 0xc8});
                release_temporaries(1);
                return;
            }
            compile_expr(args[0].get());
            if (name == "paging_entry_address") {
                x.bytes({0x48, 0xba});
                x.q(0x000ffffffffff000ULL);
                x.bytes({0x48, 0x21, 0xd0});
            } else if (name == "paging_entry_flags") {
                x.bytes({0x48, 0xba});
                x.q(0xfff0000000000fffULL);
                x.bytes({0x48, 0x21, 0xd0});
            } else if (name == "paging_present") {
                x.bytes({0x83, 0xe0, 0x01});
            } else {
                x.bytes({0x48, 0xc1, 0xe8, 0x07,
                         0x83, 0xe0, 0x01});
            }
            return;
        }
        if (name == "paging_read") {
            if (args.size() != 2) {
                fail(origin, "paging.read(table, index) expects two values");
            }
            uint64_t constant_index = 0;
            if (constant_integer(args[1].get(), constant_index) &&
                constant_index > 511) {
                fail(args[1].get(), "page-table index must be 0..511");
            }
            const size_t temporary = reserve_temporaries(1, origin);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.bytes({0x25, 0xff, 0x01, 0x00, 0x00,
                     0x48, 0xc1, 0xe0, 0x03});
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            x.bytes({0x48, 0x01, 0xc8, 0x48, 0x8b, 0x00});
            release_temporaries(1);
            return;
        }
        if (name == "paging_root") {
            if (!args.empty()) fail(origin, "paging.root() takes no arguments");
            x.bytes({0x0f, 0x20, 0xd8,
                     0x48, 0xba});
            x.q(0x000ffffffffff000ULL);
            x.bytes({0x48, 0x21, 0xd0});
            return;
        }
        if (name == "context_frame_size") {
            if (!args.empty()) {
                fail(origin, "context.frame_size() takes no arguments");
            }
            x.mov_rax_imm(72);
            return;
        }
        if (name == "preempt_frame_size") {
            if (!args.empty()) {
                fail(origin, "preempt.frame_size() takes no arguments");
            }
            x.mov_rax_imm(152);
            return;
        }
        if (name == "preempt_init") {
            if (args.size() != 5) {
                fail(origin, "preempt.init(stack_top, entry, argument, "
                             "exit_handler, code_selector) expects five values");
            }
            uint64_t constant_selector = 0;
            if (constant_integer(args[4].get(), constant_selector) &&
                (constant_selector == 0 || constant_selector > 0xffff ||
                 (constant_selector & 3) != 0)) {
                fail(args[4].get(), "preemptive task code selector must be a "
                                    "nonzero 16-bit selector with RPL 0");
            }
            uint64_t constant_top = 0;
            if (constant_integer(args[0].get(), constant_top) &&
                (constant_top < 152 || (constant_top & 15))) {
                fail(args[0].get(), "preempt.init() constant stack top must be "
                                    "16-byte aligned and leave 152 frame bytes");
            }

            context_helpers_used = true;
            const size_t argument_base = save_call_args(args);
            const size_t temporary = reserve_temporaries(1, origin);
            std::vector<size_t> invalid;

            const auto validate_canonical =
                [&](size_t argument_index, const char* /*description*/) {
                    x.mov_rax_rbp(scratch_slots[argument_base + argument_index]);
                    x.bytes({0x48, 0x85, 0xc0});
                    invalid.push_back(x.rel32({0x0f, 0x84}));
                    x.bytes({0x48, 0x89, 0xc2,
                             0x48, 0xc1, 0xea, 0x2f,
                             0x48, 0x85, 0xd2});
                    const size_t canonical = x.rel32({0x0f, 0x84});
                    x.bytes({0x48, 0x81, 0xfa,
                             0xff, 0xff, 0x01, 0x00});
                    invalid.push_back(x.rel32({0x0f, 0x85}));
                    x.patch_rel32(canonical, x.pos());
                };

            validate_canonical(0, "stack top");
            x.bytes({0xa8, 0x0f});
            invalid.push_back(x.rel32({0x0f, 0x85}));
            x.bytes({0x48, 0x3d, 0x98, 0x00, 0x00, 0x00});
            invalid.push_back(x.rel32({0x0f, 0x86}));
            validate_canonical(1, "task entry");
            validate_canonical(3, "task exit handler");
            x.mov_rax_rbp(scratch_slots[argument_base + 4]);
            x.bytes({0x48, 0x89, 0xc2,
                     0x48, 0xc1, 0xea, 0x10,
                     0x48, 0x85, 0xd2});
            invalid.push_back(x.rel32({0x0f, 0x85}));
            x.bytes({0xa8, 0x03});
            invalid.push_back(x.rel32({0x0f, 0x85}));
            x.bytes({0x48, 0x85, 0xc0});
            invalid.push_back(x.rel32({0x0f, 0x84}));

            x.mov_rax_rbp(scratch_slots[argument_base]);
            x.bytes({0x48, 0x83, 0xe0, 0xf0,
                     0x48, 0x2d, 0x98, 0x00, 0x00, 0x00});
            x.mov_rbp_rax(scratch_slots[temporary]);
            x.bytes({0x48, 0x89, 0xc1, 0x31, 0xc0,
                     0x48, 0x89, 0x01});
            for (uint8_t offset :
                 {uint8_t(32), uint8_t(40), uint8_t(48), uint8_t(56),
                  uint8_t(64), uint8_t(72), uint8_t(80), uint8_t(88),
                  uint8_t(96), uint8_t(104), uint8_t(112), uint8_t(120)}) {
                x.bytes({0x48, 0x89, 0x41, offset});
            }
            x.mov_rax_rbp(scratch_slots[argument_base + 3]);
            x.bytes({0x48, 0x89, 0x41, 0x08});
            x.mov_rax_rbp(scratch_slots[argument_base + 2]);
            x.bytes({0x48, 0x89, 0x41, 0x10});
            x.mov_rax_rbp(scratch_slots[argument_base + 1]);
            x.bytes({0x48, 0x89, 0x41, 0x18});

            const size_t bootstrap = x.rel32({0x48, 0x8d, 0x05});
            function_address_patches.push_back(
                {bootstrap, "__sura_context_bootstrap", origin->line});
            x.bytes({0x48, 0x89, 0x81, 0x80, 0x00, 0x00, 0x00});
            x.mov_rax_rbp(scratch_slots[argument_base + 4]);
            x.bytes({0x48, 0x89, 0x81, 0x88, 0x00, 0x00, 0x00});
            x.mov_rax_imm(0x202);
            x.bytes({0x48, 0x89, 0x81, 0x90, 0x00, 0x00, 0x00});
            x.mov_rax_rbp(scratch_slots[temporary]);
            const size_t finished = x.rel32({0xe9});

            const size_t invalid_target = x.pos();
            for (size_t patch : invalid) {
                x.patch_rel32(patch, invalid_target);
            }
            x.bytes({0x31, 0xc0});
            x.patch_rel32(finished, x.pos());
            release_temporaries(1);
            return;
        }
        if (name == "preempt_frame_valid") {
            if (args.size() != 1) {
                fail(origin, "preempt.frame_valid(frame) expects one value");
            }
            preempt_helpers_used = true;
            const size_t argument_base = save_call_args(args);
            load_call_args(1, argument_base);
            const size_t patch = x.rel32({0xe8});
            call_patches.push_back(
                {patch, "__sura_preempt_frame_valid", origin->line});
            return;
        }
        if (name == "preempt_resume") {
            if (args.size() != 1) {
                fail(origin, "preempt.resume(frame) expects one value");
            }
            if (!current_function ||
                (current_function->abi != "interrupt" &&
                 current_function->abi != "interrupt_error")) {
                fail(origin, "preempt.resume() is only available inside an "
                             "interrupt or interrupt_error function");
            }
            preempt_helpers_used = true;
            const size_t argument_base = save_call_args(args);
            load_call_args(1, argument_base);
            const size_t patch = x.rel32({0xe8});
            call_patches.push_back(
                {patch, "__sura_preempt_resume", origin->line});
            return;
        }
        if (name == "interrupt_invoke") {
            if (args.size() != 1) {
                fail(origin, "interrupt.invoke(vector) expects one value");
            }
            const uint64_t vector =
                require_constant_integer(args[0].get(),
                                         "software interrupt vector");
            if (vector < 32 || vector > 255) {
                fail(args[0].get(),
                     "software interrupt vector must be 32..255");
            }
            x.bytes({0xcd, static_cast<uint8_t>(vector)});
            x.mov_rax_imm(1);
            return;
        }
        if (name == "context_init") {
            if (args.size() != 4) {
                fail(origin, "context.init(stack_top, entry, argument, "
                             "exit_handler) expects four values");
            }
            uint64_t constant_top = 0;
            if (constant_integer(args[0].get(), constant_top) &&
                constant_top < 72) {
                fail(args[0].get(), "context.init() stack top must leave "
                                    "at least 72 bytes for the initial frame");
            }
            context_helpers_used = true;
            const size_t argument_base = save_call_args(args);
            const size_t temporary = reserve_temporaries(1, origin);

            x.mov_rax_rbp(scratch_slots[argument_base]);
            x.bytes({0x48, 0x83, 0xe0, 0xf0,
                     0x48, 0x83, 0xe8, 0x48});
            x.mov_rbp_rax(scratch_slots[temporary]);
            x.bytes({0x48, 0x89, 0xc1, 0x31, 0xc0,
                     0x48, 0x89, 0x01,
                     0x48, 0x89, 0x41, 0x20,
                     0x48, 0x89, 0x41, 0x28,
                     0x48, 0x89, 0x41, 0x30,
                     0x48, 0x89, 0x41, 0x38});

            x.mov_rax_rbp(scratch_slots[argument_base + 3]);
            x.bytes({0x48, 0x89, 0x41, 0x08});
            x.mov_rax_rbp(scratch_slots[argument_base + 2]);
            x.bytes({0x48, 0x89, 0x41, 0x10});
            x.mov_rax_rbp(scratch_slots[argument_base + 1]);
            x.bytes({0x48, 0x89, 0x41, 0x18});
            const size_t bootstrap =
                x.rel32({0x48, 0x8d, 0x05});
            function_address_patches.push_back(
                {bootstrap, "__sura_context_bootstrap", origin->line});
            x.bytes({0x48, 0x89, 0x41, 0x40});
            x.mov_rax_rbp(scratch_slots[temporary]);
            release_temporaries(1);
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
            const size_t temporary = reserve_temporaries(2, origin);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary + 1]);
            load_image_handle_rax();
            x.mov_rbp_rax(scratch_slots[temporary]);
            load_system_table_rax();
            x.bytes({0x48, 0x8b, 0x40, 0x60, 0x4c, 0x8b, 0x98});
            x.d(232);
            load_call_args(2, temporary);
            x.bytes({0x41, 0xff, 0xd3});
            release_temporaries(2);
            return;
        }

        if (name == "uefi_gop_framebuffer" ||
            name == "uefi_gop_framebuffer_size" ||
            name == "uefi_gop_width" || name == "uefi_gop_height" ||
            name == "uefi_gop_stride" || name == "uefi_gop_pixel_format") {
            if (!args.empty()) fail(origin, raw_name + "() takes no arguments");
            const size_t temporary = reserve_temporaries(6, origin);

            // LocateProtocol(&GOP_GUID, nil, &gop). The last scratch slot is
            // stable addressable storage for the returned interface pointer.
            x.bytes({0x31, 0xc0});
            x.mov_rbp_rax(scratch_slots[temporary + 5]);
            address_of_data_rax(ensure_gop_guid());
            x.mov_rbp_rax(scratch_slots[temporary]);
            x.bytes({0x31, 0xc0});
            x.mov_rbp_rax(scratch_slots[temporary + 1]);
            x.bytes({0x48, 0x8d, 0x85});
            x.d(static_cast<uint32_t>(scratch_slots[temporary + 5]));
            x.mov_rbp_rax(scratch_slots[temporary + 2]);

            load_system_table_rax();
            x.bytes({0x48, 0x8b, 0x40, 0x60, 0x4c, 0x8b, 0x98});
            x.d(320);
            load_call_args(3, temporary);
            x.bytes({0x41, 0xff, 0xd3, 0x48, 0x85, 0xc0});
            const size_t status_failure = x.rel32({0x0f, 0x85});
            x.mov_rax_rbp(scratch_slots[temporary + 5]);
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
                release_temporaries(6);
                return;
            }
            const size_t success = x.rel32({0xe9});
            const size_t failure = x.pos();
            x.patch_rel32(status_failure, failure);
            x.patch_rel32(null_failure, failure);
            x.patch_rel32(mode_failure, failure);
            x.bytes({0x31, 0xc0});
            x.patch_rel32(success, x.pos());
            release_temporaries(6);
            return;
        }

        auto user = functions.find(raw_name);
        if (user != functions.end()) {
            if (user->second->abi != "sura") {
                fail(origin, "interrupt handler '" + raw_name +
                             "' cannot be called like a normal function; "
                             "use addr_of(" + raw_name + ") for an IDT gate");
            }
            if (args.size() != user->second->params.size()) {
                fail(origin, "freestanding function '" + raw_name +
                             "' expects " +
                             std::to_string(user->second->params.size()) +
                             " argument(s), got " +
                             std::to_string(args.size()));
            }
            const size_t argument_base = save_call_args(args);
            load_call_args(args.size(), argument_base);
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
            const size_t temporary = reserve_temporaries(1, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.bytes({0x48, 0xc1, 0xe0, 0x04});
            x.bytes({0x48, 0x0b, 0x85});
            x.d(static_cast<uint32_t>(scratch_slots[temporary]));
            x.bytes({0x48, 0x89, 0xc2});
            release_temporaries(1);
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

        if (name == "syscall_fast_configure") {
            if (args.size() != 7) {
                fail(expr, "syscall.fast_configure(dispatch, bad_return, "
                           "kernel_cs, user_cs, flags_mask, kernel_rsp_offset, "
                           "user_rsp_offset) expects seven arguments");
            }
            if (fast_syscall_helper_used) {
                fail(expr, "syscall.fast_configure() may appear only once");
            }
            const auto require_direct_handler =
                [&](const Expr* value, const std::string& description) {
                    std::string address_call;
                    const std::vector<ExprPtr>* address_args = nullptr;
                    if (!flatten_call(value, address_call, address_args) ||
                        canonical_intrinsic(address_call) != "addr_of" ||
                        address_args->size() != 1 ||
                        (*address_args)[0]->kind != NK::IDENT) {
                        fail(value, description + " must be written as "
                                    "addr_of(function)");
                    }
                    const std::string function_name =
                        static_cast<const Ident*>((*address_args)[0].get())->name;
                    auto function = functions.find(function_name);
                    if (function == functions.end() ||
                        function->second->abi != "sura" ||
                        function->second->params.size() != 1 ||
                        function->second->param_types.empty() ||
                        annotated_type_name(
                            function->second->param_types[0]).rfind("ptr", 0) != 0) {
                        fail(value, description + " '" + function_name +
                                    "' must be a normal one-pointer function");
                    }
                    return function_name;
                };

            const std::string dispatch =
                require_direct_handler(args[0].get(), "fast syscall dispatcher");
            const std::string bad_return =
                require_direct_handler(args[1].get(), "bad-return handler");
            const uint64_t kernel_cs =
                require_constant_integer(args[2].get(), "kernel code selector");
            const uint64_t user_cs =
                require_constant_integer(args[3].get(), "user code selector");
            const uint64_t flags_mask =
                require_constant_integer(args[4].get(), "syscall flags mask");
            const uint64_t kernel_rsp_offset =
                require_constant_integer(args[5].get(), "kernel RSP offset");
            const uint64_t user_rsp_offset =
                require_constant_integer(args[6].get(), "user RSP offset");

            if (kernel_cs == 0 || kernel_cs > 0xffff ||
                (kernel_cs & 3) != 0) {
                fail(args[2].get(), "kernel code selector must be a nonzero "
                                    "16-bit selector with RPL 0");
            }
            if (user_cs < 19 || user_cs > 0xffff ||
                (user_cs & 3) != 3) {
                fail(args[3].get(), "user code selector must be a 16-bit "
                                    "selector with RPL 3 and value at least 19");
            }
            constexpr uint64_t required_mask = 0x47700;
            if (flags_mask > 0xffffffffULL ||
                (flags_mask & required_mask) != required_mask) {
                fail(args[4].get(), "syscall flags mask must include TF, IF, "
                                    "DF, IOPL, NT, and AC (0x47700)");
            }
            if (kernel_rsp_offset > 0x7ffffff8ULL ||
                user_rsp_offset > 0x7ffffff8ULL ||
                (kernel_rsp_offset & 7) || (user_rsp_offset & 7) ||
                kernel_rsp_offset == user_rsp_offset) {
                fail(expr, "fast syscall RSP offsets must be distinct, "
                           "8-byte-aligned nonnegative disp32 values");
            }

            fast_syscall_helper_used = true;
            fast_syscall_handler = dispatch;
            fast_syscall_bad_return_handler = bad_return;
            fast_syscall_kernel_rsp_offset =
                static_cast<uint32_t>(kernel_rsp_offset);
            fast_syscall_user_rsp_offset =
                static_cast<uint32_t>(user_rsp_offset);

            // IA32_EFER.SCE
            x.bytes({0xb9, 0x80, 0x00, 0x00, 0xc0,
                     0x0f, 0x32,
                     0x83, 0xc8, 0x01,
                     0x0f, 0x30});

            // IA32_STAR. SYSRET derives SS as user_cs-8 and CS as user_cs.
            const uint64_t star =
                ((user_cs - 16) << 48) | (kernel_cs << 32);
            x.mov_rax_imm(star);
            x.bytes({0x48, 0x89, 0xc2,
                     0x48, 0xc1, 0xea, 0x20,
                     0xb9, 0x81, 0x00, 0x00, 0xc0,
                     0x0f, 0x30});

            // IA32_LSTAR points at the compiler-generated stack-switch stub.
            const size_t lstar_address = x.rel32({0x48, 0x8d, 0x05});
            function_address_patches.push_back(
                {lstar_address, "__sura_fast_syscall_entry", expr->line});
            x.bytes({0x48, 0x89, 0xc2,
                     0x48, 0xc1, 0xea, 0x20,
                     0xb9, 0x82, 0x00, 0x00, 0xc0,
                     0x0f, 0x30});

            // IA32_FMASK clears unsafe user flags during kernel entry.
            x.mov_rax_imm(flags_mask);
            x.bytes({0x48, 0x89, 0xc2,
                     0x48, 0xc1, 0xea, 0x20,
                     0xb9, 0x84, 0x00, 0x00, 0xc0,
                     0x0f, 0x30});
            return true;
        }

        const auto memory_write = [&](unsigned width) {
            if (args.size() != 2) fail(expr, "memory write expects address and value");
            const size_t temporary = reserve_temporaries(1, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            store_rax_to_rcx(width);
            release_temporaries(1);
        };
        if (name == "mem_write8") { memory_write(8); return true; }
        if (name == "mem_write16") { memory_write(16); return true; }
        if (name == "mem_write32") { memory_write(32); return true; }
        if (name == "mem_write64") { memory_write(64); return true; }

        const auto atomic_store = [&](unsigned width) {
            if (args.size() != 2) {
                fail(expr, "atomic store expects address and value");
            }
            const size_t temporary = reserve_temporaries(1, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            // xchg with memory is implicitly locked and therefore provides
            // sequentially consistent store semantics on x86-64.
            if (width == 8) x.bytes({0x86, 0x01});
            else if (width == 16) x.bytes({0x66, 0x87, 0x01});
            else if (width == 32) x.bytes({0x87, 0x01});
            else x.bytes({0x48, 0x87, 0x01});
            release_temporaries(1);
        };
        if (name == "atomic_store" || name == "atomic_store64") {
            atomic_store(64); return true;
        }
        if (name == "atomic_store8") { atomic_store(8); return true; }
        if (name == "atomic_store16") { atomic_store(16); return true; }
        if (name == "atomic_store32") { atomic_store(32); return true; }
        if (name == "atomic_fence" || name == "atomic_load_fence" ||
            name == "atomic_store_fence") {
            if (!args.empty()) fail(expr, raw_name + "() takes no arguments");
            if (name == "atomic_load_fence") x.bytes({0x0f, 0xae, 0xe8});
            else if (name == "atomic_store_fence") x.bytes({0x0f, 0xae, 0xf8});
            else x.bytes({0x0f, 0xae, 0xf0});
            return true;
        }

        const auto port_write = [&](unsigned width) {
            if (args.size() != 2) fail(expr, "port output expects port and value");
            const size_t temporary = reserve_temporaries(1, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.mov_reg_rbp(2, scratch_slots[temporary]);
            if (width == 8) x.b(0xee);
            else if (width == 16) x.bytes({0x66, 0xef});
            else x.b(0xef);
            release_temporaries(1);
        };
        if (name == "io_out8") { port_write(8); return true; }
        if (name == "io_out16") { port_write(16); return true; }
        if (name == "io_out32") { port_write(32); return true; }

        if (name == "percpu_set_base" ||
            name == "percpu_set_kernel_base") {
            if (args.size() != 1) {
                fail(expr, raw_name + "(address) expects one value");
            }
            compile_expr(args[0].get());
            x.bytes({0x48, 0x89, 0xc2, 0x48, 0xc1, 0xea, 0x20});
            x.b(0xb9);
            x.d(name == "percpu_set_base" ? 0xc0000101U : 0xc0000102U);
            x.bytes({0x0f, 0x30});
            return true;
        }
        const auto percpu_write = [&](unsigned width) {
            if (args.size() != 2) {
                fail(expr, "percpu write expects byte offset and value");
            }
            const size_t temporary = reserve_temporaries(1, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            if (width == 8) x.bytes({0x65, 0x88, 0x01});
            else if (width == 16) x.bytes({0x65, 0x66, 0x89, 0x01});
            else if (width == 32) x.bytes({0x65, 0x89, 0x01});
            else x.bytes({0x65, 0x48, 0x89, 0x01});
            release_temporaries(1);
        };
        if (name == "percpu_write8") { percpu_write(8); return true; }
        if (name == "percpu_write16") { percpu_write(16); return true; }
        if (name == "percpu_write32") { percpu_write(32); return true; }
        if (name == "percpu_write64") { percpu_write(64); return true; }

        if (name == "apic_write" || name == "apic_eoi") {
            uint64_t offset = 0xb0;
            const Expr* value = nullptr;
            if (name == "apic_write") {
                if (args.size() != 2) {
                    fail(expr, "apic.write(register_offset, value) expects "
                               "two values");
                }
                offset = require_constant_integer(
                    args[0].get(), "APIC register offset");
                value = args[1].get();
            } else if (!args.empty()) {
                fail(expr, "apic.eoi() takes no arguments");
            }
            if (offset < 0x20 || offset > 0x3f0 || (offset & 0x0f)) {
                fail(expr, "APIC register offset must be 0x20..0x3f0 "
                           "and 16-byte aligned");
            }

            const size_t temporary = reserve_temporaries(2, expr);
            if (value) compile_expr(value);
            else x.bytes({0x31, 0xc0});
            x.mov_rbp_rax(scratch_slots[temporary]);
            x.bytes({0xb9, 0x1b, 0x00, 0x00, 0x00, 0x0f, 0x32,
                     0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0});
            x.mov_rbp_rax(scratch_slots[temporary + 1]);
            x.bytes({0x48, 0xa9, 0x00, 0x04, 0x00, 0x00});
            const size_t use_xapic = x.rel32({0x0f, 0x84});

            x.mov_rax_rbp(scratch_slots[temporary]);
            x.bytes({0x48, 0x89, 0xc2, 0x48, 0xc1, 0xea, 0x20});
            x.b(0xb9);
            x.d(static_cast<uint32_t>(0x800 + offset / 16));
            x.bytes({0x0f, 0x30});
            const size_t finished = x.rel32({0xe9});

            x.patch_rel32(use_xapic, x.pos());
            x.mov_reg_rbp(10, scratch_slots[temporary + 1]);
            x.bytes({0x49, 0x81, 0xe2, 0x00, 0xf0, 0xff, 0xff});
            x.mov_rax_rbp(scratch_slots[temporary]);
            x.bytes({0x41, 0x89, 0x82});
            x.d(static_cast<uint32_t>(offset));
            x.patch_rel32(finished, x.pos());
            release_temporaries(2);
            return true;
        }

        if (name == "apic_send_ipi" || name == "apic_send_init" ||
            name == "apic_send_startup") {
            const size_t expected_args =
                name == "apic_send_init" ? 1 : 2;
            if (args.size() != expected_args) {
                fail(expr, raw_name +
                           (expected_args == 1
                                ? "(destination) expects one value"
                                : "(destination, value) expects two values"));
            }
            uint64_t destination_constant = 0;
            if (constant_integer(args[0].get(), destination_constant) &&
                destination_constant > 0xffffffffULL) {
                fail(args[0].get(), "APIC destination must fit 32 bits");
            }
            if (name == "apic_send_startup") {
                uint64_t address = 0;
                if (constant_integer(args[1].get(), address) &&
                    ((address & 0xfff) || address < 0x1000 ||
                     address > 0xff000)) {
                    fail(args[1].get(), "SIPI trampoline address must be "
                                        "4 KiB aligned in 0x1000..0xff000");
                }
            }

            const size_t temporary = reserve_temporaries(3, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            if (name == "apic_send_ipi") {
                compile_expr(args[1].get());
            } else if (name == "apic_send_init") {
                x.mov_rax_imm(0xc500);
            } else {
                compile_expr(args[1].get());
                x.bytes({0x48, 0xc1, 0xe8, 0x0c,
                         0x25, 0xff, 0x00, 0x00, 0x00,
                         0x0d, 0x00, 0x06, 0x00, 0x00});
            }
            x.mov_rbp_rax(scratch_slots[temporary + 1]);
            x.bytes({0xb9, 0x1b, 0x00, 0x00, 0x00, 0x0f, 0x32,
                     0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0});
            x.mov_rbp_rax(scratch_slots[temporary + 2]);
            x.bytes({0x48, 0xa9, 0x00, 0x04, 0x00, 0x00});
            const size_t use_xapic = x.rel32({0x0f, 0x84});

            x.mov_rax_rbp(scratch_slots[temporary + 1]);
            x.mov_reg_rbp(2, scratch_slots[temporary]);
            x.bytes({0xb9, 0x30, 0x08, 0x00, 0x00, 0x0f, 0x30});
            const size_t finished = x.rel32({0xe9});

            x.patch_rel32(use_xapic, x.pos());
            x.mov_reg_rbp(10, scratch_slots[temporary + 2]);
            x.bytes({0x49, 0x81, 0xe2, 0x00, 0xf0, 0xff, 0xff});
            x.mov_rax_rbp(scratch_slots[temporary]);
            x.bytes({0x25, 0xff, 0x00, 0x00, 0x00,
                     0xc1, 0xe0, 0x18,
                     0x41, 0x89, 0x82, 0x10, 0x03, 0x00, 0x00});
            x.mov_rax_rbp(scratch_slots[temporary + 1]);
            x.bytes({0x41, 0x89, 0x82, 0x00, 0x03, 0x00, 0x00});
            x.patch_rel32(finished, x.pos());
            release_temporaries(3);
            return true;
        }

        if (name == "paging_write" ||
            name == "paging_map" ||
            name == "paging_clear") {
            const size_t expected =
                name == "paging_map" ? 4 :
                (name == "paging_write" ? 3 : 2);
            if (args.size() != expected) {
                if (name == "paging_map") {
                    fail(expr, "paging.map(table, index, physical_address, "
                               "flags) expects four values");
                }
                fail(expr, raw_name +
                           (expected == 3
                                ? "(table, index, entry) expects three values"
                                : "(table, index) expects two values"));
            }
            uint64_t constant_index = 0;
            if (constant_integer(args[1].get(), constant_index) &&
                constant_index > 511) {
                fail(args[1].get(), "page-table index must be 0..511");
            }
            if (name == "paging_map") {
                uint64_t constant_address = 0;
                if (constant_integer(args[2].get(), constant_address) &&
                    (constant_address & 0xfffU)) {
                    fail(args[2].get(), "paging.map() physical address must "
                                        "be 4 KiB aligned");
                }
            }

            const size_t temporary = reserve_temporaries(3, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.bytes({0x25, 0xff, 0x01, 0x00, 0x00,
                     0x48, 0xc1, 0xe0, 0x03});
            x.mov_rbp_rax(scratch_slots[temporary + 1]);

            if (name == "paging_clear") {
                x.bytes({0x31, 0xc0});
            } else if (name == "paging_write") {
                compile_expr(args[2].get());
            } else {
                compile_expr(args[2].get());
                x.bytes({0x48, 0xba});
                x.q(0x000ffffffffff000ULL);
                x.bytes({0x48, 0x21, 0xd0});
                x.mov_rbp_rax(scratch_slots[temporary + 2]);
                compile_expr(args[3].get());
                x.bytes({0x48, 0xba});
                x.q(0xfff0000000000fffULL);
                x.bytes({0x48, 0x21, 0xd0});
                x.mov_reg_rbp(1, scratch_slots[temporary + 2]);
                x.bytes({0x48, 0x09, 0xc8});
            }
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            x.mov_reg_rbp(2, scratch_slots[temporary + 1]);
            x.bytes({0x48, 0x01, 0xd1, 0x48, 0x89, 0x01});
            release_temporaries(3);
            return true;
        }
        if (name == "paging_activate") {
            if (args.size() != 1) {
                fail(expr, "paging.activate(root) expects one value");
            }
            uint64_t constant_address = 0;
            if (constant_integer(args[0].get(), constant_address) &&
                (constant_address & 0xfffU)) {
                fail(args[0].get(), "paging.activate() root must be 4 KiB aligned");
            }
            compile_expr(args[0].get());
            x.bytes({0x48, 0xba});
            x.q(0x000ffffffffff000ULL);
            x.bytes({0x48, 0x21, 0xd0, 0x0f, 0x22, 0xd8});
            return true;
        }
        if (name == "paging_invalidate") {
            if (args.size() != 1) {
                fail(expr, "paging.invalidate(address) expects one value");
            }
            compile_expr(args[0].get());
            x.bytes({0x0f, 0x01, 0x38});
            return true;
        }
        if (name == "paging_flush") {
            if (!args.empty()) fail(expr, "paging.flush() takes no arguments");
            x.bytes({0x0f, 0x20, 0xd8, 0x0f, 0x22, 0xd8});
            return true;
        }
        if (name == "context_switch") {
            if (args.size() != 2) {
                fail(expr, "context.switch(saved_rsp_address, next_rsp) "
                           "expects two values");
            }
            context_helpers_used = true;
            const size_t argument_base = save_call_args(args);
            load_call_args(2, argument_base);
            const size_t patch = x.rel32({0xe8});
            call_patches.push_back(
                {patch, "__sura_context_switch", expr->line});
            return true;
        }

        if (name == "cpu_gdt_set_tss") {
            if (args.size() != 4) {
                fail(expr, "cpu.gdt_set_tss(table, index, tss, limit) "
                           "expects four values");
            }
            const uint64_t index = require_constant_integer(
                args[1].get(), "TSS GDT index");
            const uint64_t limit = require_constant_integer(
                args[3].get(), "TSS limit");
            if (index == 0 || index > 8190) {
                fail(args[1].get(), "TSS GDT index must be 1..8190");
            }
            if (limit > 0xfffff) {
                fail(args[3].get(), "TSS limit must fit 20 bits");
            }
            if (args[0]->kind == NK::IDENT) {
                const std::string& table_name =
                    static_cast<const Ident*>(args[0].get())->name;
                const auto table = globals.find(table_name);
                if (table != globals.end() && table->second.address_value &&
                    index * 8 + 16 > table->second.size) {
                    fail(args[0].get(), "TSS descriptor exceeds static GDT '" +
                                        table_name + "'");
                }
            }
            if (args[2]->kind == NK::IDENT) {
                const std::string& tss_name =
                    static_cast<const Ident*>(args[2].get())->name;
                const auto tss = globals.find(tss_name);
                if (tss != globals.end() && tss->second.address_value &&
                    limit + 1 > tss->second.size) {
                    fail(args[2].get(), "TSS limit exceeds static object '" +
                                        tss_name + "'");
                }
            }

            const size_t temporary = reserve_temporaries(2, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[2].get());
            x.mov_rbp_rax(scratch_slots[temporary + 1]);
            x.mov_reg_rbp(10, scratch_slots[temporary]); // r10 = GDT base
            x.bytes({0x49, 0x81, 0xc2}); // add r10, index * 8
            x.d(static_cast<uint32_t>(index * 8));
            x.mov_reg_rbp(11, scratch_slots[temporary + 1]); // r11 = TSS base

            x.bytes({0x66, 0x41, 0xc7, 0x02});
            x.b(static_cast<uint8_t>(limit));
            x.b(static_cast<uint8_t>(limit >> 8));
            x.bytes({0x66, 0x45, 0x89, 0x5a, 0x02});
            x.bytes({0x4c, 0x89, 0xd8, 0x48, 0xc1, 0xe8, 0x10,
                     0x41, 0x88, 0x42, 0x04});
            x.bytes({0x41, 0xc6, 0x42, 0x05, 0x89}); // present, available TSS
            x.bytes({0x41, 0xc6, 0x42, 0x06,
                     static_cast<uint8_t>((limit >> 16) & 0x0f)});
            x.bytes({0x48, 0xc1, 0xe8, 0x08, 0x41, 0x88, 0x42, 0x07});
            x.bytes({0x4c, 0x89, 0xd8, 0x48, 0xc1, 0xe8, 0x20,
                     0x41, 0x89, 0x42, 0x08});
            x.bytes({0x41, 0xc7, 0x42, 0x0c, 0x00, 0x00, 0x00, 0x00});
            release_temporaries(2);
            return true;
        }

        if (name == "cpu_tss_set_rsp" || name == "cpu_tss_set_ist") {
            if (args.size() != 3) {
                fail(expr, raw_name + "(tss, index, stack_top) expects "
                           "three values");
            }
            const uint64_t index = require_constant_integer(
                args[1].get(),
                name == "cpu_tss_set_rsp" ? "TSS privilege level"
                                           : "TSS IST index");
            if (name == "cpu_tss_set_rsp" && index > 2) {
                fail(args[1].get(), "TSS RSP privilege level must be 0..2");
            }
            if (name == "cpu_tss_set_ist" &&
                (index == 0 || index > 7)) {
                fail(args[1].get(), "TSS IST index must be 1..7");
            }
            const uint8_t offset = static_cast<uint8_t>(
                name == "cpu_tss_set_rsp" ? 4 + index * 8
                                           : 28 + index * 8);
            const size_t temporary = reserve_temporaries(1, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[2].get());
            x.mov_reg_rbp(10, scratch_slots[temporary]);
            x.bytes({0x49, 0x89, 0x42, offset});
            release_temporaries(1);
            return true;
        }

        if (name == "cpu_tss_set_iomap") {
            if (args.size() != 2) {
                fail(expr, "cpu.tss_set_iomap(tss, offset) expects two values");
            }
            const uint64_t offset = require_constant_integer(
                args[1].get(), "TSS I/O-map offset");
            if (offset > 0xffff) {
                fail(args[1].get(), "TSS I/O-map offset must fit 16 bits");
            }
            compile_expr(args[0].get());
            x.bytes({0x66, 0xc7, 0x40, 0x66,
                     static_cast<uint8_t>(offset),
                     static_cast<uint8_t>(offset >> 8)});
            return true;
        }

        if (name == "cpu_idt_set_gate") {
            if (args.size() != 6) {
                fail(expr, "cpu.idt_set_gate(table, vector, handler, selector, "
                           "ist, attributes) expects six values");
            }
            const uint64_t vector = require_constant_integer(
                args[1].get(), "IDT vector");
            const uint64_t selector = require_constant_integer(
                args[3].get(), "IDT code selector");
            const uint64_t ist = require_constant_integer(
                args[4].get(), "IDT IST index");
            const uint64_t attributes = require_constant_integer(
                args[5].get(), "IDT attributes");
            if (vector > 255) fail(args[1].get(), "IDT vector must be 0..255");
            if (selector > 0xffff) {
                fail(args[3].get(), "IDT code selector must fit 16 bits");
            }
            if (ist > 7) fail(args[4].get(), "IDT IST index must be 0..7");
            if (attributes > 0xff) {
                fail(args[5].get(), "IDT attributes must fit 8 bits");
            }
            if (args[0]->kind == NK::IDENT) {
                const std::string& table_name =
                    static_cast<const Ident*>(args[0].get())->name;
                const auto table = globals.find(table_name);
                if (table != globals.end() && table->second.address_value &&
                    vector * 16 + 16 > table->second.size) {
                    fail(args[0].get(), "IDT vector exceeds static table '" +
                                        table_name + "'");
                }
            }
            std::string handler_call;
            const std::vector<ExprPtr>* handler_args = nullptr;
            if (!flatten_call(args[2].get(), handler_call, handler_args) ||
                canonical_intrinsic(handler_call) != "addr_of" ||
                handler_args->size() != 1 ||
                (*handler_args)[0]->kind != NK::IDENT) {
                fail(args[2].get(), "IDT handler must be written as "
                                    "addr_of(interrupt_function)");
            }
            const std::string handler_name =
                static_cast<const Ident*>((*handler_args)[0].get())->name;
            auto handler = functions.find(handler_name);
            if (handler == functions.end() ||
                (handler->second->abi != "interrupt" &&
                 handler->second->abi != "interrupt_error")) {
                fail(args[2].get(), "IDT handler '" + handler_name +
                                    "' must use interrupt or interrupt_error ABI");
            }
            const bool hardware_error_code =
                vector == 8 || vector == 10 || vector == 11 ||
                vector == 12 || vector == 13 || vector == 14 ||
                vector == 17 || vector == 21 || vector == 29 ||
                vector == 30;
            const bool handler_expects_error =
                handler->second->abi == "interrupt_error";
            if (hardware_error_code != handler_expects_error) {
                fail(args[2].get(), "IDT vector " + std::to_string(vector) +
                                    (hardware_error_code
                                         ? " pushes an error code and requires "
                                           "interrupt_error ABI"
                                         : " does not push an error code and "
                                           "requires interrupt ABI"));
            }

            const size_t temporary = reserve_temporaries(2, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[2].get());
            x.mov_rbp_rax(scratch_slots[temporary + 1]);
            x.mov_reg_rbp(10, scratch_slots[temporary]); // r10 = IDT base
            if (vector) {
                x.bytes({0x49, 0x81, 0xc2}); // add r10, vector * 16
                x.d(static_cast<uint32_t>(vector * 16));
            }
            x.mov_reg_rbp(11, scratch_slots[temporary + 1]); // r11 = handler
            x.bytes({0x66, 0x45, 0x89, 0x1a}); // offset[15:0]
            x.bytes({0x66, 0x41, 0xc7, 0x42, 0x02});
            x.b(static_cast<uint8_t>(selector));
            x.b(static_cast<uint8_t>(selector >> 8));
            x.bytes({0x41, 0xc6, 0x42, 0x04,
                     static_cast<uint8_t>(ist)});
            x.bytes({0x41, 0xc6, 0x42, 0x05,
                     static_cast<uint8_t>(attributes)});
            x.bytes({0x4c, 0x89, 0xd8, 0x48, 0xc1, 0xe8, 0x10,
                     0x66, 0x41, 0x89, 0x42, 0x06});
            x.bytes({0x4c, 0x89, 0xd8, 0x48, 0xc1, 0xe8, 0x20,
                     0x41, 0x89, 0x42, 0x08});
            x.bytes({0x41, 0xc7, 0x42, 0x0c, 0x00, 0x00, 0x00, 0x00});
            release_temporaries(2);
            return true;
        }

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
        if (name == "cpu_swapgs" || name == "cpu_stac" ||
            name == "cpu_clac" || name == "cpu_wbinvd" ||
            name == "cpu_clts" || name == "cpu_fninit") {
            if (!args.empty()) fail(expr, raw_name + "() takes no arguments");
            if (name == "cpu_swapgs") x.bytes({0x0f, 0x01, 0xf8});
            else if (name == "cpu_stac") x.bytes({0x0f, 0x01, 0xcb});
            else if (name == "cpu_clac") x.bytes({0x0f, 0x01, 0xca});
            else if (name == "cpu_wbinvd") x.bytes({0x0f, 0x09});
            else if (name == "cpu_clts") x.bytes({0x0f, 0x06});
            else x.bytes({0xdb, 0xe3});
            return true;
        }
        if (name == "cpu_load_task_register") {
            if (args.size() != 1) {
                fail(expr, "cpu.load_task_register(selector) expects one value");
            }
            compile_expr(args[0].get());
            x.bytes({0x0f, 0x00, 0xd8}); // ltr ax
            return true;
        }
        if (name == "cpu_reload_segments") {
            if (args.size() != 2) {
                fail(expr, "cpu.reload_segments(code_selector, data_selector) "
                           "expects two values");
            }
            const uint64_t code_selector = require_constant_integer(
                args[0].get(), "code selector");
            const uint64_t data_selector = require_constant_integer(
                args[1].get(), "data selector");
            if (code_selector > 0xffff || data_selector > 0xffff) {
                fail(expr, "segment selectors must fit 16 bits");
            }
            x.b(0xb8);
            x.d(static_cast<uint32_t>(code_selector));
            x.b(0x50);
            x.bytes({0x48, 0x8d, 0x05, 0x03, 0x00, 0x00, 0x00,
                     0x50, 0x48, 0xcb}); // far return to the next instruction
            x.b(0xb8);
            x.d(static_cast<uint32_t>(data_selector));
            x.bytes({0x8e, 0xd8, 0x8e, 0xc0, 0x8e, 0xd0});
            return true;
        }
        if (name == "cpu_load_gdt" || name == "cpu_load_idt" ||
            name == "cpu_invalidate_page") {
            if (name == "cpu_invalidate_page") {
                if (args.size() != 1) {
                    fail(expr, "cpu.invalidate_page() expects one address");
                }
                compile_expr(args[0].get());
                x.bytes({0x0f, 0x01, 0x38});
                return true;
            }
            if (args.size() != 1 && args.size() != 2) {
                fail(expr, raw_name +
                           "(descriptor_address) or (table, size) expected");
            }
            if (args.size() == 1) {
                compile_expr(args[0].get());
                if (name == "cpu_load_gdt") x.bytes({0x0f, 0x01, 0x10});
                else x.bytes({0x0f, 0x01, 0x18});
                return true;
            }
            const uint64_t size = require_constant_integer(
                args[1].get(),
                name == "cpu_load_gdt" ? "GDT byte size" : "IDT byte size");
            if (size == 0 || size > 65536) {
                fail(args[1].get(), "descriptor-table size must be 1..65536");
            }
            compile_expr(args[0].get());
            x.bytes({0x49, 0x89, 0xc2, 0x48, 0x83, 0xec, 0x10,
                     0x66, 0xc7, 0x04, 0x24});
            x.b(static_cast<uint8_t>(size - 1));
            x.b(static_cast<uint8_t>((size - 1) >> 8));
            x.bytes({0x4c, 0x89, 0x54, 0x24, 0x02});
            if (name == "cpu_load_gdt") {
                x.bytes({0x0f, 0x01, 0x14, 0x24});
            } else {
                x.bytes({0x0f, 0x01, 0x1c, 0x24});
            }
            x.bytes({0x48, 0x83, 0xc4, 0x10});
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
            const size_t temporary = reserve_temporaries(1, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.bytes({0x48, 0x89, 0xc2, 0x48, 0xc1, 0xea, 0x20});
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            x.bytes({0x0f, 0x30});
            release_temporaries(1);
            return true;
        }
        if (name == "cpu_xsetbv") {
            if (args.size() != 2) {
                fail(expr, "cpu.xsetbv(index, value) expects two values");
            }
            const size_t temporary = reserve_temporaries(1, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.bytes({0x48, 0x89, 0xc2, 0x48, 0xc1, 0xea, 0x20});
            x.mov_reg_rbp(1, scratch_slots[temporary]);
            x.bytes({0x0f, 0x01, 0xd1});
            release_temporaries(1);
            return true;
        }
        if (name == "cpu_fxsave" || name == "cpu_fxrstor") {
            if (args.size() != 1) {
                fail(expr, raw_name + "(area) expects one address");
            }
            compile_expr(args[0].get());
            if (name == "cpu_fxsave") x.bytes({0x48, 0x0f, 0xae, 0x00});
            else x.bytes({0x48, 0x0f, 0xae, 0x08});
            return true;
        }
        if (name == "cpu_xsave" || name == "cpu_xrstor") {
            if (args.size() != 2) {
                fail(expr, raw_name + "(area, state_mask) expects two values");
            }
            const size_t temporary = reserve_temporaries(1, expr);
            compile_expr(args[0].get());
            x.mov_rbp_rax(scratch_slots[temporary]);
            compile_expr(args[1].get());
            x.bytes({0x48, 0x89, 0xc2, 0x48, 0xc1, 0xea, 0x20});
            x.mov_reg_rbp(11, scratch_slots[temporary]);
            if (name == "cpu_xsave") x.bytes({0x49, 0x0f, 0xae, 0x23});
            else x.bytes({0x49, 0x0f, 0xae, 0x2b});
            release_temporaries(1);
            return true;
        }
        return false;
    }

    void compile_block(const SuraBlock* block) {
        if (!block) return;
        for (const auto& stmt : block->body) {
            if (top_level_global_statements.find(stmt.get()) !=
                top_level_global_statements.end()) {
                continue;
            }
            compile_stmt(stmt.get());
        }
    }

    void compile_stmt(const Stmt* stmt) {
        if (!stmt) return;
        switch (stmt->kind) {
            case NK::ASSIGN: {
                auto* assignment = static_cast<const AssignStmt*>(stmt);
                compile_expr(assignment->value.get());
                if (function_globals.find(assignment->name) !=
                    function_globals.end()) {
                    store_global_rax(assignment->name, assignment);
                } else {
                    auto local = slots.find(assignment->name);
                    if (local == slots.end()) {
                        fail(assignment, "unknown freestanding local '" +
                                         assignment->name + "'");
                    }
                    x.mov_rbp_rax(local->second);
                }
                return;
            }
            case NK::IN_PLACE: {
                auto* update = static_cast<const InPlaceStmt*>(stmt);
                const bool is_global =
                    function_globals.find(update->name) !=
                    function_globals.end();
                auto found = slots.find(update->name);
                if (is_global) load_global_rax(update->name, update);
                else if (found != slots.end()) x.mov_rax_rbp(found->second);
                else fail(stmt, "unknown freestanding local '" + update->name + "'");
                x.bytes({0x50, 0x48, 0x83, 0xec, 0x08});
                compile_expr(update->value.get());
                x.bytes({0x48, 0x83, 0xc4, 0x08, 0x59});
                if (update->op == "+") x.bytes({0x48, 0x01, 0xc1});
                else if (update->op == "-") x.bytes({0x48, 0x29, 0xc1});
                else if (update->op == "&") x.bytes({0x48, 0x21, 0xc1});
                else if (update->op == "|") x.bytes({0x48, 0x09, 0xc1});
                else if (update->op == "^") x.bytes({0x48, 0x31, 0xc1});
                else fail(stmt, "unsupported freestanding in-place operator");
                x.bytes({0x48, 0x89, 0xc8});
                if (is_global) store_global_rax(update->name, update);
                else x.mov_rbp_rax(found->second);
                return;
            }
            case NK::DOT_ASSIGN: {
                auto* assignment =
                    static_cast<const DotAssignStmt*>(stmt);
                Ident pointer(assignment->obj_name, assignment->line);
                const auto& field = typed_pointer_field(
                    &pointer, assignment->prop, assignment);
                if (field.size != 1 && field.size != 2 &&
                    field.size != 4 && field.size != 8) {
                    fail(assignment, "embedded struct fields cannot be assigned "
                                     "as a scalar");
                }
                const size_t temporary =
                    reserve_temporaries(1, assignment);
                compile_expr(assignment->value.get());
                x.mov_rbp_rax(scratch_slots[temporary]);
                compile_expr(&pointer);
                if (field.offset) {
                    x.bytes({0x48, 0x05});
                    x.d(static_cast<uint32_t>(field.offset));
                }
                x.bytes({0x48, 0x89, 0xc1}); // rcx = field address
                x.mov_rax_rbp(scratch_slots[temporary]);
                store_rax_to_rcx(static_cast<unsigned>(field.size * 8));
                release_temporaries(1);
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
            case NK::CLASS_DEF:
            case NK::USE:
            case NK::IMPORT:
            case NK::GLOBAL_DECL:
                return;
            default:
                fail(stmt, "statement is not available in the freestanding OS target");
        }
    }

    void emit_context_helpers() {
        if (!context_helpers_used) return;

        while (x.pos() & 15U) x.b(0x90);
        function_offsets["__sura_context_switch"] = x.pos();
        x.bytes({
            0x53,                         // push rbx
            0x55,                         // push rbp
            0x57,                         // push rdi
            0x56,                         // push rsi
            0x41, 0x54,                   // push r12
            0x41, 0x55,                   // push r13
            0x41, 0x56,                   // push r14
            0x41, 0x57,                   // push r15
            0x48, 0x89, 0x21,             // mov [rcx],rsp
            0x48, 0x89, 0xd4,             // mov rsp,rdx
            0x41, 0x5f,                   // pop r15
            0x41, 0x5e,                   // pop r14
            0x41, 0x5d,                   // pop r13
            0x41, 0x5c,                   // pop r12
            0x5e,                         // pop rsi
            0x5f,                         // pop rdi
            0x5d,                         // pop rbp
            0x5b,                         // pop rbx
            0xc3                          // ret to continuation/bootstrap
        });

        while (x.pos() & 15U) x.b(0x90);
        function_offsets["__sura_context_bootstrap"] = x.pos();
        x.bytes({
            0xfc,                         // cld
            0x4c, 0x89, 0xe9,             // mov rcx,r13 (task argument)
            0x48, 0x83, 0xec, 0x20,       // Win64 shadow space
            0x41, 0xff, 0xd4,             // call r12 (task entry)
            0x48, 0x83, 0xc4, 0x20,
            0x4d, 0x85, 0xf6,             // test r14,r14
            0x0f, 0x84, 0x0e, 0x00, 0x00, 0x00,
            0x48, 0x89, 0xc1,             // mov rcx,rax (task result)
            0x48, 0x83, 0xec, 0x20,
            0x41, 0xff, 0xd6,             // call r14 (exit handler)
            0x48, 0x83, 0xc4, 0x20,
            0xfa,                         // returned exit handler is fatal
            0xf4,
            0xeb, 0xfd
        });
    }

    void emit_preempt_helpers() {
        if (!preempt_helpers_used) return;

        while (x.pos() & 15U) x.b(0x90);
        function_offsets["__sura_preempt_frame_valid"] = x.pos();
        std::vector<size_t> invalid;
        x.bytes({0x31, 0xc0,                   // false by default
                 0x48, 0x85, 0xc9});          // frame != 0
        invalid.push_back(x.rel32({0x0f, 0x84}));
        x.bytes({0xf6, 0xc1, 0x07});           // 8-byte aligned
        invalid.push_back(x.rel32({0x0f, 0x85}));

        const auto validate_canonical_register =
            [&](std::initializer_list<uint8_t> move_to_rdx) {
                x.bytes(move_to_rdx);
                x.bytes({0x48, 0xc1, 0xea, 0x2f,
                         0x48, 0x85, 0xd2});
                const size_t canonical = x.rel32({0x0f, 0x84});
                x.bytes({0x48, 0x81, 0xfa,
                         0xff, 0xff, 0x01, 0x00});
                invalid.push_back(x.rel32({0x0f, 0x85}));
                x.patch_rel32(canonical, x.pos());
            };
        validate_canonical_register({0x48, 0x89, 0xca}); // frame pointer

        x.bytes({0x4c, 0x8b, 0x81, 0x80, 0x00, 0x00, 0x00,
                 0x4d, 0x85, 0xc0});          // saved RIP != 0
        invalid.push_back(x.rel32({0x0f, 0x84}));
        validate_canonical_register({0x4c, 0x89, 0xc2});

        // Kernel-only preemption: a non-null 16-bit CS with RPL 0.
        x.bytes({0x48, 0x8b, 0x91, 0x88, 0x00, 0x00, 0x00,
                 0x49, 0x89, 0xd0,
                 0x49, 0xc1, 0xe8, 0x10,
                 0x4d, 0x85, 0xc0});
        invalid.push_back(x.rel32({0x0f, 0x85}));
        x.bytes({0xf6, 0xc2, 0x03});
        invalid.push_back(x.rel32({0x0f, 0x85}));
        x.bytes({0x81, 0xe2, 0xfc, 0xff, 0x00, 0x00});
        invalid.push_back(x.rel32({0x0f, 0x84}));

        // Architectural reserved RFLAGS bit 1 must be set.
        x.bytes({0xf6, 0x81, 0x90, 0x00, 0x00, 0x00, 0x02});
        invalid.push_back(x.rel32({0x0f, 0x84}));
        x.bytes({0xb8, 0x01, 0x00, 0x00, 0x00});
        const size_t valid_return = x.rel32({0xe9});
        const size_t invalid_return = x.pos();
        for (size_t patch : invalid) {
            x.patch_rel32(patch, invalid_return);
        }
        x.bytes({0x31, 0xc0});
        x.patch_rel32(valid_return, x.pos());
        x.b(0xc3);

        while (x.pos() & 15U) x.b(0x90);
        function_offsets["__sura_preempt_resume"] = x.pos();
        x.bytes({0x49, 0x89, 0xca,             // r10 = frame
                 0x48, 0x83, 0xec, 0x28});
        const size_t validation_call = x.rel32({0xe8});
        call_patches.push_back(
            {validation_call, "__sura_preempt_frame_valid", 0});
        x.bytes({0x48, 0x83, 0xc4, 0x28,
                 0x85, 0xc0});
        const size_t invalid_frame = x.rel32({0x0f, 0x84});
        x.bytes({0xfa,                         // close maskable IRQ window
                 0x4c, 0x89, 0xd4,             // rsp = saved frame
                 0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c,
                 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58,
                 0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58,
                 0x48, 0x83, 0xc4, 0x08,       // normalized error code
                 0x48, 0xcf});                 // iretq
        x.patch_rel32(invalid_frame, x.pos());
        x.bytes({0x31, 0xc0, 0xc3});
    }

    void emit_user_frame_helpers() {
        if (!user_frame_helpers_used) return;

        while (x.pos() & 15U) x.b(0x90);
        function_offsets["__sura_user_frame_valid"] = x.pos();
        std::vector<size_t> invalid;
        x.bytes({0x31, 0xc0,                   // false by default
                 0x48, 0x85, 0xc9});          // frame != 0
        invalid.push_back(x.rel32({0x0f, 0x84}));
        x.bytes({0xf6, 0xc1, 0x07});           // 8-byte aligned
        invalid.push_back(x.rel32({0x0f, 0x85}));

        const auto validate_canonical_register =
            [&](std::initializer_list<uint8_t> move_to_rdx) {
                x.bytes(move_to_rdx);
                x.bytes({0x48, 0xc1, 0xea, 0x2f,
                         0x48, 0x85, 0xd2});
                const size_t canonical = x.rel32({0x0f, 0x84});
                x.bytes({0x48, 0x81, 0xfa,
                         0xff, 0xff, 0x01, 0x00});
                invalid.push_back(x.rel32({0x0f, 0x85}));
                x.patch_rel32(canonical, x.pos());
            };
        validate_canonical_register({0x48, 0x89, 0xca}); // frame pointer

        // User RIP must be a nonzero lower-half canonical address.
        x.bytes({0x4c, 0x8b, 0x81, 0x80, 0x00, 0x00, 0x00,
                 0x4d, 0x85, 0xc0});
        invalid.push_back(x.rel32({0x0f, 0x84}));
        x.bytes({0x4c, 0x89, 0xc2,
                 0x48, 0xc1, 0xea, 0x2f,
                 0x48, 0x85, 0xd2});
        invalid.push_back(x.rel32({0x0f, 0x85}));

        // User CS must be a non-null 16-bit selector with RPL 3.
        x.bytes({0x48, 0x8b, 0x91, 0x88, 0x00, 0x00, 0x00,
                 0x49, 0x89, 0xd0,
                 0x49, 0xc1, 0xe8, 0x10,
                 0x4d, 0x85, 0xc0});
        invalid.push_back(x.rel32({0x0f, 0x85}));
        x.bytes({0x89, 0xd0,
                 0x83, 0xe0, 0x03,
                 0x83, 0xf8, 0x03});
        invalid.push_back(x.rel32({0x0f, 0x85}));
        x.bytes({0x81, 0xe2, 0xfc, 0xff, 0x00, 0x00});
        invalid.push_back(x.rel32({0x0f, 0x84}));

        // Reserved bit 1 is required. Reject user IOPL, NT, and VM.
        x.bytes({0xf6, 0x81, 0x90, 0x00, 0x00, 0x00, 0x02});
        invalid.push_back(x.rel32({0x0f, 0x84}));
        x.bytes({0xf7, 0x81, 0x90, 0x00, 0x00, 0x00,
                 0x00, 0x70, 0x02, 0x00});
        invalid.push_back(x.rel32({0x0f, 0x85}));
        x.bytes({0x83, 0xb9, 0x94, 0x00, 0x00, 0x00, 0x00});
        invalid.push_back(x.rel32({0x0f, 0x85}));

        // User RSP must be a nonzero lower-half canonical address.
        x.bytes({0x4c, 0x8b, 0x81, 0x98, 0x00, 0x00, 0x00,
                 0x4d, 0x85, 0xc0});
        invalid.push_back(x.rel32({0x0f, 0x84}));
        x.bytes({0x4c, 0x89, 0xc2,
                 0x48, 0xc1, 0xea, 0x2f,
                 0x48, 0x85, 0xd2});
        invalid.push_back(x.rel32({0x0f, 0x85}));

        // User SS follows the same selector constraints as CS.
        x.bytes({0x48, 0x8b, 0x91, 0xa0, 0x00, 0x00, 0x00,
                 0x49, 0x89, 0xd0,
                 0x49, 0xc1, 0xe8, 0x10,
                 0x4d, 0x85, 0xc0});
        invalid.push_back(x.rel32({0x0f, 0x85}));
        x.bytes({0x89, 0xd0,
                 0x83, 0xe0, 0x03,
                 0x83, 0xf8, 0x03});
        invalid.push_back(x.rel32({0x0f, 0x85}));
        x.bytes({0x81, 0xe2, 0xfc, 0xff, 0x00, 0x00});
        invalid.push_back(x.rel32({0x0f, 0x84}));

        x.bytes({0xb8, 0x01, 0x00, 0x00, 0x00});
        const size_t valid_return = x.rel32({0xe9});
        const size_t invalid_return = x.pos();
        for (size_t patch : invalid) {
            x.patch_rel32(patch, invalid_return);
        }
        x.bytes({0x31, 0xc0});
        x.patch_rel32(valid_return, x.pos());
        x.b(0xc3);

        while (x.pos() & 15U) x.b(0x90);
        function_offsets["__sura_user_resume"] = x.pos();
        x.bytes({0x49, 0x89, 0xca,             // r10 = frame
                 0x48, 0x83, 0xec, 0x28});
        const size_t validation_call = x.rel32({0xe8});
        call_patches.push_back(
            {validation_call, "__sura_user_frame_valid", 0});
        x.bytes({0x48, 0x83, 0xc4, 0x28,
                 0x85, 0xc0});
        const size_t invalid_frame = x.rel32({0x0f, 0x84});
        x.bytes({0xfa,                         // close maskable IRQ window
                 0x4c, 0x89, 0xd4,             // rsp = saved frame
                 0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c,
                 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58,
                 0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58,
                 0x48, 0x83, 0xc4, 0x08,       // normalized error code
                 0x0f, 0x01, 0xf8,             // kernel GS -> user GS
                 0x0f, 0xae, 0xe8,             // lfence
                 0x48, 0xcf});                 // iretq
        x.patch_rel32(invalid_frame, x.pos());
        x.bytes({0x31, 0xc0, 0xc3});
    }

    void emit_fast_syscall_helper() {
        if (!fast_syscall_helper_used) return;

        while (x.pos() & 15U) x.b(0x90);
        function_offsets["__sura_fast_syscall_entry"] = x.pos();

        x.bytes({0x0f, 0x01, 0xf8,             // swapgs
                 0x65, 0x48, 0x89, 0x24, 0x25});
        x.d(fast_syscall_user_rsp_offset);      // gs:[user_rsp] = rsp
        x.bytes({0x65, 0x48, 0x8b, 0x24, 0x25});
        x.d(fast_syscall_kernel_rsp_offset);    // rsp = gs:[kernel_rsp]
        x.bytes({0x65, 0xff, 0x34, 0x25});
        x.d(fast_syscall_user_rsp_offset);      // saved frame user_rsp

        // Same GP ordering as interrupt frames, followed by user_rsp:
        // r15..r8, rdi, rsi, rbp, rbx, rdx, rcx, rax, user_rsp.
        x.bytes({0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,
                 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
                 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                 0xfc,
                 0x48, 0x89, 0xe5,             // rbp = frame
                 0x48, 0x83, 0xe4, 0xf0,
                 0x48, 0x83, 0xec, 0x20,
                 0x48, 0x89, 0xe9});            // rcx = frame
        const size_t dispatch_call = x.rel32({0xe8});
        call_patches.push_back(
            {dispatch_call, fast_syscall_handler, 0});
        x.bytes({0x48, 0x89, 0xec});             // rsp = frame

        std::vector<size_t> bad_return;
        x.bytes({0x48, 0x8b, 0x45, 0x68,       // saved user RIP
                 0x48, 0x85, 0xc0});
        bad_return.push_back(x.rel32({0x0f, 0x84}));
        x.bytes({0x48, 0x89, 0xc2,
                 0x48, 0xc1, 0xea, 0x2f,
                 0x48, 0x85, 0xd2});
        bad_return.push_back(x.rel32({0x0f, 0x85}));
        x.bytes({0x48, 0x8b, 0x45, 0x78,       // saved user RSP
                 0x48, 0x85, 0xc0});
        bad_return.push_back(x.rel32({0x0f, 0x84}));
        x.bytes({0x48, 0x89, 0xc2,
                 0x48, 0xc1, 0xea, 0x2f,
                 0x48, 0x85, 0xd2});
        bad_return.push_back(x.rel32({0x0f, 0x85}));

        // Return only a conservative user flag set. IF and arithmetic flags
        // survive; IOPL, NT, RF, VM, AC, TF, and DF are cleared.
        x.bytes({0x48, 0x81, 0x65, 0x20,
                 0xd7, 0x0a, 0x00, 0x00,
                 0x48, 0x83, 0x4d, 0x20, 0x02,
                 0x48, 0x89, 0xec,
                 0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c,
                 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58,
                 0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58,
                 0x65, 0x48, 0x8b, 0x24, 0x25});
        x.d(fast_syscall_user_rsp_offset);
        x.bytes({0x0f, 0x01, 0xf8,             // swapgs
                 0x48, 0x0f, 0x07});            // sysretq

        const size_t bad_return_target = x.pos();
        for (size_t patch : bad_return) {
            x.patch_rel32(patch, bad_return_target);
        }
        x.bytes({0x48, 0x89, 0xec,
                 0x48, 0x83, 0xe4, 0xf0,
                 0x48, 0x83, 0xec, 0x20,
                 0x48, 0x89, 0xe9});
        const size_t bad_handler_call = x.rel32({0xe8});
        call_patches.push_back(
            {bad_handler_call, fast_syscall_bad_return_handler, 0});
        x.bytes({0xfa, 0xf4, 0xeb, 0xfd});
    }

    void compile_function(const std::string& name, const FuncDef* function,
                          const SuraBlock* body, bool is_entry) {
        while (x.pos() & 15U) x.b(0x90);
        function_offsets[name] = x.pos();
        current_function = function;
        current_is_entry = is_entry;
        slots.clear();
        local_types.clear();
        function_globals.clear();
        repeat_slots.clear();
        scratch_slots.clear();
        return_patches.clear();
        next_slot = 0;
        temporary_depth = 0;
        call_argument_depth = 0;

        const bool is_interrupt =
            function && (function->abi == "interrupt" ||
                         function->abi == "interrupt_error");
        if (function && function->abi != "sura" && !is_interrupt) {
            fail(function, "unknown freestanding function ABI '" +
                           function->abi + "'");
        }
        if (is_entry && is_interrupt) {
            fail(function, "a UEFI entry function cannot use an interrupt ABI");
        }

        scan_global_declarations(body);
        if (function) {
            if (function->params.size() > 6) {
                fail(function, "freestanding functions support at most six parameters");
            }
            if (is_entry && function->params.size() > 2) {
                fail(function, "a UEFI entry function accepts at most image_handle "
                               "and system_table");
            }
            if (is_interrupt) {
                if (function->params.size() != 1) {
                    fail(function, "an interrupt function requires exactly one "
                                   "saved-frame pointer parameter");
                }
                if (function->param_types.empty() ||
                    annotated_type_name(function->param_types[0]).rfind(
                        "ptr", 0) != 0) {
                    fail(function, "an interrupt function parameter must have "
                                   "a ptr or ptr[StructName] annotation");
                }
            }
            for (size_t i = 0; i < function->params.size(); ++i) {
                const TypeAnnot* type =
                    i < function->param_types.size()
                        ? &function->param_types[i]
                        : nullptr;
                allocate_slot(function->params[i], type);
            }
        }
        scan_locals(body);
        // Nested intrinsic temporary cells plus independent argument banks.
        // Separate ranges prevent nested expressions from overwriting values
        // that an outer expression or call has already evaluated.
        for (unsigned i = 0;
             i < temporary_slot_count +
                     argument_bank_width * argument_bank_count;
             ++i) {
            scratch_slots.push_back(allocate_slot("__sura_arg_" + std::to_string(i)));
        }

        // 32 bytes of Win64 shadow space, 16 bytes for outgoing stack args,
        // then all fixed-width locals. RBP-relative locals therefore never
        // overlap the caller-owned argument area at RSP+32.
        frame_size = align_up_u32(
            static_cast<uint32_t>(next_slot * 8 + 32 + 16), 16);

        if (is_interrupt) {
            // Close the maskable interrupt window and switch from the user GS
            // base only when the hardware frame says the interrupted context
            // was ring 3. This test does not clobber a saved GP register.
            x.b(0xfa);
            const uint8_t cs_offset =
                function->abi == "interrupt" ? uint8_t{0x08} : uint8_t{0x10};
            x.bytes({0xf6, 0x44, 0x24, cs_offset, 0x01});
            const size_t kernel_gs_ready_low = x.rel32({0x0f, 0x84});
            x.bytes({0xf6, 0x44, 0x24, cs_offset, 0x02});
            const size_t kernel_gs_ready_high = x.rel32({0x0f, 0x84});
            x.bytes({0x0f, 0x01, 0xf8});       // user GS -> kernel GS
            x.patch_rel32(kernel_gs_ready_low, x.pos());
            x.patch_rel32(kernel_gs_ready_high, x.pos());
            x.bytes({0x0f, 0xae, 0xe8});       // serialize SWAPGS decision

            // Normalize both hardware frame shapes. Exceptions in the
            // `interrupt_error` set already have an error code at [rsp];
            // ordinary interrupts receive a synthetic zero error code.
            if (function->abi == "interrupt") x.bytes({0x6a, 0x00});

            // Save all general-purpose registers in a documented order:
            // r15..r8, rdi, rsi, rbp, rbx, rdx, rcx, rax at offsets 0..112.
            x.bytes({0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,
                     0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
                     0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                     0xfc}); // cld for the function-call ABI

            // Keep the saved frame at rbp+8. Align RSP before any nested
            // Win64-style Sura calls while retaining fixed RBP-relative locals.
            x.bytes({0x55, 0x48, 0x89, 0xe5,
                     0x48, 0x83, 0xe4, 0xf0,
                     0x48, 0x81, 0xec});
            x.d(frame_size);
            x.bytes({0x48, 0x8d, 0x85, 0x08, 0x00, 0x00, 0x00});
            x.mov_rbp_rax(slots.at(function->params[0]));
        } else {
            x.bytes({0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec});
            x.d(frame_size);
        }

        if (is_entry) {
            rip_data_disp({0x48, 0x89, 0x0d}, image_handle_data_offset);
            rip_data_disp({0x48, 0x89, 0x15}, system_table_data_offset);
        }

        if (function && !is_interrupt) {
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
        if (!is_interrupt) {
            x.bytes({0x31, 0xc0}); // implicit EFI_SUCCESS / integer zero
        }
        const size_t epilogue = x.pos();
        for (size_t patch : return_patches) x.patch_rel32(patch, epilogue);
        if (is_interrupt) {
            x.bytes({
                0x48, 0x89, 0xec, 0x5d, // mov rsp,rbp; pop rbp
                0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c,
                0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58,
                0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58,
                0x48, 0x83, 0xc4, 0x08, // discard real/synthetic error code
                0xfa,                   // no IRQ between test and IRETQ
                0xf6, 0x44, 0x24, 0x08, 0x01
            });
            const size_t kernel_return_low = x.rel32({0x0f, 0x84});
            x.bytes({0xf6, 0x44, 0x24, 0x08, 0x02});
            const size_t kernel_return_high = x.rel32({0x0f, 0x84});
            x.bytes({0x0f, 0x01, 0xf8});       // kernel GS -> user GS
            x.patch_rel32(kernel_return_low, x.pos());
            x.patch_rel32(kernel_return_high, x.pos());
            x.bytes({0x0f, 0xae, 0xe8,         // serialize SWAPGS decision
                     0x48, 0xcf});              // iretq
        } else {
            x.bytes({0xc9, 0xc3}); // leave; ret
        }
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
        context_helpers_used = false;
        preempt_helpers_used = false;
        user_frame_helpers_used = false;
        fast_syscall_helper_used = false;
        fast_syscall_handler.clear();
        fast_syscall_bad_return_handler.clear();
        fast_syscall_kernel_rsp_offset = 0;
        fast_syscall_user_rsp_offset = 0;
        functions.clear();
        struct_defs.clear();
        struct_layouts.clear();
        layouts_in_progress.clear();
        for (const auto& stmt : root->body) {
            if (stmt && stmt->kind == NK::FUNC_DEF) {
                auto* function = static_cast<const FuncDef*>(stmt.get());
                if (function->name.rfind("__sura_", 0) == 0) {
                    fail(function, "freestanding function names beginning "
                                   "with '__sura_' are reserved");
                }
                if (!functions.emplace(function->name, function).second) {
                    fail(function, "duplicate freestanding function '" + function->name + "'");
                }
                for (const auto& default_value : function->defaults) {
                    if (default_value) {
                        fail(function, "default function arguments are not yet "
                                       "available in the freestanding target");
                    }
                }
            } else if (stmt && stmt->kind == NK::CLASS_DEF) {
                auto* definition = static_cast<const ClassDef*>(stmt.get());
                if (definition->value_struct &&
                    !struct_defs.emplace(definition->name, definition).second) {
                    fail(definition, "duplicate freestanding struct '" +
                                     definition->name + "'");
                }
            }
        }
        build_top_level_declarations(root);
        for (const auto& global : globals) {
            if (functions.find(global.first) != functions.end() ||
                struct_defs.find(global.first) != struct_defs.end()) {
                fail(root, "top-level static declaration '" + global.first +
                           "' conflicts with a function or struct name");
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
        emit_context_helpers();
        emit_preempt_helpers();
        emit_user_frame_helpers();
        emit_fast_syscall_helper();

        for (const CallPatch& patch : call_patches) {
            auto target = function_offsets.find(patch.function);
            if (target == function_offsets.end()) {
                throw SuraOsCompileError(
                    "unresolved freestanding function '" + patch.function + "'",
                    patch.line);
            }
            x.patch_rel32(patch.displacement_offset, target->second);
        }
        for (const FunctionAddressPatch& patch : function_address_patches) {
            auto target = function_offsets.find(patch.function);
            if (target == function_offsets.end()) {
                throw SuraOsCompileError(
                    "unresolved freestanding function address '" +
                        patch.function + "'",
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
