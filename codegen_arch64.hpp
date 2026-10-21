
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

} // namespace cru::codegen

#endif
