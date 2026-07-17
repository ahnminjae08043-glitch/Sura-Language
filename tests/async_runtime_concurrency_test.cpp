#include "../stdlib.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

using namespace SuraStd;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Predicate>
void wait_until(Predicate predicate, std::chrono::milliseconds timeout, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
        std::this_thread::yield();
    }
}

void test_fifo_backpressure_and_cancel_removal() {
    AsyncRuntime runtime;
    require(runtime.configure(1, 8), "initial configure failed");

    std::atomic<bool> release_gate{false};
    std::atomic<bool> gate_started{false};
    const int gate = runtime.submit([&](const AsyncCancellationToken& token) {
        gate_started.store(true, std::memory_order_release);
        while (!release_gate.load(std::memory_order_acquire)) {
            if (token.wait_for(std::chrono::milliseconds(1))) throw AsyncCancelled{};
        }
        return std::string("gate");
    }, 0, "test", 1);
    wait_until([&] { return gate_started.load(std::memory_order_acquire); },
               std::chrono::seconds(2), "worker did not start gate");

    std::mutex order_mutex;
    std::vector<int> order;
    std::vector<std::shared_ptr<AsyncTask>> tasks;
    std::vector<int> ids;
    for (int i = 0; i < 8; ++i) {
        const int id = runtime.submit([&, i](const AsyncCancellationToken& token) {
            token.throw_if_cancelled();
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(i);
            return std::to_string(i);
        }, 0, "test", 1);
        ids.push_back(id);
        tasks.push_back(runtime.find(id));
    }

    bool rejected = false;
    try {
        runtime.submit([](const AsyncCancellationToken&) { return std::string(); }, 0, "test", 1);
    } catch (const JitThrow& error) {
        rejected = error.message.find("capacity exhausted") != std::string::npos;
    }
    require(rejected, "bounded queue did not reject excess work");

    size_t workers = 0, capacity = 0, queued = 0, running = 0, tracked = 0, scopes = 0;
    runtime.limits(workers, capacity, queued, running, tracked, scopes);
    require(queued == 8 && running == 1 && tracked == 9, "initial queue metrics are inconsistent");

    require(runtime.cancel(ids[3]), "queued cancellation was not accepted");
    runtime.limits(workers, capacity, queued, running, tracked, scopes);
    require(queued == 7, "cancelled queued task was not removed immediately");
    require(runtime.wait(tasks[3], 1000), "cancelled queued task did not become ready");
    require(runtime.erase_terminal(ids[3], tasks[3]), "cancelled handle was not erasable");

    const int replacement_id = runtime.submit([&](const AsyncCancellationToken& token) {
        token.throw_if_cancelled();
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(8);
        return std::string("8");
    }, 0, "test", 1);
    auto replacement = runtime.find(replacement_id);

    release_gate.store(true, std::memory_order_release);
    auto gate_task = runtime.find(gate);
    require(runtime.wait(gate_task, 2000), "gate did not complete");
    require(runtime.erase_terminal(gate, gate_task), "gate result was not erasable");
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (i == 3) continue;
        require(runtime.wait(tasks[i], 2000), "FIFO task did not complete");
        require(runtime.erase_terminal(ids[i], tasks[i]), "FIFO task was not erasable");
    }
    require(runtime.wait(replacement, 2000), "replacement task did not complete");
    require(runtime.erase_terminal(replacement_id, replacement), "replacement was not erasable");

    const std::vector<int> expected{0, 1, 2, 4, 5, 6, 7, 8};
    require(order == expected, "single-worker queue did not preserve FIFO order");
}

void test_completion_event() {
    AsyncRuntime runtime;
    require(runtime.configure(1, 4), "completion runtime configure failed");
    const uint64_t epoch = runtime.completion_epoch();
    const int id = runtime.submit([](const AsyncCancellationToken& token) {
        if (token.wait_for(std::chrono::milliseconds(25))) throw AsyncCancelled{};
        return std::string("done");
    }, 0, "test", 1);
    require(runtime.wait_for_completion(epoch, 2000), "completion event timed out");
    auto task = runtime.find(id);
    require(runtime.wait(task, 2000), "completion event preceded terminal publication");
    require(runtime.erase_terminal(id, task), "completion task was not erasable");
}

void test_scope_cancel_closes_admission() {
    AsyncRuntime runtime;
    require(runtime.configure(1, 8), "scope-cancel runtime configure failed");
    const int scope = runtime.open_scope();
    runtime.submit([](const AsyncCancellationToken& token) {
        if (token.wait_for(std::chrono::seconds(30))) throw AsyncCancelled{};
        return std::string();
    }, scope, "test", 1);
    require(runtime.cancel_scope(scope, "test", 1) == 1,
            "scope cancellation did not request its child");
    bool rejected = false;
    try {
        runtime.submit([](const AsyncCancellationToken&) { return std::string(); },
                       scope, "test", 1);
    } catch (const JitThrow& error) {
        rejected = error.message.find("scope is closed") != std::string::npos;
    }
    require(rejected, "cancelled scope admitted a late child");
    AsyncScopeCloseSummary summary;
    bool known = false;
    require(runtime.close_scope(scope, true, 2000, summary, known),
            "cancelled scope did not close");
    require(known && summary.total == 1 && summary.cancelled == 1,
            "cancelled scope summary is inconsistent");
}

void test_large_queue_cancel_is_linear() {
    constexpr int queued_count = 50000;
    AsyncRuntime runtime;
    require(runtime.configure(1, queued_count), "large-queue runtime configure failed");
    std::atomic<bool> gate_started{false};
    const int gate_id = runtime.submit([&](const AsyncCancellationToken& token) {
        gate_started.store(true, std::memory_order_release);
        if (token.wait_for(std::chrono::seconds(30))) throw AsyncCancelled{};
        return std::string();
    }, 0, "test", 1);
    wait_until([&] { return gate_started.load(std::memory_order_acquire); },
               std::chrono::seconds(2), "large-queue gate did not start");

    const int scope = runtime.open_scope();
    for (int i = 0; i < queued_count; ++i) {
        runtime.submit([](const AsyncCancellationToken&) { return std::string(); },
                       scope, "test", 1);
    }
    const auto cancel_started = std::chrono::steady_clock::now();
    AsyncScopeCloseSummary summary;
    bool known = false;
    require(runtime.close_scope(scope, true, 10000, summary, known),
            "large queued scope cancellation timed out");
    const auto cancel_elapsed = std::chrono::steady_clock::now() - cancel_started;
    require(known && summary.total == queued_count && summary.cancelled == queued_count,
            "large queued scope cancellation lost tasks");
    require(cancel_elapsed < std::chrono::seconds(10),
            "large queued scope cancellation is not linear-time in practice");
    std::cout << "  50000 queued tasks cancelled/closed in "
              << std::chrono::duration_cast<std::chrono::milliseconds>(cancel_elapsed).count()
              << " ms\n";

    require(runtime.cancel(gate_id), "large-queue gate cancellation failed");
    auto gate = runtime.find(gate_id);
    require(runtime.wait(gate, 2000), "large-queue gate did not cancel");
    require(runtime.erase_terminal(gate_id, gate), "large-queue gate was not erasable");
}

void test_scope_cancel_close_races() {
    AsyncRuntime runtime;
    require(runtime.configure(4, 128), "stress runtime configure failed");

    for (int round = 0; round < 80; ++round) {
        const int scope = runtime.open_scope();
        std::vector<int> ids;
        ids.reserve(96);
        for (int i = 0; i < 96; ++i) {
            ids.push_back(runtime.submit([](const AsyncCancellationToken& token) {
                for (int step = 0; step < 20; ++step) {
                    if (token.wait_for(std::chrono::microseconds(100))) throw AsyncCancelled{};
                }
                return std::string("ok");
            }, scope, "test", 1));
        }

        std::atomic<bool> start{false};
        std::vector<std::thread> cancellers;
        for (int lane = 0; lane < 6; ++lane) {
            cancellers.emplace_back([&, lane] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (size_t i = (size_t)lane; i < ids.size(); i += 6) {
                    runtime.cancel(ids[i]);
                }
            });
        }
        std::thread observer([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int sample = 0; sample < 200; ++sample) {
                bool open = false;
                std::vector<std::shared_ptr<AsyncTask>> snapshot;
                if (!runtime.scope_snapshot(scope, open, snapshot)) return;
                for (const auto& task : snapshot) {
                    (void)task->scope_id.load(std::memory_order_acquire);
                    std::lock_guard<std::mutex> lock(task->mutex);
                    (void)task->state;
                }
            }
        });

        start.store(true, std::memory_order_release);
        AsyncScopeCloseSummary summary;
        bool known = false;
        require(runtime.close_scope(scope, true, 5000, summary, known), "scope close timed out");
        require(known, "scope disappeared before close linearized");
        for (auto& thread : cancellers) thread.join();
        observer.join();
        require(summary.total == 96, "scope lost a child during cancellation race");
        require(summary.succeeded + summary.failed + summary.cancelled == summary.total,
                "scope terminal accounting is inconsistent");

        size_t workers = 0, capacity = 0, queued = 0, running = 0, tracked = 0, scopes = 0;
        runtime.limits(workers, capacity, queued, running, tracked, scopes);
        require(queued == 0 && running == 0 && tracked == 0 && scopes == 0,
                "scope close leaked runtime state");
    }
}

void test_destructor_cancels_and_joins() {
    const auto started = std::chrono::steady_clock::now();
    {
        AsyncRuntime runtime;
        require(runtime.configure(4, 64), "shutdown runtime configure failed");
        for (int i = 0; i < 4; ++i) {
            runtime.submit([](const AsyncCancellationToken& token) {
                if (token.wait_for(std::chrono::seconds(30))) throw AsyncCancelled{};
                return std::string();
            }, 0, "test", 1);
        }
        wait_until([&] {
            size_t workers = 0, capacity = 0, queued = 0, running = 0, tracked = 0, scopes = 0;
            runtime.limits(workers, capacity, queued, running, tracked, scopes);
            return running == 4;
        }, std::chrono::seconds(2), "shutdown workers did not start");
        for (int i = 4; i < 68; ++i) {
            runtime.submit([](const AsyncCancellationToken& token) {
                if (token.wait_for(std::chrono::seconds(30))) throw AsyncCancelled{};
                return std::string();
            }, 0, "test", 1);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(elapsed < std::chrono::seconds(3), "runtime shutdown did not cancel/join promptly");
}

void test_unique_request_body_files() {
    constexpr int count = 64;
    std::vector<std::filesystem::path> paths(count);
    std::vector<std::thread> threads;
    std::exception_ptr failure;
    std::mutex failure_mutex;
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&, i] {
            try {
                paths[i] = async_write_request_body_temp("body-" + std::to_string(i), 1);
            } catch (...) {
                std::lock_guard<std::mutex> lock(failure_mutex);
                if (!failure) failure = std::current_exception();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    if (failure) std::rethrow_exception(failure);

    std::set<std::filesystem::path> unique(paths.begin(), paths.end());
    require(unique.size() == count, "request body temp paths collided");
    for (int i = 0; i < count; ++i) {
        std::ifstream input(paths[i], std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        require(body == "body-" + std::to_string(i), "request body temp file was overwritten");
        std::error_code ignored;
        std::filesystem::remove(paths[i], ignored);
    }
}

void test_async_result_bounds() {
    {
        AsyncRuntime runtime(1024);
        require(runtime.configure(1, 4), "retained-budget runtime configure failed");
        const int first_id = runtime.submit([](const AsyncCancellationToken&) {
            return std::string(800, 'a');
        }, 0, "test", 1);
        auto first = runtime.find(first_id);
        require(runtime.wait(first, 2000), "first retained-budget task did not complete");

        const int second_id = runtime.submit([](const AsyncCancellationToken&) {
            return std::string(800, 'b');
        }, 0, "test", 1);
        auto second = runtime.find(second_id);
        require(runtime.wait(second, 2000), "second retained-budget task did not complete");
        {
            std::lock_guard<std::mutex> lock(second->mutex);
            require(second->state == AsyncTaskState::Failed &&
                    second->error.find("retained output capacity exhausted") != std::string::npos,
                    "aggregate retained-output budget was not enforced");
        }
        size_t workers = 0, capacity = 0, queued = 0, running = 0, tracked = 0, scopes = 0;
        size_t max_retained = 0, retained = 0;
        runtime.limits(workers, capacity, queued, running, tracked, scopes,
                       &max_retained, &retained);
        require(max_retained == 1024 && retained == 800,
                "retained-output accounting is inconsistent");
        require(runtime.erase_terminal(first_id, first), "first retained task was not erasable");
        require(runtime.erase_terminal(second_id, second), "failed retained task was not erasable");
        runtime.limits(workers, capacity, queued, running, tracked, scopes,
                       &max_retained, &retained);
        require(retained == 0, "retained-output budget was not released on consume");

        const int error_id = runtime.submit([](const AsyncCancellationToken&) -> std::string {
            throw JitThrow{std::string(100000, 'e'), 1};
        }, 0, "test", 1);
        auto error_task = runtime.find(error_id);
        require(runtime.wait(error_task, 2000), "large-error task did not complete");
        {
            std::lock_guard<std::mutex> lock(error_task->mutex);
            require(error_task->state == AsyncTaskState::Failed &&
                    error_task->error.size() == ASYNC_MAX_ERROR_BYTES,
                    "async task error retention was not bounded");
        }
        require(runtime.erase_terminal(error_id, error_task), "large-error task was not erasable");
    }

    std::string output;
    const std::string chunk(1024 * 1024, 'x');
    for (int i = 0; i < 64; ++i) {
        require(async_append_bounded(output, chunk.data(), chunk.size()),
                "bounded output rejected data below its limit");
    }
    require(output.size() == ASYNC_MAX_CAPTURE_BYTES, "bounded output size is incorrect");
    require(!async_append_bounded(output, "x", 1), "bounded output accepted data above its limit");
    require(output.size() == ASYNC_MAX_CAPTURE_BYTES, "bounded output grew above its limit");

    const std::filesystem::path oversized = std::filesystem::temp_directory_path() /
        ("sura_async_oversized_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream file(oversized, std::ios::binary);
        file.seekp((std::streamoff)ASYNC_MAX_CAPTURE_BYTES);
        file.put('x');
    }
    bool rejected = false;
    try {
        auto cancellation = std::make_shared<AsyncCancellationState>();
        (void)async_read_text_file_cancellable(
            oversized.string(), 1, "test", AsyncCancellationToken(cancellation));
    } catch (const JitThrow& error) {
        rejected = error.message.find("64 MiB") != std::string::npos;
    }
    std::error_code ignored;
    std::filesystem::remove(oversized, ignored);
    require(rejected, "oversized async file result was not rejected before allocation");

#ifdef _WIN32
    const std::string flood =
        "for /L %i in (1,1,100000) do @echo 0123456789abcdef0123456789abcdef";
#else
    const std::string flood = "yes 0123456789abcdef0123456789abcdef";
#endif
    rejected = false;
    const auto flood_started = std::chrono::steady_clock::now();
    try {
        auto cancellation = std::make_shared<AsyncCancellationState>();
        (void)run_capture_command_cancellable(
            flood, AsyncCancellationToken(cancellation), 4096);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("4096 byte limit") != std::string::npos;
    }
    require(rejected, "subprocess output flood did not trip the capture limit");
    require(std::chrono::steady_clock::now() - flood_started < std::chrono::seconds(5),
            "subprocess output flood was not killed promptly");

    // A continuously readable pipe must not starve cancellation polling. Keep
    // the producer below the capture limit long enough for cancellation to be
    // the reason the process tree stops.
#ifdef _WIN32
    const std::string endless_stream =
        "powershell.exe -NoProfile -NonInteractive -Command \""
        "while ($true) { [Console]::WriteLine('stream'); "
        "Start-Sleep -Milliseconds 1 }\"";
#else
    const std::string endless_stream =
        "while true; do echo stream; sleep 0.001; done";
#endif
    auto stream_cancellation = std::make_shared<AsyncCancellationState>();
    std::atomic<bool> stream_cancelled{false};
    std::exception_ptr stream_failure;
    std::thread stream_runner([&] {
        try {
            (void)run_capture_command_cancellable(
                endless_stream, AsyncCancellationToken(stream_cancellation));
        } catch (const AsyncCancelled&) {
            stream_cancelled.store(true, std::memory_order_release);
        } catch (...) {
            stream_failure = std::current_exception();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto stream_cancel_started = std::chrono::steady_clock::now();
    async_request_stop(stream_cancellation);
    stream_runner.join();
    if (stream_failure) std::rethrow_exception(stream_failure);
    require(stream_cancelled.load(std::memory_order_acquire),
            "continuous-output command did not report cancellation");
    require(std::chrono::steady_clock::now() - stream_cancel_started <
                std::chrono::seconds(2),
            "continuous-output command starved cancellation polling");
}

void test_process_tree_cancellation() {
    const std::filesystem::path sentinel = std::filesystem::temp_directory_path() /
        ("sura_async_cancel_sentinel_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ignored;
    std::filesystem::remove(sentinel, ignored);

#ifdef _WIN32
    std::string path = sentinel.string();
    std::string quoted_path;
    for (char ch : path) quoted_path += ch == '\'' ? "''" : std::string(1, ch);
    const std::string command =
        "powershell.exe -NoProfile -NonInteractive -Command \"Start-Sleep -Seconds 2; "
        "[IO.File]::WriteAllText('" + quoted_path + "','leaked')\"";
#else
    const std::string command = "(sleep 2; echo leaked > '" + sentinel.string() + "') & wait";
#endif

    auto cancellation = std::make_shared<AsyncCancellationState>();
    std::atomic<bool> observed_cancel{false};
    std::exception_ptr failure;
    std::thread runner([&] {
        try {
            (void)run_capture_command_cancellable(command, AsyncCancellationToken(cancellation));
        } catch (const AsyncCancelled&) {
            observed_cancel.store(true, std::memory_order_release);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto cancel_started = std::chrono::steady_clock::now();
    async_request_stop(cancellation);
    runner.join();
    if (failure) std::rethrow_exception(failure);
    require(observed_cancel.load(std::memory_order_acquire), "process runner did not report cancellation");
    require(std::chrono::steady_clock::now() - cancel_started < std::chrono::seconds(2),
            "process cancellation did not return promptly");
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    require(!std::filesystem::exists(sentinel), "cancelled process tree left a live descendant");
    std::filesystem::remove(sentinel, ignored);
}

void test_async_sura_isolation_primitives() {
    Value input = Value::make_dict();
    input.as_dict()->elements["ok"] = Value(true);
    Value values = Value::make_array();
    values.as_arr()->elements.push_back(Value(1.0));
    values.as_arr()->elements.push_back(Value("two"));
    input.as_dict()->elements["values"] = values;
    const std::string encoded = AsyncJsonSnapshotEncoder(1).encode(input);
    require(encoded.find("\"ok\":true") != std::string::npos,
            "async Sura JSON snapshot lost bool input");
    require(encoded.find("\"values\":[1,\"two\"]") != std::string::npos,
            "async Sura JSON snapshot lost array input");

    bool closure_rejected = false;
    try {
        (void)AsyncJsonSnapshotEncoder(1).encode(Value::make_closure("capture"));
    } catch (const JitThrow& error) {
        closure_rejected = error.message.find("supports only nil, bool, finite number") != std::string::npos;
    }
    require(closure_rejected, "async Sura snapshot accepted a closure capture");

    Value cycle = Value::make_array();
    cycle.as_arr()->elements.push_back(cycle);
    bool cycle_rejected = false;
    try {
        (void)AsyncJsonSnapshotEncoder(1).encode(cycle);
    } catch (const JitThrow& error) {
        cycle_rejected = error.message.find("cyclic array or dict") != std::string::npos;
    }
    require(cycle_rejected, "async Sura snapshot accepted a cyclic graph");

    auto invocation = std::make_shared<AsyncSuraInvocation>();
    invocation->program_snapshot = async_write_snapshot_temp(
        std::string("program\0bytes", 13), ".sura.bc", 1);
    invocation->input_snapshot = async_write_snapshot_temp(encoded, ".json", 1);
    require(std::filesystem::exists(invocation->program_snapshot) &&
            std::filesystem::exists(invocation->input_snapshot),
            "async Sura snapshot files were not created");
    const auto program_path = invocation->program_snapshot;
    const auto input_path = invocation->input_snapshot;
    invocation->cleanup();
    require(!std::filesystem::exists(program_path) && !std::filesystem::exists(input_path),
            "async Sura snapshot cleanup left temporary files");

#ifdef _WIN32
    const std::string command = "powershell.exe -NoProfile -NonInteractive -Command \"Start-Sleep -Seconds 5\"";
#else
    const std::string command = "sleep 5";
#endif
    bool timed_out = false;
    const auto timeout_started = std::chrono::steady_clock::now();
    try {
        auto cancellation = std::make_shared<AsyncCancellationState>();
        (void)run_capture_command_cancellable_status(
            command, AsyncCancellationToken(cancellation), 4096, 50, true);
    } catch (const std::runtime_error& error) {
        timed_out = std::string(error.what()).find("timed out") != std::string::npos;
    }
    require(timed_out, "async Sura subprocess timeout was not reported");
    require(std::chrono::steady_clock::now() - timeout_started < std::chrono::seconds(2),
            "async Sura subprocess timeout did not terminate promptly");
    GC::shutdown();
}

} // namespace

int main() {
    try {
        std::cout << "[1/10] fifo/backpressure/cancel\n";
        test_fifo_backpressure_and_cancel_removal();
        std::cout << "[2/10] completion event\n";
        test_completion_event();
        std::cout << "[3/10] scope cancel admission\n";
        test_scope_cancel_closes_admission();
        std::cout << "[4/10] large-queue cancellation\n";
        test_large_queue_cancel_is_linear();
        std::cout << "[5/10] scope races\n";
        test_scope_cancel_close_races();
        std::cout << "[6/10] shutdown\n";
        test_destructor_cancels_and_joins();
        std::cout << "[7/10] request body files\n";
        test_unique_request_body_files();
        std::cout << "[8/10] async result bounds\n";
        test_async_result_bounds();
        std::cout << "[9/10] process tree cancellation\n";
        test_process_tree_cancellation();
        std::cout << "[10/10] isolated Sura process primitives\n";
        test_async_sura_isolation_primitives();
        std::cout << "async runtime concurrency: PASS\n";
        return 0;
    } catch (const JitThrow& error) {
        std::cerr << "async runtime concurrency: FAIL: " << error.message
                  << " (line " << error.line << ")\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "async runtime concurrency: FAIL: " << error.what() << "\n";
        return 1;
    }
}
