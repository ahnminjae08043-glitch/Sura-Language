#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static std::string argument_after(int argc, char** argv, const std::string& flag) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == flag) return argv[index + 1];
    }
    return "";
}

static int integer_after(int argc, char** argv, const std::string& flag, int fallback) {
    std::string text = argument_after(argc, argv, flag);
    if (text.empty()) return fallback;
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

static void write_frame(std::ofstream& output, const std::string& header,
                        const std::vector<unsigned char>& pixels) {
    output.write(header.data(), (std::streamsize)header.size());
    output.write((const char*)pixels.data(), (std::streamsize)pixels.size());
}

int main(int argc, char** argv) {
    if (argc < 2) return 90;
    std::string input_path = argument_after(argc, argv, "-i");
    std::string filter = argument_after(argc, argv, "-vf");
    std::string protocol = argument_after(argc, argv, "-protocol_whitelist");
    std::string output_path = argv[argc - 1];
    int frame_limit = integer_after(argc, argv, "-frames:v", 0);
    if (input_path.empty() || output_path.empty() || frame_limit <= 0) return 91;
    if (protocol != "file,pipe") return 92;
    if (filter.find("fps=") == std::string::npos
        || filter.find("scale=4:2") == std::string::npos) return 93;

    std::ifstream input(input_path, std::ios::binary);
    std::string mode((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    if (mode == "sleep") {
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        return 0;
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return 94;
    std::cerr << "fake decoder stderr: P2 garbage must stay separate\n";
    if (mode == "badmagic") {
        output << "P2\n4 2\n255\n0 0 0 0 0 0 0 0\n";
        return 0;
    }
    if (mode == "truncated") {
        output << "P5\n4 2\n255\n";
        const unsigned char short_pixels[] = {0, 1, 2};
        output.write((const char*)short_pixels, 3);
        return 0;
    }

    const std::vector<std::pair<std::string, std::vector<unsigned char>>> frames = {
        {"P5\n# header comment\n4 2\n255\n", {0, 85, 170, 255, 255, 170, 85, 0}},
        {"P5\r\n4\t2\r\n255\n", {0, 32, 10, 35, 255, 128, 64, 192}},
        {"P5\n4 2\n255\n", {255, 255, 255, 255, 0, 0, 0, 0}}
    };
    int count = std::min<int>(frame_limit, (int)frames.size());
    for (int index = 0; index < count; ++index) {
        write_frame(output, frames[index].first, frames[index].second);
    }
    output.close();
    return mode == "fail" ? 7 : 0;
}

