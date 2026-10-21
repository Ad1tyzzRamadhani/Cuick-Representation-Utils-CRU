#ifndef CRU_CODEGEN_HPP
#define CRU_CODEGEN_HPP

#include "ast.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cru::codegen {

enum class TargetArch {
    AArch64
};

enum class TargetABI {
    AAPCS64
};

enum class OutputFormat {
    Assembly
};

struct CodegenOptions {
    TargetArch target = TargetArch::AArch64;
    TargetABI abi = TargetABI::AAPCS64;
    OutputFormat format = OutputFormat::Assembly;

    // Linux AArch64 ELF, LP64.
    std::string targetTriple = "aarch64-unknown-linux-gnu";

    bool emitGNUStackNote = true;
};

struct Diagnostic {
    ast::SourceLocation location;
    std::string message;
};

struct CodegenResult {
    bool success = false;
    std::string output;
    std::vector<Diagnostic> diagnostics;
};

class CodegenBackend {
public:
    virtual ~CodegenBackend() = default;

    virtual CodegenResult generate(
        const ast::Module& module,
        const CodegenOptions& options
    ) = 0;
};

class CodegenSystem {
public:
    CodegenSystem() = default;

    CodegenResult generate(
        const ast::Module& module,
        const CodegenOptions& options = {}
    );

private:
    std::unique_ptr<CodegenBackend> createBackend(
        const CodegenOptions& options
    ) const;
};

} // namespace cru::codegen

#endif
