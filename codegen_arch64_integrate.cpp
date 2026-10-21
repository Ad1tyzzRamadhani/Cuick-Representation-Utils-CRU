
#include "codegen_arch64.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>

namespace cru::codegen {
namespace {

using namespace ast;

void emitImmediate64(
    std::string& out,
    const std::string& reg,
    std::uint64_t value
) {
    bool first = true;

    for (unsigned shift = 0; shift < 64; shift += 16) {
        const auto part = static_cast<unsigned>(
            (value >> shift) & 0xffffu
        );

        if (part == 0 && !first)
            continue;

        out += first ? "\tmovz " : "\tmovk ";
        out += reg + ", #" + std::to_string(part);

        if (shift != 0)
            out += ", lsl #" + std::to_string(shift);

        out += "\n";
        first = false;
    }

    if (first)
        out += "\tmovz " + reg + ", #0\n";
}

void emitStackAllocation(
    std::string& out,
    std::uint64_t size
) {
    if (size == 0)
        return;

    if (size <= 4095) {
        out += "\tsub sp, sp, #" + std::to_string(size) + "\n";
    } else {
        emitImmediate64(out, "x16", size);
        out += "\tsub sp, sp, x16\n";
    }
}

bool emitFunctionParameters(
    AArch64Context& context,
    const Function& function,
    const FunctionFrame& frame,
    std::string& out
) {
    unsigned nextGpr = 0;
    unsigned nextFpr = 0;
    std::uint64_t stackOffset = 16;

    for (const auto& parameter : function.header.params) {
        const Type& type = parameter.type;

        const auto slotIt =
            frame.parameterSlots.find(normalizeName(parameter.name));

        if (slotIt == frame.parameterSlots.end()) {
            context.report(
                parameter.location,
                "missing parameter home slot: " + parameter.name
            );
            return false;
        }

        const StackSlot& slot = slotIt->second;
        const bool floating =
            type.kind == TypeKind::F32 ||
            type.kind == TypeKind::F64;

        std::string source;

        if (floating && nextFpr < 8) {
            source = std::string(
                type.kind == TypeKind::F32 ? "s" : "d"
            ) + std::to_string(nextFpr++);
        } else if (!floating && nextGpr < 8) {
            source = "x" + std::to_string(nextGpr++);
        } else {
            // Stack arguments are located above the saved frame record.
            emitImmediate64(out, "x16", stackOffset);
            out += "\tadd x16, x29, x16\n";

            if (floating) {
                source = type.kind == TypeKind::F32 ? "s16" : "d16";
                out += "\tldr " + source + ", [x16]\n";
            } else {
                out += "\tldr x17, [x16]\n";
            }

            stackOffset += 8;
        }

        emitImmediate64(out, "x16", slot.offset);
        out += "\tsub x16, x29, x16\n";

        if (floating) {
            out += "\tstr " + source + ", [x16]\n";
        } else {
            if (source != "x17")
                out += "\tmov x17, " + source + "\n";

            out += "\tstr x17, [x16]\n";
        }
    }

    return true;
}

bool emitFunction(
    AArch64Context& context,
    const Function& function
) {
    auto frame = buildFunctionFrame(context, function);
    if (!frame)
        return false;

    std::string body;

    body += "\t.text\n";
    body += "\t.balign 4\n";
    body += "\t.global " + function.header.name + "\n";
    body += "\t.type " + function.header.name + ", %function\n";
    body += function.header.name + ":\n";

    // Standard AAPCS64 frame record.
    body += "\tstp x29, x30, [sp, #-16]!\n";
    body += "\tmov x29, sp\n";

    emitStackAllocation(body, frame->frameSize);

    if (!emitFunctionParameters(
            context, function, *frame, body)) {
        return false;
    }

    for (const auto& block : function.blocks) {
        body += aarch64BlockLabel(function, block.name) + ":\n";

        for (const auto& instruction : block.instructions) {
            if (!emitAArch64Instruction(
                    context, *frame, instruction,
                    body, block.location)) {
                return false;
            }
        }

        if (!block.terminator.has_value()) {
            context.report(
            block.location,
            "basic block tidak memiliki terminator"
            );
            return false;
        }

        if (!emitAArch64Terminator(
                context, *frame, function, block.terminator,
                body, *block.location)) {
            return false;
        }
    }

    body += "\t.size " + function.header.name + ", .-" +
            function.header.name + "\n";

    context.output += body;
    return true;
}

} // namespace

CodegenResult codegenArch64(
    const ast::Module& module,
    const std::string& filename
) {
    AArch64Context context(module);

    if (!context.indexModule()) {
        return {
            false,
            {},
            std::move(context.diagnostics)
        };
    }

    // Emit global objects and external symbol declarations first.
    if (!context.emitGlobals()) {
        return {
            false,
            {},
            std::move(context.diagnostics)
        };
    }

    // Emit function definitions.
    for (const auto& declaration : module.declarations) {
        const auto* function =
            std::get_if<ast::Function>(&declaration);

        if (!function)
            continue;

        if (!emitFunction(context, *function)) {
            return {
                false,
                {},
                std::move(context.diagnostics)
            };
        }
    }

    if (!context.diagnostics.empty()) {
        return {
            false,
            {},
            std::move(context.diagnostics)
        };
    }

    // Write only after successful code generation.
    std::ofstream file(filename, std::ios::binary);

    if (!file) {
        context.report(
            {},
            "cannot open assembly output file: " + filename
        );

        return {
            false,
            {},
            std::move(context.diagnostics)
        };
    }

    file << context.output;
    file.close();

    if (!file) {
        context.report(
            {},
            "failed to write assembly output file: " + filename
        );

        return {
            false,
            {},
            std::move(context.diagnostics)
        };
    }

    return {
        true,
        std::move(context.output),
        std::move(context.diagnostics)
    };
}

} // namespace cru::codegen
