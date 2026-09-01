#include "../cuda_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Driver = SuraStd::SuraCudaDriver;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

std::vector<float> expected_matmul(const std::vector<float>& left,
                                   const std::vector<float>& right,
                                   uint32_t rows, uint32_t cols,
                                   uint32_t inner, bool transpose_left,
                                   bool transpose_right) {
    std::vector<float> output((size_t)rows * cols, 0.0f);
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t col = 0; col < cols; ++col) {
            float value = 0.0f;
            for (uint32_t k = 0; k < inner; ++k) {
                size_t left_index = transpose_left
                    ? (size_t)k * rows + row
                    : (size_t)row * inner + k;
                size_t right_index = transpose_right
                    ? (size_t)col * inner + k
                    : (size_t)k * cols + col;
                value = std::fma(left[left_index], right[right_index], value);
            }
            output[(size_t)row * cols + col] = value;
        }
    }
    return output;
}

void check_close(const std::vector<float>& actual,
                 const std::vector<float>& expected,
                 const std::string& label) {
    require(actual.size() == expected.size(), label + " size mismatch");
    for (size_t index = 0; index < actual.size(); ++index) {
        float tolerance = 2.0e-5f * std::max(1.0f, std::fabs(expected[index]));
        if (!std::isfinite(actual[index])
            || std::fabs(actual[index] - expected[index]) > tolerance) {
            fail(label + " mismatch at " + std::to_string(index)
                 + ": expected " + std::to_string(expected[index])
                 + ", got " + std::to_string(actual[index]));
        }
    }
}

void free_checked(Driver& driver, Driver::DeviceHandle& handle) {
    if (handle != Driver::INVALID_DEVICE_HANDLE && !driver.free_device(handle)) {
        fail("free_device: " + driver.error());
    }
}

void run_device_case(Driver& driver, bool transpose_left,
                     bool transpose_right) {
    constexpr uint32_t rows = 3;
    constexpr uint32_t cols = 4;
    constexpr uint32_t inner = 5;
    std::vector<float> left((size_t)rows * inner);
    std::vector<float> right((size_t)inner * cols);
    for (size_t i = 0; i < left.size(); ++i) {
        left[i] = ((int)(i * 7 % 17) - 8) * 0.125f;
    }
    for (size_t i = 0; i < right.size(); ++i) {
        right[i] = ((int)(i * 11 % 19) - 9) * 0.0625f;
    }
    std::vector<float> expected = expected_matmul(
        left, right, rows, cols, inner, transpose_left, transpose_right);

    Driver::DeviceHandle left_handle = Driver::INVALID_DEVICE_HANDLE;
    Driver::DeviceHandle right_handle = Driver::INVALID_DEVICE_HANDLE;
    Driver::DeviceHandle output_handle = Driver::INVALID_DEVICE_HANDLE;
    require(driver.allocate_f32(left.size(), left_handle),
            "allocate left: " + driver.error());
    require(driver.allocate_f32(right.size(), right_handle),
            "allocate right: " + driver.error());
    require(driver.allocate_f32(expected.size(), output_handle),
            "allocate output: " + driver.error());
    require(driver.upload_f32(left_handle, 0, left.data(), left.size()),
            "upload left: " + driver.error());
    require(driver.upload_f32(right_handle, 0, right.data(), right.size()),
            "upload right: " + driver.error());
    require(driver.matmul_device_f32(left_handle, right_handle, output_handle,
                                     rows, cols, inner,
                                     transpose_left, transpose_right),
            "matmul_device_f32: " + driver.error());
    require(driver.synchronize(), "synchronize: " + driver.error());
    std::vector<float> actual(expected.size());
    require(driver.download_f32(output_handle, 0, actual.data(), actual.size()),
            "download output: " + driver.error());
    check_close(actual, expected,
                std::string("transpose_left=") + (transpose_left ? "1" : "0")
                + ",transpose_right=" + (transpose_right ? "1" : "0"));
    free_checked(driver, output_handle);
    free_checked(driver, right_handle);
    free_checked(driver, left_handle);
}

void run_host_case(Driver& driver) {
    std::vector<float> left = {1.0f, -2.0f, 3.0f, 4.0f, 0.5f, -1.0f};
    std::vector<float> right = {
        2.0f, 1.0f, -1.0f, 0.0f,
        3.0f, -2.0f, 0.5f, 4.0f,
        -1.0f, 2.0f, 3.0f, -0.5f
    };
    std::vector<float> actual;
    std::vector<float> expected = expected_matmul(
        left, right, 2, 4, 3, false, false);
    require(driver.matmul_f32(left, right, actual, 2, 4, 3),
            "host matmul_f32: " + driver.error());
    check_close(actual, expected, "host matmul_f32");
}

void run_benchmark(Driver& driver) {
    constexpr uint32_t size = 1024;
    constexpr int warmup = 3;
    constexpr int runs = 10;
    const size_t elements = (size_t)size * size;
    std::vector<float> left(elements);
    std::vector<float> right(elements);
    for (size_t i = 0; i < elements; ++i) {
        left[i] = ((int)(i * 13 % 23) - 11) * 0.01f;
        right[i] = ((int)(i * 17 % 29) - 14) * 0.0075f;
    }
    Driver::DeviceHandle left_handle = Driver::INVALID_DEVICE_HANDLE;
    Driver::DeviceHandle right_handle = Driver::INVALID_DEVICE_HANDLE;
    Driver::DeviceHandle output_handle = Driver::INVALID_DEVICE_HANDLE;
    require(driver.allocate_f32(elements, left_handle),
            "benchmark allocate left: " + driver.error());
    require(driver.allocate_f32(elements, right_handle),
            "benchmark allocate right: " + driver.error());
    require(driver.allocate_f32(elements, output_handle),
            "benchmark allocate output: " + driver.error());
    require(driver.upload_f32(left_handle, left),
            "benchmark upload left: " + driver.error());
    require(driver.upload_f32(right_handle, right),
            "benchmark upload right: " + driver.error());
    for (int i = 0; i < warmup; ++i) {
        require(driver.matmul_device_f32(left_handle, right_handle, output_handle,
                                         size, size, size),
                "benchmark warmup matmul: " + driver.error());
        require(driver.synchronize(),
                "benchmark warmup synchronize: " + driver.error());
    }
    driver.reset_stats();
    std::vector<double> milliseconds;
    for (int i = 0; i < runs; ++i) {
        auto start = std::chrono::steady_clock::now();
        require(driver.matmul_device_f32(left_handle, right_handle, output_handle,
                                         size, size, size),
                "benchmark matmul: " + driver.error());
        require(driver.synchronize(),
                "benchmark synchronize: " + driver.error());
        auto finish = std::chrono::steady_clock::now();
        milliseconds.push_back(std::chrono::duration<double, std::milli>(
            finish - start).count());
    }
    std::sort(milliseconds.begin(), milliseconds.end());
    Driver::StatsSnapshot stats = driver.stats_snapshot();
    require(stats.matmul_launches == runs,
            "benchmark dispatch counter mismatch");

    std::vector<float> first_row(size);
    require(driver.download_f32(output_handle, 0, first_row.data(), size),
            "benchmark download: " + driver.error());
    float expected_first = 0.0f;
    for (uint32_t k = 0; k < size; ++k) {
        expected_first = std::fma(left[k], right[(size_t)k * size],
                                  expected_first);
    }
    require(std::fabs(first_row[0] - expected_first)
                <= 5.0e-4f * std::max(1.0f, std::fabs(expected_first)),
            "benchmark result validation failed");
    std::cout << "BENCH backend="
              << (driver.cublas_available() ? "cublas" : "ptx-reference")
              << " size=" << size << " runs=" << runs
              << " median_ms=" << milliseconds[milliseconds.size() / 2]
              << " min_ms=" << milliseconds.front()
              << " max_ms=" << milliseconds.back()
              << " cublas_launches=" << stats.cublas_matmul_launches
              << " reference_launches=" << stats.reference_matmul_launches
              << '\n';
    free_checked(driver, output_handle);
    free_checked(driver, right_handle);
    free_checked(driver, left_handle);
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc >= 2 ? argv[1] : "auto";
    Driver& driver = Driver::instance();

    if (mode == "cuda-unavailable") {
        require(!driver.available(), "CUDA unexpectedly available");
        std::vector<float> output = {123.0f};
        require(!driver.matmul_f32({1.0f}, {1.0f}, output, 1, 1, 1),
                "matmul unexpectedly succeeded with CUDA disabled");
        Driver::StatsSnapshot stats = driver.stats_snapshot();
        require(stats.matmul_launches == 0 && stats.kernel_launches == 0,
                "disabled CUDA recorded a launch");
        std::cout << "PASS cuda-unavailable error=" << driver.error() << '\n';
        return 0;
    }

    if (!driver.available()) fail("CUDA unavailable: " + driver.error());
    const bool has_cublas = driver.cublas_available();
    if (mode == "cublas") {
        require(has_cublas, "cuBLAS unavailable: " + driver.cublas_error());
    } else if (mode == "reference") {
        require(!has_cublas, "cuBLAS unexpectedly available");
    } else if (mode != "auto" && mode != "benchmark") {
        fail("usage: cuda_cublas_backend_test [auto|cublas|reference|benchmark|cuda-unavailable]");
    }

    driver.reset_stats();
    run_device_case(driver, false, false);
    run_device_case(driver, true, false);
    run_device_case(driver, false, true);
    run_device_case(driver, true, true);
    run_host_case(driver);
    Driver::StatsSnapshot stats = driver.stats_snapshot();
    require(stats.matmul_launches == 5, "expected five matmul launches");
    require(stats.kernel_launches == 5,
            "expected five logical CUDA kernel/library dispatches");
    require(stats.cublas_matmul_launches + stats.reference_matmul_launches
                == stats.matmul_launches,
            "backend counters do not partition matmul_launches");
    require(stats.cublas_available == has_cublas,
            "snapshot cuBLAS availability disagrees with driver");
    if (has_cublas) {
        require(stats.cublas_matmul_launches == 5
                    && stats.reference_matmul_launches == 0,
                "cuBLAS mode used the reference kernel");
    } else {
        require(stats.cublas_matmul_launches == 0
                    && stats.reference_matmul_launches == 5,
                "reference mode did not use the PTX kernel exclusively");
    }
    std::cout << "PASS backend=" << (has_cublas ? "cublas" : "ptx-reference")
              << " library=" << driver.cublas_library_name()
              << " cublas_launches=" << stats.cublas_matmul_launches
              << " reference_launches=" << stats.reference_matmul_launches
              << " h2d_bytes=" << stats.h2d_bytes
              << " d2h_bytes=" << stats.d2h_bytes << '\n';
    if (mode == "benchmark") run_benchmark(driver);
    return 0;
}
