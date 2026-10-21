
#include "codegen_arch64.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace cru::codegen {
namespace {

using namespace ast;

std::string keyOf(std::string name) {
    if (!name.empty() && name.front() == '%')
        name.erase(name.begin());
    return name;
}

bool isFloat(const Type& type) {
    return type.kind == TypeKind::F32 ||
           type.kind == TypeKind::F64;
}

bool isUnsigned(const Type& type) {
    return type.kind == TypeKind::U8 ||
           type.kind == TypeKind::U16 ||
           type.kind == TypeKind::U32 ||
           type.kind == TypeKind::U64 ||
           type.kind == TypeKind::USize ||
           type.kind == TypeKind::Bool ||
           type.kind == TypeKind::Ptr;
}

std::uint64_t bitWidth(const Type& type) {
    switch (type.kind) {
    case TypeKind::I8:
    case TypeKind::U8:
    case TypeKind::Bool: return 8;
    case TypeKind::I16:
    case TypeKind::U16: return 16;
    case TypeKind::I32:
    case TypeKind::U32:
    case TypeKind::F32: return 32;
    case TypeKind::I64:
    case TypeKind::U64:
    case TypeKind::ISize:
    case TypeKind::USize:
    case TypeKind::F64:
    case TypeKind::Ptr: return 64;
    default: return 0;
    }
}

std::string xreg(unsigned index) {
    return "x" + std::to_string(index);
}

std::string wreg(unsigned index) {
    return "w" + std::to_string(index);
}

std::string freg(unsigned index, const Type& type) {
    return std::string(type.kind == TypeKind::F32 ? "s" : "d") +
           std::to_string(index);
}

bool parseInteger(const Literal& literal, std::uint64_t& result) {
    if (literal.kind == LiteralKind::Null) {
        result = 0;
        return true;
    }

    if (literal.kind == LiteralKind::Boolean) {
        if (literal.text == "true") {
            result = 1;
            return true;
        }
        if (literal.text == "false") {
            result = 0;
            return true;
        }
        return false;
    }

    if (literal.kind != LiteralKind::Integer)
        return false;

    errno = 0;
    char* end = nullptr;
    const auto signedValue = std::strtoll(
        literal.text.c_str(), &end, 0
    );

    if (errno == ERANGE || end == literal.text.c_str() || *end != '\0')
        return false;

    result = static_cast<std::uint64_t>(signedValue);
    return true;
}

std::optional<std::uint64_t> parseFloatBits(
    const Literal& literal,
    const Type& type
) {
    if (literal.kind != LiteralKind::Float &&
        literal.kind != LiteralKind::Integer) {
        return std::nullopt;
    }

    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(literal.text.c_str(), &end);

    if (errno == ERANGE || end == literal.text.c_str() || *end != '\0')
        return std::nullopt;

    if (type.kind == TypeKind::F32) {
        const float f = static_cast<float>(value);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    }

    if (type.kind == TypeKind::F64) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    return std::nullopt;
}

// Materialisasi immediate 64-bit menggunakan movz/movk.
void emitImmediate(
    std::string& out,
    const std::string& reg,
    std::uint64_t value
) {
    bool first = true;

    for (unsigned shift = 0; shift < 64; shift += 16) {
        const auto part =
            static_cast<unsigned>((value >> shift) & 0xffffu);

        if (part == 0 && !first)
            continue;

        out += "\t";
        out += first ? "movz " : "movk ";
        out += reg + ", #" + std::to_string(part);

        if (shift != 0)
            out += ", lsl #" + std::to_string(shift);

        out += "\n";
        first = false;
    }

    if (first)
        out += "\tmovz " + reg + ", #0\n";
}

void emitAddressFromFrame(
    std::string& out,
    const std::string& reg,
    std::uint64_t offset
) {
    emitImmediate(out, "x16", offset);
    out += "\tsub " + reg + ", x29, x16\n";
}

void emitLoadAtOffset(
    std::string& out,
    const std::string& reg,
    const std::string& base,
    std::uint64_t offset,
    const Type& type
) {
    const auto width = bitWidth(type);
    const bool fp = isFloat(type);

    if (offset <= 32760 && offset % (width / 8) == 0) {
        const auto opcode = fp
            ? (type.kind == TypeKind::F32 ? "ldr" : "ldr")
            : (width == 8 ? "ldr" : "ldr");

        out += "\t" + std::string(opcode) + " " + reg + ", [" +
               base + ", #" + std::to_string(offset) + "]\n";
        return;
    }

    emitImmediate(out, "x16", offset);
    out += "\tadd x16, " + base + ", x16\n";
    out += "\tldr " + reg + ", [x16]\n";
}

void emitStoreAtOffset(
    std::string& out,
    const std::string& reg,
    const std::string& base,
    std::uint64_t offset,
    const Type& type
) {
    const auto width = bitWidth(type);

    if (offset <= 32760 && width != 0 &&
        offset % (width / 8) == 0) {
        out += "\tstr " + reg + ", [" + base + ", #" +
               std::to_string(offset) + "]\n";
        return;
    }

    emitImmediate(out, "x16", offset);
    out += "\tadd x16, " + base + ", x16\n";
    out += "\tstr " + reg + ", [x16]\n";
}

const StackSlot* findSlot(
    const std::unordered_map<std::string, StackSlot>& slots,
    const std::string& name
) {
    auto it = slots.find(keyOf(name));
    return it == slots.end() ? nullptr : &it->second;
}

const Type* findValueType(
    const FunctionFrame& frame,
    const std::string& name
) {
    auto it = frame.valueTypes.find(keyOf(name));
    return it == frame.valueTypes.end() ? nullptr : &it->second;
}

bool emitLoadOperand(
    AArch64Context& context,
    const FunctionFrame& frame,
    const Operand& operand,
    const Type& type,
    const std::string& integerReg,
    const std::string& floatReg,
    std::string& out,
    const SourceLocation& location
) {
    if (const auto* literal = std::get_if<Literal>(&operand)) {
        if (isFloat(type)) {
            auto bits = parseFloatBits(*literal, type);
            if (!bits) {
                context.report(location,
                    "invalid floating-point operand");
                return false;
            }

            emitImmediate(out, "x16", *bits);

            if (type.kind == TypeKind::F32)
                out += "\tfmov " + floatReg + ", w16\n";
            else
                out += "\tfmov " + floatReg + ", x16\n";

            return true;
        }

        std::uint64_t value = 0;
        if (!parseInteger(*literal, value)) {
            context.report(location, "invalid integer operand");
            return false;
        }

        emitImmediate(out, integerReg, value);
        return true;
    }

    const auto& ref = std::get<ResultRef>(operand);
    const auto* slot = findSlot(frame.valueSlots, ref.name);

    if (!slot) {
        context.report(location,
            "no stack slot for value: " + ref.name);
        return false;
    }

    const auto* sourceType = findValueType(frame, ref.name);
    const Type& effectiveType = sourceType ? *sourceType : type;

    const std::string reg = isFloat(type)
        ? floatReg
        : integerReg;

    emitLoadAtOffset(
        out, reg, "x29", slot->offset,
        isFloat(effectiveType) ? effectiveType : type
    );

    return true;
}

bool emitStoreResult(
    AArch64Context& context,
    const FunctionFrame& frame,
    const ResultRef& result,
    const Type& type,
    const std::string& reg,
    std::string& out,
    const SourceLocation& location
) {
    const auto* slot = findSlot(frame.valueSlots, result.name);

    if (!slot) {
        context.report(location,
            "no result slot for: " + result.name);
        return false;
    }

    emitStoreAtOffset(
        out, reg, "x29", slot->offset, type
    );
    return true;
}

std::string integerMnemonic(BinaryOp op, bool wide) {
    switch (op) {
    case BinaryOp::Add: return "add";
    case BinaryOp::Sub: return "sub";
    case BinaryOp::Mul: return "mul";
    case BinaryOp::Div: return "sdiv";
    case BinaryOp::Rem: return "srem";
    case BinaryOp::And: return "and";
    case BinaryOp::Or:  return "orr";
    case BinaryOp::Xor: return "eor";
    case BinaryOp::Shl: return "lsl";
    case BinaryOp::Shr: return wide ? "asr" : "asr";
    }
    return {};
}

std::string floatMnemonic(FloatBinaryOp op) {
    switch (op) {
    case FloatBinaryOp::Add: return "fadd";
    case FloatBinaryOp::Sub: return "fsub";
    case FloatBinaryOp::Mul: return "fmul";
    case FloatBinaryOp::Div: return "fdiv";
    case FloatBinaryOp::Rem: return {};
    }
    return {};
}

std::string conditionCode(CompareOp op) {
    switch (op) {
    case CompareOp::Eq: return "eq";
    case CompareOp::Ne: return "ne";
    case CompareOp::Lt: return "lt";
    case CompareOp::Le: return "le";
    case CompareOp::Gt: return "gt";
    case CompareOp::Ge: return "ge";
    }
    return "eq";
}


bool emitAArch64Instruction(
    AArch64Context& context,
    const FunctionFrame& frame,
    const ast::Instruction& instruction,
    std::string& output,
    const ast::SourceLocation& location
) {
    using namespace ast;

    auto emitIntegerBinary = [&](const BinaryInst& inst) -> bool {
        const auto width = bitWidth(inst.type);
        if (width == 0 || isFloat(inst.type)) {
            context.report(location,
                "integer binary operation requires an integer type");
            return false;
        }

        const auto reg = width == 64 ? xreg : wreg;
        const std::string r0 = reg(9);
        const std::string r1 = reg(10);
        const std::string r2 = reg(11);

        if (!emitLoadOperand(context, frame, inst.lhs, inst.type,
                             r0, "d0", output, location) ||
            !emitLoadOperand(context, frame, inst.rhs, inst.type,
                             r1, "d1", output, location)) {
            return false;
        }

        const bool uns = isUnsigned(inst.type);
        std::string opcode;

        switch (inst.op) {
        case BinaryOp::Add: opcode = "add"; break;
        case BinaryOp::Sub: opcode = "sub"; break;
        case BinaryOp::Mul: opcode = "mul"; break;
        case BinaryOp::Div: opcode = uns ? "udiv" : "sdiv"; break;
        case BinaryOp::Rem: {
            output += "\t" + std::string(uns ? "udiv " : "sdiv ") +
                      r2 + ", " + r0 + ", " + r1 + "\n";
            output += "\tmsub " + r2 + ", " + r2 + ", " + r1 +
                      ", " + r0 + "\n";
            return emitStoreResult(
                context, frame, inst.result, inst.type, r2, output, location
            );
        }
        case BinaryOp::And: opcode = "and"; break;
        case BinaryOp::Or:  opcode = "orr"; break;
        case BinaryOp::Xor: opcode = "eor"; break;
        case BinaryOp::Shl: opcode = "lsl"; break;
        case BinaryOp::Shr: opcode = uns ? "lsr" : "asr"; break;
        }

        output += "\t" + opcode + " " + r2 + ", " + r0 + ", " + r1 + "\n";
        return emitStoreResult(
            context, frame, inst.result, inst.type, r2, output, location
        );
    };

    auto emitCompare = [&](const auto& inst, bool floating) -> bool {
        const Type& type = inst.type;

        if (floating != isFloat(type)) {
            context.report(location,
                "comparison type does not match its instruction kind");
            return false;
        }

        const std::string lhs = floating ? "d0" : "x9";
        const std::string rhs = floating ? "d1" : "x10";

        if (!emitLoadOperand(context, frame, inst.lhs, type,
                             "x9", "d0", output, location) ||
            !emitLoadOperand(context, frame, inst.rhs, type,
                             "x10", "d1", output, location)) {
            return false;
        }

        if (floating) {
            const std::string fpType =
                type.kind == TypeKind::F32 ? "s" : "d";

            output += "\tfcmp " + fpType + "0, " + fpType + "1\n";
        } else {
            output += "\tcmp " + lhs + ", " + rhs + "\n";
        }

        std::string cc = conditionCode(inst.op);

        if (!floating && isUnsigned(type)) {
            switch (inst.op) {
            case CompareOp::Lt: cc = "lo"; break;
            case CompareOp::Le: cc = "ls"; break;
            case CompareOp::Gt: cc = "hi"; break;
            case CompareOp::Ge: cc = "hs"; break;
            default: break;
            }
        }

        // AArch64 floating-point comparisons need ordered/unordered-aware
        // conditions. The basic conditions below handle ordinary finite
        // values; NaN semantics require an explicit language rule.
        output += "\tcset w11, " + cc + "\n";

        Type boolType;
        boolType.kind = TypeKind::Bool;

        return emitStoreResult(
            context, frame, inst.result, boolType, "x11", output, location
        );
    };

    return std::visit(
        [&](const auto& inst) -> bool {
            using T = std::decay_t<decltype(inst)>;

            if constexpr (std::is_same_v<T, ConstantInst>) {
                Operand operand = inst.value;

                if (!emitLoadOperand(context, frame, operand, inst.type,
                                     "x9", "d0", output, location))
                    return false;

                return emitStoreResult(
                    context, frame, inst.result, inst.type,
                    isFloat(inst.type) ? freg(0, inst.type) : "x9",
                    output, location
                );
            } else if constexpr (std::is_same_v<T, UnaryInst>) {
                if (isFloat(inst.type)) {
                    if (inst.op != UnaryOp::Neg) {
                        context.report(location,
                            "bitwise/logical unary operation on float is unsupported");
                        return false;
                    }

                    if (!emitLoadOperand(context, frame, inst.operand,
                                         inst.type, "x9", "d0",
                                         output, location))
                        return false;

                    const auto fp = freg(0, inst.type);
                    const auto result = freg(1, inst.type);
                    output += "\tfneg " + result + ", " + fp + "\n";

                    return emitStoreResult(
                        context, frame, inst.result, inst.type,
                        result, output, location
                    );
                }

                const auto width = bitWidth(inst.type);
                if (width == 0) {
                    context.report(location,
                        "unsupported type for unary operation");
                    return false;
                }

                const auto reg = width == 64 ? xreg : wreg;
                const auto r0 = reg(9);
                const auto r1 = reg(11);

                if (!emitLoadOperand(context, frame, inst.operand,
                                     inst.type, r0, "d0",
                                     output, location))
                    return false;

                switch (inst.op) {
                case UnaryOp::Neg:
                    output += "\tneg " + r1 + ", " + r0 + "\n";
                    break;
                case UnaryOp::BitNot:
                    output += "\tmvn " + r1 + ", " + r0 + "\n";
                    break;
                case UnaryOp::Not:
                    output += "\tcmp " + r0 + ", #0\n";
                    output += "\tcset " + r1 + ", eq\n";
                    break;
                }

                return emitStoreResult(
                    context, frame, inst.result, inst.type,
                    r1, output, location
                );
            } else if constexpr (std::is_same_v<T, BinaryInst>) {
                return emitIntegerBinary(inst);
            } else if constexpr (std::is_same_v<T, CompareInst>) {
                return emitCompare(inst, false);
            } else if constexpr (std::is_same_v<T, FloatCompareInst>) {
                return emitCompare(inst, true);
            } else if constexpr (std::is_same_v<T, FloatBinaryInst>) {
                if (inst.op == FloatBinaryOp::Rem) {
                    context.report(location,
                        "floating-point remainder needs a runtime helper");
                    return false;
                }

                if (!isFloat(inst.type)) {
                    context.report(location,
                        "floating-point instruction requires f32 or f64");
                    return false;
                }

                if (!emitLoadOperand(context, frame, inst.lhs, inst.type,
                                     "x9", "d0", output, location) ||
                    !emitLoadOperand(context, frame, inst.rhs, inst.type,
                                     "x10", "d1", output, location)) {
                    return false;
                }

                const auto fp = freg(0, inst.type);
                const auto fp1 = freg(1, inst.type);
                const auto fp2 = freg(2, inst.type);
                const auto opcode = floatMnemonic(inst.op);

                if (opcode.empty()) {
                    context.report(location,
                        "unsupported floating-point operation");
                    return false;
                }

                output += "\t" + opcode + " " + fp2 + ", " +
                          fp + ", " + fp1 + "\n";

                return emitStoreResult(
                    context, frame, inst.result, inst.type,
                    fp2, output, location
                );
            } else if constexpr (std::is_same_v<T, CastInst>) {
                const auto* resultType = &inst.targetType;
                const auto* sourceType = std::get_if<ResultRef>(&inst.operand)
                    ? findValueType(
                        frame, std::get<ResultRef>(inst.operand).name
                    )
                    : nullptr;

                Type inferredSource;
                if (!sourceType) {
                    if (const auto* literal =
                            std::get_if<Literal>(&inst.operand)) {
                        switch (literal->kind) {
                        case LiteralKind::Integer:
                            inferredSource.kind = TypeKind::I64;
                            break;
                        case LiteralKind::Float:
                            inferredSource.kind = TypeKind::F64;
                            break;
                        case LiteralKind::Boolean:
                            inferredSource.kind = TypeKind::Bool;
                            break;
                        case LiteralKind::Null:
                            inferredSource.kind = TypeKind::Ptr;
                            break;
                        }
                        sourceType = &inferredSource;
                    }
                }

                if (!sourceType) {
                    context.report(location,
                        "cannot determine source type for cast");
                    return false;
                }

                const Type& src = *sourceType;
                const Type& dst = *resultType;

                if (!emitLoadOperand(context, frame, inst.operand, src,
                                     "x9", "d0", output, location))
                    return false;

                if (isFloat(src) && isFloat(dst)) {
                    output += "\tfcvt " + freg(1, dst) + ", " +
                              freg(0, src) + "\n";
                    return emitStoreResult(
                        context, frame, inst.result, dst,
                        freg(1, dst), output, location
                    );
                }

                if (isFloat(src) && !isFloat(dst)) {
                    const auto dstReg = bitWidth(dst) == 32 ? "w11" : "x11";
                    output += "\tfcvtzs " + dstReg + ", " +
                              freg(0, src) + "\n";
                    return emitStoreResult(
                        context, frame, inst.result, dst,
                        dstReg, output, location
                    );
                }

                if (!isFloat(src) && isFloat(dst)) {
                    const auto srcReg = bitWidth(src) == 32 ? "w9" : "x9";
                    output += "\t" +
                        std::string(isUnsigned(src) ? "ucvtf " : "scvtf ") +
                        freg(1, dst) + ", " + srcReg + "\n";
                    return emitStoreResult(
                        context, frame, inst.result, dst,
                        freg(1, dst), output, location
                    );
                }

                const auto dstWidth = bitWidth(dst);
                const auto srcWidth = bitWidth(src);

                if (dstWidth == 0 || srcWidth == 0) {
                    context.report(location, "unsupported cast type");
                    return false;
                }

                const auto dstReg = dstWidth == 32 ? "w11" : "x11";
                const auto srcReg = srcWidth == 32 ? "w9" : "x9";

                if (dstWidth < srcWidth) {
                    output += "\t" + std::string(dstWidth == 32 ? "mov w11, w9\n" :
                                                                  "mov x11, x9\n");
                } else if (dstWidth > srcWidth) {
                    if (isUnsigned(src) || src.kind == TypeKind::Bool) {
                        output += "\tuxt" + std::to_string(dstWidth) +
                                  " " + dstReg + ", " + srcReg + "\n";
                    } else {
                        output += "\tsxt" + std::to_string(dstWidth) +
                                  " " + dstReg + ", " + srcReg + "\n";
                    }
                } else {
                    output += "\tmov " + dstReg + ", " + srcReg + "\n";
                }

                return emitStoreResult(
                    context, frame, inst.result, dst,
                    dstReg, output, location
                );
            } else if constexpr (std::is_same_v<T, AllocaInst>) {
                const auto* resultSlot =
                    findSlot(frame.valueSlots, inst.result.name);
                const auto* objectSlot =
                    findSlot(frame.allocaObjects, inst.result.name);

                if (!resultSlot || !objectSlot) {
                    context.report(location,
                        "missing frame allocation for alloca result");
                    return false;
                }

                emitAddressFromFrame(output, "x9", objectSlot->offset);

                Type pointerType;
                pointerType.kind = TypeKind::Ptr;

                emitStoreAtOffset(
                    output, "x9", "x29", resultSlot->offset, pointerType
                );
                return true;
            } else if constexpr (std::is_same_v<T, LoadInst>) {
                if (!emitLoadOperand(context, frame, inst.address,
                                     Type{TypeKind::Ptr}, "x9", "d0",
                                     output, location))
                    return false;

                const auto width = bitWidth(inst.type);
                if (width == 0) {
                    context.report(location,
                        "load of non-scalar type is unsupported");
                    return false;
                }

                const auto valueReg = isFloat(inst.type)
                    ? freg(0, inst.type)
                    : (width == 32 ? "w10" : "x10");

                output += "\tldr " + valueReg + ", [x9]\n";

                return emitStoreResult(
                    context, frame, inst.result, inst.type,
                    valueReg, output, location
                );
            } else if constexpr (std::is_same_v<T, StoreInst>) {
                if (!emitLoadOperand(context, frame, inst.address,
                                     Type{TypeKind::Ptr}, "x9", "d0",
                                     output, location) ||
                    !emitLoadOperand(context, frame, inst.value, inst.type,
                                     "x10", "d1", output, location)) {
                    return false;
                }

                if (bitWidth(inst.type) == 0) {
                    context.report(location,
                        "store of non-scalar type is unsupported");
                    return false;
                }

                const auto valueReg = isFloat(inst.type)
                    ? freg(1, inst.type)
                    : (bitWidth(inst.type) == 32 ? "w10" : "x10");

                output += "\tstr " + valueReg + ", [x9]\n";
                return true;
            } else if constexpr (std::is_same_v<T, GlobalAddrInst>) {
                if (inst.globalName.empty()) {
                    context.report(location, "empty global symbol name");
                    return false;
                }

                output += "\tadrp x9, " + inst.globalName + "\n";
                output += "\tadd x9, x9, :lo12:" +
                          inst.globalName + "\n";

                Type pointerType;
                pointerType.kind = TypeKind::Ptr;

                return emitStoreResult(
                    context, frame, inst.result, pointerType,
                    "x9", output, location
                );
            } else if constexpr (std::is_same_v<T, FieldAddrInst>) {
                Type pointerType;
                pointerType.kind = TypeKind::Ptr;

                if (!emitLoadOperand(context, frame, inst.object,
                                     pointerType, "x9", "d0",
                                     output, location))
                    return false;

                auto offset = context.fieldOffset(
                    inst.structName, inst.fieldIndex, location
                );
                if (!offset)
                    return false;

                if (*offset == 0) {
                    output += "\tmov x10, x9\n";
                } else {
                    emitImmediate(output, "x10", *offset);
                    output += "\tadd x10, x9, x10\n";
                }

                return emitStoreResult(
                    context, frame, inst.result, pointerType,
                    "x10", output, location
                );
            } else if constexpr (std::is_same_v<T, ElementAddrInst>) {
                Type pointerType;
                pointerType.kind = TypeKind::Ptr;

                if (!emitLoadOperand(context, frame, inst.base,
                                     pointerType, "x9", "d0",
                                     output, location) ||
                    !emitLoadOperand(context, frame, inst.index,
                                     Type{TypeKind::USize}, "x10", "d1",
                                     output, location))
                    return false;

                auto element = context.typeLayout(
                    inst.elementType, location
                );
                if (!element)
                    return false;

                emitImmediate(output, "x11", element->size);
                output += "\tmul x10, x10, x11\n";
                output += "\tadd x10, x9, x10\n";

                return emitStoreResult(
                    context, frame, inst.result, pointerType,
                    "x10", output, location
                );
            } else if constexpr (std::is_same_v<T, CallInst>) {
                context.report(location,
                    "call emission belongs to codegen_arch64_abi.cpp");
                return false;
            } else if constexpr (std::is_same_v<T, AtomicLoadInst> ||
                                 std::is_same_v<T, AtomicStoreInst> ||
                                 std::is_same_v<T, AtomicRMWInst> ||
                                 std::is_same_v<T, AtomicCASInst>) {
                context.report(location,
                    "atomic instruction emission belongs to codegen_arch64_atomic.cpp");
                return false;
            } else {
                context.report(location,
                    "instruction is not handled by the AArch64 instruction emitter");
                return false;
            }
        },
        instruction
      }
} // namespace
