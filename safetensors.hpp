#pragma once

// Native Safetensors weight I/O for Sura autograd tensors.
//
// This header is included after autograd.hpp and the stdlib argument helpers.
// It deliberately does not depend on Python, PyTorch, protobuf, or the custom
// Sura checkpoint format. Safetensors stores a bounded JSON header followed by
// a contiguous, little-endian tensor byte buffer.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace SuraStd {

static constexpr size_t SAFETENSORS_MAX_TENSORS = 100000;
static constexpr size_t SAFETENSORS_MAX_NAME_BYTES = 512;
static constexpr size_t SAFETENSORS_MAX_PATH_BYTES = 32768;
static constexpr size_t SAFETENSORS_MAX_HEADER_BYTES = 16ULL * 1024ULL * 1024ULL;
static constexpr size_t SAFETENSORS_MAX_METADATA_ENTRIES = 10000;
static constexpr size_t SAFETENSORS_MAX_METADATA_STRING_BYTES = 4096;
static constexpr uint64_t SAFETENSORS_MAX_FILE_BYTES =
    65ULL * 1024ULL * 1024ULL * 1024ULL;
static constexpr size_t SAFETENSORS_IO_CHUNK_BYTES = 64ULL * 1024ULL;

inline bool safetensors_valid_utf8(const std::string& text) {
    size_t index = 0;
    while (index < text.size()) {
        unsigned char first = (unsigned char)text[index++];
        if (first <= 0x7fU) continue;
        unsigned needed = 0;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        if ((first & 0xe0U) == 0xc0U) {
            needed = 1;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            needed = 2;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            needed = 3;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (index + needed > text.size()) return false;
        for (unsigned part = 0; part < needed; ++part) {
            unsigned char next = (unsigned char)text[index++];
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU
            || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

inline void safetensors_validate_path(const char* api, const std::string& path,
                                      int line) {
    if (path.empty() || path.size() > SAFETENSORS_MAX_PATH_BYTES) {
        throw JitThrow{std::string(api) + "(): path must contain 1.."
                       + std::to_string(SAFETENSORS_MAX_PATH_BYTES) + " UTF-8 bytes",
                       line};
    }
    if (path.find('\0') != std::string::npos || !safetensors_valid_utf8(path)) {
        throw JitThrow{std::string(api) + "(): path must be valid UTF-8 without NUL bytes",
                       line};
    }
}

inline void safetensors_validate_name(const char* api, const std::string& name,
                                      int line) {
    if (name.empty() || name.size() > SAFETENSORS_MAX_NAME_BYTES) {
        throw JitThrow{std::string(api) + "(): tensor names must contain 1.."
                       + std::to_string(SAFETENSORS_MAX_NAME_BYTES) + " UTF-8 bytes",
                       line};
    }
    if (name == "__metadata__") {
        throw JitThrow{std::string(api) + "(): '__metadata__' is a reserved tensor name",
                       line};
    }
    if (name.find('\0') != std::string::npos || !safetensors_valid_utf8(name)) {
        throw JitThrow{std::string(api)
                       + "(): tensor names must be valid UTF-8 without NUL bytes", line};
    }
}

inline uint64_t safetensors_checked_add(uint64_t left, uint64_t right,
                                        const char* api, int line) {
    if (right > std::numeric_limits<uint64_t>::max() - left) {
        throw JitThrow{std::string(api) + "(): file size overflow", line};
    }
    return left + right;
}

inline uint64_t safetensors_checked_mul(uint64_t left, uint64_t right,
                                        const char* api, int line) {
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
        throw JitThrow{std::string(api) + "(): tensor size overflow", line};
    }
    return left * right;
}

inline const char* safetensors_dtype_code(TensorDType dtype) {
    switch (dtype) {
        case TensorDType::FLOAT64: return "F64";
        case TensorDType::FLOAT32: return "F32";
        case TensorDType::FLOAT16: return "F16";
        case TensorDType::BFLOAT16: return "BF16";
    }
    return nullptr;
}

inline TensorDType safetensors_parse_dtype_code(const std::string& code,
                                                int line) {
    if (code == "F64") return TensorDType::FLOAT64;
    if (code == "F32") return TensorDType::FLOAT32;
    if (code == "F16") return TensorDType::FLOAT16;
    if (code == "BF16") return TensorDType::BFLOAT16;
    throw JitThrow{"autograd_load_safetensors(): unsupported dtype '" + code
                   + "' (supported: F64, F32, F16, BF16)", line};
}

inline void safetensors_append_utf8(std::string& out, uint32_t codepoint,
                                    const char* api, int line) {
    if (codepoint > 0x10ffffU
        || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        throw JitThrow{std::string(api) + "(): invalid Unicode codepoint in header", line};
    }
    if (codepoint <= 0x7fU) {
        out.push_back((char)codepoint);
    } else if (codepoint <= 0x7ffU) {
        out.push_back((char)(0xc0U | (codepoint >> 6)));
        out.push_back((char)(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        out.push_back((char)(0xe0U | (codepoint >> 12)));
        out.push_back((char)(0x80U | ((codepoint >> 6) & 0x3fU)));
        out.push_back((char)(0x80U | (codepoint & 0x3fU)));
    } else {
        out.push_back((char)(0xf0U | (codepoint >> 18)));
        out.push_back((char)(0x80U | ((codepoint >> 12) & 0x3fU)));
        out.push_back((char)(0x80U | ((codepoint >> 6) & 0x3fU)));
        out.push_back((char)(0x80U | (codepoint & 0x3fU)));
    }
}

inline std::string safetensors_json_escape(const std::string& text) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20U) {
                    out += "\\u00";
                    out.push_back(hex[(ch >> 4) & 0x0fU]);
                    out.push_back(hex[ch & 0x0fU]);
                } else {
                    out.push_back((char)ch);
                }
        }
    }
    return out;
}

struct SafetensorsRecord {
    std::string name;
    TensorDType dtype = TensorDType::FLOAT64;
    std::vector<size_t> shape;
    uint64_t begin = 0;
    uint64_t end = 0;
    size_t numel = 0;
};

class SafetensorsHeaderParser {
    const std::string& text_;
    size_t position_ = 0;
    int line_ = 0;
    static constexpr const char* api_ = "autograd_load_safetensors";

    [[noreturn]] void fail(const std::string& message) const {
        throw JitThrow{std::string(api_) + "(): " + message, line_};
    }

    void skip_space() {
        while (position_ < text_.size()) {
            char ch = text_[position_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
            ++position_;
        }
    }

    bool consume(char expected) {
        skip_space();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected, const char* description) {
        if (!consume(expected)) fail(std::string("expected ") + description + " in header");
    }

    static int hex_digit(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }

    uint32_t parse_hex4() {
        if (position_ + 4 > text_.size()) fail("truncated Unicode escape");
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            int digit = hex_digit(text_[position_++]);
            if (digit < 0) fail("invalid Unicode escape");
            value = (value << 4) | (uint32_t)digit;
        }
        return value;
    }

    std::string parse_string() {
        skip_space();
        if (position_ >= text_.size() || text_[position_] != '"') {
            fail("expected JSON string");
        }
        ++position_;
        std::string out;
        while (position_ < text_.size()) {
            unsigned char ch = (unsigned char)text_[position_++];
            if (ch == '"') {
                if (!safetensors_valid_utf8(out)) fail("header string is not valid UTF-8");
                if (out.find('\0') != std::string::npos) fail("header strings may not contain NUL");
                return out;
            }
            if (ch < 0x20U) fail("unescaped control character in header string");
            if (ch != '\\') {
                out.push_back((char)ch);
                continue;
            }
            if (position_ >= text_.size()) fail("truncated string escape");
            char escaped = text_[position_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t first = parse_hex4();
                    uint32_t codepoint = first;
                    if (first >= 0xd800U && first <= 0xdbffU) {
                        if (position_ + 2 > text_.size()
                            || text_[position_] != '\\' || text_[position_ + 1] != 'u') {
                            fail("high surrogate is not followed by a low surrogate");
                        }
                        position_ += 2;
                        uint32_t second = parse_hex4();
                        if (second < 0xdc00U || second > 0xdfffU) {
                            fail("high surrogate is not followed by a low surrogate");
                        }
                        codepoint = 0x10000U
                                  + ((first - 0xd800U) << 10)
                                  + (second - 0xdc00U);
                    } else if (first >= 0xdc00U && first <= 0xdfffU) {
                        fail("isolated low surrogate in header string");
                    }
                    safetensors_append_utf8(out, codepoint, api_, line_);
                    break;
                }
                default: fail("invalid string escape");
            }
        }
        fail("unterminated header string");
    }

    uint64_t parse_uint() {
        skip_space();
        if (position_ >= text_.size() || text_[position_] < '0'
            || text_[position_] > '9') {
            fail("expected a non-negative integer");
        }
        if (text_[position_] == '0'
            && position_ + 1 < text_.size()
            && text_[position_ + 1] >= '0' && text_[position_ + 1] <= '9') {
            fail("integers may not contain leading zeroes");
        }
        uint64_t value = 0;
        do {
            unsigned digit = (unsigned)(text_[position_] - '0');
            if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
                fail("integer exceeds uint64 range");
            }
            value = value * 10U + digit;
            ++position_;
        } while (position_ < text_.size()
                 && text_[position_] >= '0' && text_[position_] <= '9');
        return value;
    }

    std::vector<size_t> parse_shape() {
        expect('[', "'['");
        std::vector<size_t> shape;
        if (consume(']')) return shape;
        while (true) {
            if (shape.size() >= AG_MAX_RANK) fail("tensor rank exceeds the Sura rank limit");
            uint64_t dimension = parse_uint();
            if (dimension == 0) fail("zero-sized tensor dimensions are not supported by Sura");
            if (dimension > (uint64_t)ag_max_elements()
                || dimension > (uint64_t)std::numeric_limits<size_t>::max()) {
                fail("tensor dimension exceeds the Sura element limit");
            }
            shape.push_back((size_t)dimension);
            if (consume(']')) break;
            expect(',', "','");
        }
        return shape;
    }

    std::pair<uint64_t, uint64_t> parse_offsets() {
        expect('[', "'['");
        uint64_t begin = parse_uint();
        expect(',', "','");
        uint64_t end = parse_uint();
        expect(']', "']'");
        return {begin, end};
    }

    void parse_metadata() {
        expect('{', "'{'");
        std::unordered_set<std::string> keys;
        if (consume('}')) return;
        while (true) {
            if (keys.size() >= SAFETENSORS_MAX_METADATA_ENTRIES) {
                fail("metadata contains too many entries");
            }
            std::string key = parse_string();
            if (key.size() > SAFETENSORS_MAX_METADATA_STRING_BYTES) {
                fail("metadata key is too long");
            }
            if (!keys.insert(key).second) fail("duplicate metadata key '" + key + "'");
            expect(':', "':'");
            std::string value = parse_string();
            if (value.size() > SAFETENSORS_MAX_METADATA_STRING_BYTES) {
                fail("metadata value is too long");
            }
            if (consume('}')) break;
            expect(',', "','");
        }
    }

    SafetensorsRecord parse_tensor(const std::string& name) {
        SafetensorsRecord record;
        record.name = name;
        bool has_dtype = false;
        bool has_shape = false;
        bool has_offsets = false;
        expect('{', "'{'");
        std::unordered_set<std::string> fields;
        if (consume('}')) fail("tensor entry '" + name + "' is empty");
        while (true) {
            std::string field = parse_string();
            if (!fields.insert(field).second) {
                fail("duplicate field '" + field + "' in tensor '" + name + "'");
            }
            expect(':', "':'");
            if (field == "dtype") {
                record.dtype = safetensors_parse_dtype_code(parse_string(), line_);
                has_dtype = true;
            } else if (field == "shape") {
                record.shape = parse_shape();
                has_shape = true;
            } else if (field == "data_offsets") {
                auto offsets = parse_offsets();
                record.begin = offsets.first;
                record.end = offsets.second;
                has_offsets = true;
            } else {
                fail("unknown tensor field '" + field + "'");
            }
            if (consume('}')) break;
            expect(',', "','");
        }
        if (!has_dtype || !has_shape || !has_offsets) {
            fail("tensor '" + name + "' must define dtype, shape, and data_offsets");
        }
        record.numel = ag_numel(api_, record.shape, line_);
        uint64_t expected = safetensors_checked_mul(
            (uint64_t)record.numel, (uint64_t)tensor_dtype_size(record.dtype), api_, line_);
        if (record.end < record.begin || record.end - record.begin != expected) {
            fail("tensor '" + name + "' byte range does not match its dtype and shape");
        }
        return record;
    }

public:
    SafetensorsHeaderParser(const std::string& text, int line)
        : text_(text), line_(line) {}

    std::vector<SafetensorsRecord> parse() {
        if (text_.empty() || text_[0] != '{') {
            fail("header must begin with a JSON object");
        }
        expect('{', "'{'");
        std::vector<SafetensorsRecord> records;
        std::unordered_set<std::string> root_keys;
        if (consume('}')) fail("header contains no tensors");
        while (true) {
            std::string key = parse_string();
            if (!root_keys.insert(key).second) fail("duplicate root key '" + key + "'");
            expect(':', "':'");
            if (key == "__metadata__") {
                parse_metadata();
            } else {
                safetensors_validate_name(api_, key, line_);
                if (records.size() >= SAFETENSORS_MAX_TENSORS) fail("too many tensors");
                records.push_back(parse_tensor(key));
            }
            if (consume('}')) break;
            expect(',', "','");
        }
        skip_space();
        if (position_ != text_.size()) fail("trailing non-whitespace bytes in header");
        if (records.empty()) fail("header contains no tensors");
        return records;
    }
};

inline void safetensors_store_u16(unsigned char* destination, uint16_t value) {
    destination[0] = (unsigned char)(value & 0xffU);
    destination[1] = (unsigned char)((value >> 8) & 0xffU);
}

inline void safetensors_store_u32(unsigned char* destination, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        destination[i] = (unsigned char)((value >> (8U * i)) & 0xffU);
    }
}

inline void safetensors_store_u64(unsigned char* destination, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        destination[i] = (unsigned char)((value >> (8U * i)) & 0xffU);
    }
}

inline uint16_t safetensors_load_u16(const unsigned char* source) {
    return (uint16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8));
}

inline uint32_t safetensors_load_u32(const unsigned char* source) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)source[i] << (8U * i);
    return value;
}

inline uint64_t safetensors_load_u64(const unsigned char* source) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)source[i] << (8U * i);
    return value;
}

inline uint16_t safetensors_float_to_half(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000U;
    uint32_t exponent = (bits >> 23) & 0xffU;
    uint32_t mantissa = bits & 0x7fffffU;
    if (exponent == 0xffU) {
        return (uint16_t)(sign | (mantissa ? 0x7e00U : 0x7c00U));
    }
    int adjusted = (int)exponent - 127 + 15;
    if (adjusted >= 31) return (uint16_t)(sign | 0x7c00U);
    if (adjusted <= 0) {
        if (adjusted < -10) return (uint16_t)sign;
        mantissa |= 0x800000U;
        unsigned shift = (unsigned)(14 - adjusted);
        uint32_t result = mantissa >> shift;
        uint32_t remainder = mantissa & ((1U << shift) - 1U);
        uint32_t halfway = 1U << (shift - 1U);
        if (remainder > halfway || (remainder == halfway && (result & 1U))) ++result;
        return (uint16_t)(sign | result);
    }
    uint32_t result = mantissa >> 13;
    uint32_t remainder = mantissa & 0x1fffU;
    if (remainder > 0x1000U || (remainder == 0x1000U && (result & 1U))) {
        ++result;
        if (result == 0x400U) {
            result = 0;
            ++adjusted;
            if (adjusted >= 31) return (uint16_t)(sign | 0x7c00U);
        }
    }
    return (uint16_t)(sign | ((uint32_t)adjusted << 10) | result);
}

inline float safetensors_half_to_float(uint16_t value) {
    uint32_t sign = ((uint32_t)value & 0x8000U) << 16;
    uint32_t exponent = ((uint32_t)value >> 10) & 0x1fU;
    uint32_t mantissa = (uint32_t)value & 0x3ffU;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int normalized_exponent = 1;
            while ((mantissa & 0x400U) == 0) {
                mantissa <<= 1;
                --normalized_exponent;
            }
            mantissa &= 0x3ffU;
            uint32_t float_exponent = (uint32_t)(normalized_exponent + (127 - 15));
            bits = sign | (float_exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1fU) {
        bits = sign | 0x7f800000U | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

inline uint16_t safetensors_float_to_bfloat16(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bits += 0x7fffU + ((bits >> 16) & 1U);
    return (uint16_t)(bits >> 16);
}

inline float safetensors_bfloat16_to_float(uint16_t value) {
    uint32_t bits = (uint32_t)value << 16;
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

inline void safetensors_encode_value(unsigned char* destination,
                                     TensorDType dtype, double value,
                                     const char* api, int line) {
    if (!std::isfinite(value)) {
        throw JitThrow{std::string(api) + "(): tensor data must be finite", line};
    }
    switch (dtype) {
        case TensorDType::FLOAT64: {
            uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            safetensors_store_u64(destination, bits);
            return;
        }
        case TensorDType::FLOAT32: {
            float converted = (float)value;
            if (!std::isfinite(converted)) {
                throw JitThrow{std::string(api) + "(): value overflows float32", line};
            }
            uint32_t bits = 0;
            std::memcpy(&bits, &converted, sizeof(bits));
            safetensors_store_u32(destination, bits);
            return;
        }
        case TensorDType::FLOAT16: {
            float converted = (float)value;
            uint16_t bits = safetensors_float_to_half(converted);
            if ((bits & 0x7c00U) == 0x7c00U) {
                throw JitThrow{std::string(api) + "(): value overflows float16", line};
            }
            safetensors_store_u16(destination, bits);
            return;
        }
        case TensorDType::BFLOAT16: {
            float converted = (float)value;
            if (!std::isfinite(converted)) {
                throw JitThrow{std::string(api) + "(): value overflows bfloat16", line};
            }
            uint16_t bits = safetensors_float_to_bfloat16(converted);
            if ((bits & 0x7f80U) == 0x7f80U) {
                throw JitThrow{std::string(api) + "(): value overflows bfloat16", line};
            }
            safetensors_store_u16(destination, bits);
            return;
        }
    }
    throw JitThrow{std::string(api) + "(): unsupported tensor dtype", line};
}

inline double safetensors_decode_value(const unsigned char* source,
                                       TensorDType dtype,
                                       const char* api, int line) {
    double value = 0.0;
    switch (dtype) {
        case TensorDType::FLOAT64: {
            uint64_t bits = safetensors_load_u64(source);
            std::memcpy(&value, &bits, sizeof(value));
            break;
        }
        case TensorDType::FLOAT32: {
            uint32_t bits = safetensors_load_u32(source);
            float converted = 0.0f;
            std::memcpy(&converted, &bits, sizeof(converted));
            value = (double)converted;
            break;
        }
        case TensorDType::FLOAT16:
            value = (double)safetensors_half_to_float(safetensors_load_u16(source));
            break;
        case TensorDType::BFLOAT16:
            value = (double)safetensors_bfloat16_to_float(safetensors_load_u16(source));
            break;
    }
    if (!std::isfinite(value)) {
        throw JitThrow{std::string(api) + "(): non-finite tensor value is not supported",
                       line};
    }
    return value;
}

struct SafetensorsSaveRecord {
    std::string name;
    GCTensor* tensor = nullptr;
    uint64_t begin = 0;
    uint64_t end = 0;
};

inline std::vector<SafetensorsSaveRecord> safetensors_collect_save_records(
    const Value& state, int line) {
    const char* api = "autograd_save_safetensors";
    GCDict* dict = need_dict(api, state, 0, line);
    if (dict->elements.empty()) {
        throw JitThrow{"autograd_save_safetensors(): state_dict must not be empty", line};
    }
    if (dict->elements.size() > SAFETENSORS_MAX_TENSORS) {
        throw JitThrow{"autograd_save_safetensors(): too many tensors", line};
    }
    std::vector<SafetensorsSaveRecord> records;
    records.reserve(dict->elements.size());
    std::unordered_set<GCTensor*> identities;
    for (const auto& entry : dict->elements) {
        safetensors_validate_name(api, entry.first, line);
        if (!entry.second.is_tensor()) {
            throw JitThrow{"autograd_save_safetensors(): state_dict values must be tensors",
                           line};
        }
        GCTensor* tensor = entry.second.as_tensor();
        if (!identities.insert(tensor).second) {
            throw JitThrow{"autograd_save_safetensors(): the same tensor appears under multiple names",
                           line};
        }
        ag_cuda_materialize_host(tensor, api, line);
        size_t count = ag_numel(api, tensor->shape, line);
        if (count != tensor->data.size()) {
            throw JitThrow{"autograd_save_safetensors(): tensor shape/size mismatch", line};
        }
        size_t element_bytes = tensor_dtype_size(tensor->data.dtype());
        if (element_bytes == 0
            || !tensor->data.host_readable()
            || count > std::numeric_limits<size_t>::max() / element_bytes
            || tensor->data.byte_size() != count * element_bytes
            || tensor->data.host_byte_size() != count * element_bytes
            || !safetensors_dtype_code(tensor->data.dtype())) {
            throw JitThrow{"autograd_save_safetensors(): corrupted tensor storage", line};
        }
        for (double value : tensor->data) {
            if (!std::isfinite(value)) {
                throw JitThrow{"autograd_save_safetensors(): tensor data must be finite",
                               line};
            }
        }
        records.push_back({entry.first, tensor, 0, 0});
    }
    std::sort(records.begin(), records.end(),
              [](const SafetensorsSaveRecord& left,
                 const SafetensorsSaveRecord& right) {
                  return left.name < right.name;
              });
    uint64_t offset = 0;
    for (auto& record : records) {
        record.begin = offset;
        offset = safetensors_checked_add(
            offset, (uint64_t)record.tensor->data.byte_size(), api, line);
        record.end = offset;
    }
    return records;
}

inline std::string safetensors_build_header(
    const std::vector<SafetensorsSaveRecord>& records, int line) {
    std::string header = "{";
    for (size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        if (index) header.push_back(',');
        header += "\"" + safetensors_json_escape(record.name) + "\":{";
        header += "\"dtype\":\"";
        header += safetensors_dtype_code(record.tensor->data.dtype());
        header += "\",\"shape\":[";
        for (size_t axis = 0; axis < record.tensor->shape.size(); ++axis) {
            if (axis) header.push_back(',');
            header += std::to_string(record.tensor->shape[axis]);
        }
        header += "],\"data_offsets\":[" + std::to_string(record.begin)
               + "," + std::to_string(record.end) + "]}";
        if (header.size() > SAFETENSORS_MAX_HEADER_BYTES) {
            throw JitThrow{"autograd_save_safetensors(): header exceeds the safety limit",
                           line};
        }
    }
    header.push_back('}');
    while ((header.size() & 7U) != 0U) header.push_back(' ');
    if (header.size() > SAFETENSORS_MAX_HEADER_BYTES) {
        throw JitThrow{"autograd_save_safetensors(): header exceeds the safety limit", line};
    }
    return header;
}

class SafetensorsWriter {
    std::ofstream out_;
    uint64_t written_ = 0;
    int line_ = 0;

public:
    SafetensorsWriter(const std::filesystem::path& path, int line)
        : out_(path, std::ios::binary | std::ios::trunc), line_(line) {
        if (!out_) {
            throw JitThrow{"autograd_save_safetensors(): cannot open temporary file", line_};
        }
    }

    void write(const void* data, size_t bytes) {
        if ((uint64_t)bytes > SAFETENSORS_MAX_FILE_BYTES - written_) {
            throw JitThrow{"autograd_save_safetensors(): file exceeds the safety limit",
                           line_};
        }
        if (bytes) out_.write((const char*)data, (std::streamsize)bytes);
        if (!out_) {
            throw JitThrow{"autograd_save_safetensors(): failed while writing file", line_};
        }
        written_ += (uint64_t)bytes;
    }

    uint64_t finish() {
        out_.flush();
        if (!out_) {
            throw JitThrow{"autograd_save_safetensors(): failed flushing file", line_};
        }
        out_.close();
        if (!out_) {
            throw JitThrow{"autograd_save_safetensors(): failed closing file", line_};
        }
        return written_;
    }
};

inline void safetensors_write_tensor(SafetensorsWriter& writer,
                                     const GCTensor* tensor, int line) {
    std::array<unsigned char, SAFETENSORS_IO_CHUNK_BYTES> buffer{};
    size_t element_bytes = tensor_dtype_size(tensor->data.dtype());
    size_t chunk_elements = buffer.size() / element_bytes;
    for (size_t first = 0; first < tensor->data.size(); first += chunk_elements) {
        size_t count = std::min(chunk_elements, tensor->data.size() - first);
        for (size_t index = 0; index < count; ++index) {
            safetensors_encode_value(buffer.data() + index * element_bytes,
                                     tensor->data.dtype(), tensor->data[first + index],
                                     "autograd_save_safetensors", line);
        }
        writer.write(buffer.data(), count * element_bytes);
    }
}

inline std::string safetensors_random_suffix() {
    uint64_t first = (uint64_t)std::chrono::high_resolution_clock::now()
                         .time_since_epoch().count();
    uint64_t second = (uint64_t)(uintptr_t)&first;
    try {
        std::random_device random;
        first ^= ((uint64_t)random() << 32) ^ (uint64_t)random();
        second ^= ((uint64_t)random() << 32) ^ (uint64_t)random();
    } catch (...) {
        second ^= first * 0x9e3779b97f4a7c15ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << first
        << std::setw(16) << std::setfill('0') << second;
    return out.str();
}

class SafetensorsTempGuard {
    std::filesystem::path path_;
    bool active_ = true;

public:
    explicit SafetensorsTempGuard(std::filesystem::path path)
        : path_(std::move(path)) {}
    ~SafetensorsTempGuard() {
        if (!active_) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    void release() { active_ = false; }
};

inline std::filesystem::path safetensors_unique_temp_path(const std::string& path,
                                                          int line) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::filesystem::path candidate = fs_path_from_utf8(
            path + ".tmp." + safetensors_random_suffix() + "."
            + std::to_string(attempt));
        std::error_code ec;
        bool exists = std::filesystem::exists(candidate, ec);
        if (ec) {
            throw JitThrow{"autograd_save_safetensors(): cannot inspect temporary path: "
                           + ec.message(), line};
        }
        if (!exists) return candidate;
    }
    throw JitThrow{"autograd_save_safetensors(): cannot create a unique temporary path",
                   line};
}

inline void safetensors_commit_file(const std::filesystem::path& temp,
                                    const std::filesystem::path& target,
                                    int line) {
    std::error_code ec;
    std::filesystem::file_status existing = std::filesystem::symlink_status(target, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        throw JitThrow{"autograd_save_safetensors(): cannot inspect target: "
                       + ec.message(), line};
    }
    if (!ec && existing.type() != std::filesystem::file_type::not_found) {
        if (existing.type() == std::filesystem::file_type::symlink) {
            throw JitThrow{"autograd_save_safetensors(): refusing to replace a symlink",
                           line};
        }
        if (existing.type() != std::filesystem::file_type::regular) {
            throw JitThrow{"autograd_save_safetensors(): target must be a regular file",
                           line};
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD code = GetLastError();
        throw JitThrow{"autograd_save_safetensors(): atomic commit failed (Windows error "
                       + std::to_string((unsigned long)code) + ")", line};
    }
#else
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        throw JitThrow{"autograd_save_safetensors(): atomic commit failed: "
                       + ec.message(), line};
    }
#endif
}

struct SafetensorsFileInfo {
    uint64_t file_size = 0;
    uint64_t header_size = 0;
    uint64_t data_start = 0;
    size_t total_storage_bytes = 0;
    size_t maximum_temporary_bytes = 0;
    std::filesystem::file_time_type modified_time{};
    std::vector<SafetensorsRecord> records;
};

inline void safetensors_read_exact(std::ifstream& input, void* destination,
                                   size_t bytes, int line) {
    if (bytes) input.read((char*)destination, (std::streamsize)bytes);
    if (!input) {
        throw JitThrow{"autograd_load_safetensors(): truncated file", line};
    }
}

inline void safetensors_seek(std::ifstream& input, uint64_t offset, int line) {
    if (offset > (uint64_t)std::numeric_limits<std::streamoff>::max()) {
        throw JitThrow{"autograd_load_safetensors(): file offset exceeds platform limits",
                       line};
    }
    input.clear();
    input.seekg((std::streamoff)offset, std::ios::beg);
    if (!input) {
        throw JitThrow{"autograd_load_safetensors(): failed seeking tensor data", line};
    }
}

inline SafetensorsFileInfo safetensors_read_file_info(
    const std::filesystem::path& path, std::ifstream& input, int line) {
    std::error_code ec;
    std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    if (ec || status.type() != std::filesystem::file_type::regular) {
        if (!ec && status.type() == std::filesystem::file_type::symlink) {
            throw JitThrow{"autograd_load_safetensors(): refusing to read a symlink", line};
        }
        throw JitThrow{"autograd_load_safetensors(): path must be a regular file", line};
    }
    uintmax_t raw_file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw JitThrow{"autograd_load_safetensors(): cannot determine file size: "
                       + ec.message(), line};
    }
    if (raw_file_size < 10 || raw_file_size > SAFETENSORS_MAX_FILE_BYTES) {
        throw JitThrow{"autograd_load_safetensors(): invalid or oversized file", line};
    }
    auto modified = std::filesystem::last_write_time(path, ec);
    if (ec) {
        throw JitThrow{"autograd_load_safetensors(): cannot inspect modification time: "
                       + ec.message(), line};
    }

    input.open(path, std::ios::binary);
    if (!input) throw JitThrow{"autograd_load_safetensors(): cannot open file", line};
    unsigned char length_bytes[8]{};
    safetensors_read_exact(input, length_bytes, sizeof(length_bytes), line);
    uint64_t header_size = safetensors_load_u64(length_bytes);
    if (header_size < 2 || header_size > SAFETENSORS_MAX_HEADER_BYTES
        || header_size > raw_file_size - 8) {
        throw JitThrow{"autograd_load_safetensors(): invalid header length", line};
    }
    std::string header((size_t)header_size, '\0');
    safetensors_read_exact(input, header.data(), header.size(), line);
    SafetensorsHeaderParser parser(header, line);
    std::vector<SafetensorsRecord> records = parser.parse();

    uint64_t data_start = 8 + header_size;
    uint64_t data_bytes = (uint64_t)raw_file_size - data_start;
    std::vector<const SafetensorsRecord*> ordered;
    ordered.reserve(records.size());
    for (const auto& record : records) ordered.push_back(&record);
    std::sort(ordered.begin(), ordered.end(),
              [](const SafetensorsRecord* left, const SafetensorsRecord* right) {
                  if (left->begin != right->begin) return left->begin < right->begin;
                  return left->end < right->end;
              });
    uint64_t cursor = 0;
    size_t maximum_temporary = 0;
    for (const SafetensorsRecord* record : ordered) {
        if (record->begin != cursor || record->end > data_bytes) {
            throw JitThrow{"autograd_load_safetensors(): tensor data offsets contain a gap, overlap, or out-of-range byte",
                           line};
        }
        cursor = record->end;
        if (record->numel > std::numeric_limits<size_t>::max() / sizeof(double)) {
            throw JitThrow{"autograd_load_safetensors(): temporary tensor size overflow",
                           line};
        }
        maximum_temporary = std::max(maximum_temporary,
                                     record->numel * sizeof(double));
    }
    if (cursor != data_bytes) {
        throw JitThrow{"autograd_load_safetensors(): tensor data does not cover the complete byte buffer",
                       line};
    }
    if (data_bytes > (uint64_t)std::numeric_limits<size_t>::max()) {
        throw JitThrow{"autograd_load_safetensors(): tensor data exceeds platform limits",
                       line};
    }
    size_t total_storage = (size_t)data_bytes;
    if (maximum_temporary > std::numeric_limits<size_t>::max() - total_storage) {
        throw JitThrow{"autograd_load_safetensors(): aggregate memory size overflow", line};
    }

    SafetensorsFileInfo info;
    info.file_size = (uint64_t)raw_file_size;
    info.header_size = header_size;
    info.data_start = data_start;
    info.total_storage_bytes = total_storage;
    info.maximum_temporary_bytes = maximum_temporary;
    info.modified_time = modified;
    info.records = std::move(records);
    return info;
}

inline void safetensors_read_record_values(
    std::ifstream& input, const SafetensorsFileInfo& info,
    const SafetensorsRecord& record, std::vector<double>* destination,
    int line) {
    safetensors_seek(input,
                     safetensors_checked_add(info.data_start, record.begin,
                                             "autograd_load_safetensors", line),
                     line);
    if (destination) destination->resize(record.numel);
    std::array<unsigned char, SAFETENSORS_IO_CHUNK_BYTES> buffer{};
    size_t element_bytes = tensor_dtype_size(record.dtype);
    size_t chunk_elements = buffer.size() / element_bytes;
    size_t consumed = 0;
    while (consumed < record.numel) {
        size_t count = std::min(chunk_elements, record.numel - consumed);
        size_t bytes = count * element_bytes;
        safetensors_read_exact(input, buffer.data(), bytes, line);
        for (size_t index = 0; index < count; ++index) {
            double value = safetensors_decode_value(
                buffer.data() + index * element_bytes, record.dtype,
                "autograd_load_safetensors", line);
            if (destination) (*destination)[consumed + index] = value;
        }
        consumed += count;
    }
}

inline void safetensors_discard_loaded_tensors(std::vector<GCTensor*>& tensors) {
    for (GCTensor* tensor : tensors) {
        if (!tensor) continue;
        size_t reserved = tensor->tracked_bytes;
        tensor->data.clear_and_release();
        std::vector<double>().swap(tensor->grad);
        std::vector<double>().swap(tensor->adam_m);
        std::vector<double>().swap(tensor->adam_v);
        std::vector<double>().swap(tensor->sgd_velocity);
        std::vector<size_t>().swap(tensor->shape);
        std::vector<GCTensor*>().swap(tensor->parents);
        std::vector<uint64_t>().swap(tensor->parent_versions);
        std::vector<size_t>().swap(tensor->op_indices);
        ag_release_bytes(tensor, reserved);
    }
    tensors.clear();
}

inline Value b_autograd_save_safetensors(const Value* args, int nargs, int line) {
    need_args("autograd_save_safetensors", nargs, 2, 2, line);
    std::string path = need_str("autograd_save_safetensors", args[1], 1, line);
    safetensors_validate_path("autograd_save_safetensors", path, line);
    try {
        std::vector<SafetensorsSaveRecord> records =
            safetensors_collect_save_records(args[0], line);
        std::string header = safetensors_build_header(records, line);
        uint64_t data_bytes = records.empty() ? 0 : records.back().end;
        uint64_t expected_size = safetensors_checked_add(
            8, (uint64_t)header.size(), "autograd_save_safetensors", line);
        expected_size = safetensors_checked_add(
            expected_size, data_bytes, "autograd_save_safetensors", line);
        if (expected_size > SAFETENSORS_MAX_FILE_BYTES) {
            throw JitThrow{"autograd_save_safetensors(): file exceeds the safety limit",
                           line};
        }

        std::filesystem::path target = fs_path_from_utf8(path);
        std::filesystem::path parent = target.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                throw JitThrow{"autograd_save_safetensors(): cannot create parent directory: "
                               + ec.message(), line};
            }
        }
        std::filesystem::path temporary = safetensors_unique_temp_path(path, line);
        SafetensorsTempGuard cleanup(temporary);
        SafetensorsWriter writer(temporary, line);
        unsigned char length_bytes[8]{};
        safetensors_store_u64(length_bytes, (uint64_t)header.size());
        writer.write(length_bytes, sizeof(length_bytes));
        writer.write(header.data(), header.size());
        for (const auto& record : records) {
            safetensors_write_tensor(writer, record.tensor, line);
        }
        uint64_t written = writer.finish();
        if (written != expected_size) {
            throw JitThrow{"autograd_save_safetensors(): internal file size mismatch", line};
        }
        safetensors_commit_file(temporary, target, line);
        cleanup.release();
        return Value((double)written);
    } catch (const JitThrow&) {
        throw;
    } catch (const std::exception& error) {
        throw JitThrow{"autograd_save_safetensors(): " + std::string(error.what()), line};
    } catch (...) {
        throw JitThrow{"autograd_save_safetensors(): unexpected Safetensors failure", line};
    }
}

inline Value b_autograd_load_safetensors(const Value* args, int nargs, int line) {
    need_args("autograd_load_safetensors", nargs, 1, 2, line);
    std::string path = need_str("autograd_load_safetensors", args[0], 0, line);
    safetensors_validate_path("autograd_load_safetensors", path, line);
    GCDict* options = nn_options("autograd_load_safetensors", args, nargs, 1, line);
    ag_validate_options("autograd_load_safetensors", options, {"requires_grad"}, line);
    bool requires_grad = nn_option_bool(
        "autograd_load_safetensors", options, "requires_grad", false, line);

    std::filesystem::path source = fs_path_from_utf8(path);
    std::vector<GCTensor*> created;
    try {
        std::ifstream input;
        SafetensorsFileInfo info = safetensors_read_file_info(source, input, line);
        size_t aggregate = info.total_storage_bytes
                         + info.maximum_temporary_bytes;
        ag_preflight_bytes(aggregate, "autograd_load_safetensors", line);

        // Validate every payload scalar before allocating any Tensor buffer.
        for (const auto& record : info.records) {
            safetensors_read_record_values(input, info, record, nullptr, line);
        }

        Value result = Value::make_dict();
        GCNativeRoot result_root(result.as_obj());
        created.reserve(info.records.size());
        for (const auto& record : info.records) {
            Value tensor_value = Value::make_tensor();
            GCTensor* tensor = tensor_value.as_tensor();
            GCNativeRoot tensor_root(tensor);
            created.push_back(tensor);
            size_t storage_bytes = (size_t)(record.end - record.begin);
            ag_reserve_bytes(tensor, storage_bytes,
                             "autograd_load_safetensors", line);
            AgTemporaryBytes temporary(record.numel * sizeof(double),
                                       "autograd_load_safetensors", line);
            std::vector<double> values;
            safetensors_read_record_values(input, info, record, &values, line);
            ag_validate_dtype_values("autograd_load_safetensors", values,
                                     record.dtype, line);
            tensor->data.assign(std::move(values), record.dtype);
            tensor->shape = record.shape;
            tensor->requires_grad = requires_grad;
            tensor->op = TensorOp::LEAF;
            tensor->graph_freed = false;
            tensor->version = 0;
            result.as_dict()->elements.emplace(record.name, tensor_value);
        }

        std::error_code ec;
        uintmax_t final_size = std::filesystem::file_size(source, ec);
        if (ec || final_size != info.file_size) {
            throw JitThrow{"autograd_load_safetensors(): file changed while loading", line};
        }
        auto final_modified = std::filesystem::last_write_time(source, ec);
        if (ec || final_modified != info.modified_time) {
            throw JitThrow{"autograd_load_safetensors(): file changed while loading", line};
        }
        return result;
    } catch (const JitThrow&) {
        safetensors_discard_loaded_tensors(created);
        throw;
    } catch (const std::exception& error) {
        safetensors_discard_loaded_tensors(created);
        throw JitThrow{"autograd_load_safetensors(): " + std::string(error.what()), line};
    } catch (...) {
        safetensors_discard_loaded_tensors(created);
        throw JitThrow{"autograd_load_safetensors(): unexpected Safetensors failure", line};
    }
}

} // namespace SuraStd
