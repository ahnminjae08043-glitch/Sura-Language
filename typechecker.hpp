#pragma once

#include "ast.hpp"
#include "value.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct TypeError {
    int         line;
    std::string message;
};

struct FuncSig {
    std::string              name;
    std::vector<TypeAnnot>   param_types;
    std::vector<std::string> param_names;
    TypeAnnot                return_type;
    size_t                   min_args = 0;
    size_t                   max_args = 0;
};

class TypeEnv {
    struct Binding {
        TypeAnnot type;
        bool      fixed = false;
    };

    std::vector<std::unordered_map<std::string, Binding>> scopes;

public:
    TypeEnv() { reset(); }

    void reset() {
        scopes.clear();
        scopes.push_back({});
    }

    void push() { scopes.push_back({}); }
    void pop()  { if (scopes.size() > 1) scopes.pop_back(); }

    void set(const std::string& name, const TypeAnnot& type, bool fixed = false) {
        scopes.back()[name] = Binding{type, fixed};
    }

    TypeAnnot get(const std::string& name) const {
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            auto it = scopes[static_cast<size_t>(i)].find(name);
            if (it != scopes[static_cast<size_t>(i)].end()) return it->second.type;
        }
        return TypeAnnot{};
    }

    bool has(const std::string& name) const {
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            if (scopes[static_cast<size_t>(i)].count(name) != 0) return true;
        }
        return false;
    }

    bool fixed_in_current_scope(const std::string& name) const {
        auto it = scopes.back().find(name);
        return it != scopes.back().end() && it->second.fixed;
    }
};

class FuncEnv {
    std::vector<std::unordered_map<std::string, FuncSig>> scopes;

public:
    FuncEnv() { reset(); }

    void reset() {
        scopes.clear();
        scopes.push_back({});
    }

    void push() { scopes.push_back({}); }
    void pop()  { if (scopes.size() > 1) scopes.pop_back(); }

    void set(const std::string& name, FuncSig sig) {
        scopes.back()[name] = std::move(sig);
    }

    const FuncSig* get(const std::string& name) const {
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            const auto& scope = scopes[static_cast<size_t>(i)];
            auto it = scope.find(name);
            if (it != scope.end()) return &it->second;
        }
        return nullptr;
    }
};

class TypeChecker {
    TypeEnv                                  env;
    FuncEnv                                  functions;
    std::unordered_map<std::string, std::string> class_parents;
    std::vector<TypeError>                   errors;
    TypeAnnot                                current_return;

    static TypeAnnot type(SType kind, std::string class_name = {}) {
        TypeAnnot result;
        result.kind = kind;
        result.class_name = std::move(class_name);
        result.present = true;
        return result;
    }

    static bool known(const TypeAnnot& value) {
        return value.present && value.kind != SType::ANY;
    }

    static std::string describe(const TypeAnnot& value) {
        if (!known(value)) return "any";
        if (value.kind == SType::CLASS && !value.class_name.empty()) {
            return value.class_name;
        }
        return stype_name(value.kind);
    }

    void error(int line, const char* code, const std::string& message) {
        errors.push_back({line, "[" + std::string(code) + "] " + message});
    }

    bool is_subclass_of(const std::string& actual, const std::string& expected) const {
        if (actual == expected) return true;
        std::string cursor = actual;
        std::unordered_set<std::string> visited;
        while (!cursor.empty() && visited.insert(cursor).second) {
            auto it = class_parents.find(cursor);
            if (it == class_parents.end()) return false;
            cursor = it->second;
            if (cursor == expected) return true;
        }
        return false;
    }

    bool compatible(const TypeAnnot& expected, const TypeAnnot& actual) const {
        // `any` is the gradual-typing escape hatch. Unknown expressions remain
        // valid and acquire checks only when both sides have useful types.
        if (expected.is_any() || actual.is_any()) return true;
        // `nil` is Sura's absence value and every declared type is currently
        // nullable. This matches ??, nil-safe iteration, and a bare return. A
        // future non-null annotation can deliberately tighten this rule.
        if (actual.kind == SType::NIL) return true;
        if (expected.kind != actual.kind) return false;
        if (expected.kind != SType::CLASS) return true;
        if (expected.class_name.empty() || actual.class_name.empty()) return true;
        return is_subclass_of(actual.class_name, expected.class_name);
    }

    void require_type(const TypeAnnot& actual, SType expected, int line,
                      const char* code, const std::string& context) {
        if (!known(actual) || actual.kind == expected) return;
        error(line, code, context + " must be " + stype_name(expected) +
                          ", got " + describe(actual) + ".");
    }

    FuncSig make_signature(const FuncDef* function) const {
        FuncSig signature;
        signature.name = function->name;
        signature.param_names = function->params;
        signature.param_types = function->param_types;
        signature.return_type = function->return_type;
        signature.max_args = function->params.size();

        while (signature.param_types.size() < signature.max_args) {
            signature.param_types.push_back(TypeAnnot{});
        }

        // Missing positional arguments are valid only when every omitted
        // parameter has a default. This also handles a non-trailing default
        // without pretending that a later required parameter is optional.
        for (size_t i = 0; i < signature.max_args; ++i) {
            const bool has_default = i < function->defaults.size() &&
                                     function->defaults[i] != nullptr;
            if (!has_default) signature.min_args = i + 1;
        }
        return signature;
    }

    void collect_functions(const SuraBlock* block) {
        if (!block) return;
        for (const auto& statement : block->body) {
            if (statement && statement->kind == NK::FUNC_DEF) {
                auto* function = static_cast<const FuncDef*>(statement.get());
                functions.set(function->name, make_signature(function));
            }
        }
    }

    void collect_classes(const SuraBlock* block) {
        if (!block) return;
        for (const auto& statement : block->body) {
            if (statement && statement->kind == NK::CLASS_DEF) {
                auto* class_def = static_cast<const ClassDef*>(statement.get());
                class_parents[class_def->name] = class_def->parent;
            }
        }
    }

    TypeAnnot check_call(const CallExpr* call) {
        std::vector<TypeAnnot> actual_types;
        actual_types.reserve(call->args.size());
        for (const auto& argument : call->args) {
            actual_types.push_back(infer(argument.get()));
        }

        const FuncSig* signature = functions.get(call->name);
        if (!signature) return TypeAnnot{};

        if (call->args.size() < signature->min_args ||
            call->args.size() > signature->max_args) {
            std::string expected;
            if (signature->min_args == signature->max_args) {
                expected = std::to_string(signature->min_args);
            } else {
                expected = std::to_string(signature->min_args) + ".." +
                           std::to_string(signature->max_args);
            }
            error(call->line, "E203",
                  "function '" + call->name + "' expects " + expected +
                  " argument(s), got " + std::to_string(call->args.size()) + ".");
        }

        const size_t count = std::min(actual_types.size(), signature->param_types.size());
        for (size_t i = 0; i < count; ++i) {
            const TypeAnnot& expected = signature->param_types[i];
            const TypeAnnot& actual = actual_types[i];
            if (!compatible(expected, actual)) {
                const std::string param = i < signature->param_names.size()
                    ? " ('" + signature->param_names[i] + "')" : std::string{};
                error(call->line, "E204",
                      "argument " + std::to_string(i + 1) + param + " of function '" +
                      call->name + "' expects " + describe(expected) + ", got " +
                      describe(actual) + ".");
            }
        }
        return signature->return_type;
    }

    TypeAnnot infer(const Expr* expression) {
        if (!expression) return TypeAnnot{};

        switch (expression->kind) {
            case NK::NUM_LIT:  return type(SType::NUMBER);
            case NK::STR_LIT:  return type(SType::STRING);
            case NK::BOOL_LIT: return type(SType::BOOL);
            case NK::NIL_LIT:  return type(SType::NIL);

            case NK::IDENT: {
                auto* identifier = static_cast<const Ident*>(expression);
                return env.get(identifier->name);
            }

            case NK::ARRAY_LIT: {
                auto* array = static_cast<const ArrayLit*>(expression);
                for (const auto& element : array->elements) infer(element.get());
                return type(SType::ARRAY);
            }

            case NK::DICT_LIT: {
                auto* dict = static_cast<const DictLit*>(expression);
                for (const auto& entry : dict->pairs) infer(entry.second.get());
                return type(SType::DICT);
            }

            case NK::STR_INTERP: {
                auto* interpolation = static_cast<const StrInterp*>(expression);
                for (const auto& part : interpolation->parts) infer(part.get());
                return type(SType::STRING);
            }

            case NK::BIN_OP: {
                auto* binary = static_cast<const BinOp*>(expression);
                TypeAnnot left = infer(binary->left.get());
                TypeAnnot right = infer(binary->right.get());
                const std::string& op = binary->op;

                if (op == "and" || op == "or") {
                    // Logical operators use Value::truthy(), just like branch
                    // conditions, and therefore accept every runtime value.
                    return type(SType::BOOL);
                }

                if (op == "==" || op == "!=" || op == "in") {
                    return type(SType::BOOL);
                }

                if (op == "<" || op == ">" || op == "<=" || op == ">=") {
                    require_type(left, SType::NUMBER, binary->line, "E201",
                                 "left operand of '" + op + "'");
                    require_type(right, SType::NUMBER, binary->line, "E201",
                                 "right operand of '" + op + "'");
                    return type(SType::BOOL);
                }

                if (op == "??") {
                    if (left.present && left.kind == SType::NIL) return right;
                    if (compatible(left, right) && compatible(right, left)) return left;
                    return TypeAnnot{};
                }

                if (op == "+") {
                    if (known(left) && known(right)) {
                        if (left.kind == SType::NUMBER && right.kind == SType::NUMBER)
                            return type(SType::NUMBER);
                        // Runtime concatenation stringifies the non-string side
                        // when either operand is a string.
                        if (left.kind == SType::STRING || right.kind == SType::STRING)
                            return type(SType::STRING);
                        if (left.kind == SType::ARRAY && right.kind == SType::ARRAY)
                            return type(SType::ARRAY);
                        error(binary->line, "E201",
                              "operator '+' requires two numbers, two arrays, or at least one string, got " +
                              describe(left) + " and " + describe(right) + ".");
                    }
                    return TypeAnnot{};
                }

                if (op == "-" || op == "*" || op == "/" || op == "%" ||
                    op == "|" || op == "^" || op == "&" || op == "<<" || op == ">>") {
                    require_type(left, SType::NUMBER, binary->line, "E201",
                                 "left operand of '" + op + "'");
                    require_type(right, SType::NUMBER, binary->line, "E201",
                                 "right operand of '" + op + "'");
                    return type(SType::NUMBER);
                }
                return TypeAnnot{};
            }

            case NK::UNARY_OP: {
                auto* unary = static_cast<const UnaryOp*>(expression);
                TypeAnnot operand = infer(unary->operand.get());
                if (unary->op == "not") {
                    // `not` negates runtime truthiness for every Value kind.
                    return type(SType::BOOL);
                }
                if (unary->op == "-" || unary->op == "~") {
                    require_type(operand, SType::NUMBER, unary->line, "E201",
                                 "operand of '" + unary->op + "'");
                    return type(SType::NUMBER);
                }
                return TypeAnnot{};
            }

            case NK::DOT_ACCESS: {
                auto* access = static_cast<const DotAccess*>(expression);
                infer(access->obj.get());
                return TypeAnnot{};
            }

            case NK::INDEX: {
                auto* index = static_cast<const IndexExpr*>(expression);
                TypeAnnot object = infer(index->obj.get());
                infer(index->key.get());
                if (known(object) && object.kind == SType::STRING) return type(SType::STRING);
                return TypeAnnot{};
            }

            case NK::CALL:
                return check_call(static_cast<const CallExpr*>(expression));

            case NK::METHOD_CALL: {
                auto* call = static_cast<const MethodCallExpr*>(expression);
                infer(call->obj.get());
                for (const auto& argument : call->args) infer(argument.get());
                return TypeAnnot{};
            }

            case NK::SUPER_CALL: {
                auto* call = static_cast<const SuperCallExpr*>(expression);
                for (const auto& argument : call->args) infer(argument.get());
                return TypeAnnot{};
            }

            case NK::NEW_EXPR: {
                auto* creation = static_cast<const NewExpr*>(expression);
                for (const auto& argument : creation->args) infer(argument.get());
                return type(SType::CLASS, creation->class_name);
            }

            case NK::TERNARY: {
                auto* ternary = static_cast<const TernaryExpr*>(expression);
                infer(ternary->cond.get());
                TypeAnnot then_type = infer(ternary->then_val.get());
                TypeAnnot else_type = infer(ternary->else_val.get());
                if (compatible(then_type, else_type) && compatible(else_type, then_type))
                    return then_type;
                return TypeAnnot{};
            }

            case NK::FUNC_EXPR: {
                auto* function = static_cast<const FuncExpr*>(expression);
                TypeAnnot previous_return = current_return;
                current_return = TypeAnnot{};
                env.push();
                for (const auto& param : function->params) env.set(param, TypeAnnot{});
                for (const auto& default_value : function->defaults) {
                    if (default_value) infer(default_value.get());
                }
                check_block(function->body.get());
                env.pop();
                current_return = previous_return;
                return TypeAnnot{};
            }

            default:
                return TypeAnnot{};
        }
    }

    void check_block(const SuraBlock* block) {
        if (!block) return;
        functions.push();
        collect_functions(block);
        for (const auto& statement : block->body) check_stmt(statement.get());
        functions.pop();
    }

    void check_stmt(const Stmt* statement) {
        if (!statement) return;

        switch (statement->kind) {
            case NK::SuraBlock:
                check_block(static_cast<const SuraBlock*>(statement));
                break;

            case NK::ASSIGN: {
                auto* assignment = static_cast<const AssignStmt*>(statement);
                TypeAnnot value_type = infer(assignment->value.get());
                TypeAnnot expected = assignment->type_annot;
                const bool existing_fixed = !expected.present &&
                                            env.fixed_in_current_scope(assignment->name);
                if (existing_fixed) expected = env.get(assignment->name);

                if (expected.present && !compatible(expected, value_type)) {
                    error(assignment->line, "E200",
                          "assignment to '" + assignment->name + "' expects " +
                          describe(expected) + ", got " + describe(value_type) + ".");
                }
                const bool annotation_fixed = assignment->type_annot.present &&
                                              !assignment->type_annot.is_any();
                env.set(assignment->name, expected.present ? expected : value_type,
                        annotation_fixed || existing_fixed);
                break;
            }

            case NK::IN_PLACE: {
                auto* update = static_cast<const InPlaceStmt*>(statement);
                TypeAnnot variable_type = env.get(update->name);
                TypeAnnot value_type = infer(update->value.get());
                if (known(variable_type)) {
                    if (update->op == "+" && variable_type.kind == SType::STRING) {
                        // String += follows the same runtime stringification
                        // rule as binary string concatenation.
                    } else {
                        require_type(variable_type, SType::NUMBER, update->line, "E209",
                                     "left operand of '" + update->op + "='");
                        require_type(value_type, SType::NUMBER, update->line, "E209",
                                     "right operand of '" + update->op + "='");
                    }
                }
                break;
            }

            case NK::DOT_ASSIGN: {
                auto* assignment = static_cast<const DotAssignStmt*>(statement);
                infer(assignment->value.get());
                break;
            }

            case NK::INDEX_ASSIGN: {
                auto* assignment = static_cast<const IndexAssignStmt*>(statement);
                infer(assignment->key.get());
                infer(assignment->value.get());
                break;
            }

            case NK::IF: {
                auto* conditional = static_cast<const IfStmt*>(statement);
                infer(conditional->cond.get());
                check_block(conditional->then_block.get());
                if (conditional->else_block) check_block(conditional->else_block.get());
                break;
            }

            case NK::WHILE: {
                auto* loop = static_cast<const WhileStmt*>(statement);
                infer(loop->cond.get());
                check_block(loop->body.get());
                break;
            }

            case NK::REPEAT: {
                auto* loop = static_cast<const RepeatStmt*>(statement);
                TypeAnnot count = infer(loop->count.get());
                require_type(count, SType::NUMBER, loop->line, "E207",
                             "repeat count");
                check_block(loop->body.get());
                break;
            }

            case NK::FOR: {
                auto* loop = static_cast<const ForStmt*>(statement);
                require_type(infer(loop->from.get()), SType::NUMBER, loop->line, "E207",
                             "for range start");
                require_type(infer(loop->to.get()), SType::NUMBER, loop->line, "E207",
                             "for range end");
                if (loop->step) {
                    require_type(infer(loop->step.get()), SType::NUMBER, loop->line, "E207",
                                 "for range step");
                }
                env.push();
                env.set(loop->var, type(SType::NUMBER));
                check_block(loop->body.get());
                env.pop();
                break;
            }

            case NK::FOREACH: {
                auto* loop = static_cast<const ForeachStmt*>(statement);
                TypeAnnot collection = infer(loop->collection.get());
                if (known(collection) && collection.kind != SType::ARRAY &&
                    collection.kind != SType::DICT && collection.kind != SType::STRING &&
                    collection.kind != SType::NIL) {
                    error(loop->line, "E208",
                          "foreach collection must be array, dict, string, or nil, got " +
                          describe(collection) + ".");
                }
                env.push();
                env.set(loop->var, TypeAnnot{});
                if (!loop->var2.empty()) env.set(loop->var2, TypeAnnot{});
                check_block(loop->body.get());
                env.pop();
                break;
            }

            case NK::FUNC_DEF: {
                auto* function = static_cast<const FuncDef*>(statement);
                FuncSig signature = make_signature(function);
                TypeAnnot previous_return = current_return;
                current_return = function->return_type;

                env.push();
                for (size_t i = 0; i < function->params.size(); ++i) {
                    env.set(function->params[i], signature.param_types[i],
                            !signature.param_types[i].is_any());
                }
                for (size_t i = 0; i < function->defaults.size() && i < function->params.size(); ++i) {
                    if (!function->defaults[i]) continue;
                    TypeAnnot default_type = infer(function->defaults[i].get());
                    if (!compatible(signature.param_types[i], default_type)) {
                        error(function->line, "E206",
                              "default value for parameter '" + function->params[i] +
                              "' expects " + describe(signature.param_types[i]) + ", got " +
                              describe(default_type) + ".");
                    }
                }
                check_block(function->body.get());
                env.pop();
                current_return = previous_return;
                break;
            }

            case NK::RETURN: {
                auto* return_statement = static_cast<const ReturnStmt*>(statement);
                TypeAnnot actual = return_statement->value
                    ? infer(return_statement->value.get()) : type(SType::NIL);
                if (known(current_return) && !compatible(current_return, actual)) {
                    error(return_statement->line, "E205",
                          "return value expects " + describe(current_return) + ", got " +
                          describe(actual) + ".");
                }
                break;
            }

            case NK::EXPR_STMT: {
                auto* expression = static_cast<const ExprStmt*>(statement);
                infer(expression->expr.get());
                break;
            }

            case NK::NEW_INST: {
                auto* creation = static_cast<const NewInstStmt*>(statement);
                for (const auto& argument : creation->args) infer(argument.get());
                env.set(creation->var, type(SType::CLASS, creation->class_name));
                break;
            }

            case NK::THROW: {
                auto* throw_statement = static_cast<const ThrowStmt*>(statement);
                infer(throw_statement->msg.get());
                break;
            }

            case NK::TRY: {
                auto* try_statement = static_cast<const TryStmt*>(statement);
                check_block(try_statement->try_block.get());
                env.push();
                env.set(try_statement->catch_var, type(SType::STRING));
                check_block(try_statement->catch_block.get());
                env.pop();
                if (try_statement->finally_block) check_block(try_statement->finally_block.get());
                break;
            }

            case NK::CLASS_DEF: {
                auto* class_def = static_cast<const ClassDef*>(statement);
                for (const auto& field_name : class_def->field_order) {
                    auto field = class_def->field_defaults.find(field_name);
                    if (field != class_def->field_defaults.end()) infer(field->second.get());
                }
                for (const auto& method : class_def->methods) {
                    const MethodEntry& entry = method.second;
                    TypeAnnot previous_return = current_return;
                    current_return = TypeAnnot{};
                    env.push();
                    env.set("self", type(SType::CLASS, class_def->name));
                    for (const auto& param : entry.params) env.set(param, TypeAnnot{});
                    for (const auto& default_value : entry.defaults) {
                        if (default_value) infer(default_value.get());
                    }
                    if (entry.body) check_block(entry.body);
                    env.pop();
                    current_return = previous_return;
                }
                break;
            }

            case NK::CMD: {
                auto* command = static_cast<const CmdStmt*>(statement);
                for (const auto& argument : command->args) infer(argument.get());
                break;
            }

            case NK::MATCH: {
                auto* match = static_cast<const MatchStmt*>(statement);
                TypeAnnot subject = infer(match->subject.get());
                for (const auto& arm : match->arms) {
                    if (arm.pattern) {
                        TypeAnnot pattern = infer(arm.pattern.get());
                        if (arm.is_range) {
                            // Range arms lower to numeric >= / <= bytecode. Keep
                            // those runtime-only failures from passing --check.
                            require_type(subject, SType::NUMBER, match->line, "E210",
                                         "match range subject");
                            require_type(pattern, SType::NUMBER, match->line, "E210",
                                         "match range start");
                            TypeAnnot range_end = infer(arm.range_end.get());
                            require_type(range_end, SType::NUMBER, match->line, "E210",
                                         "match range end");
                        } else if (known(subject) && known(pattern) &&
                                   !compatible(subject, pattern) &&
                                   !compatible(pattern, subject)) {
                            error(match->line, "E210",
                                  "match pattern type " + describe(pattern) +
                                  " is incompatible with subject type " + describe(subject) + ".");
                        }
                    }
                    check_block(arm.body.get());
                }
                break;
            }

            case NK::ENUM_DEF: {
                auto* enum_def = static_cast<const EnumDef*>(statement);
                for (const auto& member : enum_def->members) {
                    if (member.value) infer(member.value.get());
                }
                break;
            }

            case NK::USE:
            case NK::IMPORT:
            case NK::GLOBAL_DECL:
            case NK::BREAK:
            case NK::CONTINUE:
                break;

            default:
                break;
        }
    }

public:
    int check(const SuraBlock* program) {
        errors.clear();
        env.reset();
        functions.reset();
        class_parents.clear();
        current_return = TypeAnnot{};
        if (!program) return 0;

        // Declaration pass: top-level functions are known before any body or
        // call is checked, so forward calls and mutual recursion are typed.
        collect_classes(program);
        collect_functions(program);
        for (const auto& statement : program->body) check_stmt(statement.get());
        return static_cast<int>(errors.size());
    }

    void print_errors(bool as_warning = false) const {
        const char* red = "\033[1;31m";
        const char* yellow = "\033[1;33m";
        const char* reset = "\033[0m";
        for (const auto& type_error : errors) {
            if (as_warning) {
                std::cerr << yellow << "[Sura Type Warning] line " << type_error.line
                          << ": " << type_error.message << reset << "\n";
            } else {
                std::cerr << red << "[Sura Type Error] line " << type_error.line
                          << ": " << type_error.message << reset << "\n";
            }
        }
    }

    const std::vector<TypeError>& get_errors() const { return errors; }
};
