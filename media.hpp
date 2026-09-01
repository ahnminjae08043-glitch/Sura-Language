#pragma once

// Native text-video helpers.
//
// Frame-to-text conversion is dependency-free and deterministic. Video
// decoding intentionally delegates only the codec/container boundary to an
// FFmpeg executable, then parses a bounded stream of binary PGM frames and
// performs all luminance-to-glyph conversion inside Sura. This keeps codec
// support broad without loading untrusted codec plugins into the language
// process.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#endif

namespace SuraStd {

static constexpr size_t MEDIA_MAX_PATH_BYTES = 8192;
static constexpr size_t MEDIA_MAX_FRAME_PIXELS = 4ULL * 1024ULL * 1024ULL;
static constexpr size_t MEDIA_MAX_OUTPUT_BYTES = 64ULL * 1024ULL * 1024ULL;
static constexpr uint64_t MEDIA_MAX_DECODE_FILE_BYTES = 512ULL * 1024ULL * 1024ULL;
static constexpr uint64_t MEDIA_MAX_FRAMES = 10000;

inline const Value* media_dict_find(const GCDict* dict, const char* key) {
    if (!dict) return nullptr;
    auto found = dict->elements.find(key);
    return found == dict->elements.end() ? nullptr : &found->second;
}

inline void media_validate_options(const char* name, const GCDict* options,
                                   std::initializer_list<const char*> allowed,
                                   int line) {
    if (!options) return;
    for (const auto& entry : options->elements) {
        bool known = false;
        for (const char* key : allowed) {
            if (entry.first == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw JitThrow{std::string(name) + "(): unknown option '" + entry.first + "'", line};
        }
    }
}

inline bool media_option_bool(const char* name, const GCDict* options,
                              const char* key, bool fallback, int line) {
    const Value* value = media_dict_find(options, key);
    if (!value) return fallback;
    if (!value->is_bool()) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be a bool", line};
    }
    return value->as_bool();
}

inline double media_option_number(const char* name, const GCDict* options,
                                  const char* key, double fallback,
                                  double minimum, double maximum, int line) {
    const Value* value = media_dict_find(options, key);
    if (!value) return fallback;
    if (!value->is_num()) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be a number", line};
    }
    double number = value->as_num();
    if (!std::isfinite(number) || number < minimum || number > maximum) {
        std::ostringstream range;
        range.imbue(std::locale::classic());
        range << minimum << " to " << maximum;
        throw JitThrow{std::string(name) + "(): option " + key
                       + " must be from " + range.str(), line};
    }
    return number;
}

inline uint64_t media_option_integer(const char* name, const GCDict* options,
                                     const char* key, uint64_t fallback,
                                     uint64_t minimum, uint64_t maximum,
                                     int line) {
    const Value* value = media_dict_find(options, key);
    if (!value) return fallback;
    if (!value->is_num()) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be a number", line};
    }
    double number = value->as_num();
    if (!std::isfinite(number) || number != std::floor(number)
        || number < (double)minimum || number > (double)maximum) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be an integer from "
                       + std::to_string(minimum) + " to " + std::to_string(maximum), line};
    }
    return (uint64_t)number;
}

inline std::string media_option_string(const char* name, const GCDict* options,
                                       const char* key, const std::string& fallback,
                                       int line) {
    const Value* value = media_dict_find(options, key);
    if (!value) return fallback;
    if (!value->is_str()) {
        throw JitThrow{std::string(name) + "(): option " + key + " must be a string", line};
    }
    return value->as_str();
}

inline bool media_utf8_continuation(unsigned char byte) {
    return (byte & 0xc0U) == 0x80U;
}

inline std::vector<std::string> media_split_glyphs(const char* name,
                                                   const std::string& text,
                                                   int line) {
    std::vector<std::string> glyphs;
    for (size_t offset = 0; offset < text.size();) {
        unsigned char first = (unsigned char)text[offset];
        size_t width = 0;
        if (first <= 0x7fU) {
            if (first < 0x20U || first == 0x7fU) {
                throw JitThrow{std::string(name)
                               + "(): charset must not contain control characters", line};
            }
            width = 1;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            width = 2;
        } else if (first >= 0xe0U && first <= 0xefU) {
            width = 3;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            width = 4;
        } else {
            throw JitThrow{std::string(name) + "(): charset is not valid UTF-8", line};
        }
        if (offset + width > text.size()) {
            throw JitThrow{std::string(name) + "(): charset is not valid UTF-8", line};
        }
        for (size_t index = 1; index < width; ++index) {
            if (!media_utf8_continuation((unsigned char)text[offset + index])) {
                throw JitThrow{std::string(name) + "(): charset is not valid UTF-8", line};
            }
        }
        if (width == 3) {
            unsigned char second = (unsigned char)text[offset + 1];
            if ((first == 0xe0U && second < 0xa0U)
                || (first == 0xedU && second >= 0xa0U)) {
                throw JitThrow{std::string(name) + "(): charset is not valid UTF-8", line};
            }
        } else if (width == 4) {
            unsigned char second = (unsigned char)text[offset + 1];
            if ((first == 0xf0U && second < 0x90U)
                || (first == 0xf4U && second > 0x8fU)) {
                throw JitThrow{std::string(name) + "(): charset is not valid UTF-8", line};
            }
        }
        glyphs.push_back(text.substr(offset, width));
        offset += width;
        if (glyphs.size() > 256) {
            throw JitThrow{std::string(name) + "(): charset may contain at most 256 glyphs", line};
        }
    }
    if (glyphs.size() < 2) {
        throw JitThrow{std::string(name) + "(): charset must contain at least 2 glyphs", line};
    }
    return glyphs;
}

struct MediaTextStyle {
    std::string charset = " .:-=+*#%@";
    std::vector<std::string> glyphs;
    double gamma = 1.0;
    bool invert = false;
    bool dither = false;
};

inline MediaTextStyle media_text_style(const char* name, const GCDict* options,
                                       int line) {
    MediaTextStyle style;
    style.charset = media_option_string(name, options, "charset", style.charset, line);
    style.glyphs = media_split_glyphs(name, style.charset, line);
    style.gamma = media_option_number(name, options, "gamma", 1.0, 0.1, 5.0, line);
    style.invert = media_option_bool(name, options, "invert", false, line);
    style.dither = media_option_bool(name, options, "dither", false, line);
    return style;
}

inline double media_channel(const char* name, const Value& value,
                            const char* label, int line) {
    if (!value.is_num() || !std::isfinite(value.as_num())
        || value.as_num() < 0.0 || value.as_num() > 255.0) {
        throw JitThrow{std::string(name) + "(): " + label
                       + " channels must be finite numbers from 0 to 255", line};
    }
    return value.as_num();
}

inline double media_pixel_luma(const char* name, const Value& pixel, int line) {
    if (pixel.is_num()) return media_channel(name, pixel, "grayscale", line);
    if (!pixel.is_arr()) {
        throw JitThrow{std::string(name)
                       + "(): each pixel must be grayscale or an RGB/RGBA array", line};
    }
    const auto& channels = pixel.as_arr()->elements;
    if (channels.size() != 3 && channels.size() != 4) {
        throw JitThrow{std::string(name)
                       + "(): RGB pixels need 3 channels and RGBA pixels need 4", line};
    }
    double red = media_channel(name, channels[0], "RGB", line);
    double green = media_channel(name, channels[1], "RGB", line);
    double blue = media_channel(name, channels[2], "RGB", line);
    double alpha = channels.size() == 4
        ? media_channel(name, channels[3], "alpha", line) / 255.0 : 1.0;
    return (0.2126 * red + 0.7152 * green + 0.0722 * blue) * alpha;
}

inline std::string media_render_luma(const char* name,
                                     const std::vector<double>& luminance,
                                     size_t width, size_t height,
                                     const MediaTextStyle& style,
                                     int line) {
    if (width == 0 || height == 0 || luminance.size() != width * height) {
        throw JitThrow{std::string(name) + "(): internal frame shape is invalid", line};
    }
    size_t widest_glyph = 1;
    for (const auto& glyph : style.glyphs) widest_glyph = std::max(widest_glyph, glyph.size());
    if (width > (MEDIA_MAX_OUTPUT_BYTES - height) / widest_glyph
        || height > MEDIA_MAX_OUTPUT_BYTES / (width * widest_glyph + 1)) {
        throw JitThrow{std::string(name) + "(): rendered frame exceeds the output safety limit", line};
    }

    std::vector<double> corrected(luminance.size(), 0.0);
    for (size_t index = 0; index < luminance.size(); ++index) {
        double value = std::clamp(luminance[index] / 255.0, 0.0, 1.0);
        corrected[index] = std::pow(value, style.gamma);
    }

    std::string output;
    output.reserve(height * (width * widest_glyph + 1));
    const size_t levels = style.glyphs.size() - 1;
    for (size_t row = 0; row < height; ++row) {
        for (size_t column = 0; column < width; ++column) {
            size_t index = row * width + column;
            double value = std::clamp(corrected[index], 0.0, 1.0);
            size_t level = (size_t)std::floor(value * (double)levels + 0.5);
            if (level > levels) level = levels;
            size_t glyph = style.invert ? levels - level : level;
            output += style.glyphs[glyph];

            if (style.dither) {
                double error = value - (double)level / (double)levels;
                auto spread = [&](size_t target, double weight) {
                    corrected[target] = std::clamp(corrected[target] + error * weight,
                                                   0.0, 1.0);
                };
                if (column + 1 < width) spread(index + 1, 7.0 / 16.0);
                if (row + 1 < height) {
                    if (column > 0) spread(index + width - 1, 3.0 / 16.0);
                    spread(index + width, 5.0 / 16.0);
                    if (column + 1 < width) spread(index + width + 1, 1.0 / 16.0);
                }
            }
        }
        if (row + 1 < height) output.push_back('\n');
    }
    return output;
}

inline Value b_media_frame_to_text(const Value* a, int n, int l) {
    need_args("media_frame_to_text", n, 1, 2, l);
    auto* rows = need_arr("media_frame_to_text", a[0], 0, l);
    GCDict* options = n == 2 ? need_dict("media_frame_to_text", a[1], 1, l) : nullptr;
    media_validate_options("media_frame_to_text", options,
                           {"charset", "gamma", "invert", "dither"}, l);
    if (rows->elements.empty()) {
        throw JitThrow{"media_frame_to_text(): frame must contain at least one row", l};
    }
    if (!rows->elements[0].is_arr() || rows->elements[0].as_arr()->elements.empty()) {
        throw JitThrow{"media_frame_to_text(): frame rows must be non-empty arrays", l};
    }
    size_t height = rows->elements.size();
    size_t width = rows->elements[0].as_arr()->elements.size();
    if (height > MEDIA_MAX_FRAME_PIXELS / width) {
        throw JitThrow{"media_frame_to_text(): frame exceeds the pixel safety limit", l};
    }
    std::vector<double> luminance;
    luminance.reserve(width * height);
    for (const auto& row_value : rows->elements) {
        if (!row_value.is_arr() || row_value.as_arr()->elements.size() != width) {
            throw JitThrow{"media_frame_to_text(): frame rows must have equal width", l};
        }
        for (const auto& pixel : row_value.as_arr()->elements) {
            luminance.push_back(media_pixel_luma("media_frame_to_text", pixel, l));
        }
    }
    MediaTextStyle style = media_text_style("media_frame_to_text", options, l);
    return Value(media_render_luma("media_frame_to_text", luminance,
                                   width, height, style, l));
}

inline std::string media_resolve_ffmpeg(const std::string& requested) {
    std::string resolved = find_command_on_path(requested.empty() ? "ffmpeg" : requested);
#ifdef _WIN32
    if (!resolved.empty()) {
        std::string extension = fs_path_from_utf8(resolved).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return (char)std::tolower(ch); });
        if (extension != ".exe" && extension != ".com") return "";
    }
#endif
    return resolved;
}

inline Value b_media_ffmpeg_available(const Value* a, int n, int l) {
    need_args("media_ffmpeg_available", n, 0, 1, l);
    std::string requested;
    if (n == 1) requested = need_str("media_ffmpeg_available", a[0], 0, l);
    if (requested.empty()) {
        const char* configured = std::getenv("SURA_FFMPEG");
        if (configured && *configured) requested = configured;
    }
    if (requested.find_first_of("\r\n") != std::string::npos
        || requested.find('\0') != std::string::npos
        || requested.size() > MEDIA_MAX_PATH_BYTES) {
        return Value(false);
    }
    return Value(!media_resolve_ffmpeg(requested).empty());
}

struct MediaCommandResult {
    int exit_code = -1;
    std::string output;
    bool timed_out = false;
    bool output_limit_exceeded = false;
};

inline bool media_output_over_limit(const std::filesystem::path& path,
                                    uint64_t limit) {
    std::error_code ec;
    uint64_t bytes = std::filesystem::file_size(path, ec);
    return !ec && bytes > limit;
}

#ifdef _WIN32
inline std::wstring media_windows_quote_argument(const std::wstring& argument) {
    if (!argument.empty()
        && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

inline MediaCommandResult media_run_process(const std::vector<std::string>& arguments,
                                            uint32_t timeout_ms,
                                            const std::filesystem::path& output_path,
                                            uint64_t output_limit) {
    MediaCommandResult result;
    if (arguments.empty()) {
        result.output = "empty process argument list";
        return result;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE error_read = nullptr;
    HANDLE error_write = nullptr;
    if (!CreatePipe(&error_read, &error_write, &attributes, 0)
        || !SetHandleInformation(error_read, HANDLE_FLAG_INHERIT, 0)) {
        if (error_read) CloseHandle(error_read);
        if (error_write) CloseHandle(error_write);
        result.output = "cannot create the decoder stderr pipe";
        return result;
    }
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &attributes, OPEN_EXISTING, 0, nullptr);
    HANDLE null_output = CreateFileW(L"NUL", GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &attributes, OPEN_EXISTING, 0, nullptr);
    if (null_input == INVALID_HANDLE_VALUE || null_output == INVALID_HANDLE_VALUE) {
        if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
        if (null_output != INVALID_HANDLE_VALUE) CloseHandle(null_output);
        CloseHandle(error_read);
        CloseHandle(error_write);
        result.output = "cannot open the null device for the decoder";
        return result;
    }

    std::wstring application = windows_path_bytes_to_wide(arguments[0]);
    std::wstring command_line;
    for (const auto& argument : arguments) {
        if (!command_line.empty()) command_line.push_back(L' ');
        command_line += media_windows_quote_argument(
            windows_path_bytes_to_wide(argument));
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input;
    startup.hStdOutput = null_output;
    startup.hStdError = error_write;
    PROCESS_INFORMATION process{};
    BOOL created = CreateProcessW(
        application.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
    CloseHandle(error_write);
    CloseHandle(null_input);
    CloseHandle(null_output);
    if (!created) {
        result.output = "CreateProcessW failed with error "
                      + std::to_string((unsigned long)GetLastError());
        CloseHandle(error_read);
        return result;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))
            || !AssignProcessToJobObject(job, process.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    if (ResumeThread(process.hThread) == (DWORD)-1) {
        if (job) TerminateJobObject(job, 126);
        else TerminateProcess(process.hProcess, 126);
        WaitForSingleObject(process.hProcess, INFINITE);
        if (job) CloseHandle(job);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(error_read);
        result.output = "cannot resume the decoder process";
        return result;
    }

    std::thread reader([&]() {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(error_read, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            if (result.output.size() < 65536) {
                size_t available = 65536 - result.output.size();
                result.output.append(buffer, std::min<size_t>(available, read));
            }
        }
        CloseHandle(error_read);
    });

    auto terminate_tree = [&](unsigned code) {
        if (job) TerminateJobObject(job, code);
        else TerminateProcess(process.hProcess, code);
    };
    DWORD wait_error = 0;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (true) {
        DWORD waited = WaitForSingleObject(process.hProcess, 10);
        if (waited == WAIT_OBJECT_0) break;
        if (waited == WAIT_FAILED) {
            wait_error = GetLastError();
            terminate_tree(125);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
        if (media_output_over_limit(output_path, output_limit)) {
            result.output_limit_exceeded = true;
            terminate_tree(127);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            terminate_tree(124);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
    }
    if (media_output_over_limit(output_path, output_limit)) {
        result.output_limit_exceeded = true;
    }
    DWORD exit_code = (DWORD)-1;
    if (GetExitCodeProcess(process.hProcess, &exit_code)) {
        result.exit_code = (int)exit_code;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job) {
        TerminateJobObject(job, 0);
        CloseHandle(job);
    }
    reader.join();
    if (wait_error != 0) {
        result.output += " decoder wait failed with error "
                       + std::to_string((unsigned long)wait_error);
    }
    return result;
}
#else
inline MediaCommandResult media_run_process(const std::vector<std::string>& arguments,
                                            uint32_t timeout_ms,
                                            const std::filesystem::path& output_path,
                                            uint64_t output_limit) {
    MediaCommandResult result;
    if (arguments.empty()) {
        result.output = "empty process argument list";
        return result;
    }
    int error_pipe[2] = {-1, -1};
    if (pipe(error_pipe) != 0) {
        result.output = "cannot create the decoder stderr pipe";
        return result;
    }
    std::vector<char*> raw_arguments;
    raw_arguments.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        raw_arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    raw_arguments.push_back(nullptr);

    pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        dup2(error_pipe[1], STDERR_FILENO);
        close(error_pipe[0]);
        close(error_pipe[1]);
        execv(arguments[0].c_str(), raw_arguments.data());
        _exit(127);
    }
    close(error_pipe[1]);
    if (child < 0) {
        close(error_pipe[0]);
        result.output = "fork failed while starting the decoder";
        return result;
    }
    setpgid(child, child);

    std::thread reader([&]() {
        char buffer[4096];
        while (true) {
            ssize_t count = read(error_pipe[0], buffer, sizeof(buffer));
            if (count > 0) {
                if (result.output.size() < 65536) {
                    size_t available = 65536 - result.output.size();
                    result.output.append(buffer,
                                         std::min<size_t>(available, (size_t)count));
                }
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            break;
        }
        close(error_pipe[0]);
    });

    int status = 0;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (true) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) break;
        if (media_output_over_limit(output_path, output_limit)) {
            result.output_limit_exceeded = true;
            kill(-child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            kill(-child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(-child, SIGKILL);
    reader.join();
    if (media_output_over_limit(output_path, output_limit)) {
        result.output_limit_exceeded = true;
    }
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    return result;
}
#endif

inline std::filesystem::path media_unique_temp_path(int line) {
    static std::atomic<uint64_t> sequence{0};
    std::error_code ec;
    std::filesystem::path root = std::filesystem::temp_directory_path(ec);
    if (ec) throw JitThrow{"media_video_to_text(): cannot locate the temporary directory", line};
    uint64_t stamp = (uint64_t)std::chrono::high_resolution_clock::now()
                         .time_since_epoch().count();
    for (uint64_t attempt = 0; attempt < 128; ++attempt) {
        uint64_t id = sequence.fetch_add(1, std::memory_order_relaxed);
        std::filesystem::path directory = root /
            ("sura_text_video_" + std::to_string(stamp) + "_"
             + std::to_string(id) + "_" + std::to_string(attempt));
        bool created = std::filesystem::create_directory(directory, ec);
        if (created) {
#ifndef _WIN32
            std::filesystem::permissions(
                directory, std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace, ec);
            if (ec) {
                std::error_code ignored;
                std::filesystem::remove(directory, ignored);
                throw JitThrow{"media_video_to_text(): cannot protect the temporary directory", line};
            }
#endif
            return directory / "frames.pgmstream";
        }
        if (ec && ec != std::errc::file_exists) {
            throw JitThrow{"media_video_to_text(): cannot create a temporary directory", line};
        }
        ec.clear();
    }
    throw JitThrow{"media_video_to_text(): cannot create a unique temporary path", line};
}

struct MediaTempFile {
    std::filesystem::path path;
    explicit MediaTempFile(std::filesystem::path value) : path(std::move(value)) {}
    ~MediaTempFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.parent_path(), ignored);
    }
};

inline std::string media_decimal(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(12) << value;
    return out.str();
}

inline std::string media_clean_error(std::string text) {
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
    if (text.size() > 2048) text = text.substr(0, 2045) + "...";
    return text;
}

inline bool media_read_pgm_token(std::istream& input, std::string& token,
                                 bool allow_clean_eof,
                                 const char* name, int line) {
    token.clear();
    while (true) {
        int next = input.peek();
        if (next == EOF) {
            if (allow_clean_eof) return false;
            throw JitThrow{std::string(name) + "(): decoded PGM header is truncated", line};
        }
        unsigned char byte = (unsigned char)next;
        if (std::isspace(byte)) {
            input.get();
            continue;
        }
        if (byte == '#') {
            input.get();
            std::string ignored;
            std::getline(input, ignored);
            if (!input && !input.eof()) {
                throw JitThrow{std::string(name) + "(): cannot read a PGM comment", line};
            }
            continue;
        }
        break;
    }
    while (true) {
        int raw = input.get();
        if (raw == EOF) break;
        unsigned char byte = (unsigned char)raw;
        if (std::isspace(byte)) {
            if (byte == '\r' && input.peek() == '\n') input.get();
            break;
        }
        if (token.size() >= 64) {
            throw JitThrow{std::string(name) + "(): decoded PGM token is too long", line};
        }
        token.push_back((char)byte);
    }
    if (token.empty()) {
        throw JitThrow{std::string(name) + "(): decoded PGM header is malformed", line};
    }
    return true;
}

inline uint64_t media_parse_uint(const std::string& text, const char* label,
                                 const char* name, int line) {
    uint64_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end) {
        throw JitThrow{std::string(name) + "(): decoded PGM " + label
                       + " is invalid", line};
    }
    return value;
}

inline std::vector<double> media_resample_height(const std::vector<unsigned char>& pixels,
                                                 size_t width, size_t source_height,
                                                 size_t target_height) {
    std::vector<double> result(width * target_height, 0.0);
    if (target_height == source_height) {
        for (size_t index = 0; index < pixels.size(); ++index) result[index] = pixels[index];
        return result;
    }
    for (size_t row = 0; row < target_height; ++row) {
        double source_y = ((double)row + 0.5) * (double)source_height
                        / (double)target_height - 0.5;
        source_y = std::clamp(source_y, 0.0, (double)(source_height - 1));
        size_t low = (size_t)std::floor(source_y);
        size_t high = std::min(low + 1, source_height - 1);
        double fraction = source_y - (double)low;
        for (size_t column = 0; column < width; ++column) {
            double first = pixels[low * width + column];
            double second = pixels[high * width + column];
            result[row * width + column] = first + (second - first) * fraction;
        }
    }
    return result;
}

inline Value b_media_video_to_text(const Value* a, int n, int l) {
    need_args("media_video_to_text", n, 1, 2, l);
    std::string source_text = need_str("media_video_to_text", a[0], 0, l);
    GCDict* options = n == 2 ? need_dict("media_video_to_text", a[1], 1, l) : nullptr;
    media_validate_options(
        "media_video_to_text", options,
        {"width", "height", "fps", "max_frames", "start", "duration",
         "char_aspect", "charset", "gamma", "invert", "dither", "ffmpeg",
         "timeout_ms"}, l);

    if (source_text.empty() || source_text.size() > MEDIA_MAX_PATH_BYTES
        || source_text.find_first_of("\r\n") != std::string::npos
        || source_text.find('\0') != std::string::npos) {
        throw JitThrow{"media_video_to_text(): source path is invalid or too long", l};
    }
    std::filesystem::path source = fs_path_from_utf8(source_text);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec) || ec) {
        throw JitThrow{"media_video_to_text(): source must be an existing regular file", l};
    }
    std::filesystem::path absolute_source = std::filesystem::absolute(source, ec);
    if (ec) absolute_source = source;
    absolute_source = absolute_source.lexically_normal();

    uint64_t width = media_option_integer("media_video_to_text", options,
                                          "width", 80, 1, 512, l);
    const bool explicit_height = media_dict_find(options, "height") != nullptr;
    uint64_t height = explicit_height
        ? media_option_integer("media_video_to_text", options, "height", 24, 1, 512, l)
        : 0;
    double fps = media_option_number("media_video_to_text", options,
                                     "fps", 8.0, 0.1, 60.0, l);
    uint64_t max_frames = media_option_integer("media_video_to_text", options,
                                               "max_frames", 300, 1,
                                               MEDIA_MAX_FRAMES, l);
    uint64_t timeout_ms = media_option_integer("media_video_to_text", options,
                                               "timeout_ms", 120000, 1000,
                                               600000, l);
    double start = media_option_number("media_video_to_text", options,
                                       "start", 0.0, 0.0, 86400.0, l);
    const bool has_duration = media_dict_find(options, "duration") != nullptr;
    double duration = has_duration
        ? media_option_number("media_video_to_text", options,
                              "duration", 1.0, 0.001, 86400.0, l)
        : 0.0;
    const bool has_char_aspect = media_dict_find(options, "char_aspect") != nullptr;
    if (explicit_height && has_char_aspect) {
        throw JitThrow{"media_video_to_text(): height and char_aspect are mutually exclusive", l};
    }
    double char_aspect = media_option_number("media_video_to_text", options,
                                             "char_aspect", 0.5, 0.1, 2.0, l);
    uint64_t decoder_height_limit = explicit_height ? height : 512;
    uint64_t maximum_frame_bytes = width * decoder_height_limit + 1024;
    if (max_frames + 1 > MEDIA_MAX_DECODE_FILE_BYTES / maximum_frame_bytes) {
        throw JitThrow{"media_video_to_text(): width/height/max_frames can exceed the "
                       "decoded-stream safety limit; reduce the requested size", l};
    }
    MediaTextStyle style = media_text_style("media_video_to_text", options, l);

    std::string requested_ffmpeg = media_option_string(
        "media_video_to_text", options, "ffmpeg", "", l);
    if (requested_ffmpeg.empty()) {
        const char* configured = std::getenv("SURA_FFMPEG");
        if (configured && *configured) requested_ffmpeg = configured;
    }
    if (requested_ffmpeg.size() > MEDIA_MAX_PATH_BYTES
        || requested_ffmpeg.find_first_of("\r\n") != std::string::npos
        || requested_ffmpeg.find('\0') != std::string::npos) {
        throw JitThrow{"media_video_to_text(): ffmpeg path is invalid or too long", l};
    }
    std::string ffmpeg = media_resolve_ffmpeg(requested_ffmpeg);
    if (ffmpeg.empty()) {
        throw JitThrow{
            "media_video_to_text(): ffmpeg was not found on PATH; install FFmpeg "
            "or pass {ffmpeg: \"path/to/ffmpeg\"}", l};
    }

    std::filesystem::path temp_path = media_unique_temp_path(l);
    MediaTempFile cleanup(temp_path);
    std::string filter = "fps=" + media_decimal(fps) + ",scale="
        + std::to_string(width) + ":";
    if (explicit_height) {
        filter += std::to_string(height)
            + ":force_original_aspect_ratio=decrease:flags=area,pad="
            + std::to_string(width) + ":" + std::to_string(height)
            + ":(ow-iw)/2:(oh-ih)/2:color=black";
    } else {
        filter += "min(512\\,max(1\\,round(ih*" + std::to_string(width)
               + "/iw))):flags=area";
    }

    std::vector<std::string> command = {
        ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y"
    };
    if (start > 0.0) {
        command.push_back("-ss");
        command.push_back(media_decimal(start));
    }
    command.push_back("-protocol_whitelist");
    command.push_back("file,pipe");
    command.push_back("-i");
    command.push_back(fs_path_to_utf8(absolute_source));
    if (has_duration) {
        command.push_back("-t");
        command.push_back(media_decimal(duration));
    }
    command.insert(command.end(), {
        "-map", "0:v:0", "-vf", filter,
        "-frames:v", std::to_string(max_frames + 1),
        "-an", "-sn", "-dn", "-pix_fmt", "gray",
        "-f", "image2pipe", "-vcodec", "pgm",
        fs_path_to_utf8(temp_path)
    });

    MediaCommandResult decoded = media_run_process(
        command, (uint32_t)timeout_ms, temp_path,
        MEDIA_MAX_DECODE_FILE_BYTES);
    if (decoded.timed_out) {
        throw JitThrow{"media_video_to_text(): ffmpeg exceeded timeout_ms", l};
    }
    if (decoded.output_limit_exceeded) {
        throw JitThrow{"media_video_to_text(): decoded frame stream exceeds the safety limit", l};
    }
    if (decoded.exit_code != 0) {
        std::string detail = media_clean_error(decoded.output);
        std::string message = "media_video_to_text(): ffmpeg failed with exit code "
                            + std::to_string(decoded.exit_code);
        if (!detail.empty()) message += ": " + detail;
        throw JitThrow{message, l};
    }

    uint64_t file_bytes = std::filesystem::file_size(temp_path, ec);
    if (ec || file_bytes == 0) {
        throw JitThrow{"media_video_to_text(): decoder produced no video frames", l};
    }
    if (file_bytes > MEDIA_MAX_DECODE_FILE_BYTES) {
        throw JitThrow{"media_video_to_text(): decoded frame stream exceeds the safety limit", l};
    }
    std::ifstream input(temp_path, std::ios::binary);
    if (!input) {
        throw JitThrow{"media_video_to_text(): cannot open the decoded frame stream", l};
    }

    Value frames = Value::make_array();
    Value timestamps = Value::make_array();
    size_t output_bytes = 0;
    size_t rendered_width = 0;
    size_t rendered_height = 0;
    size_t source_frame_height = 0;
    bool truncated = false;
    size_t decoded_frames = 0;
    std::string token;
    while (media_read_pgm_token(input, token, true, "media_video_to_text", l)) {
        if (token != "P5") {
            throw JitThrow{"media_video_to_text(): decoder output is not binary PGM", l};
        }
        media_read_pgm_token(input, token, false, "media_video_to_text", l);
        uint64_t frame_width = media_parse_uint(token, "width", "media_video_to_text", l);
        media_read_pgm_token(input, token, false, "media_video_to_text", l);
        uint64_t frame_height = media_parse_uint(token, "height", "media_video_to_text", l);
        media_read_pgm_token(input, token, false, "media_video_to_text", l);
        uint64_t maximum = media_parse_uint(token, "maximum", "media_video_to_text", l);
        if (frame_width == 0 || frame_height == 0
            || frame_width > MEDIA_MAX_FRAME_PIXELS / frame_height) {
            throw JitThrow{"media_video_to_text(): decoded frame exceeds the pixel safety limit", l};
        }
        if (maximum == 0 || maximum > 255) {
            throw JitThrow{"media_video_to_text(): decoder must emit 8-bit PGM frames", l};
        }
        if (frame_width != width
            || (explicit_height && frame_height != height)) {
            throw JitThrow{"media_video_to_text(): decoder returned unexpected frame dimensions", l};
        }
        size_t count = (size_t)(frame_width * frame_height);
        std::vector<unsigned char> pixels(count);
        input.read((char*)pixels.data(), (std::streamsize)count);
        if ((size_t)input.gcount() != count) {
            throw JitThrow{"media_video_to_text(): decoded PGM pixel payload is truncated", l};
        }
        if (maximum != 255) {
            for (unsigned char& pixel : pixels) {
                pixel = (unsigned char)std::lround((double)pixel * 255.0 / (double)maximum);
            }
        }
        ++decoded_frames;
        if (decoded_frames > max_frames + 1) {
            throw JitThrow{"media_video_to_text(): decoder exceeded its frame contract", l};
        }
        if (rendered_width != 0
            && (rendered_width != (size_t)frame_width
                || source_frame_height != (size_t)frame_height)) {
            throw JitThrow{"media_video_to_text(): decoded frame dimensions changed mid-stream", l};
        }
        rendered_width = (size_t)frame_width;
        source_frame_height = (size_t)frame_height;
        rendered_height = explicit_height
            ? source_frame_height
            : std::max<size_t>(1, (size_t)std::llround(
                  (double)source_frame_height * char_aspect));
        if (rendered_height > 512) {
            throw JitThrow{"media_video_to_text(): automatic text height exceeds 512; "
                           "pass an explicit height", l};
        }
        if (frames.as_arr()->elements.size() >= max_frames) {
            truncated = true;
            continue;
        }
        std::vector<double> luminance = media_resample_height(
            pixels, rendered_width, source_frame_height, rendered_height);
        std::string text = media_render_luma(
            "media_video_to_text", luminance, rendered_width,
            rendered_height, style, l);
        if (text.size() > MEDIA_MAX_OUTPUT_BYTES - output_bytes) {
            throw JitThrow{"media_video_to_text(): text frames exceed the output safety limit", l};
        }
        output_bytes += text.size();
        size_t frame_index = frames.as_arr()->elements.size();
        frames.as_arr()->elements.push_back(Value(std::move(text)));
        timestamps.as_arr()->elements.push_back(Value(start + (double)frame_index / fps));
    }
    if (frames.as_arr()->elements.empty()) {
        throw JitThrow{"media_video_to_text(): decoder produced no readable video frames", l};
    }

    Value result = Value::make_dict();
    auto* dict = result.as_dict();
    dict->elements["schema"] = Value(std::string("sura.media.ascii-frames.v1"));
    dict->elements["format"] = Value(std::string("sura.text-video.v1"));
    dict->elements["source"] = Value(fs_path_to_utf8(absolute_source));
    dict->elements["decoder"] = Value(ffmpeg);
    dict->elements["width"] = Value((double)rendered_width);
    dict->elements["height"] = Value((double)rendered_height);
    dict->elements["source_frame_height"] = Value((double)source_frame_height);
    dict->elements["fps"] = Value(fps);
    dict->elements["frame_count"] = Value((double)frames.as_arr()->elements.size());
    dict->elements["sampled_duration"] = Value((double)frames.as_arr()->elements.size() / fps);
    dict->elements["frame_duration_ms"] = Value(1000.0 / fps);
    dict->elements["charset"] = Value(style.charset);
    dict->elements["gamma"] = Value(style.gamma);
    dict->elements["inverted"] = Value(style.invert);
    dict->elements["dithered"] = Value(style.dither);
    dict->elements["truncated"] = Value(truncated);
    dict->elements["backend"] = Value(std::string("ffmpeg-pgm"));
    dict->elements["frames"] = frames;
    dict->elements["timestamps"] = timestamps;
    return result;
}

inline Value b_media_video_text_frames(const Value* a, int n, int l) {
    return b_media_video_to_text(a, n, l);
}

} // namespace SuraStd
