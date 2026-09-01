#pragma once
#include "value.hpp"
#include <string>
#include <vector>

// ================================================================
//  Runtime exception carriers
//  JitThrow — a raised error, with:
//    - message    : stringified summary (fallback for uncaught)
//    - line       : source line at throw site
//    - thrown_value : original Value (preserves dict/instance structure
//                     for custom error types like {type: "ValueError", ...})
//    - stack_trace  : {function_name, line} pairs, populated by VM on
//                     uncaught exceptions before the throw escapes run()
//  JitReturn — used for the initial frame's early-return path
// ================================================================

struct StackFrameInfo {
    std::string func_name;
    int line = 0;
};

struct JitThrow {
    std::string message;
    int line = 0;
    Value thrown_value = Value::nil();
    std::vector<StackFrameInfo> stack_trace;
};

struct JitReturn { Value value; };
