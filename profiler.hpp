#pragma once
#include "jit_op.hpp"
#include <unordered_map>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>

// opname helper — minimal, avoids depending on JitVM
static inline std::string prof_opname(JitOp op) {
    switch (op) {
        case JitOp::ADD: return "ADD"; case JitOp::SUB: return "SUB";
        case JitOp::MUL: return "MUL"; case JitOp::DIV: return "DIV";
        case JitOp::MOD: return "MOD"; case JitOp::NEG: return "NEG";
        default: return "OP_" + std::to_string((int)op);
    }
}

// ================================================================
//  Sura Runtime Profiler — hot-path & type feedback collector
//  Usage:
//    Profiler prof;
//    vm.set_profiler(&prof);
//    vm.run(chunk);
//    prof.print_report(chunk);
// ================================================================

struct ArithSite {
    uint64_t num_num = 0;   // both operands were numbers (fast path)
    uint64_t other   = 0;   // at least one non-number (slow / polymorphic)

    uint64_t total()    const { return num_num + other; }
    double   mono_pct() const { return total() ? num_num * 100.0 / total() : 0.0; }
};

struct CallSite {
    uint64_t call_count  = 0;
    bool     is_mono     = true;           // still monomorphic?
    std::string mono_callee;               // set on first call, cleared on mismatch
    std::unordered_map<std::string, uint64_t> callee_hits;
};

struct BranchSite {
    uint64_t taken     = 0;   // condition was true (branch jumped)
    uint64_t not_taken = 0;

    uint64_t total()     const { return taken + not_taken; }
    double   bias_pct()  const {
        if (!total()) return 0.0;
        auto mx = std::max(taken, not_taken);
        return mx * 100.0 / total();
    }
};

class Profiler {
public:
    bool enabled = true;

    std::unordered_map<size_t, ArithSite>  arith;
    std::unordered_map<size_t, CallSite>   calls;
    std::unordered_map<size_t, BranchSite> branches;

    // ── Called from VM ──────────────────────────────────────────

    inline void record_arith(size_t ip, bool both_num) {
        auto& s = arith[ip];
        if (both_num) ++s.num_num; else ++s.other;
    }

    inline void record_call(size_t ip, const std::string& callee) {
        auto& s = calls[ip];
        ++s.call_count;
        ++s.callee_hits[callee];
        if (s.mono_callee.empty()) {
            s.mono_callee = callee;
        } else if (s.mono_callee != callee) {
            s.is_mono = false;
        }
    }

    inline void record_branch(size_t ip, bool taken) {
        auto& s = branches[ip];
        if (taken) ++s.taken; else ++s.not_taken;
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

    void summary_counts(size_t& mono_calls, size_t& poly_calls, size_t& pure_arith) const {
        mono_calls = 0;
        poly_calls = 0;
        pure_arith = 0;
        for (const auto& entry : calls) {
            if (entry.second.is_mono) ++mono_calls;
            else ++poly_calls;
        }
        for (const auto& entry : arith) {
            if (entry.second.other == 0) ++pure_arith;
        }
    }

    void print_text_report(const JitChunk& chunk) const {
        size_t mono_calls = 0, poly_calls = 0, pure_arith = 0;
        summary_counts(mono_calls, poly_calls, pure_arith);

        std::cout << "\n=== Sura Runtime Profile Report ===\n";
        std::cout << "Summary\n"
                  << "  Call sites: " << calls.size() << "\n"
                  << "  Arithmetic sites: " << arith.size() << "\n"
                  << "  Branch sites: " << branches.size() << "\n"
                  << "  Monomorphic call sites: " << mono_calls << "\n"
                  << "  Polymorphic call sites: " << poly_calls << "\n"
                  << "  Pure-number arithmetic sites: " << pure_arith << "\n\n";

        if (!calls.empty()) {
            std::vector<std::pair<size_t, const CallSite*>> sorted_calls;
            for (const auto& entry : calls) sorted_calls.emplace_back(entry.first, &entry.second);
            std::sort(sorted_calls.begin(), sorted_calls.end(),
                [](const auto& x, const auto& y){ return x.second->call_count > y.second->call_count; });

            std::cout << "Hot Call Sites (top " << std::min((size_t)10, sorted_calls.size()) << ")\n";
            int shown = 0;
            for (const auto& entry : sorted_calls) {
                if (shown++ >= 10) break;
                const auto& ip = entry.first;
                const auto* s = entry.second;
                std::cout << "  ip=" << std::setw(5) << ip
                          << "  calls=" << std::setw(8) << s->call_count
                          << "  " << (s->is_mono ? "mono:" + s->mono_callee
                                                : "poly:" + std::to_string(s->callee_hits.size()) + " targets")
                          << "\n";
            }
            std::cout << "\n";
        }

        if (!arith.empty()) {
            std::vector<std::pair<size_t, const ArithSite*>> sorted_arith;
            for (const auto& entry : arith) sorted_arith.emplace_back(entry.first, &entry.second);
            std::sort(sorted_arith.begin(), sorted_arith.end(),
                [](const auto& x, const auto& y){ return x.second->total() > y.second->total(); });

            std::cout << "Arithmetic Type Feedback (top " << std::min((size_t)10, sorted_arith.size()) << ")\n";
            int shown = 0;
            for (const auto& entry : sorted_arith) {
                if (shown++ >= 10) break;
                const auto& ip = entry.first;
                const auto* s = entry.second;
                std::string op_name = (ip < chunk.code.size()) ? prof_opname(chunk.code[ip].op) : "?";
                std::string tag = s->other == 0 ? "num-only JIT candidate"
                    : s->mono_pct() > 90.0 ? "mostly-num " + std::to_string((int)s->mono_pct()) + "%"
                    : "mixed";
                std::cout << "  ip=" << std::setw(5) << ip
                          << "  op=" << std::setw(8) << op_name
                          << "  execs=" << std::setw(8) << s->total()
                          << "  " << tag << "\n";
            }
            std::cout << "\n";
        }

        if (!branches.empty()) {
            std::vector<std::pair<size_t, const BranchSite*>> sorted_branches;
            for (const auto& entry : branches) sorted_branches.emplace_back(entry.first, &entry.second);
            std::sort(sorted_branches.begin(), sorted_branches.end(),
                [](const auto& x, const auto& y){ return x.second->total() > y.second->total(); });

            std::cout << "Branch Feedback (top " << std::min((size_t)10, sorted_branches.size()) << ")\n";
            int shown = 0;
            for (const auto& entry : sorted_branches) {
                if (shown++ >= 10) break;
                const auto& ip = entry.first;
                const auto* s = entry.second;
                std::string dir = s->taken > s->not_taken ? "taken" : "not-taken";
                std::cout << "  ip=" << std::setw(5) << ip
                          << "  total=" << std::setw(8) << s->total()
                          << "  taken=" << std::setw(8) << s->taken
                          << "  not_taken=" << std::setw(8) << s->not_taken
                          << "  bias=" << std::fixed << std::setprecision(1) << s->bias_pct()
                          << "% " << dir << "\n";
            }
            std::cout << "\n";
        }
    }

    std::string to_json(const JitChunk& chunk) const {
        size_t mono_calls = 0, poly_calls = 0, pure_arith = 0;
        summary_counts(mono_calls, poly_calls, pure_arith);

        std::ostringstream out;
        out << "{\n"
            << "  \"summary\": {\n"
            << "    \"call_sites\": " << calls.size() << ",\n"
            << "    \"arithmetic_sites\": " << arith.size() << ",\n"
            << "    \"branch_sites\": " << branches.size() << ",\n"
            << "    \"monomorphic_call_sites\": " << mono_calls << ",\n"
            << "    \"polymorphic_call_sites\": " << poly_calls << ",\n"
            << "    \"pure_number_arithmetic_sites\": " << pure_arith << "\n"
            << "  },\n";

        out << "  \"calls\": [";
        bool first = true;
        for (const auto& entry : calls) {
            if (!first) out << ",";
            first = false;
            const auto& s = entry.second;
            out << "\n    {\"ip\":" << entry.first
                << ",\"call_count\":" << s.call_count
                << ",\"monomorphic\":" << (s.is_mono ? "true" : "false")
                << ",\"callee\":\"" << json_escape(s.mono_callee) << "\""
                << ",\"target_count\":" << s.callee_hits.size() << "}";
        }
        out << (first ? "" : "\n  ") << "],\n";

        out << "  \"arithmetic\": [";
        first = true;
        for (const auto& entry : arith) {
            if (!first) out << ",";
            first = false;
            const auto& s = entry.second;
            std::string op_name = (entry.first < chunk.code.size()) ? prof_opname(chunk.code[entry.first].op) : "?";
            out << "\n    {\"ip\":" << entry.first
                << ",\"op\":\"" << json_escape(op_name) << "\""
                << ",\"total\":" << s.total()
                << ",\"num_num\":" << s.num_num
                << ",\"other\":" << s.other
                << ",\"mono_percent\":" << std::fixed << std::setprecision(2) << s.mono_pct() << "}";
        }
        out << (first ? "" : "\n  ") << "],\n";

        out << "  \"branches\": [";
        first = true;
        for (const auto& entry : branches) {
            if (!first) out << ",";
            first = false;
            const auto& s = entry.second;
            out << "\n    {\"ip\":" << entry.first
                << ",\"total\":" << s.total()
                << ",\"taken\":" << s.taken
                << ",\"not_taken\":" << s.not_taken
                << ",\"bias_percent\":" << std::fixed << std::setprecision(2) << s.bias_pct() << "}";
        }
        out << (first ? "" : "\n  ") << "]\n";
        out << "}\n";
        return out.str();
    }

    bool write_json(const std::string& path, const JitChunk& chunk) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << to_json(chunk);
        return (bool)out;
    }

    // ── Report ──────────────────────────────────────────────────

    void print_report(const JitChunk& chunk) const {
        std::cout << "\n\033[1;36m╔══════════════════════════════════════════╗\033[0m\n";
        std::cout <<   "\033[1;36m║       Sura Runtime Profile Report        ║\033[0m\n";
        std::cout <<   "\033[1;36m╚══════════════════════════════════════════╝\033[0m\n\n";

        // ── Hot call sites ───────────────────────────────────────
        if (!calls.empty()) {
            std::vector<std::pair<size_t, const CallSite*>> sorted_calls;
            for (auto& [ip, s] : calls) sorted_calls.emplace_back(ip, &s);
            std::sort(sorted_calls.begin(), sorted_calls.end(),
                [](auto& x, auto& y){ return x.second->call_count > y.second->call_count; });

            std::cout << "\033[1;33m▸ Hot Call Sites (top " << std::min((size_t)10, sorted_calls.size()) << ")\033[0m\n";
            int shown = 0;
            for (auto& [ip, s] : sorted_calls) {
                if (shown++ >= 10) break;
                std::string mono_tag = s->is_mono
                    ? "\033[32m[mono: " + s->mono_callee + "]\033[0m"
                    : "\033[31m[poly: " + std::to_string(s->callee_hits.size()) + " targets]\033[0m";
                std::cout << "  ip=" << std::setw(5) << ip
                          << "  calls=" << std::setw(8) << s->call_count
                          << "  " << mono_tag << "\n";
            }
            std::cout << "\n";
        }

        // ── Arithmetic type feedback ─────────────────────────────
        if (!arith.empty()) {
            std::vector<std::pair<size_t, const ArithSite*>> sorted_arith;
            for (auto& [ip, s] : arith) sorted_arith.emplace_back(ip, &s);
            std::sort(sorted_arith.begin(), sorted_arith.end(),
                [](auto& x, auto& y){ return x.second->total() > y.second->total(); });

            std::cout << "\033[1;33m▸ Arithmetic Type Feedback (top " << std::min((size_t)10, sorted_arith.size()) << ")\033[0m\n";
            int shown = 0;
            for (auto& [ip, s] : sorted_arith) {
                if (shown++ >= 10) break;
                // Look up opcode name
                std::string op_name = (ip < chunk.code.size())
                    ? prof_opname(chunk.code[ip].op) : "?";
                std::string tag = s->other == 0
                    ? "\033[32m[num-only — JIT candidate]\033[0m"
                    : s->mono_pct() > 90.0
                        ? "\033[33m[mostly-num " + std::to_string((int)s->mono_pct()) + "%]\033[0m"
                        : "\033[31m[mixed]\033[0m";
                std::cout << "  ip=" << std::setw(5) << ip
                          << "  op=" << std::setw(8) << op_name
                          << "  execs=" << std::setw(8) << s->total()
                          << "  " << tag << "\n";
            }
            std::cout << "\n";
        }

        // ── Biased branches (loop / always-true guards) ──────────
        if (!branches.empty()) {
            std::vector<std::pair<size_t, const BranchSite*>> biased;
            for (auto& [ip, s] : branches)
                if (s.total() > 100 && s.bias_pct() > 90.0)
                    biased.emplace_back(ip, &s);
            std::sort(biased.begin(), biased.end(),
                [](auto& x, auto& y){ return x.second->total() > y.second->total(); });

            if (!biased.empty()) {
                std::cout << "\033[1;33m▸ Biased Branches (>90%% one direction, top " << std::min((size_t)10, biased.size()) << ")\033[0m\n";
                int shown = 0;
                for (auto& [ip, s] : biased) {
                    if (shown++ >= 10) break;
                    std::string dir = s->taken > s->not_taken ? "taken" : "not-taken";
                    std::cout << "  ip=" << std::setw(5) << ip
                              << "  total=" << std::setw(8) << s->total()
                              << "  bias=" << std::fixed << std::setprecision(1) << s->bias_pct() << "% " << dir << "\n";
                }
                std::cout << "\n";
            }
        }

        // ── JIT opportunity summary ──────────────────────────────
        size_t mono_calls = 0, poly_calls = 0, jit_arith = 0;
        for (auto& [_, s] : calls)  { if (s.is_mono) ++mono_calls; else ++poly_calls; }
        for (auto& [_, s] : arith)  { if (s.other == 0) ++jit_arith; }

        std::cout << "\033[1;36m▸ JIT Opportunity Summary\033[0m\n"
                  << "  Monomorphic call sites : " << mono_calls << "\n"
                  << "  Polymorphic call sites : " << poly_calls << "\n"
                  << "  Pure-number arith sites: " << jit_arith  << "\n\n";
    }
};
