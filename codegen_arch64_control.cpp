
#include "codegen_arch64.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <utility>

namespace cru::codegen {
namespace {

using namespace ast;

std::string normalizeName(std::string name) {
    if (!name.empty() && name.front() == '%')
        name.erase(name.begin());
    return name;
}

std::string safeLabelPart(const std::string& name) {
    std::string result;
    result.reserve(name.size());

    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '_' || c == '.') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('_');
        }
    }

    if (result.empty())
        result = "unnamed";

    return result;
}

bool isFloatType(const Type& type) {
    return type.kind == TypeKind::F32 ||
           type.kind == TypeKind::F64;
}

std::uint64_t typeWidth(const Type& type) {
    switch (type.kind) {
    case TypeKind::I8:
    case TypeKind::U8:
    case TypeKind::Bool:
        return 8;

    case TypeKind::I16:
    case TypeKind::U16:
        return 16;

    case TypeKind::I32:
    case TypeKind::U32:
    case TypeKind::F32:
        return 32;

    case TypeKind::I64:
    case TypeKind::U64:
    case TypeKind::ISize:
    case TypeKind::USize:
    case TypeKind::F64:
    case TypeKind::Ptr:
        return 64;

    default:
        return 0;
    }
}

const StackSlot* findValueSlot(
    const FunctionFrame& frame,
    const std::string& name
) {
    const auto key = normalizeName(name);

    auto it = frame.valueSlots.find(key);
    if (it != frame.valueSlots.end())
        return &it->second;

    auto parameter = frame.parameterSlots.find(key);
    if (parameter != frame.parameterSlots.end())
        return &parameter->second;

    return nullptr;
}

const Type* findOperandType(
    const FunctionFrame& frame,
    const Operand& operand
) {
    const auto* ref = std::get_if<ResultRef>(&operand);
    if (!ref)
        return nullptr;

    const auto it = frame.valueTypes.find(normalizeName(ref->name));
    if (it == frame.valueTypes.end())
        return nullptr;

    return &it->second;
}

void emitMovImmediate(
    std::string& output,
    const std::string& reg,
    std::uint64_t value
) {
    bool emitted = false;

    for (unsigned shift = 0; shift < 64; shift += 16) {
        const auto part =
            static_cast<unsigned>((value >> shift) & 0xffffu);

        if (part == 0 && (emitted || value != 0))
            continue;

        output += emitted ? "\tmovk " : "\tmovz ";
        output += reg + ", #" + std::to_string(part);

        if (shift != 0)
            output += ", lsl #" + std::to_string(shift);

        output += "\n";
        emitted = true;
    }

    if (!emitted)
        output += "\tmovz " + reg + ", #0\n";
}

bool emitOperandToRegister(
    AArch64Context& context,
    const FunctionFrame& frame,
    const Operand& operand,
    const Type& expectedType,
    std::string& output,
    const SourceLocation& location,
    const std::string& integerReg,
    const std::string& floatReg
) {
    if (const auto* literal = std::get_if<Literal>(&operand)) {
        if (literal->kind == LiteralKind::Boolean) {
            if (literal->text != "true" && literal->text != "false") {
                context.report(location, "invalid boolean literal");
                return false;
            }

            emitMovImmediate(
                output, integerReg,
                literal->text == "true" ? 1 : 0
            );
            return true;
        }

        if (literal->kind == LiteralKind::Null) {
            emitMovImmediate(output, integerReg, 0);
            return true;
        }

        if (literal->kind == LiteralKind::Integer) {
            char* end = nullptr;
            const auto value = std::strtoll(
                literal->text.c_str(), &end, 0
            );

            if (end == literal->text.c_str() || *end != '\0') {
                context.report(location, "invalid integer literal");
                return false;
            }

            emitMovImmediate(
                output,
                integerReg,
                static_cast<std::uint64_t>(value)
            );
            return true;
        }

        context.report(location,
            "floating-point literal handling for terminators is not implemented");
        return false;
    }

    const auto& ref = std::get<ResultRef>(operand);
    const auto* slot = findValueSlot(frame, ref.name);

    if (!slot) {
        context.report(location,
            "no stack slot for operand: " + ref.name);
        return false;
    }

    const Type* sourceType = findOperandType(frame, operand);
    const Type& actualType = sourceType ? *sourceType : expectedType;

    if (isFloatType(actualType)) {
        const std::string reg =
            actualType.kind == TypeKind::F32 ? "s16" : "d16";

        output += "\tldr " + reg + ", [x29, #-" +
                  std::to_string(slot->offset) + "]\n";

        if (reg != floatReg)
            output += "\tfmov " + floatReg + ", " + reg + "\n";
    } else {
        // SSA slots are 8 bytes. Loading the complete slot avoids
        // truncating a boolean or narrow integer condition prematurely.
        output += "\tldr " + integerReg + ", [x29, #-" +
                  std::to_string(slot->offset) + "]\n";
    }

    return true;
}

bool validateBlockTarget(
    AArch64Context& context,
    const Function& function,
    const std::string& target,
    const SourceLocation& location
) {
    for (const auto& block : function.blocks) {
        if (block.name == target)
            return true;
    }

    context.report(location,
        "branch targets unknown block '" + target +
        "' in function '" + function.header.name + "'");
    return false;
}

void emitEpilogue(std::string& output) {
    // The standard prologue saves x29/x30 and sets x29 to the save area.
    // Resetting SP from x29 discards the local frame, regardless of size.
    output += "\tmov sp, x29\n";
    output += "\tldp x29, x30, [sp], #16\n";
    output += "\tret\n";
}

} // namespace

std::string aarch64BlockLabel(
    const ast::Function& function,
    const std::string& blockName
) {
    return ".Lcru_" + safeLabelPart(function.header.name) +
           "_" + safeLabelPart(blockName);
}

bool emitAArch64Terminator(
    AArch64Context& context,
    const FunctionFrame& frame,
    const ast::Function& function,
    const ast::Terminator& terminator,
    std::string& output,
    const ast::SourceLocation& location
) {
    using namespace ast;

    return std::visit(
        [&](const auto& term) -> bool {
            using T = std::decay_t<decltype(term)>;

            if constexpr (std::is_same_v<T, ReturnTerminator>) {
                const Type& returnType = function.header.returnType;

                if (term.value) {
                    const auto width = typeWidth(returnType);

                    if (width == 0) {
                        context.report(location,
                            "returning this type is not supported by scalar return emission");
                        return false;
                    }

                    if (isFloatType(returnType)) {
                        const std::string fpReg =
                            returnType.kind == TypeKind::F32 ? "s0" : "d0";

                        if (!emitOperandToRegister(
                                context, frame, *term.value, returnType,
                                output, location, "x9", fpReg)) {
                            return false;
                        }

                        const auto* operandType =
                            findOperandType(frame, *term.value);

                        if (operandType && isFloatType(*operandType)) {
                            const std::string sourceReg =
                                operandType->kind == TypeKind::F32
                                    ? "s16" : "d16";

                            if (sourceReg != fpReg) {
                                output += "\tfmov " + fpReg + ", " +
                                          sourceReg + "\n";
                            }
                        } else {
                            context.report(location,
                                "return type mismatch: expected floating-point value");
                            return false;
                        }
                    } else {
                        if (isFloatType(returnType)) {
                            context.report(location,
                                "internal return type mismatch");
                            return false;
                        }

                        if (!emitOperandToRegister(
                                context, frame, *term.value, returnType,
                                output, location, "x0", "d0")) {
                            return false;
                        }
                    }
                } else if (returnType.kind != TypeKind::Void) {
                    context.report(location,
                        "non-void function returns without a value");
                    return false;
                }

                emitEpilogue(output);
                return true;
            } else if constexpr (std::is_same_v<T, JumpTerminator>) {
                if (!validateBlockTarget(
                        context, function, term.target, location)) {
                    return false;
                }

                output += "\tb " +
                          aarch64BlockLabel(function, term.target) + "\n";
                return true;
            } else if constexpr (
                std::is_same_v<T, BranchIfTerminator>) {
                if (!validateBlockTarget(
                        context, function, term.trueTarget, location) ||
                    !validateBlockTarget(
                        context, function, term.falseTarget, location)) {
                    return false;
                }

                Type boolType;
                boolType.kind = TypeKind::Bool;

                if (!emitOperandToRegister(
                        context, frame, term.condition, boolType,
                        output, location, "x9", "d0")) {
                    return false;
                }

                output += "\tcmp x9, #0\n";
                output += "\tb.ne " +
                    aarch64BlockLabel(function, term.trueTarget) + "\n";
                output += "\tb " +
                    aarch64BlockLabel(function, term.falseTarget) + "\n";
                return true;
            } else if constexpr (
                std::is_same_v<T, UnreachableTerminator>) {
                // BRK is a deliberate trap, rather than an accidental
                // fall-through into the next block or function.
                output += "\tbrk #0\n";
                return true;
            } else {
                context.report(location,
                    "unsupported AArch64 terminator");
                return false;
            }
        },
        terminator
    );
}

} // namespace cru::codegen
