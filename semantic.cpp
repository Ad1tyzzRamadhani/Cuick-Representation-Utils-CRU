
#include "semantic.hpp"

#include <sstream>
#include <type_traits>

namespace cru::sem {

void SemanticAnalyzer::report(
    const ast::SourceLocation& location,
    std::string message
) {
    diagnostics_.push_back({location, std::move(message)});
}

bool SemanticAnalyzer::analyze(const ast::Module& module) {
    diagnostics_.clear();
    aggregates_.clear();
    globals_.clear();
    functions_.clear();
    values_.clear();
    labels_.clear();
    currentFunction_ = nullptr;

    registerDeclarations(module);
    validateDeclarations(module);

    return diagnostics_.empty();
}

void SemanticAnalyzer::registerDeclarations(
    const ast::Module& module
) {
    // Pass 1: register aggregate types first.
    for (const auto& declaration : module.declarations) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;

            if constexpr (
                std::is_same_v<T, ast::StructDecl> ||
                std::is_same_v<T, ast::UnionDecl>
            ) {
                AggregateInfo info;
                info.isUnion =
                    std::is_same_v<T, ast::UnionDecl>;
                info.fields = node.fields;

                if (!aggregates_.emplace(
                        node.name, std::move(info)).second) {
                    report(
                        node.location,
                        "Duplicate type declaration '" +
                        node.name + "'"
                    );
                }
            }
        }, declaration);
    }

    // Pass 2: globals and function signatures.
    for (const auto& declaration : module.declarations) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<T, ast::GlobalDecl>) {
                if (globals_.count(node.name) ||
                    functions_.count(node.name) ||
                    aggregates_.count(node.name)) {
                    report(
                        node.location,
                        "Duplicate declaration '" + node.name + "'"
                    );
                } else {
                    globals_.emplace(node.name, node);
                }
            }
            else if constexpr (
                std::is_same_v<T, ast::ExternDecl> ||
                std::is_same_v<T, ast::Function>
            ) {
                const auto& header =
                    [&]() -> const ast::FunctionHeader& {
                        if constexpr (
                            std::is_same_v<T, ast::ExternDecl>
                        )
                            return node.header;
                        else
                            return node.header;
                    }();

                FunctionInfo info;
                info.returnType = header.returnType;
                info.defined =
                    std::is_same_v<T, ast::Function>;

                for (const auto& param : header.params)
                    info.parameterTypes.push_back(param.type);

                auto it = functions_.find(header.name);

                if (it == functions_.end()) {
                    if (globals_.count(header.name) ||
                        aggregates_.count(header.name)) {
                        report(
                            header.location,
                            "Conflicting declaration '" +
                            header.name + "'"
                        );
                    } else {
                        functions_.emplace(header.name, info);
                    }
                } else {
                    const bool sameReturn =
                        sameType(it->second.returnType,
                                 info.returnType);

                    const auto& oldParams =
                        it->second.parameterTypes;

                    bool sameParams =
                        oldParams.size() ==
                        info.parameterTypes.size();

                    if (sameParams) {
                        for (std::size_t i = 0;
                             i < oldParams.size(); ++i) {
                            if (!sameType(
                                    oldParams[i],
                                    info.parameterTypes[i])) {
                                sameParams = false;
                                break;
                            }
                        }
                    }

                    if (!sameReturn || !sameParams) {
                        report(
                            header.location,
                            "Conflicting function signature for '" +
                            header.name + "'"
                        );
                    } else if (info.defined &&
                               it->second.defined) {
                        report(
                            header.location,
                            "Duplicate function definition '" +
                            header.name + "'"
                        );
                    } else if (info.defined) {
                        it->second.defined = true;
                    }
                }
            }
        }, declaration);
    }
}

void SemanticAnalyzer::validateType(
    const ast::Type& type,
    const ast::SourceLocation& location,
    bool allowVoid
) {
    if (type.kind == ast::TypeKind::Void && !allowVoid) {
        report(location, "'void' is not valid in this type position");
        return;
    }

    if (type.kind == ast::TypeKind::Named) {
        if (!aggregates_.count(type.name)) {
            report(
                location,
                "Unknown type '" + type.name + "'"
            );
        }
    }

    if (type.kind == ast::TypeKind::Array) {
        if (!type.elementType) {
            report(location, "Array type has no element type");
            return;
        }

        if (type.arraySize == 0)
            report(location, "Array size must be greater than zero");

        validateType(*type.elementType, location, false);
    }
}

bool SemanticAnalyzer::validAlignment(
    const std::optional<ast::AlignAttr>& alignment,
    const ast::SourceLocation& location
) {
    if (!alignment)
        return true;

    const auto value = alignment->value;

    // Alignment must be a nonzero power of two.
    if (value == 0 || (value & (value - 1)) != 0) {
        report(
            location,
            "Alignment must be a nonzero power of two"
        );
        return false;
    }

    return true;
}

void SemanticAnalyzer::validateDeclarations(
    const ast::Module& module
) {
    for (const auto& declaration : module.declarations) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;

            if constexpr (
                std::is_same_v<T, ast::StructDecl> ||
                std::is_same_v<T, ast::UnionDecl>
            ) {
                validAlignment(node.alignment, node.location);

                std::unordered_set<std::string> fieldNames;

                for (const auto& field : node.fields) {
                    if (!fieldNames.insert(field.name).second) {
                        report(
                            field.location,
                            "Duplicate field '" + field.name + "'"
                        );
                    }

                    validateType(field.type, field.location);
                    validAlignment(field.alignment, field.location);
                }
            }
            else if constexpr (std::is_same_v<T, ast::GlobalDecl>) {
                validateGlobal(node);
            }
            else if constexpr (std::is_same_v<T, ast::ExternDecl>) {
                const auto& h = node.header;

                validateType(h.returnType, h.location, true);

                std::unordered_set<std::string> params;
                for (const auto& p : h.params) {
                    validateType(p.type, p.location);
                    if (!params.insert(p.name).second)
                        report(p.location, "Duplicate parameter '" + p.name + "'");
                }
            }
            else if constexpr (std::is_same_v<T, ast::Function>) {
                validateType(node.header.returnType,
                             node.header.location, true);

                for (const auto& p : node.header.params)
                    validateType(p.type, p.location);

                validateFunction(node);
            }
        }, declaration);
    }
}

void SemanticAnalyzer::validateGlobal(
    const ast::GlobalDecl& global
) {
    validateType(global.type, global.location);
    validAlignment(global.alignment, global.location);

    if (!global.initializer)
        return;

    const auto& init = *global.initializer;

    if (const auto* literal = std::get_if<ast::Literal>(&init)) {
        ast::Operand operand = *literal;
        operandType(operand, literal->location, &global.type);
        return;
    }

    const auto* array = std::get_if<ast::ArrayLiteral>(&init);
    if (!array)
        return;

    if (global.type.kind != ast::TypeKind::Array ||
        !global.type.elementType) {
        report(
            global.location,
            "Array initializer requires an array type"
        );
        return;
    }

    if (array->elements.size() > global.type.arraySize) {
        report(
            global.location,
            "Too many elements in array initializer"
        );
    }

    for (const auto& literal : array->elements) {
        ast::Operand operand = literal;
        operandType(
            operand,
            literal.location,
            global.type.elementType.get()
        );
    }
}

void SemanticAnalyzer::validateFunction(
    const ast::Function& function
) {
    currentFunction_ = &function;
    values_.clear();
    labels_.clear();

    for (const auto& param : function.header.params) {
        if (values_.count(param.name)) {
            report(
                param.location,
                "Duplicate parameter '" + param.name + "'"
            );
        } else {
            values_.emplace(param.name, param.type);
        }
    }

    for (const auto& block : function.blocks) {
        if (!labels_.insert(block.name).second) {
            report(
                block.location,
                "Duplicate label '" + block.name + "'"
            );
        }
    }

    if (function.blocks.empty()) {
        report(
            function.location,
            "Function must contain at least one basic block"
        );
    }

    for (const auto& block : function.blocks)
        validateBlock(block);

    currentFunction_ = nullptr;
    values_.clear();
}

void SemanticAnalyzer::validateBlock(
    const ast::LabelBlock& block
) {
    for (const auto& instruction : block.instructions)
        checkInstruction(instruction);

    if (!block.terminator) {
        report(
            block.location,
            "Basic block is missing a terminator"
        );
        return;
    }

    validateTerminator(*block.terminator);
}

void SemanticAnalyzer::validateTerminator(
    const ast::Terminator& terminator
) {
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::ReturnTerminator>) {
            const auto& expected =
                currentFunction_->header.returnType;

            if (!node.value) {
                if (!isVoid(expected))
                    report(node.location, "Non-void function must return a value");
            } else {
                if (isVoid(expected)) {
                    report(node.location, "Void function cannot return a value");
                } else {
                    operandType(*node.value, node.location, &expected);
                }
            }
        }
        else if constexpr (std::is_same_v<T, ast::JumpTerminator>) {
            if (!labels_.count(node.target))
                report(node.location, "Unknown label '" + node.target + "'");
        }
        else if constexpr (std::is_same_v<T, ast::BranchIfTerminator>) {
            auto condition = operandType(node.condition, node.location);

            if (condition && !isBoolean(*condition))
                report(node.location, "br_if condition must have type bool");

            if (!labels_.count(node.trueTarget))
                report(node.location, "Unknown label '" + node.trueTarget + "'");

            if (!labels_.count(node.falseTarget))
                report(node.location, "Unknown label '" + node.falseTarget + "'");
        }
    }, terminator);
}

void SemanticAnalyzer::defineResult(
    const ast::ResultRef& result,
    const ast::Type& type
) {
    if (values_.count(result.name)) {
        report(
            result.location,
            "Duplicate SSA result '%" + result.name + "'"
        );
        return;
    }

    values_.emplace(result.name, type);
}

std::optional<ast::Type> SemanticAnalyzer::operandType(
    const ast::Operand& operand,
    const ast::SourceLocation& location,
    const ast::Type* expected
) {
    if (const auto* result = std::get_if<ast::ResultRef>(&operand)) {
        auto it = values_.find(result->name);

        if (it == values_.end()) {
            report(
                result->location,
                "Use of undefined result '%" + result->name + "'"
            );
            return std::nullopt;
        }

        if (expected)
            requireSame(it->second, *expected, location, "Operand");

        return it->second;
    }

    const auto& literal = std::get<ast::Literal>(operand);
    ast::Type inferred;

    switch (literal.kind) {
    case ast::LiteralKind::Boolean:
        inferred = ast::Type(ast::TypeKind::Bool);
        break;

    case ast::LiteralKind::Null:
        inferred = ast::Type(ast::TypeKind::Ptr);
        break;

    case ast::LiteralKind::Float:
        inferred = expected && isFloat(*expected)
            ? *expected : ast::Type(ast::TypeKind::F64);
        break;

    case ast::LiteralKind::Integer:
        inferred = expected && isInteger(*expected)
            ? *expected : ast::Type(ast::TypeKind::I32);
        break;
    }

    if (expected) {
        const bool compatible =
            sameType(inferred, *expected) ||
            (literal.kind == ast::LiteralKind::Integer &&
             isInteger(*expected)) ||
            (literal.kind == ast::LiteralKind::Float &&
             isFloat(*expected)) ||
            (literal.kind == ast::LiteralKind::Null &&
             isPointer(*expected));

        if (!compatible) {
            report(
                location,
                "Literal is incompatible with expected type '" +
                std::string(typeName(expected->kind)) + "'"
            );
            return std::nullopt;
        }

        inferred = *expected;
    }

    return inferred;
}

bool SemanticAnalyzer::requireSame(
    const ast::Type& actual,
    const ast::Type& expected,
    const ast::SourceLocation& location,
    const std::string& context
) {
    if (sameType(actual, expected))
        return true;

    report(
        location,
        context + " type mismatch: expected '" +
        typeName(expected.kind) + "', got '" +
        typeName(actual.kind) + "'"
    );
    return false;
}

} // namespace cru::sem
