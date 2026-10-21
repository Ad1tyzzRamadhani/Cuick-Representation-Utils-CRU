#include "codegen_arch64.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cru::codegen {
namespace {

using namespace ast;

bool isFloat(const Type& type) {
    return type.kind == TypeKind::F32 ||
           type.kind == TypeKind::F64;
}

bool is32Bit(const Type& type) {
    switch (type.kind) {
    case TypeKind::I8:
    case TypeKind::I16:
    case TypeKind::I32:
    case TypeKind::U8:
    case TypeKind::U16:
    case TypeKind::U32:
    case TypeKind::Bool:
    case TypeKind::F32:
        return true;
    default:
        return false;
    }
}

std::string integerReg(unsigned index, const Type& type) {
    return std::string(is32Bit(type) ? "w" : "x") +
           std::to_string(index);
}

std::string floatReg(unsigned index, const Type& type) {
    return std::string(type.kind == TypeKind::F32 ? "s" : "d") +
           std::to_string(index);
}

void emitImmediate(
    std::string& out,
    const std::string& reg,
    std::uint64_t value
) {
    bool emitted = false;

    for (unsigned shift = 0; shift < 64; shift += 16) {
        const auto part =
            static_cast<unsigned>((value >> shift) & 0xffffu);

        if (part == 0 && (emitted || shift != 0))
            continue;

        if (!emitted) {
            out += "    movz " + reg + ", #" +
                   std::to_string(part);

            if (shift != 0)
                out += ", lsl #" + std::to_string(shift);

            out += "\n";
            emitted = true;
        } else {
            out += "    movk " + reg + ", #" +
                   std::to_string(part) +
                   ", lsl #" + std::to_string(shift) + "\n";
        }
    }
}

void emitSlotAddress(
    std::string& out,
    std::uint64_t offset,
    const std::string& addressReg
) {
    // Frame slots are addressed relative to x29:
    // slot address = x29 - offset.
    if (offset <= 4095) {
        out += "    sub " + addressReg +
               ", x29, #" + std::to_string(offset) + "\n";
    } else {
        emitImmediate(out, addressReg, offset);
        out += "    sub " + addressReg +
               ", x29, " + addressReg + "\n";
    }
}

const StackSlot* findSlot(
    const FunctionFrame& frame,
    const std::string& name
) {
    if (auto it = frame.valueSlots.find(name);
        it != frame.valueSlots.end())
        return &it->second;

    if (auto it = frame.parameterSlots.find(name);
        it != frame.parameterSlots.end())
        return &it->second;

    return nullptr;
}

bool emitOperand(
    AArch64Context& context,
    const FunctionFrame& frame,
    const Operand& operand,
    const Type& type,
    const std::string& destination,
    std::string& out,
    const SourceLocation& location
) {
    if (const auto* ref = std::get_if<ResultRef>(&operand)) {
        const auto* slot = findSlot(frame, ref->name);

        if (!slot) {
            context.report(location,
                "ABI: no stack slot for value '" + ref->name + "'");
            return false;
        }

        emitSlotAddress(out, slot->offset, "x17");

        out += "    ldr " + destination +
               ", [x17]\n";
        return true;
    }

    const auto* literal = std::get_if<Literal>(&operand);
    if (!literal) {
        context.report(location, "ABI: unsupported operand");
        return false;
    }

    if (isFloat(type)) {
        char* end = nullptr;
        const double value =
            std::strtod(literal->text.c_str(), &end);

        if (end == literal->text.c_str() || *end != '\0') {
            context.report(location,
                "ABI: invalid floating-point literal '" +
                literal->text + "'");
            return false;
        }

        if (type.kind == TypeKind::F32) {
            const float f = static_cast<float>(value);
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));

            emitImmediate(out, "w16", bits);
            out += "    fmov " + destination + ", w16\n";
        } else {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));

            emitImmediate(out, "x16", bits);
            out += "    fmov " + destination + ", x16\n";
        }

        return true;
    }

    std::uint64_t value = 0;

    switch (literal->kind) {
    case LiteralKind::Boolean:
        value = literal->text == "true" ? 1 : 0;
        break;

    case LiteralKind::Null:
        value = 0;
        break;

    case LiteralKind::Integer: {
        char* end = nullptr;
        const long long parsed =
            std::strtoll(literal->text.c_str(), &end, 0);

        if (end == literal->text.c_str() || *end != '\0') {
            context.report(location,
                "ABI: invalid integer literal '" +
                literal->text + "'");
            return false;
        }

        value = static_cast<std::uint64_t>(parsed);
        break;
    }

    case LiteralKind::Float:
        context.report(location,
            "ABI: floating-point literal used with integer type");
        return false;
    }

    emitImmediate(out, destination, value);
    return true;
}

struct ArgumentLocation {
    enum class Kind { GPR, FPR, Stack };

    Kind kind = Kind::Stack;
    unsigned reg = 0;
    std::uint64_t stackOffset = 0;
};

} // namespace

bool emitAArch64Call(
    AArch64Context& context,
    const FunctionFrame& frame,
    const std::vector<ast::Operand>& arguments,
    const std::vector<const ast::Type*>& argumentTypes,
    const ast::Type& returnType,
    const std::optional<ast::ResultRef>& result,
    const std::string* directSymbol,
    const ast::Operand* indirectTarget,
    std::string& out,
    const ast::SourceLocation& location
) {
    if ((directSymbol == nullptr) == (indirectTarget == nullptr)) {
        context.report(location,
            "ABI: specify exactly one call target");
        return false;
    }

    if (arguments.size() != argumentTypes.size()) {
        context.report(location,
            "ABI: argument count and type count do not match");
        return false;
    }

    for (const auto* type : argumentTypes) {
        if (type == nullptr) {
            context.report(location, "ABI: missing argument type");
            return false;
        }

        if (type->kind == ast::TypeKind::Void ||
            type->kind == ast::TypeKind::Array ||
            type->kind == ast::TypeKind::Named ||
            type->kind == ast::TypeKind::ISize ||
            type->kind == ast::TypeKind::USize) {
            // Named and aggregate arguments need ABI classification
            // based on their resolved layouts; this scalar path
            // intentionally rejects them for now.
            context.report(location,
                "ABI: argument type is not supported by scalar ABI "
                "lowering yet");
            return false;
        }
    }

    std::vector<ArgumentLocation> locations(arguments.size());
    unsigned nextGpr = 0;
    unsigned nextFpr = 0;
    std::uint64_t stackBytes = 0;

    // Classify each scalar argument. GPR and FPR counters are
    // independent under AAPCS64.
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const Type& type = *argumentTypes[i];

        if (isFloat(type) && nextFpr < 8) {
            locations[i] = {
                ArgumentLocation::Kind::FPR, nextFpr++, 0
            };
        } else if (!isFloat(type) && nextGpr < 8) {
            locations[i] = {
                ArgumentLocation::Kind::GPR, nextGpr++, 0
            };
        } else {
            // This initial implementation reserves an 8-byte
            // slot for each scalar stack argument.
            stackBytes = (stackBytes + 7u) & ~std::uint64_t{7u};
            locations[i] = {
                ArgumentLocation::Kind::Stack, 0, stackBytes
            };
            stackBytes += 8;
        }
    }

    const std::uint64_t outgoingSize =
        (stackBytes + 15u) & ~std::uint64_t{15u};

    if (outgoingSize != 0) {
        if (outgoingSize <= 4095) {
            out += "    sub sp, sp, #" +
                   std::to_string(outgoingSize) + "\n";
        } else {
            emitImmediate(out, "x16", outgoingSize);
            out += "    sub sp, sp, x16\n";
        }
    }

    // First, place stack arguments. Register arguments are prepared
    // afterwards so temporary loads cannot overwrite their values.
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& loc = locations[i];
        if (loc.kind != ArgumentLocation::Kind::Stack)
            continue;

        const Type& type = *argumentTypes[i];
        const std::string source =
            isFloat(type) ? floatReg(16, type) : integerReg(16, type);

        if (!emitOperand(context, frame, arguments[i],
                         type, source, out, location))
            return false;

        if (loc.stackOffset <= 32760) {
            const std::string storeReg =
                isFloat(type) ? source : "x16";

            out += "    str " + storeReg +
                   ", [sp, #" +
                   std::to_string(loc.stackOffset) + "]\n";
        } else {
            emitImmediate(out, "x17", loc.stackOffset);
            out += "    add x17, sp, x17\n";
            out += "    str " +
                   (isFloat(type) ? source : "x16") +
                   ", [x17]\n";
        }
    }

    // Then load register arguments into x0-x7 or s0-s7/d0-d7.
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& loc = locations[i];
        if (loc.kind == ArgumentLocation::Kind::Stack)
            continue;

        const Type& type = *argumentTypes[i];
        std::string destination;

        if (loc.kind == ArgumentLocation::Kind::FPR)
            destination = floatReg(loc.reg, type);
        else
            destination = integerReg(loc.reg, type);

        if (!emitOperand(context, frame, arguments[i],
                         type, destination, out, location))
            return false;
    }

    if (directSymbol) {
        out += "    bl " + *directSymbol + "\n";
    } else {
        // Load the indirect target only after argument registers
        // have been prepared.
        Type pointerType;
        pointerType.kind = ast::TypeKind::Ptr;

        if (!emitOperand(context, frame, *indirectTarget,
                         pointerType, "x17", out, location))
            return false;

        out += "    blr x17\n";
    }

    if (outgoingSize != 0) {
        if (outgoingSize <= 4095) {
            out += "    add sp, sp, #" +
                   std::to_string(outgoingSize) + "\n";
        } else {
            emitImmediate(out, "x16", outgoingSize);
            out += "    add sp, sp, x16\n";
        }
    }

    if (result) {
        if (returnType.kind == ast::TypeKind::Void) {
            context.report(location,
                "ABI: void call cannot produce a result");
            return false;
        }

        const auto* slot = findSlot(frame, result->name);
        if (!slot) {
            context.report(location,
                "ABI: no result slot for '" + result->name + "'");
            return false;
        }

        emitSlotAddress(out, slot->offset, "x17");

        if (isFloat(returnType)) {
            out += "    str " +
                   floatReg(0, returnType) +
                   ", [x17]\n";
        } else {
            out += "    str " +
                   integerReg(0, returnType) +
                   ", [x17]\n";
        }
    }

    return true;
}

} // namespace cru::codegen
