#include "semantic.hpp"

#include <type_traits>

namespace cru::sem {

void SemanticAnalyzer::validateCall(
    const ast::DirectCallInst& call
) {
    auto it = functions_.find(call.functionName);

    if (it == functions_.end()) {
        report(
            call.location,
            "Call to unknown function '" + call.functionName + "'"
        );
        return;
    }

    const auto& fn = it->second;

    if (!sameType(call.returnType, fn.returnType)) {
        report(call.location, "Call return type does not match function signature");
    }

    if (call.arguments.size() != fn.parameterTypes.size()) {
        report(call.location, "Incorrect number of function arguments");
    }

    const auto count = std::min(
        call.arguments.size(), fn.parameterTypes.size()
    );

    for (std::size_t i = 0; i < count; ++i)
        operandType(call.arguments[i], call.location, &fn.parameterTypes[i]);

    if (call.result && isVoid(call.returnType))
        report(call.location, "Cannot assign the result of a void call");
}

void SemanticAnalyzer::validateCall(
    const ast::IndirectCallInst& call
) {
    auto fnType = operandType(call.function, call.location);

    if (fnType && !isPointer(*fnType))
        report(call.location, "Indirect call target must have type ptr");

    // The current AST has no function-pointer signature type.
    // Therefore, argument count and argument types cannot yet
    // be checked for indirect calls.
    for (const auto& argument : call.arguments)
        operandType(argument, call.location);

    if (call.result && isVoid(call.returnType))
        report(call.location, "Cannot assign the result of a void call");
}

void SemanticAnalyzer::checkInstruction(
    const ast::Instruction& instruction
) {
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::CallInst>) {
            std::visit([&](const auto& call) {
                validateCall(call);

                if (call.result)
                    defineResult(*call.result, call.returnType);
            }, node);
        } else {
            auto operandType = [&](const ast::Operand& operand,
                                    const ast::Type* expected = nullptr) {
                return operandType(operand, node.location, expected);
            };

            auto checkAddress = [&](const ast::Operand& address) {
                auto type = operandType(address);

                if (type && !isPointer(*type))
                    report(
                        node.location,
                        "Memory address must have type ptr"
                    );
            };
        }
        

        if constexpr (std::is_same_v<T, ast::ConstantInst>) {
            validateType(node.type, node.location);

            ast::Operand value = node.value;
            operandType(value, node.location, &node.type);

            defineResult(node.result, node.type);
        }
        else if constexpr (std::is_same_v<T, ast::UnaryInst>) {
            auto operand = checkOperand(node.operand, &node.type);

            if (node.op == ast::UnaryOp::Neg &&
                !isNumeric(node.type))
                report(node.location, "'neg' requires a numeric type");

            if (node.op == ast::UnaryOp::Not &&
                !isBoolean(node.type))
                report(node.location, "'not' requires bool");

            if (node.op == ast::UnaryOp::BitNot &&
                !isInteger(node.type))
                report(node.location, "'bitnot' requires an integer type");

            defineResult(node.result, node.type);
        }
        else if constexpr (std::is_same_v<T, ast::BinaryInst>) {
            operandType(node.lhs, &node.type);
            operandType(node.rhs, &node.type);

            if (node.op == ast::BinaryOp::And ||
                node.op == ast::BinaryOp::Or ||
                node.op == ast::BinaryOp::Xor ||
                node.op == ast::BinaryOp::Shl ||
                node.op == ast::BinaryOp::Shr) {
                if (!isInteger(node.type))
                    report(node.location, "Bitwise operation requires an integer type");
            } else if (!isNumeric(node.type)) {
                report(node.location, "Arithmetic operation requires a numeric type");
            }

            defineResult(node.result, node.type);
        }
        else if constexpr (std::is_same_v<T, ast::CompareInst>) {
            checkOperand(node.lhs, &node.type);
            checkOperand(node.rhs, &node.type);

            if (!isInteger(node.type))
                report(node.location, "Integer comparison requires an integer type");

            defineResult(node.result, ast::Type(ast::TypeKind::Bool));
        }
        else if constexpr (std::is_same_v<T, ast::FloatCompareInst>) {
            checkOperand(node.lhs, &node.type);
            checkOperand(node.rhs, &node.type);

            if (!isFloat(node.type))
                report(node.location, "Float comparison requires f32 or f64");

            defineResult(node.result, ast::Type(ast::TypeKind::Bool));
        }
        else if constexpr (std::is_same_v<T, ast::FloatBinaryInst>) {
            checkOperand(node.lhs, &node.type);
            checkOperand(node.rhs, &node.type);

            if (!isFloat(node.type))
                report(node.location, "Float operation requires f32 or f64");

            defineResult(node.result, node.type);
        }
        else if constexpr (std::is_same_v<T, ast::CastInst>) {
            auto source = checkOperand(node.operand);
            validateType(node.targetType, node.location);

            if (source) {
                const bool valid =
                    (isNumeric(*source) && isNumeric(node.targetType)) ||
                    (isPointer(*source) && isPointer(node.targetType)) ||
                    (isInteger(*source) && isPointer(node.targetType)) ||
                    (isPointer(*source) && isInteger(node.targetType)) ||
                    sameType(*source, node.targetType);

                if (!valid)
                    report(node.location, "Unsupported cast between these types");
            }

            defineResult(node.result, node.targetType);
        }
        else if constexpr (std::is_same_v<T, ast::AllocaInst>) {
            validateType(node.allocatedType, node.location);
            validAlignment(node.alignment, node.location);

            defineResult(node.result, ast::Type(ast::TypeKind::Ptr));
        }
        else if constexpr (std::is_same_v<T, ast::LoadInst>) {
            validateType(node.type, node.location);
            checkAddress(node.address);

            if (isVoid(node.type))
                report(node.location, "Cannot load void");

            defineResult(node.result, node.type);
        }
        else if constexpr (std::is_same_v<T, ast::StoreInst>) {
            validateType(node.type, node.location);
            checkOperand(node.value, &node.type);
            checkAddress(node.address);

            if (isVoid(node.type))
                report(node.location, "Cannot store void");
        }
        else if constexpr (std::is_same_v<T, ast::GlobalAddrInst>) {
            if (!globals_.count(node.globalName)) {
                report(
                    node.location,
                    "Unknown global '" + node.globalName + "'"
                );
            }

            defineResult(node.result, ast::Type(ast::TypeKind::Ptr));
        }
        else if constexpr (std::is_same_v<T, ast::FieldAddrInst>) {
            checkAddress(node.object);

            auto it = aggregates_.find(node.structName);

            if (it == aggregates_.end()) {
                report(
                    node.location,
                    "Unknown aggregate type '" + node.structName + "'"
                );
            } else if (node.fieldIndex >= it->second.fields.size()) {
                report(node.location, "Field index is out of range");
            }

            defineResult(node.result, ast::Type(ast::TypeKind::Ptr));
        }
        else if constexpr (std::is_same_v<T, ast::ElementAddrInst>) {
            validateType(node.elementType, node.location);
            checkAddress(node.base);

            auto index = checkOperand(node.index);
            if (index && !isInteger(*index))
                report(node.location, "Array index must be an integer");

            defineResult(node.result, ast::Type(ast::TypeKind::Ptr));
        }
        else if constexpr (std::is_same_v<T, ast::CallInst>) {
            std::visit([&](const auto& call) {
                validateCall(call);

                if (call.result)
                    defineResult(*call.result, call.returnType);
            }, node);
        }
        else if constexpr (std::is_same_v<T, ast::AtomicLoadInst>) {
            if (!isInteger(node.type))
                report(node.location, "atomic.load requires an integer type");

            checkAddress(node.address);
            defineResult(node.result, node.type);

            if (node.ordering == ast::MemoryOrdering::Release ||
                node.ordering == ast::MemoryOrdering::AcqRel)
                report(node.location, "atomic.load cannot use release or acq_rel");
        }
        else if constexpr (std::is_same_v<T, ast::AtomicStoreInst>) {
            if (!isInteger(node.type))
                report(node.location, "atomic.store requires an integer type");

            checkOperand(node.value, &node.type);
            checkAddress(node.address);

            if (node.ordering == ast::MemoryOrdering::Acquire ||
                node.ordering == ast::MemoryOrdering::AcqRel)
                report(node.location, "atomic.store cannot use acquire or acq_rel");
        }
        else if constexpr (std::is_same_v<T, ast::AtomicRMWInst>) {
            if (!isInteger(node.type))
                report(node.location, "atomic.rmw requires an integer type");

            checkAddress(node.address);
            checkOperand(node.value, &node.type);
            defineResult(node.result, node.type);
        }
        else if constexpr (std::is_same_v<T, ast::AtomicCASInst>) {
            if (!isInteger(node.type))
                report(node.location, "atomic.cas requires an integer type");

            checkAddress(node.address);
            checkOperand(node.expected, &node.type);
            checkOperand(node.desired, &node.type);

            defineResult(node.oldValue, node.type);
            defineResult(node.success, ast::Type(ast::TypeKind::Bool));

            if (node.successOrdering == ast::MemoryOrdering::Release &&
                node.failureOrdering != ast::FailureOrdering::Relaxed)
                report(node.location, "Invalid CAS failure ordering");

            if (node.successOrdering == ast::MemoryOrdering::AcqRel &&
                node.failureOrdering == ast::FailureOrdering::SeqCst)
                report(node.location, "CAS failure ordering cannot be stronger than success ordering");
        }
    }, instruction);
}

} // namespace cru::sem
