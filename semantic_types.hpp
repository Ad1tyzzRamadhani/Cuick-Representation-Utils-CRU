#ifndef CRU_SEMANTIC_TYPES_HPP
#define CRU_SEMANTIC_TYPES_HPP

#include "ast.hpp"

namespace cru::sem {

using Type = ast::Type;
using TypeKind = ast::TypeKind;

inline bool isInteger(const Type& t) {
    switch (t.kind) {
    case TypeKind::I8: case TypeKind::I16:
    case TypeKind::I32: case TypeKind::I64:
    case TypeKind::U8: case TypeKind::U16:
    case TypeKind::U32: case TypeKind::U64:
    case TypeKind::ISize: case TypeKind::USize:
        return true;
    default:
        return false;
    }
}

inline bool isFloat(const Type& t) {
    return t.kind == TypeKind::F32 ||
           t.kind == TypeKind::F64;
}

inline bool isNumeric(const Type& t) {
    return isInteger(t) || isFloat(t);
}

inline bool isAggregate(const Type& t) {
    return t.kind == TypeKind::Array ||
           t.kind == TypeKind::Named;
}

inline bool isVoid(const Type& t) {
    return t.kind == TypeKind::Void;
}

inline bool isPointer(const Type& t) {
    return t.kind == TypeKind::Ptr;
}

inline bool sameType(const Type& a, const Type& b) {
    if (a.kind != b.kind)
        return false;

    switch (a.kind) {
    case TypeKind::Named:
        return a.name == b.name;

    case TypeKind::Array:
        return a.arraySize == b.arraySize &&
               a.elementType && b.elementType &&
               sameType(*a.elementType, *b.elementType);

    default:
        return true;
    }
}

inline bool isBoolean(const Type& t) {
    return t.kind == TypeKind::Bool;
}

inline bool isIntegerOrPointer(const Type& t) {
    return isInteger(t) || isPointer(t);
}

inline const char* typeName(TypeKind kind) {
    switch (kind) {
    case TypeKind::Named: return "named";
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
    case TypeKind::Array: return "array";
    }
    return "unknown";
}

} // namespace cru::sem

#endif
