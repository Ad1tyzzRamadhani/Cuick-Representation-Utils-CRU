
#ifndef CRU_SEMANTIC_HPP
#define CRU_SEMANTIC_HPP

#include "ast.hpp"
#include "semantic_types.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cru::sem {

struct Diagnostic {
    ast::SourceLocation location;
    std::string message;
};

struct FunctionInfo {
    ast::Type returnType;
    std::vector<ast::Type> parameterTypes;
    bool defined = false;
};

struct AggregateInfo {
    bool isUnion = false;
    std::vector<ast::FieldDecl> fields;
};

class SemanticAnalyzer {
public:
    bool analyze(const ast::Module& module);

    const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

private:
    using ValueTable =
        std::unordered_map<std::string, ast::Type>;

    std::vector<Diagnostic> diagnostics_;

    std::unordered_map<std::string, AggregateInfo> aggregates_;
    std::unordered_map<std::string, ast::GlobalDecl> globals_;
    std::unordered_map<std::string, FunctionInfo> functions_;

    ValueTable values_;
    std::unordered_set<std::string> labels_;

    const ast::Function* currentFunction_ = nullptr;

    void report(
        const ast::SourceLocation& location,
        std::string message
    );

    void registerDeclarations(const ast::Module& module);
    void validateDeclarations(const ast::Module& module);
    void validateType(
        const ast::Type& type,
        const ast::SourceLocation& location,
        bool allowVoid = false
    );

    void validateGlobal(const ast::GlobalDecl& global);
    void validateFunction(const ast::Function& function);
    void validateBlock(const ast::LabelBlock& block);
    void validateTerminator(const ast::Terminator& terminator);

    void checkInstruction(const ast::Instruction& instruction);

    std::optional<ast::Type> operandType(
        const ast::Operand& operand,
        const ast::SourceLocation& location,
        const ast::Type* expected = nullptr
    );

    bool requireSame(
        const ast::Type& actual,
        const ast::Type& expected,
        const ast::SourceLocation& location,
        const std::string& context
    );

    bool validAlignment(
        const std::optional<ast::AlignAttr>& alignment,
        const ast::SourceLocation& location
    );

    void defineResult(
        const ast::ResultRef& result,
        const ast::Type& type
    );

    void validateCall(
        const ast::DirectCallInst& call
    );

    void validateCall(
        const ast::IndirectCallInst& call
    );
};

} // namespace cru::sem

#endif
