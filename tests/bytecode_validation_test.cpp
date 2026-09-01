#include "../bytecode_io.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {

int passed = 0;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_rejected(const std::string& name, const std::function<void()>& action) {
    try {
        action();
    } catch (const std::exception&) {
        ++passed;
        return;
    }
    throw std::runtime_error(name + " was accepted");
}

JitChunk minimal_chunk() {
    JitChunk chunk;
    chunk.max_regs = 2;
    chunk.constants.push_back(Value(42.0));
    chunk.code.emplace_back(JitOp::LOAD_CONST, 0, 0, 0, 0);
    chunk.code.emplace_back(JitOp::RETURN_VAL, 0);
    return chunk;
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(out), "cannot create test package");
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "cannot write test package");
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "cannot read test package");
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    try {
        const JitChunk valid = minimal_chunk();
        const std::string valid_bytes = chunk_to_bytes(valid);
        JitChunk roundtrip = chunk_from_bytes(valid_bytes);
        require(roundtrip.code.size() == 2 && roundtrip.max_regs == 2,
                "valid bytecode roundtrip failed");
        ++passed;

        std::string truncated = valid_bytes;
        truncated.pop_back();
        expect_rejected("truncated bytecode", [&] { (void)chunk_from_bytes(truncated); });

        std::string trailing = valid_bytes + "x";
        expect_rejected("trailing bytecode data", [&] { (void)chunk_from_bytes(trailing); });

        std::string length_bomb = valid_bytes;
        require(length_bomb.size() > 10, "unexpected bytecode layout");
        for (size_t i = 7; i < 11; ++i) length_bomb[i] = static_cast<char>(0xff);
        expect_rejected("forged string-table count", [&] { (void)chunk_from_bytes(length_bomb); });

        // Layout of minimal_chunk(): 7-byte header, empty string table,
        // one numeric constant, empty globals, then the code count. The first
        // opcode is at byte 32.
        std::string bad_opcode = valid_bytes;
        require(bad_opcode.size() > 32, "unexpected instruction layout");
        bad_opcode[32] = static_cast<char>(0xff);
        expect_rejected("unknown opcode", [&] { (void)chunk_from_bytes(bad_opcode); });

        // Exercise every serialized byte with common corruption values. Some
        // mutations remain structurally valid, which is fine; the invariant is
        // that parsing either returns a validated chunk or throws a C++ exception.
        for (size_t offset = 0; offset < valid_bytes.size(); ++offset) {
            for (unsigned char replacement : {static_cast<unsigned char>(0x00),
                                              static_cast<unsigned char>(0x7f),
                                              static_cast<unsigned char>(0xff)}) {
                std::string fuzzed = valid_bytes;
                fuzzed[offset] = static_cast<char>(replacement);
                try { (void)chunk_from_bytes(fuzzed); } catch (const std::exception&) {}
            }
        }
        ++passed;

        // Fixed-seed mutations make failures reproducible while covering
        // truncation, insertion, replacement, and bit flips beyond the small
        // hand-written corruption corpus above.
        std::mt19937_64 random(0x535552414243ULL);
        for (size_t iteration = 0; iteration < 4096; ++iteration) {
            std::string fuzzed = valid_bytes;
            const size_t operation = static_cast<size_t>(random() % 4);
            if (operation == 0 && !fuzzed.empty()) {
                const size_t offset = static_cast<size_t>(random() % fuzzed.size());
                fuzzed[offset] ^= static_cast<char>(1U << (random() % 8));
            } else if (operation == 1 && !fuzzed.empty()) {
                fuzzed.resize(static_cast<size_t>(random() % fuzzed.size()));
            } else if (operation == 2) {
                const size_t offset = static_cast<size_t>(random() % (fuzzed.size() + 1));
                fuzzed.insert(fuzzed.begin() + static_cast<std::ptrdiff_t>(offset),
                              static_cast<char>(random() & 0xff));
            } else if (!fuzzed.empty()) {
                const size_t offset = static_cast<size_t>(random() % fuzzed.size());
                fuzzed[offset] = static_cast<char>(random() & 0xff);
            }
            try { (void)chunk_from_bytes(fuzzed); } catch (const std::exception&) {}
        }
        ++passed;

        JitChunk bad_register = valid;
        bad_register.code[0].a = 99;
        expect_rejected("out-of-frame register", [&] {
            (void)chunk_from_bytes(chunk_to_bytes(bad_register));
        });

        JitChunk bad_constant = valid;
        bad_constant.code[0].operand = 7;
        expect_rejected("out-of-range constant", [&] {
            (void)chunk_from_bytes(chunk_to_bytes(bad_constant));
        });

        JitChunk bad_jump;
        bad_jump.max_regs = 1;
        bad_jump.code.emplace_back(JitOp::JUMP, 0, 0, 0, 99);
        expect_rejected("jump outside frame", [&] {
            (void)chunk_from_bytes(chunk_to_bytes(bad_jump));
        });

        JitChunk cross_frame;
        cross_frame.max_regs = 1;
        cross_frame.code.emplace_back(JitOp::JUMP, 0, 0, 0, 1); // forged entry into function
        cross_frame.code.emplace_back(JitOp::RETURN_NONE);
        cross_frame.code.emplace_back(JitOp::HALT);
        JitFuncInfo nested;
        nested.entry_ip = 1;
        nested.end_ip = 2;
        nested.max_regs = 1;
        cross_frame.func_table.push_back(nested);
        expect_rejected("cross-frame jump", [&] {
            (void)chunk_from_bytes(chunk_to_bytes(cross_frame));
        });

        JitChunk bad_capture;
        bad_capture.max_regs = 1;
        bad_capture.code.emplace_back(JitOp::JUMP, 0, 0, 0, 2);
        bad_capture.code.emplace_back(JitOp::RETURN_NONE);
        bad_capture.code.emplace_back(JitOp::MAKE_LAMBDA, 0, 0, 0, 0);
        bad_capture.code.emplace_back(JitOp::HALT);
        JitFuncInfo capturing;
        capturing.entry_ip = 1;
        capturing.end_ip = 2;
        capturing.max_regs = 1;
        capturing.upvalues.push_back({true, 7});
        bad_capture.func_table.push_back(capturing);
        expect_rejected("lambda capture outside enclosing frame", [&] {
            (void)chunk_from_bytes(chunk_to_bytes(bad_capture));
        });

        // Do not silently widen a forged callable frame to fit its parameter
        // list. The interpreter allocates such a widened frame defensively,
        // but the native tier publishes max_regs as its exact active extent;
        // accepting this mismatch can make a native direct call bind arguments
        // beyond the root-scanned frame.
        JitChunk undersized_parameter_frame;
        undersized_parameter_frame.max_regs = 1;
        undersized_parameter_frame.code.emplace_back(JitOp::JUMP, 0, 0, 0, 2);
        undersized_parameter_frame.code.emplace_back(JitOp::RETURN_NONE);
        undersized_parameter_frame.code.emplace_back(JitOp::HALT);
        JitFuncInfo too_many_parameters;
        too_many_parameters.entry_ip = 1;
        too_many_parameters.end_ip = 2;
        too_many_parameters.max_regs = 1;
        too_many_parameters.params = {"left", "right"};
        undersized_parameter_frame.func_table.push_back(too_many_parameters);
        expect_rejected("function parameters outside declared frame", [&] {
            (void)chunk_from_bytes(chunk_to_bytes(undersized_parameter_frame));
        });

        JitChunk undersized_method_frame;
        undersized_method_frame.max_regs = 1;
        undersized_method_frame.code.emplace_back(JitOp::JUMP, 0, 0, 0, 2);
        undersized_method_frame.code.emplace_back(JitOp::RETURN_NONE);
        undersized_method_frame.code.emplace_back(JitOp::HALT);
        JitClassInfo undersized_class;
        undersized_class.name = "Undersized";
        JitMethodInfo too_many_method_parameters;
        too_many_method_parameters.entry_ip = 1;
        too_many_method_parameters.end_ip = 2;
        too_many_method_parameters.max_regs = 2; // self + only one parameter
        too_many_method_parameters.params = {"left", "right"};
        undersized_class.methods["bad"] = too_many_method_parameters;
        undersized_method_frame.class_table.push_back(undersized_class);
        expect_rejected("method parameters outside declared frame", [&] {
            (void)chunk_from_bytes(chunk_to_bytes(undersized_method_frame));
        });

        JitChunk bad_field;
        bad_field.code.emplace_back(JitOp::HALT);
        JitClassInfo field_class;
        field_class.name = "BadField";
        field_class.field_defaults.push_back(Value::nil());
        field_class.field_indices["value"] = 4;
        bad_field.class_table.push_back(field_class);
        expect_rejected("class field slot outside defaults", [&] {
            (void)chunk_from_bytes(chunk_to_bytes(bad_field));
        });

        JitChunk class_cycle;
        class_cycle.code.emplace_back(JitOp::HALT);
        JitClassInfo class_a;
        class_a.name = "A";
        class_a.parent = "B";
        JitClassInfo class_b;
        class_b.name = "B";
        class_b.parent = "A";
        class_cycle.class_table.push_back(class_a);
        class_cycle.class_table.push_back(class_b);
        expect_rejected("class inheritance cycle", [&] {
            (void)chunk_from_bytes(chunk_to_bytes(class_cycle));
        });

        const std::filesystem::path temp_dir =
            std::filesystem::temp_directory_path() / "sura-bytecode-validation";
        std::filesystem::create_directories(temp_dir);
        const std::filesystem::path valid_release = temp_dir / "valid.srp";
        save_release_package(valid, valid_release.u8string());
        JitChunk release_roundtrip = load_release_package(valid_release.u8string());
        require(release_roundtrip.code.size() == valid.code.size(),
                "valid release package roundtrip failed");
        ++passed;

        std::string release_bytes = read_bytes(valid_release);
        std::string corrupt_release = release_bytes;
        corrupt_release.back() ^= static_cast<char>(0x5a);
        write_bytes(temp_dir / "corrupt.srp", corrupt_release);
        expect_rejected("corrupt sealed release payload", [&] {
            (void)load_release_package((temp_dir / "corrupt.srp").u8string());
        });

        write_bytes(temp_dir / "trailing.srp", release_bytes + "x");
        expect_rejected("trailing release data", [&] {
            (void)load_release_package((temp_dir / "trailing.srp").u8string());
        });

        require(release_bytes.size() > 10, "unexpected release package layout");
        for (size_t i = 6; i < 10; ++i) release_bytes[i] = static_cast<char>(0xff);
        write_bytes(temp_dir / "length-bomb.srp", release_bytes);
        expect_rejected("forged release nonce length", [&] {
            (void)load_release_package((temp_dir / "length-bomb.srp").u8string());
        });

        std::error_code ignored;
        std::filesystem::remove_all(temp_dir, ignored);
        // Successful parses GC-allocate constant strings; establish the
        // leak-checkable lifetime boundary GC::shutdown() exists for, so the
        // ASan/LSan lane verifies the parser itself rather than test teardown.
        GC::shutdown();
        std::cout << "bytecode validation: " << passed << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        GC::shutdown();
        std::cerr << "bytecode validation FAILED: " << error.what() << "\n";
        return 1;
    }
}
