
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace cru::codegen {
namespace {

using namespace ast;

[[noreturn]] void unsupported(const std::string& what) {
    throw std::runtime_error("AArch64 codegen: unsupported " + what);
}

std::string typeName(const Type& t) {
    switch (t.kind) {
    case TypeKind::Named: return t.name;
    case TypeKind::I8: return "i8";
    case TypeKind::I16: return "i16";
    case TypeKind::I32: return "i32";
    case TypeKind::I64: return "i64";
    case TypeKind::U8: return "u8";
    case TypeKind::U16: return "u16";
    case TypeKind::U32: return "u32";
    case TypeKind::U64: return "u64";
    case TypeKind::ISize: return "isize";
    case TypeKind::USize: return "usize";
    case TypeKind::F32: return "f32";
    case TypeKind::F64: return "f64";
    case TypeKind::Bool: return "bool";
    case TypeKind::Ptr: return "ptr";
    case TypeKind::Void: return "void";
    case TypeKind::Array:
        return "[" + std::to_string(t.arraySize) + "]" +
               (t.elementType ? typeName(*t.elementType) : "?");
    }
    return "<?>";
}

bool isFloat(const Type& t) {
    return t.kind == TypeKind::F32 || t.kind == TypeKind::F64;
}

bool isSignedInteger(const Type& t) {
    return t.kind == TypeKind::I8 || t.kind == TypeKind::I16 ||
           t.kind == TypeKind::I32 || t.kind == TypeKind::I64 ||
           t.kind == TypeKind::ISize;
}

bool isIntegerLike(const Type& t) {
    switch (t.kind) {
    case TypeKind::I8: case TypeKind::I16: case TypeKind::I32:
    case TypeKind::I64: case TypeKind::U8: case TypeKind::U16:
    case TypeKind::U32: case TypeKind::U64: case TypeKind::ISize:
    case TypeKind::USize: case TypeKind::Bool: case TypeKind::Ptr:
        return true;
    default:
        return false;
    }
}

unsigned bitWidth(const Type& t) {
    switch (t.kind) {
    case TypeKind::I8: case TypeKind::U8: return 8;
    case TypeKind::I16: case TypeKind::U16: return 16;
    case TypeKind::I32: case TypeKind::U32: case TypeKind::F32: return 32;
    case TypeKind::I64: case TypeKind::U64: case TypeKind::ISize:
    case TypeKind::USize: case TypeKind::F64: case TypeKind::Ptr: return 64;
    case TypeKind::Bool: return 32;
    default: unsupported("bit width for type " + typeName(t));
    }
}

std::uint64_t alignUp(std::uint64_t n, std::uint64_t a) {
    if (a == 0) return n;
    return (n + a - 1) / a * a;
}

struct TypeLayout {
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
};

struct LayoutEngine {
    std::unordered_map<std::string, const StructDecl*> structs;
    std::unordered_map<std::string, const UnionDecl*> unions;

    TypeLayout layout(const Type& t) const {
        switch (t.kind) {
        case TypeKind::I8: case TypeKind::U8: return {1, 1};
        case TypeKind::I16: case TypeKind::U16: return {2, 2};
        case TypeKind::I32: case TypeKind::U32: case TypeKind::F32: return {4, 4};
        case TypeKind::I64: case TypeKind::U64: case TypeKind::ISize:
        case TypeKind::USize: case TypeKind::F64: case TypeKind::Ptr: return {8, 8};
        case TypeKind::Bool: return {1, 1};
        case TypeKind::Void: return {0, 1};
        case TypeKind::Array: {
            if (!t.elementType) unsupported("array type without element type");
            auto e = layout(*t.elementType);
            return {e.size * t.arraySize, e.alignment};
        }
        case TypeKind::Named: {
            auto si = structs.find(t.name);
            if (si != structs.end()) {
                const auto& s = *si->second;
                std::uint64_t off = 0, maxAlign = 1;
                for (const auto& f : s.fields) {
                    auto fl = layout(f.type);
                    auto fa = f.alignment ? f.alignment->value : fl.alignment;
                    if (fa == 0) fa = fl.alignment;
                    off = alignUp(off, fa);
                    off += fl.size;
                    maxAlign = std::max(maxAlign, fa);
                }
                if (s.alignment) maxAlign = std::max(maxAlign, s.alignment->value);
                return {alignUp(off, maxAlign), maxAlign};
            }
            auto ui = unions.find(t.name);
            if (ui != unions.end()) {
                const auto& u = *ui->second;
                std::uint64_t maxSize = 0, maxAlign = 1;
                for (const auto& f : u.fields) {
                    auto fl = layout(f.type);
                    auto fa = f.alignment ? f.alignment->value : fl.alignment;
                    if (fa == 0) fa = fl.alignment;
                    maxSize = std::max(maxSize, fl.size);
                    maxAlign = std::max(maxAlign, fa);
                }
                if (u.alignment) maxAlign = std::max(maxAlign, u.alignment->value);
                return {alignUp(maxSize, maxAlign), maxAlign};
            }
            unsupported("unknown named type '" + t.name + "'");
        }
        }
        unsupported("type layout");
    }


std::uint64_t fieldOffset(
    const std::string& structName,
    std::uint64_t fieldIndex
) const {
    auto it = structs.find(structName);

    if(it != structs.end()) {
   
    const auto& fields = it->second->fields;

    if (fieldIndex >= fields.size()) {
        unsupported(
            "field index " + std::to_string(fieldIndex) +
            " out of bounds for struct '" + structName + "'"
        );
    }

    std::uint64_t offset = 0;

    for (std::uint64_t i = 0; i <= fieldIndex; ++i) {
        const auto& field = fields[i];
        const auto fieldLayout = layout(field.type);

        std::uint64_t alignment = field.alignment
            ? field.alignment->value
            : fieldLayout.alignment;

        if (alignment == 0) {
            alignment = fieldLayout.alignment;
        }

        offset = alignUp(offset, alignment);

        if (i == fieldIndex) {
            return offset;
        }

        offset += fieldLayout.size;
    }
    }

    auto ui = unions.find(structName);

    
    if (ui != unions.end()) {
        const auto& u = *ui->second;

        if (fieldIndex >= u.fields.size()) {
            unsupported(
                "field index out of range for union '" + structName + "'"
            );
        }

        return 0;
    }
  
    unsupported(
        "field offset for unknown struct/union '" + structName + "'"
    );

    unsupported("failed to calculate field offset");
}
};

bool isStructType(const Type& t, const LayoutEngine& layouts) {
    return t.kind == TypeKind::Named && layouts.structs.count(t.name) > 0;
}

std::uint64_t parseIntegerText(const std::string& text) {
    if (text.empty()) throw std::runtime_error("AArch64 codegen: empty integer literal");
    bool neg = text[0] == '-';
    std::string s = neg ? text.substr(1) : text;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16; s = s.substr(2);
    } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2; s = s.substr(2);
    }
    std::uint64_t v = 0;
    for (char c : s) {
        unsigned d;
        if (c >= '0' && c <= '9') d = unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') d = unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = unsigned(c - 'A' + 10);
        else throw std::runtime_error("AArch64 codegen: invalid integer literal '" + text + "'");
        if (d >= unsigned(base)) throw std::runtime_error("AArch64 codegen: invalid integer literal '" + text + "'");
        v = v * unsigned(base) + d;
    }
    return neg ? std::uint64_t(0) - v : v;
}

std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back((std::isalnum(c) || c == '_' || c == '$') ? char(c) : '_');
    return out;
}

struct FunctionInfo {
    const Function* function = nullptr;
    const ExternDecl* external = nullptr;
    Type returnType;
};

class Generator {
public:
    explicit Generator(Module module) : module_(std::move(module)) {
        for (const auto& d : module_.declarations) {
            if (auto p = std::get_if<StructDecl>(&d)) layouts_.structs[p->name] = p;
            else if (auto p = std::get_if<UnionDecl>(&d)) layouts_.unions[p->name] = p;
        }
        for (const auto& d : module_.declarations) {
            if (auto f = std::get_if<Function>(&d)) {
                functions_[f->header.name] = FunctionInfo{f, nullptr, f->header.returnType};
            } else if (auto e = std::get_if<ExternDecl>(&d)) {
                functions_[e->header.name] = FunctionInfo{nullptr, e, e->header.returnType};
            } else if (auto g = std::get_if<GlobalDecl>(&d)) {
                globals_[g->name] = true;
            }
        }
    }

    std::string run() {
        out_ << ".text\n";
        for (const auto& d : module_.declarations) {
            if (const auto* e = std::get_if<ExternDecl>(&d)) {
                out_ << ".extern " << e->header.name << "\n";
            }
        }
        for (const auto& d : module_.declarations) {
            if (const auto* f = std::get_if<Function>(&d)) emitFunction(*f);
        }
        emitGlobals();
        out_ << ".section .note.GNU-stack,\"\",%progbits\n";
        return out_.str();
    }

private:
    Module module_;
    LayoutEngine layouts_;
    std::unordered_map<std::string, FunctionInfo> functions_;
    std::unordered_map<std::string, std::uint64_t> slots_;
    std::unordered_map<std::string, Type> valueTypes_;
    std::unordered_map<std::string, std::string> blockLabels_;
    std::unordered_map<std::string, bool> globals_;
    std::unordered_map<std::string, std::uint64_t> allocaOffsets_;
    std::ostringstream out_;
    std::ostringstream* current_ = nullptr;
    const Function* currentFunction_ = nullptr;
    std::uint64_t frameSize_ = 0;
    std::uint64_t nextSlot_ = 0;

    std::ostream& os() { return current_ ? *current_ : out_; }

    std::string localLabel(const std::string& block) const {
        return ".L" + sanitize(currentFunction_->header.name) + "_" + sanitize(block);
    }

    void allocateSlot(const ResultRef& r, const Type& t) {
        if (slots_.count(r.name)) return;
        nextSlot_ = alignUp(nextSlot_, 8);
        slots_[r.name] = nextSlot_;
        nextSlot_ += 8; // First implementation: one 8-byte spill slot per SSA value.
        valueTypes_[r.name] = t;
    }

    void discoverResult(const Instruction& ins) {
        std::visit([&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, ConstantInst> ||
                          std::is_same_v<T, UnaryInst> ||
                          std::is_same_v<T, BinaryInst> ||
                          std::is_same_v<T, CompareInst> ||
                          std::is_same_v<T, FloatCompareInst> ||
                          std::is_same_v<T, FloatBinaryInst> ||
                          std::is_same_v<T, CastInst> ||
                          std::is_same_v<T, AllocaInst> ||
                          std::is_same_v<T, LoadInst> ||
                          std::is_same_v<T, GlobalAddrInst> ||
                          std::is_same_v<T, FieldAddrInst> ||
                          std::is_same_v<T, ElementAddrInst> ||
                          std::is_same_v<T, AtomicLoadInst> ||
                          std::is_same_v<T, AtomicRMWInst> ||
                          std::is_same_v<T, AtomicCASInst> ||
                          std::is_same_v<T, SyscallInst> ) {
                if constexpr (std::is_same_v<T, ConstantInst>) allocateSlot(x.result, x.type);
                else if constexpr (std::is_same_v<T, SyscallInst>) allocateSlot(x.result, x.resultType);
                else if constexpr (std::is_same_v<T, UnaryInst>) allocateSlot(x.result, x.type);
                else if constexpr (std::is_same_v<T, BinaryInst>) allocateSlot(x.result, x.type);
                else if constexpr (std::is_same_v<T, CompareInst> || std::is_same_v<T, FloatCompareInst>) allocateSlot(x.result, Type(TypeKind::Bool));
                else if constexpr (std::is_same_v<T, FloatBinaryInst>) allocateSlot(x.result, x.type);
                else if constexpr (std::is_same_v<T, CastInst>) allocateSlot(x.result, x.targetType);
                else if constexpr (std::is_same_v<T, AllocaInst> || std::is_same_v<T, GlobalAddrInst> ||
                                   std::is_same_v<T, FieldAddrInst> || std::is_same_v<T, ElementAddrInst>)
                    allocateSlot(x.result, Type(TypeKind::Ptr));
                else if constexpr (std::is_same_v<T, LoadInst> || std::is_same_v<T, AtomicLoadInst>)
                    allocateSlot(x.result, x.type);
                else if constexpr (std::is_same_v<T, AtomicRMWInst>) allocateSlot(x.result, x.type);
            } else if constexpr (std::is_same_v<T, CallInst>) {
                std::visit([&](const auto& c) {
                    if (c.result) allocateSlot(*c.result, c.returnType);
                }, x);
            } else if constexpr (std::is_same_v<T, AtomicCASInst>) {
                allocateSlot(x.oldValue, x.type);
                allocateSlot(x.success, Type(TypeKind::Bool));
            }
        }, ins);
    }

    // Di dalam class Generator private:
struct AnonymousString {
    std::string text;
    std::string label;
};
std::vector<AnonymousString> anonymousStrings_;
std::unordered_map<std::string, std::string> stringToLabel_;

std::string getOrCreateStringLabel(const std::string& text) {
    auto it = stringToLabel_.find(text);
    if (it != stringToLabel_.end()) {
        return it->second;
    }
    std::string label = ".L.str." + std::to_string(anonymousStrings_.size());
    anonymousStrings_.push_back({text, label});
    stringToLabel_[text] = label;
    return label;
}


    void emitFunction(const Function& f) {
        currentFunction_ = &f;
        slots_.clear(); valueTypes_.clear(); blockLabels_.clear();
        nextSlot_ = 0;
        for (const auto& b : f.blocks) blockLabels_[b.name] = localLabel(b.name);

        // Parameters and every SSA result get a stable stack slot.
        for (const auto& p : f.header.params) {
            ResultRef r; r.name = p.name;
            allocateSlot(r, p.type);
        }
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions) discoverResult(i);

        allocaOffsets_.clear();

std::uint64_t frameCursor = nextSlot_;

for (const auto& block : f.blocks) {
    for (const auto& instruction : block.instructions) {
        const auto* alloca = std::get_if<AllocaInst>(&instruction);
        if (!alloca) continue;

        const auto layout = layouts_.layout(alloca->allocatedType);

        std::uint64_t alignment = layout.alignment;
        if (alloca->alignment) {
            alignment = std::max(
                alignment,
                static_cast<std::uint64_t>(alloca->alignment->value)
            );
        }

        if (alignment == 0 ||
            (alignment & (alignment - 1)) != 0) {
            unsupported("invalid alloca alignment");
        }

        frameCursor = alignUp(frameCursor, alignment);

        // Simpan offset objek relatif terhadap SP setelah prolog.
        allocaOffsets_[alloca->result.name] = frameCursor;

        // Objek kosong tetap memperoleh ruang minimal 1 byte.
        const std::uint64_t size = std::max<std::uint64_t>(
            layout.size, 1
        );

        if (frameCursor > 32760 || size > 32760 - frameCursor) {
            unsupported("function stack frame larger than 32760 bytes");
        }

        frameCursor += size;
    }
}

frameSize_ = alignUp(frameCursor, 16);

if (frameSize_ > 32760) {
    unsupported("function stack frame larger than 32760 bytes");
}
        std::ostringstream body;
        current_ = &body;
        os() << "\n.global " << f.header.name << "\n";
        os() << ".type " << f.header.name << ", %function\n";
        os() << f.header.name << ":\n";
        os() << "    stp x29, x30, [sp, #-16]!\n";
        os() << "    mov x29, sp\n";
        if (frameSize_) os() << "    sub sp, sp, #" << frameSize_ << "\n";

        // AAPCS64 integer/pointer arguments arrive in x0-x7.
        // Floating-point arguments are deliberately not implemented yet.
        for (std::size_t i = 0; i < f.header.params.size(); ++i) {
            const auto& p = f.header.params[i];
            if (!isIntegerLike(p.type))
                unsupported("non-integer parameter '" + p.name + "' (floating-point ABI is not implemented yet)");
            auto off = slots_.at(p.name);
            if (i < 8) {
                const std::string reg = (bitWidth(p.type) <= 32) ? "w" + std::to_string(i) : "x" + std::to_string(i);
                storeSlot(reg, off, bitWidth(p.type) <= 32);
            } else {
                const std::uint64_t incoming = 16 + (i - 8) * 8;
                os() << "    ldr x9, [x29, #" << incoming << "]\n";
                storeSlot(is32(p.type) ? "w9" : "x9", off, is32(p.type));
            }
        }

        for (const auto& b : f.blocks) {
            os() << blockLabels_.at(b.name) << ":\n";
            for (const auto& i : b.instructions) emitInstruction(i);
            if (!b.terminator) unsupported("basic block without terminator in function " + f.header.name);
            emitTerminator(*b.terminator);
        }

        if (body.str().find("    ret ") != std::string::npos) {
            // no-op: terminators emit their own epilogue
        }
        os() << ".size " << f.header.name << ", .-" << f.header.name << "\n";
        current_ = nullptr;
        out_ << body.str();
        currentFunction_ = nullptr;
    }

    bool is32(const Type& t) const {
        return t.kind == TypeKind::I8 || t.kind == TypeKind::I16 ||
               t.kind == TypeKind::I32 || t.kind == TypeKind::U8 ||
               t.kind == TypeKind::U16 || t.kind == TypeKind::U32 ||
               t.kind == TypeKind::Bool;
    }

Type inferOperandType(const Operand& op) {
    if (const auto* r = std::get_if<ResultRef>(&op)) {
        auto it = valueTypes_.find(r->name);
        if (it == valueTypes_.end()) {
            unsupported("unknown SSA value %" + r->name);
        }
        return it->second;
    }

    const auto& lit = std::get<Literal>(op);

    switch (lit.kind) {
    case LiteralKind::Character:
        return Type(TypeKind::I8);
    case LiteralKind::Integer:
        return Type(TypeKind::I64);
    case LiteralKind::Float:
        return Type(TypeKind::F64);
    case LiteralKind::Boolean:
        return Type(TypeKind::Bool);
    case LiteralKind::String:
    case LiteralKind::Null:
        return Type(TypeKind::Ptr);
    }

    unsupported("cannot infer operand type");
}

    bool isFloat32(const Type& t) const {
    return t.kind == TypeKind::F32;
}

bool isFloat64(const Type& t) const {
    return t.kind == TypeKind::F64;
}

bool isScalarFloat(const Type& t) const {
    return isFloat32(t) || isFloat64(t);
}

std::string floatReg(const std::string& n, const Type& t) const {
    return std::string(isFloat32(t) ? "s" : "d") + n;
}

void loadFloatOperand(
    const Operand& op,
    const std::string& freg,
    const Type& type
) {
    if (!isScalarFloat(type)) {
        unsupported("expected f32/f64 operand, got " + typeName(type));
    }

    const bool single = isFloat32(type);
    const std::string wreg = single ? "w9" : "x9";

    if (const auto* r = std::get_if<ResultRef>(&op)) {
        auto it = slots_.find(r->name);
        if (it == slots_.end()) {
            unsupported("unknown SSA value %" + r->name);
        }

        loadSlot(wreg, it->second);
        os() << "    fmov " << freg << ", " << wreg << "\n";
        return;
    }

    const auto& lit = std::get<Literal>(op);

    if (lit.kind != LiteralKind::Float) {
        unsupported(
            "non-floating literal used as " + typeName(type)
        );
    }

    if (single) {
        const float value = std::stof(lit.text);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        emitMoveImmediate("w9", bits);
        os() << "    fmov " << freg << ", w9\n";
    } else {
        const double value = std::stod(lit.text);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        emitMoveImmediate("x9", bits);
        os() << "    fmov " << freg << ", x9\n";
    }
}

void storeFloatResult(
    const std::string& freg,
    const Type& type,
    std::uint64_t offset
) {
    const std::string reg = isFloat32(type) ? "w9" : "x9";
    os() << "    fmov " << reg << ", " << freg << "\n";
    storeSlot(reg, offset);
}

    void loadSlot(const std::string& reg, std::uint64_t off, bool narrow = false) {
        // All stack slots are 8-byte aligned; slot offsets are relative to sp.
        os() << "    ldr " << reg << ", [sp, #" << off << "]\n";
        (void)narrow;
    }

    void storeSlot(const std::string& reg, std::uint64_t off, bool narrow = false) {
        os() << "    str " << reg << ", [sp, #" << off << "]\n";
        (void)narrow;
    }

    void emitMoveImmediate(
    const std::string& reg,
    std::uint64_t value
) {
    const bool isW = !reg.empty() && reg[0] == 'w';
    const unsigned maxShift = isW ? 32 : 64;

    bool first = true;

    for (unsigned shift = 0; shift < maxShift; shift += 16) {
        const std::uint16_t part =
            static_cast<std::uint16_t>(
                (value >> shift) & 0xffffu
            );

        if (part != 0 || first) {
            if (first) {
                os() << "    movz " << reg << ", #" << part;

                if (shift != 0) {
                    os() << ", lsl #" << shift;
                }

                os() << "\n";
                first = false;
            } else {
                os() << "    movk " << reg << ", #" << part
                     << ", lsl #" << shift << "\n";
            }
        }
    }
    }

    const Type& operandType(const Operand& op) {
        if (const auto* r = std::get_if<ResultRef>(&op)) {
            auto it = valueTypes_.find(r->name);
            if (it == valueTypes_.end()) unsupported("unknown SSA value %" + r->name);
            return it->second;
        }
        static const Type unknown(TypeKind::I64);
        return unknown;
    }

    void loadOperand(const Operand& op, const std::string& reg, const Type& expected) {
        if (const auto* r = std::get_if<ResultRef>(&op)) {
            auto it = slots_.find(r->name);
            if (it == slots_.end()) unsupported("unknown SSA value %" + r->name);
            loadSlot(reg, it->second);
            return;
        }
        const auto& lit = std::get<Literal>(op);
        if (lit.kind == LiteralKind::Integer || lit.kind == LiteralKind::Character) {
            emitMoveImmediate(reg, parseIntegerText(lit.text));
        } else if (lit.kind == LiteralKind::Boolean) {
            emitMoveImmediate(reg, (lit.text == "true") ? 1 : 0);
        } else if (lit.kind == LiteralKind::Null) {
            emitMoveImmediate(reg, 0);
        } else if (lit.kind == LiteralKind::String) {
        // Otomatis buat label rodata untuk string literal lokal/anonim
            std::string label = getOrCreateStringLabel(lit.text);
            os() << "    adrp " << reg << ", " << label << "\n";
            os() << "    add " << reg << ", " << reg << ", :lo12:" << label << "\n";
        } else {
            unsupported("floating-point literal in integer instruction");
        }
        (void)expected;
    }

    std::string conditionCode(CompareOp op, const Type& t) {
        const bool sign = isSignedInteger(t);
        switch (op) {
        case CompareOp::Eq: return "eq";
        case CompareOp::Ne: return "ne";
        case CompareOp::Lt: return sign ? "lt" : "lo";
        case CompareOp::Le: return sign ? "le" : "ls";
        case CompareOp::Gt: return sign ? "gt" : "hi";
        case CompareOp::Ge: return sign ? "ge" : "hs";
        }
        unsupported("comparison operator");
    }

    void emitInstruction(const Instruction& ins) {
        std::visit([&](const auto& x) { emitOne(x); }, ins);
    }
void emitCallArguments(const std::vector<Operand>& arguments) {
    unsigned intReg = 0;
    unsigned fpReg = 0;

    for (const auto& arg : arguments) {
        const Type type = inferOperandType(arg);

        if (isScalarFloat(type)) {
            if (fpReg >= 8) {
                unsupported(
                    "more than 8 floating-point call arguments "
                    "(stack argument ABI is not implemented)"
                );
            }

            loadFloatOperand(arg, floatReg(std::to_string(fpReg), type), type);
            ++fpReg;
        } else if (isIntegerLike(type)) {
            if (intReg >= 8) {
                unsupported(
                    "more than 8 integer/pointer call arguments "
                    "(stack argument ABI is not implemented)"
                );
            }

            const std::string reg =
                (bitWidth(type) <= 32 ? "w" : "x") +
                std::to_string(intReg);

            loadOperand(arg, reg, type);
            ++intReg;
        } else {
            unsupported(
                "call argument type " + typeName(type)
            );
        }
    }
}

void saveCallResult(
    const std::optional<ResultRef>& result,
    const Type& returnType
) {
    if (!result) return;

    const auto offset = slots_.at(result->name);

    if (isScalarFloat(returnType)) {
        storeFloatResult(floatReg("0", returnType), returnType, offset);
    } else if (isIntegerLike(returnType)) {
        storeSlot(
            is32(returnType) ? "w0" : "x0",
            offset
        );
    } else {
        unsupported(
            "call result type " + typeName(returnType)
        );
    }
}

    void emitOne(const ConstantInst& x) {
        if (isScalarFloat(x.type)) {
            const std::string freg = floatReg("0", x.type);
            loadFloatOperand(Operand{x.value}, freg, x.type);
            storeFloatResult(freg, x.type, slots_.at(x.result.name));
            return;
        }

        if (!isIntegerLike(x.type)) unsupported("const for type " + typeName(x.type));
        loadOperand(Operand{x.value}, "x9", x.type);
        storeSlot(is32(x.type) ? "w9" : "x9", slots_.at(x.result.name));
    }

    void emitOne(const UnaryInst& x) {
        if (!isIntegerLike(x.type)) unsupported("unary operation for " + typeName(x.type));
        const bool narrow = is32(x.type);
        const std::string r = narrow ? "w9" : "x9";
        loadOperand(x.operand, r, x.type);
        switch (x.op) {
        case UnaryOp::Neg: os() << "    neg " << r << ", " << r << "\n"; break;
        case UnaryOp::BitNot: os() << "    mvn " << r << ", " << r << "\n"; break;
        case UnaryOp::Not:
            os() << "    cmp " << r << ", #0\n";
            os() << "    cset " << r << ", eq\n";
            break;
        }
        storeSlot(r, slots_.at(x.result.name));
    }

    void emitOne(const BinaryInst& x) {
        if (!isIntegerLike(x.type)) unsupported("integer binary operation for " + typeName(x.type));
        const bool narrow = is32(x.type);
        const std::string a = narrow ? "w9" : "x9";
        const std::string b = narrow ? "w10" : "x10";
        loadOperand(x.lhs, a, x.type);
        loadOperand(x.rhs, b, x.type);
        switch (x.op) {
        case BinaryOp::Add: os() << "    add " << a << ", " << a << ", " << b << "\n"; break;
        case BinaryOp::Sub: os() << "    sub " << a << ", " << a << ", " << b << "\n"; break;
        case BinaryOp::Mul: os() << "    mul " << a << ", " << a << ", " << b << "\n"; break;
        case BinaryOp::Div: os() << "    " << (isSignedInteger(x.type) ? "sdiv " : "udiv ") << a << ", " << a << ", " << b << "\n"; break;
        
        case BinaryOp::Rem: {
            const std::string dividend = narrow ? "w11" : "x11";
            const std::string quotient = narrow ? "w12" : "x12";

            // Preserve the original dividend.
            os() << "    mov " << dividend << ", " << a << "\n";

            // quotient = dividend / divisor
            os() << "    "
                << (isSignedInteger(x.type) ? "sdiv " : "udiv ")
                << quotient << ", " << a << ", " << b << "\n";

            // remainder = dividend - quotient * divisor
            os() << "    msub " << a << ", " << quotient
                << ", " << b << ", " << dividend << "\n";
            break;
        }

        case BinaryOp::And: os() << "    and " << a << ", " << a << ", " << b << "\n"; break;
        case BinaryOp::Or: os() << "    orr " << a << ", " << a << ", " << b << "\n"; break;
        case BinaryOp::Xor: os() << "    eor " << a << ", " << a << ", " << b << "\n"; break;
        case BinaryOp::Shl: os() << "    lslv " << a << ", " << a << ", " << b << "\n"; break;
        case BinaryOp::Shr: os() << "    " << (isSignedInteger(x.type) ? "asrv " : "lsrv ") << a << ", " << a << ", " << b << "\n"; break;
        }
        storeSlot(a, slots_.at(x.result.name));
    }

    void emitOne(const CompareInst& x) {
        if (!isIntegerLike(x.type)) unsupported("integer comparison for " + typeName(x.type));
        const bool narrow = is32(x.type);
        const std::string a = narrow ? "w9" : "x9";
        const std::string b = narrow ? "w10" : "x10";
        loadOperand(x.lhs, a, x.type);
        loadOperand(x.rhs, b, x.type);
        os() << "    cmp " << a << ", " << b << "\n";
        os() << "    cset w9, " << conditionCode(x.op, x.type) << "\n";
        storeSlot("w9", slots_.at(x.result.name));
    }

void emitOne(const FloatCompareInst& x) {
    if (!isScalarFloat(x.type)) {
        unsupported(
            "floating-point comparison for " +
            typeName(x.type)
        );
    }

    const std::string a = floatReg("0", x.type);
    const std::string b = floatReg("1", x.type);

    loadFloatOperand(x.lhs, a, x.type);
    loadFloatOperand(x.rhs, b, x.type);

    os() << "    fcmp " << a << ", " << b << "\n";

    switch (x.op) {
    case CompareOp::Eq:
        os() << "    cset w9, eq\n";
        break;

    case CompareOp::Ne:
        os() << "    cset w9, ne\n";
        break;

    case CompareOp::Lt:
        os() << "    cset w9, mi\n";
        break;

    case CompareOp::Le:
        os() << "    cset w9, ls\n";
        break;

    case CompareOp::Gt:
        os() << "    cset w9, gt\n";
        break;

    case CompareOp::Ge:
        os() << "    cset w9, ge\n";
        break;
    }

    storeSlot("w9", slots_.at(x.result.name));
}

    void emitOne(const FloatBinaryInst& x) {
    if (!isScalarFloat(x.type)) {
        unsupported(
            "floating-point binary operation for " +
            typeName(x.type)
        );
    }

    const std::string a = floatReg("0", x.type);
    const std::string b = floatReg("1", x.type);

    loadFloatOperand(x.lhs, a, x.type);
    loadFloatOperand(x.rhs, b, x.type);

    switch (x.op) {
    case FloatBinaryOp::Add:
        os() << "    fadd " << a << ", " << a << ", " << b << "\n";
        break;

    case FloatBinaryOp::Sub:
        os() << "    fsub " << a << ", " << a << ", " << b << "\n";
        break;

    case FloatBinaryOp::Mul:
        os() << "    fmul " << a << ", " << a << ", " << b << "\n";
        break;

    case FloatBinaryOp::Div:
        os() << "    fdiv " << a << ", " << a << ", " << b << "\n";
        break;

    case FloatBinaryOp::Rem:
        unsupported(
            "floating-point remainder: AArch64 has no direct frem instruction"
        );
    }

    storeFloatResult(a, x.type, slots_.at(x.result.name));
    }
    void emitOne(const CastInst& x) {
        const Type src = inferOperandType(x.operand);
        const Type& dst = x.targetType;

         const auto resultOffset = slots_.at(x.result.name);

        if (src.kind == dst.kind) {
        if (isScalarFloat(dst)) {
            loadFloatOperand(x.operand, floatReg("0", src), src);
            storeFloatResult(floatReg("0", dst), dst, resultOffset);
        } else if (isIntegerLike(dst)) {
            loadOperand(x.operand, is32(dst) ? "w9" : "x9", src);
            storeSlot(is32(dst) ? "w9" : "x9", resultOffset);
        } else {
            unsupported("cast from " + typeName(src) + " to " + typeName(dst));
        }
        return;
    }

    if (isScalarFloat(src) && isScalarFloat(dst)) {
        loadFloatOperand(x.operand, floatReg("0", src), src);

        os() << "    "
             << (isFloat32(src) ? "fcvt d0, s0" : "fcvt s0, d0")
             << "\n";

        storeFloatResult(floatReg("0", dst), dst, resultOffset);
        return;
    }

    if (isIntegerLike(src) && isScalarFloat(dst)) {
        loadOperand(x.operand, is32(src) ? "w9" : "x9", src);

        const std::string intReg = is32(src) ? "w9" : "x9";
        const std::string fpReg = floatReg("0", dst);

        os() << "    "
             << (isSignedInteger(src) ? "scvtf " : "ucvtf ")
             << fpReg << ", " << intReg << "\n";

        storeFloatResult(fpReg, dst, resultOffset);
        return;
    }

    if (isScalarFloat(src) && isIntegerLike(dst)) {
        loadFloatOperand(x.operand, floatReg("0", src), src);

        const std::string intReg = is32(dst) ? "w9" : "x9";
        const std::string fpReg = floatReg("0", src);

        os() << "    "
             << (isSignedInteger(dst) ? "fcvtzs " : "fcvtzu ")
             << intReg << ", " << fpReg << "\n";

        storeSlot(intReg, resultOffset);
        return;
    }

    if (isIntegerLike(src) && isIntegerLike(dst)) {
        loadOperand(x.operand, "x9", src);

        const unsigned srcBits = bitWidth(src);
        const unsigned dstBits = bitWidth(dst);

        if (dstBits < 64) {
            const std::uint64_t mask = (std::uint64_t(1) << dstBits) - 1;
            os() << "    and x9, x9, #"
                 << mask << "\n";
        }

        if (dstBits == 64 && srcBits < 64 &&
            isSignedInteger(src)) {
            if (srcBits == 8)
                os() << "    sxtb x9, w9\n";
            else if (srcBits == 16)
                os() << "    sxth x9, w9\n";
            else if (srcBits == 32)
                os() << "    sxtw x9, w9\n";
        }

        storeSlot(is32(dst) ? "w9" : "x9", resultOffset);
        return;
    }

    if ((src.kind == TypeKind::Ptr && isIntegerLike(dst)) ||
        (dst.kind == TypeKind::Ptr && isIntegerLike(src))) {
        loadOperand(x.operand, "x9", src);
        storeSlot("x9", resultOffset);
        return;
    }

    unsupported(
        "cast from " + typeName(src) + " to " + typeName(dst)
    );
    }
    void emitOne(const AllocaInst& x) {
        auto it = allocaOffsets_.find(x.result.name);

        if (it == allocaOffsets_.end()) {
            unsupported(
            "missing stack allocation for %" + x.result.name
            );
        }

        const std::uint64_t offset = it->second;

    // Alamat objek = SP + offset.
        if (offset == 0) {
        os() << "    mov x9, sp\n";
        } else if (offset <= 4095) {
        os() << "    add x9, sp, #" << offset << "\n";
        } else {
        emitMoveImmediate("x9", offset);
        os() << "    add x9, sp, x9\n";
        }

    // Simpan pointer hasil alloca ke slot SSA.
        storeSlot("x9", slots_.at(x.result.name));
    }
    void emitOne(const LoadInst& x) {
        if (!isIntegerLike(x.type)) {
        unsupported("load for type " + typeName(x.type));
        }
 
    // x10 = alamat memori yang akan dibaca.
        loadOperand(
        x.address,
        "x10",
        Type(TypeKind::Ptr)
        );

    // Muat data sesuai ukuran dan signedness tipe.
        switch (x.type.kind) {
        case TypeKind::I8:
        os() << "    ldrsb w9, [x10]\n";
        break;

        case TypeKind::U8:
        case TypeKind::Bool:
        os() << "    ldrb w9, [x10]\n";
        break;

        case TypeKind::I16:
        os() << "    ldrsh w9, [x10]\n";
        break;

        case TypeKind::U16:
        os() << "    ldrh w9, [x10]\n";
        break;

        case TypeKind::I32:
        case TypeKind::U32:
        os() << "    ldr w9, [x10]\n";
        break;

        case TypeKind::I64:
        case TypeKind::U64:
        case TypeKind::ISize:
        case TypeKind::USize:
        case TypeKind::Ptr:
        os() << "    ldr x9, [x10]\n";
        break;

        default:
        unsupported("load for type " + typeName(x.type));
        }

    // Simpan hasil pembacaan ke slot SSA.
        storeSlot(
        bitWidth(x.type) <= 32 ? "w9" : "x9",
        slots_.at(x.result.name)
        );
}
    void emitOne(const StoreInst& x) {
    TypeLayout layout = layouts_.layout(x.type);
    if (layout.size == 0) {
        unsupported("store for empty or unsupported type " + typeName(x.type));
    }

    // x9 = nilai yang akan disimpan.
    const bool narrow = bitWidth(x.type) <= 32;
    loadOperand(
        x.value,
        narrow ? "w9" : "x9",
        x.type
    );

    // x10 = alamat tujuan.
    loadOperand(
        x.address,
        "x10",
        Type(TypeKind::Ptr)
    );

    // Pilih instruksi store berdasarkan ukuran tipe.
    switch (layouts_.layout(x.type).size) {
    case 1:
        os() << "    strb w9, [x10]\n";
        break;

    case 2:
        os() << "    strh w9, [x10]\n";
        break;

    case 4:
        os() << "    str w9, [x10]\n";
        break;

    case 8:
        os() << "    str x9, [x10]\n";
        break;

    default:
        unsupported(
            "store with unsupported size for type " +
            typeName(x.type)
        );
    }
    }
    void emitOne(const GlobalAddrInst& x) {
    if (globals_.find(x.globalName) == globals_.end()) {
        unsupported(
            "global.addr references unknown global '" +
            x.globalName + "'"
        );
    }

    // x9 = alamat global.
    // ADRP mengambil alamat halaman; ADD mengambil offset
    // simbol di dalam halaman tersebut.
    os() << "    adrp x9, " << x.globalName << "\n";
    os() << "    add x9, x9, :lo12:"
         << x.globalName << "\n";

    // Simpan alamat ke slot SSA hasil.
    storeSlot("x9", slots_.at(x.result.name));
    }
    void emitOne(const FieldAddrInst& x) {
        const std::uint64_t offset =
            layouts_.fieldOffset(x.structName, x.fieldIndex);

    // x.object harus berisi pointer ke objek struct.
        loadOperand(
        x.object,
        "x10",
        Type(TypeKind::Ptr)
        );

    // AArch64 ADD immediate mendukung offset sampai 4095
    // tanpa shifted immediate.
        if (offset == 0) {
        os() << "    mov x9, x10\n";
    } else if (offset <= 4095) {
        os() << "    add x9, x10, #" << offset << "\n";
    } else {
        emitMoveImmediate("x9", offset);
        os() << "    add x9, x10, x9\n";
        }

    // Simpan alamat field sebagai hasil SSA bertipe ptr.
        storeSlot("x9", slots_.at(x.result.name));
    }
    void emitOne(const ElementAddrInst& x) {
    const TypeLayout elementLayout =
        layouts_.layout(x.elementType);

    const std::uint64_t elementSize = elementLayout.size;

    // x10 = alamat dasar array.
    loadOperand(
        x.base,
        "x10",
        Type(TypeKind::Ptr)
    );

    // x11 = indeks elemen.
    loadOperand(
        x.index,
        "x11",
        Type(TypeKind::ISize)
    );

    // Offset byte = indeks * sizeof(element).
    if (elementSize == 0) {
        unsupported(
            "element.addr with zero-sized element type '" +
            typeName(x.elementType) + "'"
        );
    } else if (elementSize != 1) {
        emitMoveImmediate("x12", elementSize);
        os() << "    mul x11, x11, x12\n";
    }

    // x9 = base + offset.
    os() << "    add x9, x10, x11\n";

    // Simpan pointer hasil.
    storeSlot("x9", slots_.at(x.result.name));
    }
    void emitOne(const AtomicLoadInst& x) {
        if (!isIntegerLike(x.type))
            unsupported("atomic.load for type " + typeName(x.type));

        const unsigned width = bitWidth(x.type);

        if (width != 32 && width != 64)
            unsupported("atomic.load currently supports only 32-bit and 64-bit integers");
  
        const std::string valueReg = width == 32 ? "w9" : "x9";

        loadOperand(x.address, "x10", Type(TypeKind::Ptr));

        switch (x.ordering) {
            case MemoryOrdering::Relaxed:
                os() << "    ldr " << valueReg << ", [x10]\n";
                break;

            case MemoryOrdering::Acquire:
            case MemoryOrdering::SeqCst:
                os() << "    ldar " << valueReg << ", [x10]\n";
                break;

            case MemoryOrdering::Release:
            case MemoryOrdering::AcqRel:
                unsupported("invalid memory ordering for atomic.load");
        }

        storeSlot(valueReg, slots_.at(x.result.name));
    }
    void emitOne(const AtomicStoreInst& x) {
        if (!isIntegerLike(x.type))
            unsupported("atomic.store for type " + typeName(x.type));

        const unsigned width = bitWidth(x.type);

        if (width != 32 && width != 64)
            unsupported(
                "atomic.store currently supports only 32-bit and 64-bit integers"
            );

        const std::string valueReg = width == 32 ? "w9" : "x9";

        loadOperand(x.value, valueReg, x.type);
        loadOperand(x.address, "x10", Type(TypeKind::Ptr));

        switch (x.ordering) {
        case MemoryOrdering::Relaxed:
            os() << "    str " << valueReg << ", [x10]\n";
            break;

        case MemoryOrdering::Release:
            os() << "    stlr " << valueReg << ", [x10]\n";
            break;

        case MemoryOrdering::SeqCst:
            unsupported(
                "atomic.store seq_cst requires a sequential-consistency strategy"
            );

        case MemoryOrdering::Acquire:
        case MemoryOrdering::AcqRel:
            unsupported("invalid memory ordering for atomic.store");
        }
    }
    void emitOne(const AtomicRMWInst& x) {
    if (!isIntegerLike(x.type) ||
        x.type.kind == TypeKind::Bool ||
        x.type.kind == TypeKind::Ptr) {
        unsupported("atomic.rmw for type " + typeName(x.type));
    }

    const unsigned width = bitWidth(x.type);

    if (width != 32 && width != 64) {
        unsupported(
            "atomic.rmw currently supports only 32-bit and 64-bit integers"
        );
    }

    const bool narrow = width == 32;

    const std::string oldReg = narrow ? "w9" : "x9";
    const std::string valueReg = narrow ? "w11" : "x11";
    const std::string newReg = narrow ? "w14" : "x14";

    const std::string retryLabel =
        localLabel("rmw_retry_" + x.result.name);
    const std::string doneLabel =
        localLabel("rmw_done_" + x.result.name);

    // x13 holds the address throughout the exclusive loop.
    loadOperand(x.address, "x13", Type(TypeKind::Ptr));
    loadOperand(x.value, valueReg, x.type);

    bool acquire = false;
    bool release = false;
    bool seqCst = false;

    switch (x.ordering) {
    case MemoryOrdering::Relaxed:
        break;

    case MemoryOrdering::Acquire:
        acquire = true;
        break;

    case MemoryOrdering::Release:
        release = true;
        break;

    case MemoryOrdering::AcqRel:
        acquire = true;
        release = true;
        break;

    case MemoryOrdering::SeqCst:
        acquire = true;
        release = true;
        seqCst = true;
        break;
    }

    if (seqCst)
        os() << "    dmb ish\n";

    os() << retryLabel << ":\n";

    os() << "    "
         << (acquire ? "ldaxr " : "ldxr ")
         << oldReg << ", [x13]\n";

    switch (x.op) {
    case RMWOp::Add:
        os() << "    add " << newReg << ", "
             << oldReg << ", " << valueReg << "\n";
        break;

    case RMWOp::Sub:
        os() << "    sub " << newReg << ", "
             << oldReg << ", " << valueReg << "\n";
        break;

    case RMWOp::And:
        os() << "    and " << newReg << ", "
             << oldReg << ", " << valueReg << "\n";
        break;

    case RMWOp::Or:
        os() << "    orr " << newReg << ", "
             << oldReg << ", " << valueReg << "\n";
        break;

    case RMWOp::Xor:
        os() << "    eor " << newReg << ", "
             << oldReg << ", " << valueReg << "\n";
        break;

    case RMWOp::Xchg:
        os() << "    mov " << newReg << ", "
             << valueReg << "\n";
        break;
    }

    os() << "    st"
         << (release ? "lxr " : "xr ")
         << "w15, " << newReg << ", [x13]\n";

    os() << "    cbnz w15, " << retryLabel << "\n";

    if (seqCst)
        os() << "    dmb ish\n";

    os() << doneLabel << ":\n";

    // Atomic RMW returns the value observed before the update.
    storeSlot(oldReg, slots_.at(x.result.name));
    }
    void emitOne(const AtomicCASInst& x) {
    if (!isIntegerLike(x.type) ||
        x.type.kind == TypeKind::Bool ||
        x.type.kind == TypeKind::Ptr) {
        unsupported("atomic.cas for type " + typeName(x.type));
    }

    const unsigned width = bitWidth(x.type);

    if (width != 32 && width != 64) {
        unsupported(
            "atomic.cas currently supports only 32-bit and 64-bit integers"
        );
    }

    const bool narrow = width == 32;

    const std::string oldReg = narrow ? "w9" : "x9";
    const std::string expectedReg = narrow ? "w11" : "x11";
    const std::string desiredReg = narrow ? "w12" : "x12";

    const std::string retryLabel =
        localLabel("cas_retry_" + x.oldValue.name);
    const std::string failedLabel =
        localLabel("cas_failed_" + x.oldValue.name);
    const std::string doneLabel =
        localLabel("cas_done_" + x.oldValue.name);

    loadOperand(x.address, "x13", Type(TypeKind::Ptr));
    loadOperand(x.expected, expectedReg, x.type);
    loadOperand(x.desired, desiredReg, x.type);

    const bool successAcquire =
        x.successOrdering == MemoryOrdering::Acquire ||
        x.successOrdering == MemoryOrdering::AcqRel ||
        x.successOrdering == MemoryOrdering::SeqCst;

    const bool successRelease =
        x.successOrdering == MemoryOrdering::Release ||
        x.successOrdering == MemoryOrdering::AcqRel ||
        x.successOrdering == MemoryOrdering::SeqCst;

    const bool failureAcquire =
        x.failureOrdering == FailureOrdering::Acquire ||
        x.failureOrdering == FailureOrdering::SeqCst;

    const bool seqCst =
        x.successOrdering == MemoryOrdering::SeqCst ||
        x.failureOrdering == FailureOrdering::SeqCst;

    const bool useAcquire = successAcquire || failureAcquire;

    if (seqCst)
        os() << "    dmb ish\n";

    os() << retryLabel << ":\n";

    os() << "    "
         << (useAcquire ? "ldaxr " : "ldxr ")
         << oldReg << ", [x13]\n";

    os() << "    cmp " << oldReg << ", " << expectedReg << "\n";
    os() << "    b.ne " << failedLabel << "\n";

    os() << "    "
         << (successRelease ? "stlxr " : "stxr ")
         << "w15, " << desiredReg << ", [x13]\n";

    os() << "    cbnz w15, " << retryLabel << "\n";

    // CAS succeeded.
    os() << "    mov w16, #1\n";
    os() << "    b " << doneLabel << "\n";

    os() << failedLabel << ":\n";
    os() << "    clrex\n";
    os() << "    mov w16, #0\n";

    os() << doneLabel << ":\n";

    if (seqCst)
        os() << "    dmb ish\n";

    storeSlot(oldReg, slots_.at(x.oldValue.name));
    storeSlot("w16", slots_.at(x.success.name));
    }
    void emitOne(const CallInst& x) {
    std::visit([&](const auto& call) {
        using T = std::decay_t<decltype(call)>;

        if constexpr (std::is_same_v<T, DirectCallInst>) {
            if (functions_.find(call.functionName) == functions_.end()) {
                unsupported(
                    "call to unknown function '" +
                    call.functionName + "'"
                );
            }

            if (call.returnType.kind != TypeKind::Void &&
                !call.result) {
                unsupported(
                    "non-void call without result for '" +
                    call.functionName + "'"
                );
            }

            emitCallArguments(call.arguments);

            os() << "    bl " << call.functionName << "\n";

            saveCallResult(call.result, call.returnType);
        } else if constexpr (std::is_same_v<T, IndirectCallInst>) {
            if (call.returnType.kind != TypeKind::Void &&
                !call.result) {
                unsupported("non-void indirect call without result");
            }

            // Muat alamat fungsi sebelum menyiapkan argumen.
            loadOperand(call.function, "x16", Type(TypeKind::Ptr));

            emitCallArguments(call.arguments);

            os() << "    blr x16\n";

            saveCallResult(call.result, call.returnType);
        }
    }, x);
    }

        void emitOne(const SyscallInst& x) {
        // 1. Masukkan argumen ke register x0 - x5
        unsigned regIdx = 0;
        for (const auto& arg : x.args) {
            if (regIdx > 5) {
                unsupported("Too many arguments for syscall (max 6)");
            }
            std::string reg = "x" + std::to_string(regIdx);
            loadOperand(arg, reg, Type(TypeKind::I64));
            ++regIdx;
        }

        // 2. Masukkan nomor syscall ke register x8
        loadOperand(x.number, "x9", Type(TypeKind::I64));
        os() << "    mov x8, x9\n";

        // 3. Eksekusi Supervisor Call (kernel trap)
        os() << "    svc #0\n";

        // 4. Ambil nilai kembalian dari x0 dan simpan ke slot SSA result
        os() << "    mov x9, x0\n";
        storeSlot(
            is32(x.resultType) ? "w9" : "x9",
            slots_.at(x.result.name)
        );
    }

    void emitTerminator(const Terminator& term) {
        std::visit([&](const auto& x) { emitTerm(x); }, term);
    }

    void emitEpilogue() {
        if (frameSize_) os() << "    add sp, sp, #" << frameSize_ << "\n";
        os() << "    ldp x29, x30, [sp], #16\n";
        os() << "    ret\n";
    }

    void emitTerm(const ReturnTerminator& x) {
        if (x.value) {
            const Type& rt = currentFunction_->header.returnType;
            if (isScalarFloat(rt)) {
            loadFloatOperand(*x.value, floatReg("0", rt), rt);
        } else if (isIntegerLike(rt)) {
            loadOperand(*x.value, is32(rt) ? "w0" : "x0", rt);
        } else {
            unsupported("return type " + typeName(rt));
        }
        
        }
        emitEpilogue();
    }

    void emitTerm(const JumpTerminator& x) {
        auto it = blockLabels_.find(x.target);
        if (it == blockLabels_.end()) unsupported("unknown block label '" + x.target + "'");
        os() << "    b " << it->second << "\n";
    }

    void emitTerm(const BranchIfTerminator& x) {
        loadOperand(x.condition, "w9", Type(TypeKind::Bool));
        os() << "    cmp w9, #0\n";
        auto t = blockLabels_.find(x.trueTarget);
        auto f = blockLabels_.find(x.falseTarget);
        if (t == blockLabels_.end() || f == blockLabels_.end())
            unsupported("branch to unknown block label");
        os() << "    b.ne " << t->second << "\n";
        os() << "    b " << f->second << "\n";
    }

    void emitTerm(const UnreachableTerminator&) {
        os() << "    brk #0\n";
    }

    void emitGlobals() {
        bool hasAny = false;
        for (const auto& d : module_.declarations)
            if (std::holds_alternative<GlobalDecl>(d)) hasAny = true;
        if (!hasAny) return;

        os() << "\n.data\n";
        for (const auto& d : module_.declarations) {
            const auto* g = std::get_if<GlobalDecl>(&d);
            if (!g) continue;
            TypeLayout l = layouts_.layout(g->type);
            std::uint64_t a = g->alignment ? g->alignment->value : l.alignment;
            if (a == 0) a = l.alignment;
            if (a && (a & (a - 1)) == 0) {
                unsigned lg = 0;
                while ((std::uint64_t(1) << lg) < a) ++lg;
                os() << "    .p2align " << lg << "\n";
            }
            os() << (g->kind == GlobalKind::Constant ? ".global " : ".global ") << g->name << "\n";
            os() << ".type " << g->name << ", %object\n";
            os() << g->name << ":\n";
            if (!g->initializer) {
                os() << "    .zero " << l.size << "\n";
            } else if (const auto* lit = std::get_if<Literal>(&*g->initializer)) {
                if (lit->kind == LiteralKind::Integer || lit->kind == LiteralKind::Character) {
                    const auto v = parseIntegerText(lit->text);
                    emitGlobalInteger(v, l.size);
                } else if (lit->kind == LiteralKind::Float) {
                    if (l.size == 4) {
                    float val = std::stof(lit->text);
                    std::uint32_t bits;
                    std::memcpy(&bits, &val, sizeof(bits));
                    os() << "    .word " << bits << "\n";
                    } else {
                    double val = std::stod(lit->text);
                    std::uint64_t bits;
                    std::memcpy(&bits, &val, sizeof(bits));
                    os() << "    .xword " << bits << "\n";
                    }
                } else if (lit->kind == LiteralKind::Boolean) {
                    os() << "    .byte " << (lit->text == "true" ? 1 : 0) << "\n";
                } else if (lit->kind == LiteralKind::Null) {
                    os() << "    .zero " << l.size << "\n";
                } else if (lit->kind == LiteralKind::String)  {
                    os() << "    .asciz " << lit->text << "\n";
                } else {
                    unsupported("floating-point global initializer");
                }
            } else {
                 emitInitializer(*g->initializer,l);
            }
            os() << ".size " << g->name << ", " << l.size << "\n";
        }
    }

void emitAnonymousStrings() {
    if (anonymousStrings_.empty()) return;

    os() << "\n.section .rodata\n";
    for (const auto& str : anonymousStrings_) {
        os() << str.label << ":\n";
        os() << "    .asciz " << str.text << "\n";
    }
}


void emitInitializer(const Initializer& init, TypeLayout currentLayout) {
        if (const auto* lit = std::get_if<Literal>(&init)) {
            if (lit->kind == LiteralKind::Integer || lit->kind == LiteralKind::Character) {
                const auto v = parseIntegerText(lit->text);
                emitGlobalInteger(v, currentLayout.size);
            } else if (lit->kind == LiteralKind::Float) {
                if (currentLayout.size == 4) {
                float val = std::stof(lit->text);
                std::uint32_t bits;
                std::memcpy(&bits, &val, sizeof(bits));
                os() << "    .word " << bits << "\n";
                } else {
                    double val = std::stod(lit->text);
                    std::uint64_t bits;
                    std::memcpy(&bits, &val, sizeof(bits));
                    os() << "    .xword " << bits << "\n";
                }
            } else if (lit->kind == LiteralKind::Boolean) {
                os() << "    .byte " << (lit->text == "true" ? 1 : 0) << "\n";
            } else if (lit->kind == LiteralKind::Null) {
                os() << "    .zero 8\n";
            } else if (lit->kind == LiteralKind::String) {
                os() << "    .asciz " << lit->text << "\n";
            } else {
                unsupported("unsupported global initializer literal");
            }
        } else if (const auto* arrPtr = std::get_if<std::shared_ptr<ArrayLiteral>>(&init)) {
            if (*arrPtr) {
                std::uint64_t elemSize = currentLayout.size / (*arrPtr)->elements.size();
                if (elemSize == 0) elemSize = 1;
                TypeLayout elemLayout{elemSize, currentLayout.alignment};

                for (const auto& elem : (*arrPtr)->elements) {
                    emitInitializer(elem, elemLayout);
                }
            }
        }
}

    void emitGlobalInteger(std::uint64_t v, std::uint64_t size) {
        switch (size) {
        case 1: os() << "    .byte " << (v & 0xffu) << "\n"; break;
        case 2: os() << "    .hword " << (v & 0xffffu) << "\n"; break;
        case 4: os() << "    .word " << (v & 0xffffffffu) << "\n"; break;
        case 8: os() << "    .xword " << v << "\n"; break;
        default: unsupported("integer global initializer of size " + std::to_string(size));
        }
    }
};

} // namespace

std::string codegenArch(ast::Module mod) {
    return Generator(std::move(mod)).run();
}

} // namespace cru::codegen
