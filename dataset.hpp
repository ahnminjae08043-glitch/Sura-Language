#pragma once

// Bounded, seek-based uint32 token dataset MVP.
//
// Packing reads source files in configurable chunks and never materializes a
// complete file. Opening validates the complete shard with a streaming
// checksum pass. Each next() call then seeks only to the samples needed for a
// single batch and returns non-gradient float32 tensors. Float32 is temporary:
// token IDs are capped at 2^24 until Sura exposes a native integer Tensor dtype.

#include "tokenizer.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace SuraStd {

static constexpr uint32_t DATASET_VERSION = 1;
static constexpr uint32_t DATASET_HEADER_SIZE = 32;
static constexpr uint64_t DATASET_MAX_TOKENS = 0xffffffffULL;
static constexpr uint64_t DATASET_MAX_SOURCES = 1000000ULL;
static constexpr uint64_t DATASET_DEFAULT_CHUNK_BYTES = 64ULL * 1024ULL;
static constexpr uint64_t DATASET_MAX_CHUNK_BYTES = 4ULL * 1024ULL * 1024ULL;
static constexpr uint64_t DATASET_MAX_BATCH_SIZE = 1000000ULL;
static constexpr uint64_t DATASET_MAX_SEQUENCE_LENGTH = 10000000ULL;
static constexpr uint64_t DATASET_MAX_WORLD_SIZE = 1000000ULL;

struct TokDataDatasetMetadata {
    uint64_t token_count = 0;
    uint32_t vocab_size = 0;
    uint64_t checksum = 0;
    uint64_t file_bytes = 0;
};

inline std::string dataset_option_string(const char* name, const GCDict* options,
                                         const char* key, const std::string& fallback,
                                         int line) {
    const Value* value = tokdata_dict_find(options, key);
    if (!value) return fallback;
    if (!value->is_str()) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be a string", line};
    }
    return value->as_str_ref();
}

template<typename Callback>
inline void dataset_for_each_source(const char* name, const Value& value,
                                    Callback&& callback, int line) {
    if (value.is_str()) {
        callback(value.as_str_ref(), (size_t)0);
        return;
    }
    if (!value.is_arr()) {
        throw JitThrow{std::string(name) + "(): sources must be a string or flat string array", line};
    }
    GCArray* sources = value.as_arr();
    if (sources->elements.size() > DATASET_MAX_SOURCES) {
        throw JitThrow{std::string(name) + "(): too many sources", line};
    }
    for (size_t i = 0; i < sources->elements.size(); ++i) {
        if (!sources->elements[i].is_str()) {
            throw JitThrow{std::string(name) + "(): source " + std::to_string(i + 1)
                           + " must be a string", line};
        }
        callback(sources->elements[i].as_str_ref(), i);
    }
}

inline void dataset_write_u32(std::ostream& out, uint32_t value,
                              const char* name, int line) {
    unsigned char bytes[4];
    tokdata_store_u32(bytes, value);
    out.write((const char*)bytes, 4);
    if (!out) throw JitThrow{std::string(name) + "(): failed writing token payload", line};
}

inline void dataset_append_special(std::ostream& out, uint32_t id,
                                   uint64_t& token_count,
                                   const char* name, int line) {
    if (id == TOKDATA_NO_ID) return;
    if (token_count >= DATASET_MAX_TOKENS) {
        throw JitThrow{std::string(name) + "(): token count exceeds the uint32 shard limit", line};
    }
    dataset_write_u32(out, id, name, line);
    ++token_count;
}

inline void dataset_append_bytes(std::ostream& out, const unsigned char* input,
                                 size_t size, std::vector<unsigned char>& encoded,
                                 uint64_t& token_count,
                                 const char* name, int line) {
    if ((uint64_t)size > DATASET_MAX_TOKENS - token_count) {
        throw JitThrow{std::string(name) + "(): token count exceeds the uint32 shard limit", line};
    }
    if (size > std::numeric_limits<size_t>::max() / 4) {
        throw JitThrow{std::string(name) + "(): encoded chunk size overflow", line};
    }
    encoded.resize(size * 4);
    for (size_t i = 0; i < size; ++i) {
        tokdata_store_u32(encoded.data() + i * 4, (uint32_t)input[i]);
    }
    if (!encoded.empty()) out.write((const char*)encoded.data(), (std::streamsize)encoded.size());
    if (!out) throw JitThrow{std::string(name) + "(): failed writing token payload", line};
    token_count += (uint64_t)size;
}

inline void dataset_append_ids(std::ostream& out, const std::vector<uint32_t>& ids,
                               size_t count, std::vector<unsigned char>& encoded,
                               uint64_t& token_count,
                               const char* name, int line) {
    if (count > ids.size() || (uint64_t)count > DATASET_MAX_TOKENS - token_count) {
        throw JitThrow{std::string(name) + "(): token count exceeds the uint32 shard limit", line};
    }
    if (count > std::numeric_limits<size_t>::max() / 4) {
        throw JitThrow{std::string(name) + "(): encoded chunk size overflow", line};
    }
    encoded.resize(count * 4);
    for (size_t i = 0; i < count; ++i) {
        tokdata_store_u32(encoded.data() + i * 4, ids[i]);
    }
    if (!encoded.empty()) out.write((const char*)encoded.data(), (std::streamsize)encoded.size());
    if (!out) throw JitThrow{std::string(name) + "(): failed writing token payload", line};
    token_count += (uint64_t)count;
}

inline void dataset_append_bpe_chunk(std::ostream& out, std::string& pending,
                                     const char* input, size_t size, bool final,
                                     const TokDataTokenizerConfig& tokenizer,
                                     std::vector<unsigned char>& encoded,
                                     uint64_t& token_count,
                                     const char* name, int line) {
    if (size > std::numeric_limits<size_t>::max() - pending.size()) {
        throw JitThrow{std::string(name) + "(): BPE chunk size overflow", line};
    }
    if (size) pending.append(input, size);
    if (pending.empty()) return;
    if (!final && pending.size() <= tokenizer.max_piece_size + TOKDATA_BPE_STREAM_FLUSH_BYTES) return;
    if (pending.size() > TOKDATA_MAX_DIRECT_TEXT_BYTES) {
        throw JitThrow{std::string(name) + "(): BPE streaming window exceeds the 16 MiB safety limit", line};
    }

    std::vector<uint32_t> ids = tokdata_bpe_encode(pending, tokenizer);
    const size_t safe_bytes = final ? pending.size()
        : pending.size() - tokenizer.max_piece_size;
    size_t committed_ids = 0;
    size_t committed_bytes = 0;
    while (committed_ids < ids.size()) {
        const uint32_t id = ids[committed_ids];
        const size_t piece_bytes = tokenizer.pieces[id].size();
        if (!final && (piece_bytes > safe_bytes - committed_bytes)) break;
        committed_bytes += piece_bytes;
        ++committed_ids;
    }
    dataset_append_ids(out, ids, committed_ids, encoded, token_count, name, line);
    if (committed_bytes) pending.erase(0, committed_bytes);
    if (final && !pending.empty()) {
        throw JitThrow{std::string(name) + "(): internal BPE streaming boundary mismatch", line};
    }
}

inline uint64_t dataset_hash_prefix(const std::filesystem::path& path,
                                    uint64_t bytes, const char* name, int line) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw JitThrow{std::string(name) + "(): cannot reopen temporary shard", line};
    TokDataFnv64 hash;
    std::array<unsigned char, 64 * 1024> buffer{};
    uint64_t remaining = bytes;
    while (remaining > 0) {
        size_t wanted = (size_t)std::min<uint64_t>(remaining, buffer.size());
        in.read((char*)buffer.data(), (std::streamsize)wanted);
        if ((size_t)in.gcount() != wanted) {
            throw JitThrow{std::string(name) + "(): truncated temporary shard", line};
        }
        hash.update(buffer.data(), wanted);
        remaining -= wanted;
    }
    return hash.value;
}

inline TokDataDatasetMetadata dataset_scan_file(const std::string& path,
                                                bool verify_payload,
                                                const char* name, int line) {
    tokdata_validate_path(name, path, line);
    std::filesystem::path source = fs_path_from_utf8(path);
    std::error_code ec;
    uintmax_t raw_size = std::filesystem::file_size(source, ec);
    if (ec) throw JitThrow{std::string(name) + "(): cannot inspect shard: " + ec.message(), line};
    if (raw_size < DATASET_HEADER_SIZE + 8ULL) {
        throw JitThrow{std::string(name) + "(): shard is truncated", line};
    }
    if (raw_size > DATASET_HEADER_SIZE + DATASET_MAX_TOKENS * 4ULL + 8ULL) {
        throw JitThrow{std::string(name) + "(): shard exceeds the supported size", line};
    }

    std::ifstream in(source, std::ios::binary);
    if (!in) throw JitThrow{std::string(name) + "(): cannot open shard", line};
    std::array<unsigned char, DATASET_HEADER_SIZE> header{};
    in.read((char*)header.data(), (std::streamsize)header.size());
    if ((size_t)in.gcount() != header.size()) {
        throw JitThrow{std::string(name) + "(): shard header is truncated", line};
    }
    const std::array<unsigned char, 8> magic{{'S','U','R','A','D','A','T','1'}};
    if (!std::equal(magic.begin(), magic.end(), header.begin())) {
        throw JitThrow{std::string(name) + "(): invalid shard magic", line};
    }
    if (tokdata_load_u32(header.data() + 8) != DATASET_VERSION
        || tokdata_load_u32(header.data() + 12) != DATASET_HEADER_SIZE
        || tokdata_load_u32(header.data() + 28) != 0U) {
        throw JitThrow{std::string(name) + "(): unsupported shard version or flags", line};
    }
    TokDataDatasetMetadata metadata;
    metadata.token_count = tokdata_load_u64(header.data() + 16);
    metadata.vocab_size = tokdata_load_u32(header.data() + 24);
    metadata.file_bytes = (uint64_t)raw_size;
    if (metadata.token_count > DATASET_MAX_TOKENS
        || metadata.vocab_size < TOKDATA_BYTE_VOCAB
        || metadata.vocab_size > TOKDATA_MAX_EXACT_FLOAT32_ID + 1U) {
        throw JitThrow{std::string(name) + "(): invalid shard dimensions or vocabulary", line};
    }
    uint64_t expected_size = DATASET_HEADER_SIZE + metadata.token_count * 4ULL + 8ULL;
    if (expected_size != metadata.file_bytes) {
        throw JitThrow{std::string(name) + "(): shard size does not match its token count", line};
    }

    if (verify_payload) {
        TokDataFnv64 hash;
        hash.update(header.data(), header.size());
        std::array<unsigned char, 64 * 1024> buffer{};
        uint64_t remaining_tokens = metadata.token_count;
        while (remaining_tokens > 0) {
            size_t tokens = (size_t)std::min<uint64_t>(remaining_tokens, buffer.size() / 4);
            size_t bytes = tokens * 4;
            in.read((char*)buffer.data(), (std::streamsize)bytes);
            if ((size_t)in.gcount() != bytes) {
                throw JitThrow{std::string(name) + "(): shard payload is truncated", line};
            }
            hash.update(buffer.data(), bytes);
            for (size_t i = 0; i < tokens; ++i) {
                if (tokdata_load_u32(buffer.data() + i * 4) >= metadata.vocab_size) {
                    throw JitThrow{std::string(name) + "(): shard contains an out-of-vocabulary token ID", line};
                }
            }
            remaining_tokens -= tokens;
        }
        unsigned char footer[8];
        in.read((char*)footer, 8);
        if ((size_t)in.gcount() != 8) {
            throw JitThrow{std::string(name) + "(): shard checksum footer is truncated", line};
        }
        metadata.checksum = tokdata_load_u64(footer);
        if (metadata.checksum != hash.value) {
            throw JitThrow{std::string(name) + "(): shard checksum mismatch", line};
        }
    } else {
        in.seekg((std::streamoff)(DATASET_HEADER_SIZE + metadata.token_count * 4ULL), std::ios::beg);
        unsigned char footer[8];
        in.read((char*)footer, 8);
        if ((size_t)in.gcount() != 8) {
            throw JitThrow{std::string(name) + "(): shard checksum footer is truncated", line};
        }
        metadata.checksum = tokdata_load_u64(footer);
    }
    return metadata;
}

inline uint64_t dataset_sample_count(uint64_t tokens, uint64_t sequence,
                                     uint64_t stride) {
    if (tokens < sequence + 1ULL) return 0;
    return 1ULL + (tokens - sequence - 1ULL) / stride;
}

inline uint64_t dataset_local_sample_count(uint64_t samples, uint64_t rank,
                                           uint64_t world_size) {
    if (rank >= samples) return 0;
    return 1ULL + (samples - 1ULL - rank) / world_size;
}

inline uint64_t dataset_mix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

inline uint64_t dataset_mul_mod(uint64_t left, uint64_t right, uint64_t modulus) {
    if (modulus <= 1) return 0;
#if defined(__SIZEOF_INT128__)
    return (uint64_t)(((unsigned __int128)left * (unsigned __int128)right) % modulus);
#else
    uint64_t result = 0;
    left %= modulus;
    while (right) {
        if (right & 1ULL) result = result >= modulus - left ? result - (modulus - left) : result + left;
        right >>= 1;
        if (right) left = left >= modulus - left ? left - (modulus - left) : left + left;
    }
    return result;
#endif
}

inline uint64_t dataset_permute(uint64_t index, uint64_t count,
                                uint64_t seed, uint64_t epoch) {
    if (count <= 1) return 0;
    uint64_t mixed = dataset_mix64(seed ^ dataset_mix64(epoch));
    uint64_t multiplier = mixed % count;
    if (multiplier == 0) multiplier = 1;
    while (std::gcd(multiplier, count) != 1ULL) {
        ++multiplier;
        if (multiplier == count) multiplier = 1;
    }
    uint64_t offset = dataset_mix64(mixed ^ 0xd1b54a32d192ed03ULL) % count;
    uint64_t product = dataset_mul_mod(multiplier, index, count);
    return product >= count - offset ? product - (count - offset) : product + offset;
}

inline Value b_dataset_pack_text(const Value* a, int n, int l) {
    need_args("dataset_pack_text", n, 3, 4, l);
    TokDataTokenizerConfig tokenizer = tokdata_need_tokenizer("dataset_pack_text", a[1], l);
    std::string output_path = need_str("dataset_pack_text", a[2], 2, l);
    tokdata_validate_path("dataset_pack_text", output_path, l);
    GCDict* options = nullptr;
    if (n == 4) options = need_dict("dataset_pack_text", a[3], 3, l);
    tokdata_validate_options("dataset_pack_text", options,
                             {"input", "chunk_bytes", "add_bos", "add_eos"}, l);
    std::string input_mode = dataset_option_string("dataset_pack_text", options,
                                                   "input", "text", l);
    if (input_mode != "text" && input_mode != "files") {
        throw JitThrow{"dataset_pack_text(): option input must be 'text' or 'files'", l};
    }
    size_t chunk_bytes = (size_t)tokdata_option_integer(
        "dataset_pack_text", options, "chunk_bytes", DATASET_DEFAULT_CHUNK_BYTES,
        1, DATASET_MAX_CHUNK_BYTES, l);
    bool add_bos = tokdata_option_bool("dataset_pack_text", options, "add_bos", false, l);
    bool add_eos = tokdata_option_bool("dataset_pack_text", options, "add_eos", false, l);
    if (add_bos && tokenizer.bos == TOKDATA_NO_ID) {
        throw JitThrow{"dataset_pack_text(): add_bos requires a configured bos_id", l};
    }
    if (add_eos && tokenizer.eos == TOKDATA_NO_ID) {
        throw JitThrow{"dataset_pack_text(): add_eos requires a configured eos_id", l};
    }

    std::filesystem::path target = fs_path_from_utf8(output_path);
    tokdata_prepare_parent("dataset_pack_text", target, l);
    std::filesystem::path temp = tokdata_unique_temp_path("dataset_pack_text", output_path, l);
    TokDataTempGuard cleanup(temp);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) throw JitThrow{"dataset_pack_text(): cannot open temporary shard", l};

    std::array<unsigned char, DATASET_HEADER_SIZE> header{};
    const std::array<unsigned char, 8> magic{{'S','U','R','A','D','A','T','1'}};
    std::copy(magic.begin(), magic.end(), header.begin());
    tokdata_store_u32(header.data() + 8, DATASET_VERSION);
    tokdata_store_u32(header.data() + 12, DATASET_HEADER_SIZE);
    tokdata_store_u64(header.data() + 16, 0ULL);
    tokdata_store_u32(header.data() + 24, tokenizer.vocab_size);
    tokdata_store_u32(header.data() + 28, 0U);
    out.write((const char*)header.data(), (std::streamsize)header.size());
    if (!out) throw JitThrow{"dataset_pack_text(): failed writing shard header", l};

    uint64_t token_count = 0;
    uint64_t source_count = 0;
    std::vector<unsigned char> encoded;
    encoded.reserve(chunk_bytes * 4);
    std::vector<unsigned char> input_buffer(chunk_bytes);
    std::string bpe_pending;
    if (tokenizer.is_bpe) {
        bpe_pending.reserve(std::min<size_t>(
            TOKDATA_MAX_DIRECT_TEXT_BYTES, chunk_bytes + tokenizer.max_piece_size));
    }

    dataset_for_each_source("dataset_pack_text", a[0],
        [&](const std::string& source, size_t) {
            ++source_count;
            if (add_bos) dataset_append_special(out, tokenizer.bos, token_count,
                                                "dataset_pack_text", l);
            if (input_mode == "files") {
                tokdata_validate_path("dataset_pack_text", source, l);
                std::ifstream input(fs_path_from_utf8(source), std::ios::binary);
                if (!input) {
                    throw JitThrow{"dataset_pack_text(): cannot open source file '" + source + "'", l};
                }
                while (input) {
                    input.read((char*)input_buffer.data(), (std::streamsize)input_buffer.size());
                    std::streamsize got = input.gcount();
                    if (got > 0) {
                        if (tokenizer.is_bpe) {
                            dataset_append_bpe_chunk(out, bpe_pending,
                                (const char*)input_buffer.data(), (size_t)got, false,
                                tokenizer, encoded, token_count, "dataset_pack_text", l);
                        } else {
                            dataset_append_bytes(out, input_buffer.data(), (size_t)got, encoded,
                                                 token_count, "dataset_pack_text", l);
                        }
                    }
                }
                if (!input.eof()) {
                    throw JitThrow{"dataset_pack_text(): failed reading source file '" + source + "'", l};
                }
            } else {
                size_t offset = 0;
                while (offset < source.size()) {
                    size_t count = std::min(chunk_bytes, source.size() - offset);
                    if (tokenizer.is_bpe) {
                        dataset_append_bpe_chunk(out, bpe_pending, source.data() + offset,
                            count, false, tokenizer, encoded, token_count,
                            "dataset_pack_text", l);
                    } else {
                        dataset_append_bytes(out,
                            (const unsigned char*)source.data() + offset, count, encoded,
                            token_count, "dataset_pack_text", l);
                    }
                    offset += count;
                }
            }
            if (tokenizer.is_bpe) {
                dataset_append_bpe_chunk(out, bpe_pending, nullptr, 0, true,
                    tokenizer, encoded, token_count, "dataset_pack_text", l);
            }
            if (add_eos) dataset_append_special(out, tokenizer.eos, token_count,
                                                "dataset_pack_text", l);
        }, l);

    tokdata_store_u64(header.data() + 16, token_count);
    out.seekp(0, std::ios::beg);
    out.write((const char*)header.data(), (std::streamsize)header.size());
    out.flush();
    if (!out) throw JitThrow{"dataset_pack_text(): failed finalizing shard header", l};
    out.close();

    uint64_t prefix_bytes = DATASET_HEADER_SIZE + token_count * 4ULL;
    uint64_t checksum = dataset_hash_prefix(temp, prefix_bytes, "dataset_pack_text", l);
    std::ofstream footer_out(temp, std::ios::binary | std::ios::app);
    if (!footer_out) throw JitThrow{"dataset_pack_text(): cannot append shard checksum", l};
    unsigned char footer[8];
    tokdata_store_u64(footer, checksum);
    footer_out.write((const char*)footer, 8);
    footer_out.flush();
    if (!footer_out) throw JitThrow{"dataset_pack_text(): failed writing shard checksum", l};
    footer_out.close();

    tokdata_commit_file("dataset_pack_text", temp, target, l);
    cleanup.release();
    Value result = Value::make_dict();
    GCDict* info = result.as_dict();
    info->elements["format"] = Value(std::string("sura.dataset.uint32.v1"));
    info->elements["path"] = Value(output_path);
    info->elements["source_count"] = Value((double)source_count);
    info->elements["token_count"] = Value((double)token_count);
    info->elements["vocab_size"] = Value((double)tokenizer.vocab_size);
    info->elements["checksum"] = Value(tokdata_hex64(checksum));
    info->elements["file_bytes"] = Value((double)(prefix_bytes + 8ULL));
    return result;
}

inline Value dataset_make_handle(const std::string& path,
                                 const TokDataDatasetMetadata& metadata,
                                 uint64_t batch_size, uint64_t sequence,
                                 uint64_t stride, bool shuffle, uint64_t seed,
                                 uint64_t rank, uint64_t world_size,
                                 uint64_t epoch, bool drop_last) {
    uint64_t samples = dataset_sample_count(metadata.token_count, sequence, stride);
    uint64_t local_samples = dataset_local_sample_count(samples, rank, world_size);
    Value result = Value::make_dict();
    GCDict* dict = result.as_dict();
    dict->elements["__sura_handle"] = Value(std::string("dataset.uint32.v1"));
    dict->elements["path"] = Value(path);
    dict->elements["token_count"] = Value((double)metadata.token_count);
    dict->elements["vocab_size"] = Value((double)metadata.vocab_size);
    dict->elements["checksum"] = Value(tokdata_hex64(metadata.checksum));
    dict->elements["file_bytes"] = Value((double)metadata.file_bytes);
    dict->elements["batch_size"] = Value((double)batch_size);
    dict->elements["sequence_length"] = Value((double)sequence);
    dict->elements["stride"] = Value((double)stride);
    dict->elements["shuffle"] = Value(shuffle);
    dict->elements["seed"] = Value((double)seed);
    dict->elements["rank"] = Value((double)rank);
    dict->elements["world_size"] = Value((double)world_size);
    dict->elements["epoch"] = Value((double)epoch);
    dict->elements["drop_last"] = Value(drop_last);
    dict->elements["sample_count"] = Value((double)samples);
    dict->elements["local_sample_count"] = Value((double)local_samples);
    dict->elements["position"] = Value(0.0);
    dict->elements["closed"] = Value(false);
    return result;
}

struct TokDataDatasetState {
    GCDict* dict = nullptr;
    std::string path;
    TokDataDatasetMetadata metadata;
    uint64_t batch_size = 0;
    uint64_t sequence = 0;
    uint64_t stride = 0;
    uint64_t seed = 0;
    uint64_t rank = 0;
    uint64_t world_size = 1;
    uint64_t epoch = 0;
    uint64_t samples = 0;
    uint64_t local_samples = 0;
    uint64_t position = 0;
    bool shuffle = false;
    bool drop_last = false;
    bool closed = false;
};

inline TokDataDatasetState dataset_need_handle(const char* name, const Value& value,
                                               bool require_open, int line) {
    if (!value.is_dict()) throw JitThrow{std::string(name) + "(): expected a dataset handle", line};
    TokDataDatasetState state;
    state.dict = value.as_dict();
    const Value* kind = tokdata_dict_find(state.dict, "__sura_handle");
    const Value* path = tokdata_dict_find(state.dict, "path");
    const Value* checksum = tokdata_dict_find(state.dict, "checksum");
    if (!kind || !kind->is_str() || kind->as_str_ref() != "dataset.uint32.v1"
        || !path || !path->is_str() || !checksum || !checksum->is_str()) {
        throw JitThrow{std::string(name) + "(): invalid dataset handle", line};
    }
    state.path = path->as_str_ref();
    tokdata_validate_path(name, state.path, line);
    auto integer_field = [&](const char* key, uint64_t minimum, uint64_t maximum) {
        const Value* field = tokdata_dict_find(state.dict, key);
        if (!field) throw JitThrow{std::string(name) + "(): dataset handle is missing " + key, line};
        return tokdata_safe_integer(name, *field, key, minimum, maximum, line);
    };
    auto bool_field = [&](const char* key) {
        const Value* field = tokdata_dict_find(state.dict, key);
        if (!field || !field->is_bool()) {
            throw JitThrow{std::string(name) + "(): dataset handle has invalid " + key, line};
        }
        return field->as_bool();
    };
    state.metadata.token_count = integer_field("token_count", 0, DATASET_MAX_TOKENS);
    state.metadata.vocab_size = (uint32_t)integer_field(
        "vocab_size", TOKDATA_BYTE_VOCAB, TOKDATA_MAX_EXACT_FLOAT32_ID + 1ULL);
    state.metadata.file_bytes = integer_field(
        "file_bytes", DATASET_HEADER_SIZE + 8ULL,
        DATASET_HEADER_SIZE + DATASET_MAX_TOKENS * 4ULL + 8ULL);
    if (!tokdata_parse_hex64(checksum->as_str_ref(), state.metadata.checksum)) {
        throw JitThrow{std::string(name) + "(): dataset handle has invalid checksum metadata", line};
    }
    state.batch_size = integer_field("batch_size", 1, DATASET_MAX_BATCH_SIZE);
    state.sequence = integer_field("sequence_length", 1, DATASET_MAX_SEQUENCE_LENGTH);
    state.stride = integer_field("stride", 1, DATASET_MAX_TOKENS);
    state.seed = integer_field("seed", 0, 9007199254740991ULL);
    state.rank = integer_field("rank", 0, DATASET_MAX_WORLD_SIZE - 1ULL);
    state.world_size = integer_field("world_size", 1, DATASET_MAX_WORLD_SIZE);
    if (state.rank >= state.world_size) {
        throw JitThrow{std::string(name) + "(): dataset rank must be less than world_size", line};
    }
    state.epoch = integer_field("epoch", 0, 9007199254740991ULL);
    state.samples = integer_field("sample_count", 0, DATASET_MAX_TOKENS);
    state.local_samples = integer_field("local_sample_count", 0, DATASET_MAX_TOKENS);
    state.position = integer_field("position", 0, state.local_samples);
    state.shuffle = bool_field("shuffle");
    state.drop_last = bool_field("drop_last");
    state.closed = bool_field("closed");
    uint64_t expected_samples = dataset_sample_count(
        state.metadata.token_count, state.sequence, state.stride);
    uint64_t expected_local = dataset_local_sample_count(
        expected_samples, state.rank, state.world_size);
    if (state.samples != expected_samples || state.local_samples != expected_local) {
        throw JitThrow{std::string(name) + "(): dataset handle sample metadata is inconsistent", line};
    }
    if (require_open && state.closed) {
        throw JitThrow{std::string(name) + "(): dataset handle is closed", line};
    }
    return state;
}

inline Value b_dataset_open(const Value* a, int n, int l) {
    need_args("dataset_open", n, 1, 2, l);
    std::string path = need_str("dataset_open", a[0], 0, l);
    GCDict* options = nullptr;
    if (n == 2) options = need_dict("dataset_open", a[1], 1, l);
    tokdata_validate_options("dataset_open", options,
                             {"batch_size", "sequence_length", "stride", "shuffle",
                              "seed", "rank", "world_size", "epoch", "drop_last"}, l);
    uint64_t batch_size = tokdata_option_integer(
        "dataset_open", options, "batch_size", 1, 1, DATASET_MAX_BATCH_SIZE, l);
    uint64_t sequence = tokdata_option_integer(
        "dataset_open", options, "sequence_length", 128, 1,
        DATASET_MAX_SEQUENCE_LENGTH, l);
    uint64_t stride = tokdata_option_integer(
        "dataset_open", options, "stride", sequence, 1, DATASET_MAX_TOKENS, l);
    bool shuffle = tokdata_option_bool("dataset_open", options, "shuffle", false, l);
    uint64_t seed = tokdata_option_integer(
        "dataset_open", options, "seed", 0, 0, 9007199254740991ULL, l);
    uint64_t world_size = tokdata_option_integer(
        "dataset_open", options, "world_size", 1, 1, DATASET_MAX_WORLD_SIZE, l);
    uint64_t rank = tokdata_option_integer(
        "dataset_open", options, "rank", 0, 0, DATASET_MAX_WORLD_SIZE - 1ULL, l);
    if (rank >= world_size) throw JitThrow{"dataset_open(): rank must be less than world_size", l};
    uint64_t epoch = tokdata_option_integer(
        "dataset_open", options, "epoch", 0, 0, 9007199254740991ULL, l);
    bool drop_last = tokdata_option_bool("dataset_open", options, "drop_last", false, l);
    if (batch_size > std::numeric_limits<uint64_t>::max() / sequence
        || batch_size * sequence > ag_max_elements()) {
        throw JitThrow{"dataset_open(): batch_size * sequence_length exceeds the active Tensor element limit", l};
    }
    TokDataDatasetMetadata metadata = dataset_scan_file(path, true, "dataset_open", l);
    return dataset_make_handle(path, metadata, batch_size, sequence, stride,
                               shuffle, seed, rank, world_size, epoch, drop_last);
}

inline Value b_dataset_next(const Value* a, int n, int l) {
    need_args("dataset_next", n, 1, 1, l);
    TokDataDatasetState state = dataset_need_handle("dataset_next", a[0], true, l);
    if (state.position >= state.local_samples) return Value::nil();
    uint64_t actual = std::min(state.batch_size, state.local_samples - state.position);
    if (state.drop_last && actual < state.batch_size) {
        state.dict->elements["position"] = Value((double)state.local_samples);
        return Value::nil();
    }
    if (actual > std::numeric_limits<uint64_t>::max() / state.sequence) {
        throw JitThrow{"dataset_next(): batch element count overflow", l};
    }
    uint64_t element_count64 = actual * state.sequence;
    if (element_count64 > ag_max_elements() || element_count64 > (uint64_t)std::numeric_limits<size_t>::max()) {
        throw JitThrow{"dataset_next(): batch exceeds the active Tensor element limit", l};
    }
    size_t element_count = (size_t)element_count64;
    if (state.sequence + 1ULL > (uint64_t)std::numeric_limits<size_t>::max() / 4ULL) {
        throw JitThrow{"dataset_next(): read buffer size overflow", l};
    }
    size_t read_bytes = (size_t)(state.sequence + 1ULL) * 4ULL;
    if (element_count > (std::numeric_limits<size_t>::max() - read_bytes) / (2ULL * sizeof(double))) {
        throw JitThrow{"dataset_next(): temporary batch size overflow", l};
    }
    size_t temporary_bytes = element_count * 2ULL * sizeof(double) + read_bytes;
    AgTemporaryBytes temporary(temporary_bytes, "dataset_next", l);
    std::vector<double> inputs(element_count, 0.0);
    std::vector<double> targets(element_count, 0.0);
    std::vector<unsigned char> buffer(read_bytes);
    std::vector<uint64_t> sample_ids((size_t)actual, 0);

    TokDataDatasetMetadata live = dataset_scan_file(state.path, false, "dataset_next", l);
    if (live.token_count != state.metadata.token_count
        || live.vocab_size != state.metadata.vocab_size
        || live.file_bytes != state.metadata.file_bytes
        || live.checksum != state.metadata.checksum) {
        throw JitThrow{"dataset_next(): shard changed after it was opened", l};
    }
    std::ifstream in(fs_path_from_utf8(state.path), std::ios::binary);
    if (!in) throw JitThrow{"dataset_next(): cannot open shard", l};
    for (uint64_t row = 0; row < actual; ++row) {
        uint64_t distributed_index = state.rank + (state.position + row) * state.world_size;
        if (distributed_index >= state.samples) {
            throw JitThrow{"dataset_next(): internal distributed sample index overflow", l};
        }
        uint64_t sample = state.shuffle
            ? dataset_permute(distributed_index, state.samples, state.seed, state.epoch)
            : distributed_index;
        sample_ids[(size_t)row] = sample;
        uint64_t start = sample * state.stride;
        if (start > state.metadata.token_count
            || state.sequence + 1ULL > state.metadata.token_count - start) {
            throw JitThrow{"dataset_next(): internal sample range exceeds the shard", l};
        }
        uint64_t byte_offset = DATASET_HEADER_SIZE + start * 4ULL;
        in.clear();
        in.seekg((std::streamoff)byte_offset, std::ios::beg);
        if (!in) throw JitThrow{"dataset_next(): failed seeking to sample", l};
        in.read((char*)buffer.data(), (std::streamsize)buffer.size());
        if ((size_t)in.gcount() != buffer.size()) {
            throw JitThrow{"dataset_next(): sample payload is truncated", l};
        }
        size_t base = (size_t)row * (size_t)state.sequence;
        for (size_t col = 0; col < (size_t)state.sequence; ++col) {
            uint32_t input_id = tokdata_load_u32(buffer.data() + col * 4);
            uint32_t target_id = tokdata_load_u32(buffer.data() + (col + 1) * 4);
            if (input_id >= state.metadata.vocab_size || target_id >= state.metadata.vocab_size) {
                throw JitThrow{"dataset_next(): sample contains an out-of-vocabulary token ID", l};
            }
            inputs[base + col] = (double)input_id;
            targets[base + col] = (double)target_id;
        }
    }

    Value result = Value::make_dict();
    GCNativeRoot result_root(result.as_obj());
    Value input_tensor = ag_make_tensor(
        "dataset_next", std::move(inputs), {(size_t)actual, (size_t)state.sequence},
        false, TensorOp::LEAF, {}, l, TensorDType::FLOAT32);
    result.as_dict()->elements["input_ids"] = input_tensor;
    Value target_tensor = ag_make_tensor(
        "dataset_next", std::move(targets), {(size_t)actual, (size_t)state.sequence},
        false, TensorOp::LEAF, {}, l, TensorDType::FLOAT32);
    result.as_dict()->elements["target_ids"] = target_tensor;
    Value ids = Value::make_array();
    ids.as_arr()->elements.reserve(sample_ids.size());
    for (uint64_t id : sample_ids) ids.as_arr()->elements.push_back(Value((double)id));
    result.as_dict()->elements["sample_ids"] = ids;
    result.as_dict()->elements["batch_size"] = Value((double)actual);
    result.as_dict()->elements["sequence_length"] = Value((double)state.sequence);
    result.as_dict()->elements["epoch"] = Value((double)state.epoch);
    state.dict->elements["position"] = Value((double)(state.position + actual));
    return result;
}

inline Value b_dataset_reset(const Value* a, int n, int l) {
    need_args("dataset_reset", n, 1, 2, l);
    TokDataDatasetState state = dataset_need_handle("dataset_reset", a[0], true, l);
    uint64_t epoch = state.epoch;
    if (n == 2) {
        epoch = tokdata_safe_integer("dataset_reset", a[1], "epoch", 0,
                                     9007199254740991ULL, l);
    }
    state.dict->elements["epoch"] = Value((double)epoch);
    state.dict->elements["position"] = Value(0.0);
    return a[0];
}

inline Value b_dataset_close(const Value* a, int n, int l) {
    need_args("dataset_close", n, 1, 1, l);
    TokDataDatasetState state = dataset_need_handle("dataset_close", a[0], false, l);
    bool was_open = !state.closed;
    state.dict->elements["closed"] = Value(true);
    return Value(was_open);
}

inline Value b_dataset_info(const Value* a, int n, int l) {
    need_args("dataset_info", n, 1, 1, l);
    TokDataDatasetState state = dataset_need_handle("dataset_info", a[0], false, l);
    Value result = Value::make_dict();
    GCDict* info = result.as_dict();
    info->elements["format"] = Value(std::string("sura.dataset.uint32.v1"));
    info->elements["path"] = Value(state.path);
    info->elements["token_count"] = Value((double)state.metadata.token_count);
    info->elements["vocab_size"] = Value((double)state.metadata.vocab_size);
    info->elements["checksum"] = Value(tokdata_hex64(state.metadata.checksum));
    info->elements["file_bytes"] = Value((double)state.metadata.file_bytes);
    info->elements["batch_size"] = Value((double)state.batch_size);
    info->elements["sequence_length"] = Value((double)state.sequence);
    info->elements["stride"] = Value((double)state.stride);
    info->elements["shuffle"] = Value(state.shuffle);
    info->elements["seed"] = Value((double)state.seed);
    info->elements["rank"] = Value((double)state.rank);
    info->elements["world_size"] = Value((double)state.world_size);
    info->elements["epoch"] = Value((double)state.epoch);
    info->elements["sample_count"] = Value((double)state.samples);
    info->elements["local_sample_count"] = Value((double)state.local_samples);
    info->elements["position"] = Value((double)state.position);
    info->elements["remaining"] = Value((double)(state.local_samples - state.position));
    info->elements["drop_last"] = Value(state.drop_last);
    info->elements["closed"] = Value(state.closed);
    info->elements["id_dtype"] = Value(std::string("float32"));
    info->elements["io"] = Value(std::string("seek"));
    return result;
}

} // namespace SuraStd
