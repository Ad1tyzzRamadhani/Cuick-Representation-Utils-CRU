
#ifndef CRU_CODEGEN_ARCH64_HPP
#define CRU_CODEGEN_ARCH64_HPP

#include "ast.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace cru::codegen {

struct Diagnostic {
    ast::SourceLocation location;
    std::string message;
};

struct TypeLayout {
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
};

struct FieldLayout {
    std::uint64_t offset = 0;
    TypeLayout type;
};

struct StructLayout {
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
    std::vector<FieldLayout> fields;
};

struct AArch64Context {
    const ast::Module* module = nullptr;

    std::unordered_map<std::string, const ast::StructDecl*> structs;
    std::unordered_map<std::string, const ast::UnionDecl*> unions;

    std::unordered_map<std::string, StructLayout> structLayouts;
    std::unordered_map<std::string, StructLayout> unionLayouts;

    std::unordered_set<std::string> layoutInProgress;
    std::unordered_set<std::string> globalNames;
    std::unordered_set<std::string> externalNames;

    std::vector<Diagnostic> diagnostics;
    std::string output;

    explicit AArch64Context(const ast::Module& source)
        : module(&source) {}

    void report(
        const ast::SourceLocation& location,
        std::string message
    );

    bool indexModule();

    std::optional<TypeLayout> typeLayout(
        const ast::Type& type,
        const ast::SourceLocation& location
    );

    std::optional<std::uint64_t> fieldOffset(
        const std::string& structName,
        std::uint64_t fieldIndex,
        const ast::SourceLocation& location
    );

    bool emitGlobals();
};


struct StackSlot {
    // Offset relatif terhadap x29; slot berada di [x29 - offset].
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
};

struct FunctionFrame {
    // Ukuran alokasi setelah x29 dibentuk, dibulatkan ke 16 byte.
    std::uint64_t frameSize = 0;

    // Slot untuk nilai hasil instruksi SSA.
    std::unordered_map<std::string, StackSlot> valueSlots;

    // Storage objek yang dibuat oleh AllocaInst.
    std::unordered_map<std::string, StackSlot> allocaObjects;

    // Home slot untuk parameter fungsi.
    std::unordered_map<std::string, StackSlot> parameterSlots;

    
std::unordered_map<std::string, ast::Type> valueTypes;

};

bool emitAArch64Call(
    AArch64Context& context,
    const FunctionFrame& frame,
    const std::vector<ast::Operand>& arguments,
    const std::vector<ast::Type>& argumentTypes,
    const ast::Type& returnType,
    const std::optional<ast::ResultRef>& result,
    const std::string* directSymbol,
    const ast::Operand* indirectTarget,
    std::string& out,
    const ast::SourceLocation& location
);

std::optional<FunctionFrame> buildFunctionFrame(
    AArch64Context& context,
    const ast::Function& function
);

bool emitAArch64Instruction(
    AArch64Context& context,
    const FunctionFrame& frame,
    const ast::Instruction& instruction,
    std::string& output,
    const ast::SourceLocation& location
);


std::string aarch64BlockLabel(
    const ast::Function& function,
    const std::string& blockName
);

bool emitAArch64Terminator(
    AArch64Context& context,
    const FunctionFrame& frame,
    const ast::Function& function,
    const ast::Terminator& terminator,
    std::string& output,
    const ast::SourceLocation& location
);



} // namespace cru::codegen

#endif
