#pragma once

// Shared-filesystem synchronous gradient all-reduce.
//
// This header is included after autograd.hpp/checkpoint.hpp.  It deliberately
// reuses the checkpoint SHA-256 implementation, but defines its own strict
// binary protocol and never mutates a Tensor until every rank file has passed
// both structural and checksum validation.

#include "checkpoint.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace SuraStd {

static constexpr unsigned char DIST_GRAD_MAGIC[8] = {
    'S', 'U', 'R', 'A', 'G', 'R', 'A', 'D'
};
static constexpr uint16_t DIST_GRAD_VERSION = 1;
static constexpr uint16_t DIST_GRAD_FLAG_AVERAGE = 1;
static constexpr uint16_t DIST_GRAD_KNOWN_FLAGS = DIST_GRAD_FLAG_AVERAGE;
static constexpr size_t DIST_GRAD_MAX_WORLD_SIZE = 1024;
static constexpr size_t DIST_GRAD_MAX_RUN_ID_BYTES = 512;
static constexpr size_t DIST_GRAD_MAX_PATH_BYTES = 32768;
static constexpr size_t DIST_GRAD_MAX_TIMEOUT_MS = 3600000;
static constexpr uint64_t DIST_GRAD_MAX_FILE_BYTES =
    65ULL * 1024ULL * 1024ULL * 1024ULL;

struct DistributedGradientOptions {
    std::string rendezvous;
    std::string run_id;
    uint64_t step = 0;
    size_t rank = 0;
    size_t world_size = 1;
    bool average = true;
    size_t timeout_ms = 30000;
};

struct DistributedGradientParameter {
    GCTensor* tensor = nullptr;
    uint8_t dtype = 0;
    size_t offset = 0;
};

inline const Value& distributed_required_option(const GCDict* options,
                                                const char* key, int line) {
    auto found = options->elements.find(key);
    if (found == options->elements.end()) {
        throw JitThrow{std::string("autograd_all_reduce_gradients(): missing option '")
                       + key + "'", line};
    }
    return found->second;
}

inline std::string distributed_required_string(const GCDict* options,
                                               const char* key, size_t maximum,
                                               int line) {
    const Value& value = distributed_required_option(options, key, line);
    if (!value.is_str()) {
        throw JitThrow{std::string("autograd_all_reduce_gradients(): option '")
                       + key + "' must be a string", line};
    }
    const std::string& text = value.as_str_ref();
    if (text.empty() || text.size() > maximum || text.find('\0') != std::string::npos
        || !checkpoint_valid_utf8(text)) {
        throw JitThrow{std::string("autograd_all_reduce_gradients(): option '")
                       + key + "' must be non-empty valid UTF-8 no longer than "
                       + std::to_string(maximum) + " bytes", line};
    }
    return text;
}

inline size_t distributed_required_integer(const GCDict* options,
                                           const char* key, size_t maximum,
                                           int line) {
    const Value& value = distributed_required_option(options, key, line);
    if (!value.is_num() || !std::isfinite(value.as_num())
        || value.as_num() < 0.0 || value.as_num() != std::floor(value.as_num())
        || value.as_num() > (double)maximum) {
        throw JitThrow{std::string("autograd_all_reduce_gradients(): option '")
                       + key + "' must be an integer between 0 and "
                       + std::to_string(maximum), line};
    }
    return (size_t)value.as_num();
}

inline DistributedGradientOptions distributed_parse_options(const Value* args,
                                                             int nargs,
                                                             int line) {
    need_args("autograd_all_reduce_gradients", nargs, 2, 2, line);
    GCDict* dict = need_dict("autograd_all_reduce_gradients", args[1], 1, line);
    ag_validate_options("autograd_all_reduce_gradients", dict,
                        {"rendezvous", "run_id", "step", "rank", "world_size",
                         "average", "timeout_ms"}, line);

    DistributedGradientOptions options;
    options.rendezvous = distributed_required_string(
        dict, "rendezvous", DIST_GRAD_MAX_PATH_BYTES, line);
    options.run_id = distributed_required_string(
        dict, "run_id", DIST_GRAD_MAX_RUN_ID_BYTES, line);
    options.step = (uint64_t)distributed_required_integer(
        dict, "step", (size_t)9007199254740991ULL, line);
    options.rank = distributed_required_integer(
        dict, "rank", DIST_GRAD_MAX_WORLD_SIZE - 1, line);
    options.world_size = distributed_required_integer(
        dict, "world_size", DIST_GRAD_MAX_WORLD_SIZE, line);
    if (options.world_size == 0) {
        throw JitThrow{"autograd_all_reduce_gradients(): world_size must be at least 1",
                       line};
    }
    if (options.rank >= options.world_size) {
        throw JitThrow{"autograd_all_reduce_gradients(): rank must be smaller than world_size",
                       line};
    }

    auto average = dict->elements.find("average");
    if (average != dict->elements.end()) {
        if (!average->second.is_bool()) {
            throw JitThrow{"autograd_all_reduce_gradients(): option 'average' must be a bool",
                           line};
        }
        options.average = average->second.as_bool();
    }
    auto timeout = dict->elements.find("timeout_ms");
    if (timeout != dict->elements.end()) {
        options.timeout_ms = distributed_required_integer(
            dict, "timeout_ms", DIST_GRAD_MAX_TIMEOUT_MS, line);
        if (options.timeout_ms == 0) {
            throw JitThrow{"autograd_all_reduce_gradients(): timeout_ms must be at least 1",
                           line};
        }
    }
    return options;
}

inline uint64_t distributed_checked_add(uint64_t left, uint64_t right, int line) {
    if (right > DIST_GRAD_MAX_FILE_BYTES
        || left > DIST_GRAD_MAX_FILE_BYTES - right) {
        throw JitThrow{"autograd_all_reduce_gradients(): gradient file is too large",
                       line};
    }
    return left + right;
}

inline uint64_t distributed_checked_mul(uint64_t left, uint64_t right, int line) {
    if (left != 0 && right > DIST_GRAD_MAX_FILE_BYTES / left) {
        throw JitThrow{"autograd_all_reduce_gradients(): gradient file is too large",
                       line};
    }
    return left * right;
}

inline std::vector<DistributedGradientParameter> distributed_collect_parameters(
    const Value& value, uint64_t& total_elements, int line) {
    std::vector<GCTensor*> tensors;
    ag_collect_parameters("autograd_all_reduce_gradients", value, tensors, line);
    ag_require_unique_parameters("autograd_all_reduce_gradients", tensors, line, true);

    std::vector<DistributedGradientParameter> parameters;
    parameters.reserve(tensors.size());
    if (tensors.size() > (size_t)std::numeric_limits<uint32_t>::max()) {
        throw JitThrow{"autograd_all_reduce_gradients(): too many parameters",
                       line};
    }
    total_elements = 0;
    for (GCTensor* tensor : tensors) {
        ag_validate_parameter("autograd_all_reduce_gradients", tensor, line);
        if (tensor->cuda_grad) {
            throw JitThrow{
                "autograd_all_reduce_gradients(): resident CUDA gradients are not supported "
                "by the file all-reduce backend yet", line};
        }
        if (tensor->grad.empty()) {
            throw JitThrow{"autograd_all_reduce_gradients(): every parameter must have a gradient",
                           line};
        }
        if (tensor->grad.size() != tensor->data.size()) {
            throw JitThrow{"autograd_all_reduce_gradients(): corrupted gradient buffer",
                           line};
        }
        size_t expected = ag_numel(
            "autograd_all_reduce_gradients", tensor->shape, line);
        if (expected != tensor->data.size()) {
            throw JitThrow{"autograd_all_reduce_gradients(): parameter shape/data size mismatch",
                           line};
        }
        for (double gradient : tensor->grad) {
            if (!std::isfinite(gradient)) {
                throw JitThrow{"autograd_all_reduce_gradients(): gradients must be finite",
                               line};
            }
        }
        if ((uint64_t)tensor->grad.size()
            > std::numeric_limits<uint64_t>::max() - total_elements) {
            throw JitThrow{"autograd_all_reduce_gradients(): total gradient size overflows",
                           line};
        }
        DistributedGradientParameter parameter;
        parameter.tensor = tensor;
        parameter.dtype = (uint8_t)tensor->data.dtype();
        parameter.offset = (size_t)total_elements;
        parameters.push_back(parameter);
        total_elements += (uint64_t)tensor->grad.size();
    }
    return parameters;
}

inline std::string distributed_digest_hex(const std::string& text) {
    CheckpointSha256 hash;
    hash.update(text.data(), text.size());
    std::array<unsigned char, 32> digest = hash.finish();
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 15U];
    }
    return result;
}

inline std::filesystem::path distributed_step_directory(
    const DistributedGradientOptions& options) {
    std::filesystem::path base = fs_path_from_utf8(options.rendezvous);
    std::string run_key = "run-" + distributed_digest_hex(options.run_id);
    return base / run_key / ("step-" + std::to_string(options.step));
}

inline std::filesystem::path distributed_rank_path(
    const std::filesystem::path& directory, size_t rank) {
    return directory / ("rank-" + std::to_string(rank) + ".sgrad");
}

inline uint64_t distributed_expected_file_size(
    const DistributedGradientOptions& options,
    const std::vector<DistributedGradientParameter>& parameters,
    uint64_t total_elements, int line) {
    uint64_t bytes = 44; // fixed header through total_elements
    bytes = distributed_checked_add(bytes, (uint64_t)options.run_id.size(), line);
    for (const auto& parameter : parameters) {
        bytes = distributed_checked_add(bytes, 16, line);
        bytes = distributed_checked_add(
            bytes, distributed_checked_mul(
                (uint64_t)parameter.tensor->shape.size(), 8, line), line);
    }
    bytes = distributed_checked_add(
        bytes, distributed_checked_mul(total_elements, 8, line), line);
    return distributed_checked_add(bytes, 32, line); // SHA-256 footer
}

class DistributedGradientWriter {
    std::ofstream stream_;
    CheckpointSha256 hash_;
    uint64_t bytes_ = 0;
    int line_ = 0;

public:
    DistributedGradientWriter(const std::filesystem::path& path, int line)
        : stream_(path, std::ios::binary | std::ios::trunc), line_(line) {
        if (!stream_) {
            throw JitThrow{"autograd_all_reduce_gradients(): cannot create rank file",
                           line_};
        }
    }

    void write(const void* bytes, size_t size) {
        if ((uint64_t)size > DIST_GRAD_MAX_FILE_BYTES
            || bytes_ > DIST_GRAD_MAX_FILE_BYTES - (uint64_t)size) {
            throw JitThrow{"autograd_all_reduce_gradients(): gradient file is too large",
                           line_};
        }
        stream_.write(static_cast<const char*>(bytes), (std::streamsize)size);
        if (!stream_) {
            throw JitThrow{"autograd_all_reduce_gradients(): failed writing rank file",
                           line_};
        }
        hash_.update(bytes, size);
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
        if (bytes_ > DIST_GRAD_MAX_FILE_BYTES - digest.size()) {
            throw JitThrow{"autograd_all_reduce_gradients(): gradient file is too large",
                           line_};
        }
        stream_.write(reinterpret_cast<const char*>(digest.data()),
                      (std::streamsize)digest.size());
        bytes_ += (uint64_t)digest.size();
        stream_.flush();
        if (!stream_) {
            throw JitThrow{"autograd_all_reduce_gradients(): failed finalizing rank file",
                           line_};
        }
        stream_.close();
        if (!stream_) {
            throw JitThrow{"autograd_all_reduce_gradients(): failed closing rank file",
                           line_};
        }
        return bytes_;
    }
};

class DistributedGradientReader {
    std::ifstream stream_;
    CheckpointSha256 hash_;
    uint64_t bytes_ = 0;
    int line_ = 0;

public:
    DistributedGradientReader(const std::filesystem::path& path, int line)
        : stream_(path, std::ios::binary), line_(line) {
        if (!stream_) {
            throw JitThrow{"autograd_all_reduce_gradients(): cannot open rank file",
                           line_};
        }
    }

    void read(void* destination, size_t size) {
        if ((uint64_t)size > DIST_GRAD_MAX_FILE_BYTES
            || bytes_ > DIST_GRAD_MAX_FILE_BYTES - (uint64_t)size) {
            throw JitThrow{"autograd_all_reduce_gradients(): gradient file is too large",
                           line_};
        }
        stream_.read(static_cast<char*>(destination), (std::streamsize)size);
        if ((size_t)stream_.gcount() != size) {
            throw JitThrow{"autograd_all_reduce_gradients(): truncated rank file",
                           line_};
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

    void verify_footer() {
        std::array<unsigned char, 32> expected{};
        stream_.read(reinterpret_cast<char*>(expected.data()),
                     (std::streamsize)expected.size());
        if ((size_t)stream_.gcount() != expected.size()) {
            throw JitThrow{"autograd_all_reduce_gradients(): truncated rank checksum",
                           line_};
        }
        std::array<unsigned char, 32> actual = hash_.finish();
        if (!checkpoint_digest_equal(expected, actual)) {
            throw JitThrow{"autograd_all_reduce_gradients(): rank checksum mismatch",
                           line_};
        }
        char trailing = 0;
        stream_.read(&trailing, 1);
        if (stream_.gcount() != 0 || !stream_.eof()) {
            throw JitThrow{"autograd_all_reduce_gradients(): trailing rank file data",
                           line_};
        }
    }
};

class DistributedPathGuard {
    std::filesystem::path path_;
    bool directory_ = false;
    bool active_ = true;
public:
    DistributedPathGuard(std::filesystem::path path, bool directory)
        : path_(std::move(path)), directory_(directory) {}
    ~DistributedPathGuard() {
        if (!active_) return;
        std::error_code ignored;
        if (directory_) std::filesystem::remove(path_, ignored);
        else std::filesystem::remove(path_, ignored);
    }
    void disarm() { active_ = false; }
    DistributedPathGuard(const DistributedPathGuard&) = delete;
    DistributedPathGuard& operator=(const DistributedPathGuard&) = delete;
};

inline uint64_t distributed_publish_rank(
    const DistributedGradientOptions& options,
    const std::vector<DistributedGradientParameter>& parameters,
    uint64_t total_elements, const std::filesystem::path& directory,
    int line) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory, error) || error) {
        throw JitThrow{"autograd_all_reduce_gradients(): cannot create rendezvous directory",
                       line};
    }

    std::filesystem::path lock = directory /
        ("rank-" + std::to_string(options.rank) + ".lock");
    bool acquired = std::filesystem::create_directory(lock, error);
    if (error || !acquired) {
        throw JitThrow{"autograd_all_reduce_gradients(): run_id/step/rank was already used; use a unique run_id",
                       line};
    }
    DistributedPathGuard lock_guard(lock, true);

    std::filesystem::path target = distributed_rank_path(directory, options.rank);
    if (std::filesystem::exists(target, error) || error) {
        throw JitThrow{"autograd_all_reduce_gradients(): rank file already exists",
                       line};
    }
    std::filesystem::path temporary = directory /
        (".rank-" + std::to_string(options.rank) + ".tmp-"
         + checkpoint_random_suffix());
    DistributedPathGuard temporary_guard(temporary, false);

    uint64_t expected = distributed_expected_file_size(
        options, parameters, total_elements, line);

    DistributedGradientWriter writer(temporary, line);
    writer.write(DIST_GRAD_MAGIC, sizeof(DIST_GRAD_MAGIC));
    writer.write_u16(DIST_GRAD_VERSION);
    writer.write_u16(options.average ? DIST_GRAD_FLAG_AVERAGE : 0);
    writer.write_u32((uint32_t)options.world_size);
    writer.write_u32((uint32_t)options.rank);
    writer.write_u64(options.step);
    writer.write_u32((uint32_t)parameters.size());
    writer.write_u32((uint32_t)options.run_id.size());
    writer.write_u64(total_elements);
    writer.write(options.run_id.data(), options.run_id.size());

    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        writer.write_u32((uint32_t)index);
        writer.write_u8(parameter.dtype);
        writer.write_u8((uint8_t)parameter.tensor->shape.size());
        writer.write_u16(0);
        writer.write_u64((uint64_t)parameter.tensor->grad.size());
        for (size_t dimension : parameter.tensor->shape) {
            writer.write_u64((uint64_t)dimension);
        }
    }
    for (const auto& parameter : parameters) {
        for (double gradient : parameter.tensor->grad) writer.write_f64(gradient);
    }
    uint64_t bytes = writer.finish();
    if (bytes != expected) {
        throw JitThrow{"autograd_all_reduce_gradients(): internal rank file size mismatch",
                       line};
    }

    std::filesystem::rename(temporary, target, error);
    if (error) {
        throw JitThrow{"autograd_all_reduce_gradients(): cannot atomically publish rank file: "
                       + error.message(), line};
    }
    temporary_guard.disarm();
    // The lock intentionally remains. It prevents stale run_id/step/rank reuse
    // from overwriting a file on platforms where rename() replaces targets.
    lock_guard.disarm();
    return bytes;
}

inline void distributed_wait_for_ranks(
    const DistributedGradientOptions& options,
    const std::filesystem::path& directory, int line) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(options.timeout_ms);
    while (true) {
        bool ready = true;
        std::vector<size_t> missing;
        for (size_t rank = 0; rank < options.world_size; ++rank) {
            std::filesystem::path path = distributed_rank_path(directory, rank);
            std::error_code error;
            bool exists = std::filesystem::exists(path, error);
            if (error) {
                throw JitThrow{"autograd_all_reduce_gradients(): cannot inspect rendezvous: "
                               + error.message(), line};
            }
            if (!exists) {
                ready = false;
                missing.push_back(rank);
                continue;
            }
            std::filesystem::file_status status =
                std::filesystem::symlink_status(path, error);
            if (error || status.type() != std::filesystem::file_type::regular) {
                throw JitThrow{"autograd_all_reduce_gradients(): rank path must be a regular non-symlink file",
                               line};
            }
        }
        if (ready) return;
        if (std::chrono::steady_clock::now() >= deadline) {
            std::string ranks;
            for (size_t rank : missing) {
                if (!ranks.empty()) ranks += ",";
                ranks += std::to_string(rank);
            }
            throw JitThrow{"autograd_all_reduce_gradients(): timed out waiting for ranks "
                           + ranks, line};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

inline void distributed_read_rank(
    const std::filesystem::path& path,
    const DistributedGradientOptions& options,
    const std::vector<DistributedGradientParameter>& parameters,
    uint64_t total_elements, size_t expected_rank,
    std::vector<std::vector<double>>* accumulated, int line) {
    std::error_code error;
    std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error || status.type() != std::filesystem::file_type::regular) {
        throw JitThrow{"autograd_all_reduce_gradients(): rank path must be a regular non-symlink file",
                       line};
    }
    uintmax_t actual_size = std::filesystem::file_size(path, error);
    uint64_t expected_size = distributed_expected_file_size(
        options, parameters, total_elements, line);
    if (error || actual_size != expected_size) {
        throw JitThrow{"autograd_all_reduce_gradients(): rank file size mismatch",
                       line};
    }

    DistributedGradientReader reader(path, line);
    unsigned char magic[sizeof(DIST_GRAD_MAGIC)];
    reader.read(magic, sizeof(magic));
    if (std::memcmp(magic, DIST_GRAD_MAGIC, sizeof(magic)) != 0) {
        throw JitThrow{"autograd_all_reduce_gradients(): invalid rank file magic",
                       line};
    }
    if (reader.read_u16() != DIST_GRAD_VERSION) {
        throw JitThrow{"autograd_all_reduce_gradients(): unsupported rank file version",
                       line};
    }
    uint16_t flags = reader.read_u16();
    uint16_t expected_flags = options.average ? DIST_GRAD_FLAG_AVERAGE : 0;
    if ((flags & ~DIST_GRAD_KNOWN_FLAGS) != 0 || flags != expected_flags) {
        throw JitThrow{"autograd_all_reduce_gradients(): rank average mode mismatch",
                       line};
    }
    uint32_t world_size = reader.read_u32();
    uint32_t rank = reader.read_u32();
    uint64_t step = reader.read_u64();
    uint32_t parameter_count = reader.read_u32();
    uint32_t run_id_size = reader.read_u32();
    uint64_t file_total_elements = reader.read_u64();
    if (world_size != options.world_size || rank != expected_rank
        || step != options.step || parameter_count != parameters.size()
        || run_id_size != options.run_id.size()
        || file_total_elements != total_elements) {
        throw JitThrow{"autograd_all_reduce_gradients(): incompatible rank header",
                       line};
    }
    std::string run_id(run_id_size, '\0');
    reader.read(run_id.data(), run_id.size());
    if (run_id != options.run_id) {
        throw JitThrow{"autograd_all_reduce_gradients(): rank run_id mismatch",
                       line};
    }

    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        uint32_t order = reader.read_u32();
        uint8_t dtype = reader.read_u8();
        uint8_t rank_count = reader.read_u8();
        uint16_t reserved = reader.read_u16();
        uint64_t elements = reader.read_u64();
        if (order != index || dtype != parameter.dtype
            || rank_count != parameter.tensor->shape.size() || reserved != 0
            || elements != parameter.tensor->grad.size()) {
            throw JitThrow{"autograd_all_reduce_gradients(): parameter order/dtype/shape mismatch",
                           line};
        }
        for (size_t axis = 0; axis < rank_count; ++axis) {
            if (reader.read_u64() != parameter.tensor->shape[axis]) {
                throw JitThrow{"autograd_all_reduce_gradients(): parameter shape mismatch",
                               line};
            }
        }
    }

    for (size_t parameter_index = 0; parameter_index < parameters.size();
         ++parameter_index) {
        const auto& parameter = parameters[parameter_index];
        for (size_t index = 0; index < parameter.tensor->grad.size(); ++index) {
            double gradient = reader.read_f64();
            if (!std::isfinite(gradient)) {
                throw JitThrow{"autograd_all_reduce_gradients(): rank gradient is not finite",
                               line};
            }
            if (accumulated) {
                double contribution = options.average
                    ? gradient / (double)options.world_size : gradient;
                double next = (*accumulated)[parameter_index][index] + contribution;
                if (!std::isfinite(next)) {
                    throw JitThrow{"autograd_all_reduce_gradients(): reduced gradient is not finite",
                                   line};
                }
                (*accumulated)[parameter_index][index] = next;
            }
        }
    }
    reader.verify_footer();
}

inline Value distributed_result_value(const DistributedGradientOptions& options,
                                      size_t parameter_count,
                                      uint64_t total_elements,
                                      uint64_t published_bytes) {
    Value result = Value::make_dict();
    GCDict* dict = result.as_dict();
    dict->elements["rank"] = Value((double)options.rank);
    dict->elements["world_size"] = Value((double)options.world_size);
    dict->elements["step"] = Value((double)options.step);
    dict->elements["parameters"] = Value((double)parameter_count);
    dict->elements["elements"] = Value((double)total_elements);
    dict->elements["average"] = Value(options.average);
    dict->elements["published_bytes"] = Value((double)published_bytes);
    dict->elements["run_id"] = Value(options.run_id);
    return result;
}

inline Value b_autograd_all_reduce_gradients(const Value* args, int nargs,
                                             int line) {
    DistributedGradientOptions options =
        distributed_parse_options(args, nargs, line);
    uint64_t total_elements = 0;
    std::vector<DistributedGradientParameter> parameters =
        distributed_collect_parameters(args[0], total_elements, line);

    // Single-process mode is intentionally a true no-op. It still validates
    // parameters/options, which makes local/distributed configuration switches
    // deterministic without touching the filesystem.
    if (options.world_size == 1) {
        return distributed_result_value(
            options, parameters.size(), total_elements, 0);
    }

    std::filesystem::path directory = distributed_step_directory(options);
    uint64_t published_bytes = distributed_publish_rank(
        options, parameters, total_elements, directory, line);
    distributed_wait_for_ranks(options, directory, line);

    // Pass one validates every rank completely before any reduction is even
    // staged. Pass two revalidates checksums while accumulating, protecting
    // against a file changing between validation and use.
    for (size_t rank = 0; rank < options.world_size; ++rank) {
        distributed_read_rank(distributed_rank_path(directory, rank), options,
                              parameters, total_elements, rank, nullptr, line);
    }

    uint64_t staged_bytes_u64 = distributed_checked_mul(total_elements, 8, line);
    if (staged_bytes_u64 > (uint64_t)std::numeric_limits<size_t>::max()) {
        throw JitThrow{"autograd_all_reduce_gradients(): reduced gradients are too large",
                       line};
    }
    AgTemporaryBytes staged_memory(
        (size_t)staged_bytes_u64, "autograd_all_reduce_gradients", line);
    std::vector<std::vector<double>> staged;
    try {
        staged.resize(parameters.size());
        for (size_t index = 0; index < parameters.size(); ++index) {
            staged[index].assign(parameters[index].tensor->grad.size(), 0.0);
        }
    } catch (const std::bad_alloc&) {
        throw JitThrow{"autograd_all_reduce_gradients(): cannot allocate reduced gradients",
                       line};
    }

    for (size_t rank = 0; rank < options.world_size; ++rank) {
        distributed_read_rank(distributed_rank_path(directory, rank), options,
                              parameters, total_elements, rank, &staged, line);
    }

    // Allocate the return value and perform every final invariant check before
    // the first swap. std::vector::swap is noexcept, so the following loop is
    // an all-or-nothing commit from the language runtime's perspective.
    Value result = distributed_result_value(
        options, parameters.size(), total_elements, published_bytes);
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (parameters[index].tensor->grad.size() != staged[index].size()) {
            throw JitThrow{"autograd_all_reduce_gradients(): gradient changed during reduction",
                           line};
        }
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        parameters[index].tensor->grad.swap(staged[index]);
    }
    return result;
}

} // namespace SuraStd
