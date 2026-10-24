#include "semantic.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace cru::sem {

namespace {

// Menggunakan overload biasa untuk dispatch std::variant.
// Tidak membutuhkan if constexpr atau std::is_same_v.
template <typename... Functions>
struct InstructionOverload : Functions... {
using Functions::operator()...;
};

template <typename... Functions>
InstructionOverload(Functions...) -> InstructionOverload<Functions...>;

} // namespace

// ============================================================
// Direct call
// ============================================================

void SemanticAnalyzer::validateCall(
const ast::DirectCallInst& call
) {
auto it = functions_.find(call.functionName);

if (it == functions_.end()) {
    report(
        call.location,
        "Call to unknown function '" + call.functionName + "'"
    );

    // Tetap periksa argumen agar diagnostic tidak berhenti
    // hanya pada kesalahan nama fungsi.
    for (const auto& argument : call.arguments) {
        operandType(argument, call.location);
    }

    return;
}

const FunctionInfo& function = it->second;

if (!sameType(call.returnType, function.returnType)) {
    report(
        call.location,
        "Call return type does not match function signature"
    );
}

if (call.arguments.size() != function.parameterTypes.size()) {
    report(
        call.location,
        "Incorrect number of function arguments"
    );
}

const std::size_t count = std::min(
    call.arguments.size(),
    function.parameterTypes.size()
);

for (std::size_t i = 0; i < count; ++i) {
    operandType(
        call.arguments[i],
        call.location,
        &function.parameterTypes[i]
    );
}

// Periksa juga argumen berlebih, walaupun tipe parameter
// pasangannya tidak tersedia.
for (std::size_t i = count; i < call.arguments.size(); ++i) {
    operandType(call.arguments[i], call.location);
}

if (call.result && isVoid(call.returnType)) {
    report(
        call.location,
        "Cannot assign the result of a void call"
    );
}

}

// ============================================================
// Indirect call
// ============================================================

void SemanticAnalyzer::validateCall(
const ast::IndirectCallInst& call
) {
const auto functionType = operandType(
call.function,
call.location
);

if (functionType && !isPointer(*functionType)) {
    report(
        call.location,
        "Indirect call target must have type ptr"
    );
}

// AST saat ini belum menyimpan signature function pointer.
// Karena itu, tipe parameter dan jumlah argumen belum bisa
// divalidasi secara lengkap untuk indirect call.
for (const auto& argument : call.arguments) {
    operandType(argument, call.location);
}

if (call.result && isVoid(call.returnType)) {
    report(
        call.location,
        "Cannot assign the result of a void call"
    );
}

}

// ============================================================
// Instruction validation
// ============================================================

void SemanticAnalyzer::checkInstruction(
const ast::Instruction& instruction
) {
// Helper lokal untuk menghindari pengulangan parameter lokasi.
auto getOperandType = [this](
const ast::Operand& operand,
const ast::SourceLocation& location,
const ast::Type* expected = nullptr
) {
return operandType(operand, location, expected);
};

auto checkAddress = [this, &getOperandType](
    const ast::Operand& address,
    const ast::SourceLocation& location
) {
    const auto type = getOperandType(address, location);

    if (type && !isPointer(*type)) {
        report(
            location,
            "Memory address must have type ptr"
        );
    }
};

auto checkIntegerType = [this](
    const ast::Type& type,
    const ast::SourceLocation& location,
    const std::string& operation
) {
    if (!isInteger(type)) {
        report(
            location,
            operation + " requires an integer type"
        );
    }
};

auto checkFloatType = [this](
    const ast::Type& type,
    const ast::SourceLocation& location,
    const std::string& operation
) {
    if (!isFloat(type)) {
        report(
            location,
            operation + " requires f32 or f64"
        );
    }
};

auto checkMemoryOrdering = [this](
    ast::MemoryOrdering ordering,
    const ast::SourceLocation& location,
    const std::string& operation
) {
    // Helper ini hanya digunakan untuk operasi yang tidak
    // menerima Release atau AcqRel sebagai ordering.
    if (ordering == ast::MemoryOrdering::Release ||
        ordering == ast::MemoryOrdering::AcqRel) {
        report(
            location,
            operation + " cannot use release or acq_rel"
        );
    }
};

std::visit(
    InstructionOverload{

        // ------------------------------------------------
        // Constant
        // ------------------------------------------------


        [this, &getOperandType](
    const ast::SyscallInst& x
) {
    validateType(x.resultType, x.location);

    const auto numberType = getOperandType(
        x.number,
        x.location
    );

    if (numberType && !isInteger(*numberType)) {
        report(
            x.location,
            "Syscall number must have an integer type"
        );
    }

    for (const auto& arg : x.args) {
        getOperandType(arg, x.location);
    }

    if (!x.result.empty()) {
        if (isVoid(x.resultType)) {
            report(
                x.location,
                "Cannot assign the result of a void syscall"
            );
        } else {
            defineResult(x.result, x.resultType);
        }
    }
},
        [this, &getOperandType](
            const ast::ConstantInst& node
        ) {
            validateType(node.type, node.location);

            ast::Operand value = node.value;

            getOperandType(
                value,
                node.location,
                &node.type
            );

            defineResult(node.result, node.type);
        },

        // ------------------------------------------------
        // Unary operations
        // ------------------------------------------------

        [this, &getOperandType](
            const ast::UnaryInst& node
        ) {
            validateType(node.type, node.location);

            getOperandType(
                node.operand,
                node.location,
                &node.type
            );

            switch (node.op) {
            case ast::UnaryOp::Neg:
                if (!isNumeric(node.type)) {
                    report(
                        node.location,
                        "'neg' requires a numeric type"
                    );
                }
                break;

            case ast::UnaryOp::Not:
                if (!isBoolean(node.type)) {
                    report(
                        node.location,
                        "'not' requires bool"
                    );
                }
                break;

            case ast::UnaryOp::BitNot:
                if (!isInteger(node.type)) {
                    report(
                        node.location,
                        "'bitnot' requires an integer type"
                    );
                }
                break;
            }

            defineResult(node.result, node.type);
        },

        // ------------------------------------------------
        // Integer arithmetic and bitwise operations
        // ------------------------------------------------

        [this, &getOperandType, &checkIntegerType](
            const ast::BinaryInst& node
        ) {
            validateType(node.type, node.location);

            getOperandType(
                node.lhs,
                node.location,
                &node.type
            );

            getOperandType(
                node.rhs,
                node.location,
                &node.type
            );

            switch (node.op) {
            case ast::BinaryOp::And:
            case ast::BinaryOp::Or:
            case ast::BinaryOp::Xor:
            case ast::BinaryOp::Shl:
            case ast::BinaryOp::Shr:
                checkIntegerType(
                    node.type,
                    node.location,
                    "Bitwise operation"
                );
                break;

            case ast::BinaryOp::Add:
            case ast::BinaryOp::Sub:
            case ast::BinaryOp::Mul:
            case ast::BinaryOp::Div:
            case ast::BinaryOp::Rem:
                if (!isNumeric(node.type)) {
                    report(
                        node.location,
                        "Arithmetic operation requires a numeric type"
                    );
                }
                break;
            }

            defineResult(node.result, node.type);
        },

        // ------------------------------------------------
        // Integer comparison
        // ------------------------------------------------

        [this, &getOperandType](
            const ast::CompareInst& node
        ) {
            validateType(node.type, node.location);

            getOperandType(
                node.lhs,
                node.location,
                &node.type
            );

            getOperandType(
                node.rhs,
                node.location,
                &node.type
            );

            if (!isInteger(node.type)) {
                report(
                    node.location,
                    "Integer comparison requires an integer type"
                );
            }

            defineResult(
                node.result,
                ast::Type(ast::TypeKind::Bool)
            );
        },

        // ------------------------------------------------
        // Floating-point comparison
        // ------------------------------------------------

        [this, &getOperandType, &checkFloatType](
            const ast::FloatCompareInst& node
        ) {
            validateType(node.type, node.location);

            getOperandType(
                node.lhs,
                node.location,
                &node.type
            );

            getOperandType(
                node.rhs,
                node.location,
                &node.type
            );

            checkFloatType(
                node.type,
                node.location,
                "Float comparison"
            );

            defineResult(
                node.result,
                ast::Type(ast::TypeKind::Bool)
            );
        },

        // ------------------------------------------------
        // Floating-point arithmetic
        // ------------------------------------------------

        [this, &getOperandType, &checkFloatType](
            const ast::FloatBinaryInst& node
        ) {
            validateType(node.type, node.location);

            getOperandType(
                node.lhs,
                node.location,
                &node.type
            );

            getOperandType(
                node.rhs,
                node.location,
                &node.type
            );

            checkFloatType(
                node.type,
                node.location,
                "Float operation"
            );

            defineResult(node.result, node.type);
        },

        // ------------------------------------------------
        // Cast
        // ------------------------------------------------

        [this, &getOperandType](
            const ast::CastInst& node
        ) {
            const auto source = getOperandType(
                node.operand,
                node.location
            );

            validateType(node.targetType, node.location);

            if (source) {
                const bool valid =
                    (isNumeric(*source) &&
                     isNumeric(node.targetType)) ||
                    (isPointer(*source) &&
                     isPointer(node.targetType)) ||
                    (isInteger(*source) &&
                     isPointer(node.targetType)) ||
                    (isPointer(*source) &&
                     isInteger(node.targetType)) ||
                    sameType(*source, node.targetType);

                if (!valid) {
                    report(
                        node.location,
                        "Unsupported cast between these types"
                    );
                }
            }

            defineResult(node.result, node.targetType);
        },

        // ------------------------------------------------
        // Stack allocation
        // ------------------------------------------------

        [this](
            const ast::AllocaInst& node
        ) {
            validateType(
                node.allocatedType,
                node.location
            );

            validAlignment(
                node.alignment,
                node.location
            );

            defineResult(
                node.result,
                ast::Type(ast::TypeKind::Ptr)
            );
        },

        // ------------------------------------------------
        // Load
        // ------------------------------------------------

        [this, &checkAddress](
            const ast::LoadInst& node
        ) {
            validateType(node.type, node.location);

            checkAddress(node.address, node.location);

            if (isVoid(node.type)) {
                report(
                    node.location,
                    "Cannot load void"
                );
            }

            defineResult(node.result, node.type);
        },

        // ------------------------------------------------
        // Store
        // ------------------------------------------------

        [this, &getOperandType, &checkAddress](
            const ast::StoreInst& node
        ) {
            validateType(node.type, node.location);

            getOperandType(
                node.value,
                node.location,
                &node.type
            );

            checkAddress(node.address, node.location);

            if (isVoid(node.type)) {
                report(
                    node.location,
                    "Cannot store void"
                );
            }
        },

        // ------------------------------------------------
        // Global address
        // ------------------------------------------------

        [this](
            const ast::GlobalAddrInst& node
        ) {
            if (!globals_.count(node.globalName)) {
                report(
                    node.location,
                    "Unknown global '" +
                    node.globalName + "'"
                );
            }

            defineResult(
                node.result,
                ast::Type(ast::TypeKind::Ptr)
            );
        },

        // ------------------------------------------------
        // Aggregate field address
        // ------------------------------------------------

        [this, &checkAddress](
            const ast::FieldAddrInst& node
        ) {
            checkAddress(node.object, node.location);

            auto it = aggregates_.find(node.structName);

            if (it == aggregates_.end()) {
                report(
                    node.location,
                    "Unknown aggregate type '" +
                    node.structName + "'"
                );
            } else if (
                node.fieldIndex >= it->second.fields.size()
            ) {
                report(
                    node.location,
                    "Field index is out of range"
                );
            }

            defineResult(
                node.result,
                ast::Type(ast::TypeKind::Ptr)
            );
        },

        // ------------------------------------------------
        // Element address
        // ------------------------------------------------

        [this, &getOperandType, &checkAddress](
            const ast::ElementAddrInst& node
        ) {
            validateType(node.elementType, node.location);

            checkAddress(node.base, node.location);

            const auto index = getOperandType(
                node.index,
                node.location
            );

            if (index && !isInteger(*index)) {
                report(
                    node.location,
                    "Array index must be an integer"
                );
            }

            defineResult(
                node.result,
                ast::Type(ast::TypeKind::Ptr)
            );
        },

        // ------------------------------------------------
        // Call instruction variant
        // ------------------------------------------------

        [this](
            const ast::CallInst& node
        ) {
            std::visit(
                InstructionOverload{
                    [this](const ast::DirectCallInst& call) {
                        validateCall(call);

                        if (call.result) {
                            defineResult(
                                *call.result,
                                call.returnType
                            );
                        }
                    },

                    [this](const ast::IndirectCallInst& call) {
                        validateCall(call);

                        if (call.result) {
                            defineResult(
                                *call.result,
                                call.returnType
                            );
                        }
                    }
                },
                node
            );
        },

        // ------------------------------------------------
        // Atomic load
        // ------------------------------------------------

        [this, &checkAddress, &checkIntegerType,
         &checkMemoryOrdering](
            const ast::AtomicLoadInst& node
        ) {
            validateType(node.type, node.location);

            checkIntegerType(
                node.type,
                node.location,
                "atomic.load"
            );

            checkAddress(node.address, node.location);

            checkMemoryOrdering(
                node.ordering,
                node.location,
                "atomic.load"
            );

            defineResult(node.result, node.type);
        },

        // ------------------------------------------------
        // Atomic store
        // ------------------------------------------------

        [this, &getOperandType, &checkAddress,
         &checkIntegerType](
            const ast::AtomicStoreInst& node
        ) {
            validateType(node.type, node.location);

            checkIntegerType(
                node.type,
                node.location,
                "atomic.store"
            );

            getOperandType(
                node.value,
                node.location,
                &node.type
            );

            checkAddress(node.address, node.location);

            if (node.ordering == ast::MemoryOrdering::Acquire ||
                node.ordering == ast::MemoryOrdering::AcqRel) {
                report(
                    node.location,
                    "atomic.store cannot use acquire or acq_rel"
                );
            }
        },

        // ------------------------------------------------
        // Atomic read-modify-write
        // ------------------------------------------------

        [this, &getOperandType, &checkAddress,
         &checkIntegerType](
            const ast::AtomicRMWInst& node
        ) {
            validateType(node.type, node.location);

            checkIntegerType(
                node.type,
                node.location,
                "atomic.rmw"
            );

            checkAddress(node.address, node.location);

            getOperandType(
                node.value,
                node.location,
                &node.type
            );

            defineResult(node.result, node.type);
        },

        // ------------------------------------------------
        // Atomic compare-and-swap
        // ------------------------------------------------

        [this, &getOperandType, &checkAddress,
         &checkIntegerType](
            const ast::AtomicCASInst& node
        ) {
            validateType(node.type, node.location);

            checkIntegerType(
                node.type,
                node.location,
                "atomic.cas"
            );

            checkAddress(node.address, node.location);

            getOperandType(
                node.expected,
                node.location,
                &node.type
            );

            getOperandType(
                node.desired,
                node.location,
                &node.type
            );

            defineResult(node.oldValue, node.type);

            defineResult(
                node.success,
                ast::Type(ast::TypeKind::Bool)
            );

            // Failure ordering tidak boleh lebih kuat
            // daripada success ordering.
            bool validFailureOrdering = false;

            switch (node.successOrdering) {
            case ast::MemoryOrdering::Relaxed:
            case ast::MemoryOrdering::Release:
                validFailureOrdering =
                    node.failureOrdering ==
                    ast::FailureOrdering::Relaxed;
                break;

            case ast::MemoryOrdering::Acquire:
            case ast::MemoryOrdering::AcqRel:
                validFailureOrdering =
                    node.failureOrdering ==
                        ast::FailureOrdering::Relaxed ||
                    node.failureOrdering ==
                        ast::FailureOrdering::Acquire;
                break;

            case ast::MemoryOrdering::SeqCst:
                validFailureOrdering =
                    node.failureOrdering ==
                        ast::FailureOrdering::Relaxed ||
                    node.failureOrdering ==
                        ast::FailureOrdering::Acquire ||
                    node.failureOrdering ==
                        ast::FailureOrdering::SeqCst;
                break;
            }

            if (!validFailureOrdering) {
                report(
                    node.location,
                    "CAS failure ordering cannot be stronger "
                    "than success ordering"
                );
            }
        }
    },
    instruction
);

}

} // namespace cru::sem
