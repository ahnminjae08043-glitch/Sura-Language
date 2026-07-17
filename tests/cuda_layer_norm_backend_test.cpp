#include "../cuda_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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

void check_close(const std::vector<float>& actual,
                 const std::vector<long double>& expected,
                 long double relative_tolerance, const std::string& label) {
    require(actual.size() == expected.size(), label + " size mismatch");
    for (size_t i = 0; i < actual.size(); ++i) {
        const long double tolerance = relative_tolerance
            * std::max(1.0L, std::fabs(expected[i]));
        if (!std::isfinite(actual[i])
            || std::fabs((long double)actual[i] - expected[i]) > tolerance) {
            fail(label + " mismatch at " + std::to_string(i)
                 + ": expected " + std::to_string((double)expected[i])
                 + ", got " + std::to_string(actual[i]));
        }
    }
}

Driver::DeviceHandle allocate(Driver& driver, size_t count,
                              std::vector<Driver::DeviceHandle>& handles) {
    Driver::DeviceHandle handle = Driver::INVALID_DEVICE_HANDLE;
    require(driver.allocate_f32(count, handle), "allocate: " + driver.error());
    handles.push_back(handle);
    return handle;
}

void upload(Driver& driver, Driver::DeviceHandle handle,
            const std::vector<float>& values) {
    require(driver.upload_f32(handle, values), "upload: " + driver.error());
}

std::vector<float> download(Driver& driver, Driver::DeviceHandle handle,
                            size_t count) {
    std::vector<float> values(count);
    require(driver.download_f32(handle, 0, values.data(), count),
            "download: " + driver.error());
    return values;
}

} // namespace

int main() {
    Driver& driver = Driver::instance();
    if (!driver.available()) fail("CUDA unavailable: " + driver.error());

    constexpr uint32_t rows = 2;
    constexpr uint32_t features = 2;
    constexpr float epsilon = 1.0e-5f;
    const std::vector<float> input = {
        16777216.0f, 16777218.0f,
        7.0f, 7.0f
    };
    const std::vector<float> weight = {1.5f, -0.5f};
    const std::vector<float> bias = {0.25f, -0.75f};
    const std::vector<float> grad_output = {1.0f, 2.0f, -1.0f, 3.0f};

    std::vector<long double> expected_output(input.size());
    std::vector<long double> expected_dx(input.size());
    std::vector<long double> expected_dw(features, 0.0L);
    std::vector<long double> expected_db(features, 0.0L);
    for (uint32_t row = 0; row < rows; ++row) {
        const size_t base = (size_t)row * features;
        const long double anchor = input[base];
        long double delta_sum = 0.0L;
        for (uint32_t col = 0; col < features; ++col) {
            delta_sum += (long double)input[base + col] - anchor;
        }
        const long double mean_delta = delta_sum / features;
        long double variance = 0.0L;
        for (uint32_t col = 0; col < features; ++col) {
            const long double centered =
                ((long double)input[base + col] - anchor) - mean_delta;
            variance += centered * centered;
        }
        variance /= features;
        const long double rstd = 1.0L / std::sqrt(variance + epsilon);
        long double mean_h = 0.0L;
        long double mean_h_xhat = 0.0L;
        for (uint32_t col = 0; col < features; ++col) {
            const long double xhat =
                (((long double)input[base + col] - anchor) - mean_delta) * rstd;
            const long double upstream = grad_output[base + col];
            const long double h = upstream * weight[col];
            expected_output[base + col] = xhat * weight[col] + bias[col];
            expected_dw[col] += upstream * xhat;
            expected_db[col] += upstream;
            mean_h += h;
            mean_h_xhat += h * xhat;
        }
        mean_h /= features;
        mean_h_xhat /= features;
        for (uint32_t col = 0; col < features; ++col) {
            const long double xhat =
                (((long double)input[base + col] - anchor) - mean_delta) * rstd;
            const long double h = (long double)grad_output[base + col] * weight[col];
            expected_dx[base + col] =
                rstd * (h - mean_h - xhat * mean_h_xhat);
        }
    }

    std::vector<Driver::DeviceHandle> handles;
    const auto input_h = allocate(driver, input.size(), handles);
    const auto weight_h = allocate(driver, weight.size(), handles);
    const auto bias_h = allocate(driver, bias.size(), handles);
    const auto output_h = allocate(driver, input.size(), handles);
    const auto mean_h = allocate(driver, rows, handles);
    const auto rstd_h = allocate(driver, rows, handles);
    const auto grad_output_h = allocate(driver, grad_output.size(), handles);
    const auto dx_h = allocate(driver, input.size(), handles);
    const auto dw_h = allocate(driver, features, handles);
    const auto db_h = allocate(driver, features, handles);
    const auto short_h = allocate(driver, 1, handles);
    upload(driver, input_h, input);
    upload(driver, weight_h, weight);
    upload(driver, bias_h, bias);
    upload(driver, grad_output_h, grad_output);

    driver.reset_stats();
    require(driver.layer_norm_f32(input_h, weight_h, bias_h, output_h,
                                  mean_h, rstd_h, rows, features, epsilon,
                                  true, true, true),
            "forward: " + driver.error());
    require(driver.layer_norm_backward_f32(input_h, weight_h, grad_output_h,
                                            mean_h, rstd_h, dx_h, rows,
                                            features, epsilon, true),
            "input backward: " + driver.error());
    require(driver.layer_norm_parameter_backward_f32(
                input_h, grad_output_h, mean_h, rstd_h, dw_h, db_h,
                rows, features, epsilon, true, true),
            "parameter backward: " + driver.error());
    require(driver.synchronize(), "synchronize: " + driver.error());
    check_close(download(driver, output_h, input.size()), expected_output,
                2.0e-6L, "forward");
    check_close(download(driver, dx_h, input.size()), expected_dx,
                2.0e-5L, "dx");
    check_close(download(driver, dw_h, features), expected_dw,
                2.0e-6L, "dweight");
    check_close(download(driver, db_h, features), expected_db,
                2.0e-6L, "dbias");
    auto stats = driver.stats_snapshot();
    require(stats.layer_norm_launches == 3 && stats.kernel_launches == 3,
            "successful dispatch counters must record exactly three kernels");

    require(!driver.layer_norm_f32(input_h, weight_h, bias_h, input_h,
                                   mean_h, rstd_h, rows, features, epsilon,
                                   true, true, true),
            "forward accepted output/input alias");
    require(!driver.layer_norm_f32(input_h, short_h, bias_h, output_h,
                                   mean_h, rstd_h, rows, features, epsilon,
                                   true, true, true),
            "forward accepted undersized weight");
    require(!driver.layer_norm_backward_f32(
                input_h, weight_h, grad_output_h, mean_h, mean_h, dx_h,
                rows, features, epsilon, true),
            "input backward accepted aliased saved statistics");
    require(!driver.layer_norm_parameter_backward_f32(
                input_h, grad_output_h, mean_h, rstd_h, dw_h, dw_h,
                rows, features, epsilon, true, true),
            "parameter backward accepted aliased outputs");
    require(!driver.layer_norm_backward_f32(
                input_h, weight_h, grad_output_h, mean_h, rstd_h, dx_h,
                rows, features, std::numeric_limits<float>::infinity(), true),
            "input backward accepted non-finite epsilon");
    stats = driver.stats_snapshot();
    require(stats.layer_norm_launches == 3 && stats.kernel_launches == 3,
            "rejected calls changed dispatch counters");

    for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        require(driver.free_device(*it), "free: " + driver.error());
    }
    std::cout << "PASS layer_norm_launches=" << stats.layer_norm_launches
              << " h2d_bytes=" << stats.h2d_bytes
              << " d2h_bytes=" << stats.d2h_bytes << '\n';
    return 0;
}
