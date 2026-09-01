#include "../value.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        GC::shutdown();

        Value survivor = Value::make_array();
        survivor.as_arr()->elements.push_back(Value(42));
        Value garbage = Value::make_dict();
        const size_t before_fault = GC::object_count();
        survivor.mark_value();

        GC::fail_next_sweep_reserve_for_test();
        bool saw_bad_alloc = false;
        try {
            GC::sweep();
        } catch (const std::bad_alloc&) {
            saw_bad_alloc = true;
        }
        require(saw_bad_alloc, "sweep reserve fault was not injected");
        require(GC::object_count() == before_fault,
                "failed sweep mutated the object registry");
        require(survivor.as_arr() && survivor.as_arr()->elements.size() == 1 &&
                    survivor.as_arr()->elements[0].is_num() &&
                    survivor.as_arr()->elements[0].as_num() == 42,
                "failed sweep damaged a live object");

        GC::sweep();
        require(GC::object_count() == 1,
                "recovery sweep did not retain exactly the marked object");
        survivor = Value::nil();
        garbage = Value::nil();
        GC::sweep();
        require(GC::object_count() == 0,
                "unrooted objects survived the recovery sweep");

        constexpr size_t depth = 100000;
        Value root = Value::make_array();
        Value cursor = root;
        for (size_t i = 0; i < depth; ++i) {
            Value next = Value::make_array();
            cursor.as_arr()->elements.push_back(next);
            cursor = next;
        }
        cursor.as_arr()->elements.push_back(root); // deep cycle

        root.mark_value();
        GC::sweep();
        require(GC::object_count() == depth + 1,
                "iterative marker lost a node in the deep cyclic graph");

        root = Value::nil();
        cursor = Value::nil();
        GC::sweep();
        require(GC::object_count() == 0,
                "deep cyclic graph was not reclaimed");

        GC::shutdown();
        GC::shutdown();
        require(GC::object_count() == 0,
                "GC shutdown is not idempotent");

        std::cout << "gc memory safety: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gc memory safety FAILED: " << error.what() << "\n";
        GC::shutdown();
        return 1;
    }
}
