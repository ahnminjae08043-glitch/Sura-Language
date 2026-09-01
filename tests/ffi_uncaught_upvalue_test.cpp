#include "../sura_ffi.hpp"

#include <cmath>
#include <iostream>

int main() {
    SuraHandle handle = sura_new();
    if (!handle) return 1;

    if (sura_run(handle, "box is {}\n") != SURA_OK) return 2;

    const int failed = sura_run(handle,
        "func install_then_throw() do\n"
        "  captured is 41\n"
        "  local_saved is func() do\n"
        "    return captured\n"
        "  end\n"
        "  box[\"saved\"] is local_saved\n"
        "  throw \"stop\"\n"
        "end\n"
        "install_then_throw()\n");
    if (failed == SURA_OK) return 3;
    std::cerr << "expected failure: " << sura_last_error(handle) << "\n";

    // The failed run inserted a closure into a dictionary that was already
    // persistent. That graph must be removed before its JitChunk is destroyed.
    if (sura_has(handle, "box")) {
        std::cerr << "run-bound closure graph survived the failed run\n";
        return 4;
    }

    if (sura_run(handle,
        "func clobber_discarded_frame() do\n"
        "  captured is 999\n"
        "  another is 123\n"
        "  return captured + another\n"
        "end\n"
        "result is clobber_discarded_frame()\n"
        "safe_box is {\"value\": result}\n"
        "run_local is func() do\n"
        "  return 41\n"
        "end\n"
        "unsafe_box is {\"saved\": run_local}\n"
        "cycle is {}\n"
        "cycle[\"self\"] is cycle\n"
        "class RunLocalBox do\n"
        "  value is 7\n"
        "end\n"
        "run_local_instance is new RunLocalBox()\n") != SURA_OK) {
        std::cerr << sura_last_error(handle) << "\n";
        return 5;
    }

    const double result = sura_get_number(handle, "result");
    const bool safe_result = std::fabs(result - 1122.0) <= 0.0001;
    const bool safe_box_persisted = sura_has(handle, "safe_box") != 0;
    const bool function_omitted = sura_has(handle, "clobber_discarded_frame") == 0 &&
                                  sura_has(handle, "run_local") == 0;
    const bool unsafe_box_omitted = sura_has(handle, "unsafe_box") == 0;
    const bool cycle_persisted = sura_has(handle, "cycle") != 0;
    const bool instance_omitted = sura_has(handle, "run_local_instance") == 0;
    bool tensor_persisted = false;
    if (sura_run(handle,
        "use autograd\n"
        "saved_tensor is autograd.tensor([3, 4])\n") == SURA_OK &&
        sura_has(handle, "saved_tensor")) {
        tensor_persisted = sura_run(handle,
            "use autograd\n"
            "tensor_total is autograd.data(saved_tensor)[0] + autograd.data(saved_tensor)[1]\n") == SURA_OK &&
            std::fabs(sura_get_number(handle, "tensor_total") - 7.0) <= 0.0001;
    }
    sura_free(handle);
    if (!safe_result || !safe_box_persisted || !function_omitted || !unsafe_box_omitted ||
        !cycle_persisted || !instance_omitted || !tensor_persisted) {
        std::cerr << "FFI persistence quarantine mismatch: result=" << result
                  << " safe_box=" << safe_box_persisted
                  << " functions_omitted=" << function_omitted
                  << " unsafe_box_omitted=" << unsafe_box_omitted
                  << " cycle=" << cycle_persisted
                  << " instance_omitted=" << instance_omitted
                  << " tensor=" << tensor_persisted << "\n";
        return 6;
    }
    std::cout << "ffi_uncaught_upvalue_test: PASS\n";
    return 0;
}
