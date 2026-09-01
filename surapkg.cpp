#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <cstdio>
#include <unordered_set>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#else
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int);
extern "C" __declspec(dllimport) wchar_t* __stdcall GetCommandLineW(void);
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableW(const wchar_t*, wchar_t*, unsigned long);
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void*, wchar_t*, unsigned long);
extern "C" __declspec(dllimport) unsigned long __stdcall GetFileAttributesW(const wchar_t*);
extern "C" __declspec(dllimport) int __stdcall MultiByteToWideChar(
    unsigned int, unsigned long, const char*, int, wchar_t*, int);
extern "C" __declspec(dllimport) int __stdcall WideCharToMultiByte(
    unsigned int, unsigned long, const wchar_t*, int, char*, int, const char*, int*);
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
struct ConsoleUtf8 {
    ConsoleUtf8() {
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
    }
} console_utf8;
#endif

#ifdef _WIN32
static std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return L"";
    int needed = MultiByteToWideChar(65001, 0, text.c_str(), -1, nullptr, 0);
    if (needed <= 1) return std::wstring(text.begin(), text.end());
    std::wstring out((size_t)needed, L'\0');
    MultiByteToWideChar(65001, 0, text.c_str(), -1, out.data(), needed);
    out.resize((size_t)needed - 1);
    return out;
}

static std::string wide_to_utf8(const wchar_t* text) {
    if (!text) return "";
    int needed = WideCharToMultiByte(65001, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return "";
    std::string out((size_t)needed, '\0');
    WideCharToMultiByte(65001, 0, text, -1, out.data(), needed, nullptr, nullptr);
    out.resize((size_t)needed - 1);
    return out;
}

static std::string getenv_utf8(const wchar_t* name) {
    wchar_t stack_buf[32768];
    unsigned long needed = GetEnvironmentVariableW(name, stack_buf, 32768);
    if (needed == 0) return "";
    if (needed < 32768) return wide_to_utf8(stack_buf);
    std::wstring buf((size_t)needed, L'\0');
    unsigned long got = GetEnvironmentVariableW(name, buf.data(), needed);
    if (got == 0) return "";
    return wide_to_utf8(buf.c_str());
}

static std::vector<std::wstring> split_windows_command_line(const wchar_t* command_line) {
    std::vector<std::wstring> args;
    const wchar_t* p = command_line;
    while (p && *p) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;

        std::wstring arg;
        bool in_quotes = false;
        int backslashes = 0;

        while (*p) {
            wchar_t ch = *p++;
            if (ch == L'\\') {
                ++backslashes;
                continue;
            }
            if (ch == L'"') {
                arg.append((size_t)(backslashes / 2), L'\\');
                if ((backslashes % 2) == 0) {
                    in_quotes = !in_quotes;
                } else {
                    arg.push_back(L'"');
                }
                backslashes = 0;
                continue;
            }
            if (!in_quotes && (ch == L' ' || ch == L'\t')) {
                arg.append((size_t)backslashes, L'\\');
                backslashes = 0;
                break;
            }
            arg.append((size_t)backslashes, L'\\');
            backslashes = 0;
            arg.push_back(ch);
        }

        arg.append((size_t)backslashes, L'\\');
        args.push_back(std::move(arg));
    }
    return args;
}
#endif

static std::vector<std::string> command_line_args(int argc, char* argv[]) {
#ifdef _WIN32
    std::vector<std::string> converted;
    for (const auto& arg : split_windows_command_line(GetCommandLineW())) {
        converted.push_back(wide_to_utf8(arg.c_str()));
    }
    if (!converted.empty()) return converted;
#endif

    std::vector<std::string> out;
    out.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) out.push_back(argv[i] ? argv[i] : "");
    return out;
}

static fs::path utf8_path(const std::string& path) {
#ifdef _WIN32
    return fs::u8path(path);
#else
    return fs::path(path);
#endif
}

static std::string path_to_utf8(const fs::path& path) {
#ifdef _WIN32
    auto text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.string();
#endif
}

static std::string path_to_generic_utf8(const fs::path& path) {
#ifdef _WIN32
    auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
#else
    return path.generic_string();
#endif
}

static const fs::path kManifest = "sura.pkg.json";
static const fs::path kLockfile = "sura.lock.json";
static const fs::path kPackages = "packages";
static const fs::path kStdlib = "stdlib";
static const fs::path kSignature = "sura.pkg.sig";
static const fs::path kToolPolicyManifest = "sura.tools.json";
static const fs::path kToolPolicySignature = "sura.tools.sig";
static const fs::path kPluginPolicyManifest = "sura.plugins.json";

static void ok(const std::string& msg) { std::cout << "[OK] " << msg << "\n"; }
static void info(const std::string& msg) { std::cout << "[info] " << msg << "\n"; }
static int err(const std::string& msg) { std::cerr << "[error] " << msg << "\n"; return 1; }
static std::string shell_quote(const std::string& value);

static std::string read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool write_all(const fs::path& path, const std::string& text) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return (bool)out;
}

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '"') out += "\\\"";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else out += ch;
    }
    return out;
}

static std::string html_escape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '&') out += "&amp;";
        else if (ch == '<') out += "&lt;";
        else if (ch == '>') out += "&gt;";
        else if (ch == '"') out += "&quot;";
        else if (ch == '\'') out += "&#39;";
        else out += ch;
    }
    return out;
}

static std::string markdown_escape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '|') out += "\\|";
        else if (ch == '`') out += "\\`";
        else if (ch == '\n' || ch == '\r') out += " ";
        else out += ch;
    }
    return out;
}

static std::string json_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        char ch = s[++i];
        if (ch == 'n') out.push_back('\n');
        else if (ch == 'r') out.push_back('\r');
        else if (ch == 't') out.push_back('\t');
        else out.push_back(ch);
    }
    return out;
}

static std::string normalize_name(std::string name) {
    for (char& ch : name) {
        if (ch == '-' || ch == ' ') ch = '_';
    }
    return name;
}

static std::string artifact_safe(std::string text, const std::string& fallback) {
    for (char& ch : text) {
        unsigned char c = (unsigned char)ch;
        if (!(std::isalnum(c) || ch == '_' || ch == '-' || ch == '.')) ch = '_';
    }
    while (!text.empty() && (text.front() == '.' || text.front() == '_')) text.erase(text.begin());
    while (!text.empty() && (text.back() == '.' || text.back() == '_')) text.pop_back();
    return text.empty() ? fallback : text;
}

static std::string default_manifest(const std::string& name) {
    return "{\n"
           "  \"name\": \"" + json_escape(name) + "\",\n"
           "  \"version\": \"0.1.0\",\n"
           "  \"main\": \"src/" + json_escape(name) + ".sura\",\n"
           "  \"dependencies\": {}\n"
           "}\n";
}

static fs::path registry_root() {
    const char* env = std::getenv("SURA_REGISTRY");
    if (env && *env) return fs::path(env);
    return "registry";
}

static std::string registry_url() {
    const char* env = std::getenv("SURA_REGISTRY_URL");
    if (!env || !*env) return "";
    std::string url = env;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

static std::string registry_token() {
    const char* env = std::getenv("SURA_REGISTRY_TOKEN");
    if (!env || !*env) return "";
    return env;
}

static std::string query_escape(const std::string& text) {
    std::ostringstream out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.') {
            out << (char)c;
        } else {
            out << '%' << hex[(c >> 4) & 0xF] << hex[c & 0xF];
        }
    }
    return out.str();
}

static std::string signing_key() {
    const char* env = std::getenv("SURA_SIGNING_KEY");
    if (!env || !*env) return "";
    return env;
}

static std::string signing_key_id() {
    const char* env = std::getenv("SURA_SIGNING_KEY_ID");
    if (!env || !*env) return "local";
    return env;
}

static std::string public_signing_private_key() {
    const char* env = std::getenv("SURA_SIGNING_PRIVATE_KEY");
    if (!env || !*env) return "";
    return env;
}

static std::string public_signing_public_key() {
    const char* env = std::getenv("SURA_SIGNING_PUBLIC_KEY");
    if (!env || !*env) return "";
    return env;
}

static fs::path trusted_public_key_dir() {
    const char* env = std::getenv("SURA_SIGNING_PUBLIC_KEY_DIR");
    if (!env || !*env) return {};
    return fs::path(env);
}

static bool require_public_signature() {
    const char* env = std::getenv("SURA_REQUIRE_PUBLIC_SIGNATURE");
    if (!env || !*env) return false;
    std::string value = env;
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value != "0" && value != "false" && value != "no" && value != "off";
}

static int run_capture_command_status(const std::string& command, std::string& out) {
    out.clear();
#ifdef _WIN32
    std::wstring wide_command = utf8_to_wide(command);
    FILE* pipe = _wpopen(wide_command.c_str(), L"r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return -1;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
#ifdef _WIN32
    return _pclose(pipe);
#else
    int status = pclose(pipe);
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return status;
#endif
}

static int run_system_command_status(const std::string& command) {
#ifdef _WIN32
    std::wstring wide_command = utf8_to_wide(command);
    return _wsystem(wide_command.c_str());
#else
    return std::system(command.c_str());
#endif
}

static std::string run_capture_command(const std::string& command) {
    std::string out;
    run_capture_command_status(command, out);
    return out;
}

static std::string http_get_text(const std::string& url) {
    if (url.find_first_of("\"\r\n") != std::string::npos) return "";
    return run_capture_command("curl -L -s --fail --max-time 20 -- \"" + url + "\"");
}

static bool http_post_file(const std::string& url, const fs::path& file, const std::string& token, std::string& response) {
    if (url.find_first_of("\"\r\n") != std::string::npos ||
        token.find_first_of("\"\r\n") != std::string::npos ||
        file.string().find_first_of("\"\r\n") != std::string::npos) {
        return false;
    }
    std::string marker = "__SURA_CURL_OK__";
    std::string cmd =
        "curl -L -s --fail --max-time 30 -X POST "
        "-H \"Authorization: Bearer " + token + "\" "
        "-H \"Content-Type: application/json\" "
        "--data-binary @\"" + file.string() + "\" "
        "-- \"" + url + "\" && echo " + marker;
    response = run_capture_command(cmd);
    return response.find(marker) != std::string::npos;
}

static std::string strip_marker(std::string response, const std::string& marker) {
    size_t pos = response.find(marker);
    if (pos != std::string::npos) response.erase(pos);
    while (!response.empty() && std::isspace((unsigned char)response.back())) response.pop_back();
    return response;
}

static bool http_post_json(const std::string& url, const std::string& json, const std::string& token, std::string& response) {
    if (url.find_first_of("\"\r\n") != std::string::npos ||
        token.find_first_of("\"\r\n") != std::string::npos) {
        return false;
    }
    fs::path tmp = fs::temp_directory_path() /
        ("sura_http_post_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    if (!write_all(tmp, json)) return false;
    std::string marker = "__SURA_CURL_OK__";
    std::string cmd =
        "curl -L -s --fail --max-time 30 -X POST "
        "-H \"Content-Type: application/json\" ";
    if (!token.empty()) cmd += "-H \"Authorization: Bearer " + token + "\" ";
    cmd += "--data-binary @\"" + tmp.string() + "\" "
           "-- \"" + url + "\" && echo " + marker;
    response = run_capture_command(cmd);
    std::error_code ec;
    fs::remove(tmp, ec);
    bool ok = response.find(marker) != std::string::npos;
    if (ok) response = strip_marker(response, marker);
    return ok;
}

static bool http_get_json(const std::string& url, const std::string& token, std::string& response) {
    response.clear();
    if (url.find_first_of("\"\r\n") != std::string::npos ||
        token.find_first_of("\"\r\n") != std::string::npos) {
        return false;
    }
    std::string cmd = "curl -L -s --fail --max-time 20 ";
    if (!token.empty()) cmd += "-H \"Authorization: Bearer " + token + "\" ";
    cmd += "-- \"" + url + "\"";
    response = run_capture_command(cmd);
    return !response.empty();
}

struct PackageRef {
    std::string name;
    std::string version;
};

static PackageRef parse_package_ref(const std::string& ref) {
    PackageRef out;
    size_t at = ref.rfind('@');
    if (at != std::string::npos && at > 0) {
        out.name = normalize_name(ref.substr(0, at));
        out.version = ref.substr(at + 1);
    } else {
        out.name = normalize_name(ref);
        out.version = "latest";
    }
    return out;
}

static std::string manifest_field(const std::string& manifest, const std::string& field, const std::string& fallback = "") {
    std::regex re("\"" + field + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (std::regex_search(manifest, m, re)) return m[1].str();
    return fallback;
}

static bool manifest_has_field(const std::string& manifest, const std::string& field) {
    return std::regex_search(manifest, std::regex("\"" + field + "\"\\s*:"));
}

static std::string manifest_string_field(const std::string& manifest, const std::string& field,
                                         const std::string& fallback = "", bool* found = nullptr) {
    std::regex re("\"" + field + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
    std::smatch m;
    if (std::regex_search(manifest, m, re)) {
        if (found) *found = true;
        return json_unescape(m[1].str());
    }
    if (found) *found = false;
    return fallback;
}

static uint64_t fnv1a_update(uint64_t h, const std::string& text) {
    for (unsigned char ch : text) {
        h ^= ch;
        h *= 1099511628211ULL;
    }
    return h;
}

static std::string hex64(uint64_t v) {
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << v;
    return ss.str();
}

static std::string sha256_file(const fs::path& file) {
    std::string path = file.string();
    if (path.find_first_of("\"\r\n") != std::string::npos) return "";
#ifdef _WIN32
    std::string cmd = "certutil -hashfile \"" + path + "\" SHA256";
#else
    std::string cmd = "sha256sum \"" + path + "\" 2>/dev/null || shasum -a 256 \"" + path + "\"";
#endif
    std::string raw = run_capture_command(cmd);
    std::regex hex_re("([A-Fa-f0-9]{64})");
    std::smatch m;
    if (!std::regex_search(raw, m, hex_re)) return "";
    std::string h = m[1].str();
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return h;
}

static std::string sha256_text(const std::string& text) {
    fs::path tmp = fs::temp_directory_path() /
        ("sura_sha256_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    if (!write_all(tmp, text)) return "";
    std::string hash = sha256_file(tmp);
    std::error_code ec;
    fs::remove(tmp, ec);
    return hash;
}

static fs::path crypto_temp_path(const std::string& prefix, const std::string& ext) {
    std::random_device rd;
    return fs::temp_directory_path() /
        (prefix + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(rd()) + ext);
}

static bool shell_path_safe(const std::string& value) {
    return !value.empty() && value.find_first_of("\"\r\n") == std::string::npos;
}

static std::string public_key_filename(const std::string& key_id) {
    std::string raw = key_id.empty() ? "local" : key_id;
    std::string out;
    for (unsigned char ch : raw) {
        if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.') out.push_back((char)ch);
        else out.push_back('_');
    }
    if (out.empty() || out == "." || out == "..") out = "local";
    if (out.size() > 96) out = out.substr(0, 96);
    return out + ".pem";
}

static bool looks_like_public_key(const std::string& text) {
    return text.find("BEGIN PUBLIC KEY") != std::string::npos ||
           text.find("BEGIN RSA PUBLIC KEY") != std::string::npos;
}

static std::string openssl_executable() {
    const char* env = std::getenv("SURA_OPENSSL");
    if (!env || !*env) return "openssl";
    return env;
}

static bool openssl_command_token(std::string& token, std::string& message) {
    std::string exe = openssl_executable();
    if (exe == "openssl") {
        token = "openssl";
        return true;
    }
    if (!shell_path_safe(exe)) {
        message = "unsafe SURA_OPENSSL path";
        return false;
    }
    token = shell_quote(exe);
    return true;
}

static std::string compact_command_output(std::string text) {
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    text = text.substr(start, end - start);
    if (text.size() > 240) text = text.substr(0, 240) + "...";
    return text;
}

static std::string base64_encode(const std::string& bytes) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0;
    int valb = -6;
    for (unsigned char c : bytes) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool base64_decode(const std::string& text, std::string& bytes) {
    std::string out;
    int val = 0;
    int valb = -8;
    bool saw_padding = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) continue;
        if (c == '=') {
            saw_padding = true;
            continue;
        }
        if (saw_padding) return false;
        int d = base64_value(c);
        if (d < 0) return false;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back((char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    bytes = out;
    return true;
}

static std::string package_public_signature_payload(const std::string& name,
                                                    const std::string& version,
                                                    const std::string& hash) {
    return "sura-package-sign-v2\n" + name + "\n" + version + "\n" + hash + "\n";
}

static std::string tool_policy_public_signature_payload(const std::string& hash) {
    return "sura-tool-policy-sign-v2\n" + hash + "\n";
}

static bool openssl_sign_payload(const std::string& payload,
                                 const std::string& private_key,
                                 std::string& signature_b64,
                                 std::string& message) {
    signature_b64.clear();
    message.clear();
    if (!shell_path_safe(private_key)) {
        message = "unsafe SURA_SIGNING_PRIVATE_KEY path";
        return false;
    }
    std::string openssl;
    if (!openssl_command_token(openssl, message)) return false;

    fs::path payload_path = crypto_temp_path("sura_sign_payload", ".txt");
    fs::path sig_path = crypto_temp_path("sura_sign_sig", ".bin");
    if (!write_all(payload_path, payload)) {
        message = "failed to write temporary signing payload";
        return false;
    }

    std::string output;
    std::string cmd = openssl + " dgst -sha256 -sign " + shell_quote(private_key) +
                      " -out " + shell_quote(sig_path.string()) + " " +
                      shell_quote(payload_path.string()) + " 2>&1";
    int code = run_capture_command_status(cmd, output);
    std::string sig_bytes = code == 0 ? read_all(sig_path) : "";

    std::error_code ec;
    fs::remove(payload_path, ec);
    fs::remove(sig_path, ec);

    if (code != 0 || sig_bytes.empty()) {
        message = "openssl public-key signing failed";
        std::string detail = compact_command_output(output);
        if (!detail.empty()) message += ": " + detail;
        return false;
    }
    signature_b64 = base64_encode(sig_bytes);
    return true;
}

static bool openssl_verify_payload(const std::string& payload,
                                   const std::string& public_key,
                                   const std::string& signature_b64,
                                   std::string& message) {
    message.clear();
    if (!shell_path_safe(public_key)) {
        message = "unsafe SURA_SIGNING_PUBLIC_KEY path";
        return false;
    }
    std::string sig_bytes;
    if (!base64_decode(signature_b64, sig_bytes) || sig_bytes.empty()) {
        message = "invalid base64 public-key signature";
        return false;
    }
    std::string openssl;
    if (!openssl_command_token(openssl, message)) return false;

    fs::path payload_path = crypto_temp_path("sura_verify_payload", ".txt");
    fs::path sig_path = crypto_temp_path("sura_verify_sig", ".bin");
    if (!write_all(payload_path, payload) || !write_all(sig_path, sig_bytes)) {
        message = "failed to write temporary verification payload";
        std::error_code ec;
        fs::remove(payload_path, ec);
        fs::remove(sig_path, ec);
        return false;
    }

    std::string output;
    std::string cmd = openssl + " dgst -sha256 -verify " + shell_quote(public_key) +
                      " -signature " + shell_quote(sig_path.string()) + " " +
                      shell_quote(payload_path.string()) + " 2>&1";
    int code = run_capture_command_status(cmd, output);

    std::error_code ec;
    fs::remove(payload_path, ec);
    fs::remove(sig_path, ec);

    if (code != 0) {
        message = compact_command_output(output);
        if (message.empty()) message = "openssl public-key verification failed";
        return false;
    }
    return true;
}

static void cleanup_temp_paths(const std::vector<fs::path>& paths) {
    std::error_code ec;
    for (const auto& path : paths) fs::remove(path, ec);
}

static std::string resolve_public_key_path(const std::string& key_id,
                                           const fs::path& registry_key_dir,
                                           const std::string& remote_key_base,
                                           std::vector<fs::path>& temp_paths,
                                           std::string& source) {
    source.clear();
    std::string explicit_key = public_signing_public_key();
    if (!explicit_key.empty()) {
        source = "SURA_SIGNING_PUBLIC_KEY";
        return explicit_key;
    }

    std::string filename = public_key_filename(key_id);
    std::vector<fs::path> dirs;
    fs::path env_dir = trusted_public_key_dir();
    if (!env_dir.empty()) dirs.push_back(env_dir);
    if (!registry_key_dir.empty()) dirs.push_back(registry_key_dir);
    for (const auto& dir : dirs) {
        fs::path candidate = dir / filename;
        if (fs::is_regular_file(candidate) && looks_like_public_key(read_all(candidate))) {
            source = candidate.generic_string();
            return candidate.string();
        }
    }

    if (!remote_key_base.empty()) {
        std::string base = remote_key_base;
        while (!base.empty() && base.back() == '/') base.pop_back();
        std::string key_url = base + "/" + filename;
        std::string key_text = http_get_text(key_url);
        if (looks_like_public_key(key_text)) {
            fs::path tmp = crypto_temp_path("sura_trusted_public_key", ".pem");
            if (write_all(tmp, key_text)) {
                temp_paths.push_back(tmp);
                source = key_url;
                return tmp.string();
            }
        }
    }
    return "";
}

static bool package_hash_ignored(const fs::path& file) {
    fs::path name = file.filename();
    return name == "package.surabundle.json" || name == kSignature;
}

static std::string file_content_hash(const fs::path& file, const std::string& label) {
    std::string body = read_all(file);
    std::ostringstream canonical;
    canonical << label << "\n"
              << "path:" << file.filename().generic_string() << "\n"
              << "size:" << body.size() << "\n"
              << body << "\n";
    std::string sha = sha256_text(canonical.str());
    if (!sha.empty()) return sha;
    uint64_t h = 1469598103934665603ULL;
    h = fnv1a_update(h, canonical.str());
    return hex64(h);
}

static std::string canonical_package_text(const fs::path& root) {
    std::ostringstream canonical;
    canonical << "sura-package-sha256-v1\n";
    if (fs::is_regular_file(root)) {
        std::string body = read_all(root);
        canonical << "path:" << root.filename().generic_string() << "\n"
                  << "size:" << body.size() << "\n"
                  << body << "\n";
        return canonical.str();
    }
    if (!fs::exists(root)) return canonical.str();

    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && !package_hash_ignored(entry.path())) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        std::string body = read_all(file);
        fs::path rel = fs::relative(file, root);
        canonical << "path:" << rel.generic_string() << "\n"
                  << "size:" << body.size() << "\n"
                  << body << "\n";
    }
    return canonical.str();
}

static std::string package_hash(const fs::path& root) {
    std::string canonical = canonical_package_text(root);
    std::string sha = sha256_text(canonical);
    if (!sha.empty()) return sha;

    uint64_t h = 1469598103934665603ULL;
    if (fs::is_regular_file(root)) return hex64(fnv1a_update(h, read_all(root)));
    if (!fs::exists(root)) return hex64(h);
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && !package_hash_ignored(entry.path())) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        h = fnv1a_update(h, fs::relative(file, root).generic_string());
        h = fnv1a_update(h, read_all(file));
    }
    return hex64(h);
}

static std::string package_signature(const std::string& name, const std::string& version, const std::string& hash) {
    std::string key = signing_key();
    if (key.empty()) return hash;
    std::string signed_text = "sura-sign-v1\n" + key + "\n" + name + "\n" + version + "\n" + hash + "\n";
    std::string sig = sha256_text(signed_text);
    return sig.empty() ? hash : sig;
}

static bool write_package_signature(const fs::path& root) {
    if (!fs::is_directory(root)) return false;
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return false;
    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "0.1.0");
    std::string hash = package_hash(root);
    std::string key = signing_key();
    std::string private_key = public_signing_private_key();
    std::string algorithm = "sha256-canonical-v1";
    std::string key_id = key.empty() ? "unsigned-integrity" : signing_key_id();
    std::string signature = package_signature(name, version, hash);
    if (!private_key.empty()) {
        std::string message;
        algorithm = "rsa-sha256-v2";
        key_id = signing_key_id();
        if (!openssl_sign_payload(package_public_signature_payload(name, version, hash),
                                  private_key, signature, message)) {
            if (!message.empty()) std::cerr << "[error] " << message << "\n";
            return false;
        }
    }
    std::ostringstream out;
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"name\": \"" << json_escape(name) << "\",\n"
        << "  \"packageVersion\": \"" << json_escape(version) << "\",\n"
        << "  \"algorithm\": \"" << json_escape(algorithm) << "\",\n"
        << "  \"hash\": \"" << json_escape(hash) << "\",\n"
        << "  \"keyId\": \"" << json_escape(key_id) << "\",\n"
        << "  \"signature\": \"" << json_escape(signature) << "\"\n"
        << "}\n";
    return write_all(root / kSignature, out.str());
}

static std::string tool_policy_signature(const std::string& hash) {
    std::string key = signing_key();
    if (key.empty()) return hash;
    std::string signed_text = "sura-tool-policy-sign-v1\n" + key + "\n" + hash + "\n";
    std::string sig = sha256_text(signed_text);
    return sig.empty() ? hash : sig;
}

static fs::path tool_policy_manifest_path(const std::string& source) {
    fs::path root = source.empty() ? fs::current_path() : fs::path(source);
    if (fs::is_regular_file(root) && root.filename() == kToolPolicyManifest) return root;
    if (fs::is_regular_file(root) && root.filename() == kToolPolicySignature) return root.parent_path() / kToolPolicyManifest;
    if (fs::is_regular_file(root)) return root.parent_path() / kToolPolicyManifest;
    return root / kToolPolicyManifest;
}

static bool write_tool_policy_signature(const fs::path& manifest_path) {
    if (!fs::is_regular_file(manifest_path)) return false;
    std::string hash = file_content_hash(manifest_path, "sura-tool-policy-sha256-v1");
    std::string key = signing_key();
    std::string private_key = public_signing_private_key();
    std::string algorithm = "sha256-tool-policy-v1";
    std::string key_id = key.empty() ? "unsigned-integrity" : signing_key_id();
    std::string signature = tool_policy_signature(hash);
    if (!private_key.empty()) {
        std::string message;
        algorithm = "rsa-sha256-tool-policy-v2";
        key_id = signing_key_id();
        if (!openssl_sign_payload(tool_policy_public_signature_payload(hash),
                                  private_key, signature, message)) {
            if (!message.empty()) std::cerr << "[error] " << message << "\n";
            return false;
        }
    }
    std::ostringstream out;
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"source\": \"" << json_escape(kToolPolicyManifest.generic_string()) << "\",\n"
        << "  \"algorithm\": \"" << json_escape(algorithm) << "\",\n"
        << "  \"hash\": \"" << json_escape(hash) << "\",\n"
        << "  \"keyId\": \"" << json_escape(key_id) << "\",\n"
        << "  \"signature\": \"" << json_escape(signature) << "\"\n"
        << "}\n";
    return write_all(manifest_path.parent_path() / kToolPolicySignature, out.str());
}

static bool write_package_bundle(const fs::path& root, const fs::path& bundle_path) {
    if (!fs::is_directory(root)) return false;
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return false;
    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "0.1.0");

    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            if (entry.path().filename() == "package.surabundle.json") continue;
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::ostringstream out;
    out << "{\n"
        << "  \"name\": \"" << json_escape(name) << "\",\n"
        << "  \"version\": \"" << json_escape(version) << "\",\n"
        << "  \"files\": [\n";
    for (size_t i = 0; i < files.size(); ++i) {
        fs::path rel = fs::relative(files[i], root);
        out << "    {\"path\":\"" << json_escape(rel.generic_string())
            << "\",\"content\":\"" << json_escape(read_all(files[i])) << "\"}"
            << (i + 1 == files.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return write_all(bundle_path, out.str());
}

static bool extract_package_bundle(const std::string& bundle, const fs::path& dst) {
    if (bundle.empty()) return false;
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    if (ec) return false;

    std::regex file_re("\\{\"path\":\"((?:\\\\.|[^\"])*)\",\"content\":\"((?:\\\\.|[^\"])*)\"\\}");
    bool wrote = false;
    for (auto it = std::sregex_iterator(bundle.begin(), bundle.end(), file_re);
         it != std::sregex_iterator(); ++it) {
        fs::path rel = fs::path(json_unescape((*it)[1].str())).lexically_normal();
        if (rel.is_absolute()) return false;
        std::string rel_text = rel.generic_string();
        if (rel_text == ".." || rel_text.rfind("../", 0) == 0 || rel_text.find("/../") != std::string::npos) return false;
        if (!write_all(dst / rel, json_unescape((*it)[2].str()))) return false;
        wrote = true;
    }
    return wrote && fs::exists(dst / kManifest);
}

static bool package_bundle_file_content(const std::string& bundle, const std::string& path, std::string& content) {
    std::regex file_re("\\{\"path\":\"((?:\\\\.|[^\"])*)\",\"content\":\"((?:\\\\.|[^\"])*)\"\\}");
    for (auto it = std::sregex_iterator(bundle.begin(), bundle.end(), file_re);
         it != std::sregex_iterator(); ++it) {
        std::string rel = fs::path(json_unescape((*it)[1].str())).lexically_normal().generic_string();
        if (rel == path) {
            content = json_unescape((*it)[2].str());
            return true;
        }
    }
    return false;
}

static std::string registry_bundle_url(const PackageRef& ref) {
    std::string base = registry_url();
    if (base.empty()) return "";
    return base + "/" + ref.name + "/" + (ref.version.empty() ? "latest" : ref.version) + "/package.surabundle.json";
}

static std::string registry_package_detail_url(const PackageRef& ref) {
    std::string base = registry_url();
    if (base.empty()) return "";
    return base + "/api/package/" + ref.name + "/" + (ref.version.empty() ? "latest" : ref.version);
}

struct RegistryYankRecord {
    std::string name;
    std::string version;
    std::string reason;
    std::string by;
    std::string at;
};

struct RegistryOwnerRecord {
    std::string name;
    std::string owner;
    std::string created_at;
    std::string updated_at;
};

static std::map<std::string, RegistryOwnerRecord> parse_registry_owners(const std::string& json) {
    std::map<std::string, RegistryOwnerRecord> out;
    std::regex obj_re("\"((?:\\\\.|[^\"])*)\"\\s*:\\s*\\{([^{}]*)\\}");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), obj_re);
         it != std::sregex_iterator(); ++it) {
        std::string key = json_unescape((*it)[1].str());
        if (key == "packages") continue;
        std::string object = it->str();
        RegistryOwnerRecord record;
        record.name = normalize_name(key);
        record.owner = manifest_field(object, "owner", "");
        record.created_at = manifest_field(object, "createdAt", "");
        record.updated_at = manifest_field(object, "updatedAt", "");
        if (!record.name.empty() && !record.owner.empty()) out[record.name] = record;
    }
    return out;
}

static std::string registry_yank_key(const std::string& name, const std::string& version) {
    return normalize_name(name) + "@" + version;
}

static std::map<std::string, RegistryYankRecord> parse_registry_yanks(const std::string& json) {
    std::map<std::string, RegistryYankRecord> out;
    std::regex obj_re("\"((?:\\\\.|[^\"])*)\"\\s*:\\s*\\{([^{}]*)\\}");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), obj_re);
         it != std::sregex_iterator(); ++it) {
        std::string key = json_unescape((*it)[1].str());
        if (key == "yanked" || key.find('@') == std::string::npos) continue;
        std::string object = it->str();
        PackageRef ref = parse_package_ref(key);
        if (ref.name.empty() || ref.version.empty() || ref.version == "latest") continue;
        RegistryYankRecord record;
        record.name = normalize_name(manifest_field(object, "name", ref.name));
        record.version = manifest_field(object, "version", ref.version);
        record.reason = manifest_field(object, "reason", "");
        record.by = manifest_field(object, "by", "");
        record.at = manifest_field(object, "at", "");
        out[registry_yank_key(record.name, record.version)] = record;
    }
    return out;
}

static int write_registry_index() {
    fs::path root = registry_root();
    fs::create_directories(root);
    auto yanks = parse_registry_yanks(read_all(root / "yanks.json"));
    std::ostringstream out;
    out << "{\n  \"packages\": [\n";
    bool first = true;
    if (fs::exists(root)) {
        std::vector<fs::path> packages;
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_directory()) packages.push_back(entry.path());
        }
        std::sort(packages.begin(), packages.end());
        for (const auto& pkg : packages) {
            std::vector<fs::path> versions;
            for (const auto& entry : fs::directory_iterator(pkg)) {
                if (entry.is_directory() && entry.path().filename() != "latest") versions.push_back(entry.path());
            }
            std::sort(versions.begin(), versions.end());
            for (const auto& version_dir : versions) {
                std::string manifest = read_all(version_dir / kManifest);
                std::string name = normalize_name(manifest_field(manifest, "name", pkg.filename().string()));
                std::string version = manifest_field(manifest, "version", version_dir.filename().string());
                if (!first) out << ",\n";
                first = false;
                auto yank_it = yanks.find(registry_yank_key(name, version));
                out << "    {\"name\":\"" << json_escape(name)
                    << "\",\"version\":\"" << json_escape(version)
                    << "\",\"bundle\":\"" << json_escape((name + "/" + version + "/package.surabundle.json"))
                    << "\",\"hash\":\"" << package_hash(version_dir) << "\"";
                if (yank_it != yanks.end()) {
                    out << ",\"yanked\":true"
                        << ",\"yankReason\":\"" << json_escape(yank_it->second.reason) << "\""
                        << ",\"yankedAt\":\"" << json_escape(yank_it->second.at) << "\"";
                }
                out << "}";
            }
        }
    }
    out << "\n  ]\n}\n";
    return write_all(root / "index.json", out.str()) ? 0 : 1;
}

struct RegistryPackage {
    std::string name;
    std::string version;
    std::string bundle;
    std::string hash;
    std::string owner;
    bool yanked = false;
};

static bool json_bool_field_near(const std::string& object, const std::string& field, bool fallback = false) {
    std::regex re("\"" + field + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (!std::regex_search(object, m, re)) return fallback;
    return m[1].str() == "true";
}

static std::vector<RegistryPackage> parse_registry_packages(const std::string& index) {
    std::vector<RegistryPackage> out;
    std::regex obj_re("\\{[^{}]*\"name\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^{}]*\"version\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^{}]*\\}");
    for (auto it = std::sregex_iterator(index.begin(), index.end(), obj_re);
         it != std::sregex_iterator(); ++it) {
        std::string object = it->str();
        RegistryPackage pkg;
        pkg.name = json_unescape((*it)[1].str());
        pkg.version = json_unescape((*it)[2].str());
        pkg.bundle = manifest_field(object, "bundle", "");
        pkg.hash = manifest_field(object, "hash", "");
        pkg.owner = manifest_field(object, "owner", "");
        pkg.yanked = json_bool_field_near(object, "yanked", false);
        out.push_back(pkg);
    }
    std::sort(out.begin(), out.end(), [](const RegistryPackage& a, const RegistryPackage& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.version < b.version;
    });
    return out;
}

static std::string registry_index_text() {
    std::string url = registry_url();
    if (!url.empty()) return http_get_text(url + "/index.json");
    fs::path index = registry_root() / "index.json";
    if (!fs::exists(index)) write_registry_index();
    return read_all(index);
}

static std::string registry_stats_text() {
    std::string url = registry_url();
    if (!url.empty()) return http_get_text(url + "/api/stats");
    fs::path stats = registry_root() / "stats.json";
    return read_all(stats);
}

static std::string registry_analytics_text() {
    std::string url = registry_url();
    if (!url.empty()) {
        std::string analytics = http_get_text(url + "/api/analytics");
        if (!analytics.empty()) return analytics;
        return http_get_text(url + "/api/stats");
    }
    return registry_stats_text();
}

static std::string lowercase_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

struct StdlibSearchEntry {
    std::string type;
    std::string module;
    std::string name;
    std::string signature;
    std::string source;
    int line = 0;
};

static std::vector<StdlibSearchEntry> builtin_stdlib_search_entries();

static int cmd_search(const std::vector<std::string>& argv) {
    std::string query;
    bool query_set = false;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg search [query] [--json]\n";
            return 0;
        }
        if (!query_set && (arg.empty() || arg[0] != '-')) {
            query = arg;
            query_set = true;
            continue;
        }
        return err("search accepts one optional query and optional --json");
    }

    std::string index = registry_index_text();
    if (index.empty()) return err("registry index not found; set SURA_REGISTRY or SURA_REGISTRY_URL");
    auto packages = parse_registry_packages(index);
    std::string needle = lowercase_copy(query);
    std::vector<RegistryPackage> matches;
    for (const auto& pkg : packages) {
        if (!needle.empty() && lowercase_copy(pkg.name).find(needle) == std::string::npos) continue;
        matches.push_back(pkg);
    }
    std::vector<StdlibSearchEntry> stdlib_matches;
    if (!needle.empty()) {
        for (const auto& entry : builtin_stdlib_search_entries()) {
            std::string haystack = lowercase_copy(entry.module + " " + entry.name + " " +
                                                  entry.signature + " " + entry.source);
            if (haystack.find(needle) == std::string::npos) continue;
            stdlib_matches.push_back(entry);
        }
    }
    if (json_output) {
        std::ostringstream out;
        out << "{\n"
            << "  \"schema\": \"sura.registry.search.v1\",\n"
            << "  \"query\": \"" << json_escape(query) << "\",\n"
            << "  \"count\": " << matches.size() << ",\n"
            << "  \"stdlib_count\": " << stdlib_matches.size() << ",\n"
            << "  \"packages\": [";
        for (size_t i = 0; i < matches.size(); ++i) {
            const auto& pkg = matches[i];
            if (i) out << ",";
            out << "\n"
                << "    {\n"
                << "      \"name\": \"" << json_escape(pkg.name) << "\",\n"
                << "      \"version\": \"" << json_escape(pkg.version) << "\",\n"
                << "      \"owner\": \"" << json_escape(pkg.owner) << "\",\n"
                << "      \"yanked\": " << (pkg.yanked ? "true" : "false") << ",\n"
                << "      \"hash\": \"" << json_escape(pkg.hash) << "\",\n"
                << "      \"bundle\": \"" << json_escape(pkg.bundle) << "\"\n"
                << "    }";
        }
        if (!matches.empty()) out << "\n  ";
        out << "],\n"
            << "  \"stdlib\": [";
        for (size_t i = 0; i < stdlib_matches.size(); ++i) {
            const auto& entry = stdlib_matches[i];
            if (i) out << ",";
            out << "\n"
                << "    {\n"
                << "      \"type\": \"" << json_escape(entry.type) << "\",\n"
                << "      \"module\": \"" << json_escape(entry.module) << "\",\n"
                << "      \"name\": \"" << json_escape(entry.name) << "\",\n"
                << "      \"signature\": \"" << json_escape(entry.signature) << "\",\n"
                << "      \"source\": \"" << json_escape(entry.source) << "\",\n"
                << "      \"line\": ";
            if (entry.line > 0) out << entry.line;
            else out << "null";
            out << "\n"
                << "    }";
        }
        if (!stdlib_matches.empty()) out << "\n  ";
        out << "]\n}\n";
        std::cout << out.str();
        return 0;
    }

    int shown = 0;
    for (const auto& pkg : matches) {
        std::cout << pkg.name << "@" << pkg.version;
        if (!pkg.owner.empty()) std::cout << " owner=" << pkg.owner;
        if (pkg.yanked) std::cout << " [yanked]";
        if (!pkg.hash.empty()) std::cout << " hash=" << pkg.hash.substr(0, std::min<size_t>(12, pkg.hash.size()));
        std::cout << "\n";
        ++shown;
    }
    if (!stdlib_matches.empty()) {
        if (shown > 0) std::cout << "\n";
        std::cout << "Standard library\n";
        for (const auto& entry : stdlib_matches) {
            std::cout << "  " << entry.name << "  " << entry.type;
            if (!entry.signature.empty()) std::cout << "  " << entry.signature;
            std::cout << "  (" << entry.source;
            if (entry.line > 0) std::cout << ":" << entry.line;
            std::cout << ")\n";
        }
    }
    if (shown == 0 && stdlib_matches.empty()) {
        std::cout << "No registry packages matched";
        if (!query.empty()) std::cout << " '" << query << "'";
        std::cout << "\n";
    }
    return 0;
}

static std::string json_object_field(const std::string& json, const std::string& field) {
    std::regex field_re("\"" + field + "\"\\s*:\\s*\\{");
    std::smatch m;
    if (!std::regex_search(json, m, field_re)) return "";
    size_t open = (size_t)m.position(0) + m.length(0) - 1;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = open; i < json.size(); ++i) {
        char ch = json[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) return json.substr(open + 1, i - open - 1);
        }
    }
    return "";
}

static std::vector<std::pair<std::string, long long>> json_number_items(const std::string& body) {
    std::vector<std::pair<std::string, long long>> out;
    std::regex item_re("\"((?:\\\\.|[^\"])*)\"\\s*:\\s*([0-9]+)");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), item_re);
         it != std::sregex_iterator(); ++it) {
        long long value = 0;
        try { value = std::stoll((*it)[2].str()); } catch (...) { value = 0; }
        out.push_back({json_unescape((*it)[1].str()), value});
    }
    return out;
}

static std::vector<std::pair<std::string, long long>> json_number_map(const std::string& json, const std::string& field) {
    std::vector<std::pair<std::string, long long>> out = json_number_items(json_object_field(json, field));
    std::sort(out.begin(), out.end());
    return out;
}

struct DailyRegistryCount {
    std::string day;
    std::string key;
    long long count = 0;
};

static std::vector<DailyRegistryCount> json_daily_number_map(const std::string& json, const std::string& field) {
    std::vector<DailyRegistryCount> out;
    std::string body = json_object_field(json, field);
    if (body.empty()) return out;
    std::regex day_re("\"((?:\\\\.|[^\"])*)\"\\s*:\\s*\\{([^}]*)\\}");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), day_re);
         it != std::sregex_iterator(); ++it) {
        std::string day = json_unescape((*it)[1].str());
        for (const auto& item : json_number_items((*it)[2].str())) {
            out.push_back({day, item.first, item.second});
        }
    }
    std::sort(out.begin(), out.end(), [](const DailyRegistryCount& a, const DailyRegistryCount& b) {
        if (a.day != b.day) return a.day > b.day;
        if (a.count != b.count) return a.count > b.count;
        return a.key < b.key;
    });
    return out;
}

static bool registry_stats_matches(const std::string& key, const std::string& needle) {
    return needle.empty() || lowercase_copy(key).find(needle) != std::string::npos;
}

static void write_registry_count_array(std::ostream& out,
                                       const std::vector<std::pair<std::string, long long>>& items,
                                       const std::string& needle,
                                       size_t limit = 0) {
    size_t written = 0;
    for (const auto& item : items) {
        if (!registry_stats_matches(item.first, needle)) continue;
        if (limit > 0 && written >= limit) break;
        if (written > 0) out << ",\n";
        out << "    {\"package\":\"" << json_escape(item.first)
            << "\",\"count\":" << item.second << "}";
        ++written;
    }
    if (written > 0) out << "\n";
}

static void write_registry_daily_array(std::ostream& out,
                                       const std::vector<DailyRegistryCount>& items,
                                       const std::string& needle,
                                       size_t limit = 0) {
    size_t written = 0;
    for (const auto& item : items) {
        if (!registry_stats_matches(item.key, needle)) continue;
        if (limit > 0 && written >= limit) break;
        if (written > 0) out << ",\n";
        out << "    {\"day\":\"" << json_escape(item.day)
            << "\",\"package\":\"" << json_escape(item.key)
            << "\",\"count\":" << item.count << "}";
        ++written;
    }
    if (written > 0) out << "\n";
}

static int cmd_stats(const std::vector<std::string>& argv) {
    std::string query;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg stats [name] [--json]\n";
            return 0;
        } else if (query.empty()) {
            query = arg;
        } else {
            return err("stats accepts at most one query plus --json");
        }
    }

    std::string stats = registry_stats_text();
    if (stats.empty()) return err("registry stats not found; start the tokenized registry API or provide stats.json");

    std::string needle = lowercase_copy(query);
    auto downloads = json_number_map(stats, "downloads");
    auto publishes = json_number_map(stats, "publishes");
    std::string last_name = manifest_field(stats, "name", "");
    std::string last_version = manifest_field(stats, "version", "");
    std::string last_user = manifest_field(stats, "user", "");
    std::string last_at = manifest_field(stats, "at", "");

    if (json_output) {
        std::cout << "{\n"
                  << "  \"schema\": \"sura.registry.stats.v1\",\n"
                  << "  \"query\": \"" << json_escape(query) << "\",\n"
                  << "  \"downloads\": [\n";
        write_registry_count_array(std::cout, downloads, needle);
        std::cout << "  ],\n"
                  << "  \"publishes\": [\n";
        write_registry_count_array(std::cout, publishes, needle);
        std::cout << "  ],\n"
                  << "  \"last_publish\": ";
        if (last_name.empty()) {
            std::cout << "null\n";
        } else {
            std::cout << "{\"name\":\"" << json_escape(last_name)
                      << "\",\"version\":\"" << json_escape(last_version)
                      << "\",\"user\":\"" << json_escape(last_user)
                      << "\",\"at\":\"" << json_escape(last_at) << "\"}\n";
        }
        std::cout << "}\n";
        return 0;
    }

    std::cout << "Downloads\n";
    int shown = 0;
    for (const auto& item : downloads) {
        if (!registry_stats_matches(item.first, needle)) continue;
        std::cout << "  " << item.first << "  " << item.second << "\n";
        ++shown;
    }
    if (shown == 0) std::cout << "  none\n";
    std::cout << "Publishes\n";
    shown = 0;
    for (const auto& item : publishes) {
        if (!registry_stats_matches(item.first, needle)) continue;
        std::cout << "  " << item.first << "  " << item.second << "\n";
        ++shown;
    }
    if (shown == 0) std::cout << "  none\n";
    if (!last_name.empty()) {
        std::cout << "Last publish\n  " << last_name << "@" << last_version
                  << " by " << last_user
                  << " at " << last_at << "\n";
    }
    return 0;
}

static int cmd_analytics(const std::vector<std::string>& argv) {
    std::string query;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg analytics [name] [--json]\n";
            return 0;
        } else if (query.empty()) {
            query = arg;
        } else {
            return err("analytics accepts at most one query plus --json");
        }
    }

    std::string stats = registry_analytics_text();
    if (stats.empty()) return err("registry analytics not found; start the tokenized registry API or provide stats.json");

    std::string needle = lowercase_copy(query);
    auto downloads = json_number_map(stats, "downloads");
    auto publishes = json_number_map(stats, "publishes");
    auto daily_downloads = json_daily_number_map(stats, "downloadDays");
    auto daily_publishes = json_daily_number_map(stats, "publishDays");

    auto count_desc = [](const std::pair<std::string, long long>& a,
                         const std::pair<std::string, long long>& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    };
    std::sort(downloads.begin(), downloads.end(), count_desc);
    std::sort(publishes.begin(), publishes.end(), count_desc);

    auto matches = [&](const std::string& key) {
        return registry_stats_matches(key, needle);
    };
    std::string last_name = manifest_field(stats, "name", "");
    std::string last_version = manifest_field(stats, "version", "");
    std::string last_user = manifest_field(stats, "user", "");
    std::string last_at = manifest_field(stats, "at", "");

    if (json_output) {
        std::cout << "{\n"
                  << "  \"schema\": \"sura.registry.analytics.v1\",\n"
                  << "  \"query\": \"" << json_escape(query) << "\",\n"
                  << "  \"top_downloads\": [\n";
        write_registry_count_array(std::cout, downloads, needle, 20);
        std::cout << "  ],\n"
                  << "  \"top_publishes\": [\n";
        write_registry_count_array(std::cout, publishes, needle, 20);
        std::cout << "  ],\n"
                  << "  \"daily_downloads\": [\n";
        write_registry_daily_array(std::cout, daily_downloads, needle, 30);
        std::cout << "  ],\n"
                  << "  \"daily_publishes\": [\n";
        write_registry_daily_array(std::cout, daily_publishes, needle, 30);
        std::cout << "  ],\n"
                  << "  \"last_publish\": ";
        if (last_name.empty()) {
            std::cout << "null\n";
        } else {
            std::cout << "{\"name\":\"" << json_escape(last_name)
                      << "\",\"version\":\"" << json_escape(last_version)
                      << "\",\"user\":\"" << json_escape(last_user)
                      << "\",\"at\":\"" << json_escape(last_at) << "\"}\n";
        }
        std::cout << "}\n";
        return 0;
    }

    std::cout << "Download analytics\n";
    std::cout << "Top downloads\n";
    int shown = 0;
    for (const auto& item : downloads) {
        if (!matches(item.first)) continue;
        std::cout << "  " << item.first << "  " << item.second << "\n";
        if (++shown >= 20) break;
    }
    if (shown == 0) std::cout << "  none\n";

    std::cout << "Top publishes\n";
    shown = 0;
    for (const auto& item : publishes) {
        if (!matches(item.first)) continue;
        std::cout << "  " << item.first << "  " << item.second << "\n";
        if (++shown >= 20) break;
    }
    if (shown == 0) std::cout << "  none\n";

    std::cout << "Daily downloads\n";
    shown = 0;
    for (const auto& item : daily_downloads) {
        if (!matches(item.key)) continue;
        std::cout << "  " << item.day << "  " << item.key << "  " << item.count << "\n";
        if (++shown >= 30) break;
    }
    if (shown == 0) std::cout << "  none\n";

    std::cout << "Daily publishes\n";
    shown = 0;
    for (const auto& item : daily_publishes) {
        if (!matches(item.key)) continue;
        std::cout << "  " << item.day << "  " << item.key << "  " << item.count << "\n";
        if (++shown >= 30) break;
    }
    if (shown == 0) std::cout << "  none\n";

    if (!last_name.empty()) {
        std::cout << "Last publish\n  " << last_name << "@" << last_version
                  << " by " << last_user
                  << " at " << last_at << "\n";
    }
    return 0;
}

static std::string safe_report_reason(std::string reason) {
    size_t start = 0;
    while (start < reason.size() && std::isspace((unsigned char)reason[start])) ++start;
    size_t end = reason.size();
    while (end > start && std::isspace((unsigned char)reason[end - 1])) --end;
    reason = reason.substr(start, end - start);
    if (reason.size() > 2000) reason.resize(2000);
    return reason;
}

static int append_local_report(const PackageRef& ref, const std::string& reason, std::string* id_out = nullptr, bool quiet = false) {
    fs::path path = registry_root() / "reports.json";
    std::string existing = read_all(path);
    std::string version = (ref.version.empty() || ref.version == "latest") ? "" : ref.version;
    std::string id = hex64(fnv1a_update(
        fnv1a_update(1469598103934665603ULL, ref.name + "@" + version),
        reason + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::ostringstream entry;
    entry << "    {\"id\":\"" << json_escape(id)
          << "\",\"name\":\"" << json_escape(ref.name)
          << "\",\"version\":\"" << json_escape(version)
          << "\",\"reason\":\"" << json_escape(reason)
          << "\",\"reporter\":\"local-cli\",\"status\":\"open\"}";

    if (existing.empty() || existing.find("\"reports\"") == std::string::npos) {
        std::ostringstream out;
        out << "{\n  \"reports\": [\n" << entry.str() << "\n  ]\n}\n";
        if (!write_all(path, out.str())) return err("failed to write " + path.generic_string());
    } else {
        size_t close = existing.rfind(']');
        if (close == std::string::npos) return err("invalid reports.json");
        bool has_entries = existing.find('{', existing.find("\"reports\"")) < close;
        std::string insert = std::string(has_entries ? ",\n" : "\n") + entry.str() + "\n  ";
        existing.insert(close, insert);
        if (!write_all(path, existing)) return err("failed to update " + path.generic_string());
    }
    if (id_out) *id_out = id;
    if (!quiet) ok("reported " + ref.name + (version.empty() ? "" : "@" + version) + " -> " + path.generic_string());
    return 0;
}

static int write_registry_report_submit_json(const std::string& source,
                                             const std::string& registry_ref,
                                             const PackageRef& ref,
                                             const std::string& version,
                                             const std::string& reason,
                                             const std::string& id,
                                             const std::string& reporter,
                                             const std::string& status) {
    std::cout << "{\n"
              << "  \"schema\": \"sura.registry.report.v1\",\n"
              << "  \"passed\": true,\n"
              << "  \"source\": \"" << json_escape(source) << "\",\n"
              << "  \"registry\": \"" << json_escape(registry_ref) << "\",\n"
              << "  \"id\": \"" << json_escape(id) << "\",\n"
              << "  \"name\": \"" << json_escape(ref.name) << "\",\n"
              << "  \"version\": \"" << json_escape(version) << "\",\n"
              << "  \"status\": \"" << json_escape(status) << "\",\n"
              << "  \"reporter\": \"" << json_escape(reporter) << "\",\n"
              << "  \"reason\": \"" << json_escape(reason) << "\"\n"
              << "}\n";
    return 0;
}

static int cmd_report(const std::vector<std::string>& argv) {
    std::vector<std::string> positional;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg report <name[@version]> <reason> [--json]\n";
            return 0;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.empty()) return err("report requires package name or name@version");
    std::string ref_text = positional[0];
    std::string reason_text;
    for (size_t i = 1; i < positional.size(); ++i) {
        if (!reason_text.empty()) reason_text += " ";
        reason_text += positional[i];
    }
    if (ref_text.empty()) return err("report requires package name or name@version");
    std::string reason = safe_report_reason(reason_text);
    if (reason.size() < 8) return err("report reason must be at least 8 characters");
    PackageRef ref = parse_package_ref(ref_text);
    if (ref.name.empty()) return err("invalid package name");
    std::string version = (ref.version.empty() || ref.version == "latest") ? "" : ref.version;

    std::string url = registry_url();
    if (url.empty()) {
        std::string id;
        int code = append_local_report(ref, reason, &id, json_output);
        if (code != 0) return code;
        if (json_output) {
            return write_registry_report_submit_json("local", path_to_generic_utf8(registry_root() / "reports.json"),
                                                     ref, version, reason, id, "local-cli", "open");
        }
        return 0;
    }

    std::ostringstream json;
    json << "{"
         << "\"name\":\"" << json_escape(ref.name) << "\","
         << "\"version\":\"" << json_escape(version) << "\","
         << "\"reason\":\"" << json_escape(reason) << "\","
         << "\"source\":\"surapkg\""
         << "}\n";
    std::string response;
    if (!http_post_json(url + "/api/report", json.str(), registry_token(), response)) {
        return err("HTTP registry report failed at " + url + "/api/report");
    }
    std::string id = manifest_field(response, "id", "");
    if (json_output) {
        std::string reporter = manifest_field(response, "reporter", registry_token().empty() ? "anonymous" : "");
        if (reporter.empty()) reporter = registry_token().empty() ? "anonymous" : "authenticated";
        return write_registry_report_submit_json("http", url, ref, version, reason, id, reporter, "open");
    }
    ok("reported " + ref.name + (version.empty() ? "" : "@" + version) + (id.empty() ? "" : " id=" + id));
    return 0;
}

struct RegistryReport {
    std::string id;
    std::string name;
    std::string version;
    std::string status;
    std::string reporter;
    std::string reason;
    std::string created_at;
    std::string reviewed_by;
    std::string reviewed_at;
    std::string review_note;
};

static std::vector<RegistryReport> parse_registry_reports(const std::string& json) {
    std::vector<RegistryReport> out;
    std::regex obj_re("\\{[^{}]*\"id\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^{}]*\\}");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), obj_re);
         it != std::sregex_iterator(); ++it) {
        std::string object = it->str();
        RegistryReport report;
        report.id = json_unescape((*it)[1].str());
        report.name = manifest_field(object, "name", "");
        report.version = manifest_field(object, "version", "");
        report.status = manifest_field(object, "status", "open");
        report.reporter = manifest_field(object, "reporter", manifest_field(object, "source", ""));
        report.reason = manifest_field(object, "reason", "");
        report.created_at = manifest_field(object, "createdAt", "");
        report.reviewed_by = manifest_field(object, "reviewedBy", "");
        report.reviewed_at = manifest_field(object, "reviewedAt", "");
        report.review_note = manifest_field(object, "reviewNote", "");
        out.push_back(report);
    }
    std::sort(out.begin(), out.end(), [](const RegistryReport& a, const RegistryReport& b) {
        if (a.status != b.status) return a.status < b.status;
        if (a.created_at != b.created_at) return a.created_at > b.created_at;
        return a.id < b.id;
    });
    return out;
}

static std::string compact_reason(std::string reason) {
    for (char& ch : reason) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    while (!reason.empty() && std::isspace((unsigned char)reason.front())) reason.erase(reason.begin());
    while (!reason.empty() && std::isspace((unsigned char)reason.back())) reason.pop_back();
    if (reason.size() > 96) reason = reason.substr(0, 96) + "...";
    return reason;
}

static void write_registry_report_json_object(std::ostream& out, const RegistryReport& report, const std::string& indent) {
    out << indent << "{\n"
        << indent << "  \"id\": \"" << json_escape(report.id) << "\",\n"
        << indent << "  \"name\": \"" << json_escape(report.name) << "\",\n"
        << indent << "  \"version\": \"" << json_escape(report.version) << "\",\n"
        << indent << "  \"status\": \"" << json_escape(report.status) << "\",\n"
        << indent << "  \"reporter\": \"" << json_escape(report.reporter) << "\",\n"
        << indent << "  \"reason\": \"" << json_escape(report.reason) << "\",\n"
        << indent << "  \"created_at\": \"" << json_escape(report.created_at) << "\",\n"
        << indent << "  \"reviewed_by\": \"" << json_escape(report.reviewed_by) << "\",\n"
        << indent << "  \"reviewed_at\": \"" << json_escape(report.reviewed_at) << "\",\n"
        << indent << "  \"review_note\": \"" << json_escape(report.review_note) << "\"\n"
        << indent << "}";
}

static int cmd_reports(const std::vector<std::string>& argv) {
    std::string status_filter;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg reports [status] [--json]\n";
            return 0;
        } else if (status_filter.empty()) {
            status_filter = arg;
        } else {
            return err("reports accepts at most one status plus --json");
        }
    }

    std::string raw;
    std::string url = registry_url();
    if (url.empty()) {
        raw = read_all(registry_root() / "reports.json");
        if (raw.empty()) return err("registry reports not found");
    } else if (!http_get_json(url + "/api/reports", registry_token(), raw)) {
        return err("HTTP registry reports failed at " + url + "/api/reports; set SURA_REGISTRY_TOKEN to an admin token");
    }

    std::string filter = lowercase_copy(status_filter);
    auto reports = parse_registry_reports(raw);
    if (json_output) {
        int count = 0;
        for (const auto& report : reports) {
            if (filter.empty() || lowercase_copy(report.status) == filter) ++count;
        }
        std::cout << "{\n"
                  << "  \"schema\": \"sura.registry.reports.v1\",\n"
                  << "  \"status\": \"" << json_escape(status_filter) << "\",\n"
                  << "  \"count\": " << count << ",\n"
                  << "  \"reports\": [\n";
        int written = 0;
        for (const auto& report : reports) {
            if (!filter.empty() && lowercase_copy(report.status) != filter) continue;
            if (written > 0) std::cout << ",\n";
            write_registry_report_json_object(std::cout, report, "    ");
            ++written;
        }
        if (written > 0) std::cout << "\n";
        std::cout << "  ]\n"
                  << "}\n";
        return 0;
    }

    std::cout << "Reports\n";
    int shown = 0;
    for (const auto& report : reports) {
        if (!filter.empty() && lowercase_copy(report.status) != filter) continue;
        std::cout << "  " << report.id << "  " << report.status << "  "
                  << report.name << (report.version.empty() ? "" : ("@" + report.version));
        if (!report.reporter.empty()) std::cout << " reporter=" << report.reporter;
        if (!report.reviewed_by.empty()) std::cout << " reviewedBy=" << report.reviewed_by;
        std::cout << "  " << compact_reason(report.reason) << "\n";
        ++shown;
    }
    if (shown == 0) std::cout << "  none\n";
    return 0;
}

struct RegistryAdvisory {
    std::string id;
    std::string name;
    std::string version;
    std::string severity;
    std::string status;
    std::string title;
    std::string description;
    std::string url;
    std::string created_by;
    std::string created_at;
    std::string updated_at;
};

static std::vector<RegistryAdvisory> parse_registry_advisories(const std::string& json) {
    std::vector<RegistryAdvisory> out;
    std::regex obj_re("\\{[^{}]*\"id\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^{}]*\\}");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), obj_re);
         it != std::sregex_iterator(); ++it) {
        std::string object = it->str();
        RegistryAdvisory advisory;
        advisory.id = json_unescape((*it)[1].str());
        advisory.name = manifest_field(object, "name", "");
        advisory.version = manifest_field(object, "version", "");
        advisory.severity = manifest_field(object, "severity", "");
        advisory.status = manifest_field(object, "status", "active");
        advisory.title = manifest_field(object, "title", "");
        advisory.description = manifest_field(object, "description", "");
        advisory.url = manifest_field(object, "url", "");
        advisory.created_by = manifest_field(object, "createdBy", "");
        advisory.created_at = manifest_field(object, "createdAt", "");
        advisory.updated_at = manifest_field(object, "updatedAt", "");
        out.push_back(advisory);
    }
    auto severity_rank = [](const std::string& severity) {
        std::string value = lowercase_copy(severity);
        if (value == "critical") return 0;
        if (value == "high") return 1;
        if (value == "moderate") return 2;
        if (value == "low") return 3;
        return 4;
    };
    std::sort(out.begin(), out.end(), [&](const RegistryAdvisory& a, const RegistryAdvisory& b) {
        if (a.status != b.status) return a.status < b.status;
        int ar = severity_rank(a.severity);
        int br = severity_rank(b.severity);
        if (ar != br) return ar < br;
        if (a.name != b.name) return a.name < b.name;
        if (a.version != b.version) return a.version < b.version;
        return a.id < b.id;
    });
    return out;
}

static bool advisory_matches_filter(const RegistryAdvisory& advisory,
                                    const std::string& name,
                                    const std::string& version,
                                    const std::string& severity,
                                    const std::string& status) {
    if (!name.empty() && normalize_name(advisory.name) != normalize_name(name)) return false;
    if (!version.empty() && !advisory.version.empty() && advisory.version != version) return false;
    if (!severity.empty() && lowercase_copy(advisory.severity) != severity) return false;
    if (!status.empty() && lowercase_copy(advisory.status) != status) return false;
    return true;
}

static void write_registry_advisory_json_object(std::ostream& out,
                                                const RegistryAdvisory& advisory,
                                                const std::string& indent) {
    out << indent << "{\n"
        << indent << "  \"id\": \"" << json_escape(advisory.id) << "\",\n"
        << indent << "  \"name\": \"" << json_escape(advisory.name) << "\",\n"
        << indent << "  \"version\": \"" << json_escape(advisory.version) << "\",\n"
        << indent << "  \"severity\": \"" << json_escape(advisory.severity) << "\",\n"
        << indent << "  \"status\": \"" << json_escape(advisory.status) << "\",\n"
        << indent << "  \"title\": \"" << json_escape(advisory.title) << "\",\n"
        << indent << "  \"description\": \"" << json_escape(advisory.description) << "\",\n"
        << indent << "  \"url\": \"" << json_escape(advisory.url) << "\",\n"
        << indent << "  \"created_by\": \"" << json_escape(advisory.created_by) << "\",\n"
        << indent << "  \"created_at\": \"" << json_escape(advisory.created_at) << "\",\n"
        << indent << "  \"updated_at\": \"" << json_escape(advisory.updated_at) << "\"\n"
        << indent << "}";
}

static bool advisory_valid_severity(const std::string& severity) {
    return severity == "low" || severity == "moderate" || severity == "high" || severity == "critical";
}

static bool advisory_valid_status(const std::string& status) {
    return status == "active" || status == "resolved" || status == "dismissed";
}

static bool advisory_valid_fail_on(const std::string& fail_on) {
    return fail_on.empty() || fail_on == "none" || fail_on == "active" || fail_on == "any" ||
           fail_on == "low" || fail_on == "moderate" || fail_on == "high" || fail_on == "critical";
}

static int registry_advisory_severity_score(const std::string& severity) {
    std::string value = lowercase_copy(severity);
    if (value == "critical") return 4;
    if (value == "high") return 3;
    if (value == "moderate") return 2;
    if (value == "low") return 1;
    return 0;
}

static void write_registry_advisory_storage_object(std::ostream& out,
                                                   const RegistryAdvisory& advisory,
                                                   const std::string& indent) {
    out << indent << "{\"id\":\"" << json_escape(advisory.id)
        << "\",\"name\":\"" << json_escape(advisory.name)
        << "\",\"version\":\"" << json_escape(advisory.version)
        << "\",\"severity\":\"" << json_escape(advisory.severity)
        << "\",\"status\":\"" << json_escape(advisory.status)
        << "\",\"title\":\"" << json_escape(advisory.title)
        << "\",\"description\":\"" << json_escape(advisory.description)
        << "\",\"url\":\"" << json_escape(advisory.url)
        << "\",\"createdBy\":\"" << json_escape(advisory.created_by)
        << "\",\"createdAt\":\"" << json_escape(advisory.created_at)
        << "\",\"updatedAt\":\"" << json_escape(advisory.updated_at)
        << "\"}";
}

static bool write_registry_advisories(const fs::path& root, std::vector<RegistryAdvisory> advisories) {
    std::sort(advisories.begin(), advisories.end(), [](const RegistryAdvisory& a, const RegistryAdvisory& b) {
        if (a.name != b.name) return a.name < b.name;
        if (a.version != b.version) return a.version < b.version;
        if (a.status != b.status) return a.status < b.status;
        return a.id < b.id;
    });
    std::ostringstream out;
    out << "{\n  \"advisories\": [";
    if (!advisories.empty()) out << "\n";
    for (size_t i = 0; i < advisories.size(); ++i) {
        write_registry_advisory_storage_object(out, advisories[i], "    ");
        if (i + 1 < advisories.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
    return write_all(root / "advisories.json", out.str());
}

static int write_registry_advisory_submit_json(const std::string& source,
                                               const std::string& registry_ref,
                                               const RegistryAdvisory& advisory) {
    std::cout << "{\n"
              << "  \"schema\": \"sura.registry.advisory.v1\",\n"
              << "  \"passed\": true,\n"
              << "  \"source\": \"" << json_escape(source) << "\",\n"
              << "  \"registry\": \"" << json_escape(registry_ref) << "\",\n"
              << "  \"advisory\": ";
    write_registry_advisory_json_object(std::cout, advisory, "  ");
    std::cout << "\n}\n";
    return 0;
}

static std::string registry_advisory_target_text(const RegistryAdvisory& advisory) {
    std::string target = advisory.name.empty() ? advisory.id : advisory.name;
    if (!advisory.version.empty()) target += "@" + advisory.version;
    return target;
}

static std::string registry_advisory_next_action(const RegistryAdvisory& advisory) {
    std::string status = lowercase_copy(advisory.status.empty() ? "active" : advisory.status);
    if (status != "active") return "";
    std::string severity = lowercase_copy(advisory.severity);
    std::string target = registry_advisory_target_text(advisory);
    if (severity == "critical") {
        return "block installs/updates for " + target +
               "; yank the affected version or publish a fixed release, then run surapkg audit";
    }
    if (severity == "high") {
        return "prioritize a fixed release or upgrade for " + target +
               "; run surapkg update and surapkg audit before shipping";
    }
    if (severity == "moderate") {
        return "review the advisory for " + target +
               " and schedule an upgrade or patched release";
    }
    if (severity == "low") {
        return "track the advisory for " + target +
               " and confirm the risk is documented before release";
    }
    return "review active advisory " + advisory.id + " for " + target;
}

static bool registry_advisory_fails_on(const RegistryAdvisory& advisory, const std::string& fail_on) {
    std::string gate = lowercase_copy(fail_on);
    if (gate.empty() || gate == "none") return false;
    std::string status = lowercase_copy(advisory.status.empty() ? "active" : advisory.status);
    if (status != "active") return false;
    if (gate == "active" || gate == "any") return true;
    int threshold = registry_advisory_severity_score(gate);
    if (threshold <= 0) return false;
    return registry_advisory_severity_score(advisory.severity) >= threshold;
}

static size_t registry_advisory_failing_count(const std::vector<RegistryAdvisory>& advisories,
                                              const std::string& fail_on) {
    size_t count = 0;
    for (const auto& advisory : advisories) {
        if (registry_advisory_fails_on(advisory, fail_on)) ++count;
    }
    return count;
}

static void write_registry_advisory_next_actions_json(std::ostream& out,
                                                      const std::vector<RegistryAdvisory>& advisories,
                                                      const std::string& indent) {
    out << indent << "\"next_actions\": [";
    bool wrote = false;
    for (const auto& advisory : advisories) {
        std::string action = registry_advisory_next_action(advisory);
        if (action.empty()) continue;
        if (!wrote) {
            out << "\n";
        } else {
            out << ",\n";
        }
        wrote = true;
        out << indent << "  {\"advisory_id\":\"" << json_escape(advisory.id)
            << "\",\"target\":\"" << json_escape(registry_advisory_target_text(advisory))
            << "\",\"severity\":\"" << json_escape(lowercase_copy(advisory.severity))
            << "\",\"status\":\"" << json_escape(lowercase_copy(advisory.status.empty() ? "active" : advisory.status))
            << "\",\"action\":\"" << json_escape(action) << "\"}";
    }
    if (wrote) out << "\n" << indent;
    out << "]";
}

static std::string join_vector_args(const std::vector<std::string>& argv, size_t start);
static std::string utc_timestamp_now();
static bool is_version_range_spec(const std::string& spec);

static int cmd_advisory(const std::vector<std::string>& argv) {
    std::vector<std::string> positional;
    bool json_output = false;
    std::string severity;
    std::string status = "active";
    std::string title;
    std::string description;
    std::string reference_url;

    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--severity") {
            if (i + 1 >= argv.size()) return err("advisory --severity requires a value");
            severity = lowercase_copy(argv[++i]);
        } else if (arg.rfind("--severity=", 0) == 0) {
            severity = lowercase_copy(arg.substr(11));
        } else if (arg == "--status") {
            if (i + 1 >= argv.size()) return err("advisory --status requires a value");
            status = lowercase_copy(argv[++i]);
        } else if (arg.rfind("--status=", 0) == 0) {
            status = lowercase_copy(arg.substr(9));
        } else if (arg == "--title") {
            if (i + 1 >= argv.size()) return err("advisory --title requires a value");
            title = safe_report_reason(argv[++i]);
        } else if (arg.rfind("--title=", 0) == 0) {
            title = safe_report_reason(arg.substr(8));
        } else if (arg == "--description") {
            if (i + 1 >= argv.size()) return err("advisory --description requires a value");
            description = safe_report_reason(argv[++i]);
        } else if (arg.rfind("--description=", 0) == 0) {
            description = safe_report_reason(arg.substr(14));
        } else if (arg == "--url") {
            if (i + 1 >= argv.size()) return err("advisory --url requires a value");
            reference_url = safe_report_reason(argv[++i]);
            if (reference_url.size() > 500) reference_url.resize(500);
        } else if (arg.rfind("--url=", 0) == 0) {
            reference_url = safe_report_reason(arg.substr(6));
            if (reference_url.size() > 500) reference_url.resize(500);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg advisory <name[@version]> [severity] [title] [description...] [--severity low|moderate|high|critical] [--status active|resolved|dismissed] [--title text] [--description text] [--url URL] [--json]\n";
            return 0;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.empty()) return err("advisory requires package name or name@version");
    if (severity.empty() && positional.size() >= 2) severity = lowercase_copy(positional[1]);
    if (title.empty() && positional.size() >= 3) title = safe_report_reason(positional[2]);
    if (description.empty() && positional.size() >= 4) description = safe_report_reason(join_vector_args(positional, 3));

    if (!advisory_valid_severity(severity)) {
        return err("advisory --severity must be low, moderate, high, or critical");
    }
    if (!advisory_valid_status(status)) {
        return err("advisory --status must be active, resolved, or dismissed");
    }
    if (title.size() < 4) return err("advisory title must be at least 4 characters");
    if (description.size() < 8) return err("advisory description must be at least 8 characters");
    if (reference_url.find_first_of("\"\r\n") != std::string::npos) {
        return err("advisory --url must not contain quotes or newlines");
    }

    std::string ref_text = positional[0];
    PackageRef ref = parse_package_ref(ref_text);
    if (ref.name.empty()) return err("invalid package name");
    std::string version = (ref.version.empty() || ref.version == "latest") ? "" : ref.version;
    if (!version.empty() && is_version_range_spec(version)) {
        return err("advisory requires a concrete package version when @version is used");
    }

    std::string url = registry_url();
    if (!url.empty()) {
        std::ostringstream body;
        body << "{"
             << "\"name\":\"" << json_escape(ref.name) << "\","
             << "\"version\":\"" << json_escape(version) << "\","
             << "\"severity\":\"" << json_escape(severity) << "\","
             << "\"status\":\"" << json_escape(status) << "\","
             << "\"title\":\"" << json_escape(title) << "\","
             << "\"description\":\"" << json_escape(description) << "\","
             << "\"url\":\"" << json_escape(reference_url) << "\""
             << "}\n";
        std::string response;
        if (!http_post_json(url + "/api/advisories", body.str(), registry_token(), response)) {
            return err("HTTP registry advisory creation failed at " + url + "/api/advisories; set SURA_REGISTRY_TOKEN to an admin token");
        }
        RegistryAdvisory advisory;
        auto parsed = parse_registry_advisories(response);
        if (!parsed.empty()) {
            advisory = parsed.front();
        } else {
            advisory.id = manifest_field(response, "id", "");
            advisory.name = ref.name;
            advisory.version = version;
            advisory.severity = severity;
            advisory.status = status;
            advisory.title = title;
            advisory.description = description;
            advisory.url = reference_url;
        }
        if (json_output) return write_registry_advisory_submit_json("http", url, advisory);
        ok("created registry advisory " + advisory.id + " for " + ref.name + (version.empty() ? "" : ("@" + version)));
        return 0;
    }

    fs::path root = registry_root();
    if (!fs::exists(root / ref.name)) {
        return err("advisory package not found: " + ref.name);
    }
    if (!version.empty() && !fs::exists(root / ref.name / version / "package.surabundle.json")) {
        return err("advisory package version not found: " + ref.name + "@" + version);
    }

    std::string now = utc_timestamp_now();
    RegistryAdvisory advisory;
    advisory.id = hex64(fnv1a_update(
        fnv1a_update(1469598103934665603ULL, "sura-advisory:" + ref.name + "@" + version),
        severity + ":" + title + ":" + now + ":" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    advisory.name = ref.name;
    advisory.version = version;
    advisory.severity = severity;
    advisory.status = status;
    advisory.title = title;
    advisory.description = description;
    advisory.url = reference_url;
    advisory.created_by = "local-admin";
    advisory.created_at = now;
    advisory.updated_at = now;

    auto advisories = parse_registry_advisories(read_all(root / "advisories.json"));
    advisories.push_back(advisory);
    if (!write_registry_advisories(root, advisories)) {
        return err("failed to write " + (root / "advisories.json").generic_string());
    }

    if (json_output) {
        return write_registry_advisory_submit_json("local", path_to_generic_utf8(root / "advisories.json"), advisory);
    }
    ok("created registry advisory " + advisory.id + " for " + ref.name + (version.empty() ? "" : ("@" + version)));
    return 0;
}

static int cmd_advisories(const std::vector<std::string>& argv) {
    std::string query;
    std::string severity;
    std::string status;
    std::string fail_on;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--severity") {
            if (i + 1 >= argv.size()) return err("advisories --severity requires a value");
            severity = lowercase_copy(argv[++i]);
        } else if (arg.rfind("--severity=", 0) == 0) {
            severity = lowercase_copy(arg.substr(11));
        } else if (arg == "--status") {
            if (i + 1 >= argv.size()) return err("advisories --status requires a value");
            status = lowercase_copy(argv[++i]);
        } else if (arg.rfind("--status=", 0) == 0) {
            status = lowercase_copy(arg.substr(9));
        } else if (arg == "--fail-on") {
            if (i + 1 >= argv.size()) return err("advisories --fail-on requires a value");
            fail_on = lowercase_copy(argv[++i]);
        } else if (arg.rfind("--fail-on=", 0) == 0) {
            fail_on = lowercase_copy(arg.substr(10));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg advisories [name[@version]] [--severity low|moderate|high|critical] [--status active|resolved|dismissed] [--fail-on none|active|any|low|moderate|high|critical] [--json]\n";
            return 0;
        } else if (query.empty()) {
            query = arg;
        } else {
            return err("advisories accepts at most one package query plus filters and --json");
        }
    }
    if (!severity.empty() && severity != "low" && severity != "moderate" && severity != "high" && severity != "critical") {
        return err("advisories --severity must be low, moderate, high, or critical");
    }
    if (!status.empty() && status != "active" && status != "resolved" && status != "dismissed") {
        return err("advisories --status must be active, resolved, or dismissed");
    }
    if (!advisory_valid_fail_on(fail_on)) {
        return err("advisories --fail-on must be none, active, any, low, moderate, high, or critical");
    }

    std::string name;
    std::string version;
    if (!query.empty()) {
        PackageRef ref = parse_package_ref(query);
        if (ref.name.empty()) return err("invalid package name");
        name = ref.name;
        version = query.find('@') == std::string::npos ? "" : ref.version;
    }

    std::string raw;
    std::string source = "local";
    std::string registry_ref = path_to_generic_utf8(registry_root() / "advisories.json");
    std::string base_url = registry_url();
    if (!base_url.empty()) {
        source = "http";
        registry_ref = base_url;
        std::string endpoint = base_url + "/api/advisories";
        std::vector<std::string> params;
        if (!name.empty()) params.push_back("name=" + query_escape(name));
        if (!version.empty()) params.push_back("version=" + query_escape(version));
        if (!severity.empty()) params.push_back("severity=" + query_escape(severity));
        if (!status.empty()) params.push_back("status=" + query_escape(status));
        for (size_t i = 0; i < params.size(); ++i) endpoint += (i == 0 ? "?" : "&") + params[i];
        raw = http_get_text(endpoint);
        if (raw.empty()) return err("HTTP registry advisories not reachable at " + endpoint);
    } else {
        raw = read_all(registry_root() / "advisories.json");
        if (raw.empty()) raw = "{\"advisories\":[]}";
    }

    auto parsed = parse_registry_advisories(raw);
    std::vector<RegistryAdvisory> advisories;
    for (const auto& advisory : parsed) {
        if (advisory_matches_filter(advisory, name, version, severity, status)) {
            advisories.push_back(advisory);
        }
    }

    size_t failing_count = registry_advisory_failing_count(advisories, fail_on);
    bool passed = failing_count == 0;

    if (json_output) {
        std::cout << "{\n"
                  << "  \"schema\": \"sura.registry.advisories.v1\",\n"
                  << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
                  << "  \"source\": \"" << json_escape(source) << "\",\n"
                  << "  \"registry\": \"" << json_escape(registry_ref) << "\",\n"
                  << "  \"query\": \"" << json_escape(query) << "\",\n"
                  << "  \"severity\": \"" << json_escape(severity) << "\",\n"
                  << "  \"status\": \"" << json_escape(status) << "\",\n"
                  << "  \"fail_on\": \"" << json_escape(fail_on) << "\",\n"
                  << "  \"failing_count\": " << failing_count << ",\n"
                  << "  \"count\": " << advisories.size() << ",\n";
        write_registry_advisory_next_actions_json(std::cout, advisories, "  ");
        std::cout << ",\n"
                  << "  \"advisories\": [\n";
        for (size_t i = 0; i < advisories.size(); ++i) {
            if (i) std::cout << ",\n";
            write_registry_advisory_json_object(std::cout, advisories[i], "    ");
        }
        if (!advisories.empty()) std::cout << "\n";
        std::cout << "  ]\n"
                  << "}\n";
        return passed ? 0 : 1;
    }

    std::cout << "Security advisories\n";
    int shown = 0;
    for (const auto& advisory : advisories) {
        std::cout << "  " << advisory.id << "  "
                  << advisory.severity << "  "
                  << advisory.status << "  "
                  << advisory.name;
        if (!advisory.version.empty()) std::cout << "@" << advisory.version;
        std::cout << "  " << advisory.title << "\n";
        if (!advisory.description.empty()) {
            std::cout << "    " << compact_reason(advisory.description) << "\n";
        }
        if (!advisory.url.empty()) {
            std::cout << "    " << advisory.url << "\n";
        }
        ++shown;
    }
    if (shown == 0) std::cout << "  none\n";
    if (!passed) {
        return err("advisories --fail-on " + fail_on + " matched " +
                   std::to_string(failing_count) + " active advisory/advisories");
    }
    return 0;
}

static bool allow_critical_advisory_install() {
    const char* env = std::getenv("SURA_ALLOW_CRITICAL_ADVISORY_INSTALL");
    if (!env || !*env) return false;
    std::string value = lowercase_copy(env);
    return value != "0" && value != "false" && value != "no" && value != "off";
}

static std::vector<RegistryAdvisory> registry_active_advisories_for_package(const std::string& name,
                                                                           const std::string& version) {
    std::string raw;
    std::string base_url = registry_url();
    if (!base_url.empty()) {
        std::string endpoint = base_url + "/api/advisories?name=" + query_escape(name) +
                               "&version=" + query_escape(version) + "&status=active";
        raw = http_get_text(endpoint);
    } else {
        raw = read_all(registry_root() / "advisories.json");
    }
    if (raw.empty()) return {};
    std::vector<RegistryAdvisory> active;
    for (const auto& advisory : parse_registry_advisories(raw)) {
        if (advisory_matches_filter(advisory, name, version, "", "active")) {
            active.push_back(advisory);
        }
    }
    return active;
}

static int check_install_advisories(const std::string& name, const std::string& version) {
    auto advisories = registry_active_advisories_for_package(name, version);
    bool critical = false;
    for (const auto& advisory : advisories) {
        std::string severity = lowercase_copy(advisory.severity);
        std::cout << "[warning] registry advisory " << severity << " "
                  << advisory.name;
        if (!advisory.version.empty()) std::cout << "@" << advisory.version;
        if (!advisory.title.empty()) std::cout << ": " << advisory.title;
        std::cout << "\n";
        if (severity == "critical") critical = true;
    }
    if (critical && !allow_critical_advisory_install()) {
        return err("install blocked by active critical registry advisory for " + name + "@" + version +
                   "; set SURA_ALLOW_CRITICAL_ADVISORY_INSTALL=1 to override");
    }
    return 0;
}

static std::string join_vector_args(const std::vector<std::string>& argv, size_t start) {
    std::string out;
    for (size_t i = start; i < argv.size(); ++i) {
        if (!out.empty()) out += " ";
        out += argv[i];
    }
    return out;
}

static std::string utc_timestamp_now() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

static bool write_registry_yanks(const fs::path& root, const std::map<std::string, RegistryYankRecord>& yanks) {
    std::ostringstream out;
    out << "{\n  \"yanked\": {";
    if (!yanks.empty()) out << "\n";
    size_t written = 0;
    for (const auto& item : yanks) {
        const auto& record = item.second;
        out << "    \"" << json_escape(item.first) << "\": {"
            << "\"name\":\"" << json_escape(record.name) << "\","
            << "\"version\":\"" << json_escape(record.version) << "\","
            << "\"reason\":\"" << json_escape(record.reason) << "\","
            << "\"by\":\"" << json_escape(record.by) << "\","
            << "\"at\":\"" << json_escape(record.at) << "\""
            << "}";
        if (++written < yanks.size()) out << ",";
        out << "\n";
    }
    out << "  }\n}\n";
    return write_all(root / "yanks.json", out.str());
}

static int write_yank_result_json(const std::string& schema,
                                  const std::string& source,
                                  const std::string& name,
                                  const std::string& version,
                                  bool yanked,
                                  const std::string& reason,
                                  const std::string& by,
                                  const std::string& at) {
    std::cout << "{\n"
              << "  \"schema\": \"" << json_escape(schema) << "\",\n"
              << "  \"ok\": true,\n"
              << "  \"source\": \"" << json_escape(source) << "\",\n"
              << "  \"name\": \"" << json_escape(name) << "\",\n"
              << "  \"version\": \"" << json_escape(version) << "\",\n"
              << "  \"yanked\": " << (yanked ? "true" : "false") << ",\n"
              << "  \"reason\": \"" << json_escape(reason) << "\",\n"
              << "  \"by\": \"" << json_escape(by) << "\",\n"
              << "  \"at\": \"" << json_escape(at) << "\"\n"
              << "}\n";
    return 0;
}

static void write_owner_record_json_object(std::ostream& out,
                                           const RegistryOwnerRecord& record,
                                           const std::string& indent) {
    out << indent << "{\n"
        << indent << "  \"name\": \"" << json_escape(record.name) << "\",\n"
        << indent << "  \"owner\": \"" << json_escape(record.owner) << "\",\n"
        << indent << "  \"created_at\": \"" << json_escape(record.created_at) << "\",\n"
        << indent << "  \"updated_at\": \"" << json_escape(record.updated_at) << "\"\n"
        << indent << "}";
}

static int cmd_owners(const std::vector<std::string>& argv) {
    std::string query;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg owners [name] [--json]\n";
            return 0;
        } else if (query.empty()) {
            query = normalize_name(arg);
        } else {
            return err("owners accepts at most one package name plus --json");
        }
    }

    std::string url = registry_url();
    bool remote = !url.empty();
    std::string raw = remote ? http_get_text(url + "/api/owners") : read_all(registry_root() / "owners.json");
    if (raw.empty()) {
        if (remote) return err("HTTP registry owners not reachable at " + url + "/api/owners");
        raw = "{\"packages\":{}}";
    }

    auto owners = parse_registry_owners(raw);
    auto matches = [&](const RegistryOwnerRecord& record) {
        return query.empty() || normalize_name(record.name) == query;
    };

    if (json_output) {
        int count = 0;
        for (const auto& item : owners) {
            if (matches(item.second)) ++count;
        }
        std::cout << "{\n"
                  << "  \"schema\": \"sura.registry.owners.v1\",\n"
                  << "  \"source\": \"" << (remote ? "http" : "local") << "\",\n"
                  << "  \"query\": \"" << json_escape(query) << "\",\n"
                  << "  \"count\": " << count << ",\n"
                  << "  \"owners\": [\n";
        int written = 0;
        for (const auto& item : owners) {
            if (!matches(item.second)) continue;
            if (written > 0) std::cout << ",\n";
            write_owner_record_json_object(std::cout, item.second, "    ");
            ++written;
        }
        if (written > 0) std::cout << "\n";
        std::cout << "  ]\n"
                  << "}\n";
        return 0;
    }

    std::cout << "Registry owners\n";
    int shown = 0;
    for (const auto& item : owners) {
        const auto& record = item.second;
        if (!matches(record)) continue;
        std::cout << "  " << record.name << "  owner=" << record.owner;
        if (!record.created_at.empty()) std::cout << " createdAt=" << record.created_at;
        if (!record.updated_at.empty()) std::cout << " updatedAt=" << record.updated_at;
        std::cout << "\n";
        ++shown;
    }
    if (shown == 0) std::cout << "  none\n";
    return 0;
}

static void write_yank_record_json_object(std::ostream& out,
                                          const RegistryYankRecord& record,
                                          const std::string& indent) {
    out << indent << "{\n"
        << indent << "  \"name\": \"" << json_escape(record.name) << "\",\n"
        << indent << "  \"version\": \"" << json_escape(record.version) << "\",\n"
        << indent << "  \"reason\": \"" << json_escape(record.reason) << "\",\n"
        << indent << "  \"by\": \"" << json_escape(record.by) << "\",\n"
        << indent << "  \"at\": \"" << json_escape(record.at) << "\"\n"
        << indent << "}";
}

static int cmd_yanks(const std::vector<std::string>& argv) {
    std::string query;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg yanks [name] [--json]\n";
            return 0;
        } else if (query.empty()) {
            query = normalize_name(arg);
        } else {
            return err("yanks accepts at most one package name plus --json");
        }
    }

    std::string url = registry_url();
    bool remote = !url.empty();
    std::string raw = remote ? http_get_text(url + "/api/yanks") : read_all(registry_root() / "yanks.json");
    if (raw.empty()) {
        if (remote) return err("HTTP registry yanks not reachable at " + url + "/api/yanks");
        raw = "{\"yanked\":{}}";
    }

    auto yanks = parse_registry_yanks(raw);
    auto matches = [&](const RegistryYankRecord& record) {
        return query.empty() || normalize_name(record.name) == query;
    };

    if (json_output) {
        int count = 0;
        for (const auto& item : yanks) {
            if (matches(item.second)) ++count;
        }
        std::cout << "{\n"
                  << "  \"schema\": \"sura.registry.yanks.v1\",\n"
                  << "  \"source\": \"" << (remote ? "http" : "local") << "\",\n"
                  << "  \"query\": \"" << json_escape(query) << "\",\n"
                  << "  \"count\": " << count << ",\n"
                  << "  \"yanks\": [\n";
        int written = 0;
        for (const auto& item : yanks) {
            if (!matches(item.second)) continue;
            if (written > 0) std::cout << ",\n";
            write_yank_record_json_object(std::cout, item.second, "    ");
            ++written;
        }
        if (written > 0) std::cout << "\n";
        std::cout << "  ]\n"
                  << "}\n";
        return 0;
    }

    std::cout << "Yanked packages\n";
    int shown = 0;
    for (const auto& item : yanks) {
        const auto& record = item.second;
        if (!matches(record)) continue;
        std::cout << "  " << record.name << "@" << record.version;
        if (!record.by.empty()) std::cout << " by " << record.by;
        if (!record.at.empty()) std::cout << " at " << record.at;
        if (!record.reason.empty()) std::cout << "  " << compact_reason(record.reason);
        std::cout << "\n";
        ++shown;
    }
    if (shown == 0) std::cout << "  none\n";
    return 0;
}

struct RegistryHealthCheck {
    std::string name;
    std::string status;
    std::string message;
};

static void registry_health_check(std::vector<RegistryHealthCheck>& checks,
                                  const std::string& name,
                                  const std::string& status,
                                  const std::string& message) {
    checks.push_back({name, status, message});
}

static int registry_health_error_count(const std::vector<RegistryHealthCheck>& checks) {
    int errors = 0;
    for (const auto& check : checks) {
        if (check.status == "error") ++errors;
    }
    return errors;
}

static int registry_health_warning_count(const std::vector<RegistryHealthCheck>& checks) {
    int warnings = 0;
    for (const auto& check : checks) {
        if (check.status == "warn") ++warnings;
    }
    return warnings;
}

static size_t registry_key_count(const fs::path& root) {
    fs::path keys = root / "keys";
    if (!fs::is_directory(keys)) return 0;
    size_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(keys, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".pem") ++count;
    }
    return count;
}

static int cmd_registry_health(const std::vector<std::string>& argv) {
    std::string path_arg;
    bool json_output = false;
    bool fail_on_warning = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--fail-on-warning") {
            fail_on_warning = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg registry-health [path] [--fail-on-warning] [--json]\n";
            return 0;
        } else if (path_arg.empty()) {
            path_arg = arg;
        } else {
            return err("registry-health accepts at most one path plus --fail-on-warning and --json");
        }
    }

    std::string url = registry_url();
    bool remote = path_arg.empty() && !url.empty();
    fs::path root = path_arg.empty() ? registry_root() : fs::path(path_arg);
    std::string registry_ref = remote ? url : path_to_generic_utf8(root);
    std::vector<RegistryHealthCheck> checks;

    std::string health_raw;
    std::string index_raw;
    std::string stats_raw;
    std::string analytics_raw;
    std::string owners_raw;
    std::string yanks_raw;
    std::string reports_raw;
    std::string advisories_raw;
    bool health_ok = false;
    bool index_present = false;
    bool stats_present = false;
    bool analytics_present = false;
    bool reports_reachable = false;
    size_t key_count = 0;

    if (remote) {
        health_raw = http_get_text(url + "/health");
        health_ok = json_bool_field_near(health_raw, "ok", false);
        registry_health_check(checks, "health", health_ok ? "ok" : "error",
                              health_ok ? "HTTP registry health endpoint returned ok" :
                                          "HTTP registry health endpoint not reachable or not ok");

        index_raw = http_get_text(url + "/index.json");
        index_present = !index_raw.empty();
        registry_health_check(checks, "index", index_present ? "ok" : "error",
                              index_present ? "HTTP registry index reachable" :
                                              "HTTP registry index not reachable");

        stats_raw = http_get_text(url + "/api/stats");
        stats_present = !stats_raw.empty();
        registry_health_check(checks, "stats", stats_present ? "ok" : "warn",
                              stats_present ? "HTTP registry stats reachable" :
                                              "HTTP registry stats endpoint not reachable");

        analytics_raw = http_get_text(url + "/api/analytics");
        analytics_present = !analytics_raw.empty();
        registry_health_check(checks, "analytics", analytics_present ? "ok" : "warn",
                              analytics_present ? "HTTP registry analytics reachable" :
                                                  "HTTP registry analytics endpoint not reachable");

        owners_raw = http_get_text(url + "/api/owners");
        registry_health_check(checks, "owners", owners_raw.empty() ? "warn" : "ok",
                              owners_raw.empty() ? "HTTP registry owners endpoint not reachable" :
                                                   "HTTP registry owners endpoint reachable");

        yanks_raw = http_get_text(url + "/api/yanks");
        registry_health_check(checks, "yanks", yanks_raw.empty() ? "warn" : "ok",
                              yanks_raw.empty() ? "HTTP registry yanks endpoint not reachable" :
                                                  "HTTP registry yanks endpoint reachable");

        advisories_raw = http_get_text(url + "/api/advisories");
        registry_health_check(checks, "advisories", advisories_raw.empty() ? "warn" : "ok",
                              advisories_raw.empty() ? "HTTP registry advisories endpoint not reachable" :
                                                       "HTTP registry advisories endpoint reachable");

        reports_reachable = http_get_json(url + "/api/reports", registry_token(), reports_raw);
        registry_health_check(checks, "reports", reports_reachable ? "ok" : "warn",
                              reports_reachable ? "HTTP registry reports endpoint reachable with current token" :
                                                  "HTTP registry reports endpoint not reachable; set SURA_REGISTRY_TOKEN to an admin token");
        registry_health_check(checks, "keys", "unknown",
                              "remote key directory listing is not exposed; verify-registry checks referenced keys");
    } else {
        health_ok = fs::is_directory(root);
        registry_health_check(checks, "root", health_ok ? "ok" : "error",
                              health_ok ? "local registry root exists" :
                                          "local registry root not found");

        index_raw = read_all(root / "index.json");
        index_present = !index_raw.empty();
        registry_health_check(checks, "index", index_present ? "ok" : "error",
                              index_present ? "local registry index found" :
                                              "local registry index not found; run surapkg index or publish a package");

        stats_raw = read_all(root / "stats.json");
        stats_present = !stats_raw.empty();
        registry_health_check(checks, "stats", stats_present ? "ok" : "warn",
                              stats_present ? "local registry stats found" :
                                              "local registry stats not found");

        analytics_raw = stats_raw;
        analytics_present = stats_present;
        owners_raw = read_all(root / "owners.json");
        registry_health_check(checks, "owners", owners_raw.empty() ? "warn" : "ok",
                              owners_raw.empty() ? "local registry owners metadata not found" :
                                                   "local registry owners metadata found");

        yanks_raw = read_all(root / "yanks.json");
        registry_health_check(checks, "yanks", yanks_raw.empty() ? "warn" : "ok",
                              yanks_raw.empty() ? "local registry yanks metadata not found" :
                                                  "local registry yanks metadata found");

        reports_raw = read_all(root / "reports.json");
        reports_reachable = !reports_raw.empty();
        registry_health_check(checks, "reports", reports_reachable ? "ok" : "warn",
                              reports_reachable ? "local registry reports metadata found" :
                                                  "local registry reports metadata not found");

        advisories_raw = read_all(root / "advisories.json");
        registry_health_check(checks, "advisories", advisories_raw.empty() ? "warn" : "ok",
                              advisories_raw.empty() ? "local registry advisories metadata not found" :
                                                       "local registry advisories metadata found");

        key_count = registry_key_count(root);
        registry_health_check(checks, "keys", key_count > 0 ? "ok" : "warn",
                              key_count > 0 ? "local registry public key store has PEM keys" :
                                              "local registry public key store has no PEM keys");
    }

    auto packages = parse_registry_packages(index_raw);
    auto owners = parse_registry_owners(owners_raw);
    auto yanks = parse_registry_yanks(yanks_raw);
    auto reports = parse_registry_reports(reports_raw);
    auto advisories = parse_registry_advisories(advisories_raw);

    size_t yanked_count = yanks.size();
    if (yanked_count == 0) {
        for (const auto& pkg : packages) {
            if (pkg.yanked) ++yanked_count;
        }
    }
    size_t open_report_count = 0;
    for (const auto& report : reports) {
        if (lowercase_copy(report.status.empty() ? "open" : report.status) == "open") ++open_report_count;
    }
    size_t active_advisory_count = 0;
    size_t critical_advisory_count = 0;
    for (const auto& advisory : advisories) {
        if (lowercase_copy(advisory.status.empty() ? "active" : advisory.status) == "active") {
            ++active_advisory_count;
            if (lowercase_copy(advisory.severity) == "critical") ++critical_advisory_count;
        }
    }

    int error_count = registry_health_error_count(checks);
    int warning_count = registry_health_warning_count(checks);
    bool passed = error_count == 0 && (!fail_on_warning || warning_count == 0);

    if (json_output) {
        std::cout << "{\n"
                  << "  \"schema\": \"sura.registry.health.v1\",\n"
                  << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
                  << "  \"fail_on_warning\": " << (fail_on_warning ? "true" : "false") << ",\n"
                  << "  \"source\": \"" << (remote ? "http" : "local") << "\",\n"
                  << "  \"registry\": \"" << json_escape(registry_ref) << "\",\n"
                  << "  \"health_endpoint_ok\": " << (health_ok ? "true" : "false") << ",\n"
                  << "  \"index_present\": " << (index_present ? "true" : "false") << ",\n"
                  << "  \"stats_present\": " << (stats_present ? "true" : "false") << ",\n"
                  << "  \"analytics_present\": " << (analytics_present ? "true" : "false") << ",\n"
                  << "  \"reports_reachable\": " << (reports_reachable ? "true" : "false") << ",\n"
                  << "  \"package_count\": " << packages.size() << ",\n"
                  << "  \"owner_count\": " << owners.size() << ",\n"
                  << "  \"yanked_count\": " << yanked_count << ",\n"
                  << "  \"report_count\": " << reports.size() << ",\n"
                  << "  \"open_report_count\": " << open_report_count << ",\n"
                  << "  \"advisory_count\": " << advisories.size() << ",\n"
                  << "  \"active_advisory_count\": " << active_advisory_count << ",\n"
                  << "  \"critical_advisory_count\": " << critical_advisory_count << ",\n"
                  << "  \"key_count\": ";
        if (remote) std::cout << "null";
        else std::cout << key_count;
        std::cout << ",\n"
                  << "  \"error_count\": " << error_count << ",\n"
                  << "  \"warning_count\": " << warning_count << ",\n"
                  << "  \"checks\": [\n";
        for (size_t i = 0; i < checks.size(); ++i) {
            const auto& check = checks[i];
            std::cout << "    {\"name\":\"" << json_escape(check.name)
                      << "\",\"status\":\"" << json_escape(check.status)
                      << "\",\"message\":\"" << json_escape(check.message) << "\"}";
            if (i + 1 < checks.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n"
                  << "}\n";
        return passed ? 0 : 1;
    }

    std::cout << "Registry health\n"
              << "  source: " << (remote ? "http" : "local") << "\n"
              << "  registry: " << registry_ref << "\n"
              << "  passed: " << (passed ? "true" : "false") << "\n"
              << "  fail_on_warning: " << (fail_on_warning ? "true" : "false") << "\n"
              << "  packages: " << packages.size() << "\n"
              << "  owners: " << owners.size() << "\n"
              << "  yanked: " << yanked_count << "\n"
              << "  reports: " << reports.size() << " open=" << open_report_count << "\n"
              << "  advisories: " << advisories.size()
              << " active=" << active_advisory_count
              << " critical=" << critical_advisory_count << "\n";
    if (!remote) std::cout << "  keys: " << key_count << "\n";
    std::cout << "Checks\n";
    for (const auto& check : checks) {
        std::cout << "  [" << check.status << "] " << check.name << " - " << check.message << "\n";
    }
    if (!passed) {
        std::string msg = "registry health failed with " + std::to_string(error_count) + " error(s)";
        if (fail_on_warning) msg += " and " + std::to_string(warning_count) + " warning(s)";
        return err(msg);
    }
    return 0;
}

static bool is_version_range_spec(const std::string& spec);

static int cmd_yank_common(const std::vector<std::string>& argv, bool unyank) {
    std::vector<std::string> positional;
    bool json_output = false;
    std::string command = unyank ? "unyank" : "yank";
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            if (unyank) std::cout << "Usage: surapkg unyank <name@version> [reason] [--json]\n";
            else std::cout << "Usage: surapkg yank <name@version> <reason> [--json]\n";
            return 0;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.empty()) return err(command + " requires name@version");
    std::string reason = safe_report_reason(join_vector_args(positional, 1));
    if (!unyank && reason.size() < 4) return err("yank reason must be at least 4 characters");

    PackageRef ref = parse_package_ref(positional[0]);
    if (ref.name.empty() || ref.version.empty() || ref.version == "latest" || is_version_range_spec(ref.version)) {
        return err(command + " requires a concrete name@version");
    }

    std::string url = registry_url();
    if (!url.empty()) {
        std::ostringstream body;
        body << "{"
             << "\"name\":\"" << json_escape(ref.name) << "\","
             << "\"version\":\"" << json_escape(ref.version) << "\","
             << "\"reason\":\"" << json_escape(reason) << "\","
             << "\"yanked\":" << (unyank ? "false" : "true")
             << "}\n";
        std::string response;
        if (!http_post_json(url + "/api/yank", body.str(), registry_token(), response)) {
            return err("HTTP registry " + command + " failed at " + url + "/api/yank; set SURA_REGISTRY_TOKEN to an admin token");
        }
        bool yanked = json_bool_field_near(response, "yanked", !unyank);
        std::string out_reason = yanked ? manifest_field(response, "reason", reason) : "";
        std::string by = yanked ? manifest_field(response, "by", "") : "";
        std::string at = yanked ? manifest_field(response, "at", "") : "";
        if (json_output) {
            return write_yank_result_json("sura.registry.yank.v1", "http", ref.name, ref.version,
                                          yanked, out_reason, by, at);
        }
        ok(std::string(yanked ? "yanked " : "unyanked ") + ref.name + "@" + ref.version);
        return 0;
    }

    fs::path root = registry_root();
    fs::path bundle = root / ref.name / ref.version / "package.surabundle.json";
    if (!fs::exists(bundle)) return err("package version not found: " + ref.name + "@" + ref.version);
    auto yanks = parse_registry_yanks(read_all(root / "yanks.json"));
    std::string key = registry_yank_key(ref.name, ref.version);
    RegistryYankRecord record;
    if (unyank) {
        yanks.erase(key);
    } else {
        record.name = ref.name;
        record.version = ref.version;
        record.reason = reason;
        record.by = "local-admin";
        record.at = utc_timestamp_now();
        yanks[key] = record;
    }
    if (!write_registry_yanks(root, yanks)) return err("failed to write " + (root / "yanks.json").generic_string());
    if (write_registry_index() != 0) return err("failed to rebuild registry index");

    if (json_output) {
        return write_yank_result_json("sura.registry.yank.v1", "local", ref.name, ref.version,
                                      !unyank, unyank ? "" : record.reason,
                                      unyank ? "" : record.by, unyank ? "" : record.at);
    }
    ok(std::string(unyank ? "unyanked " : "yanked ") + ref.name + "@" + ref.version);
    return 0;
}

static int cmd_review_report(const std::vector<std::string>& argv) {
    std::vector<std::string> positional;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage:\n  surapkg review-report <id> <open|reviewing|dismissed|actioned> [note] [--json]\n";
            return 0;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() < 2) {
        std::cout << "Usage:\n  surapkg review-report <id> <open|reviewing|dismissed|actioned> [note] [--json]\n";
        return 1;
    }
    std::string url = registry_url();
    if (url.empty()) return err("review-report requires SURA_REGISTRY_URL");
    std::string id = positional[0];
    std::string status = positional[1];
    if (id.empty() || id.find_first_of("\"\r\n") != std::string::npos) {
        return err("review-report requires a valid id");
    }
    if (status != "open" && status != "reviewing" && status != "dismissed" && status != "actioned") {
        return err("review-report status must be open, reviewing, dismissed, or actioned");
    }
    std::string note = safe_report_reason(join_vector_args(positional, 2));
    std::ostringstream json;
    json << "{"
         << "\"id\":\"" << json_escape(id) << "\","
         << "\"status\":\"" << json_escape(status) << "\","
         << "\"note\":\"" << json_escape(note) << "\""
         << "}\n";
    std::string response;
    if (!http_post_json(url + "/api/reports/review", json.str(), registry_token(), response)) {
        return err("HTTP registry report review failed at " + url + "/api/reports/review; set SURA_REGISTRY_TOKEN to an admin token");
    }
    if (json_output) {
        auto reports = parse_registry_reports(response);
        std::cout << "{\n"
                  << "  \"schema\": \"sura.registry.review_report.v1\",\n"
                  << "  \"ok\": true,\n"
                  << "  \"report\": ";
        if (reports.empty()) {
            std::cout << "null\n";
        } else {
            write_registry_report_json_object(std::cout, reports.front(), "  ");
            std::cout << "\n";
        }
        std::cout << "}\n";
        return 0;
    }
    ok("reviewed report " + id + " -> " + status);
    return 0;
}

static bool cli_secret_safe(const std::string& value) {
    return value.size() >= 8 && value.size() <= 256 &&
           value.find_first_of("\"\r\n") == std::string::npos;
}

static bool write_recover_token_json_report(const fs::path& report_path,
                                            const std::string& user,
                                            const std::string& url,
                                            const std::string& token,
                                            const std::string& recovery_code,
                                            bool requested_token_supplied,
                                            bool passed) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.registry.recover_token.v1\",\n"
        << "  \"user\": \"" << json_escape(user) << "\",\n"
        << "  \"registry_url\": \"" << json_escape(url) << "\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"requested_token_supplied\": " << (requested_token_supplied ? "true" : "false") << ",\n"
        << "  \"token_returned\": " << (!token.empty() ? "true" : "false") << ",\n"
        << "  \"recovery_code_returned\": " << (!recovery_code.empty() ? "true" : "false") << ",\n"
        << "  \"token\": \"" << json_escape(token) << "\",\n"
        << "  \"recovery_code\": \"" << json_escape(recovery_code) << "\"\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_recover_token_command(const std::string& user,
                                     const std::string& recovery_code,
                                     const std::string& new_token,
                                     const fs::path& json_report) {
    std::string url = registry_url();
    if (url.empty()) return err("recover-token requires SURA_REGISTRY_URL");
    if (user.empty() || user.find_first_of("\"\r\n") != std::string::npos) {
        return err("recover-token requires a valid user");
    }
    if (!cli_secret_safe(recovery_code)) {
        return err("recover-token requires a valid recovery code");
    }
    if (!new_token.empty() && !cli_secret_safe(new_token)) {
        return err("new token must be 8-256 chars and must not contain quotes or newlines");
    }

    std::ostringstream json;
    json << "{"
         << "\"user\":\"" << json_escape(user) << "\","
         << "\"recoveryCode\":\"" << json_escape(recovery_code) << "\"";
    if (!new_token.empty()) json << ",\"token\":\"" << json_escape(new_token) << "\"";
    json << "}\n";

    std::string response;
    if (!http_post_json(url + "/api/tokens/recover", json.str(), "", response)) {
        return err("HTTP registry token recovery failed at " + url + "/api/tokens/recover");
    }
    std::string token = manifest_field(response, "token", "");
    std::string next_recovery = manifest_field(response, "recoveryCode", "");
    if (!json_report.empty()) {
        if (!write_recover_token_json_report(json_report, user, url, token, next_recovery, !new_token.empty(), true)) {
            return err("failed to write recover-token JSON report: " + json_report.generic_string());
        }
        ok("recover-token report written: " + json_report.generic_string());
    }
    ok("recovered registry token for " + user);
    if (!token.empty()) std::cout << "token: " << token << "\n";
    if (!next_recovery.empty()) std::cout << "recoveryCode: " << next_recovery << "\n";
    return 0;
}

static int cmd_recover_token(const std::vector<std::string>& argv) {
    std::vector<std::string> positional;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("recover-token --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("recover-token --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage:\n  surapkg recover-token <user> <recovery-code> [new-token] [--json report.json]\n";
            return 0;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() < 2 || positional.size() > 3) {
        std::cout << "Usage:\n  surapkg recover-token <user> <recovery-code> [new-token] [--json report.json]\n";
        return 1;
    }
    std::string new_token = positional.size() >= 3 ? positional[2] : "";
    return run_recover_token_command(positional[0], positional[1], new_token, json_report);
}

static std::string project_name_from_cwd() {
    return normalize_name(fs::current_path().filename().string());
}

struct DependencySpec {
    std::string name;
    std::string spec;
};

static std::vector<DependencySpec> manifest_dependency_specs(const std::string& manifest) {
    std::vector<DependencySpec> deps;
    std::smatch deps_block;
    std::regex block_re("\"dependencies\"\\s*:\\s*\\{([^}]*)\\}");
    if (!std::regex_search(manifest, deps_block, block_re)) return deps;
    std::string body = deps_block[1].str();
    std::regex dep_re("\"((?:\\\\.|[^\"])*)\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), dep_re);
         it != std::sregex_iterator(); ++it) {
        deps.push_back({normalize_name(json_unescape((*it)[1].str())), json_unescape((*it)[2].str())});
    }
    return deps;
}

static std::string trim_copy(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) ++start;
    size_t end = text.size();
    while (end > start && std::isspace((unsigned char)text[end - 1])) --end;
    return text.substr(start, end - start);
}

static std::string dependency_constraint(const std::string& name, const std::string& spec) {
    std::string out = trim_copy(spec);
    if (out.rfind("file:", 0) == 0) return "";
    if (out.rfind("registry:", 0) == 0) {
        out = out.substr(9);
        std::string normalized = normalize_name(name);
        size_t at = out.rfind('@');
        if (at != std::string::npos && normalize_name(out.substr(0, at)) == normalized) {
            out = out.substr(at + 1);
        } else if (normalize_name(out) == normalized) {
            out.clear();
        }
    }
    return trim_copy(out);
}

static bool is_version_range_spec(const std::string& spec) {
    std::string text = trim_copy(spec);
    return text.find_first_of("^~<>*= ") != std::string::npos;
}

static std::string manifest_dependency_spec_for(const std::vector<DependencySpec>& deps, const std::string& name) {
    std::string normalized = normalize_name(name);
    for (const auto& dep : deps) {
        if (dep.name == normalized) return dep.spec;
    }
    return "";
}

static bool set_dependency(const std::string& name, const std::string& spec) {
    std::string manifest = read_all(kManifest);
    if (manifest.empty()) manifest = default_manifest(project_name_from_cwd());

    std::regex dep_re("(\"dependencies\"\\s*:\\s*\\{)([^}]*)(\\})");
    std::smatch match;
    std::string entry = "\"" + json_escape(name) + "\": \"" + json_escape(spec) + "\"";

    if (!std::regex_search(manifest, match, dep_re)) {
        size_t insert = manifest.rfind('}');
        if (insert == std::string::npos) return false;
        manifest.insert(insert, ",\n  \"dependencies\": {\n    " + entry + "\n  }\n");
        return write_all(kManifest, manifest);
    }

    std::string body = match[2].str();
    std::regex one_re("\"" + name + "\"\\s*:\\s*\"[^\"]*\"");
    if (std::regex_search(body, one_re)) {
        body = std::regex_replace(body, one_re, entry);
    } else {
        std::string trimmed = body;
        trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(), ::isspace), trimmed.end());
        if (trimmed.empty()) body = "\n    " + entry + "\n  ";
        else body += ",\n    " + entry + "\n  ";
    }

    manifest.replace(match.position(2), match.length(2), body);
    return write_all(kManifest, manifest);
}

static bool remove_dependency(const std::string& name) {
    std::string manifest = read_all(kManifest);
    if (manifest.empty()) return true;

    std::regex dep_re("(\"dependencies\"\\s*:\\s*\\{)([^}]*)(\\})");
    std::smatch match;
    if (!std::regex_search(manifest, match, dep_re)) return true;

    std::string body = match[2].str();
    std::regex line_re("\\s*,?\\s*\"" + name + "\"\\s*:\\s*\"[^\"]*\"\\s*,?");
    body = std::regex_replace(body, line_re, "\n  ");
    body = std::regex_replace(body, std::regex(",\\s*\\}"), "\n  }");
    manifest.replace(match.position(2), match.length(2), body);
    return write_all(kManifest, manifest);
}

static fs::path package_dir(const std::string& name) {
    return kPackages / normalize_name(name);
}

struct InstalledPackage {
    std::string name;
    std::string version;
    fs::path root;
};

static bool version_part_is_number(const std::string& part) {
    return !part.empty() && std::all_of(part.begin(), part.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

static std::vector<std::string> version_parts(const std::string& version) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : version) {
        if (ch == '.' || ch == '-' || ch == '_') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

static int compare_versions(const std::string& a, const std::string& b) {
    auto left = version_parts(a);
    auto right = version_parts(b);
    size_t count = std::max(left.size(), right.size());
    for (size_t i = 0; i < count; ++i) {
        std::string lv = i < left.size() ? left[i] : "0";
        std::string rv = i < right.size() ? right[i] : "0";
        bool ln = version_part_is_number(lv);
        bool rn = version_part_is_number(rv);
        if (ln && rn) {
            long long li = 0;
            long long ri = 0;
            try { li = std::stoll(lv); } catch (...) { li = 0; }
            try { ri = std::stoll(rv); } catch (...) { ri = 0; }
            if (li < ri) return -1;
            if (li > ri) return 1;
            continue;
        }
        std::string lc = lowercase_copy(lv);
        std::string rc = lowercase_copy(rv);
        if (lc < rc) return -1;
        if (lc > rc) return 1;
    }
    return 0;
}

static int version_number_part(const std::vector<std::string>& parts, size_t index) {
    if (index >= parts.size() || !version_part_is_number(parts[index])) return 0;
    try { return std::stoi(parts[index]); } catch (...) { return 0; }
}

static std::string dotted_version(int major, int minor, int patch) {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

static bool version_satisfies_token(const std::string& version, const std::string& raw_token) {
    std::string token = trim_copy(raw_token);
    if (token.empty() || token == "*" || token == "latest") return true;
    if (token[0] == '^') {
        std::string base = trim_copy(token.substr(1));
        auto parts = version_parts(base);
        int major = version_number_part(parts, 0);
        int minor = version_number_part(parts, 1);
        int patch = version_number_part(parts, 2);
        std::string upper;
        if (major > 0) upper = dotted_version(major + 1, 0, 0);
        else if (minor > 0) upper = dotted_version(major, minor + 1, 0);
        else upper = dotted_version(major, minor, patch + 1);
        return compare_versions(version, base) >= 0 && compare_versions(version, upper) < 0;
    }
    if (token[0] == '~') {
        std::string base = trim_copy(token.substr(1));
        auto parts = version_parts(base);
        int major = version_number_part(parts, 0);
        int minor = version_number_part(parts, 1);
        std::string upper = parts.size() <= 1 ? dotted_version(major + 1, 0, 0) : dotted_version(major, minor + 1, 0);
        return compare_versions(version, base) >= 0 && compare_versions(version, upper) < 0;
    }

    std::string op = "==";
    std::string rhs = token;
    if (token.rfind(">=", 0) == 0 || token.rfind("<=", 0) == 0 || token.rfind("==", 0) == 0) {
        op = token.substr(0, 2);
        rhs = trim_copy(token.substr(2));
    } else if (!token.empty() && (token[0] == '>' || token[0] == '<' || token[0] == '=')) {
        op = token.substr(0, 1);
        rhs = trim_copy(token.substr(1));
        if (op == "=") op = "==";
    }
    int cmp = compare_versions(version, rhs);
    if (op == ">=") return cmp >= 0;
    if (op == ">") return cmp > 0;
    if (op == "<=") return cmp <= 0;
    if (op == "<") return cmp < 0;
    return cmp == 0;
}

static bool valid_package_version_literal(const std::string& version) {
    if (version.empty() || version.size() > 64) return false;
    if (version == "latest" || is_version_range_spec(version)) return false;
    if (version.front() == '.' || version.front() == '-' || version.front() == '_' ||
        version.back() == '.' || version.back() == '-' || version.back() == '_') {
        return false;
    }
    bool has_digit = false;
    for (unsigned char ch : version) {
        if (std::isdigit(ch)) has_digit = true;
        if (!(std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_')) return false;
    }
    return has_digit;
}

static bool update_manifest_version_text(std::string& manifest, const std::string& version) {
    std::regex version_re("(\"version\"\\s*:\\s*\")((?:\\\\.|[^\"])*)\"");
    std::smatch match;
    if (std::regex_search(manifest, match, version_re)) {
        manifest.replace((size_t)match.position(2), (size_t)match.length(2), json_escape(version));
        return true;
    }

    size_t insert = manifest.rfind('}');
    if (insert == std::string::npos) return false;
    size_t before = insert;
    while (before > 0 && std::isspace((unsigned char)manifest[before - 1])) --before;
    bool needs_comma = before > 0 && manifest[before - 1] != '{' && manifest[before - 1] != ',';
    std::string entry = std::string(needs_comma ? ",\n" : "\n") +
                        "  \"version\": \"" + json_escape(version) + "\"\n";
    manifest.insert(insert, entry);
    return true;
}

static std::string bump_package_version(const std::string& current, const std::string& mode, std::string& message) {
    auto parts = version_parts(current);
    if (parts.empty() || !version_part_is_number(parts[0]) ||
        (parts.size() > 1 && !version_part_is_number(parts[1])) ||
        (parts.size() > 2 && !version_part_is_number(parts[2]))) {
        message = "current package version is not numeric enough to bump: " + current;
        return "";
    }
    int major = version_number_part(parts, 0);
    int minor = version_number_part(parts, 1);
    int patch = version_number_part(parts, 2);
    if (mode == "major") return dotted_version(major + 1, 0, 0);
    if (mode == "minor") return dotted_version(major, minor + 1, 0);
    if (mode == "patch") return dotted_version(major, minor, patch + 1);
    message = "unknown version bump mode: " + mode;
    return "";
}

static std::string package_version_report_json(const fs::path& root,
                                               const fs::path& manifest_path,
                                               const std::string& name,
                                               const std::string& previous,
                                               const std::string& version,
                                               const std::string& mode,
                                               bool changed) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.version.v1\",\n"
        << "  \"passed\": true,\n"
        << "  \"root\": \"" << json_escape(path_to_generic_utf8(root)) << "\",\n"
        << "  \"manifest\": \"" << json_escape(path_to_generic_utf8(manifest_path)) << "\",\n"
        << "  \"package\": \"" << json_escape(name) << "\",\n"
        << "  \"previous_version\": \"" << json_escape(previous) << "\",\n"
        << "  \"version\": \"" << json_escape(version) << "\",\n"
        << "  \"mode\": \"" << json_escape(mode) << "\",\n"
        << "  \"changed\": " << (changed ? "true" : "false") << "\n"
        << "}\n";
    return out.str();
}

static int cmd_version(const std::vector<std::string>& argv) {
    std::vector<std::string> positional;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("version --json requires an output path");
            json_report = fs::path(argv[++i]);
            if (json_report.empty()) return err("version --json requires an output path");
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("version --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg version [path] [major|minor|patch|version] [--json report.json]\n";
            return 0;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() > 2) {
        return err("version accepts optional path plus major|minor|patch|version");
    }

    fs::path root = fs::current_path();
    std::string requested;
    if (positional.size() == 1) {
        fs::path candidate = fs::path(positional[0]);
        if (fs::exists(candidate) && fs::is_directory(candidate)) root = candidate;
        else requested = positional[0];
    } else if (positional.size() == 2) {
        root = fs::path(positional[0]);
        requested = positional[1];
    }
    if (fs::is_regular_file(root)) root = root.parent_path();

    fs::path manifest_path = root / kManifest;
    std::string manifest = read_all(manifest_path);
    if (manifest.empty()) {
        return err("sura.pkg.json not found for version: " + path_to_generic_utf8(manifest_path));
    }
    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string previous = manifest_field(manifest, "version", "");
    if (previous.empty() && requested.empty()) {
        return err("package manifest has no version field");
    }

    std::string mode = requested.empty() ? "current" : lowercase_copy(requested);
    std::string next = previous;
    bool changed = false;
    if (!requested.empty()) {
        if (mode == "major" || mode == "minor" || mode == "patch") {
            if (previous.empty()) return err("package manifest has no version field to bump");
            std::string message;
            next = bump_package_version(previous, mode, message);
            if (next.empty()) return err(message.empty() ? "failed to bump package version" : message);
        } else {
            next = requested;
            mode = "set";
        }
        if (!valid_package_version_literal(next)) {
            return err("invalid package version: " + next);
        }
        changed = next != previous;
        if (changed) {
            if (!update_manifest_version_text(manifest, next)) {
                return err("failed to update version in " + path_to_generic_utf8(manifest_path));
            }
            if (!write_all(manifest_path, manifest)) {
                return err("failed to write " + path_to_generic_utf8(manifest_path));
            }
        }
    }

    std::string report = package_version_report_json(root, manifest_path, name, previous, next, mode, changed);
    if (!json_report.empty()) {
        if (!write_all(json_report, report)) {
            return err("failed to write version JSON report: " + path_to_generic_utf8(json_report));
        }
        ok("version report written: " + path_to_generic_utf8(json_report));
    }
    if (changed) ok("updated " + name + " " + previous + " -> " + next);
    else info(name + " version " + next);
    return 0;
}

static bool version_satisfies_constraint(const std::string& version, const std::string& raw_constraint) {
    std::string constraint = trim_copy(raw_constraint);
    if (constraint.empty() || constraint == "*" || constraint == "latest") return true;
    std::istringstream terms(constraint);
    std::string token;
    bool saw = false;
    while (terms >> token) {
        saw = true;
        if (!version_satisfies_token(version, token)) return false;
    }
    return saw;
}

static std::vector<InstalledPackage> installed_packages() {
    std::vector<InstalledPackage> out;
    if (!fs::exists(kPackages)) return out;
    for (const auto& entry : fs::directory_iterator(kPackages)) {
        if (!entry.is_directory()) continue;
        std::string manifest = read_all(entry.path() / kManifest);
        if (manifest.empty()) continue;
        InstalledPackage pkg;
        pkg.name = normalize_name(manifest_field(manifest, "name", entry.path().filename().string()));
        pkg.version = manifest_field(manifest, "version", "0.0.0");
        pkg.root = entry.path();
        out.push_back(pkg);
    }
    std::sort(out.begin(), out.end(), [](const InstalledPackage& a, const InstalledPackage& b) {
        return a.name < b.name;
    });
    return out;
}

static bool latest_registry_package(const std::vector<RegistryPackage>& packages, const std::string& name, const std::string& constraint, RegistryPackage& latest) {
    bool found = false;
    std::string normalized = normalize_name(name);
    for (const auto& pkg : packages) {
        if (normalize_name(pkg.name) != normalized || pkg.yanked) continue;
        if (!version_satisfies_constraint(pkg.version, constraint)) continue;
        if (!found || compare_versions(latest.version, pkg.version) < 0) {
            latest = pkg;
            found = true;
        }
    }
    return found;
}

static bool select_registry_package(const std::string& name, const std::string& constraint, RegistryPackage& selected, std::string& error_msg) {
    std::string index = registry_index_text();
    if (index.empty()) {
        error_msg = "registry index not found; set SURA_REGISTRY or SURA_REGISTRY_URL";
        return false;
    }
    auto packages = parse_registry_packages(index);
    if (!latest_registry_package(packages, name, constraint, selected)) {
        error_msg = "registry package not found for " + normalize_name(name);
        if (!trim_copy(constraint).empty()) error_msg += " satisfying " + trim_copy(constraint);
        return false;
    }
    return true;
}

struct OutdatedPackage {
    InstalledPackage installed;
    RegistryPackage latest;
    std::string dependency_spec;
};

static int install_package(const std::string& source, bool record_dependency);
static int install_package(const std::string& source, bool record_dependency, const fs::path& json_report);
static int cmd_install_source(const std::string& source);

static std::vector<OutdatedPackage> find_outdated_packages(const std::string& query, std::string& error_msg) {
    std::vector<OutdatedPackage> out;
    std::string index = registry_index_text();
    if (index.empty()) {
        error_msg = "registry index not found; set SURA_REGISTRY or SURA_REGISTRY_URL";
        return out;
    }
    auto registry_packages = parse_registry_packages(index);
    auto installed = installed_packages();
    auto deps = manifest_dependency_specs(read_all(kManifest));
    std::string needle = normalize_name(query);
    bool saw_requested = false;
    for (const auto& pkg : installed) {
        if (!needle.empty() && pkg.name != needle) continue;
        saw_requested = true;
        std::string spec = manifest_dependency_spec_for(deps, pkg.name);
        std::string constraint = dependency_constraint(pkg.name, spec);
        RegistryPackage latest;
        if (!latest_registry_package(registry_packages, pkg.name, constraint, latest)) continue;
        if (compare_versions(pkg.version, latest.version) < 0) {
            out.push_back({pkg, latest, spec});
        }
    }
    if (!needle.empty() && !saw_requested) {
        error_msg = "installed package not found: " + needle;
    }
    return out;
}

static int cmd_outdated(const std::vector<std::string>& argv) {
    std::string query;
    bool query_set = false;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        std::string arg = argv[i];
        if (arg == "--json") {
            json_output = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg outdated [name] [--json]\n";
            return 0;
        }
        if (!query_set && (arg.empty() || arg[0] != '-')) {
            query = arg;
            query_set = true;
            continue;
        }
        return err("outdated accepts one optional package name and optional --json");
    }

    std::string error_msg;
    auto outdated = find_outdated_packages(query, error_msg);
    if (!error_msg.empty()) return err(error_msg);
    if (json_output) {
        std::ostringstream out;
        out << "{\n"
            << "  \"schema\": \"sura.package.outdated.v1\",\n"
            << "  \"query\": \"" << json_escape(normalize_name(query)) << "\",\n"
            << "  \"up_to_date\": " << (outdated.empty() ? "true" : "false") << ",\n"
            << "  \"count\": " << outdated.size() << ",\n"
            << "  \"packages\": [";
        for (size_t i = 0; i < outdated.size(); ++i) {
            const auto& item = outdated[i];
            if (i) out << ",";
            out << "\n"
                << "    {\n"
                << "      \"name\": \"" << json_escape(item.installed.name) << "\",\n"
                << "      \"current\": \"" << json_escape(item.installed.version) << "\",\n"
                << "      \"latest\": \"" << json_escape(item.latest.version) << "\",\n"
                << "      \"dependency_spec\": \"" << json_escape(item.dependency_spec) << "\",\n"
                << "      \"bundle\": \"" << json_escape(item.latest.bundle) << "\",\n"
                << "      \"hash\": \"" << json_escape(item.latest.hash) << "\",\n"
                << "      \"owner\": \"" << json_escape(item.latest.owner) << "\"\n"
                << "    }";
        }
        if (!outdated.empty()) out << "\n  ";
        out << "]\n}\n";
        std::cout << out.str();
        return 0;
    }
    if (outdated.empty()) {
        std::cout << "All packages are up to date";
        if (!query.empty()) std::cout << " for " << normalize_name(query);
        std::cout << "\n";
        return 0;
    }
    std::cout << "Package  Current  Latest\n";
    for (const auto& item : outdated) {
        std::cout << item.installed.name << "  "
                  << item.installed.version << "  "
                  << item.latest.version << "\n";
    }
    return 0;
}

struct UpdatedPackageReport {
    std::string name;
    std::string previous;
    std::string installed;
    std::string dependency_spec;
    std::string bundle;
    std::string hash;
    std::string owner;
};

static bool write_update_json_report(const fs::path& report_path,
                                     const std::string& query,
                                     bool passed,
                                     int failed,
                                     const std::vector<UpdatedPackageReport>& updated) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.update.v1\",\n"
        << "  \"query\": \"" << json_escape(normalize_name(query)) << "\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"updated_count\": " << updated.size() << ",\n"
        << "  \"failed_count\": " << failed << ",\n"
        << "  \"packages\": [";
    for (size_t i = 0; i < updated.size(); ++i) {
        const auto& item = updated[i];
        if (i) out << ",";
        out << "\n"
            << "    {\n"
            << "      \"name\": \"" << json_escape(item.name) << "\",\n"
            << "      \"previous\": \"" << json_escape(item.previous) << "\",\n"
            << "      \"installed\": \"" << json_escape(item.installed) << "\",\n"
            << "      \"dependency_spec\": \"" << json_escape(item.dependency_spec) << "\",\n"
            << "      \"bundle\": \"" << json_escape(item.bundle) << "\",\n"
            << "      \"hash\": \"" << json_escape(item.hash) << "\",\n"
            << "      \"owner\": \"" << json_escape(item.owner) << "\"\n"
            << "    }";
    }
    if (!updated.empty()) out << "\n  ";
    out << "]\n}\n";
    return write_all(report_path, out.str());
}

static int run_update_command(const std::string& query, const fs::path& json_report) {
    std::string error_msg;
    auto outdated = find_outdated_packages(query, error_msg);
    if (!error_msg.empty()) return err(error_msg);
    if (outdated.empty()) {
        if (!json_report.empty()) {
            std::vector<UpdatedPackageReport> updated;
            if (!write_update_json_report(json_report, query, true, 0, updated)) {
                return err("failed to write update JSON report: " + json_report.generic_string());
            }
            ok("update report written: " + json_report.generic_string());
        }
        std::cout << "No package updates available";
        if (!query.empty()) std::cout << " for " << normalize_name(query);
        std::cout << "\n";
        return 0;
    }
    int failed = 0;
    std::vector<UpdatedPackageReport> updated;
    for (const auto& item : outdated) {
        std::string ref = item.installed.name + "@" + item.latest.version;
        std::cout << "Updating " << item.installed.name << " "
                  << item.installed.version << " -> " << item.latest.version << "\n";
        if (cmd_install_source(ref) != 0) {
            ++failed;
        } else if (!item.dependency_spec.empty()) {
            set_dependency(item.installed.name, item.dependency_spec);
            updated.push_back({item.installed.name, item.installed.version, item.latest.version,
                               item.dependency_spec, item.latest.bundle, item.latest.hash,
                               item.latest.owner});
        } else {
            updated.push_back({item.installed.name, item.installed.version, item.latest.version,
                               item.dependency_spec, item.latest.bundle, item.latest.hash,
                               item.latest.owner});
        }
    }
    if (!json_report.empty()) {
        if (!write_update_json_report(json_report, query, failed == 0, failed, updated)) {
            return err("failed to write update JSON report: " + json_report.generic_string());
        }
        ok("update report written: " + json_report.generic_string());
    }
    if (failed) return err("package update failed");
    ok("updated " + std::to_string(outdated.size()) + " package(s)");
    return 0;
}

static int cmd_update(const std::vector<std::string>& argv) {
    std::string query;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("update --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("update --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg update [name] [--json report.json]\n";
            return 0;
        } else if (query.empty()) {
            query = arg;
        } else {
            return err("update accepts one optional package name and optional --json");
        }
    }
    return run_update_command(query, json_report);
}

struct DoctorItem {
    std::string status;
    std::string message;
};

struct DoctorReport {
    int ok_count = 0;
    int warnings = 0;
    int errors = 0;
    std::vector<DoctorItem> items;
};

static void doctor_ok(DoctorReport& report, const std::string& msg) {
    ++report.ok_count;
    report.items.push_back({"ok", msg});
    std::cout << "[OK] " << msg << "\n";
}

static void doctor_warn(DoctorReport& report, const std::string& msg) {
    ++report.warnings;
    report.items.push_back({"warning", msg});
    std::cout << "[warn] " << msg << "\n";
}

static void doctor_error(DoctorReport& report, const std::string& msg) {
    ++report.errors;
    report.items.push_back({"error", msg});
    std::cout << "[error] " << msg << "\n";
}

static bool write_doctor_json_report(const fs::path& report_path,
                                     const fs::path& root,
                                     const DoctorReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"sura.doctor.v1\",\n";
    out << "  \"cwd\": \"" << json_escape(fs::current_path().generic_string()) << "\",\n";
    out << "  \"root\": \"" << json_escape(root.generic_string()) << "\",\n";
    out << "  \"passed\": " << (report.errors == 0 ? "true" : "false") << ",\n";
    out << "  \"ok_count\": " << report.ok_count << ",\n";
    out << "  \"warning_count\": " << report.warnings << ",\n";
    out << "  \"error_count\": " << report.errors << ",\n";
    out << "  \"items\": [\n";
    for (size_t i = 0; i < report.items.size(); ++i) {
        const auto& item = report.items[i];
        out << "    {\"status\":\"" << json_escape(item.status)
            << "\",\"message\":\"" << json_escape(item.message) << "\"}";
        if (i + 1 < report.items.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return write_all(report_path, out.str());
}

static std::string first_output_line(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || std::isspace((unsigned char)line.back()))) {
            line.pop_back();
        }
        size_t start = 0;
        while (start < line.size() && std::isspace((unsigned char)line[start])) ++start;
        if (start < line.size()) return line.substr(start);
    }
    return "";
}

static bool command_available(const std::string& command) {
    if (command.empty() || command.find_first_of("\"&|<>\r\n") != std::string::npos) return false;
#ifdef _WIN32
    std::string out = run_capture_command("where " + command + " 2>NUL");
#else
    std::string out = run_capture_command("command -v " + command + " 2>/dev/null");
#endif
    return !first_output_line(out).empty();
}

static std::vector<std::string> nonempty_output_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || std::isspace((unsigned char)line.back()))) {
            line.pop_back();
        }
        size_t start = 0;
        while (start < line.size() && std::isspace((unsigned char)line[start])) ++start;
        if (start < line.size()) lines.push_back(line.substr(start));
    }
    return lines;
}

static std::vector<std::string> doctor_command_sources(const std::string& command) {
    std::vector<std::string> sources;
    auto add_sources = [&](const std::string& output) {
        for (const auto& line : nonempty_output_lines(output)) {
            if (std::find(sources.begin(), sources.end(), line) == sources.end()) sources.push_back(line);
        }
    };
#ifdef _WIN32
    add_sources(run_capture_command("powershell -NoProfile -ExecutionPolicy Bypass -Command \"Get-Command " + command + " -All | ForEach-Object { if ($_.Source) { $_.Source } elseif ($_.Path) { $_.Path } elseif ($_.Definition) { $_.Definition } }\" 2>NUL"));
    add_sources(run_capture_command("where " + command + " 2>NUL"));
#else
    add_sources(run_capture_command("command -v " + command + " 2>/dev/null"));
#endif
    return sources;
}

static bool doctor_source_looks_like_sura_command(const std::string& source) {
    if (source.empty()) return false;
    fs::path path = utf8_path(source);
    std::string filename = path.filename().string();
    std::transform(filename.begin(), filename.end(), filename.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (filename == "surafinal.exe" || filename == "surafinal" ||
        filename == "suraengine.exe" || filename == "suraengine") {
        return true;
    }
    fs::path dir = path.has_parent_path() ? path.parent_path() : fs::path();
    if (dir.empty()) return false;
#ifdef _WIN32
    return fs::exists(dir / "SuraLanguage.exe") || fs::exists(dir / "SuraEngine.exe");
#else
    return fs::exists(dir / "SuraLanguage") || fs::exists(dir / "SuraEngine");
#endif
}

static void doctor_check_sura_command(DoctorReport& report) {
    std::vector<std::string> sources = doctor_command_sources("sura");
    if (sources.empty()) {
        doctor_warn(report, "sura command not found on PATH; run the Windows installer or add the Sura bin directory before using `sura app.sura`");
        return;
    }

    const std::string first = sources.front();
    if (doctor_source_looks_like_sura_command(first)) {
        doctor_ok(report, "sura command: " + first);
        return;
    }

    std::string later_sura;
    for (size_t i = 1; i < sources.size(); ++i) {
        if (doctor_source_looks_like_sura_command(sources[i])) {
            later_sura = sources[i];
            break;
        }
    }

    if (!later_sura.empty()) {
        doctor_warn(report, "sura command shadowed: first PATH match is " + first + "; installed Sura command also found at " + later_sura + "; move the Sura bin directory earlier in PATH or rename the conflicting launcher");
    } else {
        doctor_warn(report, "sura command points to a non-Sura launcher: " + first + "; reinstall Sura or update PATH so `sura` resolves to the Sura bin directory");
    }
}

static fs::path current_executable_dir() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    unsigned long got = GetModuleFileNameW(nullptr, buf.data(), (unsigned long)buf.size());
    if (got == 0 || got >= buf.size()) return {};
    buf.resize((size_t)got);
    return fs::path(buf).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return {};
    std::vector<char> buf((size_t)size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    fs::path executable = fs::weakly_canonical(fs::path(buf.data()), ec);
    if (ec) executable = fs::path(buf.data());
    return executable.parent_path();
#elif defined(__linux__)
    std::vector<char> buf(4096, '\0');
    ssize_t got = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (got <= 0 || (size_t)got >= buf.size()) return {};
    buf[(size_t)got] = '\0';
    return fs::path(buf.data()).parent_path();
#else
    return {};
#endif
}

static fs::path doctor_engine_path() {
    std::vector<fs::path> candidates;
#ifdef _WIN32
    candidates.push_back("SuraLanguage.exe");
    candidates.push_back("SuraEngine.exe");
    fs::path exe_dir = current_executable_dir();
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir / "SuraLanguage.exe");
        candidates.push_back(exe_dir / "SuraEngine.exe");
    }
#else
    candidates.push_back("SuraLanguage");
    candidates.push_back("SuraEngine");
#endif
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

static fs::path doctor_stdlib_path() {
    if (fs::exists(kStdlib) && fs::is_directory(kStdlib)) return kStdlib;
    fs::path exe_dir = current_executable_dir();
    if (!exe_dir.empty()) {
        std::vector<fs::path> candidates = {exe_dir / kStdlib, exe_dir.parent_path() / kStdlib};
        for (const auto& candidate : candidates) {
            if (fs::exists(candidate) && fs::is_directory(candidate)) return candidate;
        }
    }
    return {};
}

static std::string doctor_platform_name() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unix";
#endif
}

static std::string doctor_first_available_command(const std::vector<std::string>& commands) {
    for (const auto& command : commands) {
        if (command_available(command)) return command;
    }
    return "";
}

static std::string doctor_cxx_command() {
#ifdef _WIN32
    if (fs::exists("C:/msys64/mingw64/bin/g++.exe")) return "C:/msys64/mingw64/bin/g++.exe";
#endif
    return doctor_first_available_command({"c++", "g++", "clang++"});
}

static std::string doctor_powershell_command() {
#ifdef _WIN32
    std::string ps = doctor_first_available_command({"pwsh", "powershell"});
#else
    std::string ps = doctor_first_available_command({"pwsh"});
#endif
    return ps;
}

static fs::path doctor_package_root(const std::string& source) {
    fs::path root = source.empty() ? fs::current_path() : fs::path(source);
    if (fs::is_regular_file(root)) root = root.parent_path();
    return root;
}

static int run_doctor_command(const std::string& source, const fs::path& json_report) {
    DoctorReport report;
    std::cout << "Sura doctor\n";
    doctor_ok(report, "cwd: " + fs::current_path().generic_string());
    doctor_ok(report, "platform: " + doctor_platform_name());

    fs::path engine = doctor_engine_path();
    if (engine.empty()) {
        doctor_error(report, "Sura engine not found; build SuraLanguage.exe on Windows with build.bat or SuraLanguage on Linux/macOS with c++/g++");
    } else {
        doctor_ok(report, "engine: " + engine.generic_string());
    }
    doctor_check_sura_command(report);

    if (fs::exists("build.bat")) doctor_ok(report, "Windows build script: build.bat");
    else doctor_warn(report, "Windows build script not found in current directory");
    std::string cxx = doctor_cxx_command();
    if (!cxx.empty()) {
        doctor_ok(report, "C++ compiler: " + cxx);
#ifdef _WIN32
        doctor_ok(report, "native build command: build.bat");
#elif defined(__linux__)
        doctor_ok(report, "native build command: c++ -std=c++17 -O2 main.cpp gc.cpp -o SuraLanguage -ldl");
#else
        doctor_ok(report, "native build command: c++ -std=c++17 -O2 main.cpp gc.cpp -o SuraLanguage");
#endif
    } else {
        doctor_warn(report, "C++ compiler not found; install c++/g++/clang++ for native builds and embedding");
    }
    std::string ps = doctor_powershell_command();
    if (!ps.empty()) doctor_ok(report, "PowerShell runner: " + ps);
    else doctor_warn(report, "PowerShell runner not found; install PowerShell 7 (pwsh) for cross-platform tool scripts");
    if (fs::exists(".github/workflows/cross-platform-smoke.yml")) {
        doctor_ok(report, "cross-platform CI workflow: .github/workflows/cross-platform-smoke.yml");
    }

    fs::path stdlib_path = doctor_stdlib_path();
    if (!stdlib_path.empty()) {
        doctor_ok(report, "stdlib directory: " + path_to_generic_utf8(stdlib_path));
    } else {
        doctor_warn(report, "stdlib directory not found near current directory or installed runtime; built-in runtime modules still work, source checkout stdlib docs/tests may need the repo root");
    }

    bool has_curl = command_available("curl");
    bool has_node = command_available("node");
    bool has_openssl = command_available("openssl");
#ifdef _WIN32
    bool has_hash_tool = command_available("certutil");
    const char* hash_tool = "certutil";
#else
    bool has_sha256sum = command_available("sha256sum");
    bool has_shasum = command_available("shasum");
    bool has_hash_tool = has_sha256sum || has_shasum;
    const char* hash_tool = has_sha256sum ? "sha256sum" : "shasum";
#endif
    if (has_curl) doctor_ok(report, "curl available for HTTP registry and HTTP helpers");
    else doctor_warn(report, "curl not found; HTTP registry install/publish/search will fail");
    if (has_node) doctor_ok(report, "node available for tools/sura_registry_server.ps1 tokenized API");
    else doctor_warn(report, "node not found; use registry static mode or install Node for tokenized registry API");
    if (has_hash_tool) doctor_ok(report, std::string(hash_tool) + " available for package hashes/signatures");
    else doctor_warn(report, std::string(hash_tool) + " not found; package sign/verify may fail");
    if (has_openssl) doctor_ok(report, "openssl available for public-key package signatures");
    else doctor_warn(report, "openssl not found; SURA_SIGNING_PRIVATE_KEY and SURA_SIGNING_PUBLIC_KEY signatures will fail");

    fs::path root = doctor_package_root(source);
    fs::path manifest_path = root / kManifest;
    std::string manifest = read_all(manifest_path);
    if (manifest.empty()) {
        doctor_warn(report, "package manifest not found at " + manifest_path.generic_string() + "; run surapkg init for a package");
    } else {
        std::string name = normalize_name(manifest_field(manifest, "name", ""));
        std::string version = manifest_field(manifest, "version", "0.0.0");
        if (name.empty()) {
            doctor_error(report, "package manifest is missing name");
        } else {
            doctor_ok(report, "package manifest: " + name + "@" + version);
        }
        std::string main_file = manifest_field(manifest, "main", "");
        if (main_file.empty()) {
            doctor_warn(report, "package manifest has no main field");
        } else if (fs::exists(root / main_file)) {
            doctor_ok(report, "package main: " + (root / main_file).generic_string());
        } else {
            doctor_error(report, "package main file missing: " + (root / main_file).generic_string());
        }

        auto deps = manifest_dependency_specs(manifest);
        if (deps.empty()) {
            doctor_ok(report, "dependencies: none");
        } else {
            int missing = 0;
            int mismatched = 0;
            for (const auto& dep : deps) {
                fs::path dep_root = root / kPackages / dep.name;
                if (!fs::exists(dep_root)) {
                    ++missing;
                    continue;
                }
                std::string installed_manifest = read_all(dep_root / kManifest);
                std::string installed_version = manifest_field(installed_manifest, "version", "0.0.0");
                std::string constraint = dependency_constraint(dep.name, dep.spec);
                if (!version_satisfies_constraint(installed_version, constraint)) ++mismatched;
            }
            if (missing == 0 && mismatched == 0) doctor_ok(report, "dependencies installed and satisfy constraints: " + std::to_string(deps.size()));
            else doctor_warn(report, "dependency issues: " + std::to_string(missing) + " missing, " + std::to_string(mismatched) + " version mismatched; run surapkg restore");
        }

        if (fs::exists(root / kLockfile)) doctor_ok(report, "lockfile: " + (root / kLockfile).generic_string());
        else if (!deps.empty()) doctor_warn(report, "lockfile missing for package with dependencies; run surapkg lock");
    }

    std::string url = registry_url();
    if (url.empty()) {
        fs::path reg = registry_root();
        if (fs::exists(reg)) doctor_ok(report, "local registry root: " + reg.generic_string());
        else doctor_warn(report, "local registry root not found: " + reg.generic_string());
        std::string index = read_all(reg / "index.json");
        if (!index.empty()) {
            doctor_ok(report, "registry index packages: " + std::to_string(parse_registry_packages(index).size()));
        } else {
            doctor_warn(report, "registry index not found; run surapkg index after publishing packages");
        }
        std::string stats = read_all(reg / "stats.json");
        if (!stats.empty()) doctor_ok(report, "registry stats: " + (reg / "stats.json").generic_string());
        else doctor_warn(report, "registry stats not found; tokenized registry API writes download/publish stats");
    } else {
        doctor_ok(report, "SURA_REGISTRY_URL: " + url);
        if (!has_curl) {
            doctor_error(report, "SURA_REGISTRY_URL is set but curl is not available");
        } else {
            std::string index = http_get_text(url + "/index.json");
            if (!index.empty()) doctor_ok(report, "HTTP registry index packages: " + std::to_string(parse_registry_packages(index).size()));
            else doctor_error(report, "HTTP registry index unreachable: " + url + "/index.json");

            std::string stats = http_get_text(url + "/api/stats");
            if (!stats.empty()) doctor_ok(report, "HTTP registry stats reachable");
            else doctor_warn(report, "HTTP registry stats endpoint not reachable: " + url + "/api/stats");
        }
        if (registry_token().empty()) doctor_warn(report, "SURA_REGISTRY_TOKEN not set; HTTP publish will be read-only");
        else doctor_ok(report, "SURA_REGISTRY_TOKEN is set");
    }

    if (fs::exists("tools/sura_test.ps1")) doctor_ok(report, "test runner: tools/sura_test.ps1");
    else doctor_warn(report, "tools/sura_test.ps1 not found");
    if (fs::exists("tools/sura_registry_server.ps1")) doctor_ok(report, "registry server helper: tools/sura_registry_server.ps1");
    else doctor_warn(report, "tools/sura_registry_server.ps1 not found");
    if (fs::exists("sura-vscode/package.json")) doctor_ok(report, "VS Code extension project: sura-vscode");
    else doctor_warn(report, "VS Code extension project not found");

    std::cout << "doctor: " << report.ok_count << " ok, "
              << report.warnings << " warnings, "
              << report.errors << " errors\n";
    if (!json_report.empty()) {
        if (!write_doctor_json_report(json_report, root, report)) {
            return err("failed to write doctor JSON report: " + json_report.generic_string());
        }
        ok("doctor report written: " + json_report.generic_string());
    }
    return report.errors == 0 ? 0 : 1;
}

static int cmd_doctor(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("doctor --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("doctor --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg doctor [path] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("doctor accepts at most one path");
        }
    }
    return run_doctor_command(source, json_report);
}

static bool write_scaffold_json_report(const fs::path& report_path,
                                       const std::string& schema,
                                       const std::string& pkg_name,
                                       const fs::path& root,
                                       const std::vector<std::pair<fs::path, std::string>>& files) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"" << json_escape(schema) << "\",\n"
        << "  \"passed\": true,\n"
        << "  \"package\": \"" << json_escape(pkg_name) << "\",\n"
        << "  \"root\": \"" << json_escape(path_to_generic_utf8(root)) << "\",\n"
        << "  \"manifest\": \"" << json_escape(path_to_generic_utf8(root / kManifest)) << "\",\n"
        << "  \"main\": \"" << json_escape(path_to_generic_utf8(root / "src" / (pkg_name + ".sura"))) << "\",\n"
        << "  \"file_count\": " << files.size() << ",\n"
        << "  \"files\": [\n";
    for (size_t i = 0; i < files.size(); ++i) {
        out << "    {\"path\":\"" << json_escape(path_to_generic_utf8(files[i].first))
            << "\",\"kind\":\"" << json_escape(files[i].second) << "\"}";
        if (i + 1 < files.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_init_command(const std::string& name, const fs::path& json_report) {
    if (fs::exists(kManifest)) return err("sura.pkg.json already exists");
    std::string pkg_name = name.empty() ? project_name_from_cwd() : normalize_name(name);
    if (!write_all(kManifest, default_manifest(pkg_name))) return err("failed to write sura.pkg.json");
    fs::create_directories("src");
    fs::path main_file = fs::path("src") / (pkg_name + ".sura");
    if (!fs::exists(main_file)) write_all(main_file, "print \"hello from " + pkg_name + "\"\n");
    std::vector<std::pair<fs::path, std::string>> generated_files = {
        {kManifest, "manifest"},
        {main_file, "main"}
    };
    if (!json_report.empty()) {
        if (!write_scaffold_json_report(json_report, "sura.package.init.v1", pkg_name,
                                        fs::current_path(), generated_files)) {
            return err("failed to write init JSON report: " + path_to_generic_utf8(json_report));
        }
        ok("init report written: " + path_to_generic_utf8(json_report));
    }
    ok("initialized package " + pkg_name);
    return 0;
}

static int cmd_init(const std::vector<std::string>& argv) {
    std::string name;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("init --json requires an output path");
            json_report = utf8_path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = utf8_path(arg.substr(7));
            if (json_report.empty()) return err("init --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg init [name] [--json report.json]\n";
            return 0;
        } else if (name.empty()) {
            name = arg;
        } else {
            return err("init accepts at most one package name and optional --json");
        }
    }
    return run_init_command(name, json_report);
}

static int run_create_command(const std::string& name, const fs::path& json_report) {
    if (name.empty()) return err("create requires a package name");
    std::string pkg_name = normalize_name(name);
    fs::path root = pkg_name;
    if (fs::exists(root)) return err(root.string() + " already exists");
    fs::create_directories(root / "src");
    write_all(root / kManifest, default_manifest(pkg_name));
    fs::path main_file = root / "src" / (pkg_name + ".sura");
    write_all(main_file,
        "func " + pkg_name + "_hello do\n"
        "  print \"hello from " + pkg_name + "\"\n"
        "end\n");
    std::vector<std::pair<fs::path, std::string>> generated_files = {
        {root / kManifest, "manifest"},
        {main_file, "main"}
    };
    if (!json_report.empty()) {
        if (!write_scaffold_json_report(json_report, "sura.package.create.v1", pkg_name,
                                        root, generated_files)) {
            return err("failed to write create JSON report: " + path_to_generic_utf8(json_report));
        }
        ok("create report written: " + path_to_generic_utf8(json_report));
    }
    ok("created package skeleton " + path_to_generic_utf8(root));
    return 0;
}

static int cmd_create(const std::vector<std::string>& argv) {
    std::string name;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("create --json requires an output path");
            json_report = utf8_path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = utf8_path(arg.substr(7));
            if (json_report.empty()) return err("create --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg create <name> [--json report.json]\n";
            return 0;
        } else if (name.empty()) {
            name = arg;
        } else {
            return err("create accepts one package name and optional --json");
        }
    }
    return run_create_command(name, json_report);
}

static int run_new_command(const std::string& name, const fs::path& json_report) {
    if (name.empty()) return err("new requires a project name");
    std::string pkg_name = normalize_name(name);
    fs::path root = utf8_path(pkg_name);
    if (fs::exists(root)) return err(path_to_generic_utf8(root) + " already exists");

    std::error_code dir_ec;
    fs::create_directories(root / "src", dir_ec);
    if (dir_ec) return err("failed to create starter source directory: " + dir_ec.message());
    fs::create_directories(root / "tests", dir_ec);
    if (dir_ec) return err("failed to create starter test directory: " + dir_ec.message());
    fs::create_directories(root / ".vscode", dir_ec);
    if (dir_ec) return err("failed to create starter VS Code directory: " + dir_ec.message());

    std::vector<std::pair<fs::path, std::string>> generated_files;
    auto write_starter_file = [&](const fs::path& relative,
                                  const std::string& content,
                                  const std::string& kind) -> bool {
        fs::path target = root / relative;
        if (!write_all(target, content)) return false;
        generated_files.push_back({target, kind});
        return true;
    };

    const std::string manifest =
        "{\n"
        "  \"name\": \"" + json_escape(pkg_name) + "\",\n"
        "  \"version\": \"0.1.0\",\n"
        "  \"main\": \"src/" + json_escape(pkg_name) + ".sura\",\n"
        "  \"description\": \"Sura starter project\",\n"
        "  \"dependencies\": {}\n"
        "}\n";

    const std::string main_source =
        "import \"./greeting.sura\"\n\n"
        "args is argv()\n"
        "name is \"Sura\"\n"
        "if length(args) > 0 then\n"
        "  name is args[0]\n"
        "end\n\n"
        "print(greet(name))\n";

    const std::string library_source =
        "func greet(name: string) -> string do\n"
        "  return \"Hello, {name}!\"\n"
        "end\n";

    const std::string test_source =
        "import \"../src/greeting.sura\"\n\n"
        "assert_eq(greet(\"Sura\"), \"Hello, Sura!\")\n"
        "assert_eq(greet(\"World\"), \"Hello, World!\")\n";

    const std::string readme =
        "# " + pkg_name + "\n\n"
        "A starter project created by Sura Package Manager.\n\n"
        "## Run\n\n"
        "```powershell\n"
        "surapkg run\n"
        "surapkg run -- Sura\n"
        "```\n\n"
        "## Test\n\n"
        "```powershell\n"
        "surapkg test\n"
        "```\n\n"
        "Open this folder in VS Code to use the included Run, Test, and Debug settings.\n";

    const std::string gitignore =
        "sura-test-report.json\n"
        "sura-run-report.json\n"
        "artifacts/\n"
        "packages/\n";

    const std::string vscode_extensions =
        "{\n"
        "  \"recommendations\": [\"sura-team.sura-language\"]\n"
        "}\n";

    const std::string vscode_settings =
        "{\n"
        "  \"files.encoding\": \"utf8\",\n"
        "  \"files.associations\": {\"*.sura\": \"sura\"}\n"
        "}\n";

    const std::string vscode_launch =
        "{\n"
        "  \"version\": \"0.2.0\",\n"
        "  \"configurations\": [\n"
        "    {\n"
        "      \"type\": \"sura\",\n"
        "      \"request\": \"launch\",\n"
        "      \"name\": \"Debug Sura Starter\",\n"
        "      \"program\": \"${workspaceFolder}/src/" + json_escape(pkg_name) + ".sura\",\n"
        "      \"cwd\": \"${workspaceFolder}\",\n"
        "      \"stopOnEntry\": false\n"
        "    }\n"
        "  ]\n"
        "}\n";

    const std::string vscode_tasks =
        "{\n"
        "  \"version\": \"2.0.0\",\n"
        "  \"tasks\": [\n"
        "    {\n"
        "      \"label\": \"Sura: Run package\",\n"
        "      \"type\": \"shell\",\n"
        "      \"command\": \"surapkg\",\n"
        "      \"args\": [\"run\"],\n"
        "      \"options\": {\"cwd\": \"${workspaceFolder}\"},\n"
        "      \"problemMatcher\": []\n"
        "    },\n"
        "    {\n"
        "      \"label\": \"Sura: Test package\",\n"
        "      \"type\": \"shell\",\n"
        "      \"command\": \"surapkg\",\n"
        "      \"args\": [\"test\"],\n"
        "      \"options\": {\"cwd\": \"${workspaceFolder}\"},\n"
        "      \"problemMatcher\": [],\n"
        "      \"group\": {\"kind\": \"test\", \"isDefault\": true}\n"
        "    }\n"
        "  ]\n"
        "}\n";

    if (!write_starter_file(kManifest, manifest, "manifest") ||
        !write_starter_file(fs::path("src") / (pkg_name + ".sura"), main_source, "main") ||
        !write_starter_file(fs::path("src") / "greeting.sura", library_source, "library") ||
        !write_starter_file(fs::path("tests") / "greeting_test.sura", test_source, "test") ||
        !write_starter_file("README.md", readme, "readme") ||
        !write_starter_file(".gitignore", gitignore, "gitignore") ||
        !write_starter_file(fs::path(".vscode") / "extensions.json", vscode_extensions, "vscode") ||
        !write_starter_file(fs::path(".vscode") / "settings.json", vscode_settings, "vscode") ||
        !write_starter_file(fs::path(".vscode") / "launch.json", vscode_launch, "vscode") ||
        !write_starter_file(fs::path(".vscode") / "tasks.json", vscode_tasks, "vscode")) {
        return err("failed to write starter project files under " + path_to_generic_utf8(root));
    }

    if (!json_report.empty()) {
        if (!write_scaffold_json_report(json_report, "sura.package.new.v1", pkg_name,
                                        root, generated_files)) {
            return err("failed to write new JSON report: " + path_to_generic_utf8(json_report));
        }
        ok("new report written: " + path_to_generic_utf8(json_report));
    }

    ok("created starter project " + path_to_generic_utf8(root));
    info("next: cd " + path_to_generic_utf8(root));
    info("run:  surapkg run");
    info("test: surapkg test");
    return 0;
}

static int cmd_new(const std::vector<std::string>& argv) {
    std::string name;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("new --json requires an output path");
            json_report = utf8_path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = utf8_path(arg.substr(7));
            if (json_report.empty()) return err("new --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg new <name> [--json report.json]\n";
            return 0;
        } else if (name.empty()) {
            name = arg;
        } else {
            return err("new accepts one project name and optional --json");
        }
    }
    return run_new_command(name, json_report);
}

struct ExampleRecord {
    std::string id;
    std::string category;
    std::string title;
    fs::path source_path;
    std::vector<std::string> requirements;
};

static fs::path resolve_examples_root() {
    std::vector<fs::path> candidates;
#ifdef _WIN32
    std::string configured = getenv_utf8(L"SURA_EXAMPLES");
#else
    const char* raw = std::getenv("SURA_EXAMPLES");
    std::string configured = raw ? raw : "";
#endif
    if (!configured.empty()) candidates.push_back(utf8_path(configured));
    candidates.push_back(fs::current_path() / "examples");
    fs::path exe_dir = current_executable_dir();
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir / "examples");
        candidates.push_back(exe_dir.parent_path() / "examples");
    }

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_directory(candidate, ec) && !ec) {
            fs::path canonical = fs::weakly_canonical(candidate, ec);
            return ec ? candidate : canonical;
        }
    }
    return {};
}

static std::string example_title(std::string text) {
    bool capitalize = true;
    for (char& ch : text) {
        if (ch == '_' || ch == '-') {
            ch = ' ';
            capitalize = true;
        } else if (capitalize && std::isalpha((unsigned char)ch)) {
            ch = (char)std::toupper((unsigned char)ch);
            capitalize = false;
        } else {
            capitalize = ch == ' ';
        }
    }
    return text;
}

static std::vector<std::string> example_requirements(const std::string& source) {
    std::vector<std::string> out;
    std::string lower = lowercase_copy(source);
    if (lower.find("win_init(") != std::string::npos) out.push_back("windows-graphics");
    if (lower.find("sura_ffmpeg") != std::string::npos ||
        lower.find("ffmpeg was not found") != std::string::npos) out.push_back("ffmpeg");
    if (lower.find("device: \"cuda\"") != std::string::npos ||
        lower.find("cuda_info(") != std::string::npos) out.push_back("cuda");
    return out;
}

static std::vector<ExampleRecord> discover_examples(const fs::path& root) {
    std::vector<ExampleRecord> examples;
    if (root.empty()) return examples;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (it != end) {
        if (ec) {
            ec.clear();
            it.increment(ec);
            continue;
        }
        const fs::directory_entry entry = *it;
        it.increment(ec);
        if (!entry.is_regular_file() || entry.path().extension() != ".sura") continue;

        fs::path relative = fs::relative(entry.path(), root, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        relative.replace_extension();
        std::string id = path_to_generic_utf8(relative);
        std::string category = "general";
        auto component = relative.begin();
        if (component != relative.end()) {
            auto next = component;
            ++next;
            if (next != relative.end()) category = path_to_utf8(*component);
        }
        std::string stem = path_to_utf8(relative.filename());
        std::string source = read_all(entry.path());
        examples.push_back({id, category, example_title(stem), entry.path(), example_requirements(source)});
    }
    std::sort(examples.begin(), examples.end(), [](const ExampleRecord& a, const ExampleRecord& b) {
        return a.id < b.id;
    });
    return examples;
}

static void write_json_string_array(std::ostream& out, const std::vector<std::string>& values) {
    out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ",";
        out << "\"" << json_escape(values[i]) << "\"";
    }
    out << "]";
}

static bool example_matches_query(const ExampleRecord& example, const std::string& query) {
    if (query.empty()) return true;
    std::string needle = lowercase_copy(query);
    std::string searchable = lowercase_copy(example.id + " " + example.category + " " + example.title);
    for (const auto& requirement : example.requirements) searchable += " " + lowercase_copy(requirement);
    return searchable.find(needle) != std::string::npos;
}

static int cmd_examples(const std::vector<std::string>& argv) {
    std::string query;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") json_output = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg examples [query] [--json]\n";
            return 0;
        } else if (query.empty()) query = arg;
        else return err("examples accepts at most one query and optional --json");
    }

    fs::path root = resolve_examples_root();
    if (root.empty()) {
        return err("Sura example gallery was not found; reinstall Sura or set SURA_EXAMPLES to the examples directory");
    }
    std::vector<ExampleRecord> all = discover_examples(root);
    std::vector<ExampleRecord> matches;
    for (const auto& example : all) {
        if (example_matches_query(example, query)) matches.push_back(example);
    }

    if (json_output) {
        std::cout << "{\n"
                  << "  \"schema\": \"sura.package.examples.v1\",\n"
                  << "  \"passed\": true,\n"
                  << "  \"query\": \"" << json_escape(query) << "\",\n"
                  << "  \"total_count\": " << all.size() << ",\n"
                  << "  \"match_count\": " << matches.size() << ",\n"
                  << "  \"examples\": [\n";
        for (size_t i = 0; i < matches.size(); ++i) {
            const auto& example = matches[i];
            std::cout << "    {\"id\":\"" << json_escape(example.id)
                      << "\",\"category\":\"" << json_escape(example.category)
                      << "\",\"title\":\"" << json_escape(example.title)
                      << "\",\"source\":\"examples/" << json_escape(example.id) << ".sura\",\"requirements\":";
            write_json_string_array(std::cout, example.requirements);
            std::cout << "}" << (i + 1 < matches.size() ? "," : "") << "\n";
        }
        std::cout << "  ]\n}\n";
        return 0;
    }

    std::cout << "Sura example gallery (" << matches.size() << " of " << all.size() << ")\n";
    for (const auto& example : matches) {
        std::cout << "  " << example.id << "  [" << example.category << "]";
        if (!example.requirements.empty()) {
            std::cout << " requires ";
            for (size_t i = 0; i < example.requirements.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << example.requirements[i];
            }
        }
        std::cout << "\n";
    }
    if (matches.empty()) info("no examples matched: " + query);
    else info("create: surapkg example <id> <project-name>");
    return 0;
}

static bool write_example_report(const fs::path& report_path,
                                 const ExampleRecord& example,
                                 const fs::path& project_root,
                                 const std::string& package_name,
                                 const std::string& source_hash,
                                 const std::vector<std::pair<fs::path, std::string>>& files) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.example.v1\",\n"
        << "  \"passed\": true,\n"
        << "  \"example\": \"" << json_escape(example.id) << "\",\n"
        << "  \"category\": \"" << json_escape(example.category) << "\",\n"
        << "  \"package\": \"" << json_escape(package_name) << "\",\n"
        << "  \"root\": \"" << json_escape(path_to_generic_utf8(project_root)) << "\",\n"
        << "  \"main\": \"" << json_escape(path_to_generic_utf8(project_root / "src/main.sura")) << "\",\n"
        << "  \"source_sha256\": \"" << source_hash << "\",\n"
        << "  \"requirements\": ";
    write_json_string_array(out, example.requirements);
    out << ",\n  \"file_count\": " << files.size() << ",\n  \"files\": [\n";
    for (size_t i = 0; i < files.size(); ++i) {
        out << "    {\"path\":\"" << json_escape(path_to_generic_utf8(files[i].first))
            << "\",\"kind\":\"" << json_escape(files[i].second) << "\"}"
            << (i + 1 < files.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    return write_all(report_path, out.str());
}

static int run_example_command(const std::string& requested_id,
                               const fs::path& destination,
                               const fs::path& json_report) {
    if (requested_id.empty()) return err("example requires an example id");
    if (destination.empty()) return err("example requires a new project directory");
    if (fs::exists(destination)) return err(path_to_generic_utf8(destination) + " already exists");

    std::string normalized_id = requested_id;
    std::replace(normalized_id.begin(), normalized_id.end(), '\\', '/');
    if (normalized_id.size() > 5 && normalized_id.substr(normalized_id.size() - 5) == ".sura") {
        normalized_id.resize(normalized_id.size() - 5);
    }
    std::string lookup = lowercase_copy(normalized_id);

    fs::path examples_root = resolve_examples_root();
    if (examples_root.empty()) {
        return err("Sura example gallery was not found; reinstall Sura or set SURA_EXAMPLES to the examples directory");
    }
    std::vector<ExampleRecord> examples = discover_examples(examples_root);
    const ExampleRecord* selected = nullptr;
    for (const auto& example : examples) {
        if (lowercase_copy(example.id) == lookup) {
            selected = &example;
            break;
        }
    }
    if (!selected) return err("unknown example id: " + requested_id + "; run `surapkg examples` to list valid ids");

    std::string package_name = normalize_name(path_to_utf8(destination.filename()));
    if (package_name.empty() || package_name == "." || package_name == "..") {
        return err("example destination must have a valid project directory name");
    }
    std::string source = read_all(selected->source_path);
    if (source.empty()) return err("example source is empty or unreadable: " + selected->id);
    std::string source_hash = sha256_text(source);

    std::error_code ec;
    fs::create_directories(destination / "src", ec);
    if (ec) return err("failed to create example project source directory: " + ec.message());
    fs::create_directories(destination / ".vscode", ec);
    if (ec) return err("failed to create example project VS Code directory: " + ec.message());

    std::string manifest =
        "{\n"
        "  \"name\": \"" + json_escape(package_name) + "\",\n"
        "  \"version\": \"0.1.0\",\n"
        "  \"main\": \"src/main.sura\",\n"
        "  \"description\": \"Project created from Sura example " + json_escape(selected->id) + "\",\n"
        "  \"keywords\": [\"example\", \"" + json_escape(selected->category) + "\"],\n"
        "  \"dependencies\": {}\n"
        "}\n";

    std::ostringstream provenance;
    provenance << "{\n"
               << "  \"schema\": \"sura.example.provenance.v1\",\n"
               << "  \"example\": \"" << json_escape(selected->id) << "\",\n"
               << "  \"source\": \"examples/" << json_escape(selected->id) << ".sura\",\n"
               << "  \"source_sha256\": \"" << source_hash << "\",\n"
               << "  \"requirements\": ";
    write_json_string_array(provenance, selected->requirements);
    provenance << "\n}\n";

    std::ostringstream readme;
    readme << "# " << package_name << "\n\n"
           << "Created from the Sura example `" << selected->id << "`.\n\n"
           << "## Requirements\n\n";
    if (selected->requirements.empty()) readme << "- No optional runtime dependency detected.\n";
    else for (const auto& requirement : selected->requirements) readme << "- " << requirement << "\n";
    readme << "\n## Check and run\n\n```powershell\n"
           << "surapkg check\n"
           << "surapkg run\n"
           << "```\n\n"
           << "`sura.example.json` records the gallery id and source hash used to create this project.\n";

    const std::string vscode_settings =
        "{\n"
        "  \"files.encoding\": \"utf8\",\n"
        "  \"files.associations\": {\"*.sura\": \"sura\"}\n"
        "}\n";
    const std::string vscode_launch =
        "{\n"
        "  \"version\": \"0.2.0\",\n"
        "  \"configurations\": [{\n"
        "    \"type\": \"sura\",\n"
        "    \"request\": \"launch\",\n"
        "    \"name\": \"Debug Sura Example\",\n"
        "    \"program\": \"${workspaceFolder}/src/main.sura\",\n"
        "    \"cwd\": \"${workspaceFolder}\"\n"
        "  }]\n"
        "}\n";

    std::vector<std::pair<fs::path, std::string>> files;
    auto write_project_file = [&](const fs::path& relative, const std::string& content, const std::string& kind) {
        fs::path target = destination / relative;
        if (!write_all(target, content)) return false;
        files.push_back({target, kind});
        return true;
    };
    if (!write_project_file(kManifest, manifest, "manifest") ||
        !write_project_file("src/main.sura", source, "main") ||
        !write_project_file("sura.example.json", provenance.str(), "provenance") ||
        !write_project_file("README.md", readme.str(), "readme") ||
        !write_project_file(".gitignore", "artifacts/\npackages/\nsura-run-report.json\n", "gitignore") ||
        !write_project_file(".vscode/settings.json", vscode_settings, "vscode") ||
        !write_project_file(".vscode/launch.json", vscode_launch, "vscode")) {
        return err("failed to write example project files under " + path_to_generic_utf8(destination));
    }

    if (!json_report.empty()) {
        if (!write_example_report(json_report, *selected, destination, package_name, source_hash, files)) {
            return err("failed to write example JSON report: " + path_to_generic_utf8(json_report));
        }
        ok("example report written: " + path_to_generic_utf8(json_report));
    }
    ok("created example project " + path_to_generic_utf8(destination));
    info("source: " + selected->id);
    info("run: cd " + path_to_generic_utf8(destination) + " && surapkg run");
    return 0;
}

static int cmd_example(const std::vector<std::string>& argv) {
    std::string id;
    fs::path destination;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("example --json requires an output path");
            json_report = utf8_path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = utf8_path(arg.substr(7));
            if (json_report.empty()) return err("example --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg example <id> <project-directory> [--json report.json]\n";
            return 0;
        } else if (id.empty()) id = arg;
        else if (destination.empty()) destination = utf8_path(arg);
        else return err("example accepts one id, one project directory, and optional --json");
    }
    return run_example_command(id, destination, json_report);
}

static bool write_agent_json_report(const fs::path& report_path,
                                    const std::string& pkg_name,
                                    const fs::path& root,
                                    const std::vector<std::pair<fs::path, std::string>>& files) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.agent.v1\",\n"
        << "  \"passed\": true,\n"
        << "  \"package\": \"" << json_escape(pkg_name) << "\",\n"
        << "  \"root\": \"" << json_escape(path_to_generic_utf8(root)) << "\",\n"
        << "  \"main\": \"" << json_escape(path_to_generic_utf8(root / "src" / (pkg_name + ".sura"))) << "\",\n"
        << "  \"test\": \"" << json_escape(path_to_generic_utf8(root / "tests" / "agent_test.sura")) << "\",\n"
        << "  \"policy\": \"" << json_escape(path_to_generic_utf8(root / kToolPolicyManifest)) << "\",\n"
        << "  \"knowledge_dir\": \"" << json_escape(path_to_generic_utf8(root / "knowledge")) << "\",\n"
        << "  \"file_count\": " << files.size() << ",\n"
        << "  \"files\": [\n";
    for (size_t i = 0; i < files.size(); ++i) {
        out << "    {\"path\":\"" << json_escape(path_to_generic_utf8(files[i].first))
            << "\",\"kind\":\"" << json_escape(files[i].second) << "\"}";
        if (i + 1 < files.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_agent_command(const std::string& name, const fs::path& json_report) {
    if (name.empty()) return err("agent requires a project name");
    std::string pkg_name = normalize_name(name);
    fs::path root = pkg_name;
    if (fs::exists(root)) return err(root.string() + " already exists");

    fs::create_directories(root / "src");
    fs::create_directories(root / "tests");
    fs::create_directories(root / "knowledge");

    std::string manifest =
        "{\n"
        "  \"name\": \"" + json_escape(pkg_name) + "\",\n"
        "  \"version\": \"0.1.0\",\n"
        "  \"main\": \"src/" + json_escape(pkg_name) + ".sura\",\n"
        "  \"description\": \"AI automation agent template\",\n"
        "  \"keywords\": [\"ai\", \"agent\", \"automation\"],\n"
        "  \"dependencies\": {}\n"
        "}\n";
    write_all(root / kManifest, manifest);

    write_all(root / "README.md",
        "# " + pkg_name + "\n\n"
        "Sura AI automation agent template.\n\n"
        "Run:\n\n"
        "```powershell\n"
        "..\\SuraLanguage.exe .\\src\\" + pkg_name + ".sura\n"
        "..\\surapkg.exe test\n"
        "```\n\n"
        "Set your own OpenAI-compatible endpoint in app code by sending the generated tool-enabled structured request body with `llm.chat_request(endpoint, api_key, request)`; it includes `llm.request_tools_schema_json` output with OpenAI-compatible tool definitions and `response_format`. The template also keeps a schema-only request example for structured output, or you can use `llm.chat(endpoint, api_key, model, messages)` for simple direct calls.\n"
        "The generated agent reads `knowledge/*.md` documents through policy-gated tool calls, wraps them with `rag.prepare`, and keeps source metadata in `prepared.sources`.\n"
        "It also shows a model tool-call round with `llm.next_messages` and `llm.next_schema_request`, turning model-selected tools into the assistant plus `tool` role messages and then into the next tool-enabled structured LLM request.\n"
        "The generated `sura.tools.json` only allows `http_request` GET calls for `file://` URLs. Widen that policy deliberately before using network, write-capable HTTP methods, or shell tools.\n");

    write_all(root / "knowledge" / "project.md",
        "# Project Context\n\n"
        "Sura is a fast scripting language for games, automation, and AI agents.\n"
        "Use this document for project-specific goals, constraints, and domain facts.\n");

    write_all(root / "knowledge" / "sura.md",
        "# Sura Agent Tools\n\n"
        "Sura includes JSON, schema validation, policy-gated tool calls, HTTP helpers, streams, vectors, array tensor helpers, native contiguous float64 autograd tensors, native neural-network training, and LLM request helpers.\n"
        "RAG agents should cite grounded source snippets from `prepared.sources` before taking action.\n");

    write_all(root / kToolPolicyManifest,
        "{\n"
        "  \"version\": 1,\n"
        "  \"tools\": [\"http_request\"],\n"
        "  \"url_prefixes\": [\"file://\"],\n"
        "  \"http_methods\": [\"GET\"],\n"
        "  \"allowed_headers\": [\"X-Agent\"],\n"
        "  \"required_headers\": {\"X-Agent\": \"sura-agent-template\"},\n"
        "  \"max_timeout\": 30,\n"
        "  \"max_body_bytes\": 0,\n"
        "  \"approval\": false,\n"
        "  \"allow_shell\": false,\n"
        "  \"command_prefixes\": []\n"
        "}\n");

    write_all(root / "src" / (pkg_name + ".sura"),
        "use json\n"
        "use fs\n"
        "use rag\n"
        "use llm\n"
        "use tool\n\n"
        "func tool_policy do\n"
        "  return {tools: [\"http_request\"], url_prefixes: [\"file://\"], http_methods: [\"GET\"], allowed_headers: [\"X-Agent\"], required_headers: {\"X-Agent\": \"sura-agent-template\"}, max_timeout: 30, max_body_bytes: 0, approval: false, allow_shell: false}\n"
        "end\n\n"
        "func agent_tool_names do\n"
        "  return [\"http_request\"]\n"
        "end\n\n"
        "func agent_input_schema do\n"
        "  return {type: \"dict\", required: [\"goal\", \"max_steps\"], properties: {goal: {type: \"string\", min_len: 1}, max_steps: {type: \"integer\", min: 1, max: 10}}, additional: false}\n"
        "end\n\n"
        "func agent_output_schema do\n"
        "  return {type: \"dict\", required: [\"plan\", \"sources\"], properties: {plan: {type: \"array\", items: {type: \"string\", min_len: 1}}, sources: {type: \"array\", items: \"string\"}}, additional: false}\n"
        "end\n\n"
        "func validate_input(goal, max_steps) do\n"
        "  input is {goal: goal, max_steps: max_steps}\n"
        "  errors is json.schema_errors(input, agent_input_schema())\n"
        "  assert_eq(errors.len(), 0, errors.join(\" | \"))\n"
        "  return input\n"
        "end\n\n"
        "func read_context(path) do\n"
        "  spec is tool.spec(\"http_request\", {method: \"GET\", url: \"file://\" + path, headers: {\"X-Agent\": \"sura-agent-template\"}})\n"
        "  return tool.call_policy(spec, tool_policy())\n"
        "end\n\n"
        "func load_knowledge_docs do\n"
        "  paths is fs.walk(\"knowledge\", \".md\")\n"
        "  docs is []\n"
        "  for path in paths do\n"
        "    id is replace(fs.basename(path), \".md\", \"\")\n"
        "    title is id\n"
        "    embedding is [0.8, 0.2]\n"
        "    if id == \"project\" then\n"
        "      title is \"Project Context\"\n"
        "      embedding is [1, 0]\n"
        "    end\n"
        "    if id == \"sura\" then\n"
        "      title is \"Sura Agent Tools\"\n"
        "    end\n"
        "    docs.push({id: id, title: title, text: read_context(path), path: path, embedding: embedding})\n"
        "  end\n"
        "  assert(length(docs) > 0)\n"
        "  return docs\n"
        "end\n\n"
        "func build_rag(goal) do\n"
        "  docs is load_knowledge_docs()\n"
        "  prepared is rag.prepare(goal, [1, 0], docs, 2, \"embedding\", \"text\", \"You are a concise Sura automation agent. Return a short numbered plan with source-grounded steps.\")\n"
        "  assert_eq(prepared.sources[0].id, \"project\")\n"
        "  assert_eq(prepared.sources[0].item.path, \"knowledge/project.md\")\n"
        "  return prepared\n"
        "end\n\n"
        "func build_messages(goal) do\n"
        "  prepared is build_rag(goal)\n"
        "  return prepared.messages\n"
        "end\n\n"
        "func build_schema_request(goal) do\n"
        "  messages is build_messages(goal)\n"
        "  return llm.request_schema_json(\"openai-compatible-model\", messages, agent_output_schema(), 0.2, \"sura_agent_plan\")\n"
        "end\n\n"
        "func build_request(goal) do\n"
        "  messages is build_messages(goal)\n"
        "  return llm.request_tools_schema_json(\"openai-compatible-model\", messages, agent_tool_names(), agent_output_schema(), 0.2, \"sura_agent_plan\")\n"
        "end\n\n"
        "func send_request(endpoint, api_key, request) do\n"
        "  return llm.chat_request(endpoint, api_key, request)\n"
        "end\n\n"
        "func mock_model_tool_response(context_path) do\n"
        "  args is {method: \"GET\", url: \"file://\" + context_path, headers: {\"X-Agent\": \"sura-agent-template\"}}\n"
        "  return {choices: [{message: {tool_calls: [{id: \"call_context\", type: \"function\", function: {name: \"http_request\", arguments: json.stringify(args)}}]}}]}\n"
        "end\n\n"
        "func run_tool_round(messages, model_response) do\n"
        "  next_messages is llm.next_messages(messages, model_response, tool_policy())\n"
        "  base is length(messages)\n"
        "  assert_eq(next_messages[base].role, \"assistant\")\n"
        "  assert_eq(next_messages[base + 1].role, \"tool\")\n"
        "  next_request is llm.next_schema_request(\"openai-compatible-model\", messages, model_response, tool_policy(), agent_tool_names(), agent_output_schema(), 0.2, \"sura_agent_plan\")\n"
        "  assert_eq(next_request.messages[base + 1].role, \"tool\")\n"
        "  assert_eq(next_request.response_format.json_schema.name, \"sura_agent_plan\")\n"
        "  return next_messages\n"
        "end\n\n"
        "func run_agent(goal) do\n"
        "  input is validate_input(goal, 3)\n"
        "  docs is load_knowledge_docs()\n"
        "  assert(length(docs) >= 2)\n"
        "  messages is build_messages(input.goal)\n"
        "  next_messages is run_tool_round(messages, mock_model_tool_response(\"knowledge/project.md\"))\n"
        "  assert(contains(next_messages[length(messages) + 1].content, \"Sura\"))\n"
        "  request is build_request(input.goal)\n"
        "  assert(contains(request, \"\\\"messages\\\"\"))\n"
        "  assert(contains(request, \"openai-compatible-model\"))\n"
        "  assert(contains(request, \"\\\"tools\\\"\"))\n"
        "  assert(contains(request, \"http_request\"))\n"
        "  assert(contains(request, \"\\\"response_format\\\"\"))\n"
        "  mock_response_path is \"knowledge/mock_response.json\"\n"
        "  fs.write(mock_response_path, json.stringify({choices: [{message: {content: json.stringify({plan: [\"mock plan\"], sources: [\"knowledge/project.md\"]})}}]}))\n"
        "  mock_response_url is \"file://\" + fs.abs(mock_response_path).replace(\"\\\\\", \"/\")\n"
        "  mock_response is send_request(mock_response_url, \"\", json.parse(request))\n"
        "  output is llm.extract_json(mock_response, agent_output_schema())\n"
        "  assert_eq(output.plan[0], \"mock plan\")\n"
        "  assert_eq(output.sources[0], \"knowledge/project.md\")\n"
        "  schema_request is build_schema_request(input.goal)\n"
        "  assert(contains(schema_request, \"\\\"response_format\\\"\"))\n"
        "  print request\n"
        "  return request\n"
        "end\n\n"
        "func agent_main do\n"
        "  if not fs.exists(\"knowledge/project.md\") then\n"
        "    fs.mkdir(\"knowledge\")\n"
        "    fs.write(\"knowledge/project.md\", \"Sura makes AI automation scripts small.\")\n"
        "  end\n"
        "  if not fs.exists(\"knowledge/sura.md\") then\n"
        "    fs.mkdir(\"knowledge\")\n"
        "    fs.write(\"knowledge/sura.md\", \"Sura includes RAG and LLM helper functions.\")\n"
        "  end\n"
        "  run_agent(\"Create a short automation plan.\")\n"
        "end\n\n"
        "agent_main()\n");

    write_all(root / "tests" / "agent_test.sura",
        "use json\n"
        "use fs\n"
        "use rag\n"
        "use llm\n"
        "use tool\n\n"
        "messages is llm.messages(\"system\", \"user\")\n"
        "assert_eq(messages[0].role, \"system\")\n"
        "assert_eq(messages[1].content, \"user\")\n\n"
        "body is llm.request_json(\"agent-test-model\", messages, 0)\n"
        "assert(contains(body, \"\\\"messages\\\"\"))\n"
        "assert(contains(body, \"agent-test-model\"))\n\n"
        "schema is {type: \"dict\", required: [\"goal\", \"max_steps\"], properties: {goal: {type: \"string\", min_len: 1}, max_steps: {type: \"integer\", min: 1, max: 5}}, additional: false}\n"
        "schema_body is llm.request_schema_json(\"agent-test-model\", messages, schema, 0, \"agent_result\")\n"
        "parsed_schema_body is json.parse(schema_body)\n"
        "assert_eq(parsed_schema_body.response_format.type, \"json_schema\")\n"
        "assert_eq(parsed_schema_body.response_format.json_schema.name, \"agent_result\")\n"
        "assert_eq(parsed_schema_body.response_format.json_schema.schema.properties.goal.type, \"string\")\n"
        "assert_eq(parsed_schema_body.response_format.json_schema.schema.properties.max_steps.maximum, 5)\n"
        "assert_eq(parsed_schema_body.response_format.json_schema.schema.additionalProperties, false)\n\n"
        "tools_body is llm.request_tools_json(\"agent-test-model\", messages, [\"http_request\"], 0)\n"
        "parsed_tools_body is json.parse(tools_body)\n"
        "assert_eq(parsed_tools_body.tools[0].type, \"function\")\n"
        "assert_eq(parsed_tools_body.tools[0].function.name, \"http_request\")\n"
        "assert_contains(parsed_tools_body.tools[0].function.parameters.required, \"url\")\n"
        "assert_eq(parsed_tools_body.tools[0].function.parameters.properties.url.type, \"string\")\n\n"
        "combined_body is llm.request_tools_schema_json(\"agent-test-model\", messages, [\"http_request\"], schema, 0, \"agent_result\")\n"
        "parsed_combined_body is json.parse(combined_body)\n"
        "assert_eq(parsed_combined_body.tools[0].function.name, \"http_request\")\n"
        "assert_eq(parsed_combined_body.response_format.json_schema.name, \"agent_result\")\n"
        "assert_eq(parsed_combined_body.response_format.json_schema.schema.properties.goal.type, \"string\")\n\n"
        "mock_response_file is \"agent_test_llm_response.json\"\n"
        "fs.write(mock_response_file, json.stringify({choices: [{message: {content: \"tool schema ok\"}}]}))\n"
        "mock_response_url is \"file://\" + fs.abs(mock_response_file).replace(\"\\\\\", \"/\")\n"
        "chat_raw is llm.chat_request(mock_response_url, \"test-key\", parsed_combined_body)\n"
        "assert_eq(llm.extract_text(chat_raw), \"tool schema ok\")\n"
        "json_chat_raw is {choices: [{message: {content: json.stringify({goal: \"ship\", max_steps: 3})}}]}\n"
        "json_chat_value is llm.extract_json(json_chat_raw, schema)\n"
        "assert_eq(json_chat_value.goal, \"ship\")\n"
        "assert_eq(json_chat_value.max_steps, 3)\n"
        "chat_raw_json is llm.chat_request(mock_response_url, \"test-key\", combined_body)\n"
        "assert_eq(llm.extract_text(chat_raw_json), \"tool schema ok\")\n\n"
        "context_file is \"agent_test_context.txt\"\n"
        "fs.write(context_file, \"tool context\")\n"
        "context_url is \"file://\" + fs.abs(context_file).replace(\"\\\\\", \"/\")\n"
        "policy is {tools: [\"http_request\"], url_prefixes: [\"file://\"], http_methods: [\"GET\"], allowed_headers: [\"X-Agent\"], required_headers: {\"X-Agent\": \"agent-test\"}, max_timeout: 30, max_body_bytes: 0, approval: false, allow_shell: false}\n"
        "spec is tool.spec(\"http_request\", {method: \"GET\", url: context_url, headers: {\"X-Agent\": \"agent-test\"}})\n"
        "assert(tool.validate(spec))\n"
        "assert(tool.allowed(spec, policy))\n"
        "result is tool.call_policy(spec, policy)\n"
        "assert_eq(result, \"tool context\")\n"
        "model_tool_response is {choices: [{message: {tool_calls: [{id: \"call_context\", type: \"function\", function: {name: \"http_request\", arguments: json.stringify({method: \"GET\", url: context_url, headers: {\"X-Agent\": \"agent-test\"}})}}]}}]}\n"
        "next_messages is llm.next_messages(messages, model_tool_response, policy)\n"
        "assert_eq(next_messages[length(messages)].role, \"assistant\")\n"
        "assert_eq(next_messages[length(messages) + 1].role, \"tool\")\n"
        "assert_eq(next_messages[length(messages) + 1].tool_call_id, \"call_context\")\n"
        "assert_eq(next_messages[length(messages) + 1].content, \"tool context\")\n"
        "next_request is llm.next_request(\"agent-test-model\", messages, model_tool_response, policy, [\"http_request\"], 0)\n"
        "assert_eq(next_request.messages[length(messages) + 1].role, \"tool\")\n"
        "assert_eq(next_request.tools[0].function.name, \"http_request\")\n"
        "next_schema_request is llm.next_schema_request(\"agent-test-model\", messages, model_tool_response, policy, [\"http_request\"], schema, 0, \"agent_result\")\n"
        "assert_eq(next_schema_request.messages[length(messages) + 1].role, \"tool\")\n"
        "assert_eq(next_schema_request.response_format.json_schema.name, \"agent_result\")\n"
        "assert_contains(tool.schema(\"http_request\").required, \"url\")\n"
        "assert_contains(tool.schema(\"http_request\").fields.headers, \"dict\")\n"
        "assert(not tool.allowed({name: \"http_request\", url: context_url, method: \"POST\"}, policy))\n"
        "assert(not tool.allowed({name: \"shell\", command: \"echo blocked\"}, policy))\n\n"
        "rag_docs is [{id: \"roadmap\", title: \"Roadmap\", text: \"ship safe AI automation\", embedding: [1, 0]}, {id: \"other\", title: \"Other\", text: \"unrelated storage\", embedding: [0, 1]}]\n"
        "prepared is rag.prepare(\"ship\", [1, 0], rag_docs, 1)\n"
        "assert_eq(prepared.sources[0].id, \"roadmap\")\n"
        "assert_contains(prepared.context, \"[1] Roadmap\")\n"
        "assert_contains(prepared.messages[1].content, \"ship\")\n\n"
        "assert(json.schema_validate({goal: \"ship\", max_steps: 3}, schema))\n"
        "errors is json.schema_errors({goal: \"\", max_steps: 9, extra: true}, schema).join(\"|\")\n"
        "assert(contains(errors, \"$.goal\"))\n"
        "assert(contains(errors, \"$.max_steps\"))\n"
        "assert(contains(errors, \"$.extra\"))\n"
        "print \"agent template test: PASS\"\n");

    std::vector<std::pair<fs::path, std::string>> generated_files = {
        {root / kManifest, "manifest"},
        {root / "README.md", "readme"},
        {root / kToolPolicyManifest, "tool_policy"},
        {root / "src" / (pkg_name + ".sura"), "main"},
        {root / "tests" / "agent_test.sura", "test"},
        {root / "knowledge" / "project.md", "knowledge"},
        {root / "knowledge" / "sura.md", "knowledge"}
    };
    if (!json_report.empty()) {
        if (!write_agent_json_report(json_report, pkg_name, root, generated_files)) {
            return err("failed to write agent JSON report: " + path_to_generic_utf8(json_report));
        }
        ok("agent report written: " + path_to_generic_utf8(json_report));
    }
    ok("created AI agent project " + path_to_generic_utf8(root));
    return 0;
}

static int cmd_agent(const std::vector<std::string>& argv) {
    std::string name;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("agent --json requires an output path");
            json_report = utf8_path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = utf8_path(arg.substr(7));
            if (json_report.empty()) return err("agent --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg agent <name> [--json report.json]\n";
            return 0;
        } else if (name.empty()) {
            name = arg;
        } else {
            return err("agent accepts one project name and optional --json");
        }
    }
    return run_agent_command(name, json_report);
}

static bool write_embed_json_report(const fs::path& report_path,
                                    const std::string& project_name,
                                    const fs::path& root,
                                    const std::vector<std::pair<fs::path, std::string>>& files) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.embed.v1\",\n"
        << "  \"passed\": true,\n"
        << "  \"package\": \"" << json_escape(project_name) << "\",\n"
        << "  \"root\": \"" << json_escape(path_to_generic_utf8(root)) << "\",\n"
        << "  \"host\": \"" << json_escape(path_to_generic_utf8(root / "host" / "main.cpp")) << "\",\n"
        << "  \"script\": \"" << json_escape(path_to_generic_utf8(root / "scripts" / "tick.sura")) << "\",\n"
        << "  \"build_script\": \"" << json_escape(path_to_generic_utf8(root / "build.ps1")) << "\",\n"
        << "  \"run_script\": \"" << json_escape(path_to_generic_utf8(root / "run.ps1")) << "\",\n"
        << "  \"cmake\": \"" << json_escape(path_to_generic_utf8(root / "CMakeLists.txt")) << "\",\n"
        << "  \"file_count\": " << files.size() << ",\n"
        << "  \"files\": [\n";
    for (size_t i = 0; i < files.size(); ++i) {
        out << "    {\"path\":\"" << json_escape(path_to_generic_utf8(files[i].first))
            << "\",\"kind\":\"" << json_escape(files[i].second) << "\"}";
        if (i + 1 < files.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_embed_command(const std::string& name, const fs::path& json_report) {
    if (name.empty()) return err("embed requires a project name");
    std::string project_name = normalize_name(name);
    fs::path root = project_name;
    if (fs::exists(root)) return err(root.string() + " already exists");

    fs::create_directories(root / "host");
    fs::create_directories(root / "scripts");

    write_all(root / kManifest,
        "{\n"
        "  \"name\": \"" + json_escape(project_name) + "\",\n"
        "  \"version\": \"0.1.0\",\n"
        "  \"main\": \"scripts/tick.sura\",\n"
        "  \"description\": \"Native host embedding template for Sura game/app scripting\",\n"
        "  \"keywords\": [\"embed\", \"game\", \"app\", \"scripting\"],\n"
        "  \"dependencies\": {}\n"
        "}\n");

    write_all(root / "scripts" / "tick.sura",
        "func update_state(player_x, velocity, dt, input_right) do\n"
        "  step is velocity * dt\n"
        "  if input_right then\n"
        "    step is step + 1\n"
        "  end\n"
        "  return player_x + step\n"
        "end\n\n"
        "next_x is update_state(player_x, velocity, dt, input_right)\n"
        "state_label is scene_name + \":\" + to_str(next_x)\n"
        "score is next_x * 10 + frame\n");

    write_all(root / "host" / "main.cpp", R"CPP(#include "sura_ffi.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

int main(int argc, char** argv) {
    const char* script_path = argc > 1 ? argv[1] : "scripts/tick.sura";
    std::string source = read_file(script_path);
    if (source.empty()) {
        std::cerr << "failed to read script: " << script_path << "\n";
        return 1;
    }

    if (sura_abi_version() != SURA_FFI_ABI_VERSION) {
        std::cerr << "Sura ABI version mismatch\n";
        return 2;
    }

    SuraHandle vm = sura_new();
    if (!vm) return 3;

    sura_set_number(vm, "frame", 7);
    sura_set_number(vm, "player_x", 100);
    sura_set_number(vm, "velocity", 30);
    sura_set_number(vm, "dt", 0.1);
    sura_set_bool(vm, "input_right", 1);
    sura_set_string(vm, "scene_name", "arena");

    int rc = sura_run(vm, source.c_str());
    if (rc != SURA_OK) {
        std::cerr << sura_last_error(vm) << "\n";
        sura_free(vm);
        return 4;
    }

    if (!sura_has(vm, "next_x") || !sura_has(vm, "state_label") || !sura_has(vm, "score")) {
        std::cerr << "script did not publish expected globals\n";
        sura_free(vm);
        return 5;
    }

    double next_x = sura_get_number(vm, "next_x");
    std::string state = sura_get_string(vm, "state_label");
    double score = sura_get_number(vm, "score");
    sura_free(vm);

    if (std::fabs(next_x - 104.0) > 0.0001 || state != "arena:104" || std::fabs(score - 1047.0) > 0.0001) {
        std::cerr << "unexpected script result\n";
        return 6;
    }

    std::cout << "embed_template: PASS\n";
    std::cout << "next_x=" << next_x << "\n";
    std::cout << "state=" << state << "\n";
    std::cout << "score=" << score << "\n";
    return 0;
}
)CPP");

    write_all(root / "build.ps1",
        "param(\n"
        "    [string]$SuraRoot = \"\",\n"
        "    [string]$Cxx = \"\"\n"
        ")\n\n"
        "$ErrorActionPreference = \"Stop\"\n"
        "if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {\n"
        "    $PSNativeCommandUseErrorActionPreference = $false\n"
        "}\n"
        "function Resolve-Cxx {\n"
        "    param([string]$Requested)\n"
        "    $candidates = @()\n"
        "    if (-not [string]::IsNullOrWhiteSpace($Requested)) { $candidates += $Requested }\n"
        "    if ($IsWindows -or $env:OS -eq \"Windows_NT\") { $candidates += \"C:\\msys64\\mingw64\\bin\\g++.exe\" }\n"
        "    $candidates += @(\"c++\", \"g++\", \"clang++\")\n"
        "    foreach ($candidate in $candidates) {\n"
        "        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }\n"
        "        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }\n"
        "        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue\n"
        "        if ($cmd) { return $cmd.Source }\n"
        "    }\n"
        "    throw \"C++ compiler not found. Pass -Cxx or install c++/g++/clang++.\"\n"
        "}\n"
        "if ([string]::IsNullOrWhiteSpace($SuraRoot)) {\n"
        "    if ($env:SURA_ROOT) { $SuraRoot = $env:SURA_ROOT }\n"
        "    else { $SuraRoot = (Resolve-Path (Join-Path $PSScriptRoot \"..\")).Path }\n"
        "}\n"
        "$SuraRoot = (Resolve-Path $SuraRoot).Path\n"
        "$CxxPath = Resolve-Cxx $Cxx\n"
        "$outDir = Join-Path $PSScriptRoot \"build\"\n"
        "New-Item -ItemType Directory -Force -Path $outDir | Out-Null\n"
        "$exeName = if ($IsWindows -or $env:OS -eq \"Windows_NT\") { \"" + project_name + "_host.exe\" } else { \"" + project_name + "_host\" }\n"
        "$exe = Join-Path $outDir $exeName\n"
        "$compileLog = Join-Path $outDir \"compile.log\"\n"
        "$extraArgs = @()\n"
        "if ($IsWindows -or $env:OS -eq \"Windows_NT\") { $extraArgs += \"-lgdi32\" }\n"
        "if ($IsLinux) { $extraArgs += \"-ldl\" }\n"
        "$oldPreference = $ErrorActionPreference\n"
        "$ErrorActionPreference = \"Continue\"\n"
        "& $CxxPath -std=c++17 -O2 -I $SuraRoot `\n"
        "    (Join-Path $PSScriptRoot \"host/main.cpp\") `\n"
        "    (Join-Path $SuraRoot \"sura_ffi.cpp\") `\n"
        "    (Join-Path $SuraRoot \"platform.cpp\") `\n"
        "    (Join-Path $SuraRoot \"gc.cpp\") `\n"
        "    -o $exe @extraArgs > $compileLog 2>&1\n"
        "$code = $LASTEXITCODE\n"
        "$ErrorActionPreference = $oldPreference\n"
        "if ($code -ne 0) { Get-Content -Raw -Path $compileLog | Write-Output; throw \"host build failed\" }\n"
        "Write-Output \"[OK] built $exe\"\n");

    write_all(root / "run.ps1",
        "param(\n"
        "    [string]$Script = \"scripts/tick.sura\"\n"
        ")\n\n"
        "$ErrorActionPreference = \"Stop\"\n"
        "$exeName = if ($IsWindows -or $env:OS -eq \"Windows_NT\") { \"" + project_name + "_host.exe\" } else { \"" + project_name + "_host\" }\n"
        "$exe = Join-Path $PSScriptRoot (Join-Path \"build\" $exeName)\n"
        "if (-not (Test-Path -LiteralPath $exe)) {\n"
        "    & (Join-Path $PSScriptRoot \"build.ps1\")\n"
        "    if ($LASTEXITCODE -ne 0) { throw \"build failed\" }\n"
        "}\n"
        "Push-Location $PSScriptRoot\n"
        "try {\n"
        "    & $exe $Script\n"
        "    if ($LASTEXITCODE -ne 0) { throw \"host run failed\" }\n"
        "} finally {\n"
        "    Pop-Location\n"
        "}\n");

    write_all(root / "CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(" + project_name + "_host LANGUAGES CXX)\n\n"
        "set(CMAKE_CXX_STANDARD 17)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "set(CMAKE_CXX_EXTENSIONS OFF)\n\n"
        "if(NOT DEFINED SURA_ROOT)\n"
        "    if(DEFINED ENV{SURA_ROOT})\n"
        "        set(SURA_ROOT \"$ENV{SURA_ROOT}\")\n"
        "    else()\n"
        "        message(FATAL_ERROR \"Set -DSURA_ROOT=/path/to/Sura-Language or the SURA_ROOT environment variable\")\n"
        "    endif()\n"
        "endif()\n\n"
        "get_filename_component(SURA_ROOT \"${SURA_ROOT}\" ABSOLUTE)\n\n"
        "add_executable(" + project_name + "_host\n"
        "    host/main.cpp\n"
        "    \"${SURA_ROOT}/sura_ffi.cpp\"\n"
        "    \"${SURA_ROOT}/platform.cpp\"\n"
        "    \"${SURA_ROOT}/gc.cpp\"\n"
        ")\n\n"
        "target_include_directories(" + project_name + "_host PRIVATE \"${SURA_ROOT}\")\n"
        "if(WIN32)\n"
        "    target_link_libraries(" + project_name + "_host PRIVATE gdi32)\n"
        "endif()\n"
        "if(UNIX AND NOT APPLE)\n"
        "    target_link_libraries(" + project_name + "_host PRIVATE dl)\n"
        "endif()\n");

    write_all(root / "README.md",
        "# " + project_name + "\n\n"
        "Native Sura embedding host template for games, desktop apps, and automation tools.\n\n"
        "The C++ host creates a `SuraHandle`, injects host state with `sura_set_number`, `sura_set_bool`, and `sura_set_string`, runs `scripts/tick.sura`, and reads script outputs back with `sura_get_number` and `sura_get_string`.\n\n"
        "Build and run from this directory:\n\n"
        "```powershell\n"
        "$env:SURA_ROOT = \"C:\\path\\to\\Sura-Language\" # or /path/to/Sura-Language on Linux/macOS\n"
        ".\\build.ps1\n"
        ".\\run.ps1\n"
        "```\n\n"
        "Or pass the repository root explicitly:\n\n"
        "```powershell\n"
        ".\\build.ps1 -SuraRoot C:\\path\\to\\Sura-Language # or /path/to/Sura-Language\n"
        ".\\run.ps1 -Script scripts\\tick.sura\n"
        "```\n\n"
        "CMake builds are also supported for IDEs and CI:\n\n"
        "```powershell\n"
        "cmake -S . -B build-cmake -DSURA_ROOT=C:\\path\\to\\Sura-Language\n"
        "cmake --build build-cmake --config Release\n"
        ".\\build-cmake\\" + project_name + "_host.exe scripts\\tick.sura\n"
        "```\n\n"
        "With multi-config generators, the executable may be under `build-cmake\\Release`. The PowerShell build script auto-detects `c++`, `g++`, or `clang++`, uses `.exe` on Windows and an extensionless host executable on Linux/macOS, and adds `-ldl` on Linux when needed. Ship `sura_ffi.hpp`, `sura_ffi.cpp`, `platform.cpp`, `gc.cpp`, and the Sura runtime files according to your host application's build system.\n");

    std::vector<std::pair<fs::path, std::string>> generated_files = {
        {root / kManifest, "manifest"},
        {root / "README.md", "readme"},
        {root / "host" / "main.cpp", "host"},
        {root / "scripts" / "tick.sura", "script"},
        {root / "build.ps1", "build_script"},
        {root / "run.ps1", "run_script"},
        {root / "CMakeLists.txt", "cmake"}
    };
    if (!json_report.empty()) {
        if (!write_embed_json_report(json_report, project_name, root, generated_files)) {
            return err("failed to write embed JSON report: " + path_to_generic_utf8(json_report));
        }
        ok("embed report written: " + path_to_generic_utf8(json_report));
    }
    ok("created native embed host project " + path_to_generic_utf8(root));
    return 0;
}

static int cmd_embed(const std::vector<std::string>& argv) {
    std::string name;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("embed --json requires an output path");
            json_report = utf8_path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = utf8_path(arg.substr(7));
            if (json_report.empty()) return err("embed --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg embed <name> [--json report.json]\n";
            return 0;
        } else if (name.empty()) {
            name = arg;
        } else {
            return err("embed accepts one project name and optional --json");
        }
    }
    return run_embed_command(name, json_report);
}

static fs::path resolve_source(const std::string& source) {
    fs::path p(source);
    if (fs::exists(p)) return p;
    fs::path sura_file = source + ".sura";
    if (fs::exists(sura_file)) return sura_file;
    PackageRef ref = parse_package_ref(source);
    fs::path reg = registry_root() / ref.name;
    if (fs::exists(reg)) {
        if (!ref.version.empty() && ref.version != "latest" && !is_version_range_spec(ref.version) && fs::exists(reg / ref.version)) {
            return reg / ref.version;
        }
        if (is_version_range_spec(ref.version)) return p;
        std::vector<fs::path> versions;
        for (const auto& entry : fs::directory_iterator(reg)) {
            if (entry.is_directory() && entry.path().filename() != "latest") versions.push_back(entry.path());
        }
        std::sort(versions.begin(), versions.end(), [](const fs::path& a, const fs::path& b) {
            return compare_versions(a.filename().string(), b.filename().string()) < 0;
        });
        if (!versions.empty()) return versions.back();
    }
    return p;
}

static bool write_install_json_report(const fs::path& report_path,
                                      const std::string& requested,
                                      const std::string& name,
                                      const std::string& version,
                                      const std::string& source,
                                      const fs::path& destination,
                                      bool from_registry,
                                      bool remote,
                                      bool dependency_recorded,
                                      bool passed) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"sura.package.install.v1\",\n";
    out << "  \"requested\": \"" << json_escape(requested) << "\",\n";
    out << "  \"package\": \"" << json_escape(name) << "\",\n";
    out << "  \"version\": \"" << json_escape(version) << "\",\n";
    out << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    out << "  \"source\": \"" << json_escape(source) << "\",\n";
    out << "  \"destination\": \"" << json_escape(destination.generic_string()) << "\",\n";
    out << "  \"from_registry\": " << (from_registry ? "true" : "false") << ",\n";
    out << "  \"remote\": " << (remote ? "true" : "false") << ",\n";
    out << "  \"dependency_recorded\": " << (dependency_recorded ? "true" : "false") << "\n";
    out << "}\n";
    return write_all(report_path, out.str());
}

static int install_package(const std::string& source, bool record_dependency, const fs::path& json_report) {
    if (source.empty()) return err("install requires a local file or directory");
    PackageRef ref = parse_package_ref(source);
    bool registry_ref = source.find_first_of("/\\:") == std::string::npos;
    fs::path src;
    if (registry_ref && (ref.version == "latest" || is_version_range_spec(ref.version))) {
        std::string constraint = ref.version == "latest" ? "" : ref.version;
        RegistryPackage selected;
        std::string select_error;
        if (select_registry_package(ref.name, constraint, selected, select_error)) {
            ref.version = selected.version;
            src = resolve_source(ref.name + "@" + ref.version);
        } else {
            src = resolve_source(source);
        }
    } else {
        src = resolve_source(source);
    }
    if (!fs::exists(src) && registry_ref) {
        std::string constraint = ref.version == "latest" ? "" : ref.version;
        RegistryPackage selected;
        std::string select_error;
        if (select_registry_package(ref.name, constraint, selected, select_error)) {
            ref.version = selected.version;
            src = resolve_source(ref.name + "@" + ref.version);
        }
    }
    if (!fs::exists(src)) {
        std::string url = registry_bundle_url(ref);
        if (url.empty()) return err("package source not found: " + source + " (set SURA_REGISTRY or SURA_REGISTRY_URL)");
        std::string bundle = http_get_text(url);
        if (bundle.empty()) return err("registry package not found: " + url);
        fs::path dst = package_dir(ref.name);
        if (!extract_package_bundle(bundle, dst)) return err("failed to extract registry bundle: " + url);
        std::string manifest = read_all(dst / kManifest);
        std::string version = manifest_field(manifest, "version", ref.version);
        int advisory_code = check_install_advisories(ref.name, version);
        if (advisory_code != 0) {
            std::error_code cleanup_ec;
            fs::remove_all(dst, cleanup_ec);
            return advisory_code;
        }
        if (record_dependency) set_dependency(ref.name, "registry:" + ref.name + "@" + version);
        if (!json_report.empty()) {
            if (!write_install_json_report(json_report, source, ref.name, version, url, dst,
                                           true, true, record_dependency, true)) {
                return err("failed to write install JSON report: " + json_report.generic_string());
            }
            ok("install report written: " + json_report.generic_string());
        }
        ok("installed " + ref.name + "@" + version + " -> " + dst.string() + " from " + url);
        return 0;
    }

    fs::create_directories(kPackages);
    std::string manifest = fs::is_directory(src) ? read_all(src / kManifest) : "";
    std::string name = normalize_name(manifest_field(manifest, "name", src.stem().string()));
    fs::path dst = package_dir(name);
    std::error_code ec;
    fs::remove_all(dst, ec);

    if (fs::is_directory(src)) {
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) return err("copy failed: " + ec.message());
        if (!fs::exists(dst / kManifest)) {
            write_all(dst / kManifest, default_manifest(name));
        }
    } else {
        fs::create_directories(dst);
        fs::copy_file(src, dst / (name + ".sura"), fs::copy_options::overwrite_existing, ec);
        if (ec) return err("copy failed: " + ec.message());
        write_all(dst / kManifest,
            "{\n"
            "  \"name\": \"" + json_escape(name) + "\",\n"
            "  \"version\": \"0.1.0\",\n"
            "  \"main\": \"" + json_escape(name) + ".sura\",\n"
            "  \"dependencies\": {}\n"
            "}\n");
    }

    std::string installed_manifest = read_all(dst / kManifest);
    std::string version = manifest_field(installed_manifest, "version", "0.1.0");
    bool from_registry = src.generic_string().find(registry_root().generic_string()) == 0;
    if (from_registry) {
        int advisory_code = check_install_advisories(name, version);
        if (advisory_code != 0) {
            std::error_code cleanup_ec;
            fs::remove_all(dst, cleanup_ec);
            return advisory_code;
        }
    }
    if (record_dependency) {
        set_dependency(name, from_registry ? ("registry:" + name + "@" + version) : ("file:packages/" + name));
    }
    if (!json_report.empty()) {
        if (!write_install_json_report(json_report, source, name, version, src.generic_string(), dst,
                                       from_registry, false, record_dependency, true)) {
            return err("failed to write install JSON report: " + json_report.generic_string());
        }
        ok("install report written: " + json_report.generic_string());
    }
    ok("installed " + name + " -> " + dst.string());
    return 0;
}

static int install_package(const std::string& source, bool record_dependency) {
    return install_package(source, record_dependency, fs::path());
}

static int cmd_install_source(const std::string& source) {
    return install_package(source, true);
}

static int cmd_install(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("install --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("install --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg install <path|file|name[@version|range]> [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("install accepts one package spec and optional --json");
        }
    }
    return install_package(source, true, json_report);
}

static bool manifest_has_dependency(const std::string& name) {
    auto deps = manifest_dependency_specs(read_all(kManifest));
    return std::any_of(deps.begin(), deps.end(), [&](const DependencySpec& dep) {
        return dep.name == normalize_name(name);
    });
}

static bool write_remove_json_report(const fs::path& report_path,
                                     const std::string& name,
                                     const fs::path& dst,
                                     uintmax_t removed_entries,
                                     bool dependency_removed,
                                     bool passed) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.remove.v1\",\n"
        << "  \"package\": \"" << json_escape(normalize_name(name)) << "\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"path\": \"" << json_escape(dst.generic_string()) << "\",\n"
        << "  \"installed\": " << (removed_entries > 0 ? "true" : "false") << ",\n"
        << "  \"removed_entries\": " << removed_entries << ",\n"
        << "  \"dependency_removed\": " << (dependency_removed ? "true" : "false") << "\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_remove_command(const std::string& name, const fs::path& json_report) {
    if (name.empty()) return err("remove requires a package name");
    fs::path dst = package_dir(name);
    bool dependency_existed = manifest_has_dependency(name);
    std::error_code ec;
    auto removed = fs::remove_all(dst, ec);
    if (ec) return err("remove failed: " + ec.message());
    if (!remove_dependency(normalize_name(name))) return err("remove failed to update " + kManifest.generic_string());
    if (!json_report.empty()) {
        if (!write_remove_json_report(json_report, name, dst, removed, dependency_existed, true)) {
            return err("failed to write remove JSON report: " + json_report.generic_string());
        }
        ok("remove report written: " + json_report.generic_string());
    }
    if (removed == 0) info(name + " was not installed");
    else ok("removed " + normalize_name(name));
    return 0;
}

static int cmd_remove(const std::vector<std::string>& argv) {
    std::string name;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("remove --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("remove --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg remove <name> [--json report.json]\n";
            return 0;
        } else if (name.empty()) {
            name = arg;
        } else {
            return err("remove accepts one package name and optional --json");
        }
    }
    return run_remove_command(name, json_report);
}

struct PackageListEntry {
    std::string name;
    std::string kind;
    std::string version;
    fs::path path;
};

static std::string canonical_stdlib_module_name(const std::string& name) {
    if (name == "logging") return "log";
    if (name == "filesystem" || name == "file") return "fs";
    if (name == "time") return "datetime";
    if (name == "web") return "http";
    if (name == "data") return "json";
    if (name == "testing") return "test";
    if (name == "rng") return "random";
    if (name == "py") return "python";
    if (name == "g3d" || name == "graphics") return "graphics3d";
    if (name == "ai") return "nn";
    return name;
}

static std::vector<std::string> builtin_stdlib_module_names() {
    return {"array", "async", "autograd", "cli", "crypto", "dataset", "datetime", "db", "fs",
            "dict", "ffi", "graphics3d", "http", "json", "llm", "log", "math", "nn", "os", "path",
            "media", "plugin", "python", "rag", "random", "regex", "set", "stream", "string",
            "tensor", "test", "tokenizer", "tool", "vector"};
}

static bool is_builtin_stdlib_module_name(const std::string& name) {
    std::string canonical = canonical_stdlib_module_name(name);
    auto modules = builtin_stdlib_module_names();
    return std::find(modules.begin(), modules.end(), canonical) != modules.end();
}

static std::vector<PackageListEntry> collect_package_list_entries() {
    std::vector<PackageListEntry> entries;
    std::unordered_set<std::string> stdlib_seen;
    if (fs::exists(kStdlib)) {
        for (const auto& entry : fs::directory_iterator(kStdlib)) {
            if (entry.path().extension() == ".sura") {
                std::string name = entry.path().stem().string();
                stdlib_seen.insert(name);
                entries.push_back({name, "stdlib", "", entry.path()});
            }
        }
    }
    for (const auto& name : builtin_stdlib_module_names()) {
        if (!stdlib_seen.count(name)) entries.push_back({name, "stdlib", "", fs::path("builtin:" + name)});
    }
    if (fs::exists(kPackages)) {
        for (const auto& entry : fs::directory_iterator(kPackages)) {
            if (entry.is_directory()) {
                std::string version;
                std::string manifest = read_all(entry.path() / kManifest);
                if (!manifest.empty()) version = manifest_field(manifest, "version", "");
                entries.push_back({entry.path().filename().string(), "package", version, entry.path()});
            } else if (entry.path().extension() == ".sura") {
                entries.push_back({entry.path().stem().string(), "package-file", "", entry.path()});
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const PackageListEntry& a, const PackageListEntry& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.name < b.name;
    });
    return entries;
}

static int cmd_list(const std::vector<std::string>& argv) {
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg list [--json]\n";
            return 0;
        } else {
            return err("list accepts only --json");
        }
    }

    auto entries = collect_package_list_entries();
    auto deps = manifest_dependency_specs(read_all(kManifest));
    if (json_output) {
        size_t stdlib_count = 0;
        size_t package_count = 0;
        size_t package_file_count = 0;
        for (const auto& entry : entries) {
            if (entry.kind == "stdlib") ++stdlib_count;
            else if (entry.kind == "package") ++package_count;
            else if (entry.kind == "package-file") ++package_file_count;
        }
        std::ostringstream out;
        out << "{\n"
            << "  \"schema\": \"sura.package.list.v1\",\n"
            << "  \"passed\": true,\n"
            << "  \"cwd\": \"" << json_escape(path_to_generic_utf8(fs::current_path())) << "\",\n"
            << "  \"stdlib_count\": " << stdlib_count << ",\n"
            << "  \"package_count\": " << package_count << ",\n"
            << "  \"package_file_count\": " << package_file_count << ",\n"
            << "  \"dependency_count\": " << deps.size() << ",\n"
            << "  \"entries\": [\n";
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            out << "    {"
                << "\"name\":\"" << json_escape(entry.name) << "\""
                << ",\"kind\":\"" << json_escape(entry.kind) << "\""
                << ",\"version\":\"" << json_escape(entry.version) << "\""
                << ",\"path\":\"" << json_escape(path_to_generic_utf8(entry.path)) << "\""
                << "}";
            if (i + 1 < entries.size()) out << ",";
            out << "\n";
        }
        out << "  ],\n"
            << "  \"dependencies\": [\n";
        for (size_t i = 0; i < deps.size(); ++i) {
            const auto& dep = deps[i];
            out << "    {\"name\":\"" << json_escape(dep.name)
                << "\",\"spec\":\"" << json_escape(dep.spec) << "\"}";
            if (i + 1 < deps.size()) out << ",";
            out << "\n";
        }
        out << "  ]\n"
            << "}\n";
        std::cout << out.str();
        return 0;
    }

    std::cout << "Installed packages\n";
    for (const auto& entry : entries) {
        std::cout << "  " << entry.name << "  " << entry.kind;
        if (!entry.version.empty()) std::cout << "@" << entry.version;
        std::cout << "\n";
    }
    if (!deps.empty()) {
        std::cout << "\nManifest dependencies\n";
        for (const auto& dep : deps) std::cout << "  " << dep.name << "  " << dep.spec << "\n";
    }
    return 0;
}

static bool json_number_field_text(const std::string& json, const std::string& field, std::string& value) {
    std::regex re("\"" + field + "\"\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)");
    std::smatch match;
    if (!std::regex_search(json, match, re)) return false;
    value = match[1].str();
    return true;
}

struct PackageInfoSymbol {
    std::string kind;
    std::string name;
    std::string signature;
    std::string source;
    int line = 0;
};

static std::vector<PackageInfoSymbol> builtin_stdlib_module_symbols(const std::string& raw_name) {
    std::string module = canonical_stdlib_module_name(raw_name);
    std::vector<std::pair<std::string, std::string>> methods;
    auto add = [&](const std::string& name, const std::string& signature) {
        methods.push_back({name, signature});
    };

    if (module == "array") {
        add("len", "array.len(array)");
        add("length", "array.length(array)");
        add("size", "array.size(array)");
        add("slice", "array.slice(array, start, [end])");
        add("sort", "array.sort(array)");
        add("reverse", "array.reverse(array)");
        add("concat", "array.concat(array, ...)");
        add("push", "array.push(array, value, ...)");
        add("pop", "array.pop(array)");
        add("clone", "array.clone(array)");
        add("contains", "array.contains(array, value)");
        add("index_of", "array.index_of(array, value)");
        add("sum", "array.sum(array)");
        add("avg", "array.avg(array)");
        add("unique", "array.unique(array)");
        add("flatten", "array.flatten(array, [depth])");
        add("range", "array.range(end) | array.range(start, end, [step])");
        add("chunk", "array.chunk(array, size)");
        add("zip", "array.zip(array, ...)");
        add("repeat", "array.repeat(value, count)");
    } else if (module == "set") {
        add("union", "set.union(array, ...)");
        add("intersection", "set.intersection(array, ...)");
        add("difference", "set.difference(array, ...)");
        add("symmetric_difference", "set.symmetric_difference(left, right)");
        add("is_subset", "set.is_subset(left, right)");
        add("is_superset", "set.is_superset(left, right)");
    } else if (module == "math") {
        add("sqrt", "math.sqrt(value)");
        add("sin", "math.sin(value)");
        add("cos", "math.cos(value)");
        add("tan", "math.tan(value)");
        add("floor", "math.floor(value)");
        add("ceil", "math.ceil(value)");
        add("round", "math.round(value)");
        add("abs", "math.abs(value)");
        add("sign", "math.sign(value)");
        add("pow", "math.pow(base, exponent)");
        add("min", "math.min(value, ...)");
        add("max", "math.max(value, ...)");
        add("clamp", "math.clamp(value, min, max)");
        add("random", "math.random([max]) | math.random(min, max)");
    } else if (module == "string") {
        add("len", "string.len(text)");
        add("length", "string.length(text)");
        add("size", "string.size(text)");
        add("split", "string.split(text, separator)");
        add("join", "string.join(array, separator)");
        add("trim", "string.trim(text)");
        add("upper", "string.upper(text)");
        add("lower", "string.lower(text)");
        add("contains", "string.contains(text, needle)");
        add("starts_with", "string.starts_with(text, prefix)");
        add("ends_with", "string.ends_with(text, suffix)");
        add("index_of", "string.index_of(text, needle)");
        add("sub", "string.sub(text, start, [end])");
        add("replace", "string.replace(text, from, to)");
        add("lines", "string.lines(text)");
        add("words", "string.words(text)");
        add("repeat", "string.repeat(text, count)");
        add("pad_left", "string.pad_left(text, width, [fill])");
        add("pad_right", "string.pad_right(text, width, [fill])");
        add("chunks", "string.chunks(text, [max_chars], [overlap])");
    } else if (module == "os") {
        add("env_get", "os.env_get(name, [default])");
        add("env_require", "os.env_require(name)");
        add("env_set", "os.env_set(name, value)");
        add("env_load", "os.env_load(path, [override])");
        add("argv", "os.argv()");
        add("argc", "os.argc()");
        add("script_name", "os.script_name()");
        add("cwd", "os.cwd()");
        add("home_dir", "os.home_dir()");
        add("temp_dir", "os.temp_dir()");
        add("path_separator", "os.path_separator()");
        add("name", "os.name()");
        add("is_windows", "os.is_windows()");
        add("which", "os.which(command)");
        add("cmd_exists", "os.cmd_exists(command)");
        add("cmd_quote", "os.cmd_quote(text)");
        add("cmd_join", "os.cmd_join(args)");
        add("run", "os.run(command)");
        add("run_checked", "os.run_checked(command)");
        add("cmd", "os.cmd(command)");
        add("sleep_ms", "os.sleep_ms(milliseconds)");
        add("wait", "os.wait(milliseconds)");
    } else if (module == "path") {
        add("join", "path.join(part, ...)");
        add("basename", "path.basename(path)");
        add("dirname", "path.dirname(path)");
        add("ext", "path.ext(path)");
        add("stem", "path.stem(path)");
        add("normalize", "path.normalize(path)");
        add("abs", "path.abs(path)");
        add("relative", "path.relative(path, [base])");
    } else if (module == "python") {
        add("available", "python.available()");
        add("executable", "python.executable()");
        add("eval", "python.eval(code)");
        add("call", "python.call(module, function, [args], [kwargs])");
        add("call_json", "python.call_json(module, function, [args], [kwargs])");
    } else if (module == "ffi") {
        add("load", "ffi.load(path)");
        add("call", "ffi.call(lib, symbol, signature, ...args)");
    } else if (module == "plugin") {
        add("load", "plugin.load(path)");
        add("load_manifest", "plugin.load_manifest(path)");
        add("call", "plugin.call(plugin, export, ...args)");
        add("info", "plugin.info(plugin)");
        add("unload", "plugin.unload(plugin)");
    } else if (module == "cli") {
        add("parse", "cli.parse(text, [value_flags])");
        add("argv", "cli.argv()");
        add("argc", "cli.argc()");
        add("script_name", "cli.script_name()");
    } else if (module == "json") {
        add("parse", "json.parse(text)");
        add("try_parse", "json.try_parse(text, [fallback])");
        add("stringify", "json.stringify(value)");
        add("pretty", "json.pretty(value, [indent])");
        add("path", "json.path(value, path, [default])");
        add("get_path", "json.get_path(value, path, [default])");
        add("has_path", "json.has_path(value, path)");
        add("merge_patch", "json.merge_patch(target, patch)");
        add("delete_path", "json.delete_path(value, path)");
        add("set_path", "json.set_path(value, path, new_value)");
        add("schema_validate", "json.schema_validate(value, schema)");
        add("schema_errors", "json.schema_errors(value, schema)");
        add("schema_to_json_schema", "json.schema_to_json_schema(schema, [strict])");
        add("to_json_schema", "json.to_json_schema(schema, [strict])");
        add("template_render", "json.template_render(text, data, [missing])");
        add("render", "json.render(text, data, [missing])");
        add("pluck", "json.pluck(rows, path, [default])");
        add("count_by", "json.count_by(rows, path)");
        add("group_by", "json.group_by(rows, path)");
        add("sort_by", "json.sort_by(rows, path, [descending])");
        add("jsonl_parse", "json.jsonl_parse(text)");
        add("jsonl_stringify", "json.jsonl_stringify(rows, [trailing_newline])");
        add("sse_parse", "json.sse_parse(text)");
        add("sse_data", "json.sse_data(text, [parse_json])");
        add("csv_parse", "json.csv_parse(text, [has_header])");
        add("csv_stringify", "json.csv_stringify(rows, [headers])");
        add("ini_parse", "json.ini_parse(text)");
        add("ini_stringify", "json.ini_stringify(dict)");
    } else if (module == "dict") {
        add("keys", "dict.keys(dict)");
        add("values", "dict.values(dict)");
        add("items", "dict.items(dict)");
        add("merge", "dict.merge(dict, ...)");
        add("pick", "dict.pick(dict, keys)");
        add("omit", "dict.omit(dict, keys)");
        add("get_path", "dict.get_path(value, path, [default])");
    } else if (module == "fs") {
        add("read", "fs.read(path)");
        add("write", "fs.write(path, text)");
        add("read_json", "fs.read_json(path)");
        add("write_json", "fs.write_json(path, value)");
        add("read_bytes", "fs.read_bytes(path)");
        add("write_bytes", "fs.write_bytes(path, bytes)");
        add("sha256", "fs.sha256(path)");
        add("append", "fs.append(path, text)");
        add("exists", "fs.exists(path)");
        add("delete", "fs.delete(path)");
        add("remove_tree", "fs.remove_tree(path)");
        add("list", "fs.list(path)");
        add("walk", "fs.walk(path, [extension])");
        add("glob", "fs.glob(pattern)");
        add("mkdir", "fs.mkdir(path)");
        add("cwd", "fs.cwd()");
        add("join", "fs.join(part, ...)");
        add("basename", "fs.basename(path)");
        add("dirname", "fs.dirname(path)");
        add("copy", "fs.copy(src, dst, [overwrite])");
        add("move", "fs.move(src, dst, [overwrite])");
    } else if (module == "regex") {
        add("match", "regex.match(text, pattern)");
        add("replace", "regex.replace(text, pattern, replacement)");
        add("find_all", "regex.find_all(text, pattern)");
        add("escape", "regex.escape(text)");
        add("capture", "regex.capture(text, pattern)");
        add("captures", "regex.captures(text, pattern)");
        add("split", "regex.split(text, pattern)");
    } else if (module == "random") {
        add("seed", "random.seed(seed)");
        add("int", "random.int(max) | random.int(min, max)");
        add("float", "random.float([max]) | random.float(min, max)");
        add("bool", "random.bool([probability])");
        add("choice", "random.choice(array)");
        add("shuffle", "random.shuffle(array)");
        add("bytes", "random.bytes(count)");
        add("uuid", "random.uuid()");
    } else if (module == "datetime") {
        add("now", "datetime.now()");
        add("parse", "datetime.parse(text, [format])");
        add("format", "datetime.format(timestamp, format)");
        add("utc_format", "datetime.utc_format(timestamp, format)");
        add("parts", "datetime.parts(timestamp, [utc])");
        add("add", "datetime.add(timestamp, seconds)");
        add("diff", "datetime.diff(end_timestamp, start_timestamp)");
        add("timestamp", "datetime.timestamp()");
    } else if (module == "crypto") {
        add("sha256", "crypto.sha256(text)");
        add("file_sha256", "crypto.file_sha256(path)");
        add("hmac_sha256", "crypto.hmac_sha256(key, message)");
        add("file_hmac_sha256", "crypto.file_hmac_sha256(key, path)");
        add("random_bytes", "crypto.random_bytes(count)");
        add("random_hex", "crypto.random_hex(count)");
        add("constant_time_eq", "crypto.constant_time_eq(left, right)");
        add("hex_encode", "crypto.hex_encode(text)");
        add("hex_decode", "crypto.hex_decode(text)");
        add("base64_encode", "crypto.base64_encode(text)");
        add("base64_decode", "crypto.base64_decode(text)");
        add("base64_url_encode", "crypto.base64_url_encode(text)");
        add("base64_url_decode", "crypto.base64_url_decode(text)");
        add("url_encode", "crypto.url_encode(text)");
        add("url_decode", "crypto.url_decode(text)");
        add("url_parse", "crypto.url_parse(url)");
        add("url_build", "crypto.url_build(parts)");
        add("query_build", "crypto.query_build(params)");
        add("query_parse", "crypto.query_parse(query)");
        add("form_build", "crypto.form_build(params)");
        add("form_parse", "crypto.form_parse(body)");
        add("auth_bearer", "crypto.auth_bearer(token)");
        add("auth_basic", "crypto.auth_basic(username, password)");
        add("headers_merge", "crypto.headers_merge(headers, ...)");
        add("headers_get", "crypto.headers_get(headers, name, [default])");
        add("headers_has", "crypto.headers_has(headers, name)");
        add("headers_redact", "crypto.headers_redact(headers, [names], [mask])");
        add("cookie_parse", "crypto.cookie_parse(header_or_headers)");
        add("cookie_build", "crypto.cookie_build(cookies)");
        add("cookie_get", "crypto.cookie_get(header_or_cookies, name, [default])");
        add("content_type", "crypto.content_type(headers_or_value, [default])");
        add("charset", "crypto.charset(headers_or_value, [default])");
        add("is_json", "crypto.is_json(headers_or_value)");
        add("status_ok", "crypto.status_ok(status)");
        add("status_text", "crypto.status_text(status)");
        add("status_retryable", "crypto.status_retryable(status)");
        add("retry_after", "crypto.retry_after(headers_or_value, [default_ms])");
        add("backoff_delays", "crypto.backoff_delays(attempts, [base_ms], [factor], [max_ms])");
    } else if (module == "db") {
        add("set", "db.set(path, key, value)");
        add("get", "db.get(path, key, [default])");
        add("has", "db.has(path, key)");
        add("delete", "db.delete(path, key)");
        add("keys", "db.keys(path)");
        add("all", "db.all(path)");
        add("insert", "db.insert(path, row)");
        add("find", "db.find(path, criteria)");
        add("count", "db.count(path, [criteria])");
        add("update", "db.update(path, criteria, patch)");
        add("remove", "db.remove(path, criteria)");
        add("query", "db.query(path, [criteria], [options])");
    } else if (module == "log") {
        add("set_file", "log.set_file(path, [append])");
        add("set_json", "log.set_json(enabled)");
        add("set_level", "log.set_level(level)");
        add("get_level", "log.get_level()");
        add("level", "log.level([level])");
        add("event", "log.event(level, message, [fields])");
        add("debug", "log.debug(message)");
        add("info", "log.info(message)");
        add("warn", "log.warn(message)");
        add("error", "log.error(message)");
    } else if (module == "http") {
        add("get", "http.get(url)");
        add("json", "http.json(url)");
        add("post", "http.post(url, body, [content_type])");
        add("request", "http.request(spec)");
        add("request_full", "http.request_full(spec)");
        add("request_retry", "http.request_retry(spec, [attempts], [delay_ms])");
        add("request_json", "http.request_json(spec)");
        add("request_json_checked", "http.request_json_checked(spec)");
        add("request_retry_json", "http.request_retry_json(spec, [attempts], [delay_ms])");
        add("request_retry_json_checked", "http.request_retry_json_checked(spec, [attempts], [delay_ms])");
        add("serve_static", "http.serve_static(path, [port])");
        add("serve_routes", "http.serve_routes(routes, [port])");
        add("server_url", "http.server_url(server)");
        add("server_stop", "http.server_stop(server)");
        add("url_parse", "http.url_parse(url)");
        add("url_build", "http.url_build(parts)");
        add("query_build", "http.query_build(params)");
        add("query_parse", "http.query_parse(query)");
        add("form_build", "http.form_build(params)");
        add("form_parse", "http.form_parse(body)");
        add("auth_bearer", "http.auth_bearer(token)");
        add("auth_basic", "http.auth_basic(username, password)");
        add("headers_merge", "http.headers_merge(headers, ...)");
        add("headers_get", "http.headers_get(headers, name, [default])");
        add("headers_has", "http.headers_has(headers, name)");
        add("headers_redact", "http.headers_redact(headers, [names], [mask])");
        add("cookie_parse", "http.cookie_parse(header_or_headers)");
        add("cookie_build", "http.cookie_build(cookies)");
        add("cookie_get", "http.cookie_get(header_or_cookies, name, [default])");
        add("content_type", "http.content_type(headers_or_value, [default])");
        add("charset", "http.charset(headers_or_value, [default])");
        add("is_json", "http.is_json(headers_or_value)");
        add("status_ok", "http.status_ok(status)");
        add("status_text", "http.status_text(status)");
        add("status_retryable", "http.status_retryable(status)");
        add("retry_after", "http.retry_after(headers_or_value, [default_ms])");
        add("backoff_delays", "http.backoff_delays(attempts, [base_ms], [factor], [max_ms])");
    } else if (module == "async") {
        add("cmd", "async.cmd(command, [scope_id])");
        add("ready", "async.ready(task_id)");
        add("http_get", "async.http_get(url, [scope_id])");
        add("http_request", "async.http_request(spec, [scope_id])");
        add("sleep", "async.sleep(milliseconds, [scope_id])");
        add("sura", "async.sura(spec, [scope_id])");
        add("status", "async.status(task_id)");
        add("pending", "async.pending()");
        add("forget", "async.forget(task_id)");
        add("cleanup", "async.cleanup()");
        add("cancel", "async.cancel(task_id)");
        add("cancelled", "async.cancelled(task_id)");
        add("configure", "async.configure(max_workers, max_queue)");
        add("limits", "async.limits()");
        add("scope", "async.scope()");
        add("scope_open", "async.scope_open()");
        add("scope_attach", "async.scope_attach(scope_id, task_id)");
        add("scope_cancel", "async.scope_cancel(scope_id)");
        add("scope_status", "async.scope_status(scope_id)");
        add("scope_close", "async.scope_close(scope_id, [milliseconds])");
        add("scope_join", "async.scope_join(scope_id, [milliseconds])");
        add("await", "async.await(task_id)");
        add("await_timeout", "async.await_timeout(task_id, milliseconds, [default])");
        add("ready_all", "async.ready_all(task_ids)");
        add("any", "async.any(task_ids, [milliseconds], [default])");
        add("all", "async.all(task_ids)");
        add("all_timeout", "async.all_timeout(task_ids, milliseconds, [default])");
    } else if (module == "test") {
        add("assert", "test.assert(condition, [message])");
        add("eq", "test.eq(actual, expected, [message])");
        add("ne", "test.ne(actual, expected, [message])");
        add("neq", "test.neq(actual, expected, [message])");
        add("contains", "test.contains(container, value, [message])");
        add("not_contains", "test.not_contains(container, value, [message])");
        add("match", "test.match(text, pattern, [message])");
        add("type", "test.type(value, type, [message])");
        add("len", "test.len(value, length, [message])");
        add("between", "test.between(value, min, max, [message])");
        add("approx", "test.approx(actual, expected, [epsilon], [message])");
        add("check", "test.check(name, condition, [message])");
        add("check_eq", "test.check_eq(name, actual, expected, [message])");
        add("check_match", "test.check_match(name, text, pattern, [message])");
        add("summary", "test.summary(results)");
        add("report", "test.report(results, [title])");
    } else if (module == "vector") {
        add("vec3", "vector.vec3(x, y, z)");
        add("add", "vector.add(a, b)");
        add("dot", "vector.dot(a, b)");
        add("scale", "vector.scale(vector, scalar)");
        add("norm", "vector.norm(vector)");
        add("add3", "vector.add3(a, b)");
        add("sub3", "vector.sub3(a, b)");
        add("dot3", "vector.dot3(a, b)");
        add("cross", "vector.cross(a, b)");
        add("scale3", "vector.scale3(vector, scalar)");
        add("norm3", "vector.norm3(vector)");
        add("normalize3", "vector.normalize3(vector)");
        add("distance3", "vector.distance3(a, b)");
        add("neg3", "vector.neg3(vector)");
        add("lerp3", "vector.lerp3(a, b, t)");
        add("midpoint3", "vector.midpoint3(a, b)");
        add("project3", "vector.project3(vector, onto)");
        add("reject3", "vector.reject3(vector, onto)");
        add("reflect3", "vector.reflect3(vector, normal)");
        add("angle3", "vector.angle3(a, b)");
        add("transform4", "vector.transform4(vector, matrix4)");
        add("cosine", "vector.cosine(a, b)");
        add("normalize", "vector.normalize(vector)");
        add("search", "vector.search(query, rows, [k], [field])");
    } else if (module == "graphics3d") {
        add("identity", "graphics3d.identity()");
        add("translate", "graphics3d.translate(x, y, z)");
        add("scale", "graphics3d.scale(x, y, z)");
        add("rotate_y", "graphics3d.rotate_y(radians)");
        add("mul", "graphics3d.mul(left, right)");
        add("cube", "graphics3d.cube([size], [center])");
        add("transform", "graphics3d.transform(mesh, matrix4)");
        add("bounds", "graphics3d.bounds(mesh)");
        add("face_normals", "graphics3d.face_normals(mesh)");
        add("project", "graphics3d.project(point, camera, [width], [height])");
    } else if (module == "rag") {
        add("context", "rag.context(query, docs, [k], [embedding_field], [text_field])");
        add("sources", "rag.sources(query, docs, [k], [embedding_field], [text_field], [title_field])");
        add("prepare", "rag.prepare(question, query, docs, [k], [embedding_field], [text_field], [system], [title_field])");
        add("messages", "rag.messages(question, context, [system])");
    } else if (module == "tensor") {
        add("shape", "tensor.shape(tensor)");
        add("zeros", "tensor.zeros(shape)");
        add("fill", "tensor.fill(shape, value)");
        add("add", "tensor.add(a, b)");
        add("mul", "tensor.mul(a, b)");
        add("clip", "tensor.clip(tensor, min, max)");
        add("flatten", "tensor.flatten(tensor)");
        add("sum", "tensor.sum(tensor)");
        add("mean", "tensor.mean(tensor)");
        add("variance", "tensor.variance(tensor)");
        add("std", "tensor.std(tensor)");
        add("min", "tensor.min(tensor)");
        add("max", "tensor.max(tensor)");
        add("argmin", "tensor.argmin(tensor)");
        add("argmax", "tensor.argmax(tensor)");
        add("zscore", "tensor.zscore(tensor)");
        add("softmax", "tensor.softmax(tensor)");
        add("transpose", "tensor.transpose(matrix)");
        add("matmul", "tensor.matmul(a, b)");
    } else if (module == "nn") {
        add("mlp", "nn.mlp(layer_sizes, [options])");
        add("forward", "nn.forward(model, inputs)");
        add("predict", "nn.predict(model, inputs)");
        add("train", "nn.train(model, inputs, targets, [options])");
        add("classify", "nn.classify(model, inputs, [threshold])");
        add("evaluate", "nn.evaluate(model, inputs, targets, [options])");
        add("summary", "nn.summary(model)");
        add("one_hot", "nn.one_hot(labels, class_count)");
        add("fit_standardizer", "nn.fit_standardizer(inputs)");
        add("standardize", "nn.standardize(inputs, standardizer)");
        add("split", "nn.split(inputs, targets, [options])");
        add("save", "nn.save(model, path)");
        add("load", "nn.load(path)");
    } else if (module == "autograd") {
        add("tensor", "autograd.tensor(data, [options])");
        add("parameter", "autograd.parameter(data, [dtype_or_options])");
        add("zeros", "autograd.zeros(shape, [options])");
        add("ones", "autograd.ones(shape, [options])");
        add("randn", "autograd.randn(shape, [options])");
        add("data", "autograd.data(tensor)");
        add("grad", "autograd.grad(tensor)");
        add("dtype", "autograd.dtype(tensor)");
        add("device", "autograd.device(tensor)");
        add("to", "autograd.to(tensor, device)");
        add("storage_bytes", "autograd.storage_bytes(tensor)");
        add("cast", "autograd.cast(tensor, dtype)");
        add("shape", "autograd.shape(tensor)");
        add("numel", "autograd.numel(tensor)");
        add("limits", "autograd.limits()");
        add("item", "autograd.item(tensor)");
        add("detach", "autograd.detach(tensor)");
        add("requires_grad", "autograd.requires_grad(tensor)");
        add("set_requires_grad", "autograd.set_requires_grad(tensor, requires_grad)");
        add("add", "autograd.add(a, b)");
        add("sub", "autograd.sub(a, b)");
        add("mul", "autograd.mul(a, b)");
        add("div", "autograd.div(a, b)");
        add("neg", "autograd.neg(tensor)");
        add("reshape", "autograd.reshape(tensor, shape)");
        add("matmul", "autograd.matmul(a, b, [options])");
        add("transpose", "autograd.transpose(tensor, [axis1], [axis2])");
        add("linear", "autograd.linear(input, weights, [bias])");
        add("relu", "autograd.relu(tensor)");
        add("tanh", "autograd.tanh(tensor)");
        add("sigmoid", "autograd.sigmoid(tensor)");
        add("gelu", "autograd.gelu(tensor)");
        add("layer_norm", "autograd.layer_norm(tensor, [weight], [bias], [epsilon])");
        add("embedding", "autograd.embedding(token_ids, weight)");
        add("causal_attention", "autograd.causal_attention(query, key, value, [options])");
        add("softmax", "autograd.softmax(tensor)");
        add("sum", "autograd.sum(tensor)");
        add("mean", "autograd.mean(tensor)");
        add("mse", "autograd.mse(prediction, target)");
        add("bce", "autograd.bce(probabilities, target)");
        add("bce_logits", "autograd.bce_logits(logits, target)");
        add("cross_entropy", "autograd.cross_entropy(logits, one_hot_targets)");
        add("cross_entropy_ids", "autograd.cross_entropy_ids(logits, class_ids)");
        add("backward", "autograd.backward(tensor, [gradient], [retain_graph])");
        add("zero_grad", "autograd.zero_grad(parameters)");
        add("sgd", "autograd.sgd(parameters, learning_rate, [options])");
        add("adam", "autograd.adam(parameters, learning_rate, [options])");
        add("reset_optimizer", "autograd.reset_optimizer(parameters)");
        add("grad_norm", "autograd.grad_norm(parameters)");
        add("clip_grad_norm", "autograd.clip_grad_norm(parameters, max_norm)");
        add("save_checkpoint", "autograd.save_checkpoint(state_dict, path, [options])");
        add("load_checkpoint", "autograd.load_checkpoint(path, [options])");
        add("cuda_available", "autograd.cuda_available()");
        add("cuda_info", "autograd.cuda_info()");
        add("cuda_stats", "autograd.cuda_stats()");
        add("cuda_reset_stats", "autograd.cuda_reset_stats()");
        add("cuda_synchronize", "autograd.cuda_synchronize()");
        add("save_safetensors", "autograd.save_safetensors(state_dict, path)");
        add("load_safetensors", "autograd.load_safetensors(path, [options])");
        add("save_onnx_weights", "autograd.save_onnx_weights(state_dict, path)");
        add("load_onnx_weights", "autograd.load_onnx_weights(path, [options])");
        add("run_onnx", "autograd.run_onnx(path, inputs, [options])");
        add("all_reduce_gradients", "autograd.all_reduce_gradients(parameters, options)");
    } else if (module == "tokenizer") {
        add("byte", "tokenizer.byte([options])");
        add("train_bpe", "tokenizer.train_bpe(corpus, [options])");
        add("encode", "tokenizer.encode(tokenizer, text, [options])");
        add("decode", "tokenizer.decode(tokenizer, ids, [options])");
        add("info", "tokenizer.info(tokenizer)");
        add("save", "tokenizer.save(tokenizer, path)");
        add("load", "tokenizer.load(path)");
    } else if (module == "dataset") {
        add("pack_text", "dataset.pack_text(source, tokenizer, path, [options])");
        add("open", "dataset.open(path, [options])");
        add("next", "dataset.next(loader)");
        add("reset", "dataset.reset(loader, [epoch])");
        add("close", "dataset.close(loader)");
        add("info", "dataset.info(loader)");
    } else if (module == "media") {
        add("available", "media.available([ffmpeg_path])");
        add("ffmpeg_available", "media.ffmpeg_available([ffmpeg_path])");
        add("frame_to_text", "media.frame_to_text(pixels, [options])");
        add("ascii_frames", "media.ascii_frames(path, [options])");
        add("video_to_text", "media.video_to_text(path, [options])");
        add("video_text_frames", "media.video_text_frames(path, [options])");
    } else if (module == "stream") {
        add("from", "stream.from(array_or_text)");
        add("next", "stream.next(stream)");
        add("collect", "stream.collect(stream)");
        add("take", "stream.take(stream, count)");
        add("batch", "stream.batch(stream, size)");
        add("map", "stream.map(stream, path, [fallback])");
        add("filter", "stream.filter(stream, criteria)");
        add("window", "stream.window(stream, size, [step])");
        add("skip", "stream.skip(stream, count)");
        add("count", "stream.count(stream)");
        add("join", "stream.join(stream, [separator])");
        add("sum", "stream.sum(stream, [path])");
        add("avg", "stream.avg(stream, [path])");
        add("lines", "stream.lines(path)");
    } else if (module == "tool") {
        add("call", "tool.call(spec)");
        add("spec", "tool.spec(name, args)");
        add("validate", "tool.validate(spec)");
        add("schema", "tool.schema(name)");
        add("allowed", "tool.allowed(spec, policy)");
        add("call_policy", "tool.call_policy(spec, policy)");
        add("list", "tool.list()");
    } else if (module == "llm") {
        add("message", "llm.message(role, content)");
        add("messages", "llm.messages([system], user)");
        add("rag_messages", "llm.rag_messages(question, context, [system])");
        add("request", "llm.request(model, messages, [temperature])");
        add("request_json", "llm.request_json(model, messages, [temperature])");
        add("response_schema", "llm.response_schema(name, schema, [strict])");
        add("request_schema", "llm.request_schema(model, messages, schema, [temperature], [name], [strict])");
        add("request_schema_json", "llm.request_schema_json(model, messages, schema, [temperature], [name], [strict])");
        add("tools", "llm.tools([names])");
        add("tool_schemas", "llm.tool_schemas([names])");
        add("request_tools", "llm.request_tools(model, messages, tool_names, [temperature])");
        add("request_tools_json", "llm.request_tools_json(model, messages, tool_names, [temperature])");
        add("request_tools_schema", "llm.request_tools_schema(model, messages, tool_names, schema, [temperature], [name], [strict])");
        add("request_tools_schema_json", "llm.request_tools_schema_json(model, messages, tool_names, schema, [temperature], [name], [strict])");
        add("extract_text", "llm.extract_text(response)");
        add("extract_json", "llm.extract_json(response, [schema])");
        add("usage", "llm.usage(response)");
        add("cost", "llm.cost(response, pricing)");
        add("budget", "llm.budget(response, pricing, limit)");
        add("tool_calls", "llm.tool_calls(response)");
        add("tool_result", "llm.tool_result(call_or_id, result)");
        add("run_tools", "llm.run_tools(response, policy)");
        add("next_messages", "llm.next_messages(messages, response, policy)");
        add("next_request", "llm.next_request(model, messages, response, policy, tool_names, [temperature])");
        add("next_request_json", "llm.next_request_json(model, messages, response, policy, tool_names, [temperature])");
        add("next_schema_request", "llm.next_schema_request(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])");
        add("next_schema_request_json", "llm.next_schema_request_json(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])");
        add("stream_text", "llm.stream_text(sse_or_chunks)");
        add("chat", "llm.chat(endpoint, api_key, model, messages, [temperature])");
        add("chat_request", "llm.chat_request(endpoint, api_key, request)");
    }

    std::vector<PackageInfoSymbol> symbols;
    int line = 1;
    for (const auto& method : methods) {
        PackageInfoSymbol symbol;
        symbol.kind = "function";
        symbol.name = method.first;
        symbol.signature = method.second;
        symbol.source = "builtin:" + module;
        symbol.line = line++;
        symbols.push_back(symbol);
    }
    return symbols;
}

static std::vector<StdlibSearchEntry> builtin_stdlib_search_entries() {
    std::vector<StdlibSearchEntry> entries;
    for (const auto& module : builtin_stdlib_module_names()) {
        entries.push_back({"module", module, module, "use " + module, "builtin:" + module, 0});
        for (const auto& symbol : builtin_stdlib_module_symbols(module)) {
            entries.push_back({symbol.kind,
                               module,
                               module + "." + symbol.name,
                               symbol.signature,
                               symbol.source,
                               symbol.line});
        }
    }
    return entries;
}

static std::vector<PackageInfoSymbol> collect_local_info_symbols(const std::vector<fs::path>& files) {
    std::vector<PackageInfoSymbol> symbols;
    std::regex decl_re("^\\s*(func|class|struct|enum)\\s+([A-Za-z_][A-Za-z0-9_]*)");
    std::regex constant_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+is\\s+(.+?)\\s*$");
    std::regex literal_re("^([-+]?[0-9]+(\\.[0-9]+)?|\"(\\\\.|[^\"])*\"|true|false|nil)$");
    std::regex block_open_re("\\bdo\\s*$");
    for (const auto& file : files) {
        std::istringstream lines(read_all(file));
        std::string line;
        int line_no = 0;
        int block_depth = 0;
        while (std::getline(lines, line)) {
            ++line_no;
            // getline keeps the carriage return of a CRLF file. Rules that
            // match a whole line (assignments, for one) then never fire, so a
            // file saved on Windows would silently lose findings that the same
            // file with LF endings reports.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::smatch m;
            std::string trimmed = trim_copy(line);
            if (std::regex_search(line, m, decl_re)) {
                PackageInfoSymbol symbol;
                symbol.kind = m[1].str() == "func" ? "function" : m[1].str();
                symbol.name = m[2].str();
                symbol.signature = trimmed;
                symbol.source = path_to_generic_utf8(file);
                symbol.line = line_no;
                symbols.push_back(symbol);
            } else if (block_depth == 0 && std::regex_match(line, m, constant_re)) {
                std::string value = trim_copy(m[2].str());
                if (std::regex_match(value, literal_re)) {
                    PackageInfoSymbol symbol;
                    symbol.kind = "constant";
                    symbol.name = m[1].str();
                    symbol.signature = trimmed;
                    symbol.source = path_to_generic_utf8(file);
                    symbol.line = line_no;
                    symbols.push_back(symbol);
                }
            }

            if (std::regex_search(trimmed, block_open_re)) ++block_depth;
            if (trimmed == "end" && block_depth > 0) --block_depth;
        }
    }
    return symbols;
}

static std::vector<PackageInfoSymbol> collect_remote_info_symbols(const std::string& detail) {
    std::vector<PackageInfoSymbol> symbols;
    std::regex object_re("\\{[^{}]*\"kind\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^{}]*\\}");
    for (auto it = std::sregex_iterator(detail.begin(), detail.end(), object_re);
         it != std::sregex_iterator(); ++it) {
        std::string object = it->str();
        PackageInfoSymbol symbol;
        symbol.kind = manifest_string_field(object, "kind", "");
        symbol.name = manifest_string_field(object, "name", "");
        symbol.signature = manifest_string_field(object, "signature", "");
        symbol.source = manifest_string_field(object, "source", "");
        std::string line_text;
        if (json_number_field_text(object, "line", line_text)) {
            try {
                symbol.line = std::stoi(line_text);
            } catch (...) {
                symbol.line = 0;
            }
        }
        if (!symbol.kind.empty() && !symbol.name.empty()) symbols.push_back(symbol);
    }
    return symbols;
}

static std::string package_info_json(const std::string& query,
                                     const std::string& source,
                                     const std::string& name,
                                     const std::string& version,
                                     const std::string& path,
                                     const std::string& owner,
                                     const std::string& bundle,
                                     const std::string& speedup,
                                     const std::string& sura_faster_by_python,
                                     bool audit_present,
                                     const std::string& audit_passed,
                                     const std::string& audit_finding_count,
                                     const std::vector<PackageInfoSymbol>& symbols) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.info.v1\",\n"
        << "  \"passed\": true,\n"
        << "  \"query\": \"" << json_escape(query) << "\",\n"
        << "  \"source\": \"" << json_escape(source) << "\",\n"
        << "  \"package\": \"" << json_escape(name) << "\",\n"
        << "  \"version\": \"" << json_escape(version) << "\",\n"
        << "  \"path\": \"" << json_escape(path) << "\",\n"
        << "  \"owner\": \"" << json_escape(owner) << "\",\n"
        << "  \"bundle\": \"" << json_escape(bundle) << "\",\n"
        << "  \"benchmark\": {\n"
        << "    \"speedup\": " << (speedup.empty() ? "null" : speedup) << ",\n"
        << "    \"sura_faster_by_python\": "
        << (sura_faster_by_python.empty() ? "null" : sura_faster_by_python) << "\n"
        << "  },\n"
        << "  \"audit\": {\n"
        << "    \"present\": " << (audit_present ? "true" : "false") << ",\n"
        << "    \"passed\": " << (audit_present && !audit_passed.empty() ? audit_passed : "null") << ",\n"
        << "    \"finding_count\": " << (audit_present && !audit_finding_count.empty() ? audit_finding_count : "null") << "\n"
        << "  },\n"
        << "  \"symbol_count\": " << symbols.size() << ",\n"
        << "  \"symbols\": [\n";
    for (size_t i = 0; i < symbols.size(); ++i) {
        const auto& symbol = symbols[i];
        out << "    {"
            << "\"kind\":\"" << json_escape(symbol.kind) << "\""
            << ",\"name\":\"" << json_escape(symbol.name) << "\""
            << ",\"signature\":\"" << json_escape(symbol.signature) << "\""
            << ",\"source\":\"" << json_escape(symbol.source) << "\""
            << ",\"line\":" << symbol.line
            << "}";
        if (i + 1 < symbols.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
    return out.str();
}

static int cmd_remote_info(const PackageRef& ref, bool json_output) {
    std::string url = registry_package_detail_url(ref);
    if (url.empty()) return err("package not found: " + ref.name);

    std::string detail = http_get_text(url);
    std::string label = ref.name + (ref.version.empty() ? "" : ("@" + ref.version));
    if (detail.empty()) return err("package not found locally or in registry: " + label);

    std::string name = manifest_field(detail, "name", ref.name);
    std::string version = manifest_field(detail, "version", ref.version.empty() ? "latest" : ref.version);
    std::string owner = manifest_string_field(detail, "owner", "");
    std::string bundle = manifest_string_field(detail, "bundle", "");
    std::string speedup;
    std::string python_ratio;
    bool has_speedup = json_number_field_text(detail, "speedup", speedup);
    bool has_python_ratio = json_number_field_text(detail, "suraFasterByPython", python_ratio);
    if (!has_python_ratio) has_python_ratio = json_number_field_text(detail, "sura_faster_by_python", python_ratio);
    std::string audit_body = json_object_field(detail, "audit");
    bool has_audit = !audit_body.empty();
    bool audit_passed_bool = false;
    bool audit_passed_found = has_audit && audit_body.find("\"passed\"") != std::string::npos;
    if (audit_passed_found) audit_passed_bool = json_bool_field_near(audit_body, "passed", false);
    std::string audit_passed = audit_passed_found ? (audit_passed_bool ? "true" : "false") : "";
    std::string audit_finding_count;
    bool has_audit_finding_count = false;
    if (has_audit) {
        has_audit_finding_count = json_number_field_text(audit_body, "finding_count", audit_finding_count);
        if (!has_audit_finding_count) {
            has_audit_finding_count = json_number_field_text(audit_body, "findingCount", audit_finding_count);
        }
    }
    auto symbols = collect_remote_info_symbols(detail);

    if (json_output) {
        std::cout << package_info_json(label, "registry", name, version, "", owner, bundle,
                                       has_speedup ? speedup : "",
                                       has_python_ratio ? python_ratio : "",
                                       has_audit,
                                       audit_passed,
                                       has_audit_finding_count ? audit_finding_count : "",
                                       symbols);
        return 0;
    }

    std::cout << name << "@" << version << " (registry)\n";
    if (!owner.empty()) std::cout << "owner: " << owner << "\n";
    if (!bundle.empty()) std::cout << "bundle: " << bundle << "\n";

    if (has_speedup || has_python_ratio) {
        std::cout << "Benchmark\n";
        if (has_speedup) std::cout << "  JIT speedup: " << speedup << "x\n";
        if (has_python_ratio) std::cout << "  Sura faster than Python: " << python_ratio << "x\n";
    }
    if (has_audit) {
        std::cout << "Security Audit\n";
        if (audit_passed_found) std::cout << "  passed: " << audit_passed << "\n";
        if (has_audit_finding_count) std::cout << "  findings: " << audit_finding_count << "\n";
    }

    std::cout << "Functions\n";
    bool shown = false;
    for (const auto& symbol : symbols) {
        if (symbol.kind != "function") continue;
        std::cout << "  " << symbol.name << "  (" << symbol.source << ")\n";
        shown = true;
    }
    if (!shown) std::cout << "  <none reported>\n";
    return 0;
}

static int run_info_command(const std::string& name, bool json_output) {
    if (name.empty()) return err("info requires a package name");
    PackageRef ref = parse_package_ref(name);
    fs::path root = package_dir(ref.name);
    std::string source = "package";
    std::string version;
    if (!fs::exists(root)) {
        fs::path std_file = kStdlib / (ref.name + ".sura");
        if (fs::exists(std_file)) {
            source = "stdlib";
            root = std_file;
        } else if (is_builtin_stdlib_module_name(ref.name)) {
            source = "stdlib";
            root = fs::path("builtin:" + canonical_stdlib_module_name(ref.name));
        } else {
            return cmd_remote_info(ref, json_output);
        }
    } else {
        std::string manifest = read_all(root / kManifest);
        version = manifest_field(manifest, "version", "");
    }

    if (!json_output) {
        if (source == "stdlib") {
            std::cout << ref.name << " (stdlib)\n";
        } else {
            std::string manifest = read_all(root / kManifest);
            std::cout << normalize_name(ref.name) << " (package)\n";
            if (!manifest.empty()) std::cout << manifest << "\n";
        }
    }

    std::vector<PackageInfoSymbol> symbols;
    if (is_builtin_stdlib_module_name(ref.name) && root.generic_string().rfind("builtin:", 0) == 0) {
        symbols = builtin_stdlib_module_symbols(ref.name);
    } else {
        std::vector<fs::path> files;
        if (fs::is_directory(root)) {
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (entry.path().extension() == ".sura") files.push_back(entry.path());
            }
        } else {
            files.push_back(root);
        }
        symbols = collect_local_info_symbols(files);
    }
    if (json_output) {
        std::string info_name = normalize_name(ref.name);
        if (source == "stdlib" && root.generic_string().rfind("builtin:", 0) == 0) {
            info_name = canonical_stdlib_module_name(info_name);
        }
        std::cout << package_info_json(name, source, info_name, version,
                                       path_to_generic_utf8(root), "", "", "", "",
                                       false, "", "", symbols);
        return 0;
    }

    std::cout << "Functions\n";
    for (const auto& symbol : symbols) {
        if (symbol.kind == "function") {
            std::cout << "  " << symbol.name << "  (" << symbol.source << ")\n";
        }
    }
    return 0;
}

static int cmd_info(const std::vector<std::string>& argv) {
    std::string name;
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg info <name> [--json]\n";
            return 0;
        } else if (name.empty()) {
            name = arg;
        } else {
            return err("info accepts one package name and optional --json");
        }
    }
    return run_info_command(name, json_output);
}

struct DependencyRequirement {
    std::string name;
    std::string spec;
    std::string required_by;
    fs::path base;
};

struct ResolvedDependency {
    std::string name;
    std::string version;
    std::string source;
    std::string constraint;
    fs::path source_path;
    bool installed = false;
    bool from_registry = false;
    bool from_file = false;
    std::vector<DependencyRequirement> requirements;
};

struct DependencyGraph {
    std::map<std::string, ResolvedDependency> packages;
    std::vector<std::string> errors;
};

static bool dependency_spec_is_file(const std::string& spec) {
    return trim_copy(spec).rfind("file:", 0) == 0;
}

static fs::path dependency_file_path(const DependencyRequirement& req) {
    std::string rel = trim_copy(req.spec).substr(5);
    fs::path p(rel);
    if (p.is_relative()) p = req.base / p;
    return p.lexically_normal();
}

static std::string package_label(const std::string& name, const std::string& version) {
    return normalize_name(name) + (version.empty() ? "" : ("@" + version));
}

static std::string requirement_key(const DependencyRequirement& req) {
    return normalize_name(req.name) + "|" + trim_copy(req.spec) + "|" + req.required_by + "|" + req.base.generic_string();
}

static std::string combined_dependency_constraint(const std::string& name, const std::vector<DependencyRequirement>& requirements) {
    std::string combined;
    for (const auto& req : requirements) {
        std::string constraint = dependency_constraint(name, req.spec);
        if (constraint.empty() || constraint == "*" || constraint == "latest") continue;
        if (!combined.empty()) combined += " ";
        combined += constraint;
    }
    return combined;
}

static std::string dependency_requirement_summary(const std::vector<DependencyRequirement>& requirements) {
    std::string out;
    for (const auto& req : requirements) {
        if (!out.empty()) out += "; ";
        out += (req.required_by.empty() ? "root" : req.required_by);
        out += " -> ";
        out += req.spec.empty() ? "*" : req.spec;
    }
    return out;
}

static std::string registry_package_manifest(const std::string& name, const std::string& version) {
    if (registry_url().empty()) {
        return read_all(registry_root() / normalize_name(name) / version / kManifest);
    }
    PackageRef ref;
    ref.name = normalize_name(name);
    ref.version = version;
    std::string bundle = http_get_text(registry_bundle_url(ref));
    std::string manifest;
    if (package_bundle_file_content(bundle, kManifest.generic_string(), manifest)) return manifest;
    return "";
}

static std::string selected_dependency_manifest(const ResolvedDependency& dep) {
    if (!dep.source_path.empty() && fs::is_directory(dep.source_path)) {
        return read_all(dep.source_path / kManifest);
    }
    if (dep.from_registry && !dep.version.empty()) {
        return registry_package_manifest(dep.name, dep.version);
    }
    return "";
}

static void print_dependency_graph_errors(const DependencyGraph& graph) {
    for (const auto& msg : graph.errors) {
        std::cout << "[resolve] " << msg << "\n";
    }
}

static void write_dependency_graph_json(std::ostream& out, const fs::path& root, const DependencyGraph& graph) {
    std::string manifest = read_all(root / kManifest);
    std::string root_name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string root_version = manifest_field(manifest, "version", "0.1.0");
    out << "{\n"
        << "  \"schema\": \"sura.package.resolve.v1\",\n"
        << "  \"ok\": " << (graph.errors.empty() ? "true" : "false") << ",\n"
        << "  \"root\": {\"name\":\"" << json_escape(root_name)
        << "\",\"version\":\"" << json_escape(root_version)
        << "\",\"path\":\"" << json_escape(root.generic_string()) << "\"},\n"
        << "  \"packages\": [";
    size_t index = 0;
    for (const auto& item : graph.packages) {
        const auto& dep = item.second;
        if (index++) out << ",";
        out << "\n"
            << "    {\n"
            << "      \"name\": \"" << json_escape(dep.name) << "\",\n"
            << "      \"version\": \"" << json_escape(dep.version) << "\",\n"
            << "      \"resolved\": " << (!dep.version.empty() ? "true" : "false") << ",\n"
            << "      \"constraint\": \"" << json_escape(dep.constraint) << "\",\n"
            << "      \"source\": \"" << json_escape(dep.source) << "\",\n"
            << "      \"installed\": " << (dep.installed ? "true" : "false") << ",\n"
            << "      \"from_registry\": " << (dep.from_registry ? "true" : "false") << ",\n"
            << "      \"from_file\": " << (dep.from_file ? "true" : "false") << ",\n"
            << "      \"requirements\": [";
        for (size_t i = 0; i < dep.requirements.size(); ++i) {
            const auto& req = dep.requirements[i];
            if (i) out << ",";
            out << "\n"
                << "        {\"required_by\":\"" << json_escape(req.required_by.empty() ? "root" : req.required_by)
                << "\",\"spec\":\"" << json_escape(req.spec.empty() ? "*" : req.spec)
                << "\",\"base\":\"" << json_escape(req.base.generic_string()) << "\"}";
        }
        if (!dep.requirements.empty()) out << "\n      ";
        out << "]\n"
            << "    }";
    }
    if (!graph.packages.empty()) out << "\n  ";
    out << "],\n"
        << "  \"errors\": [";
    for (size_t i = 0; i < graph.errors.size(); ++i) {
        if (i) out << ",";
        out << "\n    \"" << json_escape(graph.errors[i]) << "\"";
    }
    if (!graph.errors.empty()) out << "\n  ";
    out << "]\n"
        << "}\n";
}

static DependencyGraph resolve_dependency_graph(const fs::path& root, bool verbose) {
    DependencyGraph graph;
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) {
        graph.errors.push_back("sura.pkg.json not found: " + (root / kManifest).generic_string());
        if (verbose) print_dependency_graph_errors(graph);
        return graph;
    }

    std::string root_name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string root_version = manifest_field(manifest, "version", "0.1.0");
    std::string root_label = package_label(root_name, root_version);
    auto root_deps = manifest_dependency_specs(manifest);

    std::vector<DependencyRequirement> queue;
    std::set<std::string> queued;
    auto enqueue = [&](const DependencySpec& dep, const std::string& required_by, const fs::path& base) {
        DependencyRequirement req{normalize_name(dep.name), dep.spec, required_by, base};
        std::string key = requirement_key(req);
        if (queued.insert(key).second) queue.push_back(req);
    };

    for (const auto& dep : root_deps) enqueue(dep, root_label, root);

    std::set<std::string> expanded;
    for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
        DependencyRequirement req = queue[cursor];
        std::string dep_name = normalize_name(req.name);
        auto& entry = graph.packages[dep_name];
        if (entry.name.empty()) entry.name = dep_name;
        entry.requirements.push_back(req);
        entry.constraint = combined_dependency_constraint(dep_name, entry.requirements);

        std::string previous_selection = entry.version + "|" + entry.source;
        bool selected = false;
        bool selection_error = false;

        bool has_file_requirement = false;
        fs::path file_source;
        for (const auto& item : entry.requirements) {
            if (!dependency_spec_is_file(item.spec)) continue;
            fs::path candidate = dependency_file_path(item);
            if (!has_file_requirement) {
                has_file_requirement = true;
                file_source = candidate;
            } else if (candidate.generic_string() != file_source.generic_string()) {
                graph.errors.push_back(dep_name + " has conflicting file sources: " +
                                       file_source.generic_string() + " and " + candidate.generic_string());
                selection_error = true;
            }
        }

        if (!selection_error && has_file_requirement) {
            if (!fs::exists(file_source)) {
                graph.errors.push_back(dep_name + " file dependency not found: " + file_source.generic_string());
                selection_error = true;
            } else {
                std::string pkg_manifest = fs::is_directory(file_source) ? read_all(file_source / kManifest) : "";
                std::string version = manifest_field(pkg_manifest, "version", "0.1.0");
                if (!version_satisfies_constraint(version, entry.constraint)) {
                    graph.errors.push_back(dep_name + "@" + version + " from " + file_source.generic_string() +
                                           " does not satisfy " + entry.constraint);
                    selection_error = true;
                } else {
                    entry.version = version;
                    entry.source = file_source.generic_string() + " (file)";
                    entry.source_path = file_source;
                    entry.installed = false;
                    entry.from_registry = false;
                    entry.from_file = true;
                    selected = true;
                }
            }
        }

        if (!selection_error && !selected) {
            fs::path local = package_dir(dep_name);
            if (fs::exists(local)) {
                std::string installed_manifest = read_all(local / kManifest);
                std::string installed_version = manifest_field(installed_manifest, "version", "0.0.0");
                if (version_satisfies_constraint(installed_version, entry.constraint)) {
                    entry.version = installed_version;
                    entry.source = local.generic_string() + " (installed)";
                    entry.source_path = local;
                    entry.installed = true;
                    entry.from_registry = false;
                    entry.from_file = false;
                    selected = true;
                }
            }
        }

        if (!selection_error && !selected) {
            RegistryPackage selected_pkg;
            std::string select_error;
            if (select_registry_package(dep_name, entry.constraint, selected_pkg, select_error)) {
                entry.version = selected_pkg.version;
                entry.from_registry = true;
                entry.from_file = false;
                entry.installed = false;
                if (registry_url().empty()) {
                    entry.source_path = registry_root() / dep_name / selected_pkg.version;
                    entry.source = entry.source_path.generic_string() + " (registry)";
                } else {
                    PackageRef ref;
                    ref.name = dep_name;
                    ref.version = selected_pkg.version;
                    entry.source_path.clear();
                    entry.source = registry_bundle_url(ref) + " (http registry)";
                }
                selected = true;
            } else {
                graph.errors.push_back(dep_name + " cannot be resolved" +
                    (entry.constraint.empty() ? "" : (" for " + entry.constraint)) +
                    " required by " + dependency_requirement_summary(entry.requirements) +
                    " (" + select_error + ")");
                selection_error = true;
            }
        }

        if (selection_error || !selected) continue;

        std::string selection = entry.version + "|" + entry.source;
        std::string expand_key = dep_name + "|" + selection;
        if (selection != previous_selection || expanded.find(expand_key) == expanded.end()) {
            if (expanded.insert(expand_key).second) {
                std::string selected_manifest = selected_dependency_manifest(entry);
                for (const auto& child : manifest_dependency_specs(selected_manifest)) {
                    fs::path child_base = entry.source_path.empty() ? root : entry.source_path;
                    enqueue(child, package_label(dep_name, entry.version), child_base);
                }
            }
        }
    }

    if (verbose) {
        std::cout << "Dependency resolution\n";
        std::cout << "root: " << root_label << "\n";
        if (graph.packages.empty()) {
            std::cout << "No dependencies\n";
        } else {
            for (const auto& item : graph.packages) {
                const auto& dep = item.second;
                if (dep.version.empty()) {
                    std::cout << dep.name << "  <unresolved>\n";
                } else {
                    std::cout << dep.name << "  " << dep.version << "  " << dep.source << "\n";
                }
                for (const auto& req : dep.requirements) {
                    std::cout << "  required by " << (req.required_by.empty() ? "root" : req.required_by)
                              << ": " << (req.spec.empty() ? "*" : req.spec) << "\n";
                }
            }
        }
        print_dependency_graph_errors(graph);
    }

    return graph;
}

struct RestorePackageReport {
    std::string name;
    std::string version;
    std::string source;
    std::string destination;
    std::string action;
    std::string dependency_spec;
    std::string requirements;
    bool direct = false;
    bool from_registry = false;
    bool from_file = false;
    bool installed_before = false;
    bool installed_after = false;
};

static bool write_restore_json_report(const fs::path& report_path,
                                      const fs::path& root,
                                      bool passed,
                                      int failed,
                                      const std::vector<RestorePackageReport>& packages,
                                      const std::vector<std::string>& errors) {
    std::string manifest = read_all(root / kManifest);
    std::string root_name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string root_version = manifest_field(manifest, "version", "0.1.0");
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.restore.v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"root\": {\"name\":\"" << json_escape(root_name)
        << "\",\"version\":\"" << json_escape(root_version)
        << "\",\"path\":\"" << json_escape(root.generic_string()) << "\"},\n"
        << "  \"package_count\": " << packages.size() << ",\n"
        << "  \"failed_count\": " << failed << ",\n"
        << "  \"packages\": [";
    for (size_t i = 0; i < packages.size(); ++i) {
        const auto& item = packages[i];
        if (i) out << ",";
        out << "\n"
            << "    {\n"
            << "      \"name\": \"" << json_escape(item.name) << "\",\n"
            << "      \"version\": \"" << json_escape(item.version) << "\",\n"
            << "      \"source\": \"" << json_escape(item.source) << "\",\n"
            << "      \"destination\": \"" << json_escape(item.destination) << "\",\n"
            << "      \"action\": \"" << json_escape(item.action) << "\",\n"
            << "      \"direct\": " << (item.direct ? "true" : "false") << ",\n"
            << "      \"dependency_spec\": \"" << json_escape(item.dependency_spec) << "\",\n"
            << "      \"requirements\": \"" << json_escape(item.requirements) << "\",\n"
            << "      \"from_registry\": " << (item.from_registry ? "true" : "false") << ",\n"
            << "      \"from_file\": " << (item.from_file ? "true" : "false") << ",\n"
            << "      \"installed_before\": " << (item.installed_before ? "true" : "false") << ",\n"
            << "      \"installed_after\": " << (item.installed_after ? "true" : "false") << "\n"
            << "    }";
    }
    if (!packages.empty()) out << "\n  ";
    out << "],\n"
        << "  \"errors\": [";
    for (size_t i = 0; i < errors.size(); ++i) {
        if (i) out << ",";
        out << "\n    \"" << json_escape(errors[i]) << "\"";
    }
    if (!errors.empty()) out << "\n  ";
    out << "]\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_restore_command(const fs::path& json_report) {
    std::string manifest = read_all(kManifest);
    if (manifest.empty()) return err("sura.pkg.json not found");
    auto deps = manifest_dependency_specs(manifest);
    std::map<std::string, std::string> direct_specs;
    for (const auto& dep : deps) {
        direct_specs[normalize_name(dep.name)] = dep.spec;
    }

    fs::create_directories(kPackages);
    DependencyGraph graph = resolve_dependency_graph(fs::current_path(), true);
    if (!graph.errors.empty()) {
        std::vector<RestorePackageReport> packages;
        if (!json_report.empty()) {
            if (!write_restore_json_report(json_report, fs::current_path(), false,
                                           (int)graph.errors.size(), packages, graph.errors)) {
                return err("failed to write restore JSON report: " + json_report.generic_string());
            }
            ok("restore report written: " + json_report.generic_string());
        }
        return err("dependency resolution failed");
    }

    int failed = 0;
    std::vector<RestorePackageReport> restored;
    for (const auto& item : graph.packages) {
        const auto& dep = item.second;
        fs::path local = package_dir(dep.name);
        bool installed_ok = false;
        if (fs::exists(local)) {
            std::string installed_manifest = read_all(local / kManifest);
            std::string installed_version = manifest_field(installed_manifest, "version", "0.0.0");
            installed_ok = installed_version == dep.version;
        }
        RestorePackageReport report;
        report.name = dep.name;
        report.version = dep.version;
        report.source = dep.source;
        report.destination = local.generic_string();
        report.requirements = dependency_requirement_summary(dep.requirements);
        report.from_registry = dep.from_registry;
        report.from_file = dep.from_file;
        report.installed_before = installed_ok;
        auto direct = direct_specs.find(dep.name);
        if (direct != direct_specs.end()) {
            report.direct = true;
            report.dependency_spec = direct->second;
        }
        if (installed_ok) {
            report.action = "ok";
            std::cout << "[ok] " << dep.name << "@" << dep.version << "\n";
        } else {
            std::string source = dep.from_registry
                ? (dep.name + "@" + dep.version)
                : dep.source_path.generic_string();
            if (source.empty()) {
                report.action = "missing_source";
                std::cout << "[missing] " << dep.name << " has no install source\n";
                ++failed;
            } else {
                report.source = source;
                std::cout << "[install] " << dep.name << "@" << dep.version << " from " << source << "\n";
                if (install_package(source, false) != 0) {
                    report.action = "failed";
                    ++failed;
                } else {
                    report.action = "installed";
                }
            }
        }
        if (direct != direct_specs.end()) set_dependency(dep.name, direct->second);
        if (fs::exists(local)) {
            std::string installed_manifest = read_all(local / kManifest);
            std::string installed_version = manifest_field(installed_manifest, "version", "0.0.0");
            report.installed_after = installed_version == dep.version;
        }
        restored.push_back(report);
    }
    if (!json_report.empty()) {
        std::vector<std::string> errors;
        if (!write_restore_json_report(json_report, fs::current_path(), failed == 0,
                                       failed, restored, errors)) {
            return err("failed to write restore JSON report: " + json_report.generic_string());
        }
        ok("restore report written: " + json_report.generic_string());
    }
    if (failed) return err("restore failed to install dependencies");
    ok("all manifest and transitive dependencies are present");
    return 0;
}

static int cmd_restore(const std::vector<std::string>& argv) {
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("restore --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("restore --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg restore [--json report.json]\n";
            return 0;
        } else {
            return err("restore accepts only optional --json");
        }
    }
    return run_restore_command(json_report);
}

struct LockPackageReport {
    std::string name;
    std::string version;
    std::string spec;
    std::string source;
    std::string resolved_source;
    std::string hash;
    bool from_registry = false;
    bool from_file = false;
};

static bool write_lock_json_report(const fs::path& report_path,
                                   const fs::path& root,
                                   const fs::path& lockfile,
                                   const std::vector<LockPackageReport>& packages,
                                   bool passed) {
    std::string manifest = read_all(root / kManifest);
    std::string root_name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string root_version = manifest_field(manifest, "version", "0.1.0");
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.lock.v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"root\": {\"name\":\"" << json_escape(root_name)
        << "\",\"version\":\"" << json_escape(root_version)
        << "\",\"path\":\"" << json_escape(root.generic_string()) << "\"},\n"
        << "  \"lockfile\": \"" << json_escape(lockfile.generic_string()) << "\",\n"
        << "  \"package_count\": " << packages.size() << ",\n"
        << "  \"packages\": [";
    for (size_t i = 0; i < packages.size(); ++i) {
        const auto& item = packages[i];
        if (i) out << ",";
        out << "\n"
            << "    {\n"
            << "      \"name\": \"" << json_escape(item.name) << "\",\n"
            << "      \"version\": \"" << json_escape(item.version) << "\",\n"
            << "      \"spec\": \"" << json_escape(item.spec) << "\",\n"
            << "      \"source\": \"" << json_escape(item.source) << "\",\n"
            << "      \"resolved_source\": \"" << json_escape(item.resolved_source) << "\",\n"
            << "      \"hash\": \"" << json_escape(item.hash) << "\",\n"
            << "      \"from_registry\": " << (item.from_registry ? "true" : "false") << ",\n"
            << "      \"from_file\": " << (item.from_file ? "true" : "false") << "\n"
            << "    }";
    }
    if (!packages.empty()) out << "\n  ";
    out << "]\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_lock_command(const fs::path& json_report) {
    std::string manifest = read_all(kManifest);
    if (manifest.empty()) return err("sura.pkg.json not found");
    fs::path project_root = fs::current_path();
    DependencyGraph graph = resolve_dependency_graph(fs::current_path(), false);
    if (!graph.errors.empty()) {
        print_dependency_graph_errors(graph);
        return err("dependency resolution failed");
    }
    std::ostringstream out;
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"packages\": {\n";
    size_t i = 0;
    std::vector<LockPackageReport> locked;
    for (const auto& item : graph.packages) {
        const auto& resolved = item.second;
        const std::string dep = normalize_name(resolved.name);
        fs::path root = package_dir(dep);
        if (!fs::exists(root)) return err("dependency is not installed: " + dep);
        std::string pkg_manifest = read_all(root / kManifest);
        std::string version = manifest_field(pkg_manifest, "version", "0.0.0");
        if (version != resolved.version) {
            return err("installed " + dep + "@" + version + " does not match resolved " + resolved.version);
        }
        std::string spec_summary = dependency_requirement_summary(resolved.requirements);
        std::string hash = package_hash(root);
        out << "    \"" << json_escape(dep) << "\": {\n"
            << "      \"version\": \"" << json_escape(version) << "\",\n"
            << "      \"spec\": \"" << json_escape(spec_summary) << "\",\n"
            << "      \"source\": \"" << json_escape(root.generic_string()) << "\",\n"
            << "      \"hash\": \"" << hash << "\"\n"
            << "    }" << (++i == graph.packages.size() ? "\n" : ",\n");
        locked.push_back({dep, version, spec_summary, root.generic_string(), resolved.source,
                          hash, resolved.from_registry, resolved.from_file});
    }
    out << "  }\n}\n";
    if (!write_all(kLockfile, out.str())) return err("failed to write sura.lock.json");
    if (!json_report.empty()) {
        if (!write_lock_json_report(json_report, project_root, kLockfile, locked, true)) {
            return err("failed to write lock JSON report: " + json_report.generic_string());
        }
        ok("lock report written: " + json_report.generic_string());
    }
    ok("wrote sura.lock.json");
    return 0;
}

static int cmd_lock(const std::vector<std::string>& argv) {
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("lock --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("lock --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg lock [--json report.json]\n";
            return 0;
        } else {
            return err("lock accepts only optional --json");
        }
    }
    return run_lock_command(json_report);
}

static void print_dependency_tree_node(const DependencySpec& spec,
                                       const DependencyGraph& graph,
                                       const std::string& indent,
                                       std::set<std::string>& active) {
    std::string name = normalize_name(spec.name);
    auto it = graph.packages.find(name);
    if (it == graph.packages.end() || it->second.version.empty()) {
        std::cout << indent << "- " << name << "  <unresolved>  spec=" << (spec.spec.empty() ? "*" : spec.spec) << "\n";
        return;
    }

    const auto& dep = it->second;
    std::string label = package_label(dep.name, dep.version);
    std::cout << indent << "- " << label << "  spec=" << (spec.spec.empty() ? "*" : spec.spec);
    if (!dep.source.empty()) std::cout << "  source=" << dep.source;
    std::cout << "\n";

    if (active.count(label)) {
        std::cout << indent << "  - <cycle: " << label << ">\n";
        return;
    }

    active.insert(label);
    for (const auto& child : manifest_dependency_specs(selected_dependency_manifest(dep))) {
        print_dependency_tree_node(child, graph, indent + "  ", active);
    }
    active.erase(label);
}

static void write_dependency_tree_json_node(std::ostringstream& out,
                                            const DependencySpec& spec,
                                            const DependencyGraph& graph,
                                            const std::string& indent,
                                            std::set<std::string>& active) {
    std::string name = normalize_name(spec.name);
    auto it = graph.packages.find(name);
    bool resolved = it != graph.packages.end() && !it->second.version.empty();
    out << indent << "{\n";
    out << indent << "  \"name\": \"" << json_escape(name) << "\",\n";
    out << indent << "  \"spec\": \"" << json_escape(spec.spec.empty() ? "*" : spec.spec) << "\",\n";
    out << indent << "  \"resolved\": " << (resolved ? "true" : "false");
    if (!resolved) {
        out << ",\n" << indent << "  \"dependencies\": []\n";
        out << indent << "}";
        return;
    }

    const auto& dep = it->second;
    std::string label = package_label(dep.name, dep.version);
    bool cycle = active.count(label) > 0;
    out << ",\n";
    out << indent << "  \"version\": \"" << json_escape(dep.version) << "\",\n";
    out << indent << "  \"source\": \"" << json_escape(dep.source) << "\",\n";
    out << indent << "  \"cycle\": " << (cycle ? "true" : "false") << ",\n";
    out << indent << "  \"dependencies\": [\n";
    if (!cycle) {
        active.insert(label);
        auto children = manifest_dependency_specs(selected_dependency_manifest(dep));
        for (size_t i = 0; i < children.size(); ++i) {
            write_dependency_tree_json_node(out, children[i], graph, indent + "    ", active);
            if (i + 1 < children.size()) out << ",";
            out << "\n";
        }
        active.erase(label);
    }
    out << indent << "  ]\n";
    out << indent << "}";
}

static int cmd_tree(bool json_output) {
    fs::path root = fs::current_path();
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return err("sura.pkg.json not found");

    DependencyGraph graph = resolve_dependency_graph(root, false);
    if (!graph.errors.empty()) {
        print_dependency_graph_errors(graph);
        return err("dependency resolution failed");
    }

    std::string root_name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string root_version = manifest_field(manifest, "version", "0.1.0");
    auto deps = manifest_dependency_specs(manifest);

    if (json_output) {
        std::ostringstream out;
        out << "{\n";
        out << "  \"schema\": \"sura.package.tree.v1\",\n";
        out << "  \"version\": 1,\n";
        out << "  \"passed\": true,\n";
        out << "  \"root\": {\"name\":\"" << json_escape(root_name)
            << "\",\"version\":\"" << json_escape(root_version) << "\"},\n";
        out << "  \"dependencies\": [\n";
        std::set<std::string> active;
        for (size_t i = 0; i < deps.size(); ++i) {
            write_dependency_tree_json_node(out, deps[i], graph, "    ", active);
            if (i + 1 < deps.size()) out << ",";
            out << "\n";
        }
        out << "  ]\n";
        out << "}\n";
        std::cout << out.str();
        return 0;
    }

    std::cout << "Dependency tree\n";
    std::cout << root_name << "@" << root_version << "\n";
    if (deps.empty()) {
        std::cout << "  <no dependencies>\n";
        return 0;
    }
    std::set<std::string> active;
    for (const auto& dep : deps) print_dependency_tree_node(dep, graph, "  ", active);
    return 0;
}

struct WhyNode {
    std::string name;
    std::string version;
    std::string spec;
    std::string source;
};

static void collect_dependency_why_paths(const DependencySpec& spec,
                                         const DependencyGraph& graph,
                                         const std::string& target,
                                         std::vector<WhyNode>& current,
                                         std::vector<std::vector<WhyNode>>& paths,
                                         std::set<std::string>& active) {
    std::string name = normalize_name(spec.name);
    auto it = graph.packages.find(name);
    if (it == graph.packages.end() || it->second.version.empty()) return;

    const auto& dep = it->second;
    std::string label = package_label(dep.name, dep.version);
    WhyNode node{dep.name, dep.version, spec.spec.empty() ? "*" : spec.spec, dep.source};
    current.push_back(node);

    if (dep.name == target) {
        paths.push_back(current);
        current.pop_back();
        return;
    }

    if (!active.count(label)) {
        active.insert(label);
        for (const auto& child : manifest_dependency_specs(selected_dependency_manifest(dep))) {
            collect_dependency_why_paths(child, graph, target, current, paths, active);
        }
        active.erase(label);
    }

    current.pop_back();
}

static void write_dependency_why_json_node(std::ostringstream& out,
                                           const WhyNode& node,
                                           const std::string& indent,
                                           bool is_root) {
    out << indent << "{";
    out << "\"name\":\"" << json_escape(node.name) << "\",";
    out << "\"version\":\"" << json_escape(node.version) << "\"";
    if (!is_root) {
        out << ",\"spec\":\"" << json_escape(node.spec) << "\"";
        out << ",\"source\":\"" << json_escape(node.source) << "\"";
    }
    out << "}";
}

static int cmd_why(const std::string& package_name, bool json_output) {
    std::string target = normalize_name(package_name);
    if (target.empty()) return err("why requires a package name");

    fs::path root = fs::current_path();
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return err("sura.pkg.json not found");

    DependencyGraph graph = resolve_dependency_graph(root, false);
    if (!graph.errors.empty()) {
        print_dependency_graph_errors(graph);
        return err("dependency resolution failed");
    }

    std::string root_name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string root_version = manifest_field(manifest, "version", "0.1.0");
    WhyNode root_node{root_name, root_version, "", root.generic_string()};
    std::vector<std::vector<WhyNode>> paths;
    auto deps = manifest_dependency_specs(manifest);
    for (const auto& dep : deps) {
        std::vector<WhyNode> current;
        current.push_back(root_node);
        std::set<std::string> active;
        collect_dependency_why_paths(dep, graph, target, current, paths, active);
    }

    auto resolved_it = graph.packages.find(target);
    bool found = resolved_it != graph.packages.end() && !resolved_it->second.version.empty() && !paths.empty();
    if (json_output) {
        std::ostringstream out;
        out << "{\n";
        out << "  \"schema\": \"sura.package.why.v1\",\n";
        out << "  \"version\": 1,\n";
        out << "  \"passed\": " << (found ? "true" : "false") << ",\n";
        out << "  \"root\": {\"name\":\"" << json_escape(root_name)
            << "\",\"version\":\"" << json_escape(root_version) << "\"},\n";
        out << "  \"package\": {\"name\":\"" << json_escape(target) << "\",\"found\": "
            << (found ? "true" : "false");
        if (found) out << ",\"version\":\"" << json_escape(resolved_it->second.version) << "\"";
        out << "},\n";
        out << "  \"paths\": [\n";
        for (size_t i = 0; i < paths.size(); ++i) {
            out << "    [\n";
            for (size_t j = 0; j < paths[i].size(); ++j) {
                write_dependency_why_json_node(out, paths[i][j], "      ", j == 0);
                if (j + 1 < paths[i].size()) out << ",";
                out << "\n";
            }
            out << "    ]";
            if (i + 1 < paths.size()) out << ",";
            out << "\n";
        }
        out << "  ]\n";
        out << "}\n";
        std::cout << out.str();
        return found ? 0 : 1;
    }

    std::cout << "Dependency reason\n";
    std::cout << "root: " << root_name << "@" << root_version << "\n";
    if (!found) {
        std::cout << target << " is not in the resolved dependency graph\n";
        return 1;
    }
    std::cout << "package: " << target << "@" << resolved_it->second.version << "\n";
    for (size_t i = 0; i < paths.size(); ++i) {
        std::cout << "Path " << (i + 1) << "\n";
        for (size_t j = 0; j < paths[i].size(); ++j) {
            const auto& node = paths[i][j];
            std::cout << "  " << (j == 0 ? "" : "-> ")
                      << node.name << "@" << node.version;
            if (j > 0) std::cout << "  spec=" << node.spec;
            if (j > 0 && !node.source.empty()) std::cout << "  source=" << node.source;
            std::cout << "\n";
        }
    }
    return 0;
}

struct LockedPackage {
    std::string name;
    std::string version;
    fs::path source;
    std::string hash;
};

struct VerifyPackageReport {
    std::string name;
    std::string version;
    std::string source;
    std::string expected_hash;
    std::string actual_hash;
    std::string status;
};

static bool write_verify_json_report(const fs::path& report_path,
                                     const std::string& mode,
                                     const std::string& target,
                                     bool passed,
                                     const std::vector<VerifyPackageReport>& packages,
                                     const std::vector<std::string>& errors) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.verify.v1\",\n"
        << "  \"mode\": \"" << json_escape(mode) << "\",\n"
        << "  \"target\": \"" << json_escape(target) << "\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"package_count\": " << packages.size() << ",\n"
        << "  \"packages\": [";
    for (size_t i = 0; i < packages.size(); ++i) {
        const auto& item = packages[i];
        if (i) out << ",";
        out << "\n"
            << "    {\n"
            << "      \"name\": \"" << json_escape(item.name) << "\",\n"
            << "      \"version\": \"" << json_escape(item.version) << "\",\n"
            << "      \"source\": \"" << json_escape(item.source) << "\",\n"
            << "      \"expected_hash\": \"" << json_escape(item.expected_hash) << "\",\n"
            << "      \"actual_hash\": \"" << json_escape(item.actual_hash) << "\",\n"
            << "      \"status\": \"" << json_escape(item.status) << "\"\n"
            << "    }";
    }
    if (!packages.empty()) out << "\n  ";
    out << "],\n"
        << "  \"errors\": [";
    for (size_t i = 0; i < errors.size(); ++i) {
        if (i) out << ",";
        out << "\n    \"" << json_escape(errors[i]) << "\"";
    }
    if (!errors.empty()) out << "\n  ";
    out << "]\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static std::vector<LockedPackage> parse_lock_packages(const std::string& lockfile) {
    std::vector<LockedPackage> out;
    std::regex pkg_re(
        "(?:^|\\n)\\s{4}\"((?:\\\\.|[^\"])*)\"\\s*:\\s*\\{[^}]*"
        "\"version\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^}]*"
        "\"source\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^}]*"
        "\"hash\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^}]*\\}");
    for (auto it = std::sregex_iterator(lockfile.begin(), lockfile.end(), pkg_re);
         it != std::sregex_iterator(); ++it) {
        LockedPackage pkg;
        pkg.name = normalize_name(json_unescape((*it)[1].str()));
        pkg.version = json_unescape((*it)[2].str());
        pkg.source = fs::path(json_unescape((*it)[3].str()));
        pkg.hash = json_unescape((*it)[4].str());
        out.push_back(pkg);
    }
    return out;
}

static int verify_signature_path(const fs::path& root) {
    if (!fs::is_directory(root)) return err("verify requires a package directory");
    std::string sig = read_all(root / kSignature);
    if (sig.empty()) return err("signature file not found: " + (root / kSignature).generic_string());
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return err("sura.pkg.json not found: " + (root / kManifest).generic_string());

    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "0.1.0");
    std::string signed_name = normalize_name(manifest_field(sig, "name", ""));
    std::string signed_version = manifest_field(sig, "packageVersion", "");
    std::string algorithm = manifest_field(sig, "algorithm", "sha256-canonical-v1");
    std::string signed_hash = manifest_field(sig, "hash", "");
    std::string signed_sig = manifest_field(sig, "signature", "");
    std::string key_id = manifest_field(sig, "keyId", "");
    std::string actual_hash = package_hash(root);

    if (signed_name != name) return err("signature package name mismatch: " + signed_name + " != " + name);
    if (signed_version != version) return err("signature package version mismatch: " + signed_version + " != " + version);
    if (signed_hash != actual_hash) return err("package hash mismatch for " + name + "@" + version);

    if (algorithm == "rsa-sha256-v2") {
        std::vector<fs::path> temp_keys;
        std::string key_source;
        std::string public_key = resolve_public_key_path(key_id, {}, "", temp_keys, key_source);
        if (public_key.empty()) {
            if (require_public_signature()) {
                return err("public key required to verify public-key signature for " + name + "@" + version +
                           "; set SURA_SIGNING_PUBLIC_KEY or SURA_SIGNING_PUBLIC_KEY_DIR");
            }
            info("integrity hash matches; set SURA_SIGNING_PUBLIC_KEY or SURA_SIGNING_PUBLIC_KEY_DIR to verify public-key signature for keyId=" + key_id);
        } else {
            std::string detail;
            if (!openssl_verify_payload(package_public_signature_payload(name, version, actual_hash),
                                        public_key, signed_sig, detail)) {
                cleanup_temp_paths(temp_keys);
                std::string msg = "public-key signature mismatch for " + name + "@" + version;
                if (!detail.empty()) msg += ": " + detail;
                return err(msg);
            }
            cleanup_temp_paths(temp_keys);
            info("public-key signature verified for keyId=" + key_id +
                 (key_source.empty() ? "" : (" via " + key_source)));
        }
    } else if (algorithm == "sha256-canonical-v1") {
        if (require_public_signature()) {
            return err("public-key signature required for " + name + "@" + version);
        }
        std::string key = signing_key();
        if (!key.empty()) {
            std::string expected = package_signature(name, version, actual_hash);
            if (signed_sig != expected) return err("keyed signature mismatch for " + name + "@" + version);
        } else if (signed_sig != signed_hash) {
            info("integrity hash matches; set SURA_SIGNING_KEY to verify keyed signature for keyId=" + key_id);
        }
    } else {
        return err("unsupported package signature algorithm: " + algorithm);
    }

    ok("verified " + name + "@" + version + " (" + actual_hash + ")");
    return 0;
}

static bool write_package_sign_json_report(const fs::path& report_path,
                                           const fs::path& root,
                                           bool passed) {
    std::string manifest = read_all(root / kManifest);
    std::string sig = read_all(root / kSignature);
    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "0.1.0");
    std::string algorithm = manifest_field(sig, "algorithm", "");
    std::string key_id = manifest_field(sig, "keyId", "");
    std::string hash = manifest_field(sig, "hash", "");
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.sign.v1\",\n"
        << "  \"package\": \"" << json_escape(name) << "\",\n"
        << "  \"version\": \"" << json_escape(version) << "\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"path\": \"" << json_escape(root.generic_string()) << "\",\n"
        << "  \"signature\": \"" << json_escape((root / kSignature).generic_string()) << "\",\n"
        << "  \"algorithm\": \"" << json_escape(algorithm) << "\",\n"
        << "  \"key_id\": \"" << json_escape(key_id) << "\",\n"
        << "  \"hash\": \"" << json_escape(hash) << "\"\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_sign_command(const std::string& source, const fs::path& json_report) {
    fs::path root = source.empty() ? fs::current_path() : fs::path(source);
    if (!write_package_signature(root)) return err("failed to write package signature");
    std::string manifest = read_all(root / kManifest);
    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "0.1.0");
    if (!json_report.empty()) {
        if (!write_package_sign_json_report(json_report, root, true)) {
            return err("failed to write sign JSON report: " + json_report.generic_string());
        }
        ok("sign report written: " + json_report.generic_string());
    }
    ok("signed " + name + "@" + version + " -> " + (root / kSignature).generic_string());
    return 0;
}

static int cmd_sign(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("sign --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("sign --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage:\n  surapkg sign [package-path] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("sign accepts at most one path and optional --json");
        }
    }
    return run_sign_command(source, json_report);
}

static bool tool_policy_signature_status(const fs::path& manifest_path, std::string& message) {
    if (!fs::is_regular_file(manifest_path)) {
        message = "tool policy manifest not found: " + manifest_path.generic_string();
        return false;
    }
    fs::path sig_path = manifest_path.parent_path() / kToolPolicySignature;
    std::string sig = read_all(sig_path);
    if (sig.empty()) {
        message = "tool policy signature file not found: " + sig_path.generic_string();
        return false;
    }
    std::string source = manifest_field(sig, "source", "");
    std::string algorithm = manifest_field(sig, "algorithm", "sha256-tool-policy-v1");
    std::string signed_hash = manifest_field(sig, "hash", "");
    std::string signed_sig = manifest_field(sig, "signature", "");
    std::string key_id = manifest_field(sig, "keyId", "");
    if (source != kToolPolicyManifest.generic_string()) {
        message = "tool policy signature source mismatch: " + source;
        return false;
    }
    std::string actual_hash = file_content_hash(manifest_path, "sura-tool-policy-sha256-v1");
    if (signed_hash != actual_hash) {
        message = "tool policy hash mismatch for " + manifest_path.generic_string();
        return false;
    }
    if (algorithm == "rsa-sha256-tool-policy-v2") {
        std::vector<fs::path> temp_keys;
        std::string key_source;
        std::string public_key = resolve_public_key_path(key_id, {}, "", temp_keys, key_source);
        if (public_key.empty()) {
            if (require_public_signature()) {
                message = "public key required to verify public-key tool policy signature; set SURA_SIGNING_PUBLIC_KEY or SURA_SIGNING_PUBLIC_KEY_DIR";
                return false;
            }
            message = "tool policy hash matches; public-key signature not verified without SURA_SIGNING_PUBLIC_KEY or SURA_SIGNING_PUBLIC_KEY_DIR";
            return true;
        }
        std::string detail;
        if (!openssl_verify_payload(tool_policy_public_signature_payload(actual_hash),
                                    public_key, signed_sig, detail)) {
            cleanup_temp_paths(temp_keys);
            message = "tool policy public-key signature mismatch for keyId=" + key_id;
            if (!detail.empty()) message += ": " + detail;
            return false;
        }
        cleanup_temp_paths(temp_keys);
        message = "tool policy public-key signature verified";
        if (!key_source.empty()) message += " via " + key_source;
        return true;
    }
    if (algorithm != "sha256-tool-policy-v1") {
        message = "unsupported tool policy signature algorithm: " + algorithm;
        return false;
    }
    if (require_public_signature()) {
        message = "public-key tool policy signature required";
        return false;
    }
    std::string key = signing_key();
    if (!key.empty()) {
        std::string expected = tool_policy_signature(actual_hash);
        if (signed_sig != expected) {
            message = "tool policy keyed signature mismatch for keyId=" + key_id;
            return false;
        }
    } else if (signed_sig != signed_hash) {
        message = "tool policy hash matches; keyed signature not verified without SURA_SIGNING_KEY";
        return true;
    }
    message = "tool policy signature verified";
    return true;
}

static bool write_sign_policy_json_report(const fs::path& report_path,
                                          const fs::path& manifest_path,
                                          bool passed) {
    fs::path sig_path = manifest_path.parent_path() / kToolPolicySignature;
    std::string sig = read_all(sig_path);
    std::string algorithm = manifest_field(sig, "algorithm", "");
    std::string source = manifest_field(sig, "source", "");
    std::string key_id = manifest_field(sig, "keyId", "");
    std::string hash = manifest_field(sig, "hash", "");
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.sign_policy.v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"manifest\": \"" << json_escape(manifest_path.generic_string()) << "\",\n"
        << "  \"signature\": \"" << json_escape(sig_path.generic_string()) << "\",\n"
        << "  \"source\": \"" << json_escape(source) << "\",\n"
        << "  \"algorithm\": \"" << json_escape(algorithm) << "\",\n"
        << "  \"key_id\": \"" << json_escape(key_id) << "\",\n"
        << "  \"hash\": \"" << json_escape(hash) << "\"\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_sign_policy_command(const std::string& source, const fs::path& json_report) {
    fs::path manifest_path = tool_policy_manifest_path(source);
    if (!write_tool_policy_signature(manifest_path)) {
        return err("failed to write tool policy signature for " + manifest_path.generic_string());
    }
    if (!json_report.empty()) {
        if (!write_sign_policy_json_report(json_report, manifest_path, true)) {
            return err("failed to write sign-policy JSON report: " + json_report.generic_string());
        }
        ok("sign-policy report written: " + json_report.generic_string());
    }
    ok("signed tool policy -> " + (manifest_path.parent_path() / kToolPolicySignature).generic_string());
    return 0;
}

static int cmd_sign_policy(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("sign-policy --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("sign-policy --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage:\n  surapkg sign-policy [package-path|sura.tools.json] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("sign-policy accepts at most one path and optional --json");
        }
    }
    return run_sign_policy_command(source, json_report);
}

static int cmd_verify_policy(const std::string& source) {
    if (source == "--help" || source == "-h") {
        std::cout << "Usage:\n  surapkg verify-policy [package-path|sura.tools.json|sura.tools.sig] [--json report.json]\n";
        return 0;
    }
    fs::path manifest_path = tool_policy_manifest_path(source);
    std::string message;
    if (!tool_policy_signature_status(manifest_path, message)) return err(message);
    ok(message + ": " + manifest_path.generic_string());
    return 0;
}

static bool write_verify_policy_json_report(const fs::path& report_path,
                                            const fs::path& manifest_path,
                                            bool passed,
                                            const std::string& message) {
    fs::path sig_path = manifest_path.parent_path() / kToolPolicySignature;
    std::string sig = read_all(sig_path);
    std::string algorithm = manifest_field(sig, "algorithm", "");
    std::string source = manifest_field(sig, "source", "");
    std::string key_id = manifest_field(sig, "keyId", "");
    std::string expected_hash = manifest_field(sig, "hash", "");
    std::string actual_hash;
    if (fs::is_regular_file(manifest_path)) {
        actual_hash = file_content_hash(manifest_path, "sura-tool-policy-sha256-v1");
    }

    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.verify_policy.v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"manifest\": \"" << json_escape(manifest_path.generic_string()) << "\",\n"
        << "  \"signature\": \"" << json_escape(sig_path.generic_string()) << "\",\n"
        << "  \"source\": \"" << json_escape(source) << "\",\n"
        << "  \"algorithm\": \"" << json_escape(algorithm) << "\",\n"
        << "  \"key_id\": \"" << json_escape(key_id) << "\",\n"
        << "  \"expected_hash\": \"" << json_escape(expected_hash) << "\",\n"
        << "  \"actual_hash\": \"" << json_escape(actual_hash) << "\",\n"
        << "  \"message\": \"" << json_escape(message) << "\"\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_verify_policy_command(const std::string& source, const fs::path& json_report) {
    fs::path manifest_path = tool_policy_manifest_path(source);
    std::string message;
    bool passed = tool_policy_signature_status(manifest_path, message);
    if (!json_report.empty()) {
        if (!write_verify_policy_json_report(json_report, manifest_path, passed, message)) {
            return err("failed to write verify-policy JSON report: " + json_report.generic_string());
        }
        ok("verify-policy report written: " + json_report.generic_string());
    }
    if (!passed) return err(message);
    ok(message + ": " + manifest_path.generic_string());
    return 0;
}

static int cmd_verify_policy(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("verify-policy --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("verify-policy --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage:\n  surapkg verify-policy [package-path|sura.tools.json|sura.tools.sig] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("verify-policy accepts at most one path and optional --json");
        }
    }
    return run_verify_policy_command(source, json_report);
}

static int run_verify_signature_command(const fs::path& root, const fs::path& json_report) {
    int code = verify_signature_path(root);
    if (!json_report.empty()) {
        std::vector<VerifyPackageReport> packages;
        std::vector<std::string> errors;
        VerifyPackageReport item;
        item.source = root.generic_string();
        item.status = code == 0 ? "ok" : "failed";
        if (fs::is_directory(root)) {
            std::string manifest = read_all(root / kManifest);
            item.name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
            item.version = manifest_field(manifest, "version", "0.1.0");
            std::string sig = read_all(root / kSignature);
            item.expected_hash = manifest_field(sig, "hash", "");
            if (!manifest.empty()) item.actual_hash = package_hash(root);
        }
        if (code != 0) errors.push_back("package signature verification failed");
        packages.push_back(item);
        if (!write_verify_json_report(json_report, "signature", root.generic_string(),
                                      code == 0, packages, errors)) {
            return err("failed to write verify JSON report: " + json_report.generic_string());
        }
        ok("verify report written: " + json_report.generic_string());
    }
    return code;
}

static int run_verify_command(const std::string& source, const fs::path& json_report) {
    if (!source.empty()) return run_verify_signature_command(fs::path(source), json_report);
    if (fs::exists(kLockfile)) {
        std::string lockfile = read_all(kLockfile);
        auto packages = parse_lock_packages(lockfile);
        if (packages.empty()) {
            std::vector<VerifyPackageReport> report_packages;
            std::vector<std::string> errors = {"no packages found in sura.lock.json"};
            if (!json_report.empty()) {
                if (!write_verify_json_report(json_report, "lockfile", kLockfile.generic_string(),
                                              false, report_packages, errors)) {
                    return err("failed to write verify JSON report: " + json_report.generic_string());
                }
                ok("verify report written: " + json_report.generic_string());
            }
            return err("no packages found in sura.lock.json");
        }
        int changed = 0;
        std::vector<VerifyPackageReport> report_packages;
        for (const auto& pkg : packages) {
            VerifyPackageReport report;
            report.name = pkg.name;
            report.version = pkg.version;
            report.source = pkg.source.generic_string();
            report.expected_hash = pkg.hash;
            if (!fs::exists(pkg.source)) {
                std::cout << "[missing] " << pkg.name << " -> " << pkg.source.generic_string() << "\n";
                report.status = "missing";
                ++changed;
                report_packages.push_back(report);
                continue;
            }
            std::string actual = package_hash(pkg.source);
            report.actual_hash = actual;
            if (actual != pkg.hash) {
                std::cout << "[changed] " << pkg.name << "@" << pkg.version
                          << " expected " << pkg.hash << " got " << actual << "\n";
                report.status = "changed";
                ++changed;
            } else {
                std::cout << "[ok] " << pkg.name << "@" << pkg.version << " " << actual << "\n";
                report.status = "ok";
            }
            report_packages.push_back(report);
        }
        if (!json_report.empty()) {
            std::vector<std::string> errors;
            if (changed) errors.push_back("lockfile verification failed");
            if (!write_verify_json_report(json_report, "lockfile", kLockfile.generic_string(),
                                          changed == 0, report_packages, errors)) {
                return err("failed to write verify JSON report: " + json_report.generic_string());
            }
            ok("verify report written: " + json_report.generic_string());
        }
        return changed ? err("lockfile verification failed") : 0;
    }
    return run_verify_signature_command(fs::current_path(), json_report);
}

static int cmd_verify(const std::string& source) {
    return run_verify_command(source, fs::path());
}

static int cmd_verify(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("verify --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("verify --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg verify [path] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("verify accepts at most one path and optional --json");
        }
    }
    return run_verify_command(source, json_report);
}

static bool package_signature_status(const fs::path& root,
                                     std::string& message,
                                     const fs::path& registry_key_dir = {},
                                     const std::string& remote_key_base = "") {
    std::string sig = read_all(root / kSignature);
    if (sig.empty()) {
        message = "signature file not found: " + (root / kSignature).generic_string();
        return false;
    }
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) {
        message = "sura.pkg.json not found: " + (root / kManifest).generic_string();
        return false;
    }

    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "0.1.0");
    std::string signed_name = normalize_name(manifest_field(sig, "name", ""));
    std::string signed_version = manifest_field(sig, "packageVersion", "");
    std::string algorithm = manifest_field(sig, "algorithm", "sha256-canonical-v1");
    std::string signed_hash = manifest_field(sig, "hash", "");
    std::string signed_sig = manifest_field(sig, "signature", "");
    std::string key_id = manifest_field(sig, "keyId", "");
    std::string actual_hash = package_hash(root);

    if (signed_name != name) {
        message = "signature package name mismatch: " + signed_name + " != " + name;
        return false;
    }
    if (signed_version != version) {
        message = "signature package version mismatch: " + signed_version + " != " + version;
        return false;
    }
    if (signed_hash != actual_hash) {
        message = "package hash mismatch for " + name + "@" + version;
        return false;
    }
    if (algorithm == "rsa-sha256-v2") {
        std::vector<fs::path> temp_keys;
        std::string key_source;
        std::string public_key = resolve_public_key_path(key_id, registry_key_dir, remote_key_base,
                                                         temp_keys, key_source);
        if (public_key.empty()) {
            if (require_public_signature()) {
                message = "public key required to verify public-key signature for " + name + "@" + version +
                          "; set SURA_SIGNING_PUBLIC_KEY, SURA_SIGNING_PUBLIC_KEY_DIR, or registry keys/" +
                          public_key_filename(key_id);
                return false;
            }
            message = "integrity hash matches; public-key signature not verified without SURA_SIGNING_PUBLIC_KEY, SURA_SIGNING_PUBLIC_KEY_DIR, or registry key";
            return true;
        }
        std::string detail;
        if (!openssl_verify_payload(package_public_signature_payload(name, version, actual_hash),
                                    public_key, signed_sig, detail)) {
            cleanup_temp_paths(temp_keys);
            message = "public-key signature mismatch for " + name + "@" + version + " keyId=" + key_id;
            if (!detail.empty()) message += ": " + detail;
            return false;
        }
        cleanup_temp_paths(temp_keys);
        message = "public-key signature verified";
        if (!key_source.empty()) message += " via " + key_source;
        return true;
    }
    if (algorithm != "sha256-canonical-v1") {
        message = "unsupported package signature algorithm: " + algorithm;
        return false;
    }
    if (require_public_signature()) {
        message = "public-key signature required for " + name + "@" + version;
        return false;
    }
    std::string key = signing_key();
    if (!key.empty()) {
        std::string expected = package_signature(name, version, actual_hash);
        if (signed_sig != expected) {
            message = "keyed signature mismatch for " + name + "@" + version;
            return false;
        }
    } else if (signed_sig != signed_hash) {
        message = "integrity hash matches; keyed signature not verified without SURA_SIGNING_KEY";
        return true;
    }
    message = "signature verified";
    return true;
}

static std::string package_key(const std::string& name, const std::string& version) {
    return normalize_name(name) + "@" + version;
}

static bool package_set_contains(const std::set<std::string>& packages, const std::string& name, const std::string& version) {
    if (version.empty()) {
        std::string prefix = normalize_name(name) + "@";
        for (const auto& item : packages) {
            if (item.rfind(prefix, 0) == 0) return true;
        }
        return false;
    }
    return packages.find(package_key(name, version)) != packages.end();
}

static std::vector<std::string> metadata_object_keys(const std::string& json) {
    std::vector<std::string> out;
    std::regex key_re("\"((?:\\\\.|[^\"])*)\"\\s*:\\s*\\{");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), key_re);
         it != std::sregex_iterator(); ++it) {
        out.push_back(json_unescape((*it)[1].str()));
    }
    return out;
}

struct RegistryVerifyFinding {
    std::string severity;
    std::string message;
};

static bool write_registry_verify_json_report(const fs::path& report_path,
                                              bool remote,
                                              const std::string& url,
                                              const fs::path& root,
                                              const std::string& index_ref,
                                              size_t package_count,
                                              int warnings,
                                              int errors,
                                              const std::vector<RegistryVerifyFinding>& findings) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"sura.registry.verify.v1\",\n";
    out << "  \"source\": \"" << (remote ? "remote" : "local") << "\",\n";
    out << "  \"root\": \"" << json_escape(remote ? "" : root.generic_string()) << "\",\n";
    out << "  \"url\": \"" << json_escape(remote ? url : "") << "\",\n";
    out << "  \"index\": \"" << json_escape(index_ref) << "\",\n";
    out << "  \"passed\": " << (errors == 0 ? "true" : "false") << ",\n";
    out << "  \"package_count\": " << package_count << ",\n";
    out << "  \"warning_count\": " << warnings << ",\n";
    out << "  \"error_count\": " << errors << ",\n";
    out << "  \"findings\": [\n";
    for (size_t i = 0; i < findings.size(); ++i) {
        const auto& finding = findings[i];
        out << "    {\"severity\":\"" << json_escape(finding.severity)
            << "\",\"message\":\"" << json_escape(finding.message) << "\"}";
        if (i + 1 < findings.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return write_all(report_path, out.str());
}

static int finish_verify_registry_command(const fs::path& json_report,
                                          bool remote,
                                          const std::string& url,
                                          const fs::path& root,
                                          const std::string& index_ref,
                                          size_t package_count,
                                          int warnings,
                                          int errors,
                                          const std::vector<RegistryVerifyFinding>& findings,
                                          const std::string& failure_message) {
    if (!json_report.empty()) {
        if (!write_registry_verify_json_report(json_report, remote, url, root, index_ref,
                                               package_count, warnings, errors, findings)) {
            return err("failed to write verify-registry JSON report: " + json_report.generic_string());
        }
        ok("verify-registry report written: " + json_report.generic_string());
    }
    if (errors) return err(failure_message.empty() ? "registry verification failed" : failure_message);
    return 0;
}

static int run_verify_registry_command(const std::string& source, const fs::path& json_report) {
    std::string url = registry_url();
    bool remote = !url.empty() && source.empty();
    fs::path root = source.empty() ? registry_root() : fs::path(source);
    fs::path index_path = root / "index.json";
    std::string index_ref = remote ? (url + "/index.json") : index_path.generic_string();
    std::string index = remote ? http_get_text(url + "/index.json") : read_all(index_path);
    std::vector<RegistryVerifyFinding> findings;
    if (index.empty()) {
        std::string msg = remote
            ? "remote registry index not reachable: " + url + "/index.json"
            : "registry index not found: " + index_path.generic_string();
        findings.push_back({"error", msg});
        return finish_verify_registry_command(json_report, remote, url, root, index_ref,
                                              0, 0, 1, findings, msg);
    }

    auto packages = parse_registry_packages(index);
    int warnings = 0;
    int errors = 0;
    std::set<std::string> seen;

    auto warn = [&](const std::string& msg) {
        ++warnings;
        findings.push_back({"warning", msg});
        std::cout << "[warn] " << msg << "\n";
    };
    auto fail = [&](const std::string& msg) {
        ++errors;
        findings.push_back({"error", msg});
        std::cout << "[error] " << msg << "\n";
    };

    std::cout << "Sura registry verification\n";
    if (remote) {
        std::cout << "url: " << url << "\n";
        std::cout << "index: " << url << "/index.json\n";
    } else {
        std::cout << "root: " << root.generic_string() << "\n";
        std::cout << "index: " << index_path.generic_string() << "\n";
    }
    if (packages.empty()) warn("registry index contains no packages");

    for (const auto& pkg : packages) {
        std::string key = package_key(pkg.name, pkg.version);
        if (!seen.insert(key).second) {
            fail("duplicate registry package entry: " + key);
            continue;
        }

        if (remote) {
            if (pkg.yanked) {
                warn(key + " is yanked; skipping bundle download because remote registry blocks yanked versions");
                continue;
            }
            std::string bundle_rel = pkg.bundle.empty()
                ? (pkg.name + "/" + pkg.version + "/package.surabundle.json")
                : pkg.bundle;
            while (!bundle_rel.empty() && bundle_rel.front() == '/') bundle_rel.erase(bundle_rel.begin());
            std::string bundle_url = bundle_rel.rfind("http://", 0) == 0 || bundle_rel.rfind("https://", 0) == 0
                ? bundle_rel
                : (url + "/" + bundle_rel);
            std::string bundle = http_get_text(bundle_url);
            if (bundle.empty()) {
                fail(key + " remote bundle missing: " + bundle_url);
                continue;
            }
            std::string bundle_hash = sha256_text(bundle);
            fs::path tmp = fs::temp_directory_path() /
                ("sura_registry_verify_remote_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            if (!extract_package_bundle(bundle, tmp)) {
                fail(key + " remote bundle extraction failed");
                std::error_code ec;
                fs::remove_all(tmp, ec);
                continue;
            }
            std::string manifest = read_all(tmp / kManifest);
            std::string manifest_name = normalize_name(manifest_field(manifest, "name", ""));
            std::string manifest_version = manifest_field(manifest, "version", "");
            std::string extracted_hash = package_hash(tmp);
            if (manifest_name != normalize_name(pkg.name) || manifest_version != pkg.version) {
                fail(key + " remote manifest/index mismatch");
            }
            if (pkg.hash.empty()) {
                fail(key + " index hash missing");
            } else if (pkg.hash != bundle_hash && pkg.hash != extracted_hash) {
                fail(key + " remote index hash mismatch");
            }
            std::string sig_msg;
            if (!package_signature_status(tmp, sig_msg, {}, url + "/keys")) {
                fail(key + " " + sig_msg);
            } else {
                std::cout << "[OK] " << key << " " << sig_msg << "\n";
            }
            std::error_code ec;
            fs::remove_all(tmp, ec);
            continue;
        }

        fs::path version_dir = root / pkg.name / pkg.version;
        if (!fs::is_directory(version_dir)) {
            fail(key + " directory missing: " + version_dir.generic_string());
            continue;
        }
        std::string manifest = read_all(version_dir / kManifest);
        if (manifest.empty()) {
            fail(key + " manifest missing");
            continue;
        }
        std::string manifest_name = normalize_name(manifest_field(manifest, "name", ""));
        std::string manifest_version = manifest_field(manifest, "version", "");
        if (manifest_name != normalize_name(pkg.name) || manifest_version != pkg.version) {
            fail(key + " manifest/index mismatch");
        }

        fs::path bundle_path = pkg.bundle.empty()
            ? (version_dir / "package.surabundle.json")
            : (root / fs::path(pkg.bundle).lexically_normal());
        std::string bundle = read_all(bundle_path);
        if (bundle.empty()) {
            fail(key + " bundle missing: " + bundle_path.generic_string());
        } else {
            std::string dir_hash = package_hash(version_dir);
            std::string bundle_hash = sha256_text(bundle);
            if (pkg.hash.empty()) {
                fail(key + " index hash missing");
            } else if (pkg.hash != dir_hash && pkg.hash != bundle_hash) {
                fail(key + " index hash mismatch");
            }

            fs::path tmp = fs::temp_directory_path() /
                ("sura_registry_verify_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            if (!extract_package_bundle(bundle, tmp)) {
                fail(key + " bundle extraction failed");
            } else if (package_hash(tmp) != dir_hash) {
                fail(key + " bundle content does not match package directory");
            }
            std::error_code ec;
            fs::remove_all(tmp, ec);
        }

        std::string sig_msg;
        if (!package_signature_status(version_dir, sig_msg, root / "keys", "")) {
            fail(key + " " + sig_msg);
        } else {
            std::cout << "[OK] " << key << " " << sig_msg << "\n";
        }
    }

    std::string owners = remote ? http_get_text(url + "/api/owners") : read_all(root / "owners.json");
    if (!owners.empty()) {
        for (const auto& owner_key : metadata_object_keys(owners)) {
            if (owner_key == "packages") continue;
            if (remote) {
                if (!package_set_contains(seen, owner_key, "")) warn("owner metadata references missing package: " + owner_key);
            } else if (!fs::is_directory(root / normalize_name(owner_key))) {
                warn("owner metadata references missing package: " + owner_key);
            }
        }
    }

    std::string yanks = remote ? http_get_text(url + "/api/yanks") : read_all(root / "yanks.json");
    if (!yanks.empty()) {
        for (const auto& yank_key : metadata_object_keys(yanks)) {
            if (yank_key == "yanked" || yank_key.find('@') == std::string::npos) continue;
            PackageRef ref = parse_package_ref(yank_key);
            if (!package_set_contains(seen, ref.name, ref.version)) {
                fail("yank metadata references missing package version: " + yank_key);
            }
        }
    }

    std::string reports = remote ? "" : read_all(root / "reports.json");
    if (!reports.empty()) {
        std::regex report_re("\\{[^{}]*\"name\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^{}]*\"version\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^{}]*\\}");
        for (auto it = std::sregex_iterator(reports.begin(), reports.end(), report_re);
             it != std::sregex_iterator(); ++it) {
            std::string name = normalize_name(json_unescape((*it)[1].str()));
            std::string version = json_unescape((*it)[2].str());
            if (!package_set_contains(seen, name, version)) {
                warn("report references package not in registry index: " + name + (version.empty() ? "" : "@" + version));
            }
        }
    }

    std::string advisories = remote ? http_get_text(url + "/api/advisories") : read_all(root / "advisories.json");
    if (!advisories.empty()) {
        for (const auto& advisory : parse_registry_advisories(advisories)) {
            std::string name = normalize_name(advisory.name);
            std::string version = advisory.version;
            if (name.empty()) {
                warn("advisory metadata missing package name: " + advisory.id);
                continue;
            }
            if (!package_set_contains(seen, name, version)) {
                fail("advisory metadata references missing package version: " +
                     name + (version.empty() ? "" : ("@" + version)));
            }
        }
    }

    std::cout << "verify-registry: " << seen.size() << " package(s), "
              << warnings << " warning(s), " << errors << " error(s)\n";
    return finish_verify_registry_command(json_report, remote, url, root, index_ref,
                                          seen.size(), warnings, errors, findings,
                                          "registry verification failed");
}

static int cmd_verify_registry(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("verify-registry --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("verify-registry --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg verify-registry [path] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("verify-registry accepts at most one path");
        }
    }
    return run_verify_registry_command(source, json_report);
}

static int cmd_resolve(const std::vector<std::string>& argv) {
    bool json_output = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& opt = argv[i];
        if (opt == "--json") json_output = true;
        else if (opt == "--help" || opt == "-h") {
            std::cout << "Usage: surapkg resolve [--json]\n";
            return 0;
        } else {
            return err("resolve accepts only --json");
        }
    }
    fs::path root = fs::current_path();
    DependencyGraph graph = resolve_dependency_graph(root, !json_output);
    if (json_output) {
        write_dependency_graph_json(std::cout, root, graph);
        return graph.errors.empty() ? 0 : 1;
    }
    return graph.errors.empty() ? 0 : err("dependency resolution failed");
}

static bool write_publish_json_report(const fs::path& report_path,
                                      const std::string& name,
                                      const std::string& version,
                                      const fs::path& source_root,
                                      const fs::path& registry_dir,
                                      const fs::path& bundle_path,
                                      const std::string& bundle_hash,
                                      const std::string& remote_url,
                                      bool remote_uploaded,
                                      bool dry_run,
                                      bool passed) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"sura.package.publish.v1\",\n";
    out << "  \"package\": \"" << json_escape(name) << "\",\n";
    out << "  \"version\": \"" << json_escape(version) << "\",\n";
    out << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    out << "  \"source\": \"" << json_escape(source_root.generic_string()) << "\",\n";
    out << "  \"registry_dir\": \"" << json_escape(registry_dir.generic_string()) << "\",\n";
    out << "  \"bundle\": \"" << json_escape(bundle_path.generic_string()) << "\",\n";
    out << "  \"hash\": \"" << json_escape(bundle_hash) << "\",\n";
    out << "  \"remote_url\": \"" << json_escape(remote_url) << "\",\n";
    out << "  \"dry_run\": " << (dry_run ? "true" : "false") << ",\n";
    out << "  \"remote_uploaded\": " << (remote_uploaded ? "true" : "false") << "\n";
    out << "}\n";
    return write_all(report_path, out.str());
}

static int run_publish_command(const std::string& source, const fs::path& json_report, bool dry_run = false) {
    fs::path root = source.empty() ? fs::current_path() : fs::path(source);
    fs::path manifest_path = fs::is_directory(root) ? root / kManifest : kManifest;
    std::string manifest = read_all(manifest_path);
    if (manifest.empty()) return err("sura.pkg.json not found for publish");
    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "0.1.0");
    fs::path dst = registry_root() / name / version;
    fs::path publish_root = dst;
    if (dry_run) {
        publish_root = fs::temp_directory_path() /
            ("sura_publish_dry_run_" + name + "_" + version + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    std::error_code ec;
    fs::remove_all(publish_root, ec);
    fs::create_directories(publish_root.parent_path(), ec);
    if (ec) return err("registry create failed: " + ec.message());
    if (fs::is_directory(root)) {
        fs::copy(root, publish_root, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    } else {
        fs::create_directories(publish_root, ec);
        fs::copy_file(root, publish_root / root.filename(), fs::copy_options::overwrite_existing, ec);
        write_all(publish_root / kManifest, manifest);
    }
    if (ec) return err("publish copy failed: " + ec.message());
    if (!write_package_signature(publish_root)) return err("failed to write package signature");
    fs::path bundle_path = publish_root / "package.surabundle.json";
    if (!write_package_bundle(publish_root, bundle_path)) return err("failed to write registry bundle");
    std::string url = registry_url();
    std::string token = registry_token();
    bool remote_uploaded = false;
    if (!dry_run) {
        fs::path latest = registry_root() / name / "latest";
        fs::remove_all(latest, ec);
        fs::copy(dst, latest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) return err("latest alias update failed: " + ec.message());
        if (write_registry_index() != 0) return err("failed to write registry index");
        if (!url.empty() && !token.empty()) {
            std::string response;
            if (!http_post_file(url + "/api/publish", bundle_path, token, response)) {
                return err("HTTP registry publish failed at " + url + "/api/publish");
            }
            remote_uploaded = true;
            info("uploaded " + name + "@" + version + " to HTTP registry " + url);
        } else if (!url.empty()) {
            info("SURA_REGISTRY_URL is set; set SURA_REGISTRY_TOKEN to upload during publish");
        }
    } else {
        info("dry-run publish validated " + name + "@" + version + " without writing registry or uploading");
    }
    if (!json_report.empty()) {
        std::string bundle_hash = sha256_text(read_all(bundle_path));
        if (!write_publish_json_report(json_report, name, version, root, dst, bundle_path,
                                       bundle_hash, url, remote_uploaded, dry_run, true)) {
            return err("failed to write publish JSON report: " + json_report.generic_string());
        }
        ok("publish report written: " + json_report.generic_string());
    }
    if (dry_run) ok("dry-run publish passed " + name + "@" + version + " -> " + dst.generic_string());
    else ok("published " + name + "@" + version + " -> " + dst.generic_string());
    return 0;
}

static int cmd_publish_path(const std::string& source) {
    return run_publish_command(source, fs::path());
}

static int cmd_publish(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    bool dry_run = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("publish --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("publish --json requires an output path");
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg publish [path] [--dry-run] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("publish accepts at most one path");
        }
    }
    return run_publish_command(source, json_report, dry_run);
}

static bool write_registry_index_json_report(const fs::path& report_path,
                                             const fs::path& registry,
                                             const fs::path& index_path,
                                             const std::vector<RegistryPackage>& packages) {
    int yanked_count = 0;
    for (const auto& pkg : packages) {
        if (pkg.yanked) ++yanked_count;
    }
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.registry.index.v1\",\n"
        << "  \"passed\": true,\n"
        << "  \"registry\": \"" << json_escape(path_to_generic_utf8(registry)) << "\",\n"
        << "  \"index\": \"" << json_escape(path_to_generic_utf8(index_path)) << "\",\n"
        << "  \"package_count\": " << packages.size() << ",\n"
        << "  \"yanked_count\": " << yanked_count << ",\n"
        << "  \"packages\": [\n";
    for (size_t i = 0; i < packages.size(); ++i) {
        const auto& pkg = packages[i];
        out << "    {\"name\":\"" << json_escape(pkg.name)
            << "\",\"version\":\"" << json_escape(pkg.version)
            << "\",\"bundle\":\"" << json_escape(pkg.bundle)
            << "\",\"hash\":\"" << json_escape(pkg.hash)
            << "\",\"yanked\":" << (pkg.yanked ? "true" : "false") << "}";
        if (i + 1 < packages.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int cmd_registry_index(const std::vector<std::string>& argv) {
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("index --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("index --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg index [--json report.json]\n";
            return 0;
        } else {
            return err("index accepts only optional --json");
        }
    }
    if (write_registry_index() != 0) return err("failed to write registry/index.json");
    fs::path registry = registry_root();
    fs::path index_path = registry / "index.json";
    std::vector<RegistryPackage> packages = parse_registry_packages(read_all(index_path));
    if (!json_report.empty()) {
        if (!write_registry_index_json_report(json_report, registry, index_path, packages)) {
            return err("failed to write index JSON report: " + json_report.generic_string());
        }
        ok("index report written: " + json_report.generic_string());
    }
    ok("wrote " + index_path.generic_string());
    return 0;
}

static bool write_trust_key_json_report(const fs::path& report_path,
                                        const std::string& key_id,
                                        const fs::path& source,
                                        const fs::path& destination,
                                        const std::string& store,
                                        const std::string& hash,
                                        bool passed) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.registry.trust_key.v1\",\n"
        << "  \"key_id\": \"" << json_escape(key_id) << "\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"source\": \"" << json_escape(source.generic_string()) << "\",\n"
        << "  \"destination\": \"" << json_escape(destination.generic_string()) << "\",\n"
        << "  \"store\": \"" << json_escape(store) << "\",\n"
        << "  \"hash\": \"" << json_escape(hash) << "\"\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_trust_key_command(const std::string& key_id,
                                 const fs::path& source,
                                 const fs::path& json_report) {
    if (key_id.empty() || source.empty()) return err("trust-key requires a key id and public key path");
    std::string pem = read_all(source);
    if (pem.empty()) return err("public key file not found or empty: " + source.generic_string());
    if (!looks_like_public_key(pem)) return err("public key file does not look like a PEM public key: " + source.generic_string());

    fs::path key_dir = trusted_public_key_dir();
    std::string store = "key_dir";
    if (key_dir.empty()) {
        key_dir = registry_root() / "keys";
        store = "registry";
    }
    fs::path dst = key_dir / public_key_filename(key_id);
    if (!write_all(dst, pem)) return err("failed to write trusted public key: " + dst.generic_string());
    if (!json_report.empty()) {
        if (!write_trust_key_json_report(json_report, key_id, source, dst, store, sha256_text(pem), true)) {
            return err("failed to write trust-key JSON report: " + json_report.generic_string());
        }
        ok("trust-key report written: " + json_report.generic_string());
    }
    ok("trusted public key " + key_id + " -> " + dst.generic_string());
    return 0;
}

static int cmd_trust_key(const std::vector<std::string>& argv) {
    std::string key_id;
    fs::path source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("trust-key --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("trust-key --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage:\n  surapkg trust-key <key-id> <public-key.pem> [--json report.json]\n";
            return 0;
        } else if (key_id.empty()) {
            key_id = arg;
        } else if (source.empty()) {
            source = fs::path(arg);
        } else {
            return err("trust-key accepts one key id, one public key path, and optional --json");
        }
    }
    return run_trust_key_command(key_id, source, json_report);
}

static std::vector<fs::path> package_sura_files(const fs::path& root) {
    std::vector<fs::path> files;
    if (!fs::exists(root)) return files;
    if (fs::is_regular_file(root) && root.extension() == ".sura") {
        files.push_back(root);
        return files;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sura") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

static bool has_suffix(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size()
        && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::vector<fs::path> package_plugin_manifests(const fs::path& root) {
    std::vector<fs::path> files;
    if (!fs::exists(root)) return files;
    if (fs::is_regular_file(root) && has_suffix(root.filename().string(), ".sura-plugin.json")) {
        files.push_back(root);
        return files;
    }
    if (!fs::is_directory(root)) return files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() &&
            has_suffix(entry.path().filename().string(), ".sura-plugin.json")) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

static std::vector<fs::path> package_tool_manifests(const fs::path& root) {
    std::vector<fs::path> files;
    if (!fs::exists(root)) return files;
    if (fs::is_regular_file(root) && root.filename() == kToolPolicyManifest) {
        files.push_back(root);
        return files;
    }
    if (!fs::is_directory(root)) return files;
    fs::path root_manifest = root / kToolPolicyManifest;
    if (fs::exists(root_manifest) && fs::is_regular_file(root_manifest)) {
        files.push_back(root_manifest);
    }
    return files;
}

static bool is_hex_sha256(const std::string& text) {
    if (text.size() != 64) return false;
    for (unsigned char ch : text) {
        if (!std::isxdigit(ch)) return false;
    }
    return true;
}

static std::vector<std::string> manifest_string_array(const std::string& manifest, const std::string& field) {
    std::regex array_re("\"" + field + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch m;
    std::vector<std::string> out;
    if (!std::regex_search(manifest, m, array_re)) return out;
    std::string body = m[1].str();
    std::regex str_re("\"((?:\\\\.|[^\"])*)\"");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), str_re);
         it != std::sregex_iterator(); ++it) {
        out.push_back(json_unescape((*it)[1].str()));
    }
    return out;
}

static bool manifest_bool_field(const std::string& manifest, const std::string& field, bool fallback, bool* found = nullptr) {
    std::regex re("\"" + field + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (std::regex_search(manifest, m, re)) {
        if (found) *found = true;
        return m[1].str() == "true";
    }
    if (found) *found = false;
    return fallback;
}

static double manifest_number_field(const std::string& manifest, const std::string& field, double fallback, bool* found = nullptr) {
    std::regex re("\"" + field + "\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
    std::smatch m;
    if (std::regex_search(manifest, m, re)) {
        if (found) *found = true;
        return std::stod(m[1].str());
    }
    if (found) *found = false;
    return fallback;
}

static std::vector<std::pair<std::string, std::string>> manifest_string_object(
    const std::string& manifest, const std::string& field, bool* found = nullptr) {
    std::regex obj_re("\"" + field + "\"\\s*:\\s*\\{([^}]*)\\}");
    std::smatch m;
    std::vector<std::pair<std::string, std::string>> out;
    if (!std::regex_search(manifest, m, obj_re)) {
        if (found) *found = false;
        return out;
    }
    if (found) *found = true;
    std::string body = m[1].str();
    std::regex pair_re("\"((?:\\\\.|[^\"])*)\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), pair_re);
         it != std::sregex_iterator(); ++it) {
        out.push_back({json_unescape((*it)[1].str()), json_unescape((*it)[2].str())});
    }
    return out;
}

static bool manifest_version_is_one(const std::string& manifest) {
    return std::regex_search(manifest, std::regex("\"version\"\\s*:\\s*1"));
}

static bool string_array_contains(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

static bool http_header_name_manifest_safe(const std::string& name) {
    if (name.empty()) return false;
    for (unsigned char ch : name) {
        if (!(std::isalnum(ch) || ch == '-' || ch == '_')) return false;
    }
    return true;
}

static bool path_contains_parent_ref(const fs::path& p) {
    for (const auto& part : p) {
        if (part == "..") return true;
    }
    return false;
}

struct AuditFinding {
    std::string kind;
    std::string message;
    fs::path file;
    int line = 0;
};

static void record_audit_finding(std::vector<AuditFinding>* out,
                                 const std::string& kind,
                                 const std::string& message,
                                 const fs::path& file = fs::path(),
                                 int line = 0) {
    if (!out) return;
    out->push_back(AuditFinding{kind, message, file, line});
}

struct PluginPolicyAudit {
    bool present = false;
    std::set<std::string> manifests;
    std::set<std::string> allowed_exports;
    std::set<std::string> host_capabilities;
    double max_memory_bytes = 0;
    double max_call_ms = 0;
    bool restrict_memory_bytes = false;
    bool restrict_call_ms = false;
    bool restrict_host_capabilities = false;
};

static std::string normalized_relative_manifest_path(const fs::path& path) {
    return path.lexically_normal().generic_string();
}

static bool valid_plugin_host_capability(const std::string& name) {
    return name == "log" || name == "memory" || name == "cancel";
}

static std::set<std::string> plugin_host_capability_set(const std::vector<std::string>& values) {
    return std::set<std::string>(values.begin(), values.end());
}

static int audit_plugin_policy_manifest(const fs::path& root,
                                        const std::vector<fs::path>& plugin_manifests,
                                        PluginPolicyAudit& policy,
                                        bool verbose = true,
                                        std::vector<AuditFinding>* findings_out = nullptr) {
    if (!fs::is_directory(root)) return 0;
    fs::path policy_path = root / kPluginPolicyManifest;
    if (!fs::exists(policy_path)) {
        if (plugin_manifests.empty()) return 0;
        std::string msg = "[audit] package plugin policy manifest missing: " + policy_path.generic_string();
        if (verbose) std::cout << msg << "\n";
        record_audit_finding(findings_out, "plugin_policy", msg, policy_path);
        return 1;
    }

    policy.present = true;
    std::string manifest = read_all(policy_path);
    int findings = 0;
    auto finding = [&](const std::string& msg) {
        std::string full = "[audit] package plugin policy " + msg + " at " + policy_path.generic_string();
        if (verbose) {
            std::cout << full << "\n";
        }
        record_audit_finding(findings_out, "plugin_policy", full, policy_path);
        ++findings;
    };

    if (!manifest_version_is_one(manifest)) finding("must contain numeric field 'version': 1");
    std::string sandbox = manifest_field(manifest, "sandbox", "");
    if (sandbox != "manifest-locked") {
        finding("must set \"sandbox\": \"manifest-locked\"");
    }
    std::vector<std::string> manifests = manifest_string_array(manifest, "manifests");
    if (manifests.empty()) finding("must contain non-empty string array 'manifests'");
    std::vector<std::string> allowed_exports = manifest_string_array(manifest, "allowed_exports");
    if (allowed_exports.empty()) allowed_exports = manifest_string_array(manifest, "exports");
    bool allowed_exports_present = manifest_has_field(manifest, "allowed_exports") || manifest_has_field(manifest, "exports");
    if (allowed_exports_present && allowed_exports.empty()) {
        finding("allowed_exports must be a non-empty string array when present");
    }
    for (const auto& name : allowed_exports) {
        if (name.empty()) {
            finding("allowed_exports must not contain empty names");
            continue;
        }
        if (!policy.allowed_exports.insert(name).second) {
            finding("duplicate allowed export: " + name);
        }
    }
    std::vector<std::string> host_capabilities = manifest_string_array(manifest, "host_capabilities");
    policy.restrict_host_capabilities = manifest_has_field(manifest, "host_capabilities");
    for (const auto& name : host_capabilities) {
        if (!valid_plugin_host_capability(name)) {
            finding("unsupported host capability: " + name);
            continue;
        }
        if (!policy.host_capabilities.insert(name).second) {
            finding("duplicate host capability: " + name);
        }
    }
    bool max_memory_found = false;
    double max_memory_bytes = manifest_number_field(manifest, "max_memory_bytes", 0, &max_memory_found);
    policy.restrict_memory_bytes = max_memory_found;
    policy.max_memory_bytes = max_memory_bytes;
    if (max_memory_found &&
        (max_memory_bytes < 0 || std::floor(max_memory_bytes) != max_memory_bytes ||
         max_memory_bytes > (double)std::numeric_limits<size_t>::max())) {
        finding("max_memory_bytes must be a non-negative integer");
    }
    bool max_call_found = false;
    double max_call_ms = manifest_number_field(manifest, "max_call_ms", 0, &max_call_found);
    policy.restrict_call_ms = max_call_found;
    policy.max_call_ms = max_call_ms;
    if (max_call_found &&
        (max_call_ms < 0 || std::floor(max_call_ms) != max_call_ms ||
         max_call_ms > (double)std::numeric_limits<size_t>::max())) {
        finding("max_call_ms must be a non-negative integer");
    }

    for (const auto& item : manifests) {
        if (item.empty()) {
            finding("manifests must not contain empty paths");
            continue;
        }
        fs::path rel(item);
        std::string normalized = normalized_relative_manifest_path(rel);
        if (rel.is_absolute()) finding("manifest path must be relative: " + item);
        if (path_contains_parent_ref(rel.lexically_normal())) finding("manifest path must not escape with '..': " + item);
        if (!has_suffix(rel.filename().string(), ".sura-plugin.json")) {
            finding("manifest path must end with .sura-plugin.json: " + item);
        }
        if (!policy.manifests.insert(normalized).second) {
            finding("duplicate manifest path: " + normalized);
        }
        fs::path target = root / rel;
        if (!fs::exists(target) || !fs::is_regular_file(target)) {
            finding("listed plugin manifest not found: " + target.generic_string());
        } else {
            std::string plugin_manifest = read_all(target);
            if (!policy.allowed_exports.empty()) {
                std::vector<std::string> plugin_exports = manifest_string_array(plugin_manifest, "exports");
                if (plugin_exports.empty()) plugin_exports = manifest_string_array(plugin_manifest, "allowed_exports");
                std::set<std::string> plugin_export_set(plugin_exports.begin(), plugin_exports.end());
                for (const auto& name : policy.allowed_exports) {
                    if (plugin_export_set.count(name) == 0) {
                        finding("allowed export not declared by plugin manifest " + normalized + ": " + name);
                    }
                }
            }
            if (policy.restrict_host_capabilities) {
                std::vector<std::string> plugin_caps = manifest_string_array(plugin_manifest, "host_capabilities");
                std::set<std::string> plugin_cap_set = manifest_has_field(plugin_manifest, "host_capabilities")
                    ? plugin_host_capability_set(plugin_caps)
                    : std::set<std::string>{"log", "memory", "cancel"};
                for (const auto& name : policy.host_capabilities) {
                    if (plugin_cap_set.count(name) == 0) {
                        finding("host capability not declared by plugin manifest " + normalized + ": " + name);
                    }
                }
            }
        }
    }

    for (const auto& discovered : plugin_manifests) {
        std::error_code ec;
        fs::path rel = fs::relative(discovered, root, ec);
        if (ec) rel = discovered.filename();
        std::string normalized = normalized_relative_manifest_path(rel);
        if (!policy.manifests.empty() && policy.manifests.count(normalized) == 0) {
            finding("plugin manifest not listed in package policy: " + normalized);
        }
    }

    return findings;
}

static int audit_plugin_manifest(const fs::path& manifest_path,
                                 bool verbose = true,
                                 std::vector<AuditFinding>* findings_out = nullptr) {
    std::string manifest = read_all(manifest_path);
    int findings = 0;
    auto finding = [&](const std::string& msg) {
        std::string full = "[audit] plugin manifest " + msg + " at " + manifest_path.generic_string();
        if (verbose) {
            std::cout << full << "\n";
        }
        record_audit_finding(findings_out, "plugin_manifest", full, manifest_path);
        ++findings;
    };

    std::string rel_path = manifest_field(manifest, "path", "");
    std::string name = manifest_field(manifest, "name", "");
    std::string version = manifest_field(manifest, "version", "");
    std::string expected_hash = manifest_field(manifest, "sha256", "");
    std::vector<std::string> exports = manifest_string_array(manifest, "exports");
    if (exports.empty()) exports = manifest_string_array(manifest, "allowed_exports");
    std::vector<std::string> host_capabilities = manifest_string_array(manifest, "host_capabilities");
    bool max_memory_found = false;
    double max_memory_bytes = manifest_number_field(manifest, "max_memory_bytes", 0, &max_memory_found);
    bool max_call_found = false;
    double max_call_ms = manifest_number_field(manifest, "max_call_ms", 0, &max_call_found);

    if (rel_path.empty()) finding("missing string field 'path'");
    if (name.empty()) finding("missing string field 'name'");
    if (version.empty()) finding("missing string field 'version'");
    if (!is_hex_sha256(expected_hash)) finding("missing or invalid 64-char 'sha256'");
    if (exports.empty()) finding("missing non-empty string array 'exports'");
    if (max_memory_found &&
        (max_memory_bytes < 0 || std::floor(max_memory_bytes) != max_memory_bytes ||
         max_memory_bytes > (double)std::numeric_limits<size_t>::max())) {
        finding("max_memory_bytes must be a non-negative integer");
    }
    if (max_call_found &&
        (max_call_ms < 0 || std::floor(max_call_ms) != max_call_ms ||
         max_call_ms > (double)std::numeric_limits<size_t>::max())) {
        finding("max_call_ms must be a non-negative integer");
    }
    std::set<std::string> seen_host_capabilities;
    for (const auto& cap : host_capabilities) {
        if (!valid_plugin_host_capability(cap)) {
            finding("unsupported host capability: " + cap);
            continue;
        }
        if (!seen_host_capabilities.insert(cap).second) {
            finding("duplicate host capability: " + cap);
        }
    }

    fs::path lib_rel(rel_path);
    if (!rel_path.empty()) {
        if (lib_rel.is_absolute()) finding("path must be relative to the manifest");
        if (path_contains_parent_ref(lib_rel.lexically_normal())) finding("path must not escape with '..'");

        fs::path lib_path = manifest_path.parent_path() / lib_rel;
        if (!fs::exists(lib_path)) {
            finding("library not found: " + lib_path.generic_string());
        } else if (is_hex_sha256(expected_hash)) {
            std::string actual = sha256_file(lib_path);
            if (actual.empty()) finding("could not hash library: " + lib_path.generic_string());
            else {
                std::transform(expected_hash.begin(), expected_hash.end(), expected_hash.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (actual != expected_hash) finding("sha256 mismatch for " + lib_path.generic_string());
            }
        }
    }

    return findings;
}

static int audit_tool_manifest(const fs::path& manifest_path,
                               bool verbose = true,
                               std::vector<AuditFinding>* findings_out = nullptr) {
    std::string manifest = read_all(manifest_path);
    int findings = 0;
    auto finding = [&](const std::string& msg) {
        std::string full = "[audit] tool policy manifest " + msg + " at " + manifest_path.generic_string();
        if (verbose) {
            std::cout << full << "\n";
        }
        record_audit_finding(findings_out, "tool_policy", full, manifest_path);
        ++findings;
    };

    if (manifest.empty()) {
        finding("is empty");
        return findings;
    }

    if (!manifest_version_is_one(manifest)) finding("must contain numeric field 'version': 1");

    std::vector<std::string> tools = manifest_string_array(manifest, "tools");
    std::vector<std::string> url_prefixes = manifest_string_array(manifest, "url_prefixes");
    std::vector<std::string> http_methods = manifest_string_array(manifest, "http_methods");
    std::vector<std::string> allowed_headers = manifest_string_array(manifest, "allowed_headers");
    std::vector<std::string> command_prefixes = manifest_string_array(manifest, "command_prefixes");
    bool required_headers_found = false;
    auto required_headers = manifest_string_object(manifest, "required_headers", &required_headers_found);
    bool max_body_found = false;
    double max_body_bytes = manifest_number_field(manifest, "max_body_bytes", 0, &max_body_found);
    bool max_timeout_found = false;
    double max_timeout = manifest_number_field(manifest, "max_timeout", 0, &max_timeout_found);
    bool allow_shell_found = false;
    bool allow_shell = manifest_bool_field(manifest, "allow_shell", false, &allow_shell_found);
    bool approval_bool_found = false;
    bool approval_bool = manifest_bool_field(manifest, "approval", false, &approval_bool_found);
    bool approval_string_found = false;
    std::string approval_mode = manifest_string_field(manifest, "approval", "", &approval_string_found);
    bool approval_token_found = false;
    std::string approval_token = manifest_string_field(manifest, "approval_token", "", &approval_token_found);
    bool approval_message_found = false;
    std::string approval_message = manifest_string_field(manifest, "approval_message", "", &approval_message_found);

    if (tools.empty()) finding("must contain non-empty string array 'tools'");
    for (const auto& tool : tools) {
        if (tool != "http_get" && tool != "http_request" && tool != "shell") {
            finding("contains unknown tool '" + tool + "'");
        }
    }

    bool approval_required = false;
    if (manifest_has_field(manifest, "approval")) {
        if (approval_bool_found) {
            approval_required = approval_bool;
        } else if (approval_string_found) {
            std::string mode = approval_mode;
            std::transform(mode.begin(), mode.end(), mode.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (mode == "true" || mode == "yes" || mode == "on" ||
                mode == "required" || mode == "always" || mode == "manual" ||
                mode == "interactive" || mode == "token") {
                approval_required = true;
            } else if (mode == "false" || mode == "no" || mode == "off" ||
                       mode == "none" || mode == "never" || mode == "optional") {
                approval_required = false;
            } else {
                finding("approval contains unsupported mode: " + approval_mode);
            }
        } else {
            finding("approval must be a boolean or string mode");
        }
    }
    if (manifest_has_field(manifest, "approval_token")) {
        if (!approval_token_found) {
            finding("approval_token must be a string");
        } else {
            if (!approval_required) finding("approval_token requires approval to be enabled");
            if (approval_token.empty() || approval_token.size() > 512 ||
                approval_token.find_first_of("\r\n") != std::string::npos) {
                finding("approval_token must be 1..512 bytes and contain no newlines");
            }
        }
    }
    if (manifest_has_field(manifest, "approval_message")) {
        if (!approval_message_found) {
            finding("approval_message must be a string");
        } else if (approval_message.find_first_of("\r\n") != std::string::npos) {
            finding("approval_message must not contain newlines");
        }
    }

    bool has_http_get = string_array_contains(tools, "http_get");
    bool has_http_request = string_array_contains(tools, "http_request");
    if (has_http_get || has_http_request) {
        if (url_prefixes.empty()) finding("http tools require non-empty 'url_prefixes'");
        for (const auto& prefix : url_prefixes) {
            if (prefix.empty()) finding("url_prefixes must not contain empty strings");
            if (prefix.rfind("http://", 0) != 0 &&
                prefix.rfind("https://", 0) != 0 &&
                prefix.rfind("file://", 0) != 0) {
                finding("url prefix must start with http://, https://, or file://: " + prefix);
            }
        }
    } else if (!url_prefixes.empty()) {
        finding("url_prefixes requires http_get or http_request in tools");
    }

    if (has_http_request) {
        if (http_methods.empty()) finding("http_request requires non-empty 'http_methods'");
        for (const auto& method : http_methods) {
            if (method.empty()) finding("http_methods must not contain empty strings");
            for (char ch : method) {
                if (!std::isalpha((unsigned char)ch)) {
                    finding("http method contains unsupported characters: " + method);
                    break;
                }
            }
        }
        for (const auto& header : allowed_headers) {
            if (!http_header_name_manifest_safe(header)) {
                finding("allowed_headers contains unsupported header name: " + header);
            }
        }
        if (required_headers_found && required_headers.empty()) {
            finding("required_headers must be an object with string header values");
        }
        for (const auto& [header, value] : required_headers) {
            if (!http_header_name_manifest_safe(header)) {
                finding("required_headers contains unsupported header name: " + header);
            }
            if (value.find_first_of("\r\n") != std::string::npos) {
                finding("required_headers contains unsupported newline characters");
            }
        }
        if (max_body_found && max_body_bytes < 0) finding("max_body_bytes must be non-negative");
        if (max_timeout_found && max_timeout <= 0) finding("max_timeout must be positive");
    } else if (!http_methods.empty()) {
        finding("http_methods requires http_request in tools");
    }
    if (!has_http_request) {
        if (!allowed_headers.empty()) finding("allowed_headers requires http_request in tools");
        if (required_headers_found) finding("required_headers requires http_request in tools");
        if (max_body_found) finding("max_body_bytes requires http_request in tools");
        if (max_timeout_found) finding("max_timeout requires http_request in tools");
    }

    if (string_array_contains(tools, "shell")) {
        if (!allow_shell_found || !allow_shell) finding("shell requires 'allow_shell': true");
        if (command_prefixes.empty()) finding("shell requires non-empty 'command_prefixes'");
        for (const auto& prefix : command_prefixes) {
            if (prefix.empty()) finding("command_prefixes must not contain empty strings");
            if (prefix.find_first_of("\"\r\n") != std::string::npos) {
                finding("command prefix contains unsupported characters");
            }
        }
    } else {
        if (allow_shell) finding("allow_shell true requires shell in tools");
        if (!command_prefixes.empty()) finding("command_prefixes requires shell in tools");
    }

    fs::path sig_path = manifest_path.parent_path() / kToolPolicySignature;
    if (fs::exists(sig_path)) {
        std::string sig_msg;
        if (!tool_policy_signature_status(manifest_path, sig_msg)) {
            finding(sig_msg);
        } else if (verbose) {
            std::cout << "[audit] " << sig_msg << " at " << sig_path.generic_string() << "\n";
        }
    }

    return findings;
}

static int audit_package_findings(const fs::path& root,
                                  bool verbose,
                                  std::vector<AuditFinding>* findings_out = nullptr) {
    auto tool_manifests = package_tool_manifests(root);
    bool has_tool_manifest = !tool_manifests.empty();
    auto plugin_manifests = package_plugin_manifests(root);
    PluginPolicyAudit plugin_policy;
    std::vector<std::pair<std::string, std::string>> patterns = {
        {"shell execution", "\\b(async_cmd|cmd_run(?:_checked)?|task)\\s*\\(|\\b(os\\.run(?:_checked)?|os\\.cmd|async\\.cmd)\\s*\\("},
        {"network access", "(http_(get|json|post|request(_retry_json_checked|_json_checked|_retry_json|_full|_retry|_json)?|serve_static|serve_routes)|http\\.(get|json|post|request(_retry_json_checked|_json_checked|_retry_json|_full|_retry|_json)?|serve_static|serve_routes))\\s*\\("},
        {"file deletion", "(file_delete|file_remove_tree|remove_tree)\\s*\\(|fs\\.(delete|remove|remove_tree|delete_tree)\\s*\\("},
        {"python bridge", "python_"},
        {"native ffi/plugin", "ffi_|ffi\\.|plugin_|plugin\\.|c_call|load_library"},
        {"absolute Windows path", "(^|[^A-Za-z])[A-Za-z]:\\\\[A-Za-z0-9_./\\\\-]"}
    };
    std::regex direct_tool_re("\\btool_call\\s*\\(|\\btool\\.call\\s*\\(|\\btool\\s*\\(|\\btool\\s+(http_get|http_request|shell)\\b");
    std::regex policy_tool_re("\\btool_call_policy\\s*\\(|\\btool\\.call_policy\\s*\\(");
    int findings = 0;
    auto finding = [&](const std::string& kind,
                       const std::string& msg,
                       const fs::path& file = fs::path(),
                       int line = 0) {
        if (verbose) std::cout << msg << "\n";
        record_audit_finding(findings_out, kind, msg, file, line);
        ++findings;
    };
    if (tool_manifests.size() > 1) {
        finding("tool_policy",
                "[audit] package should contain one " + kToolPolicyManifest.generic_string(),
                root / kToolPolicyManifest);
    }
    findings += audit_plugin_policy_manifest(root, plugin_manifests, plugin_policy, verbose, findings_out);
    std::regex raw_plugin_load_re("\\bplugin_load\\s*\\(|\\bplugin\\.load\\s*\\(");
    std::regex manifest_plugin_load_re("\\b(?:plugin_load_manifest|plugin\\.load_manifest)\\s*\\(\\s*[\"']([^\"']+)[\"']");
    std::regex plugin_call_literal_re("\\b(?:plugin_call|plugin\\.call)\\s*\\([^,]+,\\s*[\"']([^\"']+)[\"']");
    std::regex policy_plugin_re("\\b(plugin_load_manifest|plugin_call|plugin_info|plugin_unload)\\s*\\(|\\bplugin\\.(load_manifest|call|info|unload)\\s*\\(");
    for (const auto& file : package_sura_files(root)) {
        std::istringstream lines(read_all(file));
        std::string line;
        int line_no = 1;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::smatch manifest_plugin_match;
            bool policy_plugin_line = std::regex_search(line, policy_plugin_re);
            if (std::regex_search(line, raw_plugin_load_re)) {
                finding("plugin_policy",
                        "[audit] direct plugin_load requires a manifest and " +
                        kPluginPolicyManifest.generic_string() + " at " +
                        file.generic_string() + ":" + std::to_string(line_no),
                        file,
                        line_no);
            }
            if (std::regex_search(line, manifest_plugin_match, manifest_plugin_load_re)) {
                std::string manifest_ref = normalized_relative_manifest_path(fs::path(json_unescape(manifest_plugin_match[1].str())));
                if (!plugin_policy.present) {
                    finding("plugin_policy",
                            "[audit] plugin_load_manifest used but " +
                            kPluginPolicyManifest.generic_string() + " is missing at " +
                            file.generic_string() + ":" + std::to_string(line_no),
                            file,
                            line_no);
                } else if (plugin_policy.manifests.count(manifest_ref) == 0) {
                    finding("plugin_policy",
                            "[audit] plugin_load_manifest path not listed in " +
                            kPluginPolicyManifest.generic_string() + ": " + manifest_ref + " at " +
                            file.generic_string() + ":" + std::to_string(line_no),
                            file,
                            line_no);
                }
            }
            std::smatch plugin_call_match;
            if (plugin_policy.present && !plugin_policy.allowed_exports.empty() &&
                std::regex_search(line, plugin_call_match, plugin_call_literal_re)) {
                std::string export_name = json_unescape(plugin_call_match[1].str());
                if (plugin_policy.allowed_exports.count(export_name) == 0) {
                    finding("plugin_policy",
                            "[audit] plugin_call export not allowed by " +
                            kPluginPolicyManifest.generic_string() + ": " + export_name + " at " +
                            file.generic_string() + ":" + std::to_string(line_no),
                            file,
                            line_no);
                }
            }
            for (const auto& pat : patterns) {
                if (pat.first == "native ffi/plugin" && plugin_policy.present && policy_plugin_line) {
                    continue;
                }
                if (std::regex_search(line, std::regex(pat.second))) {
                    finding(pat.first,
                            "[audit] " + pat.first + " at " +
                            file.generic_string() + ":" + std::to_string(line_no),
                            file,
                            line_no);
                }
            }
            if (std::regex_search(line, direct_tool_re)) {
                finding("direct tool call",
                        "[audit] direct tool call without package policy at " +
                        file.generic_string() + ":" + std::to_string(line_no),
                        file,
                        line_no);
            }
            if (std::regex_search(line, policy_tool_re) && !has_tool_manifest) {
                finding("tool_policy",
                        "[audit] tool_call_policy used but " + kToolPolicyManifest.generic_string() +
                        " is missing at " + file.generic_string() + ":" + std::to_string(line_no),
                        file,
                        line_no);
            }
            ++line_no;
        }
    }
    for (const auto& file : plugin_manifests) {
        findings += audit_plugin_manifest(file, verbose, findings_out);
    }
    for (const auto& file : tool_manifests) {
        findings += audit_tool_manifest(file, verbose, findings_out);
    }
    return findings;
}

static int audit_dependency_advisories(const fs::path& root,
                                       bool verbose,
                                       std::vector<AuditFinding>* findings_out = nullptr) {
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return 0;
    int findings = 0;
    for (const auto& dep : manifest_dependency_specs(manifest)) {
        fs::path installed = root / kPackages / dep.name;
        std::string dep_manifest = read_all(installed / kManifest);
        if (dep_manifest.empty()) continue;
        std::string version = manifest_field(dep_manifest, "version", "");
        if (version.empty()) continue;
        for (const auto& advisory : registry_active_advisories_for_package(dep.name, version)) {
            std::string severity = lowercase_copy(advisory.severity);
            std::string target = dep.name + "@" + version;
            std::string title = advisory.title.empty() ? advisory.id : advisory.title;
            std::string msg = "[audit] dependency advisory " + severity + " " + target + ": " + title;
            if (severity == "critical") {
                record_audit_finding(findings_out, "registry_advisory", msg, installed / kManifest, 0);
                if (verbose) std::cout << msg << "\n";
                ++findings;
            } else if (verbose) {
                std::cout << msg << "\n";
            }
        }
    }
    return findings;
}

static std::string audit_report_file_path(const fs::path& root, const fs::path& file) {
    if (file.empty()) return "";
    std::error_code ec;
    fs::path rel = fs::relative(file, root, ec);
    if (!ec && !rel.empty() && !path_contains_parent_ref(rel)) return rel.generic_string();
    return file.generic_string();
}

static bool write_audit_json_report(const fs::path& root,
                                    const fs::path& report_path,
                                    bool passed,
                                    int finding_count,
                                    const std::vector<AuditFinding>& findings) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"root\": \"" << json_escape(root.generic_string()) << "\",\n";
    out << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    out << "  \"finding_count\": " << finding_count << ",\n";
    out << "  \"findings\": [\n";
    for (size_t i = 0; i < findings.size(); ++i) {
        const auto& item = findings[i];
        out << "    {";
        out << "\"kind\":\"" << json_escape(item.kind) << "\",";
        out << "\"message\":\"" << json_escape(item.message) << "\",";
        out << "\"file\":\"" << json_escape(audit_report_file_path(root, item.file)) << "\",";
        out << "\"line\":" << item.line;
        out << "}";
        if (i + 1 < findings.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return write_all(report_path, out.str());
}

static std::string audit_sarif_rule_id(const std::string& kind) {
    std::string out = "sura.audit.";
    bool last_sep = false;
    for (unsigned char ch : kind) {
        if (std::isalnum(ch)) {
            out.push_back((char)std::tolower(ch));
            last_sep = false;
        } else if (!last_sep && out.back() != '.') {
            out.push_back('-');
            last_sep = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out == "sura.audit." ? "sura.audit.finding" : out;
}

static bool write_audit_sarif_report(const fs::path& root,
                                     const fs::path& report_path,
                                     const std::vector<AuditFinding>& findings) {
    std::map<std::string, std::string> rules;
    for (const auto& item : findings) {
        rules[item.kind] = audit_sarif_rule_id(item.kind);
    }

    std::ostringstream out;
    out << "{\n";
    out << "  \"$schema\": \"https://json.schemastore.org/sarif-2.1.0.json\",\n";
    out << "  \"version\": \"2.1.0\",\n";
    out << "  \"runs\": [\n";
    out << "    {\n";
    out << "      \"tool\": {\n";
    out << "        \"driver\": {\n";
    out << "          \"name\": \"surapkg audit\",\n";
    out << "          \"semanticVersion\": \"1.0.0\",\n";
    out << "          \"rules\": [\n";
    size_t rule_index = 0;
    for (const auto& [kind, id] : rules) {
        out << "            {";
        out << "\"id\":\"" << json_escape(id) << "\",";
        out << "\"name\":\"" << json_escape(kind) << "\",";
        out << "\"shortDescription\":{\"text\":\"Sura audit: " << json_escape(kind) << "\"},";
        out << "\"defaultConfiguration\":{\"level\":\"error\"}";
        out << "}";
        if (++rule_index < rules.size()) out << ",";
        out << "\n";
    }
    out << "          ]\n";
    out << "        }\n";
    out << "      },\n";
    out << "      \"originalUriBaseIds\": {\n";
    out << "        \"SRCROOT\": {\"uri\":\"" << json_escape(root.generic_string()) << "/\"}\n";
    out << "      },\n";
    out << "      \"results\": [\n";
    for (size_t i = 0; i < findings.size(); ++i) {
        const auto& item = findings[i];
        std::string uri = audit_report_file_path(root, item.file);
        out << "        {";
        out << "\"ruleId\":\"" << json_escape(audit_sarif_rule_id(item.kind)) << "\",";
        out << "\"level\":\"error\",";
        out << "\"message\":{\"text\":\"" << json_escape(item.message) << "\"}";
        if (!uri.empty()) {
            out << ",\"locations\":[{\"physicalLocation\":{\"artifactLocation\":{";
            out << "\"uri\":\"" << json_escape(uri) << "\",\"uriBaseId\":\"SRCROOT\"}";
            if (item.line > 0) {
                out << ",\"region\":{\"startLine\":" << item.line << "}";
            }
            out << "}}]";
        }
        out << "}";
        if (i + 1 < findings.size()) out << ",";
        out << "\n";
    }
    out << "      ]\n";
    out << "    }\n";
    out << "  ]\n";
    out << "}\n";
    return write_all(report_path, out.str());
}

static int run_audit_command(const std::string& source,
                             const fs::path& json_report,
                             const fs::path& sarif_report) {
    fs::path root = source.empty() ? fs::current_path() : fs::path(source);
    std::vector<AuditFinding> collected;
    std::vector<AuditFinding>* sink = (json_report.empty() && sarif_report.empty()) ? nullptr : &collected;
    int findings = audit_package_findings(root, true, sink);
    findings += audit_dependency_advisories(root, true, sink);
    if (!json_report.empty()) {
        if (!write_audit_json_report(root, json_report, findings == 0, findings, collected)) {
            return err("failed to write audit JSON report: " + json_report.generic_string());
        }
        ok("audit report written: " + json_report.generic_string());
    }
    if (!sarif_report.empty()) {
        if (!write_audit_sarif_report(root, sarif_report, collected)) {
            return err("failed to write audit SARIF report: " + sarif_report.generic_string());
        }
        ok("audit SARIF report written: " + sarif_report.generic_string());
    }
    if (findings) return err("audit found " + std::to_string(findings) + " risky pattern(s)");
    ok("audit passed");
    return 0;
}

static int cmd_audit(int argc, char* argv[]) {
    std::string source;
    fs::path json_report;
    fs::path sarif_report;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argc) return err("audit --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("audit --json requires an output path");
        } else if (arg == "--sarif") {
            if (i + 1 >= argc) return err("audit --sarif requires an output path");
            sarif_report = fs::path(argv[++i]);
        } else if (arg.rfind("--sarif=", 0) == 0) {
            sarif_report = fs::path(arg.substr(8));
            if (sarif_report.empty()) return err("audit --sarif requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg audit [path] [--json report.json] [--sarif report.sarif]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("audit accepts at most one path");
        }
    }
    return run_audit_command(source, json_report, sarif_report);
}

struct LintFinding {
    std::string severity;
    std::string kind;
    fs::path file;
    int line = 0;
    std::string message;
};

struct LintReport {
    int files_checked = 0;
    int warnings = 0;
    int errors = 0;
    std::vector<LintFinding> findings;
};

static std::string lint_report_file_path(const fs::path& root, const fs::path& file) {
    if (file.empty()) return "";
    std::error_code ec;
    fs::path rel = fs::relative(file, root, ec);
    if (!ec && !rel.empty() && !path_contains_parent_ref(rel)) return rel.generic_string();
    return file.generic_string();
}

static bool write_lint_json_report(const fs::path& root,
                                   const fs::path& report_path,
                                   const LintReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"root\": \"" << json_escape(root.generic_string()) << "\",\n";
    out << "  \"passed\": " << (report.errors == 0 ? "true" : "false") << ",\n";
    out << "  \"files_checked\": " << report.files_checked << ",\n";
    out << "  \"warning_count\": " << report.warnings << ",\n";
    out << "  \"error_count\": " << report.errors << ",\n";
    out << "  \"findings\": [\n";
    for (size_t i = 0; i < report.findings.size(); ++i) {
        const auto& item = report.findings[i];
        out << "    {";
        out << "\"severity\":\"" << json_escape(item.severity) << "\",";
        out << "\"kind\":\"" << json_escape(item.kind) << "\",";
        out << "\"message\":\"" << json_escape(item.message) << "\",";
        out << "\"file\":\"" << json_escape(lint_report_file_path(root, item.file)) << "\",";
        out << "\"line\":" << item.line;
        out << "}";
        if (i + 1 < report.findings.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return write_all(report_path, out.str());
}

static LintReport lint_package_sources(const fs::path& root, bool verbose) {
    LintReport report;
    std::vector<fs::path> files = package_sura_files(root);
    report.files_checked = (int)files.size();

    const std::vector<std::pair<std::string, std::string>> risky = {
        {"network access", "(http_(get|post|json|request(_retry_json_checked|_json_checked|_retry_json|_full|_retry|_json)?|serve_static|serve_routes)|http\\.(get|post|json|request(_retry_json_checked|_json_checked|_retry_json|_full|_retry|_json)?|serve_static|serve_routes))\\s*\\("},
        {"file deletion", "(file_delete|file_remove_tree|remove_tree)\\s*\\(|fs\\.(delete|remove|remove_tree|delete_tree)\\s*\\("},
        {"python bridge", "python_[A-Za-z_]*\\s*\\("},
        {"native ffi/plugin", "(ffi_|plugin_|c_call|load_library)"},
        {"shell execution", "\\b(async_cmd|cmd_run(?:_checked)?|task)\\s*\\(|\\b(os\\.run(?:_checked)?|os\\.cmd|async\\.cmd)\\s*\\("}
    };
    std::regex block_close("^\\s*end\\b");
    std::regex if_open("^\\s*if\\b.*\\bthen\\s*$");
    std::regex loop_open("^\\s*(while|for|foreach|repeat)\\b.*\\bdo\\s*$");
    std::regex func_open("^\\s*func\\b.*\\bdo\\s*$");
    std::regex decl_open("^\\s*(class|enum|try)\\b");
    std::regex assignment_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+(?:is|=)\\s*(.+)$");
    std::regex sensitive_header_source_re(
        "\\b(auth_bearer|auth_basic|headers_merge)\\s*\\(|"
        "\\bhttp\\.(auth_bearer|auth_basic|headers_merge)\\s*\\(|"
        "\\b(authorization|proxy-authorization|cookie|set-cookie|x-api-key|api-key|x-auth-token|x-csrf-token|x-xsrf-token|token|secret|api[-_]?key)\\b",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    std::regex output_re("^\\s*(print(?:_n)?\\b|log_(debug|info|warn|error)\\s*\\(|log\\.(debug|info|warn|error|event)\\s*\\()");
    std::regex empty_tool_policy_re("^\\s*\\{\\s*\\}\\s*$");
    std::regex policy_literal_re("^\\s*\\{[^\\n]*\\}\\s*$");
    std::regex http_tool_policy_re("\\btools\\b[^\\n}]*http_(get|request)|http_(get|request)[^\\n}]*\\btools\\b",
                                   std::regex_constants::ECMAScript | std::regex_constants::icase);
    std::regex weak_tool_policy_call_re("\\b(tool_call_policy|tool\\.call_policy)\\s*\\(.*?,\\s*(\\{[^\\n]*\\}|[A-Za-z_][A-Za-z0-9_]*)\\s*\\)");
    std::regex legacy_command_re("^\\s*(print|print_n|print_no_nl|assert|assert_eq|assert_ne|assert_neq|assert_type|assert_len|assert_between|assert_approx|input|exit|clock|type|random)\\s+[^\\s(]");
    auto trim_for_lint = [](std::string text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                                 text.front() == '\r' || text.front() == '\n')) {
            text.erase(text.begin());
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                                 text.back() == '\r' || text.back() == '\n')) {
            text.pop_back();
        }
        return text;
    };

    auto add_finding = [&](const std::string& severity,
                           const std::string& kind,
                           const fs::path& file,
                           int line,
                           const std::string& message) {
        if (severity == "error") ++report.errors;
        else ++report.warnings;
        report.findings.push_back({severity, kind, file, line, message});
        if (verbose) {
            std::cout << "[lint] " << severity << " " << message << "\n";
        }
    };

    for (const auto& file : files) {
        std::istringstream lines(read_all(file));
        std::string line;
        int line_no = 0;
        int depth = 0;
        std::unordered_set<std::string> sensitive_header_vars;
        std::unordered_set<std::string> weak_tool_policy_vars;
        auto has_header_redaction = [](const std::string& text) {
            return text.find("headers_redact") != std::string::npos;
        };
        auto has_sensitive_header_source = [&](const std::string& text) {
            return !has_header_redaction(text) && std::regex_search(text, sensitive_header_source_re);
        };
        auto references_sensitive_header_var = [&](const std::string& text) {
            for (const auto& name : sensitive_header_vars) {
                if (std::regex_search(text, std::regex("\\b" + name + "\\b"))) return true;
            }
            return false;
        };
        auto is_weak_tool_policy_text = [&](const std::string& value) {
            if (!std::regex_match(value, policy_literal_re)) return false;
            if (std::regex_match(value, empty_tool_policy_re)) return true;
            if (value.find("url_prefixes") != std::string::npos) return false;
            if (std::regex_search(value, http_tool_policy_re)) return true;
            return false;
        };
        auto is_weak_tool_policy_arg = [&](const std::string& value) {
            if (is_weak_tool_policy_text(value)) return true;
            return weak_tool_policy_vars.count(value) > 0;
        };
        while (std::getline(lines, line)) {
            ++line_no;
            // getline keeps the carriage return of a CRLF file. Rules that
            // match a whole line (assignments, for one) then never fire, so a
            // file saved on Windows would silently lose findings that the same
            // file with LF endings reports.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (std::regex_search(line, block_close)) {
                --depth;
                if (depth < 0) {
                    add_finding("error", "unmatched_end", file, line_no,
                                "unmatched end at " + file.generic_string() + ":" + std::to_string(line_no));
                    depth = 0;
                }
            }

            for (const auto& rule : risky) {
                if (std::regex_search(line, std::regex(rule.second))) {
                    add_finding("warning", "risky_api", file, line_no,
                                "risky " + rule.first + " at " +
                                file.generic_string() + ":" + std::to_string(line_no));
                }
            }

            std::smatch assign_match;
            if (std::regex_match(line, assign_match, assignment_re)) {
                std::string name = assign_match[1].str();
                std::string value = assign_match[2].str();
                if (has_header_redaction(value)) {
                    sensitive_header_vars.erase(name);
                } else if (has_sensitive_header_source(value) || references_sensitive_header_var(value)) {
                    sensitive_header_vars.insert(name);
                }
                if (is_weak_tool_policy_text(value)) {
                    weak_tool_policy_vars.insert(name);
                } else {
                    weak_tool_policy_vars.erase(name);
                }
            }
            if (!has_header_redaction(line) &&
                std::regex_search(line, output_re) &&
                (has_sensitive_header_source(line) || references_sensitive_header_var(line))) {
                add_finding("warning", "unredacted_sensitive_headers", file, line_no,
                            "risky unredacted sensitive headers at " +
                            file.generic_string() + ":" + std::to_string(line_no) +
                            " (use headers_redact before logging)");
            }
            std::smatch weak_policy_match;
            if (std::regex_search(line, weak_policy_match, weak_tool_policy_call_re) &&
                weak_policy_match.size() >= 3 &&
                is_weak_tool_policy_arg(weak_policy_match[2].str())) {
                add_finding("warning", "weak_tool_policy", file, line_no,
                            "risky weak tool policy at " +
                            file.generic_string() + ":" + std::to_string(line_no) +
                            " (restrict tools, url_prefixes, http_methods, and allow_shell)");
            }
            if (std::regex_search(line, legacy_command_re)) {
                add_finding("warning", "legacy_command_syntax", file, line_no,
                            "legacy command syntax at " +
                            file.generic_string() + ":" + std::to_string(line_no) +
                            " (use function-call syntax such as print(...))");
            }

            std::string trimmed = trim_for_lint(line);
            if (std::regex_search(trimmed, if_open) ||
                std::regex_search(trimmed, loop_open) ||
                std::regex_search(trimmed, func_open) ||
                std::regex_search(trimmed, decl_open)) {
                ++depth;
            }
        }
        if (depth != 0) {
            add_finding("error", "unclosed_block", file, line_no,
                        "unclosed block depth " + std::to_string(depth) +
                        " in " + file.generic_string());
        }
    }
    return report;
}

static int run_lint_command(const std::string& source, const fs::path& json_report, bool fail_on_warning) {
    fs::path root = source.empty() ? fs::current_path() : fs::path(source);
    if (!fs::exists(root)) return err("lint path not found: " + root.generic_string());
    LintReport report = lint_package_sources(root, true);
    if (report.files_checked == 0) return err("no Sura files found under " + root.generic_string());
    if (!json_report.empty()) {
        if (!write_lint_json_report(root, json_report, report)) {
            return err("failed to write lint JSON report: " + json_report.generic_string());
        }
        ok("lint report written: " + json_report.generic_string());
    }
    std::cout << "Sura lint: " << report.files_checked << " checked, "
              << report.errors << " error(s), " << report.warnings << " warning(s)\n";
    if (report.errors > 0) return err("lint found " + std::to_string(report.errors) + " error(s)");
    if (fail_on_warning && report.warnings > 0)
        return err("lint found " + std::to_string(report.warnings) + " warning(s)");
    ok("lint passed");
    return 0;
}

static int cmd_lint_path(const std::string& source) {
    return run_lint_command(source, fs::path(), false);
}

static int cmd_lint(int argc, char* argv[]) {
    std::string source;
    fs::path json_report;
    bool fail_on_warning = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argc) return err("lint --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("lint --json requires an output path");
        } else if (arg == "--fail-on-warning" || arg == "--fail-on-warnings") {
            fail_on_warning = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg lint [path] [--json report.json] [--fail-on-warning]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("lint accepts at most one path");
        }
    }
    return run_lint_command(source, json_report, fail_on_warning);
}

static std::string test_engine_path();

struct PackageCheckResult {
    fs::path file;
    std::string status;
    int code = 0;
    long long ms = 0;
    std::string output;
};

static int run_check_file(const std::string& engine,
                          const fs::path& file,
                          bool strict,
                          std::string& output,
                          long long& ms) {
    fs::path tmp = fs::temp_directory_path() /
        ("sura_check_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    auto start = std::chrono::steady_clock::now();
    std::string cmd;
#ifdef _WIN32
    cmd = "call " + shell_quote(engine);
#else
    cmd = shell_quote(engine);
#endif
    cmd += strict ? " --strict" : " --legacy-types";
    cmd += " --check " + shell_quote(path_to_utf8(file)) +
           " > " + shell_quote(path_to_utf8(tmp)) + " 2>&1";
    int code = run_system_command_status(cmd);
    auto end = std::chrono::steady_clock::now();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    output = read_all(tmp);
    std::error_code ec;
    fs::remove(tmp, ec);
    return code;
}

static bool write_check_json_report(const fs::path& root,
                                    const fs::path& report_path,
                                    bool strict,
                                    int passed,
                                    int failed,
                                    const std::vector<PackageCheckResult>& results,
                                    const std::string& engine) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"root\": \"" << json_escape(root.generic_string()) << "\",\n";
    out << "  \"engine\": \"" << json_escape(engine) << "\",\n";
    out << "  \"strict\": " << (strict ? "true" : "false") << ",\n";
    out << "  \"passed\": " << passed << ",\n";
    out << "  \"failed\": " << failed << ",\n";
    out << "  \"files\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\"path\":\"" << json_escape(r.file.generic_string())
            << "\",\"status\":\"" << json_escape(r.status)
            << "\",\"exitCode\":" << r.code
            << ",\"durationMs\":" << r.ms
            << ",\"output\":\"" << json_escape(r.output) << "\"}"
            << (i + 1 == results.size() ? "\n" : ",\n");
    }
    out << "  ]\n";
    out << "}\n";
    return write_all(report_path, out.str());
}

static int run_check_command(const std::string& source, const fs::path& json_report, bool strict) {
    fs::path root = source.empty() ? fs::current_path() : fs::path(source);
    if (!fs::exists(root)) return err("check path not found: " + root.generic_string());
    std::vector<fs::path> files = package_sura_files(root);
    if (files.empty()) return err("no Sura files found under " + root.generic_string());

    fs::path display_root = root;
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) display_root = root.parent_path();
    std::string engine = test_engine_path();
    int passed = 0;
    int failed = 0;
    std::vector<PackageCheckResult> results;

    for (const auto& file : files) {
        std::string output;
        long long ms = 0;
        int code = run_check_file(engine, file, strict, output, ms);
        std::error_code rel_ec;
        fs::path display = fs::relative(file, display_root, rel_ec);
        if (rel_ec) display = file;
        if (code == 0) {
            ++passed;
            std::cout << "[PASS] " << display.generic_string() << " (" << ms << " ms)\n";
            results.push_back({display, "pass", code, ms, output});
        } else {
            ++failed;
            std::cout << "[FAIL] " << display.generic_string() << " (" << ms << " ms)\n";
            if (!output.empty()) std::cout << output;
            results.push_back({display, "fail", code, ms, output});
        }
    }

    if (!json_report.empty()) {
        if (!write_check_json_report(root, json_report, strict, passed, failed, results, engine)) {
            return err("failed to write check JSON report: " + json_report.generic_string());
        }
        ok("check report written: " + json_report.generic_string());
    }

    std::cout << "Sura check: " << passed << " passed, " << failed << " failed\n";
    return failed ? err("check failed") : 0;
}

static int cmd_check_path(const std::string& source) {
    return run_check_command(source, fs::path(), true);
}

static int cmd_check(int argc, char* argv[]) {
    std::string source;
    fs::path json_report;
    bool strict = true;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argc) return err("check --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("check --json requires an output path");
        } else if (arg == "--strict") {
            strict = true;
        } else if (arg == "--legacy-types") {
            strict = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg check [path] [--json report.json] [--strict|--legacy-types]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("check accepts at most one path");
        }
    }
    return run_check_command(source, json_report, strict);
}

struct PackageFormatResult {
    fs::path file;
    std::string status;
    int code = 0;
    long long ms = 0;
    std::string output;
};

static int run_format_file(const std::string& engine,
                           const fs::path& file,
                           bool check_only,
                           std::string& output,
                           long long& ms) {
    fs::path tmp = fs::temp_directory_path() /
        ("sura_format_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    auto start = std::chrono::steady_clock::now();
    std::string cmd;
#ifdef _WIN32
    cmd = "call " + shell_quote(engine);
#else
    cmd = shell_quote(engine);
#endif
    cmd += check_only ? " --format-check " : " --format ";
    cmd += shell_quote(path_to_utf8(file)) +
           " > " + shell_quote(path_to_utf8(tmp)) + " 2>&1";
    int code = run_system_command_status(cmd);
    auto end = std::chrono::steady_clock::now();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    output = read_all(tmp);
    std::error_code ec;
    fs::remove(tmp, ec);
    return code;
}

static bool write_format_json_report(const fs::path& root,
                                     const fs::path& report_path,
                                     bool check_only,
                                     int formatted,
                                     int unchanged,
                                     int failed,
                                     const std::vector<PackageFormatResult>& results,
                                     const std::string& engine) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"root\": \"" << json_escape(root.generic_string()) << "\",\n";
    out << "  \"engine\": \"" << json_escape(engine) << "\",\n";
    out << "  \"check\": " << (check_only ? "true" : "false") << ",\n";
    out << "  \"passed\": " << (failed == 0 ? "true" : "false") << ",\n";
    out << "  \"formatted\": " << formatted << ",\n";
    out << "  \"unchanged\": " << unchanged << ",\n";
    out << "  \"failed\": " << failed << ",\n";
    out << "  \"files\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\"path\":\"" << json_escape(r.file.generic_string())
            << "\",\"status\":\"" << json_escape(r.status)
            << "\",\"exitCode\":" << r.code
            << ",\"durationMs\":" << r.ms
            << ",\"output\":\"" << json_escape(r.output) << "\"}"
            << (i + 1 == results.size() ? "\n" : ",\n");
    }
    out << "  ]\n";
    out << "}\n";
    return write_all(report_path, out.str());
}

static int run_format_command(const std::string& source, const fs::path& json_report, bool check_only) {
    fs::path root = source.empty() ? fs::current_path() : fs::path(source);
    if (!fs::exists(root)) return err("format path not found: " + root.generic_string());
    std::vector<fs::path> files = package_sura_files(root);
    if (files.empty()) return err("no Sura files found under " + root.generic_string());

    fs::path display_root = root;
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) display_root = root.parent_path();
    std::string engine = test_engine_path();
    int formatted = 0;
    int unchanged = 0;
    int failed = 0;
    std::vector<PackageFormatResult> results;

    for (const auto& file : files) {
        std::string output;
        long long ms = 0;
        int code = run_format_file(engine, file, check_only, output, ms);
        std::error_code rel_ec;
        fs::path display = fs::relative(file, display_root, rel_ec);
        if (rel_ec) display = file;

        std::string status;
        if (code != 0) {
            ++failed;
            status = "fail";
            std::cout << "[FAIL] " << display.generic_string() << " (" << ms << " ms)\n";
            if (!output.empty()) std::cout << output;
        } else if (check_only || output.find("1 unchanged") != std::string::npos ||
                   output.find("format check: 1 passed") != std::string::npos) {
            ++unchanged;
            status = "unchanged";
            std::cout << "[OK] " << display.generic_string() << " (" << ms << " ms)\n";
        } else {
            ++formatted;
            status = "formatted";
            std::cout << "[FORMAT] " << display.generic_string() << " (" << ms << " ms)\n";
        }
        results.push_back({display, status, code, ms, output});
    }

    if (!json_report.empty()) {
        if (!write_format_json_report(root, json_report, check_only, formatted, unchanged, failed, results, engine)) {
            return err("failed to write format JSON report: " + json_report.generic_string());
        }
        ok("format report written: " + json_report.generic_string());
    }

    if (check_only) {
        std::cout << "Sura format check: " << unchanged << " passed, " << failed << " failed\n";
    } else {
        std::cout << "Sura format: " << formatted << " formatted, " << unchanged
                  << " unchanged, " << failed << " failed\n";
    }
    return failed ? err(check_only ? "format check failed" : "format failed") : 0;
}

static int cmd_format_check_path(const std::string& source) {
    return run_format_command(source, fs::path(), true);
}

static int cmd_format(int argc, char* argv[]) {
    std::string source;
    fs::path json_report;
    bool check_only = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argc) return err("format --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("format --json requires an output path");
        } else if (arg == "--check" || arg == "--format-check") {
            check_only = true;
        } else if (arg == "--write") {
            check_only = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg format [path] [--check] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("format accepts at most one path");
        }
    }
    return run_format_command(source, json_report, check_only);
}

static std::string tool_policy_prefix_from_url(std::string url) {
    if (url.rfind("file://", 0) == 0) return "file://";
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    size_t host_start = scheme + 3;
    size_t slash = url.find('/', host_start);
    if (slash == std::string::npos) return url;
    return url.substr(0, slash + 1);
}

static std::string json_array_from_set(const std::set<std::string>& values) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& value : values) {
        if (!first) out << ", ";
        first = false;
        out << "\"" << json_escape(value) << "\"";
    }
    out << "]";
    return out.str();
}

static bool write_policy_json_report(const fs::path& report_path,
                                     const fs::path& root,
                                     const fs::path& policy_path,
                                     const std::set<std::string>& tools,
                                     const std::set<std::string>& url_prefixes,
                                     const std::set<std::string>& http_methods,
                                     const std::set<std::string>& allowed_headers,
                                     bool shell_seen,
                                     bool passed) {
    std::string manifest = read_all(root / kManifest);
    std::string name = manifest_field(manifest, "name", root.filename().string());
    std::string version = manifest_field(manifest, "version", "0.0.0");
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.policy.v1\",\n"
        << "  \"package\": \"" << json_escape(name) << "\",\n"
        << "  \"version\": \"" << json_escape(version) << "\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"root\": \"" << json_escape(root.generic_string()) << "\",\n"
        << "  \"policy\": \"" << json_escape(policy_path.generic_string()) << "\",\n"
        << "  \"tools\": " << json_array_from_set(tools) << ",\n"
        << "  \"url_prefixes\": " << json_array_from_set(url_prefixes) << ",\n"
        << "  \"http_methods\": " << json_array_from_set(http_methods) << ",\n"
        << "  \"allowed_headers\": " << json_array_from_set(allowed_headers) << ",\n"
        << "  \"shell_seen\": " << (shell_seen ? "true" : "false") << ",\n"
        << "  \"requires_manual_command_prefixes\": " << (shell_seen ? "true" : "false") << "\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_policy_command(const std::string& source, const fs::path& json_report) {
    fs::path root = doctor_package_root(source);
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return err("sura.pkg.json not found for policy generation: " + (root / kManifest).generic_string());
    fs::path policy_path = root / kToolPolicyManifest;
    if (fs::exists(policy_path)) return err("tool policy already exists: " + policy_path.generic_string());

    std::set<std::string> tools;
    std::set<std::string> url_prefixes;
    std::set<std::string> http_methods;
    std::set<std::string> allowed_headers;
    bool shell_seen = false;

    std::regex tool_spec_re("\\b(?:tool_spec|tool\\.spec)\\s*\\(\\s*[\"'](http_get|http_request|shell)[\"']");
    std::regex raw_name_re("\\bname\\s*[:=]\\s*[\"'](http_get|http_request|shell)[\"']");
    std::regex method_re("\\bmethod\\s*[:=]\\s*[\"']([A-Za-z]+)[\"']");
    std::regex url_re("[\"']((?:https?://|file://)[^\"'\\s,}\\)]*)[\"']");
    std::regex header_re("\"([A-Za-z][A-Za-z0-9_-]*)\"\\s*:");

    for (const auto& file : package_sura_files(root)) {
        std::istringstream lines(read_all(file));
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::smatch match;
            bool policy_line = line.find("tool_spec") != std::string::npos ||
                               line.find("tool.spec") != std::string::npos ||
                               line.find("tool_call_policy") != std::string::npos ||
                               line.find("tool.call_policy") != std::string::npos ||
                               line.find("llm_run_tools") != std::string::npos ||
                               line.find("llm.run_tools") != std::string::npos;
            if (std::regex_search(line, match, tool_spec_re) ||
                (policy_line && std::regex_search(line, match, raw_name_re))) {
                std::string tool = match[1].str();
                if (tool == "shell") shell_seen = true;
                else tools.insert(tool);
            }
            if (line.find("http_get") != std::string::npos && policy_line) tools.insert("http_get");
            if (line.find("http_request") != std::string::npos && policy_line) tools.insert("http_request");
            if (std::regex_search(line, match, method_re)) {
                std::string method = match[1].str();
                std::transform(method.begin(), method.end(), method.begin(),
                               [](unsigned char c) { return (char)std::toupper(c); });
                http_methods.insert(method);
            }
            for (auto it = std::sregex_iterator(line.begin(), line.end(), url_re);
                 it != std::sregex_iterator(); ++it) {
                std::string prefix = tool_policy_prefix_from_url(json_unescape((*it)[1].str()));
                if (!prefix.empty()) url_prefixes.insert(prefix);
            }
            if (line.find("headers") != std::string::npos) {
                for (auto it = std::sregex_iterator(line.begin(), line.end(), header_re);
                     it != std::sregex_iterator(); ++it) {
                    std::string header = json_unescape((*it)[1].str());
                    if (header != "headers") allowed_headers.insert(header);
                }
            }
        }
    }

    if (shell_seen) {
        return err("shell tool policy requires manual command_prefixes; create " + policy_path.generic_string() + " explicitly");
    }
    if (tools.empty()) return err("no policy-aware http_get/http_request tool specs found under " + root.generic_string());
    if (url_prefixes.empty() && (tools.count("http_get") || tools.count("http_request"))) url_prefixes.insert("file://");
    if (tools.count("http_request") && http_methods.empty()) http_methods.insert("GET");

    std::ostringstream out;
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"tools\": " << json_array_from_set(tools) << ",\n"
        << "  \"url_prefixes\": " << json_array_from_set(url_prefixes);
    if (tools.count("http_request")) {
        out << ",\n  \"http_methods\": " << json_array_from_set(http_methods);
        if (!allowed_headers.empty()) out << ",\n  \"allowed_headers\": " << json_array_from_set(allowed_headers);
        out << ",\n  \"max_timeout\": 60,\n"
            << "  \"max_body_bytes\": 1048576";
    }
    out << ",\n"
        << "  \"approval\": false,\n"
        << "  \"allow_shell\": false,\n"
        << "  \"command_prefixes\": []\n"
        << "}\n";

    if (!write_all(policy_path, out.str())) return err("failed to write " + policy_path.generic_string());
    if (!json_report.empty()) {
        if (!write_policy_json_report(json_report, root, policy_path, tools, url_prefixes, http_methods, allowed_headers, shell_seen, true)) {
            return err("failed to write policy JSON report: " + json_report.generic_string());
        }
        ok("policy report written: " + json_report.generic_string());
    }
    ok("wrote tool policy -> " + policy_path.generic_string());
    if (audit_tool_manifest(policy_path, true) != 0) return err("generated tool policy failed audit");
    info("review url_prefixes and sign with surapkg sign-policy before release");
    return 0;
}

static int cmd_policy(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("policy --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("policy --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage:\n  surapkg policy [path] [--json report.json]\n";
            std::cout << "Scans package tool_spec/tool.spec calls and writes a starter sura.tools.json policy.\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("policy accepts at most one path and optional --json");
        }
    }
    return run_policy_command(source, json_report);
}

struct ToolLogEntry {
    int line = 0;
    int source_line = 0;
    std::string ts;
    std::string event;
    std::string tool;
    std::string target;
    std::string reason;
    bool approval_required = false;
    bool approval_token_configured = false;
};

static bool tool_log_bad_event(const std::string& event) {
    return event == "policy_denied" || event == "approval_denied" || event == "execution_failed";
}

static bool write_tool_log_json_object(std::ostream& out, const std::map<std::string, int>& values,
                                       const std::string& indent) {
    out << "{";
    if (!values.empty()) out << "\n";
    size_t i = 0;
    for (const auto& item : values) {
        out << indent << "  \"" << json_escape(item.first) << "\": " << item.second;
        if (++i < values.size()) out << ",";
        out << "\n";
    }
    out << indent << "}";
    return true;
}

static bool write_tool_log_json_report(const fs::path& report_path,
                                       const fs::path& log_path,
                                       int tail,
                                       bool fail_on_denied,
                                       int malformed,
                                       int bad,
                                       int approval_required,
                                       int approval_token_configured,
                                       const std::map<std::string, int>& event_counts,
                                       const std::map<std::string, int>& tool_counts,
                                       const std::vector<ToolLogEntry>& entries,
                                       bool passed) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.tool_log.summary.v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"log\": \"" << json_escape(path_to_generic_utf8(log_path)) << "\",\n"
        << "  \"event_count\": " << entries.size() << ",\n"
        << "  \"bad_event_count\": " << bad << ",\n"
        << "  \"denied_or_failed\": " << (bad > 0 ? "true" : "false") << ",\n"
        << "  \"malformed\": " << (malformed > 0 ? "true" : "false") << ",\n"
        << "  \"malformed_lines\": " << malformed << ",\n"
        << "  \"fail_on_denied\": " << (fail_on_denied ? "true" : "false") << ",\n"
        << "  \"tail\": " << tail << ",\n"
        << "  \"approval_required_events\": " << approval_required << ",\n"
        << "  \"approval_token_configured_events\": " << approval_token_configured << ",\n"
        << "  \"counts\": ";
    write_tool_log_json_object(out, event_counts, "  ");
    out << ",\n"
        << "  \"tool_counts\": ";
    write_tool_log_json_object(out, tool_counts, "  ");
    out << ",\n"
        << "  \"recent_events\": [\n";
    size_t start = 0;
    if (tail > 0 && entries.size() > (size_t)tail) start = entries.size() - (size_t)tail;
    if (tail <= 0) start = entries.size();
    for (size_t i = start; i < entries.size(); ++i) {
        const auto& e = entries[i];
        out << "    {"
            << "\"line\":" << e.line
            << ",\"source_line\":" << e.source_line
            << ",\"ts\":\"" << json_escape(e.ts) << "\""
            << ",\"event\":\"" << json_escape(e.event) << "\""
            << ",\"tool\":\"" << json_escape(e.tool) << "\""
            << ",\"target\":\"" << json_escape(e.target) << "\""
            << ",\"reason\":\"" << json_escape(e.reason) << "\""
            << ",\"approval_required\":" << (e.approval_required ? "true" : "false")
            << ",\"approval_token_configured\":" << (e.approval_token_configured ? "true" : "false")
            << "}";
        if (i + 1 < entries.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int cmd_tool_log(const std::vector<std::string>& argv) {
    if (argv.size() < 3 || argv[2] == "--help" || argv[2] == "-h") {
        std::cout << "Usage:\n  surapkg tool-log <tool-audit.jsonl> [--tail n] [--fail-on-denied] [--json report.json]\n";
        return argv.size() < 3 ? 1 : 0;
    }

    fs::path log_path = utf8_path(argv[2]);
    fs::path json_report;
    int tail = 8;
    bool fail_on_denied = false;
    for (size_t i = 3; i < argv.size(); ++i) {
        std::string arg = argv[i];
        if (arg == "--fail-on-denied" || arg == "--fail-on-blocked") {
            fail_on_denied = true;
        } else if (arg == "--tail") {
            if (i + 1 >= argv.size()) return err("--tail requires a value");
            try {
                tail = std::max(0, std::stoi(argv[++i]));
            } catch (...) {
                return err("--tail requires an integer");
            }
        } else if (arg.rfind("--tail=", 0) == 0) {
            try {
                tail = std::max(0, std::stoi(arg.substr(7)));
            } catch (...) {
                return err("--tail requires an integer");
            }
        } else if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("tool-log --json requires an output path");
            json_report = utf8_path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = utf8_path(arg.substr(7));
            if (json_report.empty()) return err("tool-log --json requires an output path");
        } else {
            return err("unknown tool-log option: " + arg);
        }
    }

    std::ifstream in(log_path, std::ios::binary);
    if (!in) return err("tool audit log not found: " + path_to_generic_utf8(log_path));

    std::vector<ToolLogEntry> entries;
    std::map<std::string, int> event_counts;
    std::map<std::string, int> tool_counts;
    int malformed = 0;
    int bad = 0;
    int approval_required = 0;
    int approval_token_configured = 0;

    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (trim_copy(line).empty()) continue;
        ToolLogEntry entry;
        entry.line = line_no;
        entry.ts = manifest_string_field(line, "ts", "");
        entry.event = manifest_string_field(line, "event", "");
        entry.tool = manifest_string_field(line, "tool", "");
        entry.target = manifest_string_field(line, "target", "");
        entry.reason = manifest_string_field(line, "reason", "");
        std::string source_line;
        if (json_number_field_text(line, "line", source_line)) {
            try {
                entry.source_line = std::stoi(source_line);
            } catch (...) {
                entry.source_line = 0;
            }
        }
        bool found = false;
        entry.approval_required = manifest_bool_field(line, "approvalRequired", false, &found);
        if (entry.approval_required) ++approval_required;
        entry.approval_token_configured = manifest_bool_field(line, "approvalTokenConfigured", false, &found);
        if (entry.approval_token_configured) ++approval_token_configured;

        if (entry.event.empty() || entry.tool.empty()) {
            ++malformed;
            continue;
        }
        ++event_counts[entry.event];
        ++tool_counts[entry.tool];
        if (tool_log_bad_event(entry.event)) ++bad;
        entries.push_back(entry);
    }

    std::cout << "Sura tool audit log\n";
    std::cout << "file: " << path_to_generic_utf8(log_path) << "\n";
    std::cout << "events: " << entries.size() << "\n";
    std::cout << "denied_or_failed: " << bad << "\n";
    std::cout << "approval_required_events: " << approval_required << "\n";
    std::cout << "approval_token_configured_events: " << approval_token_configured << "\n";
    if (malformed) std::cout << "malformed_lines: " << malformed << "\n";

    std::cout << "by_event:\n";
    for (const auto& [event, count] : event_counts) {
        std::cout << "  " << event << ": " << count << "\n";
    }
    std::cout << "by_tool:\n";
    for (const auto& [tool, count] : tool_counts) {
        std::cout << "  " << tool << ": " << count << "\n";
    }

    if (tail > 0 && !entries.empty()) {
        std::cout << "recent:\n";
        size_t start = entries.size() > (size_t)tail ? entries.size() - (size_t)tail : 0;
        for (size_t i = start; i < entries.size(); ++i) {
            const auto& e = entries[i];
            std::cout << "  " << (e.ts.empty() ? "<no-ts>" : e.ts)
                      << " " << e.event << " " << e.tool;
            if (!e.target.empty()) std::cout << " target=" << e.target;
            if (!e.reason.empty()) std::cout << " reason=" << e.reason;
            std::cout << "\n";
        }
    }

    bool passed = malformed == 0 && !(fail_on_denied && bad > 0);
    if (!json_report.empty()) {
        if (!write_tool_log_json_report(json_report, log_path, tail, fail_on_denied, malformed, bad,
                                        approval_required, approval_token_configured, event_counts,
                                        tool_counts, entries, passed)) {
            return err("failed to write tool-log JSON report: " + path_to_generic_utf8(json_report));
        }
        ok("tool-log report written: " + path_to_generic_utf8(json_report));
    }
    if (malformed) return err("tool audit log contains malformed lines");
    if (fail_on_denied && bad > 0) return err("tool audit log contains denied or failed events");
    return 0;
}

struct PackageDocSymbol {
    std::string kind;
    std::string name;
    std::string signature;
    fs::path source;
    int line = 0;
};

struct PackageDocSearchEntry {
    std::string type;
    std::string kind;
    std::string name;
    std::string title;
    std::string text;
    fs::path source;
    int line = 0;
};

struct PackageDocAuditFinding {
    std::string kind;
    std::string message;
    std::string source;
    int line = 0;
};

static std::vector<PackageDocAuditFinding> package_doc_audit_findings(const std::string& report) {
    std::vector<PackageDocAuditFinding> findings;
    std::regex object_re("\\{[^{}]*\"kind\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"[^{}]*\\}");
    for (auto it = std::sregex_iterator(report.begin(), report.end(), object_re);
         it != std::sregex_iterator(); ++it) {
        std::string object = it->str();
        PackageDocAuditFinding finding;
        finding.kind = manifest_string_field(object, "kind", "");
        finding.message = manifest_string_field(object, "message", "");
        finding.source = manifest_string_field(object, "file", "");
        std::string line_text;
        if (json_number_field_text(object, "line", line_text)) {
            try {
                finding.line = std::max(0, std::stoi(line_text));
            } catch (...) {
                finding.line = 0;
            }
        }
        if (!finding.kind.empty() || !finding.message.empty()) findings.push_back(finding);
    }
    return findings;
}

static fs::path package_gate_audit_report_path(const fs::path& root, const std::string& phase) {
    return root / "artifacts" / (phase + "-audit.json");
}

static fs::path package_gate_test_report_path(const fs::path& root, const std::string& phase) {
    return root / "artifacts" / (phase + "-test.json");
}

static fs::path package_gate_quality_report_path(const fs::path& root, const std::string& phase) {
    return root / "artifacts" / (phase + "-quality.json");
}

static fs::path package_gate_protect_verify_report_path(const fs::path& root, const std::string& phase) {
    return root / "artifacts" / (phase + "-protect-verify.json");
}

static bool generate_package_docs(const fs::path& root,
                                  const fs::path& out_dir,
                                  fs::path& index_path,
                                  const fs::path& quality_report_override = fs::path(),
                                  const fs::path& audit_report_override = fs::path(),
                                  const fs::path& test_report_override = fs::path()) {
    std::string manifest = read_all(root / kManifest);
    std::string name = manifest_field(manifest, "name", project_name_from_cwd());
    std::string version = manifest_field(manifest, "version", "0.0.0");
    auto join_values = [](const std::vector<std::string>& values) {
        if (values.empty()) return std::string("<em>none</em>");
        std::ostringstream out;
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) out << ", ";
            out << "<code>" << html_escape(values[i]) << "</code>";
        }
        return out.str();
    };
    auto format_bench_number = [](double value) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(3) << value;
        return out.str();
    };
    std::vector<PackageDocSymbol> symbols;
    std::regex func_re("^\\s*func\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*(\\([^)]*\\))?\\s*(?:->\\s*([^\\s]+))?\\s*do\\b");
    std::regex type_re("^\\s*(class|struct|enum)\\s+([A-Za-z_][A-Za-z0-9_]*)(?:\\s+extends\\s+([A-Za-z_][A-Za-z0-9_]*))?\\s+do\\b");
    std::regex constant_re("^\\s*([A-Za-z_][A-Za-z0-9_]*)\\s+is\\s+(.+?)\\s*$");
    std::regex literal_re("^([-+]?[0-9]+(\\.[0-9]+)?|\"(\\\\.|[^\"])*\"|true|false|nil)$");
    std::regex block_open_re("\\bdo\\s*$");
    for (const auto& file : package_sura_files(root)) {
        std::istringstream lines(read_all(file));
        std::string line;
        int line_no = 0;
        int block_depth = 0;
        while (std::getline(lines, line)) {
            ++line_no;
            // getline keeps the carriage return of a CRLF file. Rules that
            // match a whole line (assignments, for one) then never fire, so a
            // file saved on Windows would silently lose findings that the same
            // file with LF endings reports.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::smatch m;
            std::error_code ec;
            fs::path display = fs::relative(file, root, ec);
            if (ec) display = file;
            std::string trimmed = trim_copy(line);
            if (std::regex_search(line, m, func_re)) {
                symbols.push_back({"function", m[1].str(), trimmed, display, line_no});
            } else if (std::regex_search(line, m, type_re)) {
                symbols.push_back({m[1].str(), m[2].str(), trimmed, display, line_no});
            } else if (block_depth == 0 && std::regex_match(line, m, constant_re)) {
                std::string value = trim_copy(m[2].str());
                if (std::regex_match(value, literal_re)) {
                    symbols.push_back({"constant", m[1].str(), trimmed, display, line_no});
                }
            }

            if (std::regex_search(trimmed, block_open_re)) ++block_depth;
            if (trimmed == "end" && block_depth > 0) --block_depth;
        }
    }
    std::vector<std::pair<std::string, std::vector<PackageInfoSymbol>>> stdlib_modules;
    size_t stdlib_symbol_count = 0;
    for (const auto& module_name : builtin_stdlib_module_names()) {
        auto module_symbols = builtin_stdlib_module_symbols(module_name);
        stdlib_symbol_count += module_symbols.size();
        stdlib_modules.push_back({module_name, module_symbols});
    }
    fs::create_directories(out_dir);
    std::string tool_manifest = read_all(root / kToolPolicyManifest);
    std::string plugin_policy_manifest = read_all(root / kPluginPolicyManifest);
    bool bench_report_field_found = false;
    std::string bench_report_ref = manifest_string_field(manifest, "bench_report", "", &bench_report_field_found);
    fs::path bench_report_display;
    std::string bench_report;
    bool bench_report_present = false;
    bool bench_speedup_found = false;
    bool bench_python_found = false;
    double bench_speedup = 0;
    double bench_python_ratio = 0;
    if (bench_report_field_found && !bench_report_ref.empty()) {
        fs::path bench_report_path = utf8_path(bench_report_ref);
        if (bench_report_path.is_relative()) bench_report_path = root / bench_report_path;
        bench_report = read_all(bench_report_path);
        if (!bench_report.empty()) {
            std::error_code bench_rel_ec;
            bench_report_display = fs::relative(bench_report_path, root, bench_rel_ec);
            if (bench_rel_ec) bench_report_display = bench_report_path;
            bench_report_present = true;
            bench_speedup = manifest_number_field(bench_report, "speedup", 0, &bench_speedup_found);
            bench_python_ratio = manifest_number_field(bench_report, "sura_faster_by_python", 0, &bench_python_found);
        }
    }
    bool test_report_field_found = false;
    std::string test_report_ref = manifest_string_field(manifest, "test_report", "", &test_report_field_found);
    fs::path test_report_display;
    std::string test_report;
    bool test_report_present = false;
    bool test_ok_found = false;
    bool test_ok = false;
    bool test_total_found = false;
    bool test_passed_found = false;
    bool test_failed_found = false;
    bool test_jit_found = false;
    bool test_jit = false;
    double test_total = 0;
    double test_passed = 0;
    double test_failed = 0;
    auto load_test_report = [&](fs::path test_report_path) {
        if (test_report_path.empty()) return false;
        if (test_report_path.is_relative()) test_report_path = root / test_report_path;
        test_report = read_all(test_report_path);
        if (test_report.empty()) return false;
        std::error_code test_rel_ec;
        test_report_display = fs::relative(test_report_path, root, test_rel_ec);
        if (test_rel_ec) test_report_display = test_report_path;
        test_report_present = true;
        test_ok = manifest_bool_field(test_report, "ok", false, &test_ok_found);
        test_total = manifest_number_field(test_report, "total", 0, &test_total_found);
        test_passed = manifest_number_field(test_report, "passed", 0, &test_passed_found);
        test_failed = manifest_number_field(test_report, "failed", 0, &test_failed_found);
        test_jit = manifest_bool_field(test_report, "jit", false, &test_jit_found);
        return true;
    };
    if (!test_report_override.empty()) {
        load_test_report(test_report_override);
    }
    if (!test_report_present && test_report_field_found && !test_report_ref.empty()) {
        load_test_report(utf8_path(test_report_ref));
    }
    if (!test_report_present) {
        std::vector<fs::path> test_report_candidates = {
            root / "artifacts" / "test-report.json",
            package_gate_test_report_path(root, "ci"),
            package_gate_test_report_path(root, "release"),
            root / "sura-test-report.json",
        };
        fs::path newest_report;
        fs::file_time_type newest_time;
        bool found_newest = false;
        for (const auto& candidate : test_report_candidates) {
            if (read_all(candidate).empty()) continue;
            std::error_code time_ec;
            fs::file_time_type modified = fs::last_write_time(candidate, time_ec);
            if (time_ec) continue;
            if (!found_newest || modified > newest_time) {
                found_newest = true;
                newest_time = modified;
                newest_report = candidate;
            }
        }
        if (found_newest) {
            load_test_report(newest_report);
        }
    }
    bool audit_report_field_found = false;
    std::string audit_report_ref = manifest_string_field(manifest, "audit_report", "", &audit_report_field_found);
    fs::path audit_report_display;
    std::string audit_report;
    bool audit_report_present = false;
    bool audit_passed_found = false;
    bool audit_passed = false;
    bool audit_finding_count_found = false;
    double audit_finding_count_number = 0;
    std::vector<PackageDocAuditFinding> audit_findings;
    auto load_audit_report = [&](fs::path audit_report_path) {
        if (audit_report_path.empty()) return false;
        if (audit_report_path.is_relative()) audit_report_path = root / audit_report_path;
        audit_report = read_all(audit_report_path);
        if (audit_report.empty()) return false;
        std::error_code audit_rel_ec;
        audit_report_display = fs::relative(audit_report_path, root, audit_rel_ec);
        if (audit_rel_ec) audit_report_display = audit_report_path;
        audit_report_present = true;
        audit_passed = manifest_bool_field(audit_report, "passed", false, &audit_passed_found);
        audit_finding_count_number = manifest_number_field(audit_report, "finding_count", 0, &audit_finding_count_found);
        audit_findings = package_doc_audit_findings(audit_report);
        return true;
    };
    if (!audit_report_override.empty()) {
        load_audit_report(audit_report_override);
    }
    if (!audit_report_present && audit_report_field_found && !audit_report_ref.empty()) {
        load_audit_report(utf8_path(audit_report_ref));
    }
    if (!audit_report_present) {
        std::vector<fs::path> audit_report_candidates = {
            root / "artifacts" / "audit-report.json",
            package_gate_audit_report_path(root, "ci"),
            package_gate_audit_report_path(root, "release"),
            root / "audit-report.json",
        };
        fs::path newest_report;
        fs::file_time_type newest_time;
        bool found_newest = false;
        for (const auto& candidate : audit_report_candidates) {
            if (read_all(candidate).empty()) continue;
            std::error_code time_ec;
            fs::file_time_type modified = fs::last_write_time(candidate, time_ec);
            if (time_ec) continue;
            if (!found_newest || modified > newest_time) {
                found_newest = true;
                newest_time = modified;
                newest_report = candidate;
            }
        }
        if (found_newest) {
            load_audit_report(newest_report);
        }
    }
    bool quality_report_field_found = false;
    std::string quality_report_ref = manifest_string_field(manifest, "quality_report", "", &quality_report_field_found);
    fs::path quality_report_display;
    std::string quality_report;
    bool quality_report_present = false;
    bool quality_score_found = false;
    bool quality_possible_found = false;
    bool quality_passed_found = false;
    bool quality_warnings_found = false;
    bool quality_errors_found = false;
    bool quality_grade_found = false;
    double quality_score = 0;
    double quality_possible = 0;
    bool quality_passed = false;
    double quality_warnings = 0;
    double quality_errors = 0;
    std::string quality_grade;
    auto load_quality_report = [&](fs::path quality_report_path) {
        if (quality_report_path.empty()) return false;
        if (quality_report_path.is_relative()) quality_report_path = root / quality_report_path;
        quality_report = read_all(quality_report_path);
        if (quality_report.empty()) return false;
        std::error_code quality_rel_ec;
        quality_report_display = fs::relative(quality_report_path, root, quality_rel_ec);
        if (quality_rel_ec) quality_report_display = quality_report_path;
        quality_report_present = true;
        quality_score = manifest_number_field(quality_report, "score", 0, &quality_score_found);
        quality_possible = manifest_number_field(quality_report, "possible", 0, &quality_possible_found);
        quality_grade = manifest_string_field(quality_report, "grade", "", &quality_grade_found);
        quality_passed = manifest_bool_field(quality_report, "passed", false, &quality_passed_found);
        quality_warnings = manifest_number_field(quality_report, "warnings", 0, &quality_warnings_found);
        quality_errors = manifest_number_field(quality_report, "errors", 0, &quality_errors_found);
        return true;
    };
    if (!quality_report_override.empty()) {
        load_quality_report(quality_report_override);
    }
    if (!quality_report_present && quality_report_field_found && !quality_report_ref.empty()) {
        load_quality_report(utf8_path(quality_report_ref));
    }
    if (!quality_report_present) {
        std::vector<fs::path> quality_report_candidates = {
            root / "artifacts" / "quality-report.json",
            package_gate_quality_report_path(root, "ci"),
            package_gate_quality_report_path(root, "release"),
            root / "quality-report.json",
        };
        fs::path newest_report;
        fs::file_time_type newest_time;
        bool found_newest = false;
        for (const auto& candidate : quality_report_candidates) {
            if (read_all(candidate).empty()) continue;
            std::error_code time_ec;
            fs::file_time_type modified = fs::last_write_time(candidate, time_ec);
            if (time_ec) continue;
            if (!found_newest || modified > newest_time) {
                found_newest = true;
                newest_time = modified;
                newest_report = candidate;
            }
        }
        if (found_newest) {
            load_quality_report(newest_report);
        }
    }
    std::vector<std::string> tools;
    std::vector<std::string> url_prefixes;
    std::vector<std::string> http_methods;
    std::vector<std::string> allowed_headers;
    std::vector<std::pair<std::string, std::string>> required_headers;
    std::vector<std::string> command_prefixes;
    bool max_body_found = false;
    double max_body_bytes = 0;
    bool max_timeout_found = false;
    double max_timeout = 0;
    bool allow_shell_found = false;
    bool allow_shell = false;
    bool approval_bool_found = false;
    bool approval_bool = false;
    bool approval_string_found = false;
    std::string approval_mode;
    bool approval_token_found = false;
    std::string approval_token;
    bool approval_message_found = false;
    std::string approval_message;
    if (!tool_manifest.empty()) {
        tools = manifest_string_array(tool_manifest, "tools");
        url_prefixes = manifest_string_array(tool_manifest, "url_prefixes");
        http_methods = manifest_string_array(tool_manifest, "http_methods");
        allowed_headers = manifest_string_array(tool_manifest, "allowed_headers");
        required_headers = manifest_string_object(tool_manifest, "required_headers");
        max_body_bytes = manifest_number_field(tool_manifest, "max_body_bytes", 0, &max_body_found);
        max_timeout = manifest_number_field(tool_manifest, "max_timeout", 0, &max_timeout_found);
        command_prefixes = manifest_string_array(tool_manifest, "command_prefixes");
        allow_shell = manifest_bool_field(tool_manifest, "allow_shell", false, &allow_shell_found);
        approval_bool = manifest_bool_field(tool_manifest, "approval", false, &approval_bool_found);
        approval_mode = manifest_string_field(tool_manifest, "approval", "", &approval_string_found);
        approval_token = manifest_string_field(tool_manifest, "approval_token", "", &approval_token_found);
        approval_message = manifest_string_field(tool_manifest, "approval_message", "", &approval_message_found);
    }
    bool plugin_sandbox_found = false;
    std::string plugin_sandbox;
    std::vector<std::string> plugin_manifests;
    std::vector<std::string> plugin_allowed_exports;
    std::vector<std::string> plugin_host_capabilities;
    bool plugin_max_memory_found = false;
    double plugin_max_memory_bytes = 0;
    bool plugin_max_call_found = false;
    double plugin_max_call_ms = 0;
    if (!plugin_policy_manifest.empty()) {
        plugin_sandbox = manifest_string_field(plugin_policy_manifest, "sandbox", "", &plugin_sandbox_found);
        plugin_manifests = manifest_string_array(plugin_policy_manifest, "manifests");
        plugin_allowed_exports = manifest_string_array(plugin_policy_manifest, "allowed_exports");
        if (plugin_allowed_exports.empty()) plugin_allowed_exports = manifest_string_array(plugin_policy_manifest, "exports");
        plugin_host_capabilities = manifest_string_array(plugin_policy_manifest, "host_capabilities");
        plugin_max_memory_bytes = manifest_number_field(plugin_policy_manifest, "max_memory_bytes", 0, &plugin_max_memory_found);
        plugin_max_call_ms = manifest_number_field(plugin_policy_manifest, "max_call_ms", 0, &plugin_max_call_found);
    }
    auto json_string_array = [](const std::vector<std::string>& values) {
        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) out << ", ";
            out << "\"" << json_escape(values[i]) << "\"";
        }
        out << "]";
        return out.str();
    };
    auto json_string_object = [](const std::vector<std::pair<std::string, std::string>>& values) {
        std::ostringstream out;
        out << "{";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) out << ", ";
            out << "\"" << json_escape(values[i].first) << "\": \"" << json_escape(values[i].second) << "\"";
        }
        out << "}";
        return out.str();
    };
    std::ostringstream api;
    api << "{\n"
        << "  \"name\": \"" << json_escape(name) << "\",\n"
        << "  \"version\": \"" << json_escape(version) << "\",\n"
        << "  \"package_symbol_count\": " << symbols.size() << ",\n"
        << "  \"stdlib_module_count\": " << stdlib_modules.size() << ",\n"
        << "  \"stdlib_symbol_count\": " << stdlib_symbol_count << ",\n"
        << "  \"symbols\": [\n";
    for (size_t i = 0; i < symbols.size(); ++i) {
        const auto& symbol = symbols[i];
        api << "    {\"kind\": \"" << json_escape(symbol.kind)
            << "\", \"name\": \"" << json_escape(symbol.name)
            << "\", \"signature\": \"" << json_escape(symbol.signature)
            << "\", \"source\": \"" << json_escape(symbol.source.generic_string())
            << "\", \"line\": " << symbol.line << "}";
        if (i + 1 < symbols.size()) api << ",";
        api << "\n";
    }
    api << "  ],\n"
        << "  \"stdlibModules\": [\n";
    for (size_t i = 0; i < stdlib_modules.size(); ++i) {
        const auto& module = stdlib_modules[i];
        api << "    {\"name\": \"" << json_escape(module.first)
            << "\", \"symbol_count\": " << module.second.size()
            << ", \"symbols\": [";
        for (size_t j = 0; j < module.second.size(); ++j) {
            const auto& symbol = module.second[j];
            if (j) api << ", ";
            api << "{\"kind\": \"" << json_escape(symbol.kind)
                << "\", \"name\": \"" << json_escape(symbol.name)
                << "\", \"signature\": \"" << json_escape(symbol.signature)
                << "\", \"source\": \"" << json_escape(symbol.source)
                << "\", \"line\": " << symbol.line << "}";
        }
        api << "]}";
        if (i + 1 < stdlib_modules.size()) api << ",";
        api << "\n";
    }
    api << "  ]";
    if (!tool_manifest.empty()) {
        api << ",\n  \"toolPolicy\": {\n"
            << "    \"source\": \"" << json_escape(kToolPolicyManifest.generic_string()) << "\",\n"
            << "    \"tools\": " << json_string_array(tools) << ",\n"
            << "    \"urlPrefixes\": " << json_string_array(url_prefixes) << ",\n"
            << "    \"httpMethods\": " << json_string_array(http_methods) << ",\n"
            << "    \"allowedHeaders\": " << json_string_array(allowed_headers) << ",\n"
            << "    \"requiredHeaders\": " << json_string_object(required_headers) << ",\n"
            << "    \"maxBodyBytes\": " << (max_body_found ? std::to_string((long long)max_body_bytes) : "null") << ",\n"
            << "    \"maxTimeout\": " << (max_timeout_found ? std::to_string(max_timeout) : "null") << ",\n"
            << "    \"allowShell\": " << (allow_shell_found ? (allow_shell ? "true" : "false") : "null") << ",\n"
            << "    \"commandPrefixes\": " << json_string_array(command_prefixes) << ",\n"
            << "    \"approval\": ";
        if (approval_bool_found) api << (approval_bool ? "true" : "false");
        else if (approval_string_found) api << "\"" << json_escape(approval_mode) << "\"";
        else api << "null";
        api << ",\n"
            << "    \"approvalTokenConfigured\": " << (approval_token_found ? "true" : "false") << ",\n"
            << "    \"approvalMessage\": " << (approval_message_found ? "\"" + json_escape(approval_message) + "\"" : "null") << "\n"
            << "  }";
    }
    if (!plugin_policy_manifest.empty()) {
        api << ",\n  \"pluginPolicy\": {\n"
            << "    \"source\": \"" << json_escape(kPluginPolicyManifest.generic_string()) << "\",\n"
            << "    \"sandbox\": " << (plugin_sandbox_found ? "\"" + json_escape(plugin_sandbox) + "\"" : "null") << ",\n"
            << "    \"manifests\": " << json_string_array(plugin_manifests) << ",\n"
            << "    \"allowedExports\": " << json_string_array(plugin_allowed_exports) << ",\n"
            << "    \"hostCapabilities\": " << json_string_array(plugin_host_capabilities) << ",\n"
            << "    \"maxMemoryBytes\": " << (plugin_max_memory_found ? std::to_string((long long)plugin_max_memory_bytes) : "null") << ",\n"
            << "    \"maxCallMs\": " << (plugin_max_call_found ? std::to_string((long long)plugin_max_call_ms) : "null") << "\n"
            << "  }";
    }
    if (bench_report_present) {
        api << ",\n  \"benchmark\": {\n"
            << "    \"source\": \"" << json_escape(bench_report_display.generic_string()) << "\",\n"
            << "    \"speedup\": ";
        if (bench_speedup_found) api << format_bench_number(bench_speedup);
        else api << "null";
        api << ",\n"
            << "    \"suraFasterByPython\": ";
        if (bench_python_found) api << format_bench_number(bench_python_ratio);
        else api << "null";
        api << "\n  }";
    }
    if (test_report_present) {
        api << ",\n  \"tests\": {\n"
            << "    \"source\": \"" << json_escape(test_report_display.generic_string()) << "\",\n"
            << "    \"ok\": ";
        if (test_ok_found) api << (test_ok ? "true" : "false");
        else api << "null";
        api << ",\n"
            << "    \"total\": ";
        if (test_total_found) api << (long long)test_total;
        else api << "null";
        api << ",\n"
            << "    \"passed\": ";
        if (test_passed_found) api << (long long)test_passed;
        else api << "null";
        api << ",\n"
            << "    \"failed\": ";
        if (test_failed_found) api << (long long)test_failed;
        else api << "null";
        api << ",\n"
            << "    \"jit\": ";
        if (test_jit_found) api << (test_jit ? "true" : "false");
        else api << "null";
        api << "\n  }";
    }
    if (audit_report_present) {
        api << ",\n  \"audit\": {\n"
            << "    \"source\": \"" << json_escape(audit_report_display.generic_string()) << "\",\n"
            << "    \"passed\": ";
        if (audit_passed_found) api << (audit_passed ? "true" : "false");
        else api << "null";
        api << ",\n"
            << "    \"findingCount\": ";
        if (audit_finding_count_found) api << (long long)audit_finding_count_number;
        else api << audit_findings.size();
        api << ",\n"
            << "    \"findings\": [\n";
        for (size_t i = 0; i < audit_findings.size(); ++i) {
            const auto& finding = audit_findings[i];
            api << "      {\"kind\": \"" << json_escape(finding.kind)
                << "\", \"message\": \"" << json_escape(finding.message)
                << "\", \"source\": \"" << json_escape(finding.source)
                << "\", \"line\": " << finding.line << "}";
            if (i + 1 < audit_findings.size()) api << ",";
            api << "\n";
        }
        api << "    ]\n"
            << "  }";
    }
    if (quality_report_present) {
        api << ",\n  \"quality\": {\n"
            << "    \"source\": \"" << json_escape(quality_report_display.generic_string()) << "\",\n"
            << "    \"score\": ";
        if (quality_score_found) api << (long long)quality_score;
        else api << "null";
        api << ",\n"
            << "    \"possible\": ";
        if (quality_possible_found) api << (long long)quality_possible;
        else api << "null";
        api << ",\n"
            << "    \"grade\": ";
        if (quality_grade_found) api << "\"" << json_escape(quality_grade) << "\"";
        else api << "null";
        api << ",\n"
            << "    \"passed\": ";
        if (quality_passed_found) api << (quality_passed ? "true" : "false");
        else api << "null";
        api << ",\n"
            << "    \"warnings\": ";
        if (quality_warnings_found) api << (long long)quality_warnings;
        else api << "null";
        api << ",\n"
            << "    \"errors\": ";
        if (quality_errors_found) api << (long long)quality_errors;
        else api << "null";
        api << "\n  }";
    }
    api << "\n}\n";

    std::ostringstream search;
    search << "{\n"
           << "  \"name\": \"" << json_escape(name) << "\",\n"
           << "  \"version\": \"" << json_escape(version) << "\",\n"
           << "  \"entries\": [\n";
    std::vector<PackageDocSearchEntry> search_entries;
    bool first_search_entry = true;
    auto add_search_entry = [&](const std::string& type,
                                const std::string& kind,
                                const std::string& entry_name,
                                const std::string& title,
                                const std::string& text,
                                const fs::path& source,
                                int line) {
        search_entries.push_back({type, kind, entry_name, title, text, source, line});
        if (!first_search_entry) search << ",\n";
        first_search_entry = false;
        search << "    {\"type\": \"" << json_escape(type)
               << "\", \"kind\": \"" << json_escape(kind)
               << "\", \"name\": \"" << json_escape(entry_name)
               << "\", \"title\": \"" << json_escape(title)
               << "\", \"text\": \"" << json_escape(text)
               << "\", \"source\": \"" << json_escape(source.generic_string())
               << "\", \"line\": ";
        if (line > 0) search << line;
        else search << "null";
        search << "}";
    };
    for (const auto& symbol : symbols) {
        std::string source = symbol.source.generic_string() + ":" + std::to_string(symbol.line);
        add_search_entry("symbol", symbol.kind, symbol.name, symbol.name,
                         symbol.kind + " " + symbol.name + " " + symbol.signature + " " + source,
                         symbol.source, symbol.line);
    }
    for (const auto& module : stdlib_modules) {
        fs::path module_source("builtin:" + module.first);
        add_search_entry("stdlib_module", "module", module.first, module.first,
                         "standard library module " + module.first, module_source, 0);
        for (const auto& symbol : module.second) {
            std::string qualified = module.first + "." + symbol.name;
            add_search_entry("stdlib_symbol", symbol.kind, qualified, qualified,
                             "standard library " + symbol.signature + " " + symbol.source,
                             fs::path(symbol.source), symbol.line);
        }
    }
    if (!tool_manifest.empty()) {
        add_search_entry("tool_policy", "policy", kToolPolicyManifest.generic_string(), "Tool Policy",
                         "package tool policy " + kToolPolicyManifest.generic_string(),
                         kToolPolicyManifest, 0);
        for (const auto& tool : tools) {
            add_search_entry("tool_policy", "tool", tool, "tool " + tool,
                             "allowed tool " + tool + " from " + kToolPolicyManifest.generic_string(),
                             kToolPolicyManifest, 0);
        }
        for (const auto& method : http_methods) {
            add_search_entry("tool_policy", "http_method", method, "HTTP " + method,
                             "allowed HTTP method " + method + " from " + kToolPolicyManifest.generic_string(),
                             kToolPolicyManifest, 0);
        }
        for (const auto& prefix : url_prefixes) {
            add_search_entry("tool_policy", "url_prefix", prefix, "URL prefix " + prefix,
                             "allowed URL prefix " + prefix + " from " + kToolPolicyManifest.generic_string(),
                             kToolPolicyManifest, 0);
        }
        for (const auto& header : allowed_headers) {
            add_search_entry("tool_policy", "allowed_header", header, "Allowed header " + header,
                             "allowed HTTP header " + header + " from " + kToolPolicyManifest.generic_string(),
                             kToolPolicyManifest, 0);
        }
        for (const auto& header : required_headers) {
            add_search_entry("tool_policy", "required_header", header.first, "Required header " + header.first,
                             "required HTTP header " + header.first + " from " + kToolPolicyManifest.generic_string(),
                             kToolPolicyManifest, 0);
        }
        for (const auto& prefix : command_prefixes) {
            add_search_entry("tool_policy", "command_prefix", prefix, "Command prefix " + prefix,
                             "allowed shell command prefix from " + kToolPolicyManifest.generic_string(),
                             kToolPolicyManifest, 0);
        }
        if (max_body_found) {
            add_search_entry("tool_policy", "limit", "max_body_bytes", "Max body bytes",
                             "tool policy max_body_bytes " + std::to_string((long long)max_body_bytes),
                             kToolPolicyManifest, 0);
        }
        if (max_timeout_found) {
            add_search_entry("tool_policy", "limit", "max_timeout", "Max timeout",
                             "tool policy max_timeout " + std::to_string(max_timeout),
                             kToolPolicyManifest, 0);
        }
        if (allow_shell_found) {
            add_search_entry("tool_policy", "shell", "allow_shell", "Shell allowed",
                             std::string("tool policy allow_shell ") + (allow_shell ? "true" : "false"),
                             kToolPolicyManifest, 0);
        }
        if (approval_bool_found || approval_string_found) {
            std::string approval_text = approval_bool_found ? (approval_bool ? "true" : "false") : approval_mode;
            add_search_entry("tool_policy", "approval", "approval", "Approval policy",
                             "tool policy approval " + approval_text,
                             kToolPolicyManifest, 0);
        }
        if (approval_token_found) {
            add_search_entry("tool_policy", "approval", "approval_token", "Approval token",
                             "tool policy approval token configured",
                             kToolPolicyManifest, 0);
        }
        if (approval_message_found) {
            add_search_entry("tool_policy", "approval", "approval_message", "Approval message",
                             "tool policy approval message configured",
                             kToolPolicyManifest, 0);
        }
    }
    if (!plugin_policy_manifest.empty()) {
        add_search_entry("plugin_policy", "policy", kPluginPolicyManifest.generic_string(), "Plugin Policy",
                         "package native plugin policy " + kPluginPolicyManifest.generic_string(),
                         kPluginPolicyManifest, 0);
        if (plugin_sandbox_found) {
            add_search_entry("plugin_policy", "sandbox", plugin_sandbox, "Plugin sandbox " + plugin_sandbox,
                             "plugin sandbox " + plugin_sandbox + " from " + kPluginPolicyManifest.generic_string(),
                             kPluginPolicyManifest, 0);
        }
        for (const auto& item : plugin_manifests) {
            add_search_entry("plugin_policy", "manifest", item, "Plugin manifest " + item,
                             "allowed plugin manifest " + item + " from " + kPluginPolicyManifest.generic_string(),
                             kPluginPolicyManifest, 0);
        }
        for (const auto& item : plugin_allowed_exports) {
            add_search_entry("plugin_policy", "allowed_export", item, "Plugin export " + item,
                             "allowed plugin export " + item + " from " + kPluginPolicyManifest.generic_string(),
                             kPluginPolicyManifest, 0);
        }
        for (const auto& item : plugin_host_capabilities) {
            add_search_entry("plugin_policy", "host_capability", item, "Plugin host capability " + item,
                             "allowed plugin host capability " + item + " from " + kPluginPolicyManifest.generic_string(),
                             kPluginPolicyManifest, 0);
        }
        if (plugin_max_memory_found) {
            add_search_entry("plugin_policy", "quota", "max_memory_bytes", "Plugin max memory bytes",
                             "plugin max_memory_bytes " + std::to_string((long long)plugin_max_memory_bytes),
                             kPluginPolicyManifest, 0);
        }
        if (plugin_max_call_found) {
            add_search_entry("plugin_policy", "quota", "max_call_ms", "Plugin max call ms",
                             "plugin max_call_ms " + std::to_string((long long)plugin_max_call_ms),
                             kPluginPolicyManifest, 0);
        }
    }
    if (bench_report_present) {
        std::ostringstream bench_text;
        bench_text << "benchmark report " << bench_report_display.generic_string();
        if (bench_speedup_found) bench_text << " jit speedup " << format_bench_number(bench_speedup) << "x";
        if (bench_python_found) bench_text << " sura python speed " << format_bench_number(bench_python_ratio) << "x";
        add_search_entry("benchmark", "performance", "benchmark", "Benchmark Summary",
                         bench_text.str(), bench_report_display, 0);
    }
    if (test_report_present) {
        std::ostringstream test_text;
        test_text << "package test report " << test_report_display.generic_string();
        if (test_ok_found) test_text << " ok " << (test_ok ? "true" : "false");
        if (test_total_found) test_text << " total " << (long long)test_total;
        if (test_passed_found) test_text << " passed " << (long long)test_passed;
        if (test_failed_found) test_text << " failed " << (long long)test_failed;
        add_search_entry("test", "verification", "tests", "Test Summary",
                         test_text.str(), test_report_display, 0);
    }
    if (audit_report_present) {
        std::ostringstream audit_text;
        audit_text << "security audit report " << audit_report_display.generic_string();
        if (audit_passed_found) audit_text << " passed " << (audit_passed ? "true" : "false");
        audit_text << " findings ";
        if (audit_finding_count_found) audit_text << (long long)audit_finding_count_number;
        else audit_text << audit_findings.size();
        add_search_entry("audit", "security", "audit", "Security Audit Summary",
                         audit_text.str(), audit_report_display, 0);
        for (const auto& finding : audit_findings) {
            fs::path source = finding.source.empty() ? audit_report_display : fs::path(finding.source);
            std::string kind = finding.kind.empty() ? "finding" : finding.kind;
            add_search_entry("audit", kind, kind, "Audit " + kind,
                             finding.message.empty() ? ("audit finding " + kind) : finding.message,
                             source, finding.line);
        }
    }
    if (quality_report_present) {
        std::ostringstream quality_text;
        quality_text << "package quality report " << quality_report_display.generic_string();
        if (quality_score_found) quality_text << " score " << (long long)quality_score;
        if (quality_possible_found) quality_text << "/" << (long long)quality_possible;
        if (quality_grade_found) quality_text << " grade " << quality_grade;
        if (quality_passed_found) quality_text << " passed " << (quality_passed ? "true" : "false");
        add_search_entry("quality", "readiness", "quality", "Quality Summary",
                         quality_text.str(), quality_report_display, 0);
    }
    search << "\n  ]\n}\n";

    std::ostringstream html;
    html << "<!doctype html><meta charset=\"utf-8\"><title>"
         << html_escape(name) << " docs</title>"
         << "<style>body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;max-width:1100px;margin:32px auto;padding:0 20px;line-height:1.45}"
         << "table{border-collapse:collapse;width:100%;margin:12px 0 24px}th,td{border:1px solid #d4d4d8;padding:8px;text-align:left;vertical-align:top}"
         << "th{background:#f4f4f5}.docs-search{display:block;max-width:460px;width:100%;padding:9px 10px;margin:8px 0 12px;border:1px solid #a1a1aa}"
         << "pre{background:#f8fafc;border:1px solid #e4e4e7;padding:12px;overflow:auto}code{white-space:pre-wrap}</style>"
         << "<h1>" << html_escape(name) << " " << html_escape(version) << "</h1>\n"
         << "<p><a href=\"api.json\">api.json</a> | <a href=\"search-index.json\">search-index.json</a></p>\n"
         << "<h2>API Reference</h2>"
         << "<label for=\"symbol-search\"><strong>Search Docs</strong></label>"
         << "<input class=\"docs-search\" id=\"symbol-search\" type=\"search\" placeholder=\"Function, type, source, tool, or policy\" aria-label=\"Search docs\">\n"
         << "<table id=\"api-table\">"
         << "<tr><th>Kind</th><th>Name</th><th>Signature</th><th>Source</th></tr>\n";
    if (symbols.empty()) {
        html << "<tr><td colspan=\"4\"><em>No public Sura symbols found.</em></td></tr>\n";
    } else {
        for (const auto& symbol : symbols) {
            std::string search_text = symbol.kind + " " + symbol.name + " " + symbol.signature + " " + symbol.source.generic_string();
            html << "<tr data-search=\"" << html_escape(search_text) << "\"><td>" << html_escape(symbol.kind) << "</td>"
                 << "<td><code>" << html_escape(symbol.name) << "</code></td>"
                 << "<td><code>" << html_escape(symbol.signature) << "</code></td>"
                 << "<td><small>" << html_escape(symbol.source.generic_string()) << ":"
                 << symbol.line << "</small></td></tr>\n";
        }
    }
    html << "<tr id=\"search-empty\" style=\"display:none\"><td colspan=\"4\"><em>No matching API symbols.</em></td></tr>\n"
         << "</table>\n";
    html << "<h2>Standard Library Modules</h2>"
         << "<table id=\"stdlib-table\">"
         << "<tr><th>Module</th><th>Name</th><th>Signature</th><th>Source</th></tr>\n";
    for (const auto& module : stdlib_modules) {
        for (const auto& symbol : module.second) {
            std::string qualified = module.first + "." + symbol.name;
            std::string search_text = "stdlib " + module.first + " " + qualified + " " + symbol.signature + " " + symbol.source;
            html << "<tr data-search=\"" << html_escape(search_text) << "\"><td><code>"
                 << html_escape(module.first) << "</code></td>"
                 << "<td><code>" << html_escape(qualified) << "</code></td>"
                 << "<td><code>" << html_escape(symbol.signature) << "</code></td>"
                 << "<td><small>" << html_escape(symbol.source) << ":" << symbol.line << "</small></td></tr>\n";
        }
    }
    html << "<tr id=\"stdlib-search-empty\" style=\"display:none\"><td colspan=\"4\"><em>No matching standard library symbols.</em></td></tr>\n"
         << "</table>\n";
    html << "<h2>Search Index</h2>"
         << "<table id=\"search-index-table\">"
         << "<tr><th>Type</th><th>Kind</th><th>Name</th><th>Text</th><th>Source</th></tr>\n";
    for (const auto& entry : search_entries) {
        std::string source_text = entry.source.generic_string();
        if (entry.line > 0) source_text += ":" + std::to_string(entry.line);
        std::string search_text = entry.type + " " + entry.kind + " " + entry.name + " " +
                                  entry.title + " " + entry.text + " " + source_text;
        html << "<tr data-search=\"" << html_escape(search_text) << "\"><td><code>"
             << html_escape(entry.type) << "</code></td>"
             << "<td><code>" << html_escape(entry.kind) << "</code></td>"
             << "<td><code>" << html_escape(entry.name) << "</code></td>"
             << "<td>" << html_escape(entry.text) << "</td>"
             << "<td><small>" << html_escape(source_text) << "</small></td></tr>\n";
    }
    html << "<tr id=\"index-search-empty\" style=\"display:none\"><td colspan=\"5\"><em>No matching search index entries.</em></td></tr>\n"
         << "</table>\n";
    html << "<script>(function(){var input=document.getElementById('symbol-search');var apiRows=[].slice.call(document.querySelectorAll('#api-table tr[data-search]'));var stdRows=[].slice.call(document.querySelectorAll('#stdlib-table tr[data-search]'));var idxRows=[].slice.call(document.querySelectorAll('#search-index-table tr[data-search]'));var apiEmpty=document.getElementById('search-empty');var stdEmpty=document.getElementById('stdlib-search-empty');var idxEmpty=document.getElementById('index-search-empty');if(!input)return;function apply(rows,q){var shown=0;rows.forEach(function(row){var ok=!q||row.getAttribute('data-search').toLowerCase().indexOf(q)!==-1;row.style.display=ok?'':'none';if(ok)shown++;});return shown;}function update(){var q=input.value.toLowerCase().trim();var apiShown=apply(apiRows,q);var stdShown=apply(stdRows,q);var idxShown=apply(idxRows,q);if(apiEmpty)apiEmpty.style.display=(apiRows.length&&!apiShown)?'':'none';if(stdEmpty)stdEmpty.style.display=(stdRows.length&&!stdShown)?'':'none';if(idxEmpty)idxEmpty.style.display=(idxRows.length&&!idxShown)?'':'none';}input.addEventListener('input',update);update();})();</script>\n"
         << "<h2>Manifest</h2><pre>" << html_escape(manifest) << "</pre>\n";
    if (bench_report_present) {
        html << "<h2>Benchmark Summary</h2><table>"
             << "<tr><th>Report</th><td><code>" << html_escape(bench_report_display.generic_string()) << "</code></td></tr>"
             << "<tr><th>JIT speedup</th><td><code>"
             << (bench_speedup_found ? format_bench_number(bench_speedup) + "x" : std::string("unknown"))
             << "</code></td></tr>"
             << "<tr><th>Sura faster than Python</th><td><code>"
             << (bench_python_found ? format_bench_number(bench_python_ratio) + "x" : std::string("not measured"))
             << "</code></td></tr>"
             << "</table>\n";
    }
    if (quality_report_present) {
        html << "<h2>Quality Summary</h2><table>"
             << "<tr><th>Report</th><td><code>" << html_escape(quality_report_display.generic_string()) << "</code></td></tr>"
             << "<tr><th>Score</th><td><code>";
        if (quality_score_found) html << (long long)quality_score;
        else html << "unknown";
        if (quality_possible_found) html << "/" << (long long)quality_possible;
        html << "</code></td></tr>"
             << "<tr><th>Grade</th><td><code>"
             << (quality_grade_found ? html_escape(quality_grade) : std::string("unknown"))
             << "</code></td></tr>"
             << "<tr><th>Passed</th><td><code>"
             << (quality_passed_found ? (quality_passed ? "true" : "false") : "unknown")
             << "</code></td></tr>"
             << "<tr><th>Warnings</th><td><code>"
             << (quality_warnings_found ? std::to_string((long long)quality_warnings) : std::string("unknown"))
             << "</code></td></tr>"
             << "<tr><th>Errors</th><td><code>"
             << (quality_errors_found ? std::to_string((long long)quality_errors) : std::string("unknown"))
             << "</code></td></tr>"
             << "</table>\n";
    }
    if (test_report_present) {
        html << "<h2>Test Summary</h2><table>"
             << "<tr><th>Report</th><td><code>" << html_escape(test_report_display.generic_string()) << "</code></td></tr>"
             << "<tr><th>OK</th><td><code>"
             << (test_ok_found ? (test_ok ? "true" : "false") : "unknown")
             << "</code></td></tr>"
             << "<tr><th>Total</th><td><code>"
             << (test_total_found ? std::to_string((long long)test_total) : std::string("unknown"))
             << "</code></td></tr>"
             << "<tr><th>Passed</th><td><code>"
             << (test_passed_found ? std::to_string((long long)test_passed) : std::string("unknown"))
             << "</code></td></tr>"
             << "<tr><th>Failed</th><td><code>"
             << (test_failed_found ? std::to_string((long long)test_failed) : std::string("unknown"))
             << "</code></td></tr>"
             << "<tr><th>JIT</th><td><code>"
             << (test_jit_found ? (test_jit ? "true" : "false") : "unknown")
             << "</code></td></tr>"
             << "</table>\n";
    }
    if (audit_report_present) {
        html << "<h2>Security Audit Summary</h2><table>"
             << "<tr><th>Report</th><td><code>" << html_escape(audit_report_display.generic_string()) << "</code></td></tr>"
             << "<tr><th>Passed</th><td><code>"
             << (audit_passed_found ? (audit_passed ? "true" : "false") : "unknown")
             << "</code></td></tr>"
             << "<tr><th>Findings</th><td><code>";
        if (audit_finding_count_found) html << (long long)audit_finding_count_number;
        else html << audit_findings.size();
        html << "</code></td></tr>"
             << "</table>\n";
        if (!audit_findings.empty()) {
            html << "<table><tr><th>Kind</th><th>Message</th><th>Source</th></tr>\n";
            for (const auto& finding : audit_findings) {
                std::string source = finding.source;
                if (finding.line > 0) source += ":" + std::to_string(finding.line);
                html << "<tr><td><code>" << html_escape(finding.kind) << "</code></td>"
                     << "<td>" << html_escape(finding.message) << "</td>"
                     << "<td><small>" << html_escape(source) << "</small></td></tr>\n";
            }
            html << "</table>\n";
        }
    }
    if (!tool_manifest.empty()) {
        html << "<h2>Tool Policy Summary</h2><table>"
             << "<tr><th>Tools</th><td>" << join_values(tools) << "</td></tr>"
             << "<tr><th>URL prefixes</th><td>" << join_values(url_prefixes) << "</td></tr>"
             << "<tr><th>HTTP methods</th><td>" << join_values(http_methods) << "</td></tr>"
             << "<tr><th>Allowed headers</th><td>" << join_values(allowed_headers) << "</td></tr>"
             << "<tr><th>Required headers</th><td><code>" << html_escape(json_string_object(required_headers)) << "</code></td></tr>"
             << "<tr><th>Max body bytes</th><td><code>" << (max_body_found ? std::to_string((long long)max_body_bytes) : "unspecified") << "</code></td></tr>"
             << "<tr><th>Max timeout</th><td><code>" << (max_timeout_found ? std::to_string(max_timeout) : "unspecified") << "</code></td></tr>"
             << "<tr><th>Shell allowed</th><td><code>"
             << (allow_shell_found ? (allow_shell ? "true" : "false") : "unspecified")
             << "</code></td></tr>"
             << "<tr><th>Command prefixes</th><td>" << join_values(command_prefixes) << "</td></tr>"
             << "<tr><th>Approval</th><td><code>";
        if (approval_bool_found) html << (approval_bool ? "true" : "false");
        else if (approval_string_found) html << html_escape(approval_mode);
        else html << "unspecified";
        html << "</code></td></tr>"
             << "<tr><th>Approval token</th><td><code>"
             << (approval_token_found ? "configured" : "not configured")
             << "</code></td></tr>"
             << "</table>\n";
        html << "<h2>Tool Policy</h2><pre>" << html_escape(tool_manifest) << "</pre>\n";
    }
    if (!plugin_policy_manifest.empty()) {
        html << "<h2>Plugin Policy Summary</h2><table>"
             << "<tr><th>Sandbox</th><td><code>"
             << (plugin_sandbox_found ? html_escape(plugin_sandbox) : std::string("unspecified"))
             << "</code></td></tr>"
             << "<tr><th>Manifests</th><td>" << join_values(plugin_manifests) << "</td></tr>"
             << "<tr><th>Allowed exports</th><td>" << join_values(plugin_allowed_exports) << "</td></tr>"
             << "<tr><th>Host capabilities</th><td>" << join_values(plugin_host_capabilities) << "</td></tr>"
             << "<tr><th>Max memory bytes</th><td><code>"
             << (plugin_max_memory_found ? std::to_string((long long)plugin_max_memory_bytes) : "unspecified")
             << "</code></td></tr>"
             << "<tr><th>Max call ms</th><td><code>"
             << (plugin_max_call_found ? std::to_string((long long)plugin_max_call_ms) : "unspecified")
             << "</code></td></tr>"
             << "</table>\n";
        html << "<h2>Plugin Policy</h2><pre>" << html_escape(plugin_policy_manifest) << "</pre>\n";
    }
    index_path = out_dir / "index.html";
    fs::path api_path = out_dir / "api.json";
    fs::path search_path = out_dir / "search-index.json";
    return write_all(index_path, html.str()) && write_all(api_path, api.str()) && write_all(search_path, search.str());
}

static size_t json_field_occurrences(const std::string& json, const std::string& field) {
    std::regex re("\"" + field + "\"\\s*:");
    return (size_t)std::distance(std::sregex_iterator(json.begin(), json.end(), re), std::sregex_iterator());
}

static size_t json_string_field_value_occurrences(const std::string& json,
                                                  const std::string& field,
                                                  const std::string& value) {
    std::string pattern = "\"" + field + "\": \"" + value + "\"";
    size_t count = 0;
    size_t pos = 0;
    while ((pos = json.find(pattern, pos)) != std::string::npos) {
        ++count;
        pos += pattern.size();
    }
    return count;
}

static std::map<std::string, size_t> json_string_field_value_counts(const std::string& json,
                                                                    const std::string& field) {
    std::map<std::string, size_t> counts;
    std::regex re("\"" + field + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), re);
         it != std::sregex_iterator(); ++it) {
        counts[json_unescape((*it)[1].str())]++;
    }
    return counts;
}

static uintmax_t file_size_or_zero(const fs::path& path) {
    std::error_code ec;
    uintmax_t size = fs::file_size(path, ec);
    return ec ? 0 : size;
}

static bool write_docs_json_report(const fs::path& report_path,
                                   const fs::path& root,
                                   const fs::path& out_dir,
                                   const fs::path& index_path,
                                   bool passed) {
    std::string manifest = read_all(root / kManifest);
    std::string name = manifest_field(manifest, "name", project_name_from_cwd());
    std::string version = manifest_field(manifest, "version", "0.0.0");
    fs::path api_path = out_dir / "api.json";
    fs::path search_path = out_dir / "search-index.json";
    std::string api = read_all(api_path);
    std::string search = read_all(search_path);
    bool package_symbol_count_found = false;
    bool stdlib_module_count_found = false;
    bool stdlib_symbol_count_found = false;
    size_t symbol_count = (size_t)manifest_number_field(api, "package_symbol_count",
                                                        (double)json_field_occurrences(api, "kind"),
                                                        &package_symbol_count_found);
    size_t stdlib_module_count = (size_t)manifest_number_field(api, "stdlib_module_count", 0,
                                                               &stdlib_module_count_found);
    size_t stdlib_symbol_count = (size_t)manifest_number_field(api, "stdlib_symbol_count", 0,
                                                               &stdlib_symbol_count_found);
    size_t search_entry_count = json_field_occurrences(search, "type");
    bool tool_policy_present = api.find("\"toolPolicy\"") != std::string::npos;
    bool plugin_policy_present = api.find("\"pluginPolicy\"") != std::string::npos;
    bool benchmark_present = api.find("\"benchmark\"") != std::string::npos;
    bool audit_present = api.find("\"audit\"") != std::string::npos;
    bool quality_present = api.find("\"quality\"") != std::string::npos;
    std::string test_api = json_object_field(api, "tests");
    bool test_present = !test_api.empty();
    std::string audit_api = json_object_field(api, "audit");
    std::string quality_api = json_object_field(api, "quality");
    bool test_ok_found = false;
    bool test_ok = manifest_bool_field(test_api, "ok", false, &test_ok_found);
    bool test_total_found = false;
    size_t test_total = (size_t)manifest_number_field(test_api, "total", 0, &test_total_found);
    bool test_passed_found = false;
    size_t test_passed = (size_t)manifest_number_field(test_api, "passed", 0, &test_passed_found);
    bool test_failed_found = false;
    size_t test_failed = (size_t)manifest_number_field(test_api, "failed", 0, &test_failed_found);
    bool audit_passed_found = false;
    bool audit_passed = manifest_bool_field(audit_api, "passed", false, &audit_passed_found);
    bool audit_finding_count_found = false;
    size_t audit_finding_count = (size_t)manifest_number_field(audit_api, "findingCount", 0, &audit_finding_count_found);
    bool quality_passed_found = false;
    bool quality_passed = manifest_bool_field(quality_api, "passed", false, &quality_passed_found);
    bool quality_score_found = false;
    size_t quality_score = (size_t)manifest_number_field(quality_api, "score", 0, &quality_score_found);
    size_t tool_policy_search_entry_count = json_string_field_value_occurrences(search, "type", "tool_policy");
    size_t plugin_policy_search_entry_count = json_string_field_value_occurrences(search, "type", "plugin_policy");
    size_t benchmark_search_entry_count = json_string_field_value_occurrences(search, "type", "benchmark");
    size_t test_search_entry_count = json_string_field_value_occurrences(search, "type", "test");
    size_t audit_search_entry_count = json_string_field_value_occurrences(search, "type", "audit");
    size_t quality_search_entry_count = json_string_field_value_occurrences(search, "type", "quality");
    auto search_type_counts = json_string_field_value_counts(search, "type");
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.docs.v1\",\n"
        << "  \"package\": \"" << json_escape(name) << "\",\n"
        << "  \"version\": \"" << json_escape(version) << "\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"root\": \"" << json_escape(root.generic_string()) << "\",\n"
        << "  \"out_dir\": \"" << json_escape(out_dir.generic_string()) << "\",\n"
        << "  \"index\": \"" << json_escape(index_path.generic_string()) << "\",\n"
        << "  \"api\": \"" << json_escape(api_path.generic_string()) << "\",\n"
        << "  \"search_index\": \"" << json_escape(search_path.generic_string()) << "\",\n"
        << "  \"index_bytes\": " << file_size_or_zero(index_path) << ",\n"
        << "  \"api_bytes\": " << file_size_or_zero(api_path) << ",\n"
        << "  \"search_index_bytes\": " << file_size_or_zero(search_path) << ",\n"
        << "  \"symbol_count\": " << symbol_count << ",\n"
        << "  \"stdlib_module_count\": " << (stdlib_module_count_found ? stdlib_module_count : 0) << ",\n"
        << "  \"stdlib_symbol_count\": " << (stdlib_symbol_count_found ? stdlib_symbol_count : 0) << ",\n"
        << "  \"search_entry_count\": " << search_entry_count << ",\n"
        << "  \"tool_policy_present\": " << (tool_policy_present ? "true" : "false") << ",\n"
        << "  \"plugin_policy_present\": " << (plugin_policy_present ? "true" : "false") << ",\n"
        << "  \"benchmark_present\": " << (benchmark_present ? "true" : "false") << ",\n"
        << "  \"test_present\": " << (test_present ? "true" : "false") << ",\n"
        << "  \"audit_present\": " << (audit_present ? "true" : "false") << ",\n"
        << "  \"quality_present\": " << (quality_present ? "true" : "false") << ",\n"
        << "  \"test_ok\": ";
    if (test_present && test_ok_found) out << (test_ok ? "true" : "false");
    else out << "null";
    out << ",\n"
        << "  \"test_total\": " << (test_present && test_total_found ? test_total : 0) << ",\n"
        << "  \"test_passed\": " << (test_present && test_passed_found ? test_passed : 0) << ",\n"
        << "  \"test_failed\": " << (test_present && test_failed_found ? test_failed : 0) << ",\n"
        << "  \"audit_passed\": ";
    if (audit_present && audit_passed_found) out << (audit_passed ? "true" : "false");
    else out << "null";
    out << ",\n"
        << "  \"audit_finding_count\": " << (audit_present && audit_finding_count_found ? audit_finding_count : 0) << ",\n"
        << "  \"quality_passed\": ";
    if (quality_present && quality_passed_found) out << (quality_passed ? "true" : "false");
    else out << "null";
    out << ",\n"
        << "  \"quality_score\": " << (quality_present && quality_score_found ? quality_score : 0) << ",\n"
        << "  \"tool_policy_search_entry_count\": " << tool_policy_search_entry_count << ",\n"
        << "  \"plugin_policy_search_entry_count\": " << plugin_policy_search_entry_count << ",\n"
        << "  \"benchmark_search_entry_count\": " << benchmark_search_entry_count << ",\n"
        << "  \"test_search_entry_count\": " << test_search_entry_count << ",\n"
        << "  \"audit_search_entry_count\": " << audit_search_entry_count << ",\n"
        << "  \"quality_search_entry_count\": " << quality_search_entry_count << ",\n"
        << "  \"search_entry_type_counts\": {";
    bool first_type_count = true;
    for (const auto& [type, count] : search_type_counts) {
        if (!first_type_count) out << ", ";
        first_type_count = false;
        out << "\"" << json_escape(type) << "\": " << count;
    }
    out << "}\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_docs_command(const std::string& out_arg, const fs::path& json_report) {
    fs::path out_dir = out_arg.empty() ? fs::path("docs") : fs::path(out_arg);
    fs::path index_path;
    if (!generate_package_docs(fs::current_path(), out_dir, index_path)) return err("failed to write docs");
    if (!json_report.empty()) {
        if (!write_docs_json_report(json_report, fs::current_path(), out_dir, index_path, true)) {
            return err("failed to write docs JSON report: " + json_report.generic_string());
        }
        ok("docs report written: " + json_report.generic_string());
    }
    ok("generated docs at " + index_path.generic_string());
    return 0;
}

static int cmd_docs(const std::vector<std::string>& argv) {
    std::string out_arg;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("docs --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("docs --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg docs [outdir] [--json report.json]\n";
            return 0;
        } else if (out_arg.empty()) {
            out_arg = arg;
        } else {
            return err("docs accepts one output directory and optional --json");
        }
    }
    return run_docs_command(out_arg, json_report);
}

static std::string strip_c_comments_and_directives(const std::string& input) {
    std::string no_block = std::regex_replace(input, std::regex("/\\*[^*]*\\*+(?:[^/*][^*]*\\*+)*/"), " ");
    std::string no_line = std::regex_replace(no_block, std::regex("//[^\r\n]*"), " ");
    std::ostringstream out;
    std::istringstream in(no_line);
    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim_copy(line);
        if (!trimmed.empty() && trimmed[0] == '#') continue;
        out << line << "\n";
    }
    return out.str();
}

static std::string strip_cpp_class_struct_bodies(const std::string& input) {
    std::string out;
    size_t i = 0;
    while (i < input.size()) {
        bool matched = false;
        for (const std::string keyword : {"class", "struct"}) {
            if (i + keyword.size() > input.size() || input.compare(i, keyword.size(), keyword) != 0) continue;
            bool before_ok = i == 0 || !(std::isalnum((unsigned char)input[i - 1]) || input[i - 1] == '_');
            bool after_ok = i + keyword.size() >= input.size() ||
                !(std::isalnum((unsigned char)input[i + keyword.size()]) || input[i + keyword.size()] == '_');
            if (!before_ok || !after_ok) continue;

            size_t brace = input.find('{', i + keyword.size());
            size_t semi = input.find(';', i + keyword.size());
            if (brace == std::string::npos || (semi != std::string::npos && semi < brace)) continue;

            int depth = 0;
            size_t j = brace;
            for (; j < input.size(); ++j) {
                if (input[j] == '{') ++depth;
                else if (input[j] == '}') {
                    --depth;
                    if (depth == 0) {
                        while (j < input.size() && input[j] != ';') ++j;
                        if (j < input.size()) ++j;
                        break;
                    }
                }
            }
            out += "\n";
            i = j;
            matched = true;
            break;
        }
        if (!matched) out.push_back(input[i++]);
    }
    return out;
}

static std::string bind_c_clean_type(std::string text) {
    text = trim_copy(std::regex_replace(text, std::regex("\\s+"), " "));
    text = std::regex_replace(text, std::regex("\\s*\\*\\s*"), "*");
    text = std::regex_replace(text, std::regex("\\s*&\\s*"), "&");
    text = std::regex_replace(text, std::regex("\\b(?:extern|static|inline|constexpr|volatile|register)\\b\\s*"), "");
    text = trim_copy(std::regex_replace(text, std::regex("\\s+"), " "));
    return text;
}

static std::string bind_c_normalize_signature_type(std::string t) {
    if (t == "char const*") t = "const char*";
    if (t.rfind("std::", 0) == 0) t = t.substr(5);
    return t;
}

static std::string bind_c_signature_type(const std::string& raw, bool is_return, bool& supported,
                                         const std::map<std::string, std::string>* aliases = nullptr) {
    (void)is_return;
    std::string t = bind_c_normalize_signature_type(lowercase_copy(bind_c_clean_type(raw)));
    for (int i = 0; aliases && i < 8; ++i) {
        auto it = aliases->find(t);
        if (it == aliases->end() || it->second == t) break;
        t = bind_c_normalize_signature_type(it->second);
    }
    supported = true;
    if (t.find('&') != std::string::npos) {
        supported = false;
        return t;
    }
    if (t == "void") return "void";
    if (t == "double") return "double";
    if (t == "float") return "float";
    if (t == "char*" || t == "const char*") return t;
    if (t.find('*') != std::string::npos) {
        supported = false;
        return t;
    }
    if (t == "int" || t == "signed int" || t == "unsigned int" ||
        t == "short" || t == "short int" || t == "unsigned short" || t == "unsigned short int" ||
        t == "long" || t == "long int" || t == "unsigned long" || t == "unsigned long int" ||
        t == "long long" || t == "long long int" || t == "unsigned long long" ||
        t == "unsigned long long int" || t == "size_t" || t == "ssize_t" ||
        t == "bool" || t == "_bool" ||
        t == "int8_t" || t == "uint8_t" || t == "int16_t" || t == "uint16_t" ||
        t == "int32_t" || t == "uint32_t" || t == "int64_t" || t == "uint64_t") {
        return "int";
    }
    supported = false;
    return t;
}

static bool bind_c_type_word(const std::string& text) {
    static const std::set<std::string> words = {
        "const", "char", "double", "float", "void", "int", "long", "short",
        "signed", "unsigned", "size_t", "ssize_t", "std::size_t", "bool", "_bool",
        "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t",
        "int64_t", "uint64_t"
    };
    return words.count(lowercase_copy(text)) != 0;
}

static std::map<std::string, std::string> bind_c_parse_type_aliases(const std::string& header) {
    std::map<std::string, std::string> raw_aliases;
    std::string content = strip_cpp_class_struct_bodies(strip_c_comments_and_directives(header));

    std::regex typedef_re("\\btypedef\\s+([^;{}()]+?)\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*;");
    for (auto it = std::sregex_iterator(content.begin(), content.end(), typedef_re);
         it != std::sregex_iterator(); ++it) {
        std::string alias = lowercase_copy(bind_c_clean_type((*it)[2].str()));
        std::string target = bind_c_normalize_signature_type(lowercase_copy(bind_c_clean_type((*it)[1].str())));
        if (!alias.empty() && !target.empty() && alias != target) raw_aliases[alias] = target;
    }

    std::regex using_re("\\busing\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*([^;{}()]+?)\\s*;");
    for (auto it = std::sregex_iterator(content.begin(), content.end(), using_re);
         it != std::sregex_iterator(); ++it) {
        std::string alias = lowercase_copy(bind_c_clean_type((*it)[1].str()));
        std::string target = bind_c_normalize_signature_type(lowercase_copy(bind_c_clean_type((*it)[2].str())));
        if (!alias.empty() && !target.empty() && alias != target) raw_aliases[alias] = target;
    }

    std::map<std::string, std::string> aliases;
    for (const auto& [alias, target] : raw_aliases) {
        bool supported = true;
        std::string resolved = bind_c_signature_type(target, true, supported, &raw_aliases);
        if (supported) aliases[alias] = resolved;
    }
    return aliases;
}

static bool bind_c_reserved_sura_word(const std::string& text) {
    static const std::set<std::string> words = {
        "and", "as", "break", "catch", "class", "continue", "do", "elif", "else",
        "end", "false", "for", "func", "if", "import", "in", "is", "nil", "not",
        "or", "repeat", "return", "super", "then", "this", "throw", "true", "try",
        "while"
    };
    return words.count(text) != 0;
}

static std::string bind_c_sura_identifier(std::string text, const std::string& fallback) {
    if (text.empty()) text = fallback;
    for (char& ch : text) {
        unsigned char c = (unsigned char)ch;
        if (!(std::isalnum(c) || ch == '_')) ch = '_';
    }
    if (text.empty() || !(std::isalpha((unsigned char)text[0]) || text[0] == '_')) text = fallback + "_" + text;
    if (bind_c_reserved_sura_word(text)) text = "c_" + text;
    return text;
}

struct BindCParam {
    std::string type;
    std::string name;
};

struct BindCFunction {
    std::string return_type;
    std::string symbol;
    std::string wrapper;
    std::vector<BindCParam> params;
    std::string skip_reason;
};

struct BindCConstant {
    std::string symbol;
    std::string name;
    std::string kind;
    std::string value;
    std::string literal;
};

static bool bind_c_type_is_string(const std::string& type) {
    return type == "char*" || type == "const char*";
}

static bool bind_c_type_is_floaty(const std::string& type) {
    return type == "double" || type == "float";
}

static std::string bind_c_ffi_shape_skip_reason(const BindCFunction& fn) {
    bool has_string_arg = false;
    bool has_float_arg = false;
    bool has_non_float_arg = false;
    for (const auto& param : fn.params) {
        has_string_arg = has_string_arg || bind_c_type_is_string(param.type);
        has_float_arg = has_float_arg || bind_c_type_is_floaty(param.type);
        has_non_float_arg = has_non_float_arg || !bind_c_type_is_floaty(param.type);
    }
    bool return_floaty = bind_c_type_is_floaty(fn.return_type);
    bool return_string = bind_c_type_is_string(fn.return_type);
    if (has_string_arg && (has_float_arg || return_floaty)) {
        return "string and double/float arguments cannot be mixed in this FFI mode";
    }
    if (return_string && has_float_arg) {
        return "char* returns cannot be combined with double/float arguments in this FFI mode";
    }
    if (has_float_arg && has_non_float_arg) {
        return "double/float arguments cannot be mixed with integer or string arguments in this FFI mode";
    }
    if (has_float_arg && !(return_floaty || fn.return_type == "void")) {
        return "double/float arguments require a double/float or void return in this FFI mode";
    }
    if (return_floaty && has_non_float_arg) {
        return "double/float returns require only double/float arguments in this FFI mode";
    }
    return "";
}

static std::vector<std::string> bind_c_split_params(const std::string& args) {
    std::vector<std::string> out;
    std::string current;
    int depth = 0;
    for (char ch : args) {
        if (ch == '(' || ch == '[') ++depth;
        else if ((ch == ')' || ch == ']') && depth > 0) --depth;
        if (ch == ',' && depth == 0) {
            out.push_back(trim_copy(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!trim_copy(current).empty()) out.push_back(trim_copy(current));
    return out;
}

static bool bind_c_parse_param(const std::string& raw, int index, BindCParam& out, std::string& reason,
                               const std::map<std::string, std::string>* aliases) {
    std::string text = trim_copy(raw);
    size_t eq = text.find('=');
    if (eq != std::string::npos) text = trim_copy(text.substr(0, eq));
    text = std::regex_replace(text, std::regex("\\[[^\\]]*\\]"), "*");
    if (text.empty() || text == "void") return false;

    std::string type_text = text;
    std::string param_name = "a" + std::to_string(index);
    std::smatch m;
    std::regex name_re("^(.*?)([A-Za-z_][A-Za-z0-9_]*)\\s*$");
    if (std::regex_match(text, m, name_re)) {
        std::string prefix = trim_copy(m[1].str());
        std::string last = m[2].str();
        if (!prefix.empty() && !(bind_c_type_word(last) && prefix.find('*') == std::string::npos)) {
            type_text = prefix;
            param_name = last;
        }
    }

    bool supported = true;
    std::string sig_type = bind_c_signature_type(type_text, false, supported, aliases);
    if (!supported || sig_type == "void") {
        reason = "unsupported parameter type '" + bind_c_clean_type(type_text) + "'";
        return false;
    }
    out.type = sig_type;
    out.name = bind_c_sura_identifier(param_name, "a" + std::to_string(index));
    return true;
}

static std::string bind_c_constant_text(std::string raw) {
    raw = std::regex_replace(raw, std::regex("/\\*.*?\\*/"), "");
    raw = std::regex_replace(raw, std::regex("//.*$"), "");
    raw = trim_copy(raw);
    std::smatch m;
    std::regex paren_re("^\\(([^()]+)\\)$");
    while (std::regex_match(raw, m, paren_re)) raw = trim_copy(m[1].str());
    return raw;
}

static bool bind_c_numeric_constant_value(std::string raw, std::string& value) {
    std::string text = bind_c_constant_text(raw);
    if (text.empty()) return false;

    std::smatch m;
    std::regex hex_re("^([+-]?)(0[xX][0-9A-Fa-f]+)[uUlL]*$");
    if (std::regex_match(text, m, hex_re)) {
        try {
            std::string sign = m[1].str();
            std::string digits = m[2].str().substr(2);
            unsigned long long parsed = std::stoull(digits, nullptr, 16);
            unsigned long long max_ll = static_cast<unsigned long long>(std::numeric_limits<long long>::max());
            if (sign == "-") {
                if (parsed > max_ll + 1ULL) return false;
                if (parsed == max_ll + 1ULL) value = std::to_string(std::numeric_limits<long long>::min());
                else value = "-" + std::to_string(parsed);
            } else {
                value = std::to_string(parsed);
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    std::regex int_re("^[+-]?[0-9]+[uUlL]*$");
    if (std::regex_match(text, int_re)) {
        value = std::regex_replace(text, std::regex("[uUlL]+$"), "");
        if (!value.empty() && value[0] == '+') value.erase(value.begin());
        return !value.empty();
    }

    std::regex float_re("^[+-]?(?:[0-9]+\\.[0-9]*|\\.[0-9]+)[fFlL]?$");
    if (std::regex_match(text, float_re)) {
        value = std::regex_replace(text, std::regex("[fFlL]$"), "");
        if (!value.empty() && value[0] == '+') value.erase(value.begin());
        if (value.rfind("-.", 0) == 0) value = "-0" + value.substr(1);
        else if (!value.empty() && value[0] == '.') value = "0" + value;
        return !value.empty();
    }

    return false;
}

static bool bind_c_string_constant_value(const std::string& raw, std::string& value) {
    std::string text = bind_c_constant_text(raw);
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') return false;
    std::string out;
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        char ch = text[i];
        if (ch == '\r' || ch == '\n') return false;
        if (ch == '\\') {
            if (++i + 1 >= text.size()) return false;
            switch (text[i]) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                default: return false;
            }
        } else if (ch == '"') {
            return false;
        } else {
            out.push_back(ch);
        }
    }
    value = out;
    return true;
}

static std::vector<BindCConstant> bind_c_parse_constants(const std::string& header, const std::string& prefix) {
    std::vector<BindCConstant> constants;
    std::string content = std::regex_replace(header, std::regex("/\\*[^*]*\\*+(?:[^/*][^*]*\\*+)*/"), "");
    std::istringstream in(content);
    std::string line;
    std::regex define_re("^\\s*#\\s*define\\s+([A-Za-z_][A-Za-z0-9_]*)\\s+(.+?)\\s*$");
    while (std::getline(in, line)) {
        std::string trimmed = trim_copy(line);
        if (!trimmed.empty() && trimmed.back() == '\\') continue;
        std::smatch m;
        if (!std::regex_match(line, m, define_re)) continue;
        std::string literal;
        std::string kind = "number";
        std::string value;
        if (bind_c_numeric_constant_value(m[2].str(), literal)) {
            value = literal;
        } else if (bind_c_string_constant_value(m[2].str(), value)) {
            kind = "string";
            literal = "\"" + json_escape(value) + "\"";
        } else {
            continue;
        }
        BindCConstant constant;
        constant.symbol = m[1].str();
        constant.name = bind_c_sura_identifier(prefix + constant.symbol, "c_const");
        constant.kind = kind;
        constant.value = value;
        constant.literal = literal;
        constants.push_back(constant);
    }
    return constants;
}

static bool bind_c_integer_constant_value(const std::string& raw, long long& value) {
    std::string literal;
    if (!bind_c_numeric_constant_value(raw, literal)) return false;
    if (literal.find('.') != std::string::npos) return false;
    try {
        value = std::stoll(literal);
        return true;
    } catch (...) {
        return false;
    }
}

static std::vector<BindCConstant> bind_c_parse_enum_constants(const std::string& header, const std::string& prefix) {
    std::vector<BindCConstant> constants;
    std::string content = strip_cpp_class_struct_bodies(strip_c_comments_and_directives(header));
    std::regex enum_re("\\benum(?:\\s+class)?(?:\\s+[A-Za-z_][A-Za-z0-9_]*)?\\s*\\{([^}]*)\\}\\s*(?:[A-Za-z_][A-Za-z0-9_]*)?\\s*;");
    for (auto it = std::sregex_iterator(content.begin(), content.end(), enum_re);
         it != std::sregex_iterator(); ++it) {
        long long next_value = 0;
        bool can_infer_next = true;
        for (std::string entry : bind_c_split_params((*it)[1].str())) {
            entry = trim_copy(std::regex_replace(entry, std::regex("^\\s*\\[\\[[^\\]]+\\]\\]\\s*"), ""));
            if (entry.empty()) continue;
            std::smatch m;
            std::regex item_re("^([A-Za-z_][A-Za-z0-9_]*)(?:\\s*=\\s*(.+))?$");
            if (!std::regex_match(entry, m, item_re)) {
                can_infer_next = false;
                continue;
            }

            long long current = 0;
            if (m[2].matched) {
                if (!bind_c_integer_constant_value(m[2].str(), current)) {
                    can_infer_next = false;
                    continue;
                }
            } else if (can_infer_next) {
                current = next_value;
            } else {
                continue;
            }

            BindCConstant constant;
            constant.symbol = m[1].str();
            constant.name = bind_c_sura_identifier(prefix + constant.symbol, "c_const");
            constant.kind = "number";
            constant.value = std::to_string(current);
            constant.literal = constant.value;
            constants.push_back(constant);

            if (current == std::numeric_limits<long long>::max()) {
                can_infer_next = false;
            } else {
                next_value = current + 1;
                can_infer_next = true;
            }
        }
    }
    return constants;
}

static std::vector<BindCFunction> bind_c_parse_header(const std::string& header, const std::string& prefix) {
    std::vector<BindCFunction> funcs;
    auto aliases = bind_c_parse_type_aliases(header);
    std::string content = strip_cpp_class_struct_bodies(strip_c_comments_and_directives(header));
    std::regex proto_re(
        "(?:extern\\s+(?:\"C\"\\s+)?)?"
        "(?:(?:__declspec\\s*\\([^)]*\\)|\\[\\[[^\\]]+\\]\\]|[A-Z_][A-Z0-9_]*|static|inline|constexpr)\\s+)*"
        "((?:const\\s+)?(?:signed\\s+|unsigned\\s+)?(?:(?:std::)?size_t|ssize_t|u?int(?:8|16|32|64)_t|long\\s+long|long|short|int|double|float|void|char|bool|_Bool|[A-Za-z_][A-Za-z0-9_:]*)\\s*(?:const\\s*)?\\*?)\\s+"
        "([A-Za-z_][A-Za-z0-9_]*)\\s*\\(([^;{}]*)\\)\\s*"
        "(?:noexcept(?:\\s*\\([^)]*\\))?\\s*)?"
        "(?:throw\\s*\\([^)]*\\)\\s*)?"
        "(?:__attribute__\\s*\\(\\([^)]*\\)\\)\\s*)?;"
    );
    for (auto it = std::sregex_iterator(content.begin(), content.end(), proto_re);
         it != std::sregex_iterator(); ++it) {
        BindCFunction fn;
        bool return_supported = true;
        fn.return_type = bind_c_signature_type((*it)[1].str(), true, return_supported, &aliases);
        fn.symbol = (*it)[2].str();
        fn.wrapper = bind_c_sura_identifier(prefix + fn.symbol, "c_func");
        std::string args = (*it)[3].str();
        if (!return_supported) fn.skip_reason = "unsupported return type '" + bind_c_clean_type((*it)[1].str()) + "'";

        auto raw_params = bind_c_split_params(args);
        if (raw_params.size() > 4) fn.skip_reason = "ffi_call supports at most 4 arguments";
        if (fn.skip_reason.empty()) {
            for (size_t i = 0; i < raw_params.size(); ++i) {
                BindCParam param;
                std::string reason;
                if (bind_c_parse_param(raw_params[i], (int)i, param, reason, &aliases)) {
                    fn.params.push_back(param);
                } else if (!reason.empty()) {
                    fn.skip_reason = reason;
                    break;
                }
            }
        }
        if (fn.skip_reason.empty()) fn.skip_reason = bind_c_ffi_shape_skip_reason(fn);
        funcs.push_back(fn);
    }
    return funcs;
}

static bool write_bind_c_json_report(const fs::path& report_path,
                                     const fs::path& header_path,
                                     const fs::path& out_path,
                                     const std::string& lib_path,
                                     const std::string& prefix,
                                     const std::vector<BindCFunction>& funcs,
                                     const std::vector<BindCConstant>& constants,
                                     int emitted,
                                     int skipped,
                                     bool passed) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.bind_c.v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"header\": \"" << json_escape(header_path.generic_string()) << "\",\n"
        << "  \"output\": \"" << json_escape(out_path.generic_string()) << "\",\n"
        << "  \"library\": \"" << json_escape(lib_path) << "\",\n"
        << "  \"prefix\": \"" << json_escape(prefix) << "\",\n"
        << "  \"emitted\": " << emitted << ",\n"
        << "  \"skipped\": " << skipped << ",\n"
        << "  \"emitted_constants\": " << constants.size() << ",\n"
        << "  \"constants\": [\n";
    for (size_t i = 0; i < constants.size(); ++i) {
        const auto& constant = constants[i];
        out << "    {\"symbol\": \"" << json_escape(constant.symbol)
            << "\", \"name\": \"" << json_escape(constant.name)
            << "\", \"kind\": \"" << json_escape(constant.kind)
            << "\", \"value\": \"" << json_escape(constant.value)
            << "\", \"literal\": \"" << json_escape(constant.literal)
            << "\", \"emitted\": true}";
        if (i + 1 < constants.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n"
        << "  \"functions\": [\n";
    for (size_t i = 0; i < funcs.size(); ++i) {
        const auto& fn = funcs[i];
        bool fn_emitted = fn.skip_reason.empty();
        out << "    {\n"
            << "      \"symbol\": \"" << json_escape(fn.symbol) << "\",\n"
            << "      \"wrapper\": \"" << json_escape(fn.wrapper) << "\",\n"
            << "      \"return_type\": \"" << json_escape(fn.return_type) << "\",\n"
            << "      \"emitted\": " << (fn_emitted ? "true" : "false") << ",\n"
            << "      \"skip_reason\": \"" << json_escape(fn.skip_reason) << "\",\n"
            << "      \"params\": [";
        for (size_t j = 0; j < fn.params.size(); ++j) {
            const auto& param = fn.params[j];
            if (j) out << ", ";
            out << "{\"name\": \"" << json_escape(param.name)
                << "\", \"type\": \"" << json_escape(param.type) << "\"}";
        }
        out << "]\n"
            << "    }";
        if (i + 1 < funcs.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int cmd_bind_c(const std::vector<std::string>& argv) {
    std::string header_arg;
    fs::path out_path;
    fs::path json_report;
    std::string lib_path = "PATH_TO_LIBRARY";
    std::string prefix;

    auto need_value = [&](size_t& i, const std::string& option, std::string& value) -> bool {
        if (i + 1 >= argv.size()) {
            err(option + " requires a value");
            return false;
        }
        value = argv[++i];
        return true;
    };

    for (size_t i = 2; i < argv.size(); ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage:\n"
                << "  surapkg bind-c <header.h> [--out native.ffi.sura] [--lib native.dll] [--prefix native_] [--json report.json]\n";
            return 0;
        }
        if (arg == "--out" || arg == "-o") {
            std::string value;
            if (!need_value(i, arg, value)) return 1;
            out_path = utf8_path(value);
        } else if (arg == "--lib" || arg == "--library") {
            if (!need_value(i, arg, lib_path)) return 1;
        } else if (arg == "--prefix") {
            if (!need_value(i, arg, prefix)) return 1;
            prefix = bind_c_sura_identifier(prefix, "c_");
        } else if (arg == "--json") {
            std::string value;
            if (!need_value(i, arg, value)) return 1;
            json_report = utf8_path(value);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = utf8_path(arg.substr(7));
            if (json_report.empty()) return err("bind-c --json requires an output path");
        } else if (!arg.empty() && arg[0] == '-') {
            return err("unknown bind-c option: " + arg);
        } else if (header_arg.empty()) {
            header_arg = arg;
        } else {
            return err("bind-c accepts only one header path");
        }
    }

    if (header_arg.empty()) return err("bind-c requires a header path");
    fs::path header_path = utf8_path(header_arg);
    std::string header = read_all(header_path);
    if (header.empty()) return err("cannot read header: " + header_path.generic_string());
    if (out_path.empty()) {
        out_path = header_path;
        out_path.replace_extension(".ffi.sura");
    }

    auto constants = bind_c_parse_constants(header, prefix);
    auto enum_constants = bind_c_parse_enum_constants(header, prefix);
    constants.insert(constants.end(), enum_constants.begin(), enum_constants.end());
    auto funcs = bind_c_parse_header(header, prefix);
    std::ostringstream out;
    out << "// Generated by surapkg bind-c from " << header_path.generic_string() << "\n"
        << "// ffi_call supports 0..4 numeric C ABI arguments plus numeric, void, and char* returns; simple #define and enum constants are emitted as Sura values.\n";
    for (const auto& constant : constants) {
        out << constant.name << " is " << constant.literal << "\n";
    }
    if (!constants.empty()) out << "\n";
    out << "lib is ffi_load(\"" << json_escape(lib_path) << "\")\n\n";

    int emitted = 0;
    int skipped = 0;
    for (const auto& fn : funcs) {
        if (!fn.skip_reason.empty()) {
            ++skipped;
            out << "// skipped " << fn.symbol << ": " << fn.skip_reason << "\n\n";
            continue;
        }
        ++emitted;
        std::vector<std::string> names;
        std::vector<std::string> types;
        for (const auto& p : fn.params) {
            names.push_back(p.name);
            types.push_back(p.type);
        }
        out << "func " << fn.wrapper << "(";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) out << ", ";
            out << names[i];
        }
        out << ") do\n"
            << "  return ffi_call(lib, \"" << fn.symbol << "\", \"" << fn.return_type
            << "(";
        for (size_t i = 0; i < types.size(); ++i) {
            if (i) out << ",";
            out << types[i];
        }
        out << ")\"";
        for (const auto& name : names) out << ", " << name;
        out << ")\nend\n\n";
    }

    if (!write_all(out_path, out.str())) return err("failed to write " + out_path.generic_string());
    if (!json_report.empty()) {
        if (!write_bind_c_json_report(json_report, header_path, out_path, lib_path, prefix, funcs, constants,
                                      emitted, skipped, emitted > 0)) {
            return err("failed to write bind-c JSON report: " + json_report.generic_string());
        }
        ok("bind-c report written: " + json_report.generic_string());
    }
    ok("wrote " + out_path.generic_string() + " with " + std::to_string(emitted) +
       " binding(s), " + std::to_string(skipped) + " skipped, " +
       std::to_string(constants.size()) + " constant(s)");
    return emitted == 0 ? err("no supported C ABI prototypes found") : 0;
}

static std::string shell_quote(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') out += "\\\"";
        else out += ch;
    }
    out += "\"";
    return out;
}

static std::string test_engine_path() {
#ifdef _WIN32
    std::string env = getenv_utf8(L"SURA_ENGINE");
    if (!env.empty()) return env;

    wchar_t module_path[32768] = {};
    unsigned long module_len = GetModuleFileNameW(nullptr, module_path,
        static_cast<unsigned long>(sizeof(module_path) / sizeof(module_path[0])));
    if (module_len > 0 && module_len < sizeof(module_path) / sizeof(module_path[0])) {
        // Keep the module directory entirely wide. On MinGW, constructing a
        // std::filesystem::path from this value can apply the active ANSI code
        // page and mojibake non-ASCII directories before u8string() sees it.
        std::wstring module(module_path, module_len);
        size_t slash = module.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            std::wstring bin_dir = module.substr(0, slash);
            std::wstring language = bin_dir + L"\\SuraLanguage.exe";
            std::wstring engine = bin_dir + L"\\SuraEngine.exe";
            constexpr unsigned long invalid_attributes = 0xFFFFFFFFUL;
            if (GetFileAttributesW(language.c_str()) != invalid_attributes) {
                return wide_to_utf8(language.c_str());
            }
            if (GetFileAttributesW(engine.c_str()) != invalid_attributes) {
                return wide_to_utf8(engine.c_str());
            }
        }
    }
#else
    const char* env = std::getenv("SURA_ENGINE");
    if (env && *env) return env;
#endif
#ifdef _WIN32
    if (fs::exists("SuraLanguage.exe")) return "SuraLanguage.exe";
    if (fs::exists("SuraEngine.exe")) return "SuraEngine.exe";
    fs::path dir = fs::current_path();
    for (int i = 0; i < 5; ++i) {
        fs::path language = dir / "SuraLanguage.exe";
        fs::path engine = dir / "SuraEngine.exe";
        if (fs::exists(language)) return path_to_utf8(language);
        if (fs::exists(engine)) return path_to_utf8(engine);
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "SuraLanguage.exe";
#else
    fs::path dir = fs::current_path();
    for (int i = 0; i < 5; ++i) {
        fs::path language = dir / "SuraLanguage";
        fs::path engine = dir / "SuraEngine";
        if (fs::exists(language)) return language.string();
        if (fs::exists(engine)) return engine.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "sura";
#endif
}

static std::string cxx_compiler_path() {
    const char* env = std::getenv("CXX");
    if (env && *env) return env;
#ifdef _WIN32
    fs::path mingw = "C:/msys64/mingw64/bin/g++.exe";
    if (fs::exists(mingw)) return mingw.string();
#endif
    return "g++";
}

static std::string cpp_byte_array(const std::string& data, const std::string& name) {
    std::ostringstream out;
    out << "static const unsigned char " << name << "[] = {\n";
    for (size_t i = 0; i < data.size(); ++i) {
        if ((i % 16) == 0) out << "  ";
        out << (unsigned int)(unsigned char)data[i];
        if (i + 1 != data.size()) out << ",";
        out << ((i % 16) == 15 ? "\n" : " ");
    }
    if (data.empty() || (data.size() % 16) != 0) out << "\n";
    out << "};\n";
    return out.str();
}

static std::string launcher_secret_bytes(size_t size) {
    std::string key(size, '\0');
    uint64_t seed = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    try {
        std::random_device rd;
        seed ^= ((uint64_t)rd() << 32) ^ (uint64_t)rd();
    } catch (...) {
        seed ^= 0xD1B54A32D192ED03ULL;
    }
    std::mt19937_64 rng(seed);
    for (char& ch : key) ch = (char)(rng() & 0xFF);
    return key;
}

static std::string release_launcher_source(const std::string& package_bytes) {
    std::string key = launcher_secret_bytes(32);
    std::string encoded = package_bytes;
    for (size_t i = 0; i < encoded.size(); ++i) {
        unsigned char mask = (unsigned char)(((i * 131u) + 17u + (i >> 8)) & 0xFFu);
        encoded[i] = (char)((unsigned char)package_bytes[i] ^ (unsigned char)key[i % key.size()] ^ mask);
    }

    std::ostringstream src;
    src
        << "#include <cstdio>\n"
        << "#include <cstdlib>\n"
        << "#include <fstream>\n"
        << "#include <string>\n"
        << "#ifdef _WIN32\n"
        << "#include <windows.h>\n"
        << "#endif\n\n"
        << cpp_byte_array(encoded, "kPackageData") << "\n"
        << cpp_byte_array(key, "kPackageKey") << "\n"
        << "static std::string embedded_package() {\n"
        << "    std::string data; data.resize(sizeof(kPackageData));\n"
        << "    for (size_t i = 0; i < sizeof(kPackageData); ++i) {\n"
        << "        unsigned char mask = (unsigned char)(((i * 131u) + 17u + (i >> 8)) & 0xFFu);\n"
        << "        data[i] = (char)(kPackageData[i] ^ kPackageKey[i % sizeof(kPackageKey)] ^ mask);\n"
        << "    }\n"
        << "    return data;\n"
        << "}\n"
        << "static bool file_exists(const std::string& path) {\n"
        << "#ifdef _WIN32\n"
        << "    DWORD attrs = GetFileAttributesA(path.c_str());\n"
        << "    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);\n"
        << "#else\n"
        << "    std::ifstream in(path, std::ios::binary); return (bool)in;\n"
        << "#endif\n"
        << "}\n"
        << "static std::string quote_arg(const std::string& value) {\n"
        << "    std::string out = \"\\\"\";\n"
        << "    for (char ch : value) out += (ch == '\\\"') ? \"\\\\\\\"\" : std::string(1, ch);\n"
        << "    out += \"\\\"\"; return out;\n"
        << "}\n"
        << "static std::string module_dir() {\n"
        << "#ifdef _WIN32\n"
        << "    char path[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);\n"
        << "    std::string full(path, n); size_t pos = full.find_last_of(\"\\\\/\");\n"
        << "    return pos == std::string::npos ? std::string(\".\") : full.substr(0, pos);\n"
        << "#else\n"
        << "    return \".\";\n"
        << "#endif\n"
        << "}\n"
        << "static std::string temp_package_path() {\n"
        << "#ifdef _WIN32\n"
        << "    char dir[MAX_PATH]; GetTempPathA(MAX_PATH, dir);\n"
        << "    return std::string(dir) + \"sura_embedded_\" + std::to_string(GetCurrentProcessId()) + \"_\" + std::to_string(GetTickCount64()) + \".sura.srp\";\n"
        << "#else\n"
        << "    return std::string(\"/tmp/sura_embedded_\") + std::to_string(std::rand()) + \".sura.srp\";\n"
        << "#endif\n"
        << "}\n"
        << "int main(int argc, char** argv) {\n"
        << "    std::string package_path = temp_package_path();\n"
        << "    std::string package = embedded_package();\n"
        << "    { std::ofstream out(package_path, std::ios::binary | std::ios::trunc); out.write(package.data(), (std::streamsize)package.size()); if (!out) return 2; }\n"
        << "    const char* env_engine = std::getenv(\"SURA_ENGINE\");\n"
        << "    std::string engine = (env_engine && *env_engine) ? std::string(env_engine) : module_dir() + \"\\\\SuraLanguage.exe\";\n"
        << "    if (!file_exists(engine)) engine = \"SuraLanguage.exe\";\n"
        << "#ifdef _WIN32\n"
        << "    std::string cmd = \"call \" + quote_arg(engine) + \" --load-release \" + quote_arg(package_path);\n"
        << "#else\n"
        << "    std::string cmd = quote_arg(engine) + \" --load-release \" + quote_arg(package_path);\n"
        << "#endif\n"
        << "    for (int i = 1; i < argc; ++i) cmd += \" \" + quote_arg(argv[i]);\n"
        << "    int code = std::system(cmd.c_str());\n"
        << "    std::remove(package_path.c_str());\n"
        << "    return code;\n"
        << "}\n";
    return src.str();
}

static bool build_release_launcher_exe(const fs::path& package_path, const fs::path& exe_path, std::string& output) {
    std::string package_bytes = read_all(package_path);
    if (package_bytes.empty()) {
        output = "protected package is empty or unreadable: " + package_path.generic_string();
        return false;
    }
    fs::path cpp_path = fs::temp_directory_path() /
        ("sura_protect_launcher_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".cpp");
    if (!write_all(cpp_path, release_launcher_source(package_bytes))) {
        output = "failed to write launcher source: " + cpp_path.generic_string();
        return false;
    }
    if (exe_path.has_parent_path()) fs::create_directories(exe_path.parent_path());
#ifdef _WIN32
    std::string cmd = "call " + shell_quote(cxx_compiler_path()) + " -std=c++17 -O2 -s " +
                      shell_quote(cpp_path.string()) + " -o " + shell_quote(exe_path.string()) + " 2>&1";
#else
    std::string cmd = shell_quote(cxx_compiler_path()) + " -std=c++17 -O2 -s " +
                      shell_quote(cpp_path.string()) + " -o " + shell_quote(exe_path.string()) + " 2>&1";
#endif
    int code = run_capture_command_status(cmd, output);
    std::error_code ec;
    fs::remove(cpp_path, ec);
    return code == 0 && fs::exists(exe_path);
}

struct ProtectLeakProbe {
    std::string kind;
    std::string source;
    std::string bytes;
    bool sensitive = false;
};

struct ProtectLeakFinding {
    std::string target_kind;
    std::string target_path;
    std::string probe_kind;
    std::string probe_source;
    std::string sample;
};

static bool contains_bytes(const std::string& haystack, const std::string& needle) {
    return !needle.empty() && haystack.find(needle) != std::string::npos;
}

static std::string trim_ascii_ws(std::string text) {
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text.erase(0, 3);
    }
    while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
    while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
    return text;
}

static std::string protect_sample(const std::string& value, bool sensitive) {
    if (sensitive) return "<redacted>";
    std::string out = value.substr(0, std::min<size_t>(value.size(), 80));
    for (char& ch : out) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    if (value.size() > out.size()) out += "...";
    return out;
}

static std::string protect_action_for_finding(const ProtectLeakFinding& finding) {
    if (finding.probe_kind == "release-key") {
        return "rotate the release key, rebuild with --key-file, and keep keys outside shipped artifacts";
    }
    if (finding.probe_kind == "release-license") {
        return "rotate the release license, rebuild with --license-file, and keep licenses outside shipped artifacts";
    }
    if (finding.probe_kind == "string-literal") {
        return "move sensitive or proprietary literals out of client code, then rerun surapkg protect";
    }
    if (finding.probe_kind == "source-file" || finding.probe_kind == "source-line") {
        return "remove raw source text from protected outputs, rebuild, and ship only the .sura.srp or launcher";
    }
    return "inspect the protected artifact, remove leaked bytes, and rerun surapkg protect";
}

static bool path_has_component(const fs::path& path, const std::string& component) {
    for (const auto& part : path) {
        if (part.string() == component) return true;
    }
    return false;
}

static bool protect_should_scan_source(const fs::path& root, const fs::path& file) {
    std::error_code ec;
    fs::path rel = fs::relative(file, root, ec);
    if (ec) rel = file;
    if (path_has_component(rel, ".git") || path_has_component(rel, "packages") ||
        path_has_component(rel, "dist") || path_has_component(rel, "registry") ||
        path_has_component(rel, "artifacts")) {
        return false;
    }
    return file.extension() == ".sura";
}

static std::vector<fs::path> protect_source_files(const fs::path& root, const fs::path& main_path) {
    std::vector<fs::path> files;
    std::set<std::string> seen;
    auto add = [&](const fs::path& file) {
        std::error_code ec;
        fs::path norm = fs::weakly_canonical(file, ec);
        std::string key = (ec ? file.lexically_normal() : norm).generic_string();
        if (seen.insert(key).second) files.push_back(file);
    };
    if (fs::exists(main_path) && protect_should_scan_source(root, main_path)) add(main_path);
    if (fs::is_directory(root)) {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && protect_should_scan_source(root, entry.path())) {
                add(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

static void protect_add_probe(std::vector<ProtectLeakProbe>& probes, const std::string& kind,
                              const std::string& source, const std::string& bytes,
                              bool sensitive = false, size_t min_len = 8) {
    if (bytes.size() < min_len) return;
    for (const auto& probe : probes) {
        if (probe.bytes == bytes && probe.kind == kind) return;
    }
    probes.push_back({kind, source, bytes, sensitive});
}

static void protect_add_string_literal_probes(std::vector<ProtectLeakProbe>& probes,
                                              const std::string& source_name,
                                              const std::string& text) {
    bool in_string = false;
    char quote = 0;
    bool escaped = false;
    std::string current;
    for (char ch : text) {
        if (!in_string) {
            if (ch == '"' || ch == '\'') {
                in_string = true;
                quote = ch;
                escaped = false;
                current.clear();
            }
            continue;
        }
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            current.push_back(ch);
            escaped = true;
            continue;
        }
        if (ch == quote) {
            protect_add_probe(probes, "string-literal", source_name, current, false, 4);
            in_string = false;
            quote = 0;
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
}

static std::vector<ProtectLeakProbe> protect_build_leak_probes(const fs::path& root,
                                                               const fs::path& main_path,
                                                               const std::string& release_key,
                                                               const std::string& release_license,
                                                               const std::string& release_key_file,
                                                               const std::string& release_license_file,
                                                               int& source_files_scanned,
                                                               size_t& source_bytes_scanned) {
    std::vector<ProtectLeakProbe> probes;
    source_files_scanned = 0;
    source_bytes_scanned = 0;
    for (const auto& file : protect_source_files(root, main_path)) {
        std::string text = read_all(file);
        if (text.empty()) continue;
        ++source_files_scanned;
        source_bytes_scanned += text.size();
        std::error_code ec;
        fs::path rel = fs::relative(file, root, ec);
        std::string source_name = (ec ? file : rel).generic_string();
        if (text.size() >= 16 && text.size() <= 65536) {
            protect_add_probe(probes, "source-file", source_name, text, false, 16);
        }
        std::istringstream lines(text);
        std::string line;
        int line_no = 0;
        while (std::getline(lines, line)) {
            ++line_no;
            // getline keeps the carriage return of a CRLF file. Rules that
            // match a whole line (assignments, for one) then never fire, so a
            // file saved on Windows would silently lose findings that the same
            // file with LF endings reports.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string trimmed = trim_ascii_ws(line);
            if (trimmed.empty() || trimmed.rfind("//", 0) == 0 || trimmed.rfind("#", 0) == 0) continue;
            protect_add_probe(probes, "source-line", source_name + ":" + std::to_string(line_no),
                              trimmed, false, 16);
        }
        protect_add_string_literal_probes(probes, source_name, text);
    }

    std::string key_value = !release_key.empty() ? release_key : trim_ascii_ws(read_all(release_key_file));
    std::string license_value = !release_license.empty() ? release_license : trim_ascii_ws(read_all(release_license_file));
    protect_add_probe(probes, "release-key", "protect option", key_value, true, 4);
    protect_add_probe(probes, "release-license", "protect option", license_value, true, 4);
    return probes;
}

static std::string protect_leak_report_json(const std::vector<std::pair<std::string, fs::path>>& targets,
                                            const std::vector<ProtectLeakFinding>& findings,
                                            int source_files_scanned,
                                            size_t source_bytes_scanned,
                                            size_t probe_count,
                                            const std::string& mode,
                                            bool keyed,
                                            bool licensed,
                                            const std::string& release_expires) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.protect.v1\",\n"
        << "  \"version\": 1,\n"
        << "  \"mode\": \"" << json_escape(mode.empty() ? "standard" : mode) << "\",\n"
        << "  \"status\": \"" << (findings.empty() ? "PASS" : "FAIL") << "\",\n"
        << "  \"passed\": " << (findings.empty() ? "true" : "false") << ",\n"
        << "  \"keyed\": " << (keyed ? "true" : "false") << ",\n"
        << "  \"licensed\": " << (licensed ? "true" : "false") << ",\n"
        << "  \"expires\": \"" << json_escape(release_expires) << "\",\n"
        << "  \"next_actions\": [\n";
    if (findings.empty()) {
        out << "    {\"action\":\"ship only the protected .sura.srp or launcher; do not distribute original .sura sources\"}";
        if (keyed) {
            out << ",\n    {\"action\":\"provide the release key at runtime via --load-release-key-file or SURA_RELEASE_KEY, outside the package\"}";
        } else {
            out << ",\n    {\"action\":\"for stronger commercial distribution, rerun with --closed-source and --key-file\"}";
        }
        if (licensed) {
            out << ",\n    {\"action\":\"provide the release license at runtime via --load-release-license-file or SURA_RELEASE_LICENSE\"}";
        } else {
            out << ",\n    {\"action\":\"add --license-file when customer or seat-level control is required\"}";
        }
        if (release_expires.empty()) {
            out << ",\n    {\"action\":\"add --expires YYYY-MM-DD when the release should stop running after a date\"}";
        }
        out << ",\n    {\"action\":\"keep this protect JSON report as CI/release evidence\"}\n";
    } else {
        for (size_t i = 0; i < findings.size(); ++i) {
            const auto& finding = findings[i];
            out << "    {\"target\":\"" << json_escape(finding.target_kind)
                << "\",\"kind\":\"" << json_escape(finding.probe_kind)
                << "\",\"source\":\"" << json_escape(finding.probe_source)
                << "\",\"action\":\"" << json_escape(protect_action_for_finding(finding)) << "\"}";
            out << (i + 1 == findings.size() ? "\n" : ",\n");
        }
    }
    out << "  ],\n"
        << "  \"sourceFilesScanned\": " << source_files_scanned << ",\n"
        << "  \"sourceBytesScanned\": " << source_bytes_scanned << ",\n"
        << "  \"probes\": " << probe_count << ",\n"
        << "  \"targets\": [\n";
    for (size_t i = 0; i < targets.size(); ++i) {
        out << "    {\"kind\":\"" << json_escape(targets[i].first)
            << "\",\"path\":\"" << json_escape(targets[i].second.generic_string()) << "\"}"
            << (i + 1 == targets.size() ? "\n" : ",\n");
    }
    out << "  ],\n"
        << "  \"findings\": [\n";
    for (size_t i = 0; i < findings.size(); ++i) {
        const auto& finding = findings[i];
        out << "    {\"target\":\"" << json_escape(finding.target_kind)
            << "\",\"path\":\"" << json_escape(finding.target_path)
            << "\",\"kind\":\"" << json_escape(finding.probe_kind)
            << "\",\"source\":\"" << json_escape(finding.probe_source)
            << "\",\"sample\":\"" << json_escape(finding.sample) << "\"}"
            << (i + 1 == findings.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

static int run_protect_leak_scan(const fs::path& root,
                                 const fs::path& main_path,
                                 const fs::path& package_path,
                                 const fs::path& launcher_exe,
                                 const fs::path& report_path,
                                 const std::string& release_key,
                                 const std::string& release_license,
                                 const std::string& release_key_file,
                                 const std::string& release_license_file,
                                 const std::string& mode,
                                 bool keyed,
                                 bool licensed,
                                 const std::string& release_expires) {
    int source_files_scanned = 0;
    size_t source_bytes_scanned = 0;
    auto probes = protect_build_leak_probes(root, main_path, release_key, release_license,
                                           release_key_file, release_license_file,
                                           source_files_scanned, source_bytes_scanned);
    std::vector<std::pair<std::string, fs::path>> targets;
    targets.push_back({"package", package_path});
    if (!launcher_exe.empty()) targets.push_back({"launcher", launcher_exe});

    std::vector<ProtectLeakFinding> findings;
    for (const auto& target : targets) {
        std::string data = read_all(target.second);
        for (const auto& probe : probes) {
            if (contains_bytes(data, probe.bytes)) {
                findings.push_back({target.first, target.second.generic_string(), probe.kind,
                                    probe.source, protect_sample(probe.bytes, probe.sensitive)});
            }
        }
    }

    if (!report_path.empty()) {
        if (!write_all(report_path, protect_leak_report_json(targets, findings, source_files_scanned,
                                                            source_bytes_scanned, probes.size(),
                                                            mode, keyed, licensed, release_expires))) {
            return err("failed to write protect leak report: " + report_path.generic_string());
        }
    }
    if (!findings.empty()) {
        size_t shown = std::min<size_t>(findings.size(), 5);
        for (size_t i = 0; i < shown; ++i) {
            std::cerr << "[leak] " << findings[i].target_kind << " contains "
                      << findings[i].probe_kind << " from " << findings[i].probe_source
                      << ": " << findings[i].sample << "\n";
        }
        if (findings.size() > shown) {
            std::cerr << "[leak] " << (findings.size() - shown) << " more finding(s)\n";
        }
        return err("protect leak scan failed: source or secret bytes found in protected artifact");
    }
    ok("protect leak scan passed" + (report_path.empty() ? std::string() : " -> " + report_path.generic_string()));
    return 0;
}

static bool looks_like_test_file(const fs::path& file) {
    if (file.extension() != ".sura") return false;
    std::string name = file.filename().string();
    return name.rfind("test_", 0) == 0 ||
           (name.size() >= 10 && name.substr(name.size() - 10) == "_test.sura") ||
           (name.size() >= 10 && name.substr(name.size() - 10) == ".test.sura");
}

static std::vector<fs::path> discover_tests(const fs::path& input) {
    std::vector<fs::path> tests;
    if (fs::is_regular_file(input)) {
        if (input.extension() == ".sura") tests.push_back(input);
        return tests;
    }
    if (!fs::is_directory(input)) return tests;

    fs::path tests_dir = input / "tests";
    fs::path root = fs::is_directory(tests_dir) ? tests_dir : input;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sura") continue;
        if (root == tests_dir || looks_like_test_file(entry.path())) tests.push_back(entry.path());
    }
    std::sort(tests.begin(), tests.end());
    return tests;
}

struct QualityItem {
    std::string status;
    int earned = 0;
    int possible = 0;
    std::string message;
};

struct QualityReport {
    int score = 0;
    int possible = 0;
    int warnings = 0;
    int errors = 0;
    std::vector<QualityItem> items;
};

static void quality_item(QualityReport& report, const std::string& status, int earned, int possible, const std::string& msg) {
    report.score += earned;
    report.possible += possible;
    if (status == "warn") ++report.warnings;
    if (status == "fail") ++report.errors;
    report.items.push_back({status, earned, possible, msg});
    std::cout << "[" << status << "] +" << earned << "/" << possible << " " << msg << "\n";
}

static bool quality_contains(const std::string& text, const std::string& needle) {
    std::string a = text;
    std::string b = needle;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return a.find(b) != std::string::npos;
}

static std::string quality_item_category(const QualityItem& item) {
    const std::string& msg = item.message;
    if (quality_contains(msg, "manifest")) return "manifest";
    if (quality_contains(msg, "main file") || quality_contains(msg, "source")) return "source";
    if (quality_contains(msg, "test")) return "tests";
    if (quality_contains(msg, "readme") || quality_contains(msg, "docs")) return "docs";
    if (quality_contains(msg, "dependency") || quality_contains(msg, "lockfile")) return "dependencies";
    if (quality_contains(msg, "security") || quality_contains(msg, "audit")) return "security";
    if (quality_contains(msg, "signature")) return "signing";
    return "general";
}

static std::string quality_item_action(const QualityItem& item) {
    if (item.status == "pass") return "";
    const std::string& msg = item.message;
    if (quality_contains(msg, "package manifest missing")) return "create sura.pkg.json with name, version, and main";
    if (quality_contains(msg, "manifest missing name")) return "add name to sura.pkg.json";
    if (quality_contains(msg, "manifest missing version")) return "add version to sura.pkg.json";
    if (quality_contains(msg, "manifest missing main")) return "add main to sura.pkg.json";
    if (quality_contains(msg, "main file missing")) return "create the package main source file";
    if (quality_contains(msg, "source layout") || quality_contains(msg, "Sura source files: 0")) return "add Sura source files under src/";
    if (quality_contains(msg, "no tests") || quality_contains(msg, "tests not checked")) return "add tests under tests/ or test_*.sura";
    if (quality_contains(msg, "README") || quality_contains(msg, "docs/index.html")) return "add README.md or run surapkg docs";
    if (quality_contains(msg, "lockfile missing")) return "run surapkg lock";
    if (quality_contains(msg, "dependency install state")) return "run surapkg restore and resolve dependency versions";
    if (quality_contains(msg, "security audit")) return "run surapkg audit and add signed tool/plugin policies where needed";
    if (quality_contains(msg, "signature")) return "run surapkg sign";
    return "review this quality finding";
}

static std::string quality_grade(const QualityReport& report) {
    if (report.score >= 95) return "A+";
    if (report.score >= 90) return "A";
    if (report.score >= 80) return "B";
    if (report.score >= 70) return "C";
    return "F";
}

static bool quality_passed(const QualityReport& report) {
    return report.score >= 80 && report.errors == 0;
}

static std::string quality_report_json(const fs::path& root,
                                       const std::string& name,
                                       const std::string& version,
                                       const QualityReport& report) {
    std::string grade = quality_grade(report);
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"sura.package.quality.v1\",\n";
    out << "  \"root\": \"" << json_escape(root.generic_string()) << "\",\n";
    out << "  \"package\": \"" << json_escape(name) << "\",\n";
    out << "  \"version\": \"" << json_escape(version) << "\",\n";
    out << "  \"score\": " << report.score << ",\n";
    out << "  \"possible\": " << report.possible << ",\n";
    out << "  \"grade\": \"" << json_escape(grade) << "\",\n";
    out << "  \"passed\": " << (quality_passed(report) ? "true" : "false") << ",\n";
    out << "  \"warnings\": " << report.warnings << ",\n";
    out << "  \"errors\": " << report.errors << ",\n";
    out << "  \"next_actions\": [\n";
    bool first_action = true;
    for (const auto& item : report.items) {
        if (item.status == "pass") continue;
        std::string action = quality_item_action(item);
        if (!first_action) out << ",\n";
        first_action = false;
        out << "    {\"category\":\"" << json_escape(quality_item_category(item))
            << "\",\"status\":\"" << json_escape(item.status)
            << "\",\"message\":\"" << json_escape(item.message)
            << "\",\"action\":\"" << json_escape(action) << "\"}";
    }
    out << "\n  ],\n";
    out << "  \"items\": [\n";
    for (size_t i = 0; i < report.items.size(); ++i) {
        const auto& item = report.items[i];
        out << "    {\"status\":\"" << json_escape(item.status)
            << "\",\"earned\":" << item.earned
            << ",\"possible\":" << item.possible
            << ",\"category\":\"" << json_escape(quality_item_category(item))
            << "\",\"message\":\"" << json_escape(item.message)
            << "\",\"action\":\"" << json_escape(quality_item_action(item)) << "\"}";
        if (i + 1 < report.items.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

static int finish_quality_command(const fs::path& root,
                                  const std::string& name,
                                  const std::string& version,
                                  const QualityReport& report,
                                  const fs::path& json_report) {
    std::string grade = quality_grade(report);
    std::cout << "score: " << report.score << "/" << report.possible << " grade=" << grade << "\n";
    std::cout << "quality: " << (quality_passed(report) ? "PASS" : "FAIL") << "\n";
    if (!json_report.empty()) {
        if (!write_all(json_report, quality_report_json(root, name, version, report))) {
            return err("failed to write quality JSON report: " + json_report.generic_string());
        }
        ok("quality report written: " + json_report.generic_string());
    }
    if (!quality_passed(report)) return err("quality score below 80 or required checks failed");
    return 0;
}

static bool package_docs_present(const fs::path& root) {
    return fs::exists(root / "README.md") ||
           fs::exists(root / "readme.md") ||
           fs::exists(root / "docs" / "index.html");
}

static int run_quality_command(const std::string& source, const fs::path& json_report) {
    fs::path root = doctor_package_root(source);
    std::string manifest = read_all(root / kManifest);
    QualityReport report;
    std::cout << "Sura package quality\n";
    std::cout << "root: " << root.generic_string() << "\n";

    if (manifest.empty()) {
        quality_item(report, "fail", 0, 20, "package manifest missing: " + (root / kManifest).generic_string());
        quality_item(report, "fail", 0, 15, "source layout cannot be checked without a manifest");
        quality_item(report, "warn", 0, 15, "tests not checked");
        quality_item(report, "warn", 0, 10, "README.md or docs/index.html missing");
        quality_item(report, "warn", 0, 15, "dependency hygiene not checked");
        quality_item(report, "fail", 0, 20, "security audit cannot run without a package root");
        quality_item(report, "warn", 0, 5, "package signature missing");
        return finish_quality_command(root, "", "", report, json_report);
    }

    std::string name = normalize_name(manifest_field(manifest, "name", ""));
    std::string version = manifest_field(manifest, "version", "");
    std::string main_file = manifest_field(manifest, "main", "");
    std::cout << "package: " << (name.empty() ? "<unnamed>" : name)
              << "@" << (version.empty() ? "0.0.0" : version) << "\n";

    quality_item(report, "pass", 5, 5, "manifest present");
    quality_item(report, name.empty() ? "fail" : "pass", name.empty() ? 0 : 5, 5,
                 name.empty() ? "manifest missing name" : "manifest name: " + name);
    quality_item(report, version.empty() ? "warn" : "pass", version.empty() ? 0 : 5, 5,
                 version.empty() ? "manifest missing version" : "manifest version: " + version);
    quality_item(report, main_file.empty() ? "fail" : "pass", main_file.empty() ? 0 : 5, 5,
                 main_file.empty() ? "manifest missing main" : "manifest main: " + main_file);

    bool main_exists = !main_file.empty() && fs::exists(root / main_file);
    quality_item(report, main_exists ? "pass" : "fail", main_exists ? 10 : 0, 10,
                 main_exists ? "main file exists" : "main file missing: " + (root / main_file).generic_string());
    auto sura_files = package_sura_files(root);
    quality_item(report, sura_files.empty() ? "fail" : "pass", sura_files.empty() ? 0 : 5, 5,
                 "Sura source files: " + std::to_string(sura_files.size()));

    auto tests = discover_tests(root);
    quality_item(report, tests.empty() ? "warn" : "pass", tests.empty() ? 0 : 15, 15,
                 tests.empty() ? "no tests found under tests/ or test_*.sura" :
                                 "tests found: " + std::to_string(tests.size()));

    bool docs_present = package_docs_present(root);
    quality_item(report, docs_present ? "pass" : "warn", docs_present ? 10 : 0, 10,
                 docs_present ? "README.md or docs/index.html present" :
                                "README.md or generated docs/index.html missing");

    auto deps = manifest_dependency_specs(manifest);
    if (deps.empty()) {
        quality_item(report, "pass", 15, 15, "dependencies: none");
    } else {
        bool has_lock = fs::exists(root / kLockfile);
        quality_item(report, has_lock ? "pass" : "warn", has_lock ? 5 : 0, 5,
                     has_lock ? "lockfile present" : "lockfile missing for package with dependencies");
        int missing = 0;
        int mismatched = 0;
        for (const auto& dep : deps) {
            fs::path dep_root = root / kPackages / dep.name;
            if (!fs::exists(dep_root)) {
                ++missing;
                continue;
            }
            std::string installed_manifest = read_all(dep_root / kManifest);
            std::string installed_version = manifest_field(installed_manifest, "version", "0.0.0");
            std::string constraint = dependency_constraint(dep.name, dep.spec);
            if (!version_satisfies_constraint(installed_version, constraint)) ++mismatched;
        }
        bool deps_ok = missing == 0 && mismatched == 0;
        std::ostringstream dep_msg;
        dep_msg << "dependency install state: " << deps.size() << " declared, "
                << missing << " missing, " << mismatched << " version mismatched";
        quality_item(report, deps_ok ? "pass" : "fail", deps_ok ? 10 : 0, 10, dep_msg.str());
    }

    int audit_findings = audit_package_findings(root, false);
    quality_item(report, audit_findings == 0 ? "pass" : "fail", audit_findings == 0 ? 20 : 0, 20,
                 audit_findings == 0 ? "security audit passed" :
                                       "security audit findings: " + std::to_string(audit_findings));

    bool signature_present = fs::exists(root / kSignature) && !read_all(root / kSignature).empty();
    quality_item(report, signature_present ? "pass" : "warn", signature_present ? 5 : 0, 5,
                 signature_present ? "package signature present" : "package signature missing; run surapkg sign");

    return finish_quality_command(root, name, version, report, json_report);
}

static int cmd_quality(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("quality --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("quality --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg quality [path] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("quality accepts at most one path");
        }
    }
    return run_quality_command(source, json_report);
}

struct CleanEntry {
    fs::path path;
    std::string kind;
    std::string reason;
    bool directory = false;
};

static bool clean_exact_file_name(const std::string& name) {
    static const std::set<std::string> names = {
        "build_output.txt", "surapkg_build_output.txt", "build_log.txt", "build_errors.txt",
        "bld.log", "err.txt", "out.txt", "jit_err.txt", "jit_errors.txt", "jit_compile_err.txt",
        "link_errors.txt", "main_obj_errors.txt", "diag_main_err.txt", "diag_main_out.txt",
        "test_out.txt"
    };
    return names.count(name) != 0;
}

static bool clean_starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

static bool clean_ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::vector<CleanEntry> discover_clean_entries(const fs::path& root) {
    std::vector<CleanEntry> entries;
    if (!fs::is_directory(root)) return entries;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        std::string name = entry.path().filename().generic_string();
        std::error_code type_ec;
        if (entry.is_directory(type_ec) && clean_starts_with(name, "sura_walk_")) {
            entries.push_back({entry.path(), "directory", "temporary file_walk smoke output", true});
            continue;
        }
        type_ec.clear();
        if (!entry.is_regular_file(type_ec)) continue;
        if (clean_exact_file_name(name)) {
            entries.push_back({entry.path(), "file", "generated build/test log", false});
        } else if (clean_starts_with(name, "sura_world_log_") && clean_ends_with(name, ".jsonl")) {
            entries.push_back({entry.path(), "file", "world feature smoke log", false});
        } else if (clean_starts_with(name, "sura_world_tool_tmp") && clean_ends_with(name, ".txt")) {
            entries.push_back({entry.path(), "file", "world feature tool temp file", false});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const CleanEntry& a, const CleanEntry& b) {
        return a.path.generic_string() < b.path.generic_string();
    });
    return entries;
}

static bool write_clean_json_report(const fs::path& report_path,
                                    const fs::path& root,
                                    bool dry_run,
                                    const std::vector<CleanEntry>& entries,
                                    int removed_count) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"sura.package.clean.v1\",\n"
        << "  \"passed\": true,\n"
        << "  \"root\": \"" << json_escape(path_to_generic_utf8(root)) << "\",\n"
        << "  \"dry_run\": " << (dry_run ? "true" : "false") << ",\n"
        << "  \"matched\": " << entries.size() << ",\n"
        << "  \"removed\": " << removed_count << ",\n"
        << "  \"items\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        std::error_code rel_ec;
        fs::path rel = fs::relative(entries[i].path, root, rel_ec);
        if (rel_ec) rel = entries[i].path;
        out << "    {\"path\":\"" << json_escape(path_to_generic_utf8(rel))
            << "\",\"kind\":\"" << json_escape(entries[i].kind)
            << "\",\"reason\":\"" << json_escape(entries[i].reason)
            << "\",\"removed\":" << (!dry_run ? "true" : "false") << "}";
        if (i + 1 < entries.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
    return write_all(report_path, out.str());
}

static int run_clean_command(const std::string& source, const fs::path& json_report, bool dry_run) {
    fs::path root = source.empty() ? fs::current_path() : utf8_path(source);
    std::error_code ec;
    root = fs::absolute(root, ec).lexically_normal();
    if (ec) return err("failed to resolve clean path: " + ec.message());
    if (!fs::exists(root)) return err("clean path does not exist: " + path_to_generic_utf8(root));
    if (!fs::is_directory(root)) return err("clean path must be a directory: " + path_to_generic_utf8(root));
    if (!root.root_path().empty() && root == root.root_path()) {
        return err("refusing to clean filesystem root: " + path_to_generic_utf8(root));
    }

    auto entries = discover_clean_entries(root);
    int removed_count = 0;
    if (!dry_run) {
        for (const auto& item : entries) {
            std::error_code rm_ec;
            if (item.directory) fs::remove_all(item.path, rm_ec);
            else fs::remove(item.path, rm_ec);
            if (rm_ec) return err("failed to remove " + path_to_generic_utf8(item.path) + ": " + rm_ec.message());
            ++removed_count;
        }
    }

    if (!json_report.empty()) {
        if (!write_clean_json_report(json_report, root, dry_run, entries, removed_count)) {
            return err("failed to write clean JSON report: " + json_report.generic_string());
        }
        ok("clean report written: " + json_report.generic_string());
    }
    if (dry_run) ok("clean would remove " + std::to_string(entries.size()) + " item(s)");
    else ok("clean removed " + std::to_string(removed_count) + " item(s)");
    return 0;
}

static int cmd_clean(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    bool dry_run = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--dry-run" || arg == "-n") {
            dry_run = true;
        } else if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("clean --json requires an output path");
            json_report = utf8_path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = utf8_path(arg.substr(7));
            if (json_report.empty()) return err("clean --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg clean [path] [--dry-run] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("clean accepts at most one path");
        }
    }
    return run_clean_command(source, json_report, dry_run);
}

struct PackageTestResult {
    fs::path file;
    std::string status;
    int code = 0;
    long long ms = 0;
    std::string output;
};

static int run_test_file(const std::string& engine, const fs::path& file, bool use_jit, std::string& output, long long& ms) {
    auto start = std::chrono::steady_clock::now();
    std::string cmd;
#ifdef _WIN32
    cmd = "call " + shell_quote(engine);
#else
    cmd = shell_quote(engine);
#endif
    if (use_jit) cmd += " --jit";
    cmd += " " + shell_quote(path_to_utf8(file)) + " 2>&1";
    int code = run_capture_command_status(cmd, output);
    auto end = std::chrono::steady_clock::now();
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return code;
}

static bool write_junit_report(const fs::path& report_path,
                               const std::vector<PackageTestResult>& results,
                               int passed,
                               int failed,
                               const std::string& engine,
                               bool use_jit) {
    std::ostringstream xml;
    long long total_ms = 0;
    for (const auto& r : results) total_ms += r.ms;

    auto seconds = [](long long ms) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(3) << (double)ms / 1000.0;
        return out.str();
    };

    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<testsuite name=\"sura\" tests=\"" << results.size()
        << "\" failures=\"" << failed
        << "\" errors=\"0\" skipped=\"0\" time=\"" << seconds(total_ms) << "\">\n"
        << "  <properties>\n"
        << "    <property name=\"engine\" value=\"" << html_escape(engine) << "\"/>\n"
        << "    <property name=\"jit\" value=\"" << (use_jit ? "true" : "false") << "\"/>\n"
        << "    <property name=\"passed\" value=\"" << passed << "\"/>\n"
        << "  </properties>\n";

    for (const auto& r : results) {
        std::string path = r.file.generic_string();
        xml << "  <testcase classname=\"sura\" name=\"" << html_escape(path)
            << "\" file=\"" << html_escape(path)
            << "\" time=\"" << seconds(r.ms) << "\">\n";
        if (r.status != "pass") {
            xml << "    <failure message=\"exit code " << r.code << "\">"
                << html_escape(r.output) << "</failure>\n";
        }
        if (!r.output.empty()) {
            xml << "    <system-out>" << html_escape(r.output) << "</system-out>\n";
        }
        xml << "  </testcase>\n";
    }
    xml << "</testsuite>\n";
    return write_all(report_path, xml.str());
}

static int run_package_tests(const fs::path& root,
                             const fs::path& report_path,
                             const fs::path& junit_path = fs::path(),
                             bool use_jit = true) {
    std::vector<fs::path> tests = discover_tests(root);
    if (tests.empty()) return err("no Sura tests found under " + root.generic_string());

    std::string engine = test_engine_path();
    int passed = 0;
    int failed = 0;
    std::vector<PackageTestResult> results;

    for (const auto& test : tests) {
        std::string output;
        long long ms = 0;
        int code = run_test_file(engine, test, use_jit, output, ms);
        std::error_code rel_ec;
        fs::path display = fs::relative(test, root, rel_ec);
        if (rel_ec) display = test;
        if (code == 0) {
            ++passed;
            std::cout << "[PASS] " << display.generic_string() << " (" << ms << " ms)\n";
            results.push_back({display, "pass", code, ms, output});
        } else {
            ++failed;
            std::cout << "[FAIL] " << display.generic_string() << " (" << ms << " ms)\n";
            if (!output.empty()) std::cout << output;
            results.push_back({display, "fail", code, ms, output});
        }
    }

    std::ostringstream report;
    report << "{\n"
           << "  \"schema\": \"sura.package.test.v1\",\n"
           << "  \"version\": 1,\n"
           << "  \"engine\": \"" << json_escape(engine) << "\",\n"
           << "  \"jit\": " << (use_jit ? "true" : "false") << ",\n"
           << "  \"ok\": " << (failed == 0 ? "true" : "false") << ",\n"
           << "  \"total\": " << results.size() << ",\n"
           << "  \"passed\": " << passed << ",\n"
           << "  \"failed\": " << failed << ",\n"
           << "  \"tests\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        report << "    {\"path\":\"" << json_escape(r.file.generic_string())
               << "\",\"status\":\"" << r.status
               << "\",\"exitCode\":" << r.code
               << ",\"durationMs\":" << r.ms
               << ",\"output\":\"" << json_escape(r.output) << "\"}"
               << (i + 1 == results.size() ? "\n" : ",\n");
    }
    report << "  ]\n}\n";
    if (!write_all(report_path, report.str())) return err("failed to write " + report_path.generic_string());
    if (!junit_path.empty()) {
        if (!write_junit_report(junit_path, results, passed, failed, engine, use_jit)) {
            return err("failed to write " + junit_path.generic_string());
        }
    }

    std::cout << "Sura tests: " << passed << " passed, " << failed << " failed\n";
    ok("wrote " + report_path.generic_string());
    if (!junit_path.empty()) ok("wrote " + junit_path.generic_string());
    return failed ? err("test run failed") : 0;
}

static int cmd_test(const std::vector<std::string>& argv) {
    fs::path root = fs::current_path();
    fs::path report_path = fs::current_path() / "sura-test-report.json";
    fs::path junit_path;
    bool use_jit = true;
    bool path_set = false;

    auto need_value = [&](size_t& i, const std::string& option, fs::path& value) -> bool {
        if (i + 1 >= argv.size()) {
            err(option + " requires a path");
            return false;
        }
        value = utf8_path(argv[++i]);
        return true;
    };

    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage:\n"
                << "  surapkg test [path] [--json file.json|--report file.json] [--junit file.xml] [--no-jit]\n";
            return 0;
        }
        if (arg == "--json" || arg == "--report" || arg == "-r") {
            if (!need_value(i, arg, report_path)) return 1;
            continue;
        }
        if (arg.rfind("--json=", 0) == 0) {
            report_path = utf8_path(arg.substr(7));
            if (report_path.empty()) return err("test --json requires a path");
            continue;
        }
        if (arg == "--junit") {
            if (!need_value(i, arg, junit_path)) return 1;
            continue;
        }
        if (arg == "--no-jit") {
            use_jit = false;
            continue;
        }
        if (!path_set && (arg.empty() || arg[0] != '-')) {
            root = utf8_path(arg);
            path_set = true;
            continue;
        }
        return err("unexpected test argument: " + arg);
    }

    return run_package_tests(root, report_path, junit_path, use_jit);
}

static int cmd_run(const std::vector<std::string>& argv) {
    fs::path root = fs::current_path();
    fs::path json_path;
    std::vector<std::string> script_args;
    bool collect_script_args = false;
    bool use_jit = true;
    bool path_set = false;

    auto need_path_value = [&](size_t& i, const std::string& option, fs::path& value) -> bool {
        if (i + 1 >= argv.size()) {
            err(option + " requires a path");
            return false;
        }
        value = utf8_path(argv[++i]);
        return true;
    };

    for (size_t i = 2; i < argv.size(); ++i) {
        std::string arg = argv[i];
        if (collect_script_args) {
            script_args.push_back(arg);
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage:\n"
                << "  surapkg run [package-path] [--json run.json] [--no-jit] [--] [args...]\n";
            return 0;
        }
        if (arg == "--") {
            collect_script_args = true;
            continue;
        }
        if (arg == "--json") {
            if (!need_path_value(i, arg, json_path)) return 1;
            continue;
        }
        if (arg.rfind("--json=", 0) == 0) {
            json_path = utf8_path(arg.substr(7));
            if (json_path.empty()) return err("run --json requires a path");
            continue;
        }
        if (arg == "--no-jit") {
            use_jit = false;
            continue;
        }
        if (!path_set && (arg.empty() || arg[0] != '-')) {
            root = utf8_path(arg);
            path_set = true;
            continue;
        }
        script_args.push_back(arg);
    }

    if (fs::is_regular_file(root)) root = root.parent_path();
    root = fs::absolute(root).lexically_normal();
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return err("sura.pkg.json not found for run: " + path_to_generic_utf8(root / kManifest));

    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "");
    std::string main_file = manifest_field(manifest, "main", "");
    if (main_file.empty()) return err("package manifest has no main field");
    fs::path main_path = (root / utf8_path(main_file)).lexically_normal();
    if (!fs::exists(main_path) || !fs::is_regular_file(main_path)) {
        return err("package main file missing: " + path_to_generic_utf8(main_path));
    }

    fs::path old_cwd = fs::current_path();
    std::error_code abs_ec;
    fs::path engine = fs::absolute(utf8_path(test_engine_path()), abs_ec).lexically_normal();
    if (abs_ec) return err("failed to resolve Sura engine path: " + abs_ec.message());
    if (!json_path.empty()) {
        json_path = fs::absolute(json_path, abs_ec).lexically_normal();
        if (abs_ec) return err("failed to resolve run JSON path: " + abs_ec.message());
        if (json_path.has_parent_path()) fs::create_directories(json_path.parent_path());
    }
    fs::path display_main;
    std::error_code rel_ec;
    display_main = fs::relative(main_path, root, rel_ec);
    if (rel_ec) display_main = main_path;

    std::string cmd;
#ifdef _WIN32
    cmd = "cd /d " + shell_quote(path_to_utf8(root)) + " && call " + shell_quote(path_to_utf8(engine));
#else
    cmd = "cd " + shell_quote(path_to_utf8(root)) + " && " + shell_quote(path_to_utf8(engine));
#endif
    if (use_jit) cmd += " --jit";
    cmd += " " + shell_quote(path_to_utf8(display_main));
    if (!script_args.empty()) {
        cmd += " --";
        for (const auto& item : script_args) cmd += " " + shell_quote(item);
    }

    std::string output;
    auto start = std::chrono::steady_clock::now();
    int code = run_capture_command_status(cmd, output);
    auto end = std::chrono::steady_clock::now();
    long long duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    fs::current_path(old_cwd);
    if (!output.empty()) std::cout << output;
    if (!json_path.empty()) {
        std::ostringstream report;
        report << "{\n"
               << "  \"schema\": \"sura.package.run.v1\",\n"
               << "  \"root\": \"" << json_escape(path_to_generic_utf8(root)) << "\",\n"
               << "  \"package\": \"" << json_escape(name) << "\",\n"
               << "  \"version\": \"" << json_escape(version) << "\",\n"
               << "  \"main\": \"" << json_escape(path_to_generic_utf8(display_main)) << "\",\n"
               << "  \"engine\": \"" << json_escape(path_to_generic_utf8(engine)) << "\",\n"
               << "  \"jit\": " << (use_jit ? "true" : "false") << ",\n"
               << "  \"passed\": " << (code == 0 ? "true" : "false") << ",\n"
               << "  \"exitCode\": " << code << ",\n"
               << "  \"durationMs\": " << duration_ms << ",\n"
               << "  \"args\": [";
        for (size_t i = 0; i < script_args.size(); ++i) {
            if (i > 0) report << ", ";
            report << "\"" << json_escape(script_args[i]) << "\"";
        }
        report << "],\n"
               << "  \"output\": \"" << json_escape(output) << "\"\n"
               << "}\n";
        if (!write_all(json_path, report.str())) {
            return err("failed to write run JSON report: " + path_to_generic_utf8(json_path));
        }
        ok("run report written: " + path_to_generic_utf8(json_path));
    }
    if (code == 0) return 0;
    return err("package run failed with exit code " + std::to_string(code));
}

static int cmd_profile(const std::vector<std::string>& argv) {
    fs::path root = fs::current_path();
    fs::path json_path;
    std::vector<std::string> script_args;
    bool collect_script_args = false;
    bool use_jit = true;
    bool path_set = false;

    auto need_path_value = [&](size_t& i, const std::string& option, fs::path& value) -> bool {
        if (i + 1 >= argv.size()) {
            err(option + " requires a path");
            return false;
        }
        value = utf8_path(argv[++i]);
        return true;
    };

    for (size_t i = 2; i < argv.size(); ++i) {
        std::string arg = argv[i];
        if (collect_script_args) {
            script_args.push_back(arg);
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage:\n"
                << "  surapkg profile [package-path] [--json profile.json] [--no-jit] [--] [args...]\n";
            return 0;
        }
        if (arg == "--") {
            collect_script_args = true;
            continue;
        }
        if (arg == "--no-jit") {
            use_jit = false;
            continue;
        }
        if (arg == "--json" || arg == "--profile-json") {
            if (!need_path_value(i, arg, json_path)) return 1;
            continue;
        }
        if (arg.rfind("--json=", 0) == 0) {
            json_path = utf8_path(arg.substr(7));
            if (json_path.empty()) return err("profile --json requires a path");
            continue;
        }
        if (!path_set && (arg.empty() || arg[0] != '-')) {
            root = utf8_path(arg);
            path_set = true;
            continue;
        }
        script_args.push_back(arg);
    }

    if (fs::is_regular_file(root)) root = root.parent_path();
    root = fs::absolute(root).lexically_normal();
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return err("sura.pkg.json not found for profile: " + path_to_generic_utf8(root / kManifest));

    std::string main_file = manifest_field(manifest, "main", "");
    if (main_file.empty()) return err("package manifest has no main field");
    fs::path main_path = (root / utf8_path(main_file)).lexically_normal();
    if (!fs::exists(main_path) || !fs::is_regular_file(main_path)) {
        return err("package main file missing: " + path_to_generic_utf8(main_path));
    }

    fs::path old_cwd = fs::current_path();
    std::error_code abs_ec;
    fs::path engine = fs::absolute(utf8_path(test_engine_path()), abs_ec).lexically_normal();
    if (abs_ec) return err("failed to resolve Sura engine path: " + abs_ec.message());
    if (!json_path.empty()) {
        json_path = fs::absolute(json_path, abs_ec).lexically_normal();
        if (abs_ec) return err("failed to resolve profile JSON path: " + abs_ec.message());
        if (json_path.has_parent_path()) fs::create_directories(json_path.parent_path());
    }

    fs::path display_main;
    std::error_code rel_ec;
    display_main = fs::relative(main_path, root, rel_ec);
    if (rel_ec) display_main = main_path;

    std::string cmd;
#ifdef _WIN32
    cmd = "cd /d " + shell_quote(path_to_utf8(root)) + " && call " + shell_quote(path_to_utf8(engine));
#else
    cmd = "cd " + shell_quote(path_to_utf8(root)) + " && " + shell_quote(path_to_utf8(engine));
#endif
    if (use_jit) cmd += " --jit";
    if (!json_path.empty()) {
        cmd += " --profile-json " + shell_quote(path_to_utf8(json_path));
    } else {
        cmd += " --profile";
    }
    cmd += " " + shell_quote(path_to_utf8(display_main));
    if (!script_args.empty()) {
        cmd += " --";
        for (const auto& item : script_args) cmd += " " + shell_quote(item);
    }

    std::string output;
    int code = run_capture_command_status(cmd, output);
    fs::current_path(old_cwd);
    if (!output.empty()) std::cout << output;
    if (code == 0) {
        ok("profiled package main: " + path_to_generic_utf8(display_main));
        if (!json_path.empty()) ok("profile JSON: " + path_to_generic_utf8(json_path));
        return 0;
    }
    return err("package profile failed with exit code " + std::to_string(code));
}

struct BenchMetrics {
    double parse_ms = 0.0;
    double typecheck_ms = 0.0;
    double compile_ms = 0.0;
    double execute_ms = 0.0;
    double total_ms = 0.0;
    int bytecode = 0;
    bool parsed = false;
};

static bool parse_bench_number(const std::string& output, const std::string& label, double& value) {
    std::regex rx(label + R"(:\s+([0-9]+(?:\.[0-9]+)?))");
    std::smatch match;
    if (!std::regex_search(output, match, rx)) return false;
    value = std::strtod(match[1].str().c_str(), nullptr);
    return true;
}

static bool parse_bench_int(const std::string& output, const std::string& label, int& value) {
    std::regex rx(label + R"(:\s+([0-9]+))");
    std::smatch match;
    if (!std::regex_search(output, match, rx)) return false;
    value = std::atoi(match[1].str().c_str());
    return true;
}

static BenchMetrics parse_bench_metrics(const std::string& output) {
    BenchMetrics metrics;
    bool ok = true;
    ok = parse_bench_number(output, "Parse", metrics.parse_ms) && ok;
    ok = parse_bench_number(output, "TypeCheck", metrics.typecheck_ms) && ok;
    ok = parse_bench_number(output, "Compile", metrics.compile_ms) && ok;
    ok = parse_bench_number(output, "Execute", metrics.execute_ms) && ok;
    ok = parse_bench_number(output, "Total", metrics.total_ms) && ok;
    parse_bench_int(output, "Bytecode", metrics.bytecode);
    metrics.parsed = ok;
    return metrics;
}

static std::string bench_json_number(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

static std::string bench_metrics_json(const BenchMetrics& metrics, int indent) {
    std::string pad((size_t)indent, ' ');
    std::string inner((size_t)indent + 2, ' ');
    std::ostringstream out;
    out << pad << "{\n"
        << inner << "\"parse_ms\": " << bench_json_number(metrics.parse_ms) << ",\n"
        << inner << "\"typecheck_ms\": " << bench_json_number(metrics.typecheck_ms) << ",\n"
        << inner << "\"compile_ms\": " << bench_json_number(metrics.compile_ms) << ",\n"
        << inner << "\"execute_ms\": " << bench_json_number(metrics.execute_ms) << ",\n"
        << inner << "\"total_ms\": " << bench_json_number(metrics.total_ms) << ",\n"
        << inner << "\"bytecode\": " << metrics.bytecode << "\n"
        << pad << "}";
    return out.str();
}

static void bench_metrics_markdown_row(std::ostringstream& out, const std::string& label, const BenchMetrics& metrics) {
    out << "| " << markdown_escape(label)
        << " | " << bench_json_number(metrics.parse_ms)
        << " | " << bench_json_number(metrics.typecheck_ms)
        << " | " << bench_json_number(metrics.compile_ms)
        << " | " << bench_json_number(metrics.execute_ms)
        << " | " << bench_json_number(metrics.total_ms)
        << " | " << metrics.bytecode
        << " |\n";
}

static bool parse_double_arg(const std::string& text, double& value) {
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && end && *end == '\0';
}

static bool parse_python_avg_ms(const std::string& output, double& value) {
    std::regex avg_re(R"(avg\s*\(\s*[0-9]+\s*runs\s*\):\s*([0-9]+(?:\.[0-9]+)?)\s*ms)",
                      std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(output, match, avg_re)) return false;
    value = std::strtod(match[1].str().c_str(), nullptr);
    return true;
}

static std::string python_command_candidate_pkg(const std::string& raw) {
    if (raw.empty() || raw.find_first_of("\r\n") != std::string::npos) return "";
    if (raw.find(' ') != std::string::npos && raw.find('/') == std::string::npos && raw.find('\\') == std::string::npos) {
        return raw;
    }
    return shell_quote(raw);
}

static std::string find_python_command_pkg() {
    std::vector<std::string> candidates;
    const char* env = std::getenv("SURA_PYTHON");
    if (env && *env) candidates.push_back(env);
    candidates.push_back("python");
    candidates.push_back("python3");
#ifdef _WIN32
    candidates.push_back("py -3");
    candidates.push_back("C:\\msys64\\mingw64\\bin\\python.exe");
    candidates.push_back("C:\\msys64\\ucrt64\\bin\\python.exe");
#endif

    std::regex version_re(R"(Python\s+[0-9]+\.[0-9]+)");
    for (const auto& candidate : candidates) {
        std::string cmd = python_command_candidate_pkg(candidate);
        if (cmd.empty()) continue;
        std::string output;
        int code = run_capture_command_status(cmd + " --version 2>&1", output);
        if (code == 0 && std::regex_search(output, version_re)) return cmd;
    }
    return "";
}

static int cmd_bench(const std::vector<std::string>& argv) {
    fs::path root = fs::current_path();
    fs::path json_path;
    fs::path summary_path;
    fs::path python_path;
    std::vector<std::string> script_args;
    bool collect_script_args = false;
    bool run_jit = true;
    bool path_set = false;
    bool has_min_speedup = false;
    double min_speedup = 0.0;

    auto need_path_value = [&](size_t& i, const std::string& option, fs::path& value) -> bool {
        if (i + 1 >= argv.size()) {
            err(option + " requires a path");
            return false;
        }
        value = utf8_path(argv[++i]);
        return true;
    };

    auto need_double_value = [&](size_t& i, const std::string& option, double& value) -> bool {
        if (i + 1 >= argv.size()) {
            err(option + " requires a number");
            return false;
        }
        if (!parse_double_arg(argv[++i], value)) {
            err(option + " requires a valid number");
            return false;
        }
        return true;
    };

    for (size_t i = 2; i < argv.size(); ++i) {
        std::string arg = argv[i];
        if (collect_script_args) {
            script_args.push_back(arg);
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage:\n"
                << "  surapkg bench [package-path] [--json bench.json] [--summary bench.md] [--no-jit]\n"
                << "                 [--min-speedup ratio] [--python script.py] [--] [args...]\n";
            return 0;
        }
        if (arg == "--") {
            collect_script_args = true;
            continue;
        }
        if (arg == "--no-jit") {
            run_jit = false;
            continue;
        }
        if (arg == "--json" || arg == "--report") {
            if (!need_path_value(i, arg, json_path)) return 1;
            continue;
        }
        if (arg == "--summary" || arg == "--markdown") {
            if (!need_path_value(i, arg, summary_path)) return 1;
            continue;
        }
        if (arg == "--python") {
            if (!need_path_value(i, arg, python_path)) return 1;
            continue;
        }
        if (arg.rfind("--json=", 0) == 0) {
            json_path = utf8_path(arg.substr(7));
            if (json_path.empty()) return err("bench --json requires a path");
            continue;
        }
        if (arg.rfind("--summary=", 0) == 0) {
            summary_path = utf8_path(arg.substr(10));
            if (summary_path.empty()) return err("bench --summary requires a path");
            continue;
        }
        if (arg.rfind("--markdown=", 0) == 0) {
            summary_path = utf8_path(arg.substr(11));
            if (summary_path.empty()) return err("bench --markdown requires a path");
            continue;
        }
        if (arg.rfind("--python=", 0) == 0) {
            python_path = utf8_path(arg.substr(9));
            if (python_path.empty()) return err("bench --python requires a path");
            continue;
        }
        if (arg == "--min-speedup") {
            if (!need_double_value(i, arg, min_speedup)) return 1;
            has_min_speedup = true;
            continue;
        }
        if (arg.rfind("--min-speedup=", 0) == 0) {
            if (!parse_double_arg(arg.substr(14), min_speedup)) {
                return err("--min-speedup requires a valid number");
            }
            has_min_speedup = true;
            continue;
        }
        if (!path_set && (arg.empty() || arg[0] != '-')) {
            root = utf8_path(arg);
            path_set = true;
            continue;
        }
        script_args.push_back(arg);
    }

    if (has_min_speedup && min_speedup < 0.0) return err("--min-speedup must be non-negative");

    if (fs::is_regular_file(root)) root = root.parent_path();
    root = fs::absolute(root).lexically_normal();
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return err("sura.pkg.json not found for bench: " + path_to_generic_utf8(root / kManifest));

    std::string pkg_name = manifest_field(manifest, "name", "");
    std::string pkg_version = manifest_field(manifest, "version", "");
    std::string main_file = manifest_field(manifest, "main", "");
    if (main_file.empty()) return err("package manifest has no main field");
    fs::path main_path = (root / utf8_path(main_file)).lexically_normal();
    if (!fs::exists(main_path) || !fs::is_regular_file(main_path)) {
        return err("package main file missing: " + path_to_generic_utf8(main_path));
    }
    if (!python_path.empty()) {
        if (python_path.is_relative()) python_path = (root / python_path).lexically_normal();
        else python_path = fs::absolute(python_path).lexically_normal();
        if (!fs::exists(python_path) || !fs::is_regular_file(python_path)) {
            return err("Python benchmark file missing: " + path_to_generic_utf8(python_path));
        }
    }

    std::error_code abs_ec;
    fs::path engine = fs::absolute(utf8_path(test_engine_path()), abs_ec).lexically_normal();
    if (abs_ec) return err("failed to resolve Sura engine path: " + abs_ec.message());
    if (!json_path.empty()) {
        json_path = fs::absolute(json_path, abs_ec).lexically_normal();
        if (abs_ec) return err("failed to resolve bench JSON path: " + abs_ec.message());
        if (json_path.has_parent_path()) fs::create_directories(json_path.parent_path());
    }
    if (!summary_path.empty()) {
        summary_path = fs::absolute(summary_path, abs_ec).lexically_normal();
        if (abs_ec) return err("failed to resolve bench summary path: " + abs_ec.message());
        if (summary_path.has_parent_path()) fs::create_directories(summary_path.parent_path());
    }

    fs::path display_main;
    std::error_code rel_ec;
    display_main = fs::relative(main_path, root, rel_ec);
    if (rel_ec) display_main = main_path;

    auto build_command = [&](bool jit) {
        std::string cmd;
#ifdef _WIN32
        cmd = "cd /d " + shell_quote(path_to_utf8(root)) + " && call " + shell_quote(path_to_utf8(engine));
#else
        cmd = "cd " + shell_quote(path_to_utf8(root)) + " && " + shell_quote(path_to_utf8(engine));
#endif
        if (jit) cmd += " --jit";
        cmd += " --bench " + shell_quote(path_to_utf8(display_main));
        if (!script_args.empty()) {
            cmd += " --";
            for (const auto& item : script_args) cmd += " " + shell_quote(item);
        }
        cmd += " 2>&1";
        return cmd;
    };

    info("interpreter benchmark");
    std::string interp_output;
    int interp_code = run_capture_command_status(build_command(false), interp_output);
    if (!interp_output.empty()) std::cout << interp_output;
    if (interp_code != 0) return err("package interpreter benchmark failed with exit code " + std::to_string(interp_code));
    BenchMetrics interp = parse_bench_metrics(interp_output);
    if (!interp.parsed) return err("failed to parse interpreter benchmark output");

    BenchMetrics jit;
    std::string jit_output;
    double speedup = 0.0;
    bool has_speedup = false;
    if (run_jit) {
        info("JIT benchmark");
        int jit_code = run_capture_command_status(build_command(true), jit_output);
        if (!jit_output.empty()) std::cout << jit_output;
        if (jit_code != 0) return err("package JIT benchmark failed with exit code " + std::to_string(jit_code));
        jit = parse_bench_metrics(jit_output);
        if (!jit.parsed) return err("failed to parse JIT benchmark output");
        if (jit.execute_ms > 0.0) {
            speedup = interp.execute_ms / jit.execute_ms;
            has_speedup = true;
        }
    }

    double python_ms = 0.0;
    double sura_faster_by_python = 0.0;
    bool has_python = false;
    bool has_python_ratio = false;
    std::string python_time_source;
    fs::path display_python;
    if (!python_path.empty()) {
        std::string python = find_python_command_pkg();
        if (python.empty()) return err("Python executable not found; set SURA_PYTHON");

        std::error_code py_rel_ec;
        display_python = fs::relative(python_path, root, py_rel_ec);
        if (py_rel_ec) display_python = python_path;

        info("Python comparison benchmark");
        std::string py_cmd;
#ifdef _WIN32
        py_cmd = "cd /d " + shell_quote(path_to_utf8(root)) + " && call " + python + " " + shell_quote(path_to_utf8(display_python)) + " 2>&1";
#else
        py_cmd = "cd " + shell_quote(path_to_utf8(root)) + " && " + python + " " + shell_quote(path_to_utf8(display_python)) + " 2>&1";
#endif
        auto py_start = std::chrono::high_resolution_clock::now();
        std::string py_output;
        int py_code = run_capture_command_status(py_cmd, py_output);
        auto py_end = std::chrono::high_resolution_clock::now();
        double py_wall_ms = std::chrono::duration<double, std::milli>(py_end - py_start).count();
        if (!py_output.empty()) std::cout << py_output;
        if (py_code != 0) return err("Python benchmark failed with exit code " + std::to_string(py_code));
        if (parse_python_avg_ms(py_output, python_ms)) {
            python_time_source = "avg_output";
        } else {
            python_ms = py_wall_ms;
            python_time_source = "wall_clock";
        }
        has_python = true;

        double sura_compare_ms = run_jit && jit.parsed ? jit.execute_ms : interp.execute_ms;
        if (sura_compare_ms > 0.0 && python_ms > 0.0) {
            sura_faster_by_python = python_ms / sura_compare_ms;
            has_python_ratio = true;
        }
    }

    if (!json_path.empty()) {
        std::ostringstream report;
        report << "{\n"
               << "  \"schema\": \"sura.package.bench.v1\",\n"
               << "  \"package\": \"" << json_escape(pkg_name) << "\",\n"
               << "  \"version\": \"" << json_escape(pkg_version) << "\",\n"
               << "  \"main\": \"" << json_escape(path_to_generic_utf8(display_main)) << "\",\n"
               << "  \"jit_enabled\": " << (run_jit ? "true" : "false") << ",\n"
               << "  \"interpreter\": " << bench_metrics_json(interp, 2) << ",\n";
        if (run_jit) {
            report << "  \"jit\": " << bench_metrics_json(jit, 2) << ",\n";
        } else {
            report << "  \"jit\": null,\n";
        }
        if (has_speedup) {
            report << "  \"speedup\": " << bench_json_number(speedup) << "\n";
        } else {
            report << "  \"speedup\": null\n";
        }
        report << ",\n";
        if (has_python) {
            report << "  \"python\": {\n"
                   << "    \"script\": \"" << json_escape(path_to_generic_utf8(display_python)) << "\",\n"
                   << "    \"ms\": " << bench_json_number(python_ms) << ",\n"
                   << "    \"time_source\": \"" << json_escape(python_time_source) << "\"\n"
                   << "  },\n";
        } else {
            report << "  \"python\": null,\n";
        }
        if (has_python_ratio) {
            report << "  \"sura_faster_by_python\": " << bench_json_number(sura_faster_by_python) << "\n";
        } else {
            report << "  \"sura_faster_by_python\": null\n";
        }
        report << "}\n";
        if (!write_all(json_path, report.str())) return err("failed to write bench JSON: " + path_to_generic_utf8(json_path));
    }

    if (!summary_path.empty()) {
        std::ostringstream summary;
        summary << "# Sura Package Benchmark Summary\n\n"
                << "| Field | Value |\n"
                << "| --- | --- |\n"
                << "| Package | " << markdown_escape(pkg_name.empty() ? "(unnamed)" : pkg_name) << " |\n"
                << "| Version | " << markdown_escape(pkg_version.empty() ? "(unspecified)" : pkg_version) << " |\n"
                << "| Main | `" << markdown_escape(path_to_generic_utf8(display_main)) << "` |\n"
                << "| JIT enabled | " << (run_jit ? "yes" : "no") << " |\n"
                << "| JIT speedup | " << (has_speedup ? bench_json_number(speedup) + "x" : "n/a") << " |\n"
                << "| Sura/Python ratio | " << (has_python_ratio ? bench_json_number(sura_faster_by_python) + "x" : "n/a") << " |\n\n"
                << "## Timings\n\n"
                << "| Mode | Parse ms | TypeCheck ms | Compile ms | Execute ms | Total ms | Bytecode |\n"
                << "| --- | ---: | ---: | ---: | ---: | ---: | ---: |\n";
        bench_metrics_markdown_row(summary, "Interpreter", interp);
        if (run_jit) bench_metrics_markdown_row(summary, "JIT", jit);
        summary << "\n";
        if (has_python) {
            summary << "## Python Comparison\n\n"
                    << "| Script | Python ms | Source | Sura faster by |\n"
                    << "| --- | ---: | --- | ---: |\n"
                    << "| `" << markdown_escape(path_to_generic_utf8(display_python)) << "`"
                    << " | " << bench_json_number(python_ms)
                    << " | " << markdown_escape(python_time_source)
                    << " | " << (has_python_ratio ? bench_json_number(sura_faster_by_python) + "x" : "n/a")
                    << " |\n";
        } else {
            summary << "Python comparison was not run. Use `--python script.py` to include Python evidence.\n";
        }
        if (!write_all(summary_path, summary.str())) return err("failed to write bench summary: " + path_to_generic_utf8(summary_path));
    }

    ok("benchmarked package main: " + path_to_generic_utf8(display_main));
    if (has_speedup) ok("JIT speedup: " + bench_json_number(speedup) + "x");
    if (has_python_ratio) ok("Sura faster than Python: " + bench_json_number(sura_faster_by_python) + "x");
    if (!json_path.empty()) ok("bench JSON: " + path_to_generic_utf8(json_path));
    if (!summary_path.empty()) ok("bench summary: " + path_to_generic_utf8(summary_path));

    if (has_min_speedup && run_jit && has_speedup && speedup < min_speedup) {
        return err("JIT speedup " + bench_json_number(speedup) + "x is below minimum " + bench_json_number(min_speedup) + "x");
    }
    if (has_min_speedup && run_jit && !has_speedup) {
        return err("JIT speedup could not be calculated");
    }
    return 0;
}

static int cmd_bench_dashboard(const std::vector<std::string>& argv) {
    fs::path engine = utf8_path(test_engine_path());
    fs::path out = "bench_dashboard.html";
    fs::path json_out;
    fs::path summary_out;
    fs::path release_notes_out;
    fs::path history_in;
    fs::path history_out;
    fs::path native_perf_in;
    int history_limit = 100;

    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--engine") {
            if (i + 1 >= argv.size()) return err("bench-dashboard --engine requires a path");
            engine = utf8_path(argv[++i]);
        } else if (arg.rfind("--engine=", 0) == 0) {
            engine = utf8_path(arg.substr(9));
            if (engine.empty()) return err("bench-dashboard --engine requires a path");
        } else if (arg == "--out") {
            if (i + 1 >= argv.size()) return err("bench-dashboard --out requires a path");
            out = utf8_path(argv[++i]);
        } else if (arg.rfind("--out=", 0) == 0) {
            out = utf8_path(arg.substr(6));
            if (out.empty()) return err("bench-dashboard --out requires a path");
        } else if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("bench-dashboard --json requires a path");
            json_out = utf8_path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_out = utf8_path(arg.substr(7));
            if (json_out.empty()) return err("bench-dashboard --json requires a path");
        } else if (arg == "--summary") {
            if (i + 1 >= argv.size()) return err("bench-dashboard --summary requires a path");
            summary_out = utf8_path(argv[++i]);
        } else if (arg.rfind("--summary=", 0) == 0) {
            summary_out = utf8_path(arg.substr(10));
            if (summary_out.empty()) return err("bench-dashboard --summary requires a path");
        } else if (arg == "--release-notes" || arg == "--release-notes-out") {
            if (i + 1 >= argv.size()) return err("bench-dashboard --release-notes requires a path");
            release_notes_out = utf8_path(argv[++i]);
        } else if (arg.rfind("--release-notes=", 0) == 0) {
            release_notes_out = utf8_path(arg.substr(16));
            if (release_notes_out.empty()) return err("bench-dashboard --release-notes requires a path");
        } else if (arg.rfind("--release-notes-out=", 0) == 0) {
            release_notes_out = utf8_path(arg.substr(20));
            if (release_notes_out.empty()) return err("bench-dashboard --release-notes-out requires a path");
        } else if (arg == "--history-in") {
            if (i + 1 >= argv.size()) return err("bench-dashboard --history-in requires a path");
            history_in = utf8_path(argv[++i]);
        } else if (arg.rfind("--history-in=", 0) == 0) {
            history_in = utf8_path(arg.substr(13));
            if (history_in.empty()) return err("bench-dashboard --history-in requires a path");
        } else if (arg == "--history-out") {
            if (i + 1 >= argv.size()) return err("bench-dashboard --history-out requires a path");
            history_out = utf8_path(argv[++i]);
        } else if (arg.rfind("--history-out=", 0) == 0) {
            history_out = utf8_path(arg.substr(14));
            if (history_out.empty()) return err("bench-dashboard --history-out requires a path");
        } else if (arg == "--native-perf" || arg == "--native-perf-in") {
            if (i + 1 >= argv.size()) return err("bench-dashboard --native-perf requires a path");
            native_perf_in = utf8_path(argv[++i]);
        } else if (arg.rfind("--native-perf=", 0) == 0) {
            native_perf_in = utf8_path(arg.substr(14));
            if (native_perf_in.empty()) return err("bench-dashboard --native-perf requires a path");
        } else if (arg.rfind("--native-perf-in=", 0) == 0) {
            native_perf_in = utf8_path(arg.substr(17));
            if (native_perf_in.empty()) return err("bench-dashboard --native-perf-in requires a path");
        } else if (arg == "--history-limit") {
            if (i + 1 >= argv.size()) return err("bench-dashboard --history-limit requires a number");
            try { history_limit = std::stoi(argv[++i]); } catch (...) { return err("bench-dashboard --history-limit requires a number"); }
        } else if (arg.rfind("--history-limit=", 0) == 0) {
            try { history_limit = std::stoi(arg.substr(16)); } catch (...) { return err("bench-dashboard --history-limit requires a number"); }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg bench-dashboard [--engine SuraLanguage.exe] [--out bench_dashboard.html] [--json report.json] [--summary summary.md] [--release-notes notes.md] [--history-in history.json] [--history-out history.json] [--native-perf native_perf.json] [--history-limit n]\n";
            return 0;
        } else {
            return err("unknown bench-dashboard option: " + arg);
        }
    }

    if (history_limit <= 0) return err("bench-dashboard --history-limit must be positive");
    if (out.empty()) return err("bench-dashboard --out requires a path");
    if (!fs::exists(engine)) return err("bench-dashboard engine not found: " + path_to_generic_utf8(engine));
    if (!native_perf_in.empty() && !fs::exists(native_perf_in)) return err("bench-dashboard native performance report not found: " + path_to_generic_utf8(native_perf_in));
    fs::path script = fs::current_path() / "tools" / "sura_bench_dashboard.ps1";
    if (!fs::exists(script)) return err("benchmark dashboard script not found: " + path_to_generic_utf8(script));

    std::string ps = doctor_powershell_command();
    if (ps.empty()) return err("PowerShell not found; install powershell or pwsh to run bench-dashboard");

    auto ensure_parent = [](const fs::path& path) {
        if (!path.empty() && path.has_parent_path()) fs::create_directories(path.parent_path());
    };
    ensure_parent(out);
    ensure_parent(json_out);
    ensure_parent(summary_out);
    ensure_parent(release_notes_out);
    ensure_parent(history_out);

    std::string command;
#ifdef _WIN32
    command = "call " + shell_quote(ps);
#else
    command = shell_quote(ps);
#endif
    command += " -NoProfile -ExecutionPolicy Bypass -File " +
                          shell_quote(path_to_utf8(script)) +
                          " -Engine " + shell_quote(path_to_utf8(engine)) +
                          " -Out " + shell_quote(path_to_utf8(out)) +
                          " -HistoryLimit " + std::to_string(history_limit);
    if (!json_out.empty()) command += " -JsonOut " + shell_quote(path_to_utf8(json_out));
    if (!summary_out.empty()) command += " -SummaryOut " + shell_quote(path_to_utf8(summary_out));
    if (!release_notes_out.empty()) command += " -ReleaseNotesOut " + shell_quote(path_to_utf8(release_notes_out));
    if (!history_in.empty()) command += " -HistoryIn " + shell_quote(path_to_utf8(history_in));
    if (!history_out.empty()) command += " -HistoryOut " + shell_quote(path_to_utf8(history_out));
    if (!native_perf_in.empty()) command += " -NativePerfIn " + shell_quote(path_to_utf8(native_perf_in));
    command += " 2>&1";

    std::string output;
    int code = run_capture_command_status(command, output);
    if (code != 0) {
        if (!output.empty()) std::cerr << output;
        return err("bench-dashboard failed with exit code " + std::to_string(code));
    }
    if (!output.empty()) std::cout << output;
    ok("benchmark dashboard: " + path_to_generic_utf8(out));
    if (!json_out.empty()) ok("benchmark dashboard JSON: " + path_to_generic_utf8(json_out));
    if (!summary_out.empty()) ok("benchmark dashboard summary: " + path_to_generic_utf8(summary_out));
    if (!release_notes_out.empty()) ok("benchmark dashboard release notes: " + path_to_generic_utf8(release_notes_out));
    if (!history_out.empty()) ok("benchmark dashboard history: " + path_to_generic_utf8(history_out));
    if (!native_perf_in.empty()) ok("benchmark dashboard native performance: " + path_to_generic_utf8(native_perf_in));
    return 0;
}

static bool truthy_manifest_value(std::string value) {
    value = trim_ascii_ws(value);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

static bool manifest_bool_option(const std::string& manifest, const std::string& field,
                                 bool fallback, bool* found = nullptr) {
    bool string_found = false;
    std::string text = manifest_string_field(manifest, field, "", &string_found);
    if (string_found) {
        if (found) *found = true;
        return truthy_manifest_value(text);
    }

    std::regex raw_re("\"" + field + "\"\\s*:\\s*(true|false|1|0)", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(manifest, match, raw_re)) {
        if (found) *found = true;
        return truthy_manifest_value(match[1].str());
    }
    if (found) *found = false;
    return fallback;
}

static bool manifest_double_option(const std::string& manifest, const std::string& field,
                                   double& value) {
    bool string_found = false;
    std::string text = manifest_string_field(manifest, field, "", &string_found);
    if (string_found) return parse_double_arg(text, value);

    std::regex raw_re("\"" + field + "\"\\s*:\\s*([-+]?[0-9]+(?:\\.[0-9]+)?)");
    std::smatch match;
    if (!std::regex_search(manifest, match, raw_re)) return false;
    return parse_double_arg(match[1].str(), value);
}

static int run_manifest_bench_gate(const fs::path& root, const std::string& manifest,
                                   const std::string& phase) {
    bool bench_field_found = false;
    bool bench_enabled = manifest_bool_option(manifest, "bench", false, &bench_field_found);
    bool python_found = false;
    std::string python = manifest_string_field(manifest, "bench_python", "", &python_found);
    double min_speedup = 0.0;
    bool has_min_speedup = manifest_double_option(manifest, "bench_min_speedup", min_speedup);
    bool report_found = false;
    std::string report = manifest_string_field(manifest, "bench_report", "", &report_found);

    if (bench_field_found && !bench_enabled) {
        info(phase + " bench disabled by manifest");
        return 0;
    }
    if (!bench_enabled && !python_found && !has_min_speedup && !report_found) {
        info(phase + " bench not configured; skipping");
        return 0;
    }
    if (has_min_speedup && min_speedup < 0.0) return err(phase + " bench_min_speedup must be non-negative");

    fs::path report_path;
    bool keep_report = false;
    bool signed_ci_package = phase == "ci" && fs::exists(root / kSignature);
    if (report_found && !report.empty() && !signed_ci_package) {
        report_path = utf8_path(report);
        if (report_path.is_relative()) report_path = root / report_path;
        report_path = fs::absolute(report_path).lexically_normal();
        keep_report = true;
    } else {
        if (report_found && !report.empty() && signed_ci_package) {
            info("ci package signature present; writing bench report to a temporary path");
        }
        report_path = fs::temp_directory_path() /
            ("sura_" + phase + "_bench_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    }

    std::vector<std::string> args = {"surapkg", "bench", path_to_utf8(root), "--json", path_to_utf8(report_path)};
    if (has_min_speedup) {
        args.push_back("--min-speedup");
        args.push_back(bench_json_number(min_speedup));
    }
    if (python_found && !python.empty()) {
        args.push_back("--python");
        args.push_back(python);
    }

    int code = cmd_bench(args);
    if (!keep_report) {
        std::error_code ec;
        fs::remove(report_path, ec);
    }
    if (code != 0) return code;
    ok(phase + " bench passed");
    if (keep_report) ok(phase + " bench report: " + path_to_generic_utf8(report_path));
    return 0;
}

static void protect_usage() {
    std::cout
        << "Usage:\n"
        << "  surapkg protect [path] [--out file.sura.srp] [--key key|--key-file file]\n"
        << "                 [--license value|--license-file file]\n"
        << "                 [--id release-id] [--expires YYYY-MM-DD] [--exe file.exe]\n"
        << "                 [--closed-source] [--require-key] [--require-license]\n"
        << "                 [--require-expires]\n"
        << "                 [--json report.json|--scan-report file.json] [--no-leak-scan]\n";
}

static int cmd_protect(int argc, char* argv[]) {
    std::string source;
    fs::path out_path;
    std::string release_key;
    std::string release_license;
    std::string release_key_file;
    std::string release_license_file;
    std::string release_id;
    std::string release_expires;
    fs::path launcher_exe;
    fs::path scan_report;
    bool leak_scan = true;
    bool closed_source = false;
    bool require_key = false;
    bool require_license = false;
    bool require_expires = false;

    auto need_value = [&](int& i, const std::string& option, std::string& value) -> bool {
        if (i + 1 >= argc) {
            err(option + " requires a value");
            return false;
        }
        value = argv[++i];
        return true;
    };

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            protect_usage();
            return 0;
        }
        if (arg == "--out" || arg == "-o") {
            std::string value;
            if (!need_value(i, arg, value)) return 1;
            out_path = value;
        } else if (arg == "--exe" || arg == "--launcher-exe") {
            std::string value;
            if (!need_value(i, arg, value)) return 1;
            launcher_exe = value;
        } else if (arg == "--json" || arg == "--scan-report" || arg == "--leak-report" || arg == "--protect-report") {
            std::string value;
            if (!need_value(i, arg, value)) return 1;
            scan_report = value;
        } else if (arg.rfind("--json=", 0) == 0) {
            scan_report = arg.substr(7);
            if (scan_report.empty()) return err("protect --json requires an output path");
        } else if (arg == "--no-leak-scan") {
            leak_scan = false;
        } else if (arg == "--closed-source" || arg == "--private") {
            closed_source = true;
            require_key = true;
            require_license = true;
        } else if (arg == "--require-key") {
            require_key = true;
        } else if (arg == "--require-license") {
            require_license = true;
        } else if (arg == "--require-expires") {
            require_expires = true;
        } else if (arg == "--key" || arg == "--release-key") {
            if (!need_value(i, arg, release_key)) return 1;
        } else if (arg == "--key-file" || arg == "--release-key-file") {
            if (!need_value(i, arg, release_key_file)) return 1;
        } else if (arg == "--license" || arg == "--release-license") {
            if (!need_value(i, arg, release_license)) return 1;
        } else if (arg == "--license-file" || arg == "--release-license-file") {
            if (!need_value(i, arg, release_license_file)) return 1;
        } else if (arg == "--id" || arg == "--release-id" || arg == "--release-customer") {
            if (!need_value(i, arg, release_id)) return 1;
        } else if (arg == "--expires" || arg == "--release-expires") {
            if (!need_value(i, arg, release_expires)) return 1;
        } else if (!arg.empty() && arg[0] == '-') {
            return err("unknown protect option: " + arg);
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("protect accepts only one package path");
        }
    }

    fs::path root = doctor_package_root(source);
    std::string manifest = read_all(root / kManifest);
    if (manifest.empty()) return err("sura.pkg.json not found for protect: " + (root / kManifest).generic_string());

    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "0.1.0");
    std::string main_file = manifest_field(manifest, "main", "");
    if (main_file.empty()) return err("package manifest has no main field");

    fs::path main_path = root / main_file;
    if (!fs::exists(main_path)) return err("package main file missing: " + main_path.generic_string());
    if (!release_key_file.empty() && !fs::exists(release_key_file))
        return err("release key file not found: " + fs::path(release_key_file).generic_string());
    if (!release_license_file.empty() && !fs::exists(release_license_file))
        return err("release license file not found: " + fs::path(release_license_file).generic_string());

    auto option_secret_value = [](const std::string& direct, const std::string& file) {
        if (!direct.empty()) return direct;
        if (!file.empty()) return trim_ascii_ws(read_all(file));
        return std::string();
    };
    bool has_release_key = !option_secret_value(release_key, release_key_file).empty();
    bool has_release_license = !option_secret_value(release_license, release_license_file).empty();
    if (closed_source && !leak_scan)
        return err("--closed-source requires the default leak scan; remove --no-leak-scan");
    if (require_key && !has_release_key)
        return err("--closed-source/--require-key requires --key or --key-file with a non-empty value");
    if (require_license && !has_release_license)
        return err("--closed-source/--require-license requires --license or --license-file with a non-empty value");
    if (require_expires && release_expires.empty())
        return err("--require-expires requires --expires YYYY-MM-DD");

    if (out_path.empty()) {
        std::string file_name = artifact_safe(name, "package") + "-" +
                                artifact_safe(version, "0.1.0") + ".sura.srp";
        out_path = root / "dist" / file_name;
    }
    if (out_path.has_parent_path()) fs::create_directories(out_path.parent_path());

    std::string engine = test_engine_path();
#ifdef _WIN32
    std::string cmd = "call " + shell_quote(engine);
#else
    std::string cmd = shell_quote(engine);
#endif
    cmd += " --release " + shell_quote(main_path.string()) + " --out " + shell_quote(out_path.string());
    if (!release_key_file.empty()) cmd += " --release-key-file " + shell_quote(release_key_file);
    else if (!release_key.empty()) cmd += " --release-key " + shell_quote(release_key);
    if (!release_license_file.empty()) cmd += " --release-license-file " + shell_quote(release_license_file);
    else if (!release_license.empty()) cmd += " --release-license " + shell_quote(release_license);
    if (!release_id.empty()) cmd += " --release-id " + shell_quote(release_id);
    if (!release_expires.empty()) cmd += " --release-expires " + shell_quote(release_expires);
    cmd += " 2>&1";

    std::string output;
    int code = run_capture_command_status(cmd, output);
    if (code != 0) {
        if (!output.empty()) std::cerr << output;
        return err("protect failed with exit code " + std::to_string(code));
    }
    if (!fs::exists(out_path)) return err("protect did not create output: " + out_path.generic_string());

    if (!launcher_exe.empty()) {
        std::string launcher_output;
        if (!build_release_launcher_exe(out_path, launcher_exe, launcher_output)) {
            if (!launcher_output.empty()) std::cerr << launcher_output;
            return err("failed to build protected launcher exe: " + launcher_exe.generic_string());
        }
    }

    if (leak_scan) {
        fs::path report_path = scan_report.empty()
            ? fs::path(out_path.string() + ".protect.json")
            : scan_report;
        int scan_code = run_protect_leak_scan(root, main_path, out_path, launcher_exe, report_path,
                                              release_key, release_license,
                                              release_key_file, release_license_file,
                                              closed_source ? "closed-source" : "standard",
                                              has_release_key, has_release_license,
                                              release_expires);
        if (scan_code != 0) return scan_code;
    }

    ok("protected " + name + "@" + version + " -> " + out_path.generic_string() + " (source stripped)");
    if (closed_source)
        ok("closed-source protection enforced: key, license, and leak scan");
    if (!release_key.empty() || !release_key_file.empty())
        info("load with --load-release-key, --load-release-key-file, or SURA_RELEASE_KEY");
    if (!release_license.empty() || !release_license_file.empty())
        info("load with --load-release-license, --load-release-license-file, or SURA_RELEASE_LICENSE");
    if (!launcher_exe.empty()) {
        ok("protected launcher exe -> " + launcher_exe.generic_string());
        info("ship SuraLanguage.exe next to the launcher or set SURA_ENGINE");
    }
    return 0;
}

struct ProtectVerifyTarget {
    std::string kind;
    std::string path;
    std::string resolved_path;
    bool exists = false;
};

struct ProtectVerifyCheck {
    std::string name;
    std::string status;
    std::string message;
    std::string action;
};

static void protect_verify_check(std::vector<ProtectVerifyCheck>& checks,
                                 const std::string& name,
                                 bool passed,
                                 const std::string& ok_message,
                                 const std::string& fail_message,
                                 const std::string& action) {
    checks.push_back({name, passed ? "pass" : "fail", passed ? ok_message : fail_message, passed ? "" : action});
}

static bool json_empty_array_field(const std::string& json, const std::string& field) {
    return std::regex_search(json, std::regex("\"" + field + "\"\\s*:\\s*\\[\\s*\\]"));
}

static long long json_number_field_or(const std::string& json, const std::string& field, long long fallback) {
    std::string value;
    if (!json_number_field_text(json, field, value)) return fallback;
    try {
        return std::stoll(value);
    } catch (...) {
        return fallback;
    }
}

static std::vector<ProtectVerifyTarget> parse_protect_report_targets(const std::string& report) {
    std::vector<ProtectVerifyTarget> targets;
    std::regex target_re("\\{\\s*\"kind\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"\\s*,\\s*\"path\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"\\s*\\}");
    for (auto it = std::sregex_iterator(report.begin(), report.end(), target_re);
         it != std::sregex_iterator(); ++it) {
        targets.push_back({json_unescape((*it)[1].str()), json_unescape((*it)[2].str()), "", false});
    }
    return targets;
}

static bool protect_verify_has_target_kind(const std::vector<ProtectVerifyTarget>& targets,
                                           const std::string& required_kind) {
    for (const auto& target : targets) {
        if (target.kind == required_kind) return true;
    }
    return false;
}

static int protect_verify_failure_count(const std::vector<ProtectVerifyCheck>& checks) {
    int failures = 0;
    for (const auto& check : checks) {
        if (check.status == "fail") ++failures;
    }
    return failures;
}

static bool write_protect_verify_json_report(const fs::path& out_path,
                                             const fs::path& report_path,
                                             const std::string& protect_schema,
                                             const std::string& mode,
                                             bool keyed,
                                             bool licensed,
                                             const std::string& expires,
                                             long long source_files_scanned,
                                             long long probes,
                                             const std::vector<ProtectVerifyTarget>& targets,
                                             const std::vector<std::string>& required_targets,
                                             const std::vector<ProtectVerifyCheck>& checks) {
    std::ostringstream out;
    int failure_count = protect_verify_failure_count(checks);
    out << "{\n"
        << "  \"schema\": \"sura.package.protect_verify.v1\",\n"
        << "  \"passed\": " << (failure_count == 0 ? "true" : "false") << ",\n"
        << "  \"report\": \"" << json_escape(report_path.generic_string()) << "\",\n"
        << "  \"protect_schema\": \"" << json_escape(protect_schema) << "\",\n"
        << "  \"mode\": \"" << json_escape(mode) << "\",\n"
        << "  \"keyed\": " << (keyed ? "true" : "false") << ",\n"
        << "  \"licensed\": " << (licensed ? "true" : "false") << ",\n"
        << "  \"expires\": \"" << json_escape(expires) << "\",\n"
        << "  \"sourceFilesScanned\": " << source_files_scanned << ",\n"
        << "  \"probes\": " << probes << ",\n"
        << "  \"target_count\": " << targets.size() << ",\n"
        << "  \"failure_count\": " << failure_count << ",\n"
        << "  \"required_targets\": [";
    for (size_t i = 0; i < required_targets.size(); ++i) {
        if (i) out << ", ";
        out << "\"" << json_escape(required_targets[i]) << "\"";
    }
    out << "],\n"
        << "  \"next_actions\": [\n";
    bool first_action = true;
    for (const auto& check : checks) {
        if (check.status != "fail" || check.action.empty()) continue;
        if (!first_action) out << ",\n";
        first_action = false;
        out << "    {\"check\":\"" << json_escape(check.name)
            << "\",\"action\":\"" << json_escape(check.action) << "\"}";
    }
    out << "\n  ],\n"
        << "  \"targets\": [\n";
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        out << "    {\"kind\":\"" << json_escape(target.kind)
            << "\",\"path\":\"" << json_escape(target.path)
            << "\",\"resolved_path\":\"" << json_escape(target.resolved_path)
            << "\",\"exists\":" << (target.exists ? "true" : "false") << "}";
        if (i + 1 < targets.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n"
        << "  \"checks\": [\n";
    for (size_t i = 0; i < checks.size(); ++i) {
        const auto& check = checks[i];
        out << "    {\"name\":\"" << json_escape(check.name)
            << "\",\"status\":\"" << json_escape(check.status)
            << "\",\"message\":\"" << json_escape(check.message)
            << "\",\"action\":\"" << json_escape(check.action) << "\"}";
        if (i + 1 < checks.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
    return write_all(out_path, out.str());
}

static void protect_verify_usage() {
    std::cout
        << "Usage:\n"
        << "  surapkg protect-verify <protect-report.json> [--require-closed-source]\n"
        << "                 [--require-key] [--require-license] [--require-expires]\n"
        << "                 [--require-target package|launcher] [--json report.json]\n";
}

static int cmd_protect_verify(const std::vector<std::string>& argv) {
    std::string report_arg;
    fs::path json_report;
    bool require_closed_source = false;
    bool require_key = false;
    bool require_license = false;
    bool require_expires = false;
    std::vector<std::string> required_targets;

    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            protect_verify_usage();
            return 0;
        } else if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("protect-verify --json requires an output path");
            json_report = argv[++i];
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = arg.substr(7);
            if (json_report.empty()) return err("protect-verify --json requires an output path");
        } else if (arg == "--require-closed-source" || arg == "--closed-source") {
            require_closed_source = true;
        } else if (arg == "--require-key") {
            require_key = true;
        } else if (arg == "--require-license") {
            require_license = true;
        } else if (arg == "--require-expires") {
            require_expires = true;
        } else if (arg == "--require-target") {
            if (i + 1 >= argv.size()) return err("protect-verify --require-target requires package or launcher");
            required_targets.push_back(argv[++i]);
        } else if (arg.rfind("--require-target=", 0) == 0) {
            std::string value = arg.substr(17);
            if (value.empty()) return err("protect-verify --require-target requires package or launcher");
            required_targets.push_back(value);
        } else if (!arg.empty() && arg[0] == '-') {
            return err("unknown protect-verify option: " + arg);
        } else if (report_arg.empty()) {
            report_arg = arg;
        } else {
            return err("protect-verify accepts one report path");
        }
    }

    if (report_arg.empty()) return err("protect-verify requires a protect report JSON path");
    fs::path report_path = report_arg;
    std::string report = read_all(report_path);

    std::string protect_schema = manifest_field(report, "schema", "");
    std::string mode = manifest_field(report, "mode", "");
    std::string status = manifest_field(report, "status", "");
    std::string expires = manifest_field(report, "expires", "");
    bool passed = json_bool_field_near(report, "passed", false);
    bool keyed = json_bool_field_near(report, "keyed", false);
    bool licensed = json_bool_field_near(report, "licensed", false);
    long long source_files_scanned = json_number_field_or(report, "sourceFilesScanned", -1);
    long long probes = json_number_field_or(report, "probes", -1);
    std::vector<ProtectVerifyTarget> targets = parse_protect_report_targets(report);

    fs::path report_dir = report_path.has_parent_path() ? report_path.parent_path() : fs::current_path();
    for (auto& target : targets) {
        fs::path target_path = target.path;
        if (target_path.is_relative()) target_path = report_dir / target_path;
        std::error_code ec;
        fs::path canonical = fs::weakly_canonical(target_path, ec);
        target.resolved_path = (ec ? target_path.lexically_normal() : canonical).generic_string();
        target.exists = fs::exists(target_path);
    }

    std::vector<ProtectVerifyCheck> checks;
    protect_verify_check(checks, "report_file", !report.empty(),
                         "protect report is readable",
                         "protect report is missing or empty",
                         "rerun surapkg protect with --json report.json and commit the generated CI evidence");
    protect_verify_check(checks, "schema", protect_schema == "sura.package.protect.v1",
                         "protect report schema is valid",
                         "protect report schema is missing or unexpected",
                         "rerun surapkg protect to generate a sura.package.protect.v1 report");
    protect_verify_check(checks, "passed", passed && status == "PASS",
                         "protect report passed",
                         "protect report did not pass",
                         "fix protect leak findings, then rerun surapkg protect");
    protect_verify_check(checks, "findings", json_empty_array_field(report, "findings"),
                         "protect report has no findings",
                         "protect report contains findings or omits the findings array",
                         "remove leaked source/secret bytes and regenerate the protect report");
    protect_verify_check(checks, "source_scan", source_files_scanned > 0 && probes > 0,
                         "source scan metadata is present",
                         "source scan metadata is missing or empty",
                         "rerun surapkg protect with leak scanning enabled");
    protect_verify_check(checks, "targets", !targets.empty(),
                         "protect report lists output targets",
                         "protect report has no protected output targets",
                         "rerun surapkg protect and keep the generated .sura.srp or launcher output");

    bool all_targets_exist = !targets.empty();
    for (const auto& target : targets) {
        if (!target.exists) all_targets_exist = false;
    }
    protect_verify_check(checks, "target_files", all_targets_exist,
                         "all protected output targets exist",
                         "one or more protected output targets are missing",
                         "restore the protected output files or rerun surapkg protect before release");

    if (require_closed_source) {
        protect_verify_check(checks, "closed_source", mode == "closed-source",
                             "closed-source mode is recorded",
                             "closed-source mode is not recorded",
                             "rerun surapkg protect with --closed-source");
    }
    if (require_key) {
        protect_verify_check(checks, "keyed", keyed,
                             "release key control is recorded",
                             "release key control is not recorded",
                             "rerun surapkg protect with --key-file");
    }
    if (require_license) {
        protect_verify_check(checks, "licensed", licensed,
                             "release license control is recorded",
                             "release license control is not recorded",
                             "rerun surapkg protect with --license-file");
    }
    if (require_expires) {
        protect_verify_check(checks, "expires", !expires.empty(),
                             "release expiration is recorded",
                             "release expiration is not recorded",
                             "rerun surapkg protect with --expires YYYY-MM-DD");
    }
    for (const auto& required : required_targets) {
        bool valid_name = required == "package" || required == "launcher";
        bool has_target = valid_name && protect_verify_has_target_kind(targets, required);
        protect_verify_check(checks, "target_" + required, has_target,
                             "required target is present: " + required,
                             valid_name ? "required target is missing: " + required :
                                          "unknown required target kind: " + required,
                             valid_name ? "rerun surapkg protect with the required output target" :
                                          "use --require-target package or --require-target launcher");
    }

    int failure_count = protect_verify_failure_count(checks);
    bool verified = failure_count == 0;
    if (!json_report.empty()) {
        if (!write_protect_verify_json_report(json_report, report_path, protect_schema, mode, keyed, licensed,
                                             expires, source_files_scanned, probes, targets, required_targets,
                                             checks)) {
            return err("failed to write protect-verify JSON report: " + json_report.generic_string());
        }
        ok("protect-verify report written: " + json_report.generic_string());
    }

    std::cout << "Protect verification\n"
              << "  report: " << report_path.generic_string() << "\n"
              << "  passed: " << (verified ? "true" : "false") << "\n"
              << "  mode: " << (mode.empty() ? "unknown" : mode) << "\n"
              << "  targets: " << targets.size() << "\n";
    for (const auto& check : checks) {
        std::cout << "  [" << check.status << "] " << check.name << " - " << check.message << "\n";
    }
    if (!verified) return err("protect verification failed with " + std::to_string(failure_count) + " failed check(s)");
    return 0;
}

static std::vector<std::string> split_protect_target_list(const std::string& value) {
    std::vector<std::string> out;
    std::string current;
    auto flush = [&]() {
        std::string item = trim_ascii_ws(current);
        current.clear();
        if (item.empty()) return;
        for (auto& ch : item) ch = (char)std::tolower((unsigned char)ch);
        if (std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
    };
    for (char ch : value) {
        if (ch == ',' || ch == ';' || std::isspace((unsigned char)ch)) {
            flush();
        } else {
            current.push_back(ch);
        }
    }
    flush();
    return out;
}

static int run_manifest_protect_verify_gate(const fs::path& root,
                                            const std::string& manifest,
                                            const std::string& phase) {
    bool report_found = false;
    std::string report_ref = manifest_string_field(manifest, "protect_report", "", &report_found);
    bool verify_report_found = false;
    std::string verify_report_ref = manifest_string_field(manifest, "protect_verify_report", "", &verify_report_found);
    bool require_closed_found = false;
    bool require_closed = manifest_bool_option(manifest, "protect_require_closed_source", false, &require_closed_found);
    bool require_key_found = false;
    bool require_key = manifest_bool_option(manifest, "protect_require_key", false, &require_key_found);
    bool require_license_found = false;
    bool require_license = manifest_bool_option(manifest, "protect_require_license", false, &require_license_found);
    bool require_expires_found = false;
    bool require_expires = manifest_bool_option(manifest, "protect_require_expires", false, &require_expires_found);
    bool require_package_found = false;
    bool require_package = manifest_bool_option(manifest, "protect_require_package", false, &require_package_found);
    bool require_launcher_found = false;
    bool require_launcher = manifest_bool_option(manifest, "protect_require_launcher", false, &require_launcher_found);
    bool require_target_found = false;
    std::string require_target_ref = manifest_string_field(manifest, "protect_require_target", "", &require_target_found);

    bool configured = report_found || verify_report_found || require_closed_found || require_key_found ||
                      require_license_found || require_expires_found || require_package_found ||
                      require_launcher_found || require_target_found;
    if (!configured) {
        info(phase + " protect verification not configured; skipping");
        return 0;
    }
    if (!report_found || report_ref.empty()) {
        return err(phase + " protect_report is required when protect verification is configured");
    }

    fs::path report_path = utf8_path(report_ref);
    if (report_path.is_relative()) report_path = root / report_path;
    report_path = fs::absolute(report_path).lexically_normal();

    fs::path verify_report_path;
    bool signed_ci_package = phase == "ci" && fs::exists(root / kSignature);
    bool keep_report = !signed_ci_package;
    if (signed_ci_package) {
        info("ci package signature present; writing protect-verify report to a temporary path");
        verify_report_path = fs::temp_directory_path() /
            ("sura_ci_protect_verify_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    } else if (verify_report_found && !verify_report_ref.empty()) {
        verify_report_path = utf8_path(verify_report_ref);
        if (verify_report_path.is_relative()) verify_report_path = root / verify_report_path;
        verify_report_path = fs::absolute(verify_report_path).lexically_normal();
    } else {
        verify_report_path = package_gate_protect_verify_report_path(root, phase);
    }

    std::vector<std::string> required_targets = split_protect_target_list(require_target_ref);
    if (require_package && std::find(required_targets.begin(), required_targets.end(), "package") == required_targets.end()) {
        required_targets.push_back("package");
    }
    if (require_launcher && std::find(required_targets.begin(), required_targets.end(), "launcher") == required_targets.end()) {
        required_targets.push_back("launcher");
    }

    std::vector<std::string> args = {"surapkg", "protect-verify", path_to_utf8(report_path)};
    if (require_closed) args.push_back("--require-closed-source");
    if (require_key) args.push_back("--require-key");
    if (require_license) args.push_back("--require-license");
    if (require_expires) args.push_back("--require-expires");
    for (const auto& target : required_targets) {
        args.push_back("--require-target");
        args.push_back(target);
    }
    if (!verify_report_path.empty()) {
        args.push_back("--json");
        args.push_back(path_to_utf8(verify_report_path));
    }

    int code = cmd_protect_verify(args);
    if (!keep_report && !verify_report_path.empty()) {
        std::error_code cleanup_ec;
        fs::remove(verify_report_path, cleanup_ec);
    }
    if (code != 0) return code;
    if (keep_report && !verify_report_path.empty()) {
        ok(phase + " protect-verify report: " + verify_report_path.generic_string());
    }
    ok(phase + " protect verification passed");
    return 0;
}

struct PackageGateStage {
    std::string name;
    std::string status;
    std::string message;
};

static void package_gate_stage(std::vector<PackageGateStage>& stages,
                               const std::string& name,
                               const std::string& status,
                               const std::string& message) {
    stages.push_back({name, status, message});
}

static std::string package_gate_stage_action(const std::string& command_name,
                                             const PackageGateStage& stage) {
    if (stage.status != "fail") return "";
    const std::string cmd = command_name.empty() ? "ci" : command_name;
    if (stage.name == "manifest") return "create or fix sura.pkg.json with name, version, and main";
    if (stage.name == "docs" ||
        stage.name == "docs_refresh" ||
        stage.name == "docs_audit_refresh" ||
        stage.name == "docs_quality_refresh") {
        return "run surapkg docs and inspect package source, artifacts, and docs output paths";
    }
    if (stage.name == "format") return "run surapkg format and review the formatted source diff";
    if (stage.name == "check") return "run surapkg check and fix parse or type errors";
    if (stage.name == "lint") return "run surapkg lint and fix reported warnings or errors";
    if (stage.name == "tests") return "run surapkg test and fix failing package tests before surapkg " + cmd;
    if (stage.name == "bench") return "run surapkg bench and fix benchmark config or performance regressions";
    if (stage.name == "protect_verify") return "run surapkg protect with the required closed-source options, then run surapkg protect-verify";
    if (stage.name == "audit") return "run surapkg audit and fix security, dependency, tool-policy, or plugin findings";
    if (stage.name == "sign" || stage.name == "sign_refresh") return "run surapkg sign after generated release artifacts are final";
    if (stage.name == "quality") return "run surapkg quality --json quality-report.json and follow its next_actions";
    if (stage.name == "publish") return "run surapkg publish --dry-run and fix registry, version, signature, or metadata issues";
    if (stage.name == "verify_package_signature") return "run surapkg verify and re-sign only after intentional package changes";
    if (stage.name == "verify_tool_policy") return "run surapkg verify-policy or surapkg sign-policy after intentional policy changes";
    return "review the failed " + stage.name + " stage and rerun surapkg " + cmd;
}

static bool write_package_gate_json_report(const fs::path& report_path,
                                           const std::string& command_name,
                                           const std::string& schema,
                                           const fs::path& root,
                                           const std::string& name,
                                           const std::string& version,
                                           bool passed,
                                           const std::vector<PackageGateStage>& stages,
                                           bool dry_run = false) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"" << json_escape(schema) << "\",\n";
    out << "  \"root\": \"" << json_escape(root.generic_string()) << "\",\n";
    out << "  \"package\": \"" << json_escape(name) << "\",\n";
    out << "  \"version\": \"" << json_escape(version) << "\",\n";
    out << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    out << "  \"dry_run\": " << (dry_run ? "true" : "false") << ",\n";
    out << "  \"next_actions\": [\n";
    bool first_action = true;
    for (const auto& stage : stages) {
        std::string action = package_gate_stage_action(command_name, stage);
        if (action.empty()) continue;
        if (!first_action) out << ",\n";
        first_action = false;
        out << "    {\"stage\":\"" << json_escape(stage.name)
            << "\",\"message\":\"" << json_escape(stage.message)
            << "\",\"action\":\"" << json_escape(action) << "\"}";
    }
    out << "\n  ],\n";
    out << "  \"stages\": [\n";
    for (size_t i = 0; i < stages.size(); ++i) {
        const auto& stage = stages[i];
        out << "    {\"name\":\"" << json_escape(stage.name)
            << "\",\"status\":\"" << json_escape(stage.status)
            << "\",\"message\":\"" << json_escape(stage.message) << "\"}";
        if (i + 1 < stages.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return write_all(report_path, out.str());
}

static int finish_package_gate_command(const std::string& command_name,
                                       const std::string& schema,
                                       const fs::path& json_report,
                                       const fs::path& root,
                                       const std::string& name,
                                       const std::string& version,
                                       bool passed,
                                       const std::vector<PackageGateStage>& stages,
                                       const std::string& failure_message,
                                       bool dry_run = false) {
    if (!json_report.empty()) {
        if (!write_package_gate_json_report(json_report, command_name, schema, root, name, version, passed, stages, dry_run)) {
            return err("failed to write " + command_name + " JSON report: " + json_report.generic_string());
        }
        ok(command_name + " report written: " + json_report.generic_string());
    }
    if (!passed) return err(failure_message);
    return 0;
}

static int run_release_command(const std::string& source, const fs::path& json_report, bool dry_run = false) {
    fs::path root = doctor_package_root(source);
    std::string manifest = read_all(root / kManifest);
    std::vector<PackageGateStage> stages;
    const std::string schema = "sura.package.release.v1";
    if (manifest.empty()) {
        std::string msg = "sura.pkg.json not found for release: " + (root / kManifest).generic_string();
        package_gate_stage(stages, "manifest", "fail", msg);
        return finish_package_gate_command("release", schema, json_report, root, "", "", false, stages, msg, dry_run);
    }
    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "0.1.0");

    std::cout << "Sura release\n";
    std::cout << "package: " << name << "@" << version << "\n";
    std::cout << "root: " << root.generic_string() << "\n";
    if (dry_run) std::cout << "mode: dry-run\n";

    fs::path docs_index;
    if (!generate_package_docs(root, root / "docs", docs_index)) {
        package_gate_stage(stages, "docs", "fail", "release docs generation failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release docs generation failed", dry_run);
    }
    package_gate_stage(stages, "docs", "pass", "generated docs at " + docs_index.generic_string());
    ok("release docs generated: " + docs_index.generic_string());

    fs::path test_report = package_gate_test_report_path(root, "release");
    if (cmd_format_check_path(root.string()) != 0) {
        package_gate_stage(stages, "format", "fail", "release format check failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release format check failed", dry_run);
    }
    package_gate_stage(stages, "format", "pass", "format check passed");
    ok("release format check passed");
    if (cmd_check_path(root.string()) != 0) {
        package_gate_stage(stages, "check", "fail", "release check failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release check failed", dry_run);
    }
    package_gate_stage(stages, "check", "pass", "parse/typecheck passed");
    ok("release check passed");
    if (cmd_lint_path(root.string()) != 0) {
        package_gate_stage(stages, "lint", "fail", "release lint failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release lint failed", dry_run);
    }
    package_gate_stage(stages, "lint", "pass", "lint passed");
    ok("release lint passed");
    int test_code = run_package_tests(root, test_report);
    if (test_code != 0) {
        package_gate_stage(stages, "tests", "fail", "release tests failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release tests failed", dry_run);
    }
    package_gate_stage(stages, "tests", "pass", "package tests passed");
    package_gate_stage(stages, "test_report", "pass", "test report written at " + test_report.generic_string());
    ok("release test report: " + test_report.generic_string());

    if (run_manifest_bench_gate(root, manifest, "release") != 0) {
        package_gate_stage(stages, "bench", "fail", "release bench gate failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release bench gate failed", dry_run);
    }
    package_gate_stage(stages, "bench", "pass", "benchmark gate passed or was not configured");
    if (!generate_package_docs(root, root / "docs", docs_index)) {
        package_gate_stage(stages, "docs_refresh", "fail", "release docs refresh after bench failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release docs refresh after bench failed", dry_run);
    }
    package_gate_stage(stages, "docs_refresh", "pass", "refreshed docs at " + docs_index.generic_string());
    ok("release docs refreshed after bench: " + docs_index.generic_string());

    if (run_manifest_protect_verify_gate(root, manifest, "release") != 0) {
        package_gate_stage(stages, "protect_verify", "fail", "release protect verification failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release protect verification failed", dry_run);
    }
    package_gate_stage(stages, "protect_verify", "pass", "protect verification passed or was not configured");

    fs::path audit_report = package_gate_audit_report_path(root, "release");
    if (run_audit_command(root.string(), audit_report, fs::path()) != 0) {
        package_gate_stage(stages, "audit", "fail", "release audit failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release audit failed", dry_run);
    }
    package_gate_stage(stages, "audit", "pass", "audit passed");
    ok("release audit report: " + audit_report.generic_string());
    if (!generate_package_docs(root, root / "docs", docs_index, fs::path(), audit_report)) {
        package_gate_stage(stages, "docs_audit_refresh", "fail", "release docs refresh after audit failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release docs refresh after audit failed", dry_run);
    }
    package_gate_stage(stages, "docs_audit_refresh", "pass", "refreshed docs with audit summary at " + docs_index.generic_string());
    ok("release docs refreshed after audit: " + docs_index.generic_string());

    if (!write_package_signature(root)) {
        package_gate_stage(stages, "sign", "fail", "release signing failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release signing failed", dry_run);
    }
    package_gate_stage(stages, "sign", "pass", "package signature written");
    ok("release signed: " + (root / kSignature).generic_string());

    fs::path quality_report = package_gate_quality_report_path(root, "release");
    if (run_quality_command(root.string(), quality_report) != 0) {
        package_gate_stage(stages, "quality", "fail", "release quality gate failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release quality gate failed", dry_run);
    }
    package_gate_stage(stages, "quality", "pass", "quality gate passed");
    ok("release quality report: " + quality_report.generic_string());
    if (!generate_package_docs(root, root / "docs", docs_index, quality_report, audit_report, test_report)) {
        package_gate_stage(stages, "docs_quality_refresh", "fail", "release docs refresh after quality failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release docs refresh after quality failed", dry_run);
    }
    package_gate_stage(stages, "docs_quality_refresh", "pass", "refreshed docs with quality summary at " + docs_index.generic_string());
    ok("release docs refreshed after quality: " + docs_index.generic_string());
    if (!write_package_signature(root)) {
        package_gate_stage(stages, "sign_refresh", "fail", "release final signing failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages, "release final signing failed", dry_run);
    }
    package_gate_stage(stages, "sign_refresh", "pass", "package signature refreshed after quality docs");
    ok("release signature refreshed: " + (root / kSignature).generic_string());
    int publish_code = dry_run ? run_publish_command(root.string(), fs::path(), true) : cmd_publish_path(root.string());
    if (publish_code != 0) {
        package_gate_stage(stages, "publish", "fail", dry_run ? "release dry-run publish validation failed" : "release publish failed");
        return finish_package_gate_command("release", schema, json_report, root, name, version, false, stages,
                                           dry_run ? "release dry-run publish validation failed" : "release publish failed",
                                           dry_run);
    }
    package_gate_stage(stages, "publish", "pass", dry_run ? "dry-run publish validation passed" : "package published");
    ok(std::string(dry_run ? "release dry-run completed " : "release completed ") + name + "@" + version);
    return finish_package_gate_command("release", schema, json_report, root, name, version, true, stages, "", dry_run);
}

static int cmd_release(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    bool dry_run = false;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("release --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("release --json requires an output path");
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg release [path] [--dry-run] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("release accepts at most one path");
        }
    }
    return run_release_command(source, json_report, dry_run);
}

static int run_ci_command(const std::string& source, const fs::path& json_report) {
    fs::path root = doctor_package_root(source);
    std::string manifest = read_all(root / kManifest);
    std::vector<PackageGateStage> stages;
    const std::string schema = "sura.package.ci.v1";
    if (manifest.empty()) {
        std::string msg = "sura.pkg.json not found for ci: " + (root / kManifest).generic_string();
        package_gate_stage(stages, "manifest", "fail", msg);
        return finish_package_gate_command("ci", schema, json_report, root, "", "", false, stages, msg);
    }
    std::string name = normalize_name(manifest_field(manifest, "name", root.filename().string()));
    std::string version = manifest_field(manifest, "version", "0.1.0");

    std::cout << "Sura CI\n";
    std::cout << "package: " << name << "@" << version << "\n";
    std::cout << "root: " << root.generic_string() << "\n";

    fs::path docs_index;
    if (!generate_package_docs(root, root / "docs", docs_index)) {
        package_gate_stage(stages, "docs", "fail", "ci docs generation failed");
        return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci docs generation failed");
    }
    package_gate_stage(stages, "docs", "pass", "generated docs at " + docs_index.generic_string());
    ok("ci docs generated: " + docs_index.generic_string());

    bool signed_ci_package_for_tests = fs::exists(root / kSignature);
    fs::path test_report = package_gate_test_report_path(root, "ci");
    fs::path test_report_for_command = test_report;
    bool keep_test_report = !signed_ci_package_for_tests;
    if (signed_ci_package_for_tests) {
        info("ci package signature present; writing test report to a temporary path");
        test_report_for_command = fs::temp_directory_path() /
            ("sura_ci_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    }
    if (cmd_format_check_path(root.string()) != 0) {
        package_gate_stage(stages, "format", "fail", "ci format check failed");
        return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci format check failed");
    }
    package_gate_stage(stages, "format", "pass", "format check passed");
    ok("ci format check passed");
    if (cmd_check_path(root.string()) != 0) {
        package_gate_stage(stages, "check", "fail", "ci check failed");
        return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci check failed");
    }
    package_gate_stage(stages, "check", "pass", "parse/typecheck passed");
    ok("ci check passed");
    if (cmd_lint_path(root.string()) != 0) {
        package_gate_stage(stages, "lint", "fail", "ci lint failed");
        return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci lint failed");
    }
    package_gate_stage(stages, "lint", "pass", "lint passed");
    ok("ci lint passed");
    int test_code = run_package_tests(root, test_report_for_command);
    if (!keep_test_report) {
        std::error_code test_cleanup_ec;
        fs::remove(test_report_for_command, test_cleanup_ec);
    }
    if (test_code != 0) {
        package_gate_stage(stages, "tests", "fail", "ci tests failed");
        return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci tests failed");
    }
    package_gate_stage(stages, "tests", "pass", "package tests passed");
    if (keep_test_report) {
        package_gate_stage(stages, "test_report", "pass", "test report written at " + test_report.generic_string());
        ok("ci test report: " + test_report.generic_string());
    } else {
        package_gate_stage(stages, "test_report", "skip", "package signature present; skipped persisted test report");
    }

    if (run_manifest_bench_gate(root, manifest, "ci") != 0) {
        package_gate_stage(stages, "bench", "fail", "ci bench gate failed");
        return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci bench gate failed");
    }
    package_gate_stage(stages, "bench", "pass", "benchmark gate passed or was not configured");
    if (!generate_package_docs(root, root / "docs", docs_index)) {
        package_gate_stage(stages, "docs_refresh", "fail", "ci docs refresh after bench failed");
        return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci docs refresh after bench failed");
    }
    package_gate_stage(stages, "docs_refresh", "pass", "refreshed docs at " + docs_index.generic_string());
    ok("ci docs refreshed after bench: " + docs_index.generic_string());

    if (run_manifest_protect_verify_gate(root, manifest, "ci") != 0) {
        package_gate_stage(stages, "protect_verify", "fail", "ci protect verification failed");
        return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci protect verification failed");
    }
    package_gate_stage(stages, "protect_verify", "pass", "protect verification passed or was not configured");

    bool signed_ci_package_for_audit = fs::exists(root / kSignature);
    fs::path audit_report = package_gate_audit_report_path(root, "ci");
    fs::path audit_report_for_command = audit_report;
    bool keep_audit_report = !signed_ci_package_for_audit;
    if (signed_ci_package_for_audit) {
        info("ci package signature present; writing audit report to a temporary path");
        audit_report_for_command = fs::temp_directory_path() /
            ("sura_ci_audit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    }
    if (run_audit_command(root.string(), audit_report_for_command, fs::path()) != 0) {
        if (!keep_audit_report) {
            std::error_code audit_cleanup_ec;
            fs::remove(audit_report_for_command, audit_cleanup_ec);
        }
        package_gate_stage(stages, "audit", "fail", "ci audit failed");
        return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci audit failed");
    }
    if (!keep_audit_report) {
        std::error_code audit_cleanup_ec;
        fs::remove(audit_report_for_command, audit_cleanup_ec);
    }
    package_gate_stage(stages, "audit", "pass", "audit passed");
    if (keep_audit_report) {
        ok("ci audit report: " + audit_report.generic_string());
        if (!generate_package_docs(root, root / "docs", docs_index, fs::path(), audit_report)) {
            package_gate_stage(stages, "docs_audit_refresh", "fail", "ci docs refresh after audit failed");
            return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci docs refresh after audit failed");
        }
        package_gate_stage(stages, "docs_audit_refresh", "pass", "refreshed docs with audit summary at " + docs_index.generic_string());
        ok("ci docs refreshed after audit: " + docs_index.generic_string());
    } else {
        package_gate_stage(stages, "docs_audit_refresh", "skip", "package signature present; skipped persisted audit docs refresh");
    }

    if (fs::exists(root / kSignature)) {
        if (cmd_verify(root.string()) != 0) {
            package_gate_stage(stages, "verify_package_signature", "fail", "ci package signature verification failed");
            return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci package signature verification failed");
        }
        package_gate_stage(stages, "verify_package_signature", "pass", "package signature verified");
        ok("ci package signature verified");
    } else {
        package_gate_stage(stages, "verify_package_signature", "skip", "package signature not present");
        info("ci package signature not present; skipping verify");
    }

    if (fs::exists(root / kToolPolicySignature)) {
        if (cmd_verify_policy(root.string()) != 0) {
            package_gate_stage(stages, "verify_tool_policy", "fail", "ci tool policy verification failed");
            return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci tool policy verification failed");
        }
        package_gate_stage(stages, "verify_tool_policy", "pass", "tool policy signature verified");
        ok("ci tool policy verified");
    } else if (fs::exists(root / kToolPolicyManifest)) {
        package_gate_stage(stages, "verify_tool_policy", "skip", "tool policy signature not present");
        info("ci tool policy signature not present; skipping verify-policy");
    } else {
        package_gate_stage(stages, "verify_tool_policy", "skip", "tool policy not present");
    }

    bool signed_ci_package_for_quality = fs::exists(root / kSignature);
    fs::path quality_report = package_gate_quality_report_path(root, "ci");
    fs::path quality_report_for_command = quality_report;
    bool keep_quality_report = !signed_ci_package_for_quality;
    if (signed_ci_package_for_quality) {
        info("ci package signature present; writing quality report to a temporary path");
        quality_report_for_command = fs::temp_directory_path() /
            ("sura_ci_quality_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    }
    if (run_quality_command(root.string(), quality_report_for_command) != 0) {
        if (!keep_quality_report) {
            std::error_code quality_cleanup_ec;
            fs::remove(quality_report_for_command, quality_cleanup_ec);
        }
        package_gate_stage(stages, "quality", "fail", "ci quality gate failed");
        return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci quality gate failed");
    }
    if (!keep_quality_report) {
        std::error_code quality_cleanup_ec;
        fs::remove(quality_report_for_command, quality_cleanup_ec);
    }
    package_gate_stage(stages, "quality", "pass", "quality gate passed");
    if (keep_quality_report) {
        ok("ci quality report: " + quality_report.generic_string());
        if (!generate_package_docs(root, root / "docs", docs_index, quality_report,
                                   keep_audit_report ? audit_report : fs::path(),
                                   keep_test_report ? test_report : fs::path())) {
            package_gate_stage(stages, "docs_quality_refresh", "fail", "ci docs refresh after quality failed");
            return finish_package_gate_command("ci", schema, json_report, root, name, version, false, stages, "ci docs refresh after quality failed");
        }
        package_gate_stage(stages, "docs_quality_refresh", "pass", "refreshed docs with quality summary at " + docs_index.generic_string());
        ok("ci docs refreshed after quality: " + docs_index.generic_string());
    } else {
        package_gate_stage(stages, "docs_quality_refresh", "skip", "package signature present; skipped persisted quality docs refresh");
    }
    ok("ci completed " + name + "@" + version);
    return finish_package_gate_command("ci", schema, json_report, root, name, version, true, stages, "");
}

static int cmd_ci(const std::vector<std::string>& argv) {
    std::string source;
    fs::path json_report;
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--json") {
            if (i + 1 >= argv.size()) return err("ci --json requires an output path");
            json_report = fs::path(argv[++i]);
        } else if (arg.rfind("--json=", 0) == 0) {
            json_report = fs::path(arg.substr(7));
            if (json_report.empty()) return err("ci --json requires an output path");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: surapkg ci [path] [--json report.json]\n";
            return 0;
        } else if (source.empty()) {
            source = arg;
        } else {
            return err("ci accepts at most one path");
        }
    }
    return run_ci_command(source, json_report);
}

static void usage() {
    std::cout
        << "Sura package manager\n"
        << "Usage:\n"
        << "  surapkg new <name> [--json report.json]  Create a runnable starter project with tests and VS Code settings\n"
        << "  surapkg examples [query] [--json]  List installed runnable examples and optional requirements\n"
        << "  surapkg example <id> <project-directory> [--json report.json]  Create a project from an installed example\n"
        << "  surapkg init [name] [--json report.json]  Create sura.pkg.json and src/<name>.sura\n"
        << "  surapkg create <name> [--json report.json]  Create a package skeleton directory\n"
        << "  surapkg agent <name> [--json report.json]  Create an AI automation agent template\n"
        << "  surapkg embed <name> [--json report.json]  Create a native C++ host embedding template\n"
        << "  surapkg version [path] [major|minor|patch|version] [--json report.json]  Show or update package manifest version\n"
        << "  surapkg install <path|file|name[@version|range]> [--json report.json]  Install local or registry package\n"
        << "  surapkg outdated [name] [--json]  Show installed packages with newer registry versions\n"
        << "  surapkg update [name] [--json report.json]  Update installed packages from the registry\n"
        << "  surapkg publish [path] [--dry-run] [--json report.json]  Validate or publish to local/HTTP registry\n"
        << "  surapkg search [query] [--json]  Search local or HTTP registry index\n"
        << "  surapkg stats [name] [--json]  Show local or HTTP registry download/publish stats\n"
        << "  surapkg analytics [name] [--json]  Show public registry download trends and top packages\n"
        << "  surapkg registry-health [path] [--fail-on-warning] [--json]  Summarize local/HTTP registry health and moderation queues\n"
        << "  surapkg recover-token <user> <recovery-code> [new-token] [--json report.json]  Recover a registry account token\n"
        << "  surapkg owners [name] [--json]  List registry package owners\n"
        << "  surapkg yanks [name] [--json]  List yanked registry package versions\n"
        << "  surapkg advisory <name[@version]> --severity high --title text --description text [--json]  Create a registry security advisory\n"
        << "  surapkg advisories [name[@version]] [--fail-on high] [--json]  List or gate registry security advisories\n"
        << "  surapkg yank <name@version> <reason> [--json]  Yank a bad registry package version\n"
        << "  surapkg unyank <name@version> [reason] [--json]  Restore a yanked registry package version\n"
        << "  surapkg report <name[@version]> <reason> [--json]  Report a package for registry review\n"
        << "  surapkg reports [status] [--json]  List registry abuse reports\n"
        << "  surapkg review-report <id> <status> [note] [--json]  Review an abuse report\n"
        << "  surapkg doctor [path] [--json report.json]  Diagnose local Sura, package, registry, and tooling setup\n"
        << "  surapkg clean [path] [--dry-run] [--json report.json]  Remove safe generated logs and temporary smoke outputs\n"
        << "  surapkg index [--json report.json]  Rebuild registry/index.json\n"
        << "  surapkg lock [--json report.json]  Write sura.lock.json for installed dependency graph\n"
        << "  surapkg sign [path] [--json report.json]  Write sura.pkg.sig integrity/keyed/public-key signature\n"
        << "  surapkg sign-policy [path] [--json report.json]  Write sura.tools.sig for a package tool policy\n"
        << "  surapkg verify [path] [--json report.json]  Verify sura.lock.json or a package signature\n"
        << "  surapkg verify-policy [path] [--json report.json]  Verify sura.tools.sig for a package tool policy\n"
        << "  surapkg verify-registry [path] [--json report.json]  Verify local/HTTP registry index, bundles, signatures, metadata\n"
        << "  surapkg trust-key <id> <pem> [--json report.json]  Trust a public signing key for registry/key-dir verification\n"
        << "  surapkg resolve [--json]     Resolve manifest and transitive deps against packages/local/HTTP registry\n"
        << "  surapkg tree [--json]       Show the resolved dependency tree\n"
        << "  surapkg why <name> [--json] Explain why a dependency is in the resolved graph\n"
        << "  surapkg format [path] [--check] [--json report.json]\n"
        << "  surapkg check [path] [--json report.json] [--strict|--legacy-types]  Strict by default\n"
        << "  surapkg lint [path] [--json report.json] [--fail-on-warning]\n"
        << "  surapkg audit [path] [--json report.json] [--sarif report.sarif]  Scan package source for risky APIs\n"
        << "  surapkg policy [path] [--json report.json]  Generate starter sura.tools.json from package tool specs\n"
        << "  surapkg tool-log <jsonl>     Summarize SURA_TOOL_AUDIT_LOG events; use --json for CI\n"
        << "  surapkg bind-c <header.h> [--json report.json]  Generate Sura ffi_call wrappers from simple C/C++ C-ABI headers\n"
        << "  surapkg docs [outdir] [--json report.json]  Generate package HTML docs, api.json, and search-index.json\n"
        << "  surapkg quality [path] [--json report.json]  Score package readiness for CI/release gates\n"
        << "  surapkg ci [path] [--json report.json]  Generate docs, test, optional bench, audit, verify, and quality-check\n"
        << "  surapkg release [path] [--dry-run] [--json report.json]  Generate docs, test, optional bench, sign, quality-check, and publish\n"
        << "  surapkg protect [path]       Compile package main to protected .sura.srp or embedded launcher exe\n"
        << "  surapkg protect-verify <protect-report.json> [--json report.json]  Verify protected release evidence\n"
        << "  surapkg run [path] [--json run.json] [--] [args...]  Run package manifest main with script argv\n"
        << "  surapkg profile [path] [--json profile.json] [--no-jit] [--] [args...]\n"
        << "  surapkg bench [path] [--json bench.json] [--summary bench.md] [--python script.py] [--min-speedup ratio] [--] [args...]\n"
        << "  surapkg bench-dashboard [--out html] [--json report.json] [--summary summary.md] [--release-notes notes.md] [--native-perf native_perf.json]  Generate benchmark dashboard artifacts\n"
        << "  surapkg test [path] [--json file.json|--report file.json] [--junit file.xml] [--no-jit]\n"
        << "  surapkg remove <name> [--json report.json]  Remove an installed package\n"
        << "  surapkg list [--json]        List stdlib, installed packages, and manifest deps\n"
        << "  surapkg info <name> [--json] Show package metadata, benchmark evidence, and exported symbols\n"
        << "  surapkg restore [--json report.json]  Install manifest and transitive dependencies locally\n";
}

int main(int argc, char* argv[]) {
    std::vector<std::string> args = command_line_args(argc, argv);
    if (args.size() < 2) {
        usage();
        return 0;
    }

    std::string cmd = args[1];
    std::string arg = args.size() >= 3 ? args[2] : "";

    try {
        if (cmd == "new") return cmd_new(args);
        if (cmd == "examples") return cmd_examples(args);
        if (cmd == "example") return cmd_example(args);
        if (cmd == "init") return cmd_init(args);
        if (cmd == "create") return cmd_create(args);
        if (cmd == "agent") return cmd_agent(args);
        if (cmd == "embed") return cmd_embed(args);
        if (cmd == "version") return cmd_version(args);
        if (cmd == "install") return cmd_install(args);
        if (cmd == "outdated") return cmd_outdated(args);
        if (cmd == "update") return cmd_update(args);
        if (cmd == "publish") return cmd_publish(args);
        if (cmd == "search") return cmd_search(args);
        if (cmd == "stats") return cmd_stats(args);
        if (cmd == "analytics") return cmd_analytics(args);
        if (cmd == "registry-health") return cmd_registry_health(args);
        if (cmd == "recover-token") return cmd_recover_token(args);
        if (cmd == "owners") return cmd_owners(args);
        if (cmd == "yanks") return cmd_yanks(args);
        if (cmd == "advisory") return cmd_advisory(args);
        if (cmd == "advisories") return cmd_advisories(args);
        if (cmd == "yank") return cmd_yank_common(args, false);
        if (cmd == "unyank") return cmd_yank_common(args, true);
        if (cmd == "report") return cmd_report(args);
        if (cmd == "reports") return cmd_reports(args);
        if (cmd == "review-report") return cmd_review_report(args);
        if (cmd == "doctor") return cmd_doctor(args);
        if (cmd == "clean") return cmd_clean(args);
        if (cmd == "index") return cmd_registry_index(args);
        if (cmd == "lock") return cmd_lock(args);
        if (cmd == "sign") return cmd_sign(args);
        if (cmd == "sign-policy") return cmd_sign_policy(args);
        if (cmd == "verify") return cmd_verify(args);
        if (cmd == "verify-policy") return cmd_verify_policy(args);
        if (cmd == "verify-registry") return cmd_verify_registry(args);
        if (cmd == "trust-key") return cmd_trust_key(args);
        if (cmd == "resolve") return cmd_resolve(args);
        if (cmd == "tree" || cmd == "deps") {
            bool json_output = false;
            for (size_t i = 2; i < args.size(); ++i) {
                const std::string& opt = args[i];
                if (opt == "--json") json_output = true;
                else if (opt == "--help" || opt == "-h") {
                    std::cout << "Usage: surapkg tree [--json]\n";
                    return 0;
                } else {
                    return err("tree accepts only --json");
                }
            }
            return cmd_tree(json_output);
        }
        if (cmd == "why") {
            std::string package_name;
            bool json_output = false;
            for (size_t i = 2; i < args.size(); ++i) {
                const std::string& opt = args[i];
                if (opt == "--json") json_output = true;
                else if (opt == "--help" || opt == "-h") {
                    std::cout << "Usage: surapkg why <name> [--json]\n";
                    return 0;
                } else if (package_name.empty()) {
                    package_name = opt;
                } else {
                    return err("why accepts one package name and optional --json");
                }
            }
            return cmd_why(package_name, json_output);
        }
        if (cmd == "format" || cmd == "fmt") return cmd_format(argc, argv);
        if (cmd == "check") return cmd_check(argc, argv);
        if (cmd == "lint") return cmd_lint(argc, argv);
        if (cmd == "audit") return cmd_audit(argc, argv);
        if (cmd == "policy") return cmd_policy(args);
        if (cmd == "tool-log") return cmd_tool_log(args);
        if (cmd == "bind-c") return cmd_bind_c(args);
        if (cmd == "docs") return cmd_docs(args);
        if (cmd == "quality") return cmd_quality(args);
        if (cmd == "ci") return cmd_ci(args);
        if (cmd == "release") return cmd_release(args);
        if (cmd == "protect") return cmd_protect(argc, argv);
        if (cmd == "protect-verify") return cmd_protect_verify(args);
        if (cmd == "run") return cmd_run(args);
        if (cmd == "profile") return cmd_profile(args);
        if (cmd == "bench") return cmd_bench(args);
        if (cmd == "bench-dashboard" || cmd == "dashboard") return cmd_bench_dashboard(args);
        if (cmd == "test") return cmd_test(args);
        if (cmd == "remove") return cmd_remove(args);
        if (cmd == "list") return cmd_list(args);
        if (cmd == "info") return cmd_info(args);
        if (cmd == "restore") return cmd_restore(args);
        if (cmd == "help" || cmd == "--help" || cmd == "-h") {
            usage();
            return 0;
        }
        return err("unknown command: " + cmd);
    } catch (const std::exception& e) {
        return err(e.what());
    }
}
