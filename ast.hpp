
#ifndef CRU_AST_HPP
#define CRU_AST_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cru::ast {

// ============================================================
// Source location
// ============================================================

struct SourceLocation {
    std::string file;
    std::size_t line = 1;
    std::size_t column = 1;
};

// ============================================================
// Basic values
// ============================================================

struct Node {
    SourceLocation location;
};

enum class TypeKind {
    Named,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    ISize, USize,
    F32, F64,
    Bool,
    Ptr,
    Void,
    Array
};

struct Type {
    TypeKind kind = TypeKind::Named;
    std::string name;

    // Used only when kind == Array.
    std::uint64_t arraySize = 0;
    std::unique_ptr<Type> elementType;

    Type() = default;

    explicit Type(TypeKind k) : kind(k) {}

    explicit Type(std::string namedType)
        : kind(TypeKind::Named),
          name(std::move(namedType)) {}

    Type(std::uint64_t size, Type element)
        : kind(TypeKind::Array),
          arraySize(size),
          elementType(
              std::make_unique<Type>(std::move(element))) {}

    Type(Type&&) noexcept = default;
    Type& operator=(Type&&) noexcept = default;

    Type(const Type& other)
        : kind(other.kind),
          name(other.name),
          arraySize(other.arraySize) {
        if (other.elementType) {
            elementType =
                std::make_unique<Type>(*other.elementType);
        }
    }

    Type& operator=(const Type& other) {
        if (this == &other)
            return *this;

        kind = other.kind;
        name = other.name;
        arraySize = other.arraySize;
        elementType.reset();

        if (other.elementType) {
            elementType =
                std::make_unique<Type>(*other.elementType);
        }

        return *this;
    }
};

enum class LiteralKind {
    Integer,
    Float,
    Boolean,
    Null
};

// Keep numeric spelling intact until semantic checking.
// For example, hexadecimal and binary literals retain their
// original representation in text.
struct Literal {
    LiteralKind kind = LiteralKind::Integer;
    std::string text;
    SourceLocation location;
};

struct ResultRef {
    // The lexer produces %name as one RESULT token.
    // Store name without the leading '%'.
    std::string name;
    SourceLocation location;
};

using Operand = std::variant<ResultRef, Literal>;

struct AlignAttr {
    std::uint64_t value = 0;
};

// ============================================================
// Declarations
// ============================================================

struct FieldDecl : Node {
    std::string name;
    Type type;
    std::optional<AlignAttr> alignment;
};

struct StructDecl : Node {
    std::string name;
    std::optional<AlignAttr> alignment;
    std::vector<FieldDecl> fields;
};

struct UnionDecl : Node {
    std::string name;
    std::optional<AlignAttr> alignment;
    std::vector<FieldDecl> fields;
};

struct ArrayLiteral;

using Initializer = std::variant<
    Literal,
    std::shared_ptr<ArrayLiteral>
>;

struct ArrayLiteral {
    std::vector<Initializer> elements;
};

enum class GlobalKind {
    Mutable,
    Constant
};

struct GlobalDecl : Node {
    GlobalKind kind = GlobalKind::Mutable;
    std::string name;
    Type type;
    std::optional<AlignAttr> alignment;
    std::optional<Initializer> initializer;
};

struct Param : Node {
    Type type;
    std::string name;
};

struct FunctionHeader : Node {
    Type returnType;
    std::string name;
    std::vector<Param> params;
};

struct ExternDecl : Node {
    FunctionHeader header;
};

// ============================================================
// Instructions
// ============================================================

struct ConstantInst : Node {
    ResultRef result;
    Type type;
    Literal value;
};

enum class UnaryOp {
    Neg,
    Not,
    BitNot
};

struct UnaryInst : Node {
    ResultRef result;
    UnaryOp op = UnaryOp::Neg;
    Type type;
    Operand operand;
};

enum class BinaryOp {
    Add, Sub, Mul, Div, Rem,
    And, Or, Xor, Shl, Shr
};


struct SyscallInst {
    SourceLocation location;
    std::string result;
    std::string syscallName;
    Type resultType;
    Operand number;
    std::vector<Operand> args;
};

struct BinaryInst : Node {
    ResultRef result;
    BinaryOp op = BinaryOp::Add;
    Type type;
    Operand lhs;
    Operand rhs;
};

enum class CompareOp {
    Eq, Ne, Lt, Le, Gt, Ge
};

struct CompareInst : Node {
    ResultRef result;
    CompareOp op = CompareOp::Eq;
    Type type;
    Operand lhs;
    Operand rhs;
};

struct FloatCompareInst : Node {
    ResultRef result;
    CompareOp op = CompareOp::Eq;
    Type type;
    Operand lhs;
    Operand rhs;
};

enum class FloatBinaryOp {
    Add, Sub, Mul, Div, Rem
};

struct FloatBinaryInst : Node {
    ResultRef result;
    FloatBinaryOp op = FloatBinaryOp::Add;
    Type type;
    Operand lhs;
    Operand rhs;
};

struct CastInst : Node {
    ResultRef result;
    Operand operand;
    Type targetType;
};

struct AllocaInst : Node {
    ResultRef result;
    Type allocatedType;
    std::optional<AlignAttr> alignment;
};

struct LoadInst : Node {
    ResultRef result;
    Type type;
    Operand address;
    bool isVolatile = false;
};

struct StoreInst : Node {
    Type type;
    Operand value;
    Operand address;
    bool isVolatile = false;
};

struct GlobalAddrInst : Node {
    ResultRef result;
    std::string globalName;
};

struct FieldAddrInst : Node {
    ResultRef result;
    std::string structName;
    Operand object;
    std::uint64_t fieldIndex = 0;
};

struct ElementAddrInst : Node {
    ResultRef result;
    Type elementType;
    Operand base;
    Operand index;
};

// ============================================================
// Calls
// ============================================================

struct DirectCallInst : Node {
    std::optional<ResultRef> result;
    Type returnType;
    std::string functionName;
    std::vector<Operand> arguments;
};

struct IndirectCallInst : Node {
    std::optional<ResultRef> result;
    Type returnType;
    Operand function;
    std::vector<Operand> arguments;
};

using CallInst = std::variant<
    DirectCallInst,
    IndirectCallInst
>;

// ============================================================
// Atomic instructions
// ============================================================

enum class MemoryOrdering {
    Relaxed,
    Acquire,
    Release,
    AcqRel,
    SeqCst
};

enum class FailureOrdering {
    Relaxed,
    Acquire,
    SeqCst
};

enum class RMWOp {
    Add, Sub, And, Or, Xor, Xchg
};

struct AtomicLoadInst : Node {
    ResultRef result;
    Type type;
    Operand address;
    MemoryOrdering ordering = MemoryOrdering::SeqCst;
};

struct AtomicStoreInst : Node {
    Type type;
    Operand value;
    Operand address;
    MemoryOrdering ordering = MemoryOrdering::SeqCst;
};

struct AtomicRMWInst : Node {
    ResultRef result;
    RMWOp op = RMWOp::Add;
    Type type;
    Operand address;
    Operand value;
    MemoryOrdering ordering = MemoryOrdering::SeqCst;
};

struct AtomicCASInst : Node {
    ResultRef oldValue;
    ResultRef success;
    Type type;
    Operand address;
    Operand expected;
    Operand desired;
    MemoryOrdering successOrdering = MemoryOrdering::SeqCst;
    FailureOrdering failureOrdering = FailureOrdering::Acquire;
};

// ============================================================
// Instruction variant
// ============================================================

using Instruction = std::variant<
    ConstantInst,
    UnaryInst,
    BinaryInst,
    CompareInst,
    FloatCompareInst,
    FloatBinaryInst,
    CastInst,
    AllocaInst,
    LoadInst,
    StoreInst,
    GlobalAddrInst,
    FieldAddrInst,
    ElementAddrInst,
    CallInst,
    AtomicLoadInst,
    AtomicStoreInst,
    AtomicRMWInst,
    AtomicCASInst, 
    SyscallInst
>;

// ============================================================
// Terminators
// ============================================================

struct ReturnTerminator : Node {
    std::optional<Operand> value;
};

struct JumpTerminator : Node {
    std::string target;
};

struct BranchIfTerminator : Node {
    Operand condition;
    std::string trueTarget;
    std::string falseTarget;
};

struct UnreachableTerminator : Node {};

using Terminator = std::variant<
    ReturnTerminator,
    JumpTerminator,
    BranchIfTerminator,
    UnreachableTerminator
>;

// ============================================================
// Basic blocks and functions
// ============================================================

struct LabelBlock : Node {
    std::string name;
    std::vector<Instruction> instructions;

    // Grammar requires every block to end in a terminator.
    // optional lets the parser build the block before it
    // has consumed and validated the terminator.
    std::optional<Terminator> terminator;
};

struct Function : Node {
    FunctionHeader header;
    std::vector<LabelBlock> blocks;
};

// ============================================================
// Module
// ============================================================

using Declaration = std::variant<
    StructDecl,
    UnionDecl,
    GlobalDecl,
    ExternDecl,
    Function
>;

struct Module : Node {
    std::vector<Declaration> declarations;
};

} // namespace cru::ast

#endif // CRU_AST_HPP
