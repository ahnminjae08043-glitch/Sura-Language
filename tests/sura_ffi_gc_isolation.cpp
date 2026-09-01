#include "../sura_ffi.hpp"

#include <cstring>
#include <iostream>
#include <string>

// Regression for process-global GC heaps: values retained by one embedding
// context must remain rooted while a different context allocates and collects.
int main() {
    SuraHandle first = sura_new();
    SuraHandle second = sura_new();
    if (!first || !second) {
        std::cerr << "failed to create Sura contexts\n";
        return 1;
    }

    const std::string sentinel =
        "first-context-persistent-string-0123456789-abcdefghijklmnopqrstuvwxyz";
    sura_set_string(first, "saved", sentinel.c_str());

    const char* pressure = R"SURA(
i is 0
while i < 20000 do
  transient is "gc-pressure-{i}-abcdefghijklmnopqrstuvwxyz"
  values is [transient, i, {index: i, text: transient}]
  i += 1
end
)SURA";

    const int run_result = sura_run(second, pressure);
    if (run_result != SURA_OK) {
        std::cerr << "pressure run failed: " << sura_last_error(second) << "\n";
        sura_free(second);
        sura_free(first);
        return 2;
    }

    const char* observed = sura_get_string(first, "saved");
    const bool intact = observed && sentinel == observed;
    if (!intact) {
        std::cerr << "cross-context GC corrupted a persistent value\n";
    }

    sura_free(second);
    sura_free(first);
    if (!intact) return 3;

    std::cout << "sura_ffi_gc_isolation: PASS\n";
    return 0;
}
