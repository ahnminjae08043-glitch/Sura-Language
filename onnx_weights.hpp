#pragma once

// Dependency-free ONNX weights interoperability and a bounded inference subset.
//
// save_onnx_weights emits a valid inference ModelProto containing typed
// initializers, Identity nodes, and typed graph outputs. load_onnx_weights
// accepts raw-data FLOAT/DOUBLE/FLOAT16/BFLOAT16 initializers from arbitrary
// ONNX graphs. run_onnx executes a deliberately bounded CPU operator subset;
// unsupported domains, operators, attributes, and graph shapes fail closed.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "sura_version.hpp"
#include <vector>

namespace SuraStd {

static constexpr uint64_t ONNX_WEIGHTS_MAX_FILE_BYTES =
    4ULL * 1024ULL * 1024ULL * 1024ULL;
static constexpr size_t ONNX_WEIGHTS_MAX_TENSORS = 100000;
static constexpr size_t ONNX_WEIGHTS_MAX_NAME_BYTES = 512;
static constexpr size_t ONNX_EXEC_MAX_NODES = 4096;
static constexpr size_t ONNX_EXEC_MAX_NODE_INPUTS = 16;
static constexpr size_t ONNX_EXEC_MAX_NODE_OUTPUTS = 16;
static constexpr size_t ONNX_EXEC_MAX_ATTRIBUTES = 64;
static constexpr size_t ONNX_EXEC_MAX_GRAPH_NAMES = 32768;

class OnnxProtoWriter {
    std::vector<unsigned char> bytes_;
public:
    void varint(uint64_t value) {
        while (value >= 0x80U) {
            bytes_.push_back((unsigned char)((value & 0x7fU) | 0x80U));
            value >>= 7;
        }
        bytes_.push_back((unsigned char)value);
    }
    void key(unsigned field, unsigned wire) { varint(((uint64_t)field << 3) | wire); }
    void integer(unsigned field, uint64_t value) { key(field, 0); varint(value); }
    void bytes(unsigned field, const unsigned char* data, size_t size) {
        key(field, 2);
        varint((uint64_t)size);
        bytes_.insert(bytes_.end(), data, data + size);
    }
    void string(unsigned field, const std::string& value) {
        bytes(field, reinterpret_cast<const unsigned char*>(value.data()), value.size());
    }
    void message(unsigned field, const std::vector<unsigned char>& value) {
        bytes(field, value.data(), value.size());
    }
    const std::vector<unsigned char>& data() const { return bytes_; }
    std::vector<unsigned char> take() { return std::move(bytes_); }
};

class OnnxProtoReader {
    const unsigned char* current_ = nullptr;
    const unsigned char* end_ = nullptr;
    const char* api_ = nullptr;
    int line_ = 0;
public:
    OnnxProtoReader(const unsigned char* data, size_t size, const char* api, int line)
        : current_(data), end_(data + size), api_(api), line_(line) {}
    bool empty() const { return current_ == end_; }
    uint64_t varint() {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (current_ == end_) fail("truncated protobuf varint");
            unsigned char byte = *current_++;
            if (shift == 63 && (byte & 0xfeU)) fail("protobuf varint overflow");
            value |= (uint64_t)(byte & 0x7fU) << shift;
            if (!(byte & 0x80U)) return value;
        }
        fail("protobuf varint overflow");
        return 0;
    }
    std::pair<unsigned, unsigned> key() {
        uint64_t raw = varint();
        unsigned field = (unsigned)(raw >> 3);
        unsigned wire = (unsigned)(raw & 7U);
        if (field == 0 || wire == 3 || wire == 4 || wire > 5) fail("invalid protobuf field key");
        return {field, wire};
    }
    std::pair<const unsigned char*, size_t> bytes() {
        uint64_t length = varint();
        if (length > (uint64_t)(end_ - current_)) fail("truncated protobuf field");
        const unsigned char* start = current_;
        current_ += (size_t)length;
        return {start, (size_t)length};
    }
    uint32_t fixed32() {
        if ((size_t)(end_ - current_) < 4) fail("truncated fixed32 field");
        uint32_t value = uint32_t(current_[0])
                       | (uint32_t(current_[1]) << 8)
                       | (uint32_t(current_[2]) << 16)
                       | (uint32_t(current_[3]) << 24);
        current_ += 4;
        return value;
    }
    void skip(unsigned wire) {
        if (wire == 0) { (void)varint(); return; }
        if (wire == 1) {
            if ((size_t)(end_ - current_) < 8) fail("truncated fixed64 field");
            current_ += 8; return;
        }
        if (wire == 2) { (void)bytes(); return; }
        if (wire == 5) {
            if ((size_t)(end_ - current_) < 4) fail("truncated fixed32 field");
            current_ += 4; return;
        }
        fail("unsupported protobuf wire type");
    }
    [[noreturn]] void fail(const std::string& message) const {
        throw JitThrow{std::string(api_) + "(): " + message, line_};
    }
};

inline int onnx_tensor_type(TensorDType dtype) {
    switch (dtype) {
        case TensorDType::FLOAT32: return 1;
        case TensorDType::FLOAT16: return 10;
        case TensorDType::FLOAT64: return 11;
        case TensorDType::BFLOAT16: return 16;
    }
    return 0;
}

inline TensorDType onnx_sura_dtype(int type, const char* api, int line) {
    if (type == 1) return TensorDType::FLOAT32;
    if (type == 10) return TensorDType::FLOAT16;
    if (type == 11) return TensorDType::FLOAT64;
    if (type == 16) return TensorDType::BFLOAT16;
    throw JitThrow{std::string(api) + "(): initializer dtype " + std::to_string(type)
                   + " is not supported; expected FLOAT, DOUBLE, FLOAT16, or BFLOAT16", line};
}

inline std::vector<unsigned char> onnx_tensor_shape(const std::vector<size_t>& shape) {
    OnnxProtoWriter shape_writer;
    for (size_t dim : shape) {
        OnnxProtoWriter dimension;
        dimension.integer(1, (uint64_t)dim);
        shape_writer.message(1, dimension.data());
    }
    return shape_writer.take();
}

inline std::vector<unsigned char> onnx_type_proto(TensorDType dtype,
                                                  const std::vector<size_t>& shape) {
    OnnxProtoWriter tensor_type;
    tensor_type.integer(1, (uint64_t)onnx_tensor_type(dtype));
    tensor_type.message(2, onnx_tensor_shape(shape));
    OnnxProtoWriter type;
    type.message(1, tensor_type.data());
    return type.take();
}

inline std::vector<unsigned char> onnx_value_info(const std::string& name,
                                                  TensorDType dtype,
                                                  const std::vector<size_t>& shape) {
    OnnxProtoWriter info;
    info.string(1, name);
    info.message(2, onnx_type_proto(dtype, shape));
    return info.take();
}

inline std::vector<unsigned char> onnx_tensor_proto(const std::string& name,
                                                    const GCTensor* tensor,
                                                    int line) {
    OnnxProtoWriter packed_dims;
    for (size_t dim : tensor->shape) packed_dims.varint((uint64_t)dim);
    size_t element_bytes = tensor_dtype_size(tensor->data.dtype());
    if (tensor->data.size() > std::numeric_limits<size_t>::max() / element_bytes) {
        throw JitThrow{"autograd_save_onnx_weights(): tensor byte size overflow", line};
    }
    std::vector<unsigned char> raw(tensor->data.size() * element_bytes);
    for (size_t i = 0; i < tensor->data.size(); ++i) {
        safetensors_encode_value(raw.data() + i * element_bytes,
                                 tensor->data.dtype(), tensor->data[i],
                                 "autograd_save_onnx_weights", line);
    }
    OnnxProtoWriter output;
    output.message(1, packed_dims.data());
    output.integer(2, (uint64_t)onnx_tensor_type(tensor->data.dtype()));
    output.string(8, name);
    output.bytes(9, raw.data(), raw.size());
    return output.take();
}

inline std::vector<unsigned char> onnx_identity_node(const std::string& input,
                                                     const std::string& output_name) {
    OnnxProtoWriter node;
    node.string(1, input);
    node.string(2, output_name);
    node.string(3, input + "__sura_identity");
    node.string(4, "Identity");
    return node.take();
}

struct OnnxWeightRecord {
    std::string name;
    TensorDType dtype = TensorDType::FLOAT64;
    std::vector<size_t> shape;
    std::vector<unsigned char> raw;
    size_t numel = 0;
};

inline std::vector<std::pair<std::string, GCTensor*>> onnx_collect_state(
    const Value& state, int line) {
    const char* api = "autograd_save_onnx_weights";
    GCDict* dict = need_dict(api, state, 0, line);
    if (dict->elements.empty() || dict->elements.size() > ONNX_WEIGHTS_MAX_TENSORS) {
        throw JitThrow{"autograd_save_onnx_weights(): state_dict must contain 1..100000 tensors", line};
    }
    std::unordered_set<GCTensor*> identities;
    std::vector<std::pair<std::string, GCTensor*>> records;
    records.reserve(dict->elements.size());
    for (const auto& entry : dict->elements) {
        safetensors_validate_name(api, entry.first, line);
        if (!entry.second.is_tensor()) {
            throw JitThrow{"autograd_save_onnx_weights(): state_dict values must be tensors", line};
        }
        GCTensor* tensor = entry.second.as_tensor();
        if (!identities.insert(tensor).second) {
            throw JitThrow{"autograd_save_onnx_weights(): duplicate tensor identity", line};
        }
        ag_cuda_materialize_host(tensor, api, line);
        size_t element_bytes = tensor_dtype_size(tensor->data.dtype());
        if (tensor->shape.size() > AG_MAX_RANK
            || tensor->data.size() != ag_numel(api, tensor->shape, line)
            || element_bytes == 0
            || !tensor->data.host_readable()
            || tensor->data.byte_size() != tensor->data.size() * element_bytes
            || tensor->data.host_byte_size() != tensor->data.byte_size()) {
            throw JitThrow{"autograd_save_onnx_weights(): invalid tensor shape", line};
        }
        records.push_back({entry.first, tensor});
    }
    std::sort(records.begin(), records.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    return records;
}

inline std::vector<unsigned char> onnx_build_model(const Value& state, int line) {
    auto records = onnx_collect_state(state, line);
    OnnxProtoWriter graph;
    for (const auto& record : records) {
        std::string output_name = record.first + "__sura_output";
        graph.message(1, onnx_identity_node(record.first, output_name));
    }
    graph.string(2, "Sura weights");
    for (const auto& record : records) {
        graph.message(5, onnx_tensor_proto(record.first, record.second, line));
    }
    for (const auto& record : records) {
        std::string output_name = record.first + "__sura_output";
        graph.message(12, onnx_value_info(output_name, record.second->data.dtype(),
                                          record.second->shape));
    }

    OnnxProtoWriter opset;
    opset.integer(2, 18);
    OnnxProtoWriter model;
    model.integer(1, 9); // Widely supported ONNX IR version.
    model.string(2, "Sura Language");
    model.string(3, SURA_LANGUAGE_VERSION);
    model.string(4, "org.sura-language");
    model.integer(5, 1);
    model.message(7, graph.data());
    model.message(8, opset.data());
    return model.take();
}

inline std::string onnx_temp_suffix() {
    uint64_t value = (uint64_t)std::chrono::high_resolution_clock::now()
                         .time_since_epoch().count();
    value ^= (uint64_t)(uintptr_t)&value;
    return ".tmp." + std::to_string(value);
}

inline void onnx_atomic_write(const std::filesystem::path& target,
                              const std::vector<unsigned char>& bytes, int line) {
    std::error_code ec;
    auto status = std::filesystem::symlink_status(target, ec);
    if (!ec && status.type() == std::filesystem::file_type::symlink) {
        throw JitThrow{"autograd_save_onnx_weights(): refusing to replace a symlink", line};
    }
    if (!ec && status.type() != std::filesystem::file_type::not_found
        && status.type() != std::filesystem::file_type::regular) {
        throw JitThrow{"autograd_save_onnx_weights(): target must be a regular file", line};
    }
    std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) throw JitThrow{"autograd_save_onnx_weights(): cannot create parent directory", line};
    }
    std::filesystem::path temp = target;
    temp += onnx_temp_suffix();
    struct Guard {
        std::filesystem::path path;
        bool active = true;
        ~Guard() { if (active) { std::error_code ignored; std::filesystem::remove(path, ignored); } }
    } guard{temp};
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) throw JitThrow{"autograd_save_onnx_weights(): cannot open temporary file", line};
        output.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        output.flush();
        if (!output) throw JitThrow{"autograd_save_onnx_weights(): failed writing file", line};
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw JitThrow{"autograd_save_onnx_weights(): atomic commit failed", line};
    }
#else
    std::filesystem::rename(temp, target, ec);
    if (ec) throw JitThrow{"autograd_save_onnx_weights(): atomic commit failed", line};
#endif
    guard.active = false;
}

inline OnnxWeightRecord onnx_parse_tensor(const unsigned char* data, size_t size,
                                          const char* api, int line) {
    OnnxProtoReader reader(data, size, api, line);
    OnnxWeightRecord record;
    int element_type = 0;
    bool has_type = false, has_name = false, has_raw = false;
    while (!reader.empty()) {
        auto [field, wire] = reader.key();
        if (field == 1 && wire == 2) {
            auto packed = reader.bytes();
            OnnxProtoReader dims(packed.first, packed.second, api, line);
            while (!dims.empty()) {
                uint64_t raw = dims.varint();
                if (raw == 0 || raw > ag_max_elements()) dims.fail("invalid initializer dimension");
                record.shape.push_back((size_t)raw);
            }
        } else if (field == 1 && wire == 0) {
            uint64_t raw = reader.varint();
            if (raw == 0 || raw > ag_max_elements()) reader.fail("invalid initializer dimension");
            record.shape.push_back((size_t)raw);
        } else if (field == 2 && wire == 0) {
            element_type = (int)reader.varint();
            has_type = true;
        } else if (field == 8 && wire == 2) {
            auto value = reader.bytes();
            record.name.assign(reinterpret_cast<const char*>(value.first), value.second);
            has_name = true;
        } else if (field == 9 && wire == 2) {
            auto value = reader.bytes();
            record.raw.assign(value.first, value.first + value.second);
            has_raw = true;
        } else {
            reader.skip(wire);
        }
    }
    if (!has_type || !has_name || !has_raw) {
        throw JitThrow{std::string(api) + "(): initializer must contain name, data_type, and raw_data", line};
    }
    safetensors_validate_name(api, record.name, line);
    if (record.shape.size() > AG_MAX_RANK) {
        throw JitThrow{std::string(api) + "(): initializer rank exceeds limit", line};
    }
    record.dtype = onnx_sura_dtype(element_type, api, line);
    record.numel = ag_numel(api, record.shape, line);
    size_t element_bytes = tensor_dtype_size(record.dtype);
    if (record.numel > std::numeric_limits<size_t>::max() / element_bytes
        || record.raw.size() != record.numel * element_bytes) {
        throw JitThrow{std::string(api) + "(): initializer raw_data size mismatch", line};
    }
    return record;
}

inline std::vector<OnnxWeightRecord> onnx_parse_graph(const unsigned char* data,
                                                      size_t size, int line) {
    const char* api = "autograd_load_onnx_weights";
    OnnxProtoReader graph(data, size, api, line);
    std::vector<OnnxWeightRecord> records;
    std::unordered_set<std::string> names;
    while (!graph.empty()) {
        auto [field, wire] = graph.key();
        if (field == 5 && wire == 2) {
            auto tensor = graph.bytes();
            OnnxWeightRecord record = onnx_parse_tensor(
                tensor.first, tensor.second, api, line);
            if (!names.insert(record.name).second) {
                throw JitThrow{"autograd_load_onnx_weights(): duplicate initializer name", line};
            }
            records.push_back(std::move(record));
            if (records.size() > ONNX_WEIGHTS_MAX_TENSORS) {
                throw JitThrow{"autograd_load_onnx_weights(): too many initializers", line};
            }
        } else {
            graph.skip(wire);
        }
    }
    if (records.empty()) {
        throw JitThrow{"autograd_load_onnx_weights(): graph contains no supported initializers", line};
    }
    return records;
}

inline std::vector<OnnxWeightRecord> onnx_parse_model(const std::vector<unsigned char>& file,
                                                      int line) {
    const char* api = "autograd_load_onnx_weights";
    OnnxProtoReader model(file.data(), file.size(), api, line);
    std::vector<OnnxWeightRecord> records;
    bool found_graph = false;
    while (!model.empty()) {
        auto [field, wire] = model.key();
        if (field == 7 && wire == 2) {
            if (found_graph) model.fail("model contains multiple graph fields");
            auto graph = model.bytes();
            records = onnx_parse_graph(graph.first, graph.second, line);
            found_graph = true;
        } else {
            model.skip(wire);
        }
    }
    if (!found_graph) throw JitThrow{"autograd_load_onnx_weights(): model has no graph", line};
    return records;
}

struct OnnxExecAttribute {
    std::string name;
    bool has_float = false;
    float float_value = 0.0f;
    bool has_integer = false;
    int64_t integer_value = 0;
    std::vector<int64_t> integers;
};

struct OnnxExecNode {
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::string name;
    std::string op_type;
    std::string domain;
    std::unordered_map<std::string, OnnxExecAttribute> attributes;
};

struct OnnxExecTensorSpec {
    int element_type = 0;
    bool is_int64 = false;
    TensorDType dtype = TensorDType::FLOAT64;
    std::vector<size_t> shape;
};

struct OnnxExecIntegerInitializer {
    std::string name;
    std::vector<size_t> shape;
    std::vector<int64_t> values;
};

struct OnnxExecValueInfo {
    std::string name;
    bool has_tensor_spec = false;
    OnnxExecTensorSpec tensor_spec;
};

struct OnnxExecGraph {
    uint64_t ir_version = 0;
    uint64_t opset = 0;
    std::vector<OnnxExecNode> nodes;
    std::vector<OnnxWeightRecord> initializers;
    std::unordered_map<std::string, OnnxExecIntegerInitializer> integer_initializers;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::unordered_map<std::string, OnnxExecTensorSpec> declared_values;
};

inline int64_t onnx_exec_signed_integer(uint64_t raw) {
    int64_t value = 0;
    static_assert(sizeof(value) == sizeof(raw), "ONNX integer width mismatch");
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

inline std::string onnx_exec_string(const unsigned char* data, size_t size,
                                    const char* api, const char* label, int line,
                                    bool allow_empty = false) {
    std::string value(reinterpret_cast<const char*>(data), size);
    if ((!allow_empty && value.empty()) || value.size() > ONNX_WEIGHTS_MAX_NAME_BYTES
        || value.find('\0') != std::string::npos || !safetensors_valid_utf8(value)) {
        throw JitThrow{std::string(api) + "(): invalid " + label, line};
    }
    return value;
}

inline std::string onnx_exec_parse_initializer(const unsigned char* data,
                                               size_t size,
                                               OnnxExecGraph& result,
                                               int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader reader(data, size, api, line);
    std::vector<uint64_t> raw_shape;
    const unsigned char* raw_data = nullptr;
    size_t raw_data_size = 0;
    std::string name;
    int element_type = 0;
    bool has_type = false;
    bool has_name = false;
    bool has_raw = false;
    while (!reader.empty()) {
        auto [field, wire] = reader.key();
        if (field == 1 && wire == 2) {
            auto packed = reader.bytes();
            OnnxProtoReader dims(packed.first, packed.second, api, line);
            while (!dims.empty()) raw_shape.push_back(dims.varint());
        } else if (field == 1 && wire == 0) {
            raw_shape.push_back(reader.varint());
        } else if (field == 2 && wire == 0) {
            if (has_type) reader.fail("initializer contains duplicate data_type fields");
            uint64_t raw = reader.varint();
            if (raw > (uint64_t)std::numeric_limits<int>::max()) {
                reader.fail("initializer data_type is out of range");
            }
            element_type = (int)raw;
            has_type = true;
        } else if (field == 8 && wire == 2) {
            if (has_name) reader.fail("initializer contains duplicate name fields");
            auto value = reader.bytes();
            name = onnx_exec_string(value.first, value.second, api,
                                    "initializer name", line);
            has_name = true;
        } else if (field == 9 && wire == 2) {
            if (has_raw) reader.fail("initializer contains duplicate raw_data fields");
            auto value = reader.bytes();
            raw_data = value.first;
            raw_data_size = value.second;
            has_raw = true;
        } else {
            reader.skip(wire);
        }
        if (raw_shape.size() > AG_MAX_RANK) {
            reader.fail("initializer rank exceeds the tensor rank limit");
        }
    }
    if (!has_type || !has_name || !has_raw) {
        throw JitThrow{
            "autograd_run_onnx(): initializer must contain name, data_type, and raw_data",
            line};
    }
    safetensors_validate_name(api, name, line);

    if (element_type == 7) { // TensorProto.DataType.INT64
        if (raw_shape.size() != 1 || raw_shape[0] > AG_MAX_RANK) {
            throw JitThrow{
                "autograd_run_onnx(): INT64 initializers are limited to rank-1 shape tensors of at most "
                    + std::to_string(AG_MAX_RANK) + " values",
                line};
        }
        const size_t count = (size_t)raw_shape[0];
        if (count > std::numeric_limits<size_t>::max() / sizeof(int64_t)
            || raw_data_size != count * sizeof(int64_t)) {
            throw JitThrow{
                "autograd_run_onnx(): INT64 shape initializer raw_data size mismatch",
                line};
        }
        OnnxExecIntegerInitializer record;
        record.name = name;
        record.shape = {count};
        record.values.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            uint64_t bits = 0;
            for (size_t byte = 0; byte < sizeof(int64_t); ++byte) {
                bits |= (uint64_t)raw_data[index * sizeof(int64_t) + byte]
                        << (byte * 8);
            }
            record.values.push_back(onnx_exec_signed_integer(bits));
        }
        result.integer_initializers.emplace(name, std::move(record));
        return name;
    }

    OnnxWeightRecord record;
    record.name = name;
    record.dtype = onnx_sura_dtype(element_type, api, line);
    for (uint64_t raw_dimension : raw_shape) {
        if (raw_dimension == 0 || raw_dimension > ag_max_elements()) {
            throw JitThrow{"autograd_run_onnx(): invalid initializer dimension", line};
        }
        record.shape.push_back((size_t)raw_dimension);
    }
    record.numel = ag_numel(api, record.shape, line);
    const size_t element_bytes = tensor_dtype_size(record.dtype);
    if (record.numel > std::numeric_limits<size_t>::max() / element_bytes
        || raw_data_size != record.numel * element_bytes) {
        throw JitThrow{"autograd_run_onnx(): initializer raw_data size mismatch", line};
    }
    record.raw.assign(raw_data, raw_data + raw_data_size);
    result.initializers.push_back(std::move(record));
    return name;
}

inline OnnxExecAttribute onnx_exec_parse_attribute(const unsigned char* data,
                                                    size_t size, int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader reader(data, size, api, line);
    OnnxExecAttribute attribute;
    bool has_name = false;
    while (!reader.empty()) {
        auto [field, wire] = reader.key();
        if (field == 1 && wire == 2) {
            if (has_name) reader.fail("attribute contains duplicate name fields");
            auto value = reader.bytes();
            attribute.name = onnx_exec_string(value.first, value.second, api,
                                              "attribute name", line);
            has_name = true;
        } else if (field == 2 && wire == 5) {
            if (attribute.has_float) reader.fail("attribute contains duplicate float values");
            uint32_t bits = reader.fixed32();
            std::memcpy(&attribute.float_value, &bits, sizeof(bits));
            if (!std::isfinite(attribute.float_value)) {
                reader.fail("attribute float must be finite");
            }
            attribute.has_float = true;
        } else if (field == 3 && wire == 0) {
            if (attribute.has_integer) reader.fail("attribute contains duplicate integer values");
            attribute.integer_value = onnx_exec_signed_integer(reader.varint());
            attribute.has_integer = true;
        } else if (field == 7 && wire == 0) {
            attribute.integers.push_back(onnx_exec_signed_integer(reader.varint()));
        } else if (field == 7 && wire == 2) {
            auto packed = reader.bytes();
            OnnxProtoReader values(packed.first, packed.second, api, line);
            while (!values.empty()) {
                attribute.integers.push_back(onnx_exec_signed_integer(values.varint()));
            }
        } else {
            reader.skip(wire);
        }
        if (attribute.integers.size() > AG_MAX_RANK) {
            reader.fail("attribute integer list exceeds the tensor rank limit");
        }
    }
    if (!has_name) {
        throw JitThrow{"autograd_run_onnx(): attribute has no name", line};
    }
    return attribute;
}

inline OnnxExecNode onnx_exec_parse_node(const unsigned char* data, size_t size,
                                         int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader reader(data, size, api, line);
    OnnxExecNode node;
    bool has_op = false;
    while (!reader.empty()) {
        auto [field, wire] = reader.key();
        if ((field == 1 || field == 2) && wire == 2) {
            auto value = reader.bytes();
            std::string name = onnx_exec_string(value.first, value.second, api,
                field == 1 ? "node input name" : "node output name", line,
                field == 1);
            auto& names = field == 1 ? node.inputs : node.outputs;
            names.push_back(std::move(name));
            const size_t limit = field == 1 ? ONNX_EXEC_MAX_NODE_INPUTS
                                            : ONNX_EXEC_MAX_NODE_OUTPUTS;
            if (names.size() > limit) reader.fail("node arity exceeds the safety limit");
        } else if (field == 3 && wire == 2) {
            auto value = reader.bytes();
            node.name = onnx_exec_string(value.first, value.second, api,
                                         "node name", line, true);
        } else if (field == 4 && wire == 2) {
            if (has_op) reader.fail("node contains duplicate op_type fields");
            auto value = reader.bytes();
            node.op_type = onnx_exec_string(value.first, value.second, api,
                                            "operator type", line);
            has_op = true;
        } else if (field == 5 && wire == 2) {
            auto value = reader.bytes();
            OnnxExecAttribute attribute = onnx_exec_parse_attribute(
                value.first, value.second, line);
            if (!node.attributes.emplace(attribute.name, std::move(attribute)).second) {
                reader.fail("node contains duplicate attribute names");
            }
            if (node.attributes.size() > ONNX_EXEC_MAX_ATTRIBUTES) {
                reader.fail("node attribute count exceeds the safety limit");
            }
        } else if (field == 7 && wire == 2) {
            auto value = reader.bytes();
            node.domain = onnx_exec_string(value.first, value.second, api,
                                           "operator domain", line, true);
        } else {
            reader.skip(wire);
        }
    }
    if (!has_op || node.outputs.empty()) {
        throw JitThrow{"autograd_run_onnx(): node requires op_type and output", line};
    }
    if (!node.domain.empty() && node.domain != "ai.onnx") {
        throw JitThrow{"autograd_run_onnx(): custom operator domain '" + node.domain
                       + "' is not supported", line};
    }
    return node;
}

inline size_t onnx_exec_parse_dimension(const unsigned char* data,
                                        size_t size, int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader reader(data, size, api, line);
    uint64_t dimension = 0;
    bool has_dimension = false;
    bool dynamic_dimension = false;
    while (!reader.empty()) {
        auto [field, wire] = reader.key();
        if (field == 1 && wire == 0) {
            if (has_dimension) reader.fail("tensor dimension contains duplicate values");
            dimension = reader.varint();
            has_dimension = true;
        } else if (field == 2 && wire == 2) {
            (void)reader.bytes();
            dynamic_dimension = true;
        } else {
            reader.skip(wire);
        }
    }
    if (dynamic_dimension || !has_dimension || dimension > ag_max_elements()) {
        throw JitThrow{"autograd_run_onnx(): dynamic, missing, or invalid tensor dimensions are not supported", line};
    }
    return (size_t)dimension;
}

inline std::vector<size_t> onnx_exec_parse_shape(const unsigned char* data,
                                                 size_t size, int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader reader(data, size, api, line);
    std::vector<size_t> shape;
    while (!reader.empty()) {
        auto [field, wire] = reader.key();
        if (field == 1 && wire == 2) {
            auto dimension = reader.bytes();
            shape.push_back(onnx_exec_parse_dimension(
                dimension.first, dimension.second, line));
            if (shape.size() > AG_MAX_RANK) {
                reader.fail("declared tensor rank exceeds the tensor rank limit");
            }
        } else {
            reader.skip(wire);
        }
    }
    return shape;
}

inline OnnxExecTensorSpec onnx_exec_parse_tensor_type(
        const unsigned char* data, size_t size, int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader reader(data, size, api, line);
    int element_type = 0;
    bool has_type = false;
    bool has_shape = false;
    OnnxExecTensorSpec spec;
    while (!reader.empty()) {
        auto [field, wire] = reader.key();
        if (field == 1 && wire == 0) {
            if (has_type) reader.fail("tensor type contains duplicate element types");
            uint64_t raw = reader.varint();
            if (raw > (uint64_t)std::numeric_limits<int>::max()) {
                reader.fail("tensor element type is out of range");
            }
            element_type = (int)raw;
            has_type = true;
        } else if (field == 2 && wire == 2) {
            if (has_shape) reader.fail("tensor type contains duplicate shapes");
            auto shape = reader.bytes();
            spec.shape = onnx_exec_parse_shape(shape.first, shape.second, line);
            has_shape = true;
        } else {
            reader.skip(wire);
        }
    }
    if (!has_type || !has_shape) {
        throw JitThrow{"autograd_run_onnx(): declared tensor type requires element type and static shape", line};
    }
    spec.element_type = element_type;
    if (element_type == 7) {
        spec.is_int64 = true;
        if (spec.shape.size() != 1 || spec.shape[0] > AG_MAX_RANK) {
            throw JitThrow{
                "autograd_run_onnx(): declared INT64 values are limited to rank-1 shape tensors",
                line};
        }
    } else {
        spec.dtype = onnx_sura_dtype(element_type, api, line);
        for (size_t dimension : spec.shape) {
            if (dimension == 0) {
                throw JitThrow{
                    "autograd_run_onnx(): zero-sized floating Tensor dimensions are not supported",
                    line};
            }
        }
        (void)ag_numel(api, spec.shape, line);
    }
    return spec;
}

inline OnnxExecTensorSpec onnx_exec_parse_type(const unsigned char* data,
                                               size_t size, int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader reader(data, size, api, line);
    bool found_tensor = false;
    OnnxExecTensorSpec spec;
    while (!reader.empty()) {
        auto [field, wire] = reader.key();
        if (field == 1 && wire == 2) {
            if (found_tensor) reader.fail("value type contains duplicate tensor types");
            auto tensor_type = reader.bytes();
            spec = onnx_exec_parse_tensor_type(
                tensor_type.first, tensor_type.second, line);
            found_tensor = true;
        } else {
            reader.skip(wire);
        }
    }
    if (!found_tensor) {
        throw JitThrow{"autograd_run_onnx(): only Tensor value types are supported", line};
    }
    return spec;
}

inline OnnxExecValueInfo onnx_exec_parse_value_info(const unsigned char* data,
                                                     size_t size, int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader reader(data, size, api, line);
    OnnxExecValueInfo info;
    bool found_name = false;
    while (!reader.empty()) {
        auto [field, wire] = reader.key();
        if (field == 1 && wire == 2) {
            if (found_name) reader.fail("value info contains duplicate name fields");
            auto value = reader.bytes();
            info.name = onnx_exec_string(value.first, value.second, api,
                                         "graph value name", line);
            found_name = true;
        } else if (field == 2 && wire == 2) {
            if (info.has_tensor_spec) reader.fail("value info contains duplicate type fields");
            auto type = reader.bytes();
            info.tensor_spec = onnx_exec_parse_type(type.first, type.second, line);
            info.has_tensor_spec = true;
        } else {
            reader.skip(wire);
        }
    }
    if (!found_name) throw JitThrow{"autograd_run_onnx(): graph value has no name", line};
    return info;
}

inline void onnx_exec_record_value_info(OnnxExecGraph& result,
                                        const OnnxExecValueInfo& info, int line) {
    if (!info.has_tensor_spec) return;
    auto found = result.declared_values.find(info.name);
    if (found == result.declared_values.end()) {
        result.declared_values.emplace(info.name, info.tensor_spec);
        return;
    }
    if (found->second.element_type != info.tensor_spec.element_type
        || found->second.is_int64 != info.tensor_spec.is_int64
        || found->second.dtype != info.tensor_spec.dtype
        || found->second.shape != info.tensor_spec.shape) {
        throw JitThrow{"autograd_run_onnx(): conflicting declarations for graph value '"
                       + info.name + "'", line};
    }
}

inline void onnx_exec_parse_graph(const unsigned char* data, size_t size,
                                  OnnxExecGraph& result, int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader graph(data, size, api, line);
    std::unordered_set<std::string> initializer_names;
    size_t graph_name_count = 0;
    while (!graph.empty()) {
        auto [field, wire] = graph.key();
        if (field == 1 && wire == 2) {
            auto node = graph.bytes();
            result.nodes.push_back(onnx_exec_parse_node(node.first, node.second, line));
            if (result.nodes.size() > ONNX_EXEC_MAX_NODES) {
                graph.fail("node count exceeds the safety limit");
            }
        } else if (field == 5 && wire == 2) {
            auto tensor = graph.bytes();
            std::string name = onnx_exec_parse_initializer(
                tensor.first, tensor.second, result, line);
            if (!initializer_names.insert(name).second) {
                graph.fail("duplicate initializer name");
            }
            if (result.initializers.size() + result.integer_initializers.size()
                > ONNX_WEIGHTS_MAX_TENSORS) {
                graph.fail("initializer count exceeds the safety limit");
            }
        } else if ((field == 11 || field == 12 || field == 13) && wire == 2) {
            auto info = graph.bytes();
            OnnxExecValueInfo parsed = onnx_exec_parse_value_info(
                info.first, info.second, line);
            onnx_exec_record_value_info(result, parsed, line);
            if (field == 11) result.inputs.push_back(parsed.name);
            if (field == 12) result.outputs.push_back(parsed.name);
            if (++graph_name_count > ONNX_EXEC_MAX_GRAPH_NAMES) {
                graph.fail("graph value declaration count exceeds the safety limit");
            }
        } else {
            graph.skip(wire);
        }
    }
    if (result.outputs.empty()) {
        throw JitThrow{"autograd_run_onnx(): graph has no outputs", line};
    }
}

inline std::pair<std::string, uint64_t> onnx_exec_parse_opset(
        const unsigned char* data, size_t size, int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader reader(data, size, api, line);
    std::string domain;
    uint64_t version = 0;
    bool has_version = false;
    while (!reader.empty()) {
        auto [field, wire] = reader.key();
        if (field == 1 && wire == 2) {
            auto value = reader.bytes();
            domain = onnx_exec_string(value.first, value.second, api,
                                      "opset domain", line, true);
        } else if (field == 2 && wire == 0) {
            version = reader.varint();
            has_version = true;
        } else {
            reader.skip(wire);
        }
    }
    if (!has_version) throw JitThrow{"autograd_run_onnx(): opset has no version", line};
    return {domain, version};
}

inline OnnxExecGraph onnx_exec_parse_model(const std::vector<unsigned char>& file,
                                           int line) {
    const char* api = "autograd_run_onnx";
    OnnxProtoReader model(file.data(), file.size(), api, line);
    OnnxExecGraph result;
    bool found_graph = false;
    bool found_default_opset = false;
    while (!model.empty()) {
        auto [field, wire] = model.key();
        if (field == 1 && wire == 0) {
            result.ir_version = model.varint();
        } else if (field == 7 && wire == 2) {
            if (found_graph) model.fail("model contains multiple graph fields");
            auto graph = model.bytes();
            onnx_exec_parse_graph(graph.first, graph.second, result, line);
            found_graph = true;
        } else if (field == 8 && wire == 2) {
            auto opset = model.bytes();
            auto parsed = onnx_exec_parse_opset(opset.first, opset.second, line);
            if (parsed.first.empty() || parsed.first == "ai.onnx") {
                if (found_default_opset) model.fail("model contains duplicate default opsets");
                result.opset = parsed.second;
                found_default_opset = true;
            } else {
                model.fail("custom opset domains are not supported");
            }
        } else {
            model.skip(wire);
        }
    }
    if (!found_graph || !found_default_opset) {
        throw JitThrow{"autograd_run_onnx(): model requires one graph and one default opset", line};
    }
    if (result.ir_version < 3 || result.ir_version > 10) {
        throw JitThrow{"autograd_run_onnx(): supported ONNX IR versions are 3 through 10", line};
    }
    if (result.opset < 7 || result.opset > 18) {
        throw JitThrow{"autograd_run_onnx(): supported default opsets are 7 through 18", line};
    }
    return result;
}

inline std::vector<unsigned char> onnx_exec_read_file(const std::string& path,
                                                      int line) {
    const char* api = "autograd_run_onnx";
    safetensors_validate_path(api, path, line);
    std::filesystem::path source = std::filesystem::u8path(path);
    std::error_code ec;
    auto status = std::filesystem::symlink_status(source, ec);
    if (ec || status.type() != std::filesystem::file_type::regular) {
        throw JitThrow{"autograd_run_onnx(): path must be a regular non-symlink file", line};
    }
    uintmax_t file_size = std::filesystem::file_size(source, ec);
    if (ec || file_size == 0 || file_size > ONNX_WEIGHTS_MAX_FILE_BYTES
        || file_size > (uintmax_t)ag_max_external_bytes()) {
        throw JitThrow{"autograd_run_onnx(): invalid or oversized ONNX file", line};
    }
    AgTemporaryBytes file_memory((size_t)file_size, api, line);
    std::vector<unsigned char> file((size_t)file_size);
    std::ifstream input(source, std::ios::binary);
    if (!input) throw JitThrow{"autograd_run_onnx(): cannot open file", line};
    input.read(reinterpret_cast<char*>(file.data()), (std::streamsize)file.size());
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        throw JitThrow{"autograd_run_onnx(): failed reading complete file", line};
    }
    return file;
}

inline Value onnx_exec_materialize(const OnnxWeightRecord& record,
                                   bool requires_grad, int line) {
    const char* api = "autograd_run_onnx";
    size_t element_bytes = tensor_dtype_size(record.dtype);
    std::vector<double> values(record.numel);
    for (size_t i = 0; i < record.numel; ++i) {
        values[i] = safetensors_decode_value(
            record.raw.data() + i * element_bytes, record.dtype, api, line);
    }
    return ag_make_tensor(api, std::move(values), record.shape, requires_grad,
                          TensorOp::LEAF, {}, line, record.dtype);
}

inline void onnx_exec_validate_declared_value(const OnnxExecGraph& graph,
                                              const std::string& name,
                                              const Value& value, int line) {
    auto declared = graph.declared_values.find(name);
    if (declared == graph.declared_values.end()) return;
    GCTensor* tensor = ag_need_tensor("autograd_run_onnx", value, 0, line);
    if (declared->second.is_int64
        || tensor->data.dtype() != declared->second.dtype
        || tensor->shape != declared->second.shape) {
        throw JitThrow{"autograd_run_onnx(): tensor '" + name
                       + "' does not match its declared dtype and static shape", line};
    }
}

inline void onnx_exec_validate_declared_integer(
        const OnnxExecGraph& graph,
        const OnnxExecIntegerInitializer& initializer, int line) {
    auto declared = graph.declared_values.find(initializer.name);
    if (declared == graph.declared_values.end()) return;
    if (!declared->second.is_int64
        || declared->second.shape != initializer.shape) {
        throw JitThrow{
            "autograd_run_onnx(): INT64 shape initializer '" + initializer.name
                + "' does not match its declared type and static shape",
            line};
    }
}

inline void onnx_exec_require_signature(const OnnxExecNode& node,
                                        size_t min_inputs, size_t max_inputs,
                                        int line) {
    if (node.inputs.size() < min_inputs || node.inputs.size() > max_inputs
        || node.outputs.size() != 1) {
        throw JitThrow{"autograd_run_onnx(): operator " + node.op_type
                       + " has unsupported input/output arity", line};
    }
}

inline void onnx_exec_validate_attributes(
        const OnnxExecNode& node,
        std::initializer_list<const char*> allowed, int line) {
    for (const auto& entry : node.attributes) {
        bool known = false;
        for (const char* name : allowed) {
            if (entry.first == name) { known = true; break; }
        }
        if (!known) {
            throw JitThrow{"autograd_run_onnx(): operator " + node.op_type
                           + " has unsupported attribute '" + entry.first + "'", line};
        }
    }
}

inline int64_t onnx_exec_integer_attribute(const OnnxExecNode& node,
                                           const char* name, int64_t fallback,
                                           int line) {
    auto found = node.attributes.find(name);
    if (found == node.attributes.end()) return fallback;
    if (!found->second.has_integer || found->second.has_float
        || !found->second.integers.empty()) {
        throw JitThrow{"autograd_run_onnx(): attribute '" + std::string(name)
                       + "' on " + node.op_type + " must be an integer", line};
    }
    return found->second.integer_value;
}

inline double onnx_exec_float_attribute(const OnnxExecNode& node,
                                        const char* name, double fallback,
                                        int line) {
    auto found = node.attributes.find(name);
    if (found == node.attributes.end()) return fallback;
    if (!found->second.has_float || found->second.has_integer
        || !found->second.integers.empty()) {
        throw JitThrow{"autograd_run_onnx(): attribute '" + std::string(name)
                       + "' on " + node.op_type + " must be a float", line};
    }
    return (double)found->second.float_value;
}

inline const Value& onnx_exec_need_value(const GCDict* environment,
                                         const OnnxExecNode& node,
                                         size_t index, int line) {
    if (index >= node.inputs.size() || node.inputs[index].empty()) {
        throw JitThrow{"autograd_run_onnx(): operator " + node.op_type
                       + " is missing required input " + std::to_string(index), line};
    }
    auto found = environment->elements.find(node.inputs[index]);
    if (found == environment->elements.end()) {
        throw JitThrow{"autograd_run_onnx(): node input '" + node.inputs[index]
                       + "' is unavailable; graph must be topologically ordered", line};
    }
    return found->second;
}

inline Value onnx_exec_call_unary(const OnnxExecNode& node,
                                  const GCDict* environment,
                                  Value (*function)(const Value*, int, int),
                                  int line) {
    onnx_exec_require_signature(node, 1, 1, line);
    onnx_exec_validate_attributes(node, {}, line);
    Value arguments[1] = {onnx_exec_need_value(environment, node, 0, line)};
    return function(arguments, 1, line);
}

inline Value onnx_exec_call_binary(const OnnxExecNode& node,
                                   const GCDict* environment,
                                   Value (*function)(const Value*, int, int),
                                   int line) {
    onnx_exec_require_signature(node, 2, 2, line);
    onnx_exec_validate_attributes(node, {}, line);
    Value arguments[2] = {
        onnx_exec_need_value(environment, node, 0, line),
        onnx_exec_need_value(environment, node, 1, line)
    };
    return function(arguments, 2, line);
}

inline Value onnx_exec_gemm(const OnnxExecNode& node,
                            const GCDict* environment, int line) {
    onnx_exec_require_signature(node, 2, 3, line);
    onnx_exec_validate_attributes(node, {"alpha", "beta", "transA", "transB"}, line);
    const double alpha = onnx_exec_float_attribute(node, "alpha", 1.0, line);
    const double beta = onnx_exec_float_attribute(node, "beta", 1.0, line);
    const int64_t trans_a = onnx_exec_integer_attribute(node, "transA", 0, line);
    const int64_t trans_b = onnx_exec_integer_attribute(node, "transB", 0, line);
    if ((trans_a != 0 && trans_a != 1) || (trans_b != 0 && trans_b != 1)) {
        throw JitThrow{"autograd_run_onnx(): Gemm transA/transB must be 0 or 1", line};
    }
    const Value& raw_a = onnx_exec_need_value(environment, node, 0, line);
    const Value& raw_b = onnx_exec_need_value(environment, node, 1, line);
    if (!raw_a.is_tensor() || !raw_b.is_tensor()
        || raw_a.as_tensor()->shape.size() != 2
        || raw_b.as_tensor()->shape.size() != 2) {
        throw JitThrow{"autograd_run_onnx(): Gemm requires rank-2 A and B tensors", line};
    }

    Value scratch = Value::make_array();
    GCNativeRoot scratch_root(scratch.as_obj());
    auto keep = [&](Value value) -> Value {
        scratch.as_arr()->elements.push_back(value);
        return value;
    };
    Value a = raw_a;
    Value b = raw_b;
    if (trans_a) {
        Value args[1] = {a};
        a = keep(b_autograd_transpose(args, 1, line));
    }
    if (trans_b) {
        Value args[1] = {b};
        b = keep(b_autograd_transpose(args, 1, line));
    }
    Value matmul_args[2] = {a, b};
    Value result = keep(b_autograd_matmul(matmul_args, 2, line));
    if (alpha != 1.0) {
        Value scale_args[2] = {result, Value(alpha)};
        result = keep(b_autograd_mul(scale_args, 2, line));
    }
    if (node.inputs.size() == 3 && !node.inputs[2].empty()) {
        Value c = onnx_exec_need_value(environment, node, 2, line);
        if (beta != 1.0) {
            Value scale_args[2] = {c, Value(beta)};
            c = keep(b_autograd_mul(scale_args, 2, line));
        }
        Value add_args[2] = {result, c};
        result = keep(b_autograd_add(add_args, 2, line));
    }
    return result;
}

inline Value onnx_exec_transpose(const OnnxExecNode& node,
                                 const GCDict* environment, int line) {
    onnx_exec_require_signature(node, 1, 1, line);
    onnx_exec_validate_attributes(node, {"perm"}, line);
    const Value& input = onnx_exec_need_value(environment, node, 0, line);
    GCTensor* tensor = ag_need_tensor("autograd_run_onnx", input, 0, line);
    const size_t rank = tensor->shape.size();

    std::vector<size_t> permutation;
    permutation.reserve(rank);
    auto found = node.attributes.find("perm");
    if (found == node.attributes.end()) {
        for (size_t axis = rank; axis > 0; --axis) permutation.push_back(axis - 1);
    } else {
        const OnnxExecAttribute& attribute = found->second;
        if (attribute.has_float || attribute.has_integer
            || attribute.integers.size() != rank) {
            throw JitThrow{
                "autograd_run_onnx(): Transpose perm must contain one integer per input axis",
                line};
        }
        std::vector<bool> seen(rank, false);
        for (int64_t raw_axis : attribute.integers) {
            if (raw_axis < 0 || (uint64_t)raw_axis >= (uint64_t)rank
                || seen[(size_t)raw_axis]) {
                throw JitThrow{
                    "autograd_run_onnx(): Transpose perm must be a unique axis permutation",
                    line};
            }
            seen[(size_t)raw_axis] = true;
            permutation.push_back((size_t)raw_axis);
        }
    }

    // ONNX permits scalar and rank-1 transposes. Their only valid permutation
    // is an identity, so no autograd node or copy is required.
    if (rank < 2) return input;

    Value scratch = Value::make_array();
    GCNativeRoot scratch_root(scratch.as_obj());
    Value result = input;
    std::vector<size_t> current(rank);
    for (size_t axis = 0; axis < rank; ++axis) current[axis] = axis;
    for (size_t output_axis = 0; output_axis < rank; ++output_axis) {
        size_t source_axis = output_axis;
        while (source_axis < rank
               && current[source_axis] != permutation[output_axis]) {
            ++source_axis;
        }
        if (source_axis == rank) {
            throw JitThrow{"autograd_run_onnx(): invalid Transpose permutation", line};
        }
        if (source_axis == output_axis) continue;
        Value arguments[3] = {
            result,
            Value((double)output_axis),
            Value((double)source_axis)
        };
        result = b_autograd_transpose(arguments, 3, line);
        scratch.as_arr()->elements.push_back(result);
        std::swap(current[output_axis], current[source_axis]);
    }
    return result;
}

inline Value onnx_exec_flatten(const OnnxExecNode& node,
                               const GCDict* environment, int line) {
    onnx_exec_require_signature(node, 1, 1, line);
    onnx_exec_validate_attributes(node, {"axis"}, line);
    const Value& input = onnx_exec_need_value(environment, node, 0, line);
    GCTensor* tensor = ag_need_tensor("autograd_run_onnx", input, 0, line);
    const int64_t rank = (int64_t)tensor->shape.size();
    int64_t axis = onnx_exec_integer_attribute(node, "axis", 1, line);
    if (axis < 0) axis += rank;
    if (axis < 0 || axis > rank) {
        throw JitThrow{"autograd_run_onnx(): Flatten axis is out of range", line};
    }

    size_t outer = 1;
    for (int64_t index = 0; index < axis; ++index) {
        outer *= tensor->shape[(size_t)index];
    }
    const size_t inner = tensor->data.size() / outer;
    Value shape = Value::make_array();
    GCNativeRoot shape_root(shape.as_obj());
    shape.as_arr()->elements.push_back(Value((double)outer));
    shape.as_arr()->elements.push_back(Value((double)inner));
    Value arguments[2] = {input, shape};
    return b_autograd_reshape(arguments, 2, line);
}

inline Value onnx_exec_reshape(const OnnxExecGraph& graph,
                               const OnnxExecNode& node,
                               const GCDict* environment, int line) {
    onnx_exec_require_signature(node, 2, 2, line);
    onnx_exec_validate_attributes(node, {"allowzero"}, line);
    auto allowzero_attribute = node.attributes.find("allowzero");
    if (graph.opset < 14 && allowzero_attribute != node.attributes.end()) {
        throw JitThrow{
            "autograd_run_onnx(): Reshape allowzero requires opset 14 or newer",
            line};
    }
    const int64_t allowzero = onnx_exec_integer_attribute(
        node, "allowzero", 0, line);
    if (allowzero != 0 && allowzero != 1) {
        throw JitThrow{
            "autograd_run_onnx(): Reshape allowzero must be 0 or 1", line};
    }

    const Value& input = onnx_exec_need_value(environment, node, 0, line);
    GCTensor* tensor = ag_need_tensor("autograd_run_onnx", input, 0, line);
    if (node.inputs[1].empty()) {
        throw JitThrow{
            "autograd_run_onnx(): Reshape requires an INT64 shape initializer",
            line};
    }
    auto shape_found = graph.integer_initializers.find(node.inputs[1]);
    if (shape_found == graph.integer_initializers.end()) {
        throw JitThrow{
            "autograd_run_onnx(): Reshape shape input must be a bounded raw-data INT64 initializer",
            line};
    }

    const std::vector<int64_t>& requested = shape_found->second.values;
    Value shape = Value::make_array();
    GCNativeRoot shape_root(shape.as_obj());
    bool inferred = false;
    bool zero_literal = false;
    for (size_t index = 0; index < requested.size(); ++index) {
        int64_t dimension = requested[index];
        if (dimension == 0) {
            zero_literal = true;
            if (allowzero == 1) {
                throw JitThrow{
                    "autograd_run_onnx(): zero-sized Reshape outputs are not supported",
                    line};
            }
            if (index >= tensor->shape.size()) {
                throw JitThrow{
                    "autograd_run_onnx(): Reshape zero dimension cannot copy a missing input axis",
                    line};
            }
            dimension = (int64_t)tensor->shape[index];
        } else if (dimension == -1) {
            if (inferred) {
                throw JitThrow{
                    "autograd_run_onnx(): Reshape shape may contain at most one -1",
                    line};
            }
            inferred = true;
        } else if (dimension < 1
                   || (uint64_t)dimension > (uint64_t)ag_max_elements()) {
            throw JitThrow{
                "autograd_run_onnx(): Reshape dimensions must be positive, 0, or one -1",
                line};
        }
        shape.as_arr()->elements.push_back(Value((double)dimension));
    }
    if (allowzero == 1 && zero_literal && inferred) {
        throw JitThrow{
            "autograd_run_onnx(): Reshape cannot combine allowzero=1 zero dimensions with -1",
            line};
    }
    Value arguments[2] = {input, shape};
    return b_autograd_reshape(arguments, 2, line);
}

inline Value onnx_exec_node(const OnnxExecGraph& graph,
                            const OnnxExecNode& node,
                            const GCDict* environment, int line) {
    try {
        if (node.op_type == "Identity") {
            onnx_exec_require_signature(node, 1, 1, line);
            onnx_exec_validate_attributes(node, {}, line);
            return onnx_exec_need_value(environment, node, 0, line);
        }
        if (node.op_type == "Add") return onnx_exec_call_binary(node, environment, b_autograd_add, line);
        if (node.op_type == "Sub") return onnx_exec_call_binary(node, environment, b_autograd_sub, line);
        if (node.op_type == "Mul") return onnx_exec_call_binary(node, environment, b_autograd_mul, line);
        if (node.op_type == "Div") return onnx_exec_call_binary(node, environment, b_autograd_div, line);
        if (node.op_type == "MatMul") return onnx_exec_call_binary(node, environment, b_autograd_matmul, line);
        if (node.op_type == "Neg") return onnx_exec_call_unary(node, environment, b_autograd_neg, line);
        if (node.op_type == "Relu") return onnx_exec_call_unary(node, environment, b_autograd_relu, line);
        if (node.op_type == "Tanh") return onnx_exec_call_unary(node, environment, b_autograd_tanh, line);
        if (node.op_type == "Sigmoid") return onnx_exec_call_unary(node, environment, b_autograd_sigmoid, line);
        if (node.op_type == "Gemm") return onnx_exec_gemm(node, environment, line);
        if (node.op_type == "Transpose") return onnx_exec_transpose(node, environment, line);
        if (node.op_type == "Flatten") return onnx_exec_flatten(node, environment, line);
        if (node.op_type == "Reshape") return onnx_exec_reshape(graph, node, environment, line);
        if (node.op_type == "Softmax") {
            onnx_exec_require_signature(node, 1, 1, line);
            onnx_exec_validate_attributes(node, {"axis"}, line);
            const Value& input = onnx_exec_need_value(environment, node, 0, line);
            GCTensor* tensor = ag_need_tensor("autograd_run_onnx", input, 0, line);
            int64_t axis = onnx_exec_integer_attribute(
                node, "axis", graph.opset >= 13 ? -1 : 1, line);
            int64_t rank = (int64_t)tensor->shape.size();
            if (axis < 0) axis += rank;
            if (rank == 0 || axis != rank - 1) {
                throw JitThrow{"autograd_run_onnx(): Softmax supports the last axis only", line};
            }
            Value arguments[1] = {input};
            return b_autograd_softmax(arguments, 1, line);
        }
        throw JitThrow{"autograd_run_onnx(): unsupported operator '" + node.op_type + "'", line};
    } catch (const JitThrow& error) {
        if (error.message.rfind("autograd_run_onnx():", 0) == 0) throw;
        std::string label = node.name.empty() ? node.op_type : node.name;
        throw JitThrow{"autograd_run_onnx(): node '" + label
                       + "' failed: " + error.message, line};
    }
}

inline Value b_autograd_run_onnx(const Value* args, int nargs, int line) {
    need_args("autograd_run_onnx", nargs, 2, 3, line);
    std::string path = need_str("autograd_run_onnx", args[0], 0, line);
    GCDict* inputs = need_dict("autograd_run_onnx", args[1], 1, line);
    GCDict* options = nn_options("autograd_run_onnx", args, nargs, 2, line);
    ag_validate_options("autograd_run_onnx", options,
                        {"trainable_initializers"}, line);
    bool trainable_initializers = nn_option_bool(
        "autograd_run_onnx", options, "trainable_initializers", false, line);

    std::vector<unsigned char> file = onnx_exec_read_file(path, line);
    OnnxExecGraph graph = onnx_exec_parse_model(file, line);
    Value environment_value = Value::make_dict();
    GCNativeRoot environment_root(environment_value.as_obj());
    GCDict* environment = environment_value.as_dict();
    std::unordered_set<std::string> initializer_names;
    std::unordered_set<std::string> occupied_names;
    for (const auto& record : graph.initializers) {
        initializer_names.insert(record.name);
        occupied_names.insert(record.name);
        Value tensor = onnx_exec_materialize(record, trainable_initializers, line);
        onnx_exec_validate_declared_value(graph, record.name, tensor, line);
        environment->elements.emplace(record.name, tensor);
    }
    for (const auto& entry : graph.integer_initializers) {
        initializer_names.insert(entry.first);
        occupied_names.insert(entry.first);
        onnx_exec_validate_declared_integer(graph, entry.second, line);
    }

    std::unordered_set<std::string> used_integer_initializers;
    for (const OnnxExecNode& node : graph.nodes) {
        for (size_t index = 0; index < node.inputs.size(); ++index) {
            const std::string& input_name = node.inputs[index];
            if (input_name.empty()
                || graph.integer_initializers.find(input_name)
                       == graph.integer_initializers.end()) {
                continue;
            }
            if (node.op_type != "Reshape" || index != 1) {
                throw JitThrow{
                    "autograd_run_onnx(): INT64 initializer '" + input_name
                        + "' may be used only as Reshape input 1",
                    line};
            }
            used_integer_initializers.insert(input_name);
        }
    }
    for (const std::string& output_name : graph.outputs) {
        if (graph.integer_initializers.find(output_name)
            != graph.integer_initializers.end()) {
            throw JitThrow{
                "autograd_run_onnx(): INT64 shape initializers cannot be graph outputs",
                line};
        }
    }
    if (used_integer_initializers.size() != graph.integer_initializers.size()) {
        throw JitThrow{
            "autograd_run_onnx(): every INT64 initializer must be used as a Reshape shape input",
            line};
    }

    std::unordered_set<std::string> required_inputs;
    std::unordered_set<std::string> all_input_names;
    for (const std::string& name : graph.inputs) {
        if (!all_input_names.insert(name).second) {
            throw JitThrow{"autograd_run_onnx(): duplicate graph input name '" + name + "'", line};
        }
        if (initializer_names.find(name) == initializer_names.end()) {
            required_inputs.insert(name);
        }
        occupied_names.insert(name);
    }
    for (const auto& entry : inputs->elements) {
        if (required_inputs.find(entry.first) == required_inputs.end()) {
            throw JitThrow{"autograd_run_onnx(): unexpected input '" + entry.first + "'", line};
        }
        GCTensor* tensor = ag_need_tensor("autograd_run_onnx", entry.second, 1, line);
        ag_check_graph_operand("autograd_run_onnx", tensor, line);
        if (ag_is_cuda(tensor)) {
            throw JitThrow{"autograd_run_onnx(): v1 executor accepts CPU tensors only", line};
        }
        onnx_exec_validate_declared_value(graph, entry.first, entry.second, line);
        environment->elements.emplace(entry.first, entry.second);
    }
    for (const std::string& name : required_inputs) {
        if (inputs->elements.find(name) == inputs->elements.end()) {
            throw JitThrow{"autograd_run_onnx(): missing graph input '" + name + "'", line};
        }
    }

    for (const OnnxExecNode& node : graph.nodes) {
        if (node.outputs.size() != 1 || node.outputs[0].empty()
            || !occupied_names.insert(node.outputs[0]).second) {
            throw JitThrow{"autograd_run_onnx(): node output names must be unique", line};
        }
        Value result = onnx_exec_node(graph, node, environment, line);
        if (!result.is_tensor()) {
            throw JitThrow{"autograd_run_onnx(): supported operators must produce tensors", line};
        }
        onnx_exec_validate_declared_value(graph, node.outputs[0], result, line);
        environment->elements.emplace(node.outputs[0], result);
    }

    Value outputs = Value::make_dict();
    GCNativeRoot outputs_root(outputs.as_obj());
    std::unordered_set<std::string> output_names;
    for (const std::string& name : graph.outputs) {
        if (!output_names.insert(name).second) {
            throw JitThrow{"autograd_run_onnx(): duplicate graph output name '" + name + "'", line};
        }
        auto found = environment->elements.find(name);
        if (found == environment->elements.end() || !found->second.is_tensor()) {
            throw JitThrow{"autograd_run_onnx(): graph output '" + name + "' is unavailable", line};
        }
        outputs.as_dict()->elements.emplace(name, found->second);
    }
    return outputs;
}

inline Value b_autograd_save_onnx_weights(const Value* args, int nargs, int line) {
    need_args("autograd_save_onnx_weights", nargs, 2, 2, line);
    std::string path = need_str("autograd_save_onnx_weights", args[1], 1, line);
    safetensors_validate_path("autograd_save_onnx_weights", path, line);
    std::vector<unsigned char> model = onnx_build_model(args[0], line);
    if (model.empty() || model.size() > ONNX_WEIGHTS_MAX_FILE_BYTES) {
        throw JitThrow{"autograd_save_onnx_weights(): model exceeds file limit", line};
    }
    AgTemporaryBytes temporary(model.size(), "autograd_save_onnx_weights", line);
    onnx_atomic_write(std::filesystem::u8path(path), model, line);
    return Value((double)model.size());
}

inline Value b_autograd_load_onnx_weights(const Value* args, int nargs, int line) {
    need_args("autograd_load_onnx_weights", nargs, 1, 2, line);
    std::string path = need_str("autograd_load_onnx_weights", args[0], 0, line);
    safetensors_validate_path("autograd_load_onnx_weights", path, line);
    GCDict* options = nn_options("autograd_load_onnx_weights", args, nargs, 1, line);
    ag_validate_options("autograd_load_onnx_weights", options, {"requires_grad"}, line);
    bool requires_grad = nn_option_bool(
        "autograd_load_onnx_weights", options, "requires_grad", false, line);
    std::filesystem::path source = std::filesystem::u8path(path);
    std::error_code ec;
    auto status = std::filesystem::symlink_status(source, ec);
    if (ec || status.type() != std::filesystem::file_type::regular) {
        throw JitThrow{"autograd_load_onnx_weights(): path must be a regular non-symlink file", line};
    }
    uintmax_t file_size = std::filesystem::file_size(source, ec);
    if (ec || file_size == 0 || file_size > ONNX_WEIGHTS_MAX_FILE_BYTES
        || file_size > (uintmax_t)ag_max_external_bytes()) {
        throw JitThrow{"autograd_load_onnx_weights(): invalid or oversized ONNX file", line};
    }
    AgTemporaryBytes file_memory((size_t)file_size, "autograd_load_onnx_weights", line);
    std::vector<unsigned char> file((size_t)file_size);
    {
        std::ifstream input(source, std::ios::binary);
        if (!input) throw JitThrow{"autograd_load_onnx_weights(): cannot open file", line};
        input.read(reinterpret_cast<char*>(file.data()), (std::streamsize)file.size());
        if (!input || input.peek() != std::char_traits<char>::eof()) {
            throw JitThrow{"autograd_load_onnx_weights(): failed reading complete file", line};
        }
    }
    std::vector<OnnxWeightRecord> records = onnx_parse_model(file, line);
    size_t output_bytes = 0;
    size_t largest_conversion = 0;
    for (const auto& record : records) {
        size_t packed = record.numel * tensor_dtype_size(record.dtype);
        if (packed > ag_max_external_bytes() - output_bytes) {
            throw JitThrow{"autograd_load_onnx_weights(): initializer buffers exceed memory limit", line};
        }
        output_bytes += packed;
        largest_conversion = std::max(largest_conversion, record.numel * sizeof(double));
    }
    if (largest_conversion > ag_max_external_bytes() - output_bytes) {
        throw JitThrow{"autograd_load_onnx_weights(): conversion buffer exceeds memory limit", line};
    }
    ag_preflight_bytes(output_bytes + largest_conversion, "autograd_load_onnx_weights", line);
    Value result = Value::make_dict();
    GCNativeRoot result_root(result.as_obj());
    std::vector<GCTensor*> created;
    try {
        for (auto& record : records) {
            size_t element_bytes = tensor_dtype_size(record.dtype);
            std::vector<double> values(record.numel);
            for (size_t i = 0; i < record.numel; ++i) {
                values[i] = safetensors_decode_value(
                    record.raw.data() + i * element_bytes, record.dtype,
                    "autograd_load_onnx_weights", line);
            }
            Value tensor_value = ag_make_tensor(
                "autograd_load_onnx_weights", std::move(values), std::move(record.shape),
                requires_grad, TensorOp::LEAF, {}, line, record.dtype);
            created.push_back(tensor_value.as_tensor());
            result.as_dict()->elements.emplace(record.name, tensor_value);
        }
    } catch (...) {
        checkpoint_discard_loaded_tensors(created);
        throw;
    }
    return result;
}

} // namespace SuraStd
