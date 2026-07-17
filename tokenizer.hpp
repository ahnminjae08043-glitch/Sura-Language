#pragma once

// Bounded native byte and byte-level BPE tokenizers.
//
// This header is included by stdlib.hpp after the common Value, filesystem,
// argument-validation, and autograd helpers are available.  A tokenizer is a
// deliberately transparent Sura dictionary handle: it owns no process-global
// state and can therefore be copied, collected, and embedded safely.  Every
// public operation validates the handle again so a forged or mutated dict
// fails closed.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SuraStd {

static constexpr uint32_t TOKDATA_NO_ID = 0xffffffffU;
static constexpr uint32_t TOKDATA_BYTE_VOCAB = 256U;
// Float32 represents every integer exactly only through 2^24. Dataset batches
// use non-gradient float32 tensors until Sura gains a dedicated integer dtype.
static constexpr uint32_t TOKDATA_MAX_EXACT_FLOAT32_ID = 16777215U;
static constexpr size_t TOKDATA_MAX_DIRECT_TEXT_BYTES = 16ULL * 1024ULL * 1024ULL;
static constexpr size_t TOKDATA_MAX_PATH_BYTES = 32768;
static constexpr uint32_t TOKDATA_MAX_BPE_VOCAB = 4096U;
static constexpr size_t TOKDATA_MAX_BPE_TRAIN_BYTES = 1024ULL * 1024ULL;
static constexpr uint64_t TOKDATA_MAX_BPE_TRAIN_WORK = 64ULL * 1024ULL * 1024ULL;
static constexpr size_t TOKDATA_MAX_BPE_PIECE_BYTES = 1024ULL * 1024ULL;
static constexpr size_t TOKDATA_MAX_BPE_TOTAL_PIECE_BYTES = 64ULL * 1024ULL * 1024ULL;
static constexpr size_t TOKDATA_BPE_STREAM_FLUSH_BYTES = 64ULL * 1024ULL;

struct TokDataFnv64 {
    uint64_t value = 14695981039346656037ULL;

    void update(const unsigned char* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            value ^= (uint64_t)data[i];
            value *= 1099511628211ULL;
        }
    }

    void update_byte(unsigned char byte) { update(&byte, 1); }
};

inline void tokdata_store_u32(unsigned char* out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xffU);
    out[1] = (unsigned char)((value >> 8) & 0xffU);
    out[2] = (unsigned char)((value >> 16) & 0xffU);
    out[3] = (unsigned char)((value >> 24) & 0xffU);
}

inline void tokdata_store_u64(unsigned char* out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = (unsigned char)((value >> (i * 8)) & 0xffU);
}

inline uint32_t tokdata_load_u32(const unsigned char* in) {
    return uint32_t(in[0])
         | (uint32_t(in[1]) << 8)
         | (uint32_t(in[2]) << 16)
         | (uint32_t(in[3]) << 24);
}

inline uint64_t tokdata_load_u64(const unsigned char* in) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(in[i]) << (i * 8);
    return value;
}

inline std::string tokdata_hex64(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

inline bool tokdata_parse_hex64(const std::string& text, uint64_t& value) {
    if (text.size() != 16) return false;
    value = 0;
    for (char ch : text) {
        unsigned digit = 0;
        if (ch >= '0' && ch <= '9') digit = (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') digit = 10U + (unsigned)(ch - 'a');
        else if (ch >= 'A' && ch <= 'F') digit = 10U + (unsigned)(ch - 'A');
        else return false;
        value = (value << 4) | digit;
    }
    return true;
}

inline const Value* tokdata_dict_find(const GCDict* dict, const char* key) {
    if (!dict) return nullptr;
    auto found = dict->elements.find(key);
    return found == dict->elements.end() ? nullptr : &found->second;
}

inline void tokdata_validate_options(const char* name, const GCDict* options,
                                     std::initializer_list<const char*> allowed,
                                     int line) {
    if (!options) return;
    for (const auto& entry : options->elements) {
        bool known = false;
        for (const char* key : allowed) {
            if (entry.first == key) { known = true; break; }
        }
        if (!known) {
            throw JitThrow{std::string(name) + "(): unknown option '" + entry.first + "'", line};
        }
    }
}

inline bool tokdata_option_bool(const char* name, const GCDict* options,
                                const char* key, bool fallback, int line) {
    const Value* value = tokdata_dict_find(options, key);
    if (!value) return fallback;
    if (!value->is_bool()) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be a bool", line};
    }
    return value->as_bool();
}

inline uint64_t tokdata_safe_integer(const char* name, const Value& value,
                                     const char* label, uint64_t minimum,
                                     uint64_t maximum, int line) {
    if (!value.is_num()) {
        throw JitThrow{std::string(name) + "(): " + label + " must be a number", line};
    }
    double raw = value.as_num();
    if (!std::isfinite(raw) || raw < (double)minimum || raw > (double)maximum
        || raw != std::floor(raw)) {
        throw JitThrow{std::string(name) + "(): " + label + " must be an integer from "
                       + std::to_string(minimum) + " to " + std::to_string(maximum), line};
    }
    return (uint64_t)raw;
}

inline uint64_t tokdata_option_integer(const char* name, const GCDict* options,
                                       const char* key, uint64_t fallback,
                                       uint64_t minimum, uint64_t maximum,
                                       int line) {
    const Value* value = tokdata_dict_find(options, key);
    return value ? tokdata_safe_integer(name, *value, key, minimum, maximum, line) : fallback;
}

inline uint32_t tokdata_option_special_id(const char* name, const GCDict* options,
                                          const char* key, int line) {
    const Value* value = tokdata_dict_find(options, key);
    if (!value || value->is_nil()) return TOKDATA_NO_ID;
    return (uint32_t)tokdata_safe_integer(name, *value, key, TOKDATA_BYTE_VOCAB,
                                          TOKDATA_MAX_EXACT_FLOAT32_ID, line);
}

inline void tokdata_validate_special_ids(const char* name, uint32_t bos, uint32_t eos,
                                         uint32_t pad, int line,
                                         uint32_t minimum = TOKDATA_BYTE_VOCAB) {
    const std::array<uint32_t, 3> ids{{bos, eos, pad}};
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == TOKDATA_NO_ID) continue;
        if (ids[i] < minimum || ids[i] > TOKDATA_MAX_EXACT_FLOAT32_ID) {
            throw JitThrow{std::string(name) + "(): special token IDs must be from "
                           + std::to_string(minimum) + " through 16777215", line};
        }
        for (size_t j = i + 1; j < ids.size(); ++j) {
            if (ids[i] == ids[j]) {
                throw JitThrow{std::string(name) + "(): BOS, EOS, and PAD IDs must be distinct", line};
            }
        }
    }
}

inline uint32_t tokdata_vocab_size(uint32_t bos, uint32_t eos, uint32_t pad) {
    uint32_t maximum = TOKDATA_BYTE_VOCAB - 1;
    if (bos != TOKDATA_NO_ID) maximum = std::max(maximum, bos);
    if (eos != TOKDATA_NO_ID) maximum = std::max(maximum, eos);
    if (pad != TOKDATA_NO_ID) maximum = std::max(maximum, pad);
    return maximum + 1;
}

struct TokDataTokenizerConfig {
    bool is_bpe = false;
    uint32_t bos = TOKDATA_NO_ID;
    uint32_t eos = TOKDATA_NO_ID;
    uint32_t pad = TOKDATA_NO_ID;
    uint32_t learned_vocab_size = TOKDATA_BYTE_VOCAB;
    uint32_t vocab_size = TOKDATA_BYTE_VOCAB;
    size_t max_piece_size = 1;
    std::vector<std::pair<uint32_t, uint32_t>> merges;
    std::vector<std::string> pieces;
};

inline Value tokdata_optional_id_value(uint32_t id) {
    return id == TOKDATA_NO_ID ? Value::nil() : Value((double)id);
}

inline Value tokdata_make_tokenizer(uint32_t bos, uint32_t eos, uint32_t pad) {
    Value result = Value::make_dict();
    GCDict* dict = result.as_dict();
    dict->elements["__sura_handle"] = Value(std::string("tokenizer.byte.v1"));
    dict->elements["type"] = Value(std::string("byte"));
    dict->elements["bos_id"] = tokdata_optional_id_value(bos);
    dict->elements["eos_id"] = tokdata_optional_id_value(eos);
    dict->elements["pad_id"] = tokdata_optional_id_value(pad);
    dict->elements["vocab_size"] = Value((double)tokdata_vocab_size(bos, eos, pad));
    return result;
}

inline Value tokdata_make_bpe_tokenizer(
        const std::vector<std::pair<uint32_t, uint32_t>>& merges,
        uint32_t bos, uint32_t eos, uint32_t pad) {
    const uint32_t learned_vocab = TOKDATA_BYTE_VOCAB + (uint32_t)merges.size();
    Value result = Value::make_dict();
    GCDict* dict = result.as_dict();
    dict->elements["__sura_handle"] = Value(std::string("tokenizer.bpe.v1"));
    dict->elements["type"] = Value(std::string("bpe"));
    dict->elements["bos_id"] = tokdata_optional_id_value(bos);
    dict->elements["eos_id"] = tokdata_optional_id_value(eos);
    dict->elements["pad_id"] = tokdata_optional_id_value(pad);
    dict->elements["learned_vocab_size"] = Value((double)learned_vocab);
    dict->elements["vocab_size"] = Value((double)tokdata_vocab_size(bos, eos, pad));
    if (learned_vocab > TOKDATA_BYTE_VOCAB) {
        dict->elements["vocab_size"] = Value((double)std::max(
            learned_vocab, tokdata_vocab_size(bos, eos, pad)));
    }
    Value merge_values = Value::make_array();
    GCArray* merge_array = merge_values.as_arr();
    merge_array->elements.reserve(merges.size());
    for (const auto& merge : merges) {
        Value pair_value = Value::make_array();
        pair_value.as_arr()->elements.push_back(Value((double)merge.first));
        pair_value.as_arr()->elements.push_back(Value((double)merge.second));
        merge_array->elements.push_back(std::move(pair_value));
    }
    dict->elements["merges"] = std::move(merge_values);
    return result;
}

inline TokDataTokenizerConfig tokdata_need_tokenizer(const char* name,
                                                     const Value& value,
                                                     int line) {
    if (!value.is_dict()) {
        throw JitThrow{std::string(name) + "(): expected a tokenizer handle", line};
    }
    GCDict* dict = value.as_dict();
    const Value* kind = tokdata_dict_find(dict, "__sura_handle");
    const Value* type = tokdata_dict_find(dict, "type");
    if (!kind || !kind->is_str() || !type || !type->is_str()) {
        throw JitThrow{std::string(name) + "(): invalid tokenizer handle", line};
    }
    const bool is_byte = kind->as_str_ref() == "tokenizer.byte.v1"
                      && type->as_str_ref() == "byte";
    const bool is_bpe = kind->as_str_ref() == "tokenizer.bpe.v1"
                     && type->as_str_ref() == "bpe";
    if (!is_byte && !is_bpe) {
        throw JitThrow{std::string(name) + "(): invalid tokenizer handle", line};
    }

    auto read_id = [&](const char* key) -> uint32_t {
        const Value* id = tokdata_dict_find(dict, key);
        if (!id || id->is_nil()) return TOKDATA_NO_ID;
        return (uint32_t)tokdata_safe_integer(name, *id, key, TOKDATA_BYTE_VOCAB,
                                              TOKDATA_MAX_EXACT_FLOAT32_ID, line);
    };
    TokDataTokenizerConfig config;
    config.is_bpe = is_bpe;
    config.bos = read_id("bos_id");
    config.eos = read_id("eos_id");
    config.pad = read_id("pad_id");
    if (is_bpe) {
        const Value* merge_value = tokdata_dict_find(dict, "merges");
        if (!merge_value || !merge_value->is_arr()) {
            throw JitThrow{std::string(name) + "(): BPE tokenizer merges must be an array", line};
        }
        GCArray* merge_array = merge_value->as_arr();
        if (merge_array->elements.size() > TOKDATA_MAX_BPE_VOCAB - TOKDATA_BYTE_VOCAB) {
            throw JitThrow{std::string(name) + "(): BPE merge table exceeds the safety limit", line};
        }
        config.merges.reserve(merge_array->elements.size());
        config.pieces.reserve(TOKDATA_BYTE_VOCAB + merge_array->elements.size());
        for (uint32_t byte = 0; byte < TOKDATA_BYTE_VOCAB; ++byte) {
            config.pieces.push_back(std::string(1, (char)(unsigned char)byte));
        }
        size_t total_piece_bytes = TOKDATA_BYTE_VOCAB;
        for (size_t i = 0; i < merge_array->elements.size(); ++i) {
            const Value& pair_value = merge_array->elements[i];
            if (!pair_value.is_arr() || pair_value.as_arr()->elements.size() != 2) {
                throw JitThrow{std::string(name) + "(): each BPE merge must contain exactly two token IDs", line};
            }
            const uint32_t next_id = TOKDATA_BYTE_VOCAB + (uint32_t)i;
            const auto& pair_items = pair_value.as_arr()->elements;
            uint32_t left = (uint32_t)tokdata_safe_integer(name, pair_items[0],
                "BPE left token ID", 0, next_id - 1, line);
            uint32_t right = (uint32_t)tokdata_safe_integer(name, pair_items[1],
                "BPE right token ID", 0, next_id - 1, line);
            const size_t piece_size = config.pieces[left].size() + config.pieces[right].size();
            if (piece_size > TOKDATA_MAX_BPE_PIECE_BYTES
                || total_piece_bytes > TOKDATA_MAX_BPE_TOTAL_PIECE_BYTES - piece_size) {
                throw JitThrow{std::string(name) + "(): BPE token expansion exceeds the safety limit", line};
            }
            config.merges.push_back({left, right});
            config.pieces.push_back(config.pieces[left] + config.pieces[right]);
            config.max_piece_size = std::max(config.max_piece_size, piece_size);
            total_piece_bytes += piece_size;
        }
        config.learned_vocab_size = TOKDATA_BYTE_VOCAB + (uint32_t)config.merges.size();
        const Value* stored_learned = tokdata_dict_find(dict, "learned_vocab_size");
        if (!stored_learned || tokdata_safe_integer(name, *stored_learned,
                "learned_vocab_size", TOKDATA_BYTE_VOCAB, TOKDATA_MAX_BPE_VOCAB, line)
                != config.learned_vocab_size) {
            throw JitThrow{std::string(name) + "(): BPE learned vocabulary metadata is inconsistent", line};
        }
    }
    tokdata_validate_special_ids(name, config.bos, config.eos, config.pad, line,
                                 config.learned_vocab_size);
    config.vocab_size = std::max(config.learned_vocab_size,
                                 tokdata_vocab_size(config.bos, config.eos, config.pad));
    const Value* stored_vocab = tokdata_dict_find(dict, "vocab_size");
    if (!stored_vocab || tokdata_safe_integer(name, *stored_vocab, "vocab_size",
                                              TOKDATA_BYTE_VOCAB,
                                              TOKDATA_MAX_EXACT_FLOAT32_ID + 1ULL,
                                              line) != config.vocab_size) {
        throw JitThrow{std::string(name) + "(): tokenizer handle vocabulary metadata is inconsistent", line};
    }
    return config;
}

inline void tokdata_validate_path(const char* name, const std::string& path, int line) {
    if (path.empty() || path.size() > TOKDATA_MAX_PATH_BYTES
        || path.find('\0') != std::string::npos) {
        throw JitThrow{std::string(name) + "(): invalid path", line};
    }
}

inline std::atomic<uint64_t>& tokdata_temp_counter() {
    static std::atomic<uint64_t> counter{0};
    return counter;
}

inline std::filesystem::path tokdata_unique_temp_path(const char* name,
                                                      const std::string& path,
                                                      int line) {
    uint64_t stamp = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        uint64_t count = tokdata_temp_counter().fetch_add(1, std::memory_order_relaxed);
        std::string suffix = ".tmp." + tokdata_hex64(stamp ^ count)
                           + "." + std::to_string(attempt);
        std::filesystem::path candidate = fs_path_from_utf8(path + suffix);
        std::error_code ec;
        bool exists = std::filesystem::exists(candidate, ec);
        if (ec) {
            throw JitThrow{std::string(name) + "(): cannot inspect temporary path: " + ec.message(), line};
        }
        if (!exists) return candidate;
    }
    throw JitThrow{std::string(name) + "(): cannot allocate a temporary output path", line};
}

class TokDataTempGuard {
    std::filesystem::path path_;
    bool active_ = true;
public:
    explicit TokDataTempGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~TokDataTempGuard() {
        if (!active_) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    void release() { active_ = false; }
};

inline void tokdata_prepare_parent(const char* name, const std::filesystem::path& target,
                                   int line) {
    std::filesystem::path parent = target.parent_path();
    if (parent.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        throw JitThrow{std::string(name) + "(): cannot create parent directory: " + ec.message(), line};
    }
}

inline void tokdata_commit_file(const char* name, const std::filesystem::path& temp,
                                const std::filesystem::path& target, int line) {
    std::error_code ec;
    std::filesystem::file_status existing = std::filesystem::symlink_status(target, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        throw JitThrow{std::string(name) + "(): cannot inspect target: " + ec.message(), line};
    }
    if (!ec && existing.type() != std::filesystem::file_type::not_found) {
        if (existing.type() == std::filesystem::file_type::symlink) {
            throw JitThrow{std::string(name) + "(): refusing to replace a symlink", line};
        }
        if (existing.type() != std::filesystem::file_type::regular) {
            throw JitThrow{std::string(name) + "(): target must be a regular file", line};
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw JitThrow{std::string(name) + "(): atomic commit failed (Windows error "
                       + std::to_string((unsigned long)GetLastError()) + ")", line};
    }
#else
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        throw JitThrow{std::string(name) + "(): atomic commit failed: " + ec.message(), line};
    }
#endif
}

inline Value b_tokenizer_byte(const Value* a, int n, int l) {
    need_args("tokenizer_byte", n, 0, 1, l);
    GCDict* options = nullptr;
    if (n == 1) options = need_dict("tokenizer_byte", a[0], 0, l);
    tokdata_validate_options("tokenizer_byte", options, {"bos_id", "eos_id", "pad_id"}, l);
    uint32_t bos = tokdata_option_special_id("tokenizer_byte", options, "bos_id", l);
    uint32_t eos = tokdata_option_special_id("tokenizer_byte", options, "eos_id", l);
    uint32_t pad = tokdata_option_special_id("tokenizer_byte", options, "pad_id", l);
    tokdata_validate_special_ids("tokenizer_byte", bos, eos, pad, l);
    return tokdata_make_tokenizer(bos, eos, pad);
}

inline uint64_t tokdata_pair_key(uint32_t left, uint32_t right) {
    return (uint64_t(left) << 32) | uint64_t(right);
}

inline Value b_tokenizer_train_bpe(const Value* a, int n, int l) {
    need_args("tokenizer_train_bpe", n, 1, 2, l);
    const std::string corpus = need_str("tokenizer_train_bpe", a[0], 0, l);
    if (corpus.empty()) {
        throw JitThrow{"tokenizer_train_bpe(): corpus must not be empty", l};
    }
    if (corpus.size() > TOKDATA_MAX_BPE_TRAIN_BYTES) {
        throw JitThrow{"tokenizer_train_bpe(): corpus exceeds the 1 MiB training safety limit", l};
    }
    GCDict* options = nullptr;
    if (n == 2) options = need_dict("tokenizer_train_bpe", a[1], 1, l);
    tokdata_validate_options("tokenizer_train_bpe", options,
        {"vocab_size", "min_frequency", "bos_id", "eos_id", "pad_id"}, l);
    const uint32_t target_vocab = (uint32_t)tokdata_option_integer(
        "tokenizer_train_bpe", options, "vocab_size", 384,
        TOKDATA_BYTE_VOCAB, TOKDATA_MAX_BPE_VOCAB, l);
    const uint32_t min_frequency = (uint32_t)tokdata_option_integer(
        "tokenizer_train_bpe", options, "min_frequency", 2, 2,
        std::max<uint64_t>(2, (uint64_t)corpus.size()), l);
    const uint32_t bos = tokdata_option_special_id("tokenizer_train_bpe", options, "bos_id", l);
    const uint32_t eos = tokdata_option_special_id("tokenizer_train_bpe", options, "eos_id", l);
    const uint32_t pad = tokdata_option_special_id("tokenizer_train_bpe", options, "pad_id", l);

    std::vector<uint32_t> tokens;
    tokens.reserve(corpus.size());
    for (unsigned char byte : corpus) tokens.push_back((uint32_t)byte);
    std::vector<std::pair<uint32_t, uint32_t>> merges;
    merges.reserve(target_vocab - TOKDATA_BYTE_VOCAB);
    uint64_t work = 0;
    while (TOKDATA_BYTE_VOCAB + merges.size() < target_vocab && tokens.size() >= 2) {
        if (work > TOKDATA_MAX_BPE_TRAIN_WORK - tokens.size()) {
            throw JitThrow{"tokenizer_train_bpe(): training work exceeds the bounded safety budget; use a smaller corpus or vocabulary", l};
        }
        work += tokens.size();
        std::unordered_map<uint64_t, uint32_t> counts;
        counts.reserve(std::min(tokens.size(), (size_t)target_vocab * 8));
        for (size_t i = 1; i < tokens.size(); ++i) {
            uint64_t key = tokdata_pair_key(tokens[i - 1], tokens[i]);
            auto found = counts.find(key);
            if (found == counts.end()) counts.emplace(key, 1U);
            else if (found->second != std::numeric_limits<uint32_t>::max()) ++found->second;
        }
        uint64_t best_key = 0;
        uint32_t best_count = 0;
        for (const auto& entry : counts) {
            if (entry.second > best_count
                || (entry.second == best_count && entry.first < best_key)) {
                best_key = entry.first;
                best_count = entry.second;
            }
        }
        if (best_count < min_frequency) break;
        const uint32_t left = (uint32_t)(best_key >> 32);
        const uint32_t right = (uint32_t)best_key;
        const uint32_t merged_id = TOKDATA_BYTE_VOCAB + (uint32_t)merges.size();
        std::vector<uint32_t> next;
        next.reserve(tokens.size() - best_count);
        for (size_t i = 0; i < tokens.size();) {
            if (i + 1 < tokens.size() && tokens[i] == left && tokens[i + 1] == right) {
                next.push_back(merged_id);
                i += 2;
            } else {
                next.push_back(tokens[i]);
                ++i;
            }
        }
        merges.push_back({left, right});
        tokens.swap(next);
    }

    const uint32_t learned_vocab = TOKDATA_BYTE_VOCAB + (uint32_t)merges.size();
    tokdata_validate_special_ids("tokenizer_train_bpe", bos, eos, pad, l, learned_vocab);
    return tokdata_make_bpe_tokenizer(merges, bos, eos, pad);
}

struct TokDataBpeNode {
    uint32_t id = 0;
    int64_t previous = -1;
    int64_t next = -1;
    bool alive = true;
};

struct TokDataBpeCandidate {
    uint32_t rank = 0;
    size_t position = 0;
};

struct TokDataBpeCandidateAfter {
    bool operator()(const TokDataBpeCandidate& left,
                    const TokDataBpeCandidate& right) const {
        if (left.rank != right.rank) return left.rank > right.rank;
        return left.position > right.position;
    }
};

inline std::vector<uint32_t> tokdata_bpe_encode(
        const std::string& text, const TokDataTokenizerConfig& config) {
    std::vector<TokDataBpeNode> nodes(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        nodes[i].id = (uint32_t)(unsigned char)text[i];
        nodes[i].previous = i == 0 ? -1 : (int64_t)i - 1;
        nodes[i].next = i + 1 == text.size() ? -1 : (int64_t)i + 1;
    }
    std::unordered_map<uint64_t, uint32_t> ranks;
    ranks.reserve(config.merges.size() * 2 + 1);
    for (uint32_t rank = 0; rank < config.merges.size(); ++rank) {
        ranks.emplace(tokdata_pair_key(config.merges[rank].first,
                                       config.merges[rank].second), rank);
    }
    std::priority_queue<TokDataBpeCandidate, std::vector<TokDataBpeCandidate>,
                        TokDataBpeCandidateAfter> candidates;
    auto push_pair = [&](int64_t left_index) {
        if (left_index < 0 || (size_t)left_index >= nodes.size()) return;
        TokDataBpeNode& left = nodes[(size_t)left_index];
        if (!left.alive || left.next < 0) return;
        TokDataBpeNode& right = nodes[(size_t)left.next];
        if (!right.alive) return;
        auto found = ranks.find(tokdata_pair_key(left.id, right.id));
        if (found != ranks.end()) candidates.push({found->second, (size_t)left_index});
    };
    for (size_t i = 0; i + 1 < nodes.size(); ++i) push_pair((int64_t)i);

    while (!candidates.empty()) {
        TokDataBpeCandidate candidate = candidates.top();
        candidates.pop();
        if (candidate.position >= nodes.size()) continue;
        TokDataBpeNode& left = nodes[candidate.position];
        if (!left.alive || left.next < 0) continue;
        TokDataBpeNode& right = nodes[(size_t)left.next];
        if (!right.alive) continue;
        auto found = ranks.find(tokdata_pair_key(left.id, right.id));
        if (found == ranks.end() || found->second != candidate.rank) continue;
        left.id = TOKDATA_BYTE_VOCAB + candidate.rank;
        left.next = right.next;
        right.alive = false;
        if (left.next >= 0) nodes[(size_t)left.next].previous = (int64_t)candidate.position;
        push_pair(left.previous);
        push_pair((int64_t)candidate.position);
    }

    std::vector<uint32_t> output;
    output.reserve(text.size());
    int64_t current = text.empty() ? -1 : 0;
    while (current >= 0) {
        const TokDataBpeNode& node = nodes[(size_t)current];
        if (node.alive) output.push_back(node.id);
        current = node.next;
    }
    return output;
}

inline Value b_tokenizer_encode(const Value* a, int n, int l) {
    need_args("tokenizer_encode", n, 2, 3, l);
    TokDataTokenizerConfig config = tokdata_need_tokenizer("tokenizer_encode", a[0], l);
    const std::string text = need_str("tokenizer_encode", a[1], 1, l);
    if (text.size() > TOKDATA_MAX_DIRECT_TEXT_BYTES) {
        throw JitThrow{"tokenizer_encode(): direct text exceeds the 16 MiB safety limit; use dataset.pack_text for large files", l};
    }
    GCDict* options = nullptr;
    if (n == 3) options = need_dict("tokenizer_encode", a[2], 2, l);
    tokdata_validate_options("tokenizer_encode", options, {"add_bos", "add_eos"}, l);
    bool add_bos = tokdata_option_bool("tokenizer_encode", options, "add_bos", false, l);
    bool add_eos = tokdata_option_bool("tokenizer_encode", options, "add_eos", false, l);
    if (add_bos && config.bos == TOKDATA_NO_ID) {
        throw JitThrow{"tokenizer_encode(): add_bos requires a configured bos_id", l};
    }
    if (add_eos && config.eos == TOKDATA_NO_ID) {
        throw JitThrow{"tokenizer_encode(): add_eos requires a configured eos_id", l};
    }
    if (text.size() > std::numeric_limits<size_t>::max() - (size_t)add_bos - (size_t)add_eos) {
        throw JitThrow{"tokenizer_encode(): token count overflow", l};
    }
    std::vector<uint32_t> encoded;
    if (config.is_bpe) encoded = tokdata_bpe_encode(text, config);
    Value result = Value::make_array();
    GCArray* ids = result.as_arr();
    ids->elements.reserve((config.is_bpe ? encoded.size() : text.size())
                          + (size_t)add_bos + (size_t)add_eos);
    if (add_bos) ids->elements.push_back(Value((double)config.bos));
    if (config.is_bpe) {
        for (uint32_t id : encoded) ids->elements.push_back(Value((double)id));
    } else {
        for (unsigned char byte : text) ids->elements.push_back(Value((double)byte));
    }
    if (add_eos) ids->elements.push_back(Value((double)config.eos));
    return result;
}

inline uint32_t tokdata_decode_id(const char* name, const Value& value,
                                  const TokDataTokenizerConfig& config,
                                  int line) {
    uint64_t raw = tokdata_safe_integer(name, value, "token ID", 0,
                                        TOKDATA_MAX_EXACT_FLOAT32_ID, line);
    uint32_t id = (uint32_t)raw;
    if (id < config.learned_vocab_size
        || id == config.bos || id == config.eos || id == config.pad) {
        return id;
    }
    throw JitThrow{std::string(name) + "(): token ID is not in this tokenizer vocabulary", line};
}

inline void tokdata_append_decoded(std::string& output, uint32_t id,
                                   const TokDataTokenizerConfig& config,
                                   bool skip_special, int line) {
    if (id < config.learned_vocab_size) {
        const size_t added = config.is_bpe ? config.pieces[id].size() : 1;
        if (added > TOKDATA_MAX_DIRECT_TEXT_BYTES - output.size()) {
            throw JitThrow{"tokenizer_decode(): decoded text exceeds the 16 MiB safety limit", line};
        }
        if (config.is_bpe) output += config.pieces[id];
        else output.push_back((char)(unsigned char)id);
    } else if (!skip_special) {
        const char* marker = id == config.bos ? "<bos>"
                           : id == config.eos ? "<eos>" : "<pad>";
        const size_t added = std::char_traits<char>::length(marker);
        if (added > TOKDATA_MAX_DIRECT_TEXT_BYTES - output.size()) {
            throw JitThrow{"tokenizer_decode(): decoded text exceeds the 16 MiB safety limit", line};
        }
        output += marker;
    }
}

inline Value b_tokenizer_decode(const Value* a, int n, int l) {
    need_args("tokenizer_decode", n, 2, 3, l);
    TokDataTokenizerConfig config = tokdata_need_tokenizer("tokenizer_decode", a[0], l);
    GCDict* options = nullptr;
    if (n == 3) options = need_dict("tokenizer_decode", a[2], 2, l);
    tokdata_validate_options("tokenizer_decode", options, {"skip_special"}, l);
    bool skip_special = tokdata_option_bool("tokenizer_decode", options, "skip_special", true, l);
    std::string output;

    if (a[1].is_tensor()) {
        GCTensor* tensor = a[1].as_tensor();
        if (tensor->requires_grad) {
            throw JitThrow{"tokenizer_decode(): token ID tensor cannot require gradients", l};
        }
        ag_cuda_materialize_host(tensor, "tokenizer_decode", l);
        if (tensor->data.size() > TOKDATA_MAX_DIRECT_TEXT_BYTES) {
            throw JitThrow{"tokenizer_decode(): token input exceeds the 16 MiB safety limit", l};
        }
        output.reserve(tensor->data.size());
        for (size_t i = 0; i < tensor->data.size(); ++i) {
            double raw = tensor->data[i];
            Value item(raw);
            uint32_t id = tokdata_decode_id("tokenizer_decode", item, config, l);
            tokdata_append_decoded(output, id, config, skip_special, l);
        }
    } else if (a[1].is_arr()) {
        GCArray* ids = a[1].as_arr();
        if (ids->elements.size() > TOKDATA_MAX_DIRECT_TEXT_BYTES) {
            throw JitThrow{"tokenizer_decode(): token input exceeds the 16 MiB safety limit", l};
        }
        output.reserve(ids->elements.size());
        for (const Value& item : ids->elements) {
            if (item.is_arr()) throw JitThrow{"tokenizer_decode(): token ID array must be flat", l};
            uint32_t id = tokdata_decode_id("tokenizer_decode", item, config, l);
            tokdata_append_decoded(output, id, config, skip_special, l);
        }
    } else {
        throw JitThrow{"tokenizer_decode(): token IDs must be an array or non-gradient tensor", l};
    }
    return Value(output);
}

inline Value b_tokenizer_info(const Value* a, int n, int l) {
    need_args("tokenizer_info", n, 1, 1, l);
    TokDataTokenizerConfig config = tokdata_need_tokenizer("tokenizer_info", a[0], l);
    Value result = Value::make_dict();
    GCDict* info = result.as_dict();
    info->elements["format"] = Value(std::string(
        config.is_bpe ? "sura.tokenizer.bpe.v1" : "sura.tokenizer.byte.v1"));
    info->elements["type"] = Value(std::string(config.is_bpe ? "bpe" : "byte"));
    info->elements["base_vocab_size"] = Value((double)TOKDATA_BYTE_VOCAB);
    info->elements["learned_vocab_size"] = Value((double)config.learned_vocab_size);
    info->elements["merge_count"] = Value((double)config.merges.size());
    info->elements["vocab_size"] = Value((double)config.vocab_size);
    info->elements["bos_id"] = tokdata_optional_id_value(config.bos);
    info->elements["eos_id"] = tokdata_optional_id_value(config.eos);
    info->elements["pad_id"] = tokdata_optional_id_value(config.pad);
    info->elements["max_direct_text_bytes"] = Value((double)TOKDATA_MAX_DIRECT_TEXT_BYTES);
    info->elements["dataset_id_dtype"] = Value(std::string("float32"));
    return result;
}

inline Value b_tokenizer_save(const Value* a, int n, int l) {
    need_args("tokenizer_save", n, 2, 2, l);
    TokDataTokenizerConfig config = tokdata_need_tokenizer("tokenizer_save", a[0], l);
    std::string path = need_str("tokenizer_save", a[1], 1, l);
    tokdata_validate_path("tokenizer_save", path, l);

    std::vector<unsigned char> bytes(config.is_bpe
        ? 48 + config.merges.size() * 8
        : 40, 0);
    const std::array<unsigned char, 8> magic{{'S','U','R','A','T','O','K','1'}};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    TokDataFnv64 checksum;
    if (config.is_bpe) {
        tokdata_store_u32(bytes.data() + 8, 2U);
        tokdata_store_u32(bytes.data() + 12, 1U);
        tokdata_store_u32(bytes.data() + 16, config.bos);
        tokdata_store_u32(bytes.data() + 20, config.eos);
        tokdata_store_u32(bytes.data() + 24, config.pad);
        tokdata_store_u32(bytes.data() + 28, config.learned_vocab_size);
        tokdata_store_u32(bytes.data() + 32, (uint32_t)config.merges.size());
        tokdata_store_u32(bytes.data() + 36, 0U);
        size_t offset = 40;
        for (const auto& merge : config.merges) {
            tokdata_store_u32(bytes.data() + offset, merge.first);
            tokdata_store_u32(bytes.data() + offset + 4, merge.second);
            offset += 8;
        }
        checksum.update(bytes.data(), bytes.size() - 8);
        tokdata_store_u64(bytes.data() + bytes.size() - 8, checksum.value);
    } else {
        tokdata_store_u32(bytes.data() + 8, 1U);
        tokdata_store_u32(bytes.data() + 12, config.bos);
        tokdata_store_u32(bytes.data() + 16, config.eos);
        tokdata_store_u32(bytes.data() + 20, config.pad);
        tokdata_store_u32(bytes.data() + 24, config.vocab_size);
        tokdata_store_u32(bytes.data() + 28, 0U);
        checksum.update(bytes.data(), 32);
        tokdata_store_u64(bytes.data() + 32, checksum.value);
    }

    std::filesystem::path target = fs_path_from_utf8(path);
    tokdata_prepare_parent("tokenizer_save", target, l);
    std::filesystem::path temp = tokdata_unique_temp_path("tokenizer_save", path, l);
    TokDataTempGuard cleanup(temp);
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) throw JitThrow{"tokenizer_save(): cannot open temporary output", l};
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    out.flush();
    if (!out) throw JitThrow{"tokenizer_save(): failed writing tokenizer", l};
    out.close();
    tokdata_commit_file("tokenizer_save", temp, target, l);
    cleanup.release();
    return Value((double)bytes.size());
}

inline Value b_tokenizer_load(const Value* a, int n, int l) {
    need_args("tokenizer_load", n, 1, 1, l);
    std::string path = need_str("tokenizer_load", a[0], 0, l);
    tokdata_validate_path("tokenizer_load", path, l);
    std::filesystem::path source = fs_path_from_utf8(path);
    std::error_code ec;
    uintmax_t file_size = std::filesystem::file_size(source, ec);
    if (ec) throw JitThrow{"tokenizer_load(): cannot inspect tokenizer: " + ec.message(), l};
    const uintmax_t max_bpe_file = 48ULL
        + (uintmax_t)(TOKDATA_MAX_BPE_VOCAB - TOKDATA_BYTE_VOCAB) * 8ULL;
    if (file_size != 40 && (file_size < 48 || file_size > max_bpe_file)) {
        throw JitThrow{"tokenizer_load(): invalid tokenizer file size", l};
    }

    std::vector<unsigned char> bytes((size_t)file_size);
    std::ifstream in(source, std::ios::binary);
    if (!in) throw JitThrow{"tokenizer_load(): cannot open tokenizer", l};
    in.read((char*)bytes.data(), (std::streamsize)bytes.size());
    if ((size_t)in.gcount() != bytes.size()) throw JitThrow{"tokenizer_load(): truncated tokenizer", l};
    const std::array<unsigned char, 8> magic{{'S','U','R','A','T','O','K','1'}};
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        throw JitThrow{"tokenizer_load(): invalid tokenizer magic", l};
    }
    const uint32_t version = tokdata_load_u32(bytes.data() + 8);
    TokDataFnv64 checksum;
    if (version == 1U && bytes.size() == 40) {
        if (tokdata_load_u32(bytes.data() + 28) != 0U) {
            throw JitThrow{"tokenizer_load(): unsupported tokenizer version or flags", l};
        }
        checksum.update(bytes.data(), 32);
        if (checksum.value != tokdata_load_u64(bytes.data() + 32)) {
            throw JitThrow{"tokenizer_load(): tokenizer checksum mismatch", l};
        }
        uint32_t bos = tokdata_load_u32(bytes.data() + 12);
        uint32_t eos = tokdata_load_u32(bytes.data() + 16);
        uint32_t pad = tokdata_load_u32(bytes.data() + 20);
        tokdata_validate_special_ids("tokenizer_load", bos, eos, pad, l);
        uint32_t vocab = tokdata_vocab_size(bos, eos, pad);
        if (tokdata_load_u32(bytes.data() + 24) != vocab) {
            throw JitThrow{"tokenizer_load(): inconsistent tokenizer vocabulary metadata", l};
        }
        return tokdata_make_tokenizer(bos, eos, pad);
    }
    if (version != 2U || bytes.size() < 48 || tokdata_load_u32(bytes.data() + 12) != 1U
        || tokdata_load_u32(bytes.data() + 36) != 0U) {
        throw JitThrow{"tokenizer_load(): unsupported tokenizer version, kind, or flags", l};
    }
    const uint32_t merge_count = tokdata_load_u32(bytes.data() + 32);
    if (merge_count > TOKDATA_MAX_BPE_VOCAB - TOKDATA_BYTE_VOCAB
        || bytes.size() != 48ULL + (size_t)merge_count * 8ULL) {
        throw JitThrow{"tokenizer_load(): inconsistent BPE merge metadata", l};
    }
    checksum.update(bytes.data(), bytes.size() - 8);
    if (checksum.value != tokdata_load_u64(bytes.data() + bytes.size() - 8)) {
        throw JitThrow{"tokenizer_load(): tokenizer checksum mismatch", l};
    }
    const uint32_t learned_vocab = TOKDATA_BYTE_VOCAB + merge_count;
    if (tokdata_load_u32(bytes.data() + 28) != learned_vocab) {
        throw JitThrow{"tokenizer_load(): inconsistent BPE vocabulary metadata", l};
    }
    const uint32_t bos = tokdata_load_u32(bytes.data() + 16);
    const uint32_t eos = tokdata_load_u32(bytes.data() + 20);
    const uint32_t pad = tokdata_load_u32(bytes.data() + 24);
    tokdata_validate_special_ids("tokenizer_load", bos, eos, pad, l, learned_vocab);
    std::vector<std::pair<uint32_t, uint32_t>> merges;
    merges.reserve(merge_count);
    size_t offset = 40;
    for (uint32_t i = 0; i < merge_count; ++i) {
        const uint32_t next_id = TOKDATA_BYTE_VOCAB + i;
        uint32_t left = tokdata_load_u32(bytes.data() + offset);
        uint32_t right = tokdata_load_u32(bytes.data() + offset + 4);
        if (left >= next_id || right >= next_id) {
            throw JitThrow{"tokenizer_load(): BPE merge references an unavailable token ID", l};
        }
        merges.push_back({left, right});
        offset += 8;
    }
    Value result = tokdata_make_bpe_tokenizer(merges, bos, eos, pad);
    (void)tokdata_need_tokenizer("tokenizer_load", result, l);
    return result;
}

} // namespace SuraStd
