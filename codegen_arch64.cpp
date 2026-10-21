#include "codegen_arch64.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace cru::codegen {
namespace {

using namespace ast;

bool isPowerOfTwo(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

std::optional<std::uint64_t> alignTo(
    std::uint64_t value,
    std::uint64_t alignment
) {
    if (!isPowerOfTwo(alignment))
        return std::nullopt;

    const std::uint64_t mask = alignment - 1;

    if (value > std::numeric_limits<std::uint64_t>::max() - mask)
        return std::nullopt;

    return (value + mask) & ~mask;
}

std::optional<std::uint64_t> explicitAlignment(
    const std::optional<AlignAttr>& attr,
    const SourceLocation& location,
    AArch64Context& context
) {
    if (!attr)
        return std::uint64_t{1};

    if (!isPowerOfTwo(attr->value)) {
        context.report(
            location,
            "explicit alignment must be a non-zero power of two"
        );
        return std::nullopt;
    }

    return attr->value;
}

} // namespace

void AArch64Context::report(
    const ast::SourceLocation& location,
    std::string message
) {
    diagnostics.push_back(Diagnostic{location, std::move(message)});
}

bool AArch64Context::indexModule() {
    if (!module) {
        report({}, "AArch64 context has no AST module");
        return false;
    }

    structs.clear();
    unions.clear();
    structLayouts.clear();
    unionLayouts.clear();
    globalNames.clear();
    externalNames.clear();
    layoutInProgress.clear();

    for (const auto& declaration : module->declarations) {
        if (const auto* item =
                std::get_if<ast::StructDecl>(&declaration)) {
            if (structs.count(item->name) || unions.count(item->name)) {
                report(item->location,
                       "duplicate type declaration: " + item->name);
                continue;
            }
            structs.emplace(item->name, item);
        } else if (const auto* item =
                       std::get_if<ast::UnionDecl>(&declaration)) {
            if (structs.count(item->name) || unions.count(item->name)) {
                report(item->location,
                       "duplicate type declaration: " + item->name);
                continue;
            }
            unions.emplace(item->name, item);
        }
    }

    return diagnostics.empty();
}

std::optional<TypeLayout> AArch64Context::typeLayout(
    const ast::Type& type,
    const ast::SourceLocation& location
) {
    using ast::TypeKind;

    switch (type.kind) {
    case TypeKind::I8:
    case TypeKind::U8:
    case TypeKind::Bool:
        return TypeLayout{1, 1};

    case TypeKind::I16:
    case TypeKind::U16:
        return TypeLayout{2, 2};

    case TypeKind::I32:
    case TypeKind::U32:
    case TypeKind::F32:
        return TypeLayout{4, 4};

    case TypeKind::I64:
    case TypeKind::U64:
    case TypeKind::ISize:
    case TypeKind::USize:
    case TypeKind::F64:
    case TypeKind::Ptr:
        return TypeLayout{8, 8};

    case TypeKind::Void:
        report(location, "void has no object size");
        return std::nullopt;

    case TypeKind::Array: {
        if (!type.elementType) {
            report(location, "array type has no element type");
            return std::nullopt;
        }

        auto element = typeLayout(*type.elementType, location);
        if (!element)
            return std::nullopt;

        if (element->size != 0 &&
            type.arraySize >
                std::numeric_limits<std::uint64_t>::max() /
                    element->size) {
            report(location, "array size overflows 64-bit layout");
            return std::nullopt;
        }

        return TypeLayout{
            element->size * type.arraySize,
            element->alignment
        };
    }

    case TypeKind::Named: {
        if (structs.count(type.name)) {
            auto it = structLayouts.find(type.name);
            if (it == structLayouts.end()) {
                const auto* declaration = structs.at(type.name);

                if (!layoutInProgress.insert(type.name).second) {
                    report(location,
                           "recursive by-value type: " + type.name);
                    return std::nullopt;
                }

                StructLayout layout;
                std::uint64_t offset = 0;
                std::uint64_t maxAlignment = 1;

                auto declaredAlignment = explicitAlignment(
                    declaration->alignment,
                    declaration->location,
                    *this
                );

                if (!declaredAlignment) {
                    layoutInProgress.erase(type.name);
                    return std::nullopt;
                }

                bool valid = true;

                for (const auto& field : declaration->fields) {
                    auto fieldType = typeLayout(field.type, field.location);
                    auto fieldAlignment = explicitAlignment(
                        field.alignment, field.location, *this
                    );

                    if (!fieldType || !fieldAlignment) {
                        valid = false;
                        break;
                    }

                    const auto effectiveAlignment =
                        std::max(fieldType->alignment, *fieldAlignment);

                    auto alignedOffset = alignTo(offset, effectiveAlignment);
                    if (!alignedOffset) {
                        report(field.location,
                               "struct field offset overflows");
                        valid = false;
                        break;
                    }

                    if (fieldType->size >
                        std::numeric_limits<std::uint64_t>::max() -
                            *alignedOffset) {
                        report(field.location,
                               "struct size overflows");
                        valid = false;
                        break;
                    }

                    layout.fields.push_back(
                        FieldLayout{*alignedOffset, *fieldType}
                    );

                    offset = *alignedOffset + fieldType->size;
                    maxAlignment = std::max(maxAlignment, effectiveAlignment);
                }

                layoutInProgress.erase(type.name);

                if (!valid)
                    return std::nullopt;

                layout.alignment =
                    std::max(maxAlignment, *declaredAlignment);

                auto finalSize = alignTo(offset, layout.alignment);
                if (!finalSize) {
                    report(declaration->location,
                           "struct final size overflows");
                    return std::nullopt;
                }

                layout.size = *finalSize;
                it = structLayouts.emplace(type.name, layout).first;
            }

            return TypeLayout{it->second.size, it->second.alignment};
        }

        if (unions.count(type.name)) {
            auto it = unionLayouts.find(type.name);
            if (it == unionLayouts.end()) {
                const auto* declaration = unions.at(type.name);

                if (!layoutInProgress.insert(type.name).second) {
                    report(location,
                           "recursive by-value type: " + type.name);
                    return std::nullopt;
                }

                StructLayout layout;
                std::uint64_t maxSize = 0;
                std::uint64_t maxAlignment = 1;

                auto declaredAlignment = explicitAlignment(
                    declaration->alignment,
                    declaration->location,
                    *this
                );

                if (!declaredAlignment) {
                    layoutInProgress.erase(type.name);
                    return std::nullopt;
                }

                bool valid = true;

                for (const auto& field : declaration->fields) {
                    auto fieldType = typeLayout(field.type, field.location);
                    auto fieldAlignment = explicitAlignment(
                        field.alignment, field.location, *this
                    );

                    if (!fieldType || !fieldAlignment) {
                        valid = false;
                        break;
                    }

                    const auto effectiveAlignment =
                        std::max(fieldType->alignment, *fieldAlignment);

                    layout.fields.push_back(
                        FieldLayout{0, *fieldType}
                    );

                    maxSize = std::max(maxSize, fieldType->size);
                    maxAlignment = std::max(maxAlignment, effectiveAlignment);
                }

                layoutInProgress.erase(type.name);

                if (!valid)
                    return std::nullopt;

                layout.alignment =
                    std::max(maxAlignment, *declaredAlignment);

                auto finalSize = alignTo(maxSize, layout.alignment);
                if (!finalSize) {
                    report(declaration->location,
                           "union final size overflows");
                    return std::nullopt;
                }

                layout.size = *finalSize;
                it = unionLayouts.emplace(type.name, layout).first;
            }

            return TypeLayout{it->second.size, it->second.alignment};
        }

        report(location, "unknown named type: " + type.name);
        return std::nullopt;
    }
    }

    report(location, "unsupported type kind in AArch64 layout");
    return std::nullopt;
}

std::optional<std::uint64_t> AArch64Context::fieldOffset(
    const std::string& structName,
    std::uint64_t fieldIndex,
    const ast::SourceLocation& location
) {
    const bool isStruct = structs.count(structName) != 0;
    const bool isUnion = unions.count(structName) != 0;

    if (!isStruct && !isUnion) {
        report(location, "unknown aggregate type: " + structName);
        return std::nullopt;
    }

    ast::Type named;
    named.kind = ast::TypeKind::Named;
    named.name = structName;

    if (!typeLayout(named, location))
        return std::nullopt;

    const auto& layout = isStruct
        ? structLayouts.at(structName)
        : unionLayouts.at(structName);

    if (fieldIndex >= layout.fields.size()) {
        report(location, "field index is out of range for " + structName);
        return std::nullopt;
    }

    return layout.fields[static_cast<std::size_t>(fieldIndex)].offset;
}

} // namespace cru::codegen
