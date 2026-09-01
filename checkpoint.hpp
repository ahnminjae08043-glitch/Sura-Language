#pragma once

// Native autograd checkpoint I/O.
//
// This header is intentionally kept separate from the builtin registry.  It
// must be included after stdlib.hpp's common helpers and autograd.hpp are
// available.  The public entry points at the bottom follow the normal Sura
// builtin ABI and can be registered by the embedding translation unit.

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
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
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

static constexpr unsigned char CKPT_MAGIC[8] = {
    'S', 'U', 'R', 'A', 'C', 'K', 'P', 'T'
};
static constexpr uint16_t CKPT_VERSION = 3;
static constexpr size_t CKPT_MAX_TENSORS = 100000;
static constexpr size_t CKPT_MAX_NAME_BYTES = 512;
static constexpr size_t CKPT_MAX_PATH_BYTES = 32768;
static constexpr uint64_t CKPT_MAX_FILE_BYTES =
    65ULL * 1024ULL * 1024ULL * 1024ULL;
static constexpr uint8_t CKPT_STATE_ADAM = 1;
static constexpr uint8_t CKPT_STATE_SGD = 2;
static constexpr uint8_t CKPT_STATE_MASTER = 4;
static constexpr uint8_t CKPT_STATE_CUDA_OPTIMIZER = 8;
static constexpr uint8_t CKPT_STATE_KNOWN =
    CKPT_STATE_ADAM | CKPT_STATE_SGD | CKPT_STATE_MASTER
    | CKPT_STATE_CUDA_OPTIMIZER;

// A small, dependency-free SHA-256 implementation.  Checkpoints use the
// digest for corruption detection, not authentication.
class CheckpointSha256 {
    std::array<uint32_t, 8> state_{{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    }};
    std::array<unsigned char, 64> block_{};
    size_t block_size_ = 0;
    uint64_t byte_count_ = 0;

    static uint32_t rotate_right(uint32_t value, unsigned bits) {
        return (value >> bits) | (value << (32U - bits));
    }

    void transform(const unsigned char* input) {
        static constexpr uint32_t constants[64] = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
        };

        uint32_t words[64];
        for (size_t i = 0; i < 16; ++i) {
            size_t offset = i * 4;
            words[i] = (uint32_t(input[offset]) << 24)
                     | (uint32_t(input[offset + 1]) << 16)
                     | (uint32_t(input[offset + 2]) << 8)
                     | uint32_t(input[offset + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            uint32_t s0 = rotate_right(words[i - 15], 7)
                        ^ rotate_right(words[i - 15], 18)
                        ^ (words[i - 15] >> 3);
            uint32_t s1 = rotate_right(words[i - 2], 17)
                        ^ rotate_right(words[i - 2], 19)
                        ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];

        for (size_t i = 0; i < 64; ++i) {
            uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11)
                          ^ rotate_right(e, 25);
            uint32_t choice = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + sum1 + choice + constants[i] + words[i];
            uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13)
                          ^ rotate_right(a, 22);
            uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = sum0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

public:
    void update(const void* bytes, size_t size) {
        const auto* input = static_cast<const unsigned char*>(bytes);
        if (size > std::numeric_limits<uint64_t>::max() - byte_count_) {
            throw std::overflow_error("SHA-256 input is too large");
        }
        byte_count_ += (uint64_t)size;
        while (size > 0) {
            size_t take = std::min(size, block_.size() - block_size_);
            std::memcpy(block_.data() + block_size_, input, take);
            block_size_ += take;
            input += take;
            size -= take;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0;
            }
        }
    }

    std::array<unsigned char, 32> finish() {
        uint64_t bit_count = byte_count_ * 8ULL;
        block_[block_size_++] = 0x80;
        if (block_size_ > 56) {
            std::fill(block_.begin() + (ptrdiff_t)block_size_, block_.end(), 0);
            transform(block_.data());
            block_size_ = 0;
        }
        std::fill(block_.begin() + (ptrdiff_t)block_size_, block_.begin() + 56, 0);
        for (size_t i = 0; i < 8; ++i) {
            block_[63 - i] = (unsigned char)((bit_count >> (i * 8)) & 0xffU);
        }
        transform(block_.data());

        std::array<unsigned char, 32> digest{};
        for (size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4] = (unsigned char)(state_[i] >> 24);
            digest[i * 4 + 1] = (unsigned char)(state_[i] >> 16);
            digest[i * 4 + 2] = (unsigned char)(state_[i] >> 8);
            digest[i * 4 + 3] = (unsigned char)state_[i];
        }
        return digest;
    }
};

inline bool checkpoint_valid_utf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        unsigned char lead = (unsigned char)text[i++];
        if (lead <= 0x7f) continue;
        uint32_t value = 0;
        size_t continuation = 0;
        uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            value = lead & 0x1fU;
            continuation = 1;
            minimum = 0x80U;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            value = lead & 0x0fU;
            continuation = 2;
            minimum = 0x800U;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            value = lead & 0x07U;
            continuation = 3;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (continuation > text.size() - i) return false;
        for (size_t j = 0; j < continuation; ++j) {
            unsigned char next = (unsigned char)text[i++];
            if ((next & 0xc0U) != 0x80U) return false;
            value = (value << 6) | (next & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU
            || (value >= 0xd800U && value <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

inline void checkpoint_validate_path(const char* api, const std::string& path,
                                     int line) {
    if (path.empty()) {
        throw JitThrow{std::string(api) + "(): path must not be empty", line};
    }
    if (path.size() > CKPT_MAX_PATH_BYTES) {
        throw JitThrow{std::string(api) + "(): path exceeds the "
                       + std::to_string(CKPT_MAX_PATH_BYTES) + " byte limit", line};
    }
    if (path.find('\0') != std::string::npos || !checkpoint_valid_utf8(path)) {
        throw JitThrow{std::string(api) + "(): path must be valid UTF-8 without NUL bytes", line};
    }
}

inline void checkpoint_validate_name(const char* api, const std::string& name,
                                     int line) {
    if (name.empty()) {
        throw JitThrow{std::string(api) + "(): tensor names must not be empty", line};
    }
    if (name.size() > CKPT_MAX_NAME_BYTES) {
        throw JitThrow{std::string(api) + "(): tensor name exceeds the "
                       + std::to_string(CKPT_MAX_NAME_BYTES) + " byte limit", line};
    }
    if (name.find('\0') != std::string::npos || !checkpoint_valid_utf8(name)) {
        throw JitThrow{std::string(api)
                       + "(): tensor names must be valid UTF-8 without NUL bytes", line};
    }
}

inline uint64_t checkpoint_checked_mul(uint64_t left, uint64_t right,
                                       const char* api, int line) {
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
        throw JitThrow{std::string(api) + "(): checkpoint size overflow", line};
    }
    return left * right;
}

inline void checkpoint_checked_add(uint64_t& total, uint64_t amount,
                                   uint64_t limit, const char* api, int line) {
    if (amount > limit || total > limit - amount) {
        throw JitThrow{std::string(api) + "(): checkpoint tensor buffers exceed the "
                       + std::to_string(limit) + " byte safety limit", line};
    }
    total += amount;
}

inline bool checkpoint_option_optimizer(const char* api, const Value* args,
                                        int count, int index, int line) {
    if (count <= index || args[index].is_nil()) return true;
    GCDict* options = need_dict(api, args[index], index, line);
    for (const auto& entry : options->elements) {
        if (entry.first != "optimizer") {
            throw JitThrow{std::string(api) + "(): unknown option '"
                           + entry.first + "'", line};
        }
    }
    auto found = options->elements.find("optimizer");
    if (found == options->elements.end() || found->second.is_nil()) return true;
    if (!found->second.is_bool()) {
        throw JitThrow{std::string(api) + "(): option optimizer must be a bool", line};
    }
    return found->second.as_bool();
}

struct CheckpointLoadOptions {
    bool optimizer = true;
    bool cuda = false;
};

inline CheckpointLoadOptions checkpoint_load_options(
        const char* api, const Value* args, int count, int index, int line) {
    CheckpointLoadOptions result;
    if (count <= index || args[index].is_nil()) return result;
    GCDict* options = need_dict(api, args[index], index, line);
    for (const auto& entry : options->elements) {
        if (entry.first != "optimizer" && entry.first != "device") {
            throw JitThrow{std::string(api) + "(): unknown option '"
                           + entry.first + "'", line};
        }
    }
    auto optimizer = options->elements.find("optimizer");
    if (optimizer != options->elements.end() && !optimizer->second.is_nil()) {
        if (!optimizer->second.is_bool()) {
            throw JitThrow{std::string(api)
                           + "(): option optimizer must be a bool", line};
        }
        result.optimizer = optimizer->second.as_bool();
    }
    auto device = options->elements.find("device");
    if (device != options->elements.end() && !device->second.is_nil()) {
        if (!device->second.is_str()) {
            throw JitThrow{std::string(api)
                           + "(): option device must be a string", line};
        }
        result.cuda = ag_parse_device_request(
            api, device->second.as_str_ref(), line);
    }
    return result;
}

class CheckpointWriter {
    std::ofstream stream_;
    CheckpointSha256 hash_;
    uint64_t bytes_ = 0;
    int line_;

public:
    CheckpointWriter(const std::filesystem::path& path, int line)
        : stream_(path, std::ios::binary | std::ios::trunc), line_(line) {
        if (!stream_) {
            throw JitThrow{"autograd_save_checkpoint(): cannot open temporary file", line_};
        }
    }

    void write(const void* data, size_t size) {
        if (size > CKPT_MAX_FILE_BYTES || bytes_ > CKPT_MAX_FILE_BYTES - size) {
            throw JitThrow{"autograd_save_checkpoint(): checkpoint exceeds the file size limit",
                           line_};
        }
        stream_.write(static_cast<const char*>(data), (std::streamsize)size);
        if (!stream_) {
            throw JitThrow{"autograd_save_checkpoint(): failed while writing checkpoint",
                           line_};
        }
        hash_.update(data, size);
        bytes_ += (uint64_t)size;
    }

    void write_u8(uint8_t value) { write(&value, 1); }

    void write_u16(uint16_t value) {
        unsigned char bytes[2];
        for (size_t i = 0; i < 2; ++i) bytes[i] = (unsigned char)(value >> (i * 8));
        write(bytes, sizeof(bytes));
    }

    void write_u32(uint32_t value) {
        unsigned char bytes[4];
        for (size_t i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (i * 8));
        write(bytes, sizeof(bytes));
    }

    void write_u64(uint64_t value) {
        unsigned char bytes[8];
        for (size_t i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (i * 8));
        write(bytes, sizeof(bytes));
    }

    void write_f64(double value) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        write_u64(bits);
    }

    uint64_t finish() {
        std::array<unsigned char, 32> digest = hash_.finish();
        if (bytes_ > CKPT_MAX_FILE_BYTES - digest.size()) {
            throw JitThrow{"autograd_save_checkpoint(): checkpoint exceeds the file size limit",
                           line_};
        }
        stream_.write(reinterpret_cast<const char*>(digest.data()),
                      (std::streamsize)digest.size());
        bytes_ += (uint64_t)digest.size();
        stream_.flush();
        if (!stream_) {
            throw JitThrow{"autograd_save_checkpoint(): failed finalizing checkpoint",
                           line_};
        }
        stream_.close();
        if (!stream_) {
            throw JitThrow{"autograd_save_checkpoint(): failed closing checkpoint",
                           line_};
        }
        return bytes_;
    }
};

class CheckpointReader {
    std::ifstream stream_;
    CheckpointSha256 hash_;
    uint64_t bytes_ = 0;
    int line_;

public:
    CheckpointReader(const std::filesystem::path& path, int line)
        : stream_(path, std::ios::binary), line_(line) {
        if (!stream_) {
            throw JitThrow{"autograd_load_checkpoint(): cannot open checkpoint", line_};
        }
    }

    void read(void* destination, size_t size) {
        if (size > CKPT_MAX_FILE_BYTES || bytes_ > CKPT_MAX_FILE_BYTES - size) {
            throw JitThrow{"autograd_load_checkpoint(): checkpoint exceeds the file size limit",
                           line_};
        }
        stream_.read(static_cast<char*>(destination), (std::streamsize)size);
        if ((size_t)stream_.gcount() != size) {
            throw JitThrow{"autograd_load_checkpoint(): truncated checkpoint", line_};
        }
        hash_.update(destination, size);
        bytes_ += (uint64_t)size;
    }

    uint8_t read_u8() {
        unsigned char value = 0;
        read(&value, 1);
        return value;
    }

    uint16_t read_u16() {
        unsigned char bytes[2];
        read(bytes, sizeof(bytes));
        return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
    }

    uint32_t read_u32() {
        unsigned char bytes[4];
        read(bytes, sizeof(bytes));
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i) value |= uint32_t(bytes[i]) << (i * 8);
        return value;
    }

    uint64_t read_u64() {
        unsigned char bytes[8];
        read(bytes, sizeof(bytes));
        uint64_t value = 0;
        for (size_t i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (i * 8);
        return value;
    }

    double read_f64() {
        uint64_t bits = read_u64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::array<unsigned char, 32> verify_footer() {
        std::array<unsigned char, 32> expected{};
        stream_.read(reinterpret_cast<char*>(expected.data()),
                     (std::streamsize)expected.size());
        if ((size_t)stream_.gcount() != expected.size()) {
            throw JitThrow{"autograd_load_checkpoint(): truncated checksum footer", line_};
        }
        bytes_ += (uint64_t)expected.size();
        std::array<unsigned char, 32> actual = hash_.finish();
        unsigned difference = 0;
        for (size_t i = 0; i < actual.size(); ++i) {
            difference |= unsigned(actual[i] ^ expected[i]);
        }
        if (difference != 0) {
            throw JitThrow{"autograd_load_checkpoint(): SHA-256 integrity check failed", line_};
        }

        char trailing = 0;
        stream_.read(&trailing, 1);
        if (stream_.gcount() != 0) {
            throw JitThrow{"autograd_load_checkpoint(): trailing data after checksum", line_};
        }
        if (!stream_.eof()) {
            throw JitThrow{"autograd_load_checkpoint(): failed checking checkpoint end",
                           line_};
        }
        return actual;
    }
};

struct CheckpointSaveTensor {
    std::string name;
    GCTensor* tensor = nullptr;
    uint8_t state_flags = 0;
    bool staged_cuda = false;
    TensorBuffer staged_data;
    std::vector<float> staged_master;
    std::vector<float> staged_adam_m;
    std::vector<float> staged_adam_v;
    std::vector<float> staged_sgd_velocity;
    std::unique_ptr<AgTemporaryBytes> staging_charge;
};

inline size_t checkpoint_tensor_numel(const char* api, const GCTensor* tensor,
                                      int line) {
    if (tensor->shape.size() > AG_MAX_RANK) {
        throw JitThrow{std::string(api) + "(): tensor rank exceeds "
                       + std::to_string(AG_MAX_RANK), line};
    }
    size_t count = 1;
    for (size_t dim : tensor->shape) {
        if (dim == 0 || count > ag_max_elements() / dim) {
            throw JitThrow{std::string(api) + "(): invalid or oversized tensor shape", line};
        }
        count *= dim;
    }
    if (count != tensor->data.size() || count > ag_max_elements()) {
        throw JitThrow{std::string(api) + "(): tensor shape/data size mismatch", line};
    }
    return count;
}

inline uint8_t checkpoint_validate_tensor_for_save(const char* api,
                                                   const GCTensor* tensor,
                                                   bool include_optimizer,
                                                   uint64_t& total_buffer_bytes,
                                                   int line) {
    size_t count = checkpoint_tensor_numel(api, tensor, line);
    size_t element_bytes = tensor_dtype_size(tensor->data.dtype());
    if (element_bytes == 0
        || !tensor->data.host_readable()
        || count > std::numeric_limits<size_t>::max() / element_bytes
        || tensor->data.byte_size() != count * element_bytes
        || tensor->data.host_byte_size() != count * element_bytes) {
        throw JitThrow{std::string(api) + "(): corrupted tensor host storage", line};
    }
    for (double value : tensor->data) {
        if (!std::isfinite(value)) {
            throw JitThrow{std::string(api) + "(): tensor data must be finite", line};
        }
    }

    uint8_t flags = 0;
    if (include_optimizer) {
        bool adam_empty = tensor->adam_m.empty() && tensor->adam_v.empty();
        bool adam_ready = tensor->adam_m.size() == count
                       && tensor->adam_v.size() == count;
        if (!adam_empty && !adam_ready) {
            throw JitThrow{std::string(api) + "(): corrupted Adam buffers", line};
        }
        if (adam_empty) {
            if (tensor->adam_step != 0
                || tensor->adam_beta1_product != 1.0
                || tensor->adam_beta2_product != 1.0) {
                throw JitThrow{std::string(api) + "(): Adam counters exist without moments",
                               line};
            }
        } else {
            if (tensor->adam_step == 0
                || !std::isfinite(tensor->adam_beta1_product)
                || !std::isfinite(tensor->adam_beta2_product)
                || tensor->adam_beta1_product < 0.0
                || tensor->adam_beta1_product > 1.0
                || tensor->adam_beta2_product < 0.0
                || tensor->adam_beta2_product > 1.0) {
                throw JitThrow{std::string(api) + "(): invalid Adam optimizer state", line};
            }
            for (size_t i = 0; i < count; ++i) {
                if (!std::isfinite(tensor->adam_m[i])
                    || !std::isfinite(tensor->adam_v[i])
                    || tensor->adam_v[i] < 0.0) {
                    throw JitThrow{std::string(api) + "(): Adam state must be finite",
                                   line};
                }
            }
            flags |= CKPT_STATE_ADAM;
        }

        if (!tensor->sgd_velocity.empty()) {
            if (tensor->sgd_velocity.size() != count) {
                throw JitThrow{std::string(api) + "(): corrupted SGD velocity buffer",
                               line};
            }
            for (double value : tensor->sgd_velocity) {
                if (!std::isfinite(value)) {
                    throw JitThrow{std::string(api) + "(): SGD state must be finite",
                                   line};
                }
            }
            flags |= CKPT_STATE_SGD;
        }
    }

    uint64_t one_buffer = checkpoint_checked_mul((uint64_t)count, sizeof(double),
                                                 api, line);
    uint64_t multiplier = 1;
    if (flags & CKPT_STATE_ADAM) multiplier += 2;
    if (flags & CKPT_STATE_SGD) multiplier += 1;
    checkpoint_checked_add(total_buffer_bytes,
                           checkpoint_checked_mul(one_buffer, multiplier, api, line),
                           (uint64_t)ag_max_external_bytes(), api, line);
    return flags;
}

inline void checkpoint_require_cuda_f32_state(
        const char* api, const GCTensor* tensor,
        const std::shared_ptr<AgCudaAllocation>& state,
        const char* description, int line) {
    if (!state) return;
    if (state->handle == SuraCudaDriver::INVALID_DEVICE_HANDLE
        || state->elements != tensor->data.size()
        || state->device_index != tensor->cuda_data->device_index
        || state->storage != SuraCudaDriver::TensorStorage::FLOAT32) {
        throw JitThrow{std::string(api) + "(): corrupted CUDA "
                       + description, line};
    }
}

inline uint8_t checkpoint_validate_cuda_tensor_for_save(
        const char* api, const GCTensor* tensor, bool include_optimizer,
        uint64_t& total_buffer_bytes, int line) {
    const size_t count = checkpoint_tensor_numel(api, tensor, line);
    ag_cuda_require_tensor(api, tensor, line);
    uint8_t flags = 0;
    if (include_optimizer) {
        if (!tensor->adam_m.empty() || !tensor->adam_v.empty()
            || !tensor->sgd_velocity.empty()) {
            throw JitThrow{std::string(api)
                           + "(): CUDA tensor has legacy CPU optimizer state", line};
        }
        checkpoint_require_cuda_f32_state(
            api, tensor, tensor->cuda_master_data, "master-weight buffer", line);
        checkpoint_require_cuda_f32_state(
            api, tensor, tensor->cuda_adam_m, "first-moment buffer", line);
        checkpoint_require_cuda_f32_state(
            api, tensor, tensor->cuda_adam_v, "second-moment buffer", line);
        checkpoint_require_cuda_f32_state(
            api, tensor, tensor->cuda_sgd_velocity, "SGD velocity buffer", line);

        const bool moments_empty = !tensor->cuda_adam_m && !tensor->cuda_adam_v;
        const bool moments_ready = tensor->cuda_adam_m && tensor->cuda_adam_v;
        if (!moments_empty && !moments_ready) {
            throw JitThrow{std::string(api)
                           + "(): corrupted CUDA Adam moment buffers", line};
        }
        if ((moments_empty && (tensor->adam_step != 0
                               || tensor->adam_beta1_product != 1.0
                               || tensor->adam_beta2_product != 1.0))
            || (moments_ready && tensor->adam_step == 0)
            || !std::isfinite(tensor->adam_beta1_product)
            || !std::isfinite(tensor->adam_beta2_product)
            || tensor->adam_beta1_product < 0.0
            || tensor->adam_beta1_product > 1.0
            || tensor->adam_beta2_product < 0.0
            || tensor->adam_beta2_product > 1.0) {
            throw JitThrow{std::string(api)
                           + "(): invalid CUDA Adam optimizer metadata", line};
        }
        const bool low_precision = tensor->data.dtype() == TensorDType::FLOAT16
            || tensor->data.dtype() == TensorDType::BFLOAT16;
        if (tensor->cuda_master_data && !low_precision) {
            throw JitThrow{std::string(api)
                           + "(): float32 CUDA tensor must not have master weights", line};
        }
        const bool has_optimizer_state = tensor->cuda_master_data
            || tensor->cuda_sgd_velocity || moments_ready;
        if (has_optimizer_state
            && (!tensor->requires_grad || tensor->op != TensorOp::LEAF)) {
            throw JitThrow{std::string(api)
                           + "(): CUDA optimizer state requires a trainable leaf tensor",
                           line};
        }
        if (tensor->cuda_master_data) flags |= CKPT_STATE_MASTER;
        if (moments_ready) flags |= CKPT_STATE_ADAM;
        if (tensor->cuda_sgd_velocity) flags |= CKPT_STATE_SGD;
        if (low_precision && (flags & (CKPT_STATE_ADAM | CKPT_STATE_SGD))
            && !(flags & CKPT_STATE_MASTER)) {
            throw JitThrow{std::string(api)
                           + "(): low-precision CUDA optimizer state requires "
                             "f32 master weights", line};
        }
        if (has_optimizer_state) flags |= CKPT_STATE_CUDA_OPTIMIZER;
    }

    const uint64_t one_buffer = checkpoint_checked_mul(
        (uint64_t)count, sizeof(double), api, line);
    uint64_t multiplier = 1;
    if (flags & CKPT_STATE_MASTER) multiplier += 1;
    if (flags & CKPT_STATE_ADAM) multiplier += 2;
    if (flags & CKPT_STATE_SGD) multiplier += 1;
    checkpoint_checked_add(total_buffer_bytes,
                           checkpoint_checked_mul(one_buffer, multiplier, api, line),
                           (uint64_t)ag_max_external_bytes(), api, line);
    return flags;
}

inline void checkpoint_validate_staged_f32(
        const char* api, const std::vector<float>& values,
        bool non_negative, int line) {
    for (float value : values) {
        if (!std::isfinite(value) || (non_negative && value < 0.0f)) {
            throw JitThrow{std::string(api)
                           + (non_negative
                               ? "(): CUDA Adam variance must be finite and non-negative"
                               : "(): CUDA tensor or optimizer state must be finite"),
                           line};
        }
    }
}

inline void checkpoint_stage_cuda_tensor(CheckpointSaveTensor& saved, int line) {
    const char* api = "autograd_save_checkpoint";
    GCTensor* tensor = saved.tensor;
    const size_t count = tensor->data.size();
    SuraCudaDriver& driver = SuraCudaDriver::instance();
    const auto storage = tensor->cuda_data->storage;
    if (storage == SuraCudaDriver::TensorStorage::FLOAT32) {
        std::vector<float> packed;
        if (!driver.download_f32(tensor->cuda_data->handle, packed, count)) {
            ag_cuda_fail(api, line);
        }
        checkpoint_validate_staged_f32(api, packed, false, line);
        saved.staged_data.assign_packed(
            packed.data(), count, TensorDType::FLOAT32);
    } else {
        std::vector<uint16_t> packed;
        if (!driver.download_u16(
                tensor->cuda_data->handle, storage, packed, count)) {
            ag_cuda_fail(api, line);
        }
        saved.staged_data.assign_packed(
            packed.data(), count, tensor->data.dtype());
        for (double value : saved.staged_data) {
            if (!std::isfinite(value)) {
                throw JitThrow{std::string(api)
                               + "(): CUDA tensor data must be finite", line};
            }
        }
    }
    auto download_state = [&](const std::shared_ptr<AgCudaAllocation>& source,
                              std::vector<float>& destination,
                              bool non_negative) {
        if (!source) return;
        if (!driver.download_f32(source->handle, destination, count)) {
            ag_cuda_fail(api, line);
        }
        checkpoint_validate_staged_f32(api, destination, non_negative, line);
    };
    if (saved.state_flags & CKPT_STATE_MASTER) {
        download_state(tensor->cuda_master_data, saved.staged_master, false);
        // The visible packed parameter must be the quantized image of the
        // hidden f32 master. A mismatched pair cannot resume deterministically.
        std::vector<double> master_values(saved.staged_master.begin(),
                                          saved.staged_master.end());
        ag_validate_dtype_values(api, master_values, tensor->data.dtype(), line);
        TensorBuffer repacked;
        repacked.assign(master_values, tensor->data.dtype());
        if (repacked.byte_size() != saved.staged_data.byte_size()
            || std::memcmp(repacked.packed_data(), saved.staged_data.packed_data(),
                           repacked.byte_size()) != 0) {
            throw JitThrow{std::string(api)
                           + "(): CUDA master weights do not match visible storage", line};
        }
    }
    if (saved.state_flags & CKPT_STATE_ADAM) {
        download_state(tensor->cuda_adam_m, saved.staged_adam_m, false);
        download_state(tensor->cuda_adam_v, saved.staged_adam_v, true);
    }
    if (saved.state_flags & CKPT_STATE_SGD) {
        download_state(tensor->cuda_sgd_velocity,
                       saved.staged_sgd_velocity, false);
    }
    saved.staged_cuda = true;
}

inline std::vector<CheckpointSaveTensor> checkpoint_collect_save_tensors(
    const Value& state, bool include_optimizer, int line) {
    const char* api = "autograd_save_checkpoint";
    GCDict* dict = need_dict(api, state, 0, line);
    if (dict->elements.empty()) {
        throw JitThrow{"autograd_save_checkpoint(): state_dict must not be empty", line};
    }
    if (dict->elements.size() > CKPT_MAX_TENSORS) {
        throw JitThrow{"autograd_save_checkpoint(): too many tensors", line};
    }

    std::vector<CheckpointSaveTensor> tensors;
    tensors.reserve(dict->elements.size());
    std::unordered_set<GCTensor*> identities;
    for (const auto& entry : dict->elements) {
        checkpoint_validate_name(api, entry.first, line);
        if (!entry.second.is_tensor()) {
            throw JitThrow{"autograd_save_checkpoint(): state_dict values must be tensors",
                           line};
        }
        GCTensor* tensor = entry.second.as_tensor();
        if (!identities.insert(tensor).second) {
            throw JitThrow{"autograd_save_checkpoint(): the same tensor appears under multiple names",
                           line};
        }
        tensors.push_back({entry.first, tensor, 0});
    }

    // Finish every metadata/state validation before the first CUDA D2H. A
    // malformed later tensor must not partially stage an earlier one merely
    // because of unordered dictionary iteration order.
    uint64_t total_buffer_bytes = 0;
    for (auto& saved : tensors) {
        if (ag_is_cuda(saved.tensor)) {
            saved.state_flags = checkpoint_validate_cuda_tensor_for_save(
                api, saved.tensor, include_optimizer, total_buffer_bytes, line);
        } else {
            saved.state_flags = checkpoint_validate_tensor_for_save(
                api, saved.tensor, include_optimizer, total_buffer_bytes, line);
        }
    }
    for (auto& saved : tensors) {
        if (ag_is_cuda(saved.tensor)) {
            uint64_t multiplier = 1;
            if (saved.state_flags & CKPT_STATE_MASTER) multiplier += 1;
            if (saved.state_flags & CKPT_STATE_ADAM) multiplier += 2;
            if (saved.state_flags & CKPT_STATE_SGD) multiplier += 1;
            const uint64_t charge = checkpoint_checked_mul(
                checkpoint_checked_mul((uint64_t)saved.tensor->data.size(),
                                       sizeof(double), api, line),
                multiplier, api, line);
            saved.staging_charge = std::make_unique<AgTemporaryBytes>(
                (size_t)charge, api, line);
            checkpoint_stage_cuda_tensor(saved, line);
        }
    }
    std::sort(tensors.begin(), tensors.end(),
              [](const CheckpointSaveTensor& left, const CheckpointSaveTensor& right) {
                  return left.name < right.name;
              });
    return tensors;
}

template<typename Values>
inline void checkpoint_write_values(CheckpointWriter& writer,
                                    const Values& values) {
    for (double value : values) writer.write_f64(value);
}

inline uint64_t checkpoint_write_file(const std::filesystem::path& temp_path,
                                      const std::vector<CheckpointSaveTensor>& tensors,
                                      int line) {
    CheckpointWriter writer(temp_path, line);
    writer.write(CKPT_MAGIC, sizeof(CKPT_MAGIC));
    writer.write_u16(CKPT_VERSION);
    writer.write_u16(0); // Reserved global flags.
    writer.write_u32((uint32_t)tensors.size());

    for (const auto& saved : tensors) {
        GCTensor* tensor = saved.tensor;
        writer.write_u32((uint32_t)saved.name.size());
        writer.write(saved.name.data(), saved.name.size());
        writer.write_u8((uint8_t)tensor->shape.size());
        writer.write_u8(tensor->requires_grad ? 1 : 0);
        writer.write_u8(saved.state_flags);
        writer.write_u8((uint8_t)tensor->data.dtype());
        writer.write_u64((uint64_t)tensor->data.size());
        for (size_t dim : tensor->shape) writer.write_u64((uint64_t)dim);
        if (saved.state_flags & CKPT_STATE_ADAM) {
            writer.write_u64(tensor->adam_step);
            writer.write_f64(tensor->adam_beta1_product);
            writer.write_f64(tensor->adam_beta2_product);
        }
        if (saved.staged_cuda) {
            checkpoint_write_values(writer, saved.staged_data);
        } else {
            checkpoint_write_values(writer, tensor->data);
        }
        if (saved.state_flags & CKPT_STATE_MASTER) {
            checkpoint_write_values(writer, saved.staged_master);
        }
        if (saved.state_flags & CKPT_STATE_ADAM) {
            if (saved.staged_cuda) {
                checkpoint_write_values(writer, saved.staged_adam_m);
                checkpoint_write_values(writer, saved.staged_adam_v);
            } else {
                checkpoint_write_values(writer, tensor->adam_m);
                checkpoint_write_values(writer, tensor->adam_v);
            }
        }
        if (saved.state_flags & CKPT_STATE_SGD) {
            if (saved.staged_cuda) {
                checkpoint_write_values(writer, saved.staged_sgd_velocity);
            } else {
                checkpoint_write_values(writer, tensor->sgd_velocity);
            }
        }
    }
    return writer.finish();
}

inline std::string checkpoint_random_suffix() {
    uint64_t first = (uint64_t)std::chrono::high_resolution_clock::now()
                         .time_since_epoch().count();
    uint64_t second = (uint64_t)(uintptr_t)&first;
    try {
        std::random_device random;
        first ^= (uint64_t(random()) << 32) ^ uint64_t(random());
        second ^= (uint64_t(random()) << 32) ^ uint64_t(random());
    } catch (...) {
        second ^= first * 0x9e3779b97f4a7c15ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << first
        << std::setw(16) << std::setfill('0') << second;
    return out.str();
}

class CheckpointTempGuard {
    std::filesystem::path path_;
    bool active_ = true;

public:
    explicit CheckpointTempGuard(std::filesystem::path path)
        : path_(std::move(path)) {}
    ~CheckpointTempGuard() {
        if (!active_) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    void release() { active_ = false; }
};

inline std::filesystem::path checkpoint_unique_temp_path(const std::string& path,
                                                         int line) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::string suffix = ".tmp." + checkpoint_random_suffix()
                           + "." + std::to_string(attempt);
        std::filesystem::path candidate = fs_path_from_utf8(path + suffix);
        std::error_code ec;
        bool exists = std::filesystem::exists(candidate, ec);
        if (ec) {
            throw JitThrow{"autograd_save_checkpoint(): cannot inspect temporary path: "
                           + ec.message(), line};
        }
        if (!exists) return candidate;
    }
    throw JitThrow{"autograd_save_checkpoint(): cannot create a unique temporary path",
                   line};
}

inline void checkpoint_commit_file(const std::filesystem::path& temp,
                                   const std::filesystem::path& target,
                                   int line) {
    std::error_code ec;
    std::filesystem::file_status existing = std::filesystem::symlink_status(target, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        throw JitThrow{"autograd_save_checkpoint(): cannot inspect target: " + ec.message(),
                       line};
    }
    if (!ec && existing.type() != std::filesystem::file_type::not_found) {
        if (existing.type() == std::filesystem::file_type::symlink) {
            throw JitThrow{"autograd_save_checkpoint(): refusing to replace a symlink",
                           line};
        }
        if (existing.type() != std::filesystem::file_type::regular) {
            throw JitThrow{"autograd_save_checkpoint(): target must be a regular file",
                           line};
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD code = GetLastError();
        throw JitThrow{"autograd_save_checkpoint(): atomic commit failed (Windows error "
                       + std::to_string((unsigned long)code) + ")", line};
    }
#else
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        throw JitThrow{"autograd_save_checkpoint(): atomic commit failed: " + ec.message(),
                       line};
    }
#endif
}

enum class CheckpointBufferKind {
    DATA,
    MASTER,
    ADAM_M,
    ADAM_V,
    SGD_VELOCITY
};

inline void checkpoint_read_values(CheckpointReader& reader, uint64_t count,
                                   CheckpointBufferKind kind,
                                   std::vector<double>* destination,
                                   int line) {
    if (destination) destination->resize((size_t)count);
    for (uint64_t i = 0; i < count; ++i) {
        double value = reader.read_f64();
        if (!std::isfinite(value)) {
            throw JitThrow{"autograd_load_checkpoint(): non-finite tensor or optimizer value",
                           line};
        }
        if (kind == CheckpointBufferKind::ADAM_V && value < 0.0) {
            throw JitThrow{"autograd_load_checkpoint(): Adam variance must be non-negative",
                           line};
        }
        if (destination) (*destination)[(size_t)i] = value;
    }
}

inline std::vector<float> checkpoint_pack_cuda_f32(
        const char* api, const std::vector<double>& values,
        bool non_negative, int line, bool require_exact = false) {
    std::vector<float> packed(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        float converted = (float)values[i];
        if (!std::isfinite(converted)
            || (non_negative && converted < 0.0f)
            || (require_exact
                && ((double)converted != values[i]
                    || std::signbit(converted) != std::signbit(values[i])))) {
            throw JitThrow{std::string(api)
                           + (require_exact
                               ? "(): checkpoint f32 state is not stored exactly"
                               : non_negative
                               ? "(): optimizer variance is not representable as "
                                 "non-negative CUDA float32"
                               : "(): optimizer state is not representable as CUDA float32"),
                           line};
        }
        packed[i] = converted;
    }
    return packed;
}

inline std::shared_ptr<AgCudaAllocation> checkpoint_upload_cuda_f32(
        const char* api, const std::vector<double>& values,
        bool non_negative, int line,
        std::vector<float>* packed_copy = nullptr) {
    std::vector<float> packed = checkpoint_pack_cuda_f32(
        api, values, non_negative, line);
    auto allocation = ag_cuda_allocate(api, values.size(), line);
    if (!SuraCudaDriver::instance().upload_f32(allocation->handle, packed)) {
        ag_cuda_fail(api, line);
    }
    if (packed_copy) *packed_copy = packed;
    return allocation;
}

struct CheckpointScanResult {
    std::array<unsigned char, 32> digest{};
    uint64_t load_buffer_bytes = 0;
    uint64_t device_buffer_bytes = 0;
    uint32_t tensor_count = 0;
    bool has_master = false;
    bool has_cuda_optimizer = false;
    bool cuda_incompatible_dtype = false;
};

inline void checkpoint_discard_loaded_tensors(std::vector<GCTensor*>& tensors) {
    for (GCTensor* tensor : tensors) {
        if (!tensor) continue;
        tensor->cuda_grad.reset();
        tensor->cuda_master_data.reset();
        tensor->cuda_sgd_velocity.reset();
        tensor->cuda_adam_m.reset();
        tensor->cuda_adam_v.reset();
        tensor->cuda_data.reset();
        size_t reserved = tensor->tracked_bytes;
        tensor->data.clear_and_release();
        std::vector<double>().swap(tensor->grad);
        std::vector<double>().swap(tensor->adam_m);
        std::vector<double>().swap(tensor->adam_v);
        std::vector<double>().swap(tensor->sgd_velocity);
        std::vector<size_t>().swap(tensor->shape);
        std::vector<GCTensor*>().swap(tensor->parents);
        std::vector<uint64_t>().swap(tensor->parent_versions);
        ag_release_bytes(tensor, reserved);
    }
    tensors.clear();
}

inline CheckpointScanResult checkpoint_scan_file(
    const std::filesystem::path& path, bool restore_optimizer,
    bool target_cuda, int line,
    GCDict* materialized, std::vector<GCTensor*>* created) {
    const char* api = "autograd_load_checkpoint";
    const bool validation_only = materialized == nullptr;
    std::error_code ec;
    std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    if (ec || status.type() != std::filesystem::file_type::regular) {
        if (!ec && status.type() == std::filesystem::file_type::symlink) {
            throw JitThrow{"autograd_load_checkpoint(): refusing to read a symlink", line};
        }
        throw JitThrow{"autograd_load_checkpoint(): path must be a regular file", line};
    }
    uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw JitThrow{"autograd_load_checkpoint(): cannot determine file size: "
                       + ec.message(), line};
    }
    constexpr uint64_t minimum_file_size = 8 + 2 + 2 + 4 + 32;
    if (file_size < minimum_file_size) {
        throw JitThrow{"autograd_load_checkpoint(): truncated checkpoint", line};
    }
    if (file_size > CKPT_MAX_FILE_BYTES) {
        throw JitThrow{"autograd_load_checkpoint(): checkpoint exceeds the file size limit",
                       line};
    }

    CheckpointReader reader(path, line);
    unsigned char magic[sizeof(CKPT_MAGIC)];
    reader.read(magic, sizeof(magic));
    if (std::memcmp(magic, CKPT_MAGIC, sizeof(magic)) != 0) {
        throw JitThrow{"autograd_load_checkpoint(): invalid checkpoint magic", line};
    }
    uint16_t version = reader.read_u16();
    if (version != 1 && version != 2 && version != CKPT_VERSION) {
        throw JitThrow{"autograd_load_checkpoint(): unsupported checkpoint version "
                       + std::to_string(version), line};
    }
    if (reader.read_u16() != 0) {
        throw JitThrow{"autograd_load_checkpoint(): unknown global flags", line};
    }
    uint32_t tensor_count = reader.read_u32();
    if (tensor_count == 0 || tensor_count > CKPT_MAX_TENSORS) {
        throw JitThrow{"autograd_load_checkpoint(): invalid tensor count", line};
    }

    std::unordered_set<std::string> names;
    names.reserve(tensor_count);
    uint64_t persisted_buffer_bytes = 0;
    uint64_t load_buffer_bytes = 0;
    uint64_t device_buffer_bytes = 0;
    bool has_master = false;
    bool has_cuda_optimizer = false;
    bool cuda_incompatible_dtype = false;

    for (uint32_t record = 0; record < tensor_count; ++record) {
        uint32_t name_size = reader.read_u32();
        if (name_size == 0 || name_size > CKPT_MAX_NAME_BYTES) {
            throw JitThrow{"autograd_load_checkpoint(): invalid tensor name length", line};
        }
        std::string name(name_size, '\0');
        reader.read(name.data(), name.size());
        checkpoint_validate_name(api, name, line);
        if (!names.insert(name).second) {
            throw JitThrow{"autograd_load_checkpoint(): duplicate tensor name '"
                           + name + "'", line};
        }

        uint8_t rank = reader.read_u8();
        uint8_t requires_grad = reader.read_u8();
        uint8_t state_flags = reader.read_u8();
        uint8_t record_flags = reader.read_u8();
        if (rank > AG_MAX_RANK) {
            throw JitThrow{"autograd_load_checkpoint(): tensor rank exceeds the safety limit",
                           line};
        }
        if (requires_grad > 1) {
            throw JitThrow{"autograd_load_checkpoint(): invalid requires_grad flag", line};
        }
        const uint8_t known_state_flags = version >= 3
            ? CKPT_STATE_KNOWN
            : (uint8_t)(CKPT_STATE_ADAM | CKPT_STATE_SGD);
        if ((state_flags & ~known_state_flags) != 0) {
            throw JitThrow{"autograd_load_checkpoint(): unknown tensor state flags", line};
        }
        TensorDType dtype = TensorDType::FLOAT64;
        if (version == 1) {
            if (record_flags != 0) {
                throw JitThrow{"autograd_load_checkpoint(): unknown tensor record flags", line};
            }
        } else {
            if (record_flags > (uint8_t)TensorDType::BFLOAT16) {
                throw JitThrow{"autograd_load_checkpoint(): invalid tensor dtype", line};
            }
            dtype = (TensorDType)record_flags;
        }
        const bool low_precision = dtype == TensorDType::FLOAT16
            || dtype == TensorDType::BFLOAT16;
        if (state_flags & CKPT_STATE_MASTER) {
            if (!low_precision || requires_grad == 0) {
                throw JitThrow{
                    "autograd_load_checkpoint(): master weights require a trainable "
                    "float16 or bfloat16 tensor", line};
            }
            has_master = true;
        }
        if (state_flags & CKPT_STATE_CUDA_OPTIMIZER) {
            if (requires_grad == 0
                || !(state_flags
                     & (CKPT_STATE_MASTER | CKPT_STATE_ADAM | CKPT_STATE_SGD))) {
                throw JitThrow{
                    "autograd_load_checkpoint(): invalid CUDA optimizer state marker",
                    line};
            }
            has_cuda_optimizer = true;
        }
        if (target_cuda && dtype == TensorDType::FLOAT64) {
            cuda_incompatible_dtype = true;
        }

        uint64_t numel = reader.read_u64();
        if (numel == 0 || numel > ag_max_elements()
            || numel > (uint64_t)std::numeric_limits<size_t>::max()) {
            throw JitThrow{"autograd_load_checkpoint(): invalid tensor element count", line};
        }
        std::vector<size_t> shape;
        shape.reserve(rank);
        uint64_t computed = 1;
        for (uint8_t axis = 0; axis < rank; ++axis) {
            uint64_t dim = reader.read_u64();
            if (dim == 0 || dim > ag_max_elements()
                || dim > (uint64_t)std::numeric_limits<size_t>::max()
                || computed > (uint64_t)ag_max_elements() / dim) {
                throw JitThrow{"autograd_load_checkpoint(): invalid tensor shape", line};
            }
            computed *= dim;
            shape.push_back((size_t)dim);
        }
        if (computed != numel) {
            throw JitThrow{"autograd_load_checkpoint(): tensor shape/size mismatch", line};
        }

        uint64_t adam_step = 0;
        double adam_beta1_product = 1.0;
        double adam_beta2_product = 1.0;
        if (state_flags & CKPT_STATE_ADAM) {
            adam_step = reader.read_u64();
            adam_beta1_product = reader.read_f64();
            adam_beta2_product = reader.read_f64();
            if (adam_step == 0
                || !std::isfinite(adam_beta1_product)
                || !std::isfinite(adam_beta2_product)
                || adam_beta1_product < 0.0 || adam_beta1_product > 1.0
                || adam_beta2_product < 0.0 || adam_beta2_product > 1.0) {
                throw JitThrow{"autograd_load_checkpoint(): invalid Adam optimizer metadata",
                               line};
            }
        }

        uint64_t one_buffer = checkpoint_checked_mul(numel, sizeof(double), api, line);
        uint64_t persisted_multiplier = 1;
        if (state_flags & CKPT_STATE_MASTER) persisted_multiplier += 1;
        if (state_flags & CKPT_STATE_ADAM) persisted_multiplier += 2;
        if (state_flags & CKPT_STATE_SGD) persisted_multiplier += 1;
        checkpoint_checked_add(
            persisted_buffer_bytes,
            checkpoint_checked_mul(one_buffer, persisted_multiplier, api, line),
            (uint64_t)ag_max_external_bytes(), api, line);

        uint64_t data_buffer = checkpoint_checked_mul(
            numel, (uint64_t)tensor_dtype_size(dtype), api, line);
        uint64_t optimizer_buffers = 0;
        if (restore_optimizer && (state_flags & CKPT_STATE_MASTER)) optimizer_buffers += 1;
        if (restore_optimizer && (state_flags & CKPT_STATE_ADAM)) optimizer_buffers += 2;
        if (restore_optimizer && (state_flags & CKPT_STATE_SGD)) optimizer_buffers += 1;
        uint64_t materialized_bytes = data_buffer;
        const uint64_t state_host_bytes = checkpoint_checked_mul(
            one_buffer, optimizer_buffers, api, line);
        checkpoint_checked_add(materialized_bytes, state_host_bytes,
                               (uint64_t)ag_max_external_bytes(), api, line);
        checkpoint_checked_add(
            load_buffer_bytes, materialized_bytes,
            (uint64_t)ag_max_external_bytes(), api, line);
        if (target_cuda) {
            uint64_t device_optimizer_buffers = optimizer_buffers;
            if (restore_optimizer && low_precision
                && !(state_flags & CKPT_STATE_MASTER)
                && (state_flags & (CKPT_STATE_ADAM | CKPT_STATE_SGD))) {
                device_optimizer_buffers += 1;
            }
            uint64_t persistent_device_bytes = data_buffer;
            checkpoint_checked_add(
                persistent_device_bytes,
                checkpoint_checked_mul(numel,
                                       sizeof(float) * device_optimizer_buffers,
                                       api, line),
                (uint64_t)AG_HARD_MAX_EXTERNAL_BYTES, api, line);
            checkpoint_checked_add(
                device_buffer_bytes, persistent_device_bytes,
                (uint64_t)AG_HARD_MAX_EXTERNAL_BYTES, api, line);
        }
        if (validation_only) {
            uint64_t validation_buffers = 1;
            // Master validation simultaneously holds visible/master f64,
            // packed f32, re-expanded f64, and a quantized visible buffer.
            // Four f64-sized buffers conservatively cover that peak.
            if (state_flags & CKPT_STATE_MASTER) validation_buffers = 4;
            if (target_cuda && restore_optimizer
                && (state_flags & CKPT_STATE_ADAM)) {
                validation_buffers = std::max<uint64_t>(validation_buffers, 3);
            } else if (target_cuda && restore_optimizer
                       && (state_flags & CKPT_STATE_SGD)) {
                validation_buffers = std::max<uint64_t>(validation_buffers, 2);
            }
            const uint64_t validation_peak = checkpoint_checked_mul(
                one_buffer, validation_buffers, api, line);
            if (validation_peak > (uint64_t)std::numeric_limits<size_t>::max()) {
                throw JitThrow{
                    "autograd_load_checkpoint(): validation staging size overflows",
                    line};
            }
            ag_preflight_bytes((size_t)validation_peak, api, line);
        }

        GCTensor* tensor = nullptr;
        std::unique_ptr<GCNativeRoot> tensor_root;
        if (materialized) {
            Value value = Value::make_tensor();
            tensor = value.as_tensor();
            tensor_root = std::make_unique<GCNativeRoot>(value.as_obj());
            if (created) created->push_back(tensor);
            const size_t reservation = target_cuda
                ? (size_t)data_buffer
                : (size_t)materialized_bytes;
            ag_reserve_bytes(tensor, reservation, api, line);
            tensor->shape = shape;
            tensor->requires_grad = requires_grad != 0;
            tensor->op = TensorOp::LEAF;
            tensor->graph_freed = false;
            tensor->version = 0;
            tensor->adam_step = 0;
            tensor->adam_beta1_product = 1.0;
            tensor->adam_beta2_product = 1.0;
            materialized->elements.emplace(name, value);
        }

        std::vector<double> loaded_data;
        checkpoint_read_values(reader, numel, CheckpointBufferKind::DATA,
                               tensor || validation_only ? &loaded_data : nullptr,
                               line);
        if (tensor || validation_only) {
            // A valid checksum proves integrity, not representability in the
            // persisted dtype. Validate before TensorBuffer quantization so
            // crafted F16/BF16 records cannot become infinity during load.
            ag_validate_dtype_values(api, loaded_data, dtype, line);
        }
        if (tensor) {
            tensor->data.assign(loaded_data, dtype);
            if (target_cuda) ag_cuda_upload(tensor, api, line);
        }
        if (state_flags & CKPT_STATE_MASTER) {
            std::vector<double> loaded_master;
            checkpoint_read_values(
                reader, numel, CheckpointBufferKind::MASTER,
                validation_only
                    || (tensor && restore_optimizer && target_cuda)
                    ? &loaded_master : nullptr,
                line);
            if (validation_only || (tensor && restore_optimizer && target_cuda)) {
                std::vector<float> packed_master = checkpoint_pack_cuda_f32(
                    api, loaded_master, false, line, true);
                std::vector<double> rounded_master(
                    packed_master.begin(), packed_master.end());
                ag_validate_dtype_values(api, rounded_master, dtype, line);
                TensorBuffer repacked;
                repacked.assign(rounded_master, dtype);
                TensorBuffer validated_visible;
                const TensorBuffer* visible = nullptr;
                if (tensor) {
                    visible = &tensor->data;
                } else {
                    validated_visible.assign(loaded_data, dtype);
                    visible = &validated_visible;
                }
                if (repacked.byte_size() != visible->byte_size()
                    || std::memcmp(repacked.packed_data(),
                                   visible->packed_data(),
                                   repacked.byte_size()) != 0) {
                    throw JitThrow{
                        "autograd_load_checkpoint(): master weights do not match "
                        "visible tensor storage", line};
                }
                if (tensor && restore_optimizer && target_cuda) {
                    tensor->cuda_master_data = checkpoint_upload_cuda_f32(
                        api, loaded_master, false, line);
                }
            }
        }
        if (state_flags & CKPT_STATE_ADAM) {
            std::vector<double> loaded_adam_m;
            std::vector<double> loaded_adam_v;
            checkpoint_read_values(
                reader, numel, CheckpointBufferKind::ADAM_M,
                validation_only && target_cuda && restore_optimizer
                    ? &loaded_adam_m
                    : (tensor && restore_optimizer
                        ? (target_cuda ? &loaded_adam_m : &tensor->adam_m)
                        : nullptr),
                line);
            checkpoint_read_values(
                reader, numel, CheckpointBufferKind::ADAM_V,
                validation_only && target_cuda && restore_optimizer
                    ? &loaded_adam_v
                    : (tensor && restore_optimizer
                        ? (target_cuda ? &loaded_adam_v : &tensor->adam_v)
                        : nullptr),
                line);
            if (validation_only && target_cuda && restore_optimizer) {
                (void)checkpoint_pack_cuda_f32(
                    api, loaded_adam_m, false, line);
                (void)checkpoint_pack_cuda_f32(
                    api, loaded_adam_v, true, line);
            }
            if (tensor && restore_optimizer) {
                if (target_cuda) {
                    tensor->cuda_adam_m = checkpoint_upload_cuda_f32(
                        api, loaded_adam_m, false, line);
                    tensor->cuda_adam_v = checkpoint_upload_cuda_f32(
                        api, loaded_adam_v, true, line);
                }
                tensor->adam_step = adam_step;
                tensor->adam_beta1_product = adam_beta1_product;
                tensor->adam_beta2_product = adam_beta2_product;
            }
        }
        if (state_flags & CKPT_STATE_SGD) {
            std::vector<double> loaded_velocity;
            checkpoint_read_values(
                reader, numel, CheckpointBufferKind::SGD_VELOCITY,
                validation_only && target_cuda && restore_optimizer
                    ? &loaded_velocity
                    : (tensor && restore_optimizer
                        ? (target_cuda ? &loaded_velocity : &tensor->sgd_velocity)
                        : nullptr),
                line);
            if (validation_only && target_cuda && restore_optimizer) {
                (void)checkpoint_pack_cuda_f32(
                    api, loaded_velocity, false, line);
            }
            if (tensor && restore_optimizer && target_cuda) {
                tensor->cuda_sgd_velocity = checkpoint_upload_cuda_f32(
                    api, loaded_velocity, false, line);
            }
        }
        if (tensor && restore_optimizer && target_cuda && low_precision
            && !(state_flags & CKPT_STATE_MASTER)
            && (state_flags & (CKPT_STATE_ADAM | CKPT_STATE_SGD))) {
            // v1/v2 and CPU-origin checkpoints predate f32 master records.
            // Their visible value is authoritative, so synthesize the initial
            // CUDA master from that quantized value.
            std::vector<double> quantized_visible = tensor->data.to_vector();
            tensor->cuda_master_data = checkpoint_upload_cuda_f32(
                api, quantized_visible, false, line);
        }
        if (tensor && target_cuda) {
            const size_t host_bytes = tensor->data.host_byte_size();
            TensorBuffer metadata;
            metadata.assign_device_metadata((size_t)numel, dtype);
            tensor->data = std::move(metadata);
            ag_release_bytes(tensor, host_bytes);
            tensor->coherence = TensorCoherence::DEVICE_ONLY;
        }
    }

    CheckpointScanResult result;
    result.digest = reader.verify_footer();
    result.load_buffer_bytes = std::max(
        load_buffer_bytes, persisted_buffer_bytes);
    result.device_buffer_bytes = device_buffer_bytes;
    result.tensor_count = tensor_count;
    result.has_master = has_master;
    result.has_cuda_optimizer = has_cuda_optimizer;
    result.cuda_incompatible_dtype = cuda_incompatible_dtype;
    return result;
}

inline bool checkpoint_digest_equal(const std::array<unsigned char, 32>& left,
                                    const std::array<unsigned char, 32>& right) {
    unsigned difference = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        difference |= unsigned(left[i] ^ right[i]);
    }
    return difference == 0;
}

inline Value b_autograd_save_checkpoint(const Value* a, int n, int l) {
    need_args("autograd_save_checkpoint", n, 2, 3, l);
    std::string path = need_str("autograd_save_checkpoint", a[1], 1, l);
    checkpoint_validate_path("autograd_save_checkpoint", path, l);
    bool include_optimizer = checkpoint_option_optimizer(
        "autograd_save_checkpoint", a, n, 2, l);

    try {
        std::vector<CheckpointSaveTensor> tensors = checkpoint_collect_save_tensors(
            a[0], include_optimizer, l);
        std::filesystem::path target = fs_path_from_utf8(path);
        std::filesystem::path parent = target.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                throw JitThrow{"autograd_save_checkpoint(): cannot create parent directory: "
                               + ec.message(), l};
            }
        }

        std::filesystem::path temp = checkpoint_unique_temp_path(path, l);
        CheckpointTempGuard cleanup(temp);
        uint64_t bytes = checkpoint_write_file(temp, tensors, l);
        checkpoint_commit_file(temp, target, l);
        cleanup.release();
        return Value((double)bytes);
    } catch (const JitThrow&) {
        throw;
    } catch (const std::exception& error) {
        throw JitThrow{"autograd_save_checkpoint(): " + std::string(error.what()), l};
    } catch (...) {
        throw JitThrow{"autograd_save_checkpoint(): unexpected checkpoint failure", l};
    }
}

inline Value b_autograd_load_checkpoint(const Value* a, int n, int l) {
    need_args("autograd_load_checkpoint", n, 1, 2, l);
    std::string path = need_str("autograd_load_checkpoint", a[0], 0, l);
    checkpoint_validate_path("autograd_load_checkpoint", path, l);
    CheckpointLoadOptions options = checkpoint_load_options(
        "autograd_load_checkpoint", a, n, 1, l);
    std::filesystem::path source = fs_path_from_utf8(path);

    std::vector<GCTensor*> created;
    try {
        // First pass validates every field and the complete digest without
        // allocating Tensor buffers.  The second pass materializes only after
        // the complete file is known to be structurally valid.
        CheckpointScanResult validated = checkpoint_scan_file(
            source, options.optimizer, options.cuda, l, nullptr, nullptr);
        if (options.optimizer
            && (validated.has_master || validated.has_cuda_optimizer)
            && !options.cuda) {
            throw JitThrow{
                "autograd_load_checkpoint(): checkpoint contains CUDA optimizer "
                "state; use {device: \"cuda\"} for exact optimizer resume or "
                "{optimizer: false} for visible weights only", l};
        }
        if (options.cuda && validated.cuda_incompatible_dtype) {
            throw JitThrow{
                "autograd_load_checkpoint(): float64 tensors cannot be restored "
                "to resident CUDA storage", l};
        }
        if (options.cuda) {
            if (validated.device_buffer_bytes
                > (uint64_t)std::numeric_limits<size_t>::max()) {
                throw JitThrow{
                    "autograd_load_checkpoint(): CUDA checkpoint size overflows",
                    l};
            }
            SuraCudaDriver& driver = SuraCudaDriver::instance();
            const size_t total = driver.total_memory();
            const size_t allocated = driver.allocated_memory();
            const size_t required = (size_t)validated.device_buffer_bytes;
            if (total != 0
                && (allocated > total || required > total - allocated)) {
                throw JitThrow{
                    "autograd_load_checkpoint(): insufficient CUDA memory for "
                    "transactional restore", l};
            }
        }
        ag_preflight_bytes((size_t)validated.load_buffer_bytes,
                           "autograd_load_checkpoint", l);

        Value result = Value::make_dict();
        GCNativeRoot result_root(result.as_obj());
        created.reserve(validated.tensor_count);
        CheckpointScanResult loaded = checkpoint_scan_file(
            source, options.optimizer, options.cuda, l,
            result.as_dict(), &created);
        if (!checkpoint_digest_equal(validated.digest, loaded.digest)) {
            throw JitThrow{"autograd_load_checkpoint(): checkpoint changed while loading",
                           l};
        }
        return result;
    } catch (const JitThrow&) {
        checkpoint_discard_loaded_tensors(created);
        throw;
    } catch (const std::exception& error) {
        checkpoint_discard_loaded_tensors(created);
        throw JitThrow{"autograd_load_checkpoint(): " + std::string(error.what()), l};
    } catch (...) {
        checkpoint_discard_loaded_tensors(created);
        throw JitThrow{"autograd_load_checkpoint(): unexpected checkpoint failure", l};
    }
}

} // namespace SuraStd
