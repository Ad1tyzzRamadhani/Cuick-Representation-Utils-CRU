#include "codegen_arch64.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace cru::codegen {
namespace {

using namespace ast;

std::string integerDirective(std::uint64_t size) {
    switch (size) {
    case 1: return ".byte";
    case 2: return ".hword";
    case 4: return ".word";
    case 8: return ".xword";
    default: return {};
    }
}

std::string integerText(const Literal& literal, bool& ok) {
    ok = false;

    if (literal.kind != LiteralKind::Integer &&
        literal.kind != LiteralKind::Boolean &&
        literal.kind != LiteralKind::Null) {
        return {};
    }

    if (literal.kind == LiteralKind::Boolean) {
        ok = literal.text == "true" || literal.text == "false";
        if (!ok)
            return {};
        return literal.text == "true" ? "1" : "0";
    }

    if (literal.kind == LiteralKind::Null) {
        ok = true;
        return "0";
    }

    errno = 0;
    char* end = nullptr;
    const auto value = std::strtoull(literal.text.c_str(), &end, 0);

    if (errno == ERANGE || end == literal.text.c_str() || *end != '\0')
        return {};

    ok = true;
    return std::to_string(value);
}

std::string floatingDirective(std::uint64_t size) {
    if (size == 4) return ".word";
    if (size == 8) return ".xword";
    return {};
}

std::optional<std::uint64_t> floatBits(
    const Literal& literal,
    std::uint64_t size
) {
    if (literal.kind != LiteralKind::Float &&
        literal.kind != LiteralKind::Integer) {
        return std::nullopt;
    }

    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(literal.text.c_str(), &end);

    if (errno == ERANGE || end == literal.text.c_str() || *end != '\0')
        return std::nullopt;

    if (!std::isfinite(value))
        return std::nullopt;

    if (size == 4) {
        const float narrowed = static_cast<float>(value);
        if (!std::isfinite(narrowed))
            return std::nullopt;

        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(narrowed));
        std::memcpy(&bits, &narrowed, sizeof(bits));
        return bits;
    }

    if (size == 8) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    return std::nullopt;
}

bool isArrayInitializer(const Initializer* initializer) {
    return initializer &&
        std::holds_alternative<std::shared_ptr<ArrayLiteral>>(*initializer);
}

const ArrayLiteral* getArrayInitializer(const Initializer* initializer) {
    if (!isArrayInitializer(initializer))
        return nullptr;

    const auto& ptr =
        std::get<std::shared_ptr<ArrayLiteral>>(*initializer);

    return ptr.get();
}

const Literal* getLiteralInitializer(const Initializer* initializer) {
    if (!initializer ||
        !std::holds_alternative<Literal>(*initializer)) {
        return nullptr;
    }

    return &std::get<Literal>(*initializer);
}

class GlobalObjectEmitter {
public:
    explicit GlobalObjectEmitter(AArch64Context& context)
        : context_(context) {}

    bool emit(
        const Type& type,
        const Initializer* initializer,
        const SourceLocation& location,
        std::string& out
    ) {
        auto layout = context_.typeLayout(type, location);
        if (!layout)
            return false;

        if (!initializer) {
            out += "\t.zero " + std::to_string(layout->size) + "\n";
            return true;
        }

        return emitValue(type, initializer, location, out);
    }

private:
    AArch64Context& context_;

    bool emitZero(
        const Type& type,
        const SourceLocation& location,
        std::string& out
    ) {
        auto layout = context_.typeLayout(type, location);
        if (!layout)
            return false;

        out += "\t.zero " + std::to_string(layout->size) + "\n";
        return true;
    }

    bool emitScalar(
        const Type& type,
        const Initializer* initializer,
        const SourceLocation& location,
        std::string& out
    ) {
        auto layout = context_.typeLayout(type, location);
        if (!layout)
            return false;

        if (!initializer)
            return emitZero(type, location, out);

        const Literal* literal = getLiteralInitializer(initializer);
        if (!literal) {
            context_.report(location,
                "scalar global requires a scalar literal initializer");
            return false;
        }

        if (type.kind == TypeKind::F32 || type.kind == TypeKind::F64) {
            auto bits = floatBits(*literal, layout->size);
            if (!bits) {
                context_.report(literal->location,
                    "invalid or out-of-range floating-point initializer");
                return false;
            }

            out += "\t" + floatingDirective(layout->size) + " " +
                   std::to_string(*bits) + "\n";
            return true;
        }

        if (type.kind == TypeKind::Ptr &&
            literal->kind != LiteralKind::Null &&
            literal->kind != LiteralKind::Integer) {
            context_.report(literal->location,
                "pointer initializer must be an integer address or null");
            return false;
        }

        bool ok = false;
        const std::string value = integerText(*literal, ok);
        if (!ok) {
            context_.report(literal->location,
                "invalid integer global initializer");
            return false;
        }

        const auto directive = integerDirective(layout->size);
        if (directive.empty()) {
            context_.report(location,
                "unsupported scalar size for global initializer");
            return false;
        }

        out += "\t" + directive + " " + value + "\n";
        return true;
    }

    bool emitArray(
        const Type& type,
        const Initializer* initializer,
        const SourceLocation& location,
        std::string& out
    ) {
        if (!type.elementType) {
            context_.report(location, "array type has no element type");
            return false;
        }

        const ArrayLiteral* array = getArrayInitializer(initializer);
        if (initializer && !array) {
            context_.report(location,
                "array global requires an array literal initializer");
            return false;
        }

        if (array && array->elements.size() > type.arraySize) {
            context_.report(location,
                "too many elements in array initializer");
            return false;
        }

        for (std::uint64_t i = 0; i < type.arraySize; ++i) {
            const Initializer* element = nullptr;

            if (array && i < array->elements.size()) {
                element = &array->elements[static_cast<std::size_t>(i)];
            }

            if (!emitValue(*type.elementType, element, location, out))
                return false;
        }

        return true;
    }

    bool emitAggregate(
        const Type& type,
        const Initializer* initializer,
        const SourceLocation& location,
        std::string& out
    ) {
        const bool isStruct = context_.structs.count(type.name) != 0;
        const bool isUnion = context_.unions.count(type.name) != 0;

        if (!isStruct && !isUnion) {
            context_.report(location, "unknown aggregate type: " + type.name);
            return false;
        }

        auto aggregateLayout = context_.typeLayout(type, location);
        if (!aggregateLayout)
            return false;

        const ArrayLiteral* values = getArrayInitializer(initializer);

        if (initializer && !values) {
            context_.report(location,
                "struct or union initializer must be an aggregate literal");
            return false;
        }

        const std::size_t fieldCount = isStruct
            ? context_.structs.at(type.name)->fields.size()
            : context_.unions.at(type.name)->fields.size();

        if (values && values->elements.size() > (isUnion ? 1u : fieldCount)) {
            context_.report(location,
                "too many elements in aggregate initializer");
            return false;
        }

        if (isUnion) {
            if (fieldCount == 0 || !values || values->elements.empty()) {
                out += "\t.zero " +
                       std::to_string(aggregateLayout->size) + "\n";
                return true;
            }

            const auto& field = context_.unions.at(type.name)->fields[0];
            const Initializer* first = &values->elements[0];

            if (!emitValue(field.type, first, field.location, out))
                return false;

            auto fieldLayout = context_.typeLayout(field.type, field.location);
            if (!fieldLayout)
                return false;

            if (aggregateLayout->size > fieldLayout->size) {
                out += "\t.zero " + std::to_string(
                    aggregateLayout->size - fieldLayout->size
                ) + "\n";
            }

            return true;
        }

        const auto& declaration = *context_.structs.at(type.name);
        const auto& layout = context_.structLayouts.at(type.name);
        std::uint64_t cursor = 0;

        for (std::size_t i = 0; i < fieldCount; ++i) {
            const auto& field = declaration.fields[i];
            const auto& fieldLayout = layout.fields[i];

            if (fieldLayout.offset > cursor) {
                out += "\t.zero " +
                    std::to_string(fieldLayout.offset - cursor) + "\n";
                cursor = fieldLayout.offset;
            }

            const Initializer* fieldInitializer = nullptr;
            if (values && i < values->elements.size())
                fieldInitializer = &values->elements[i];

            if (!emitValue(field.type, fieldInitializer,
                           field.location, out)) {
                return false;
            }

            auto fieldTypeLayout =
                context_.typeLayout(field.type, field.location);

            if (!fieldTypeLayout)
                return false;

            cursor = fieldLayout.offset + fieldTypeLayout->size;
        }

        if (aggregateLayout->size > cursor) {
            out += "\t.zero " +
                std::to_string(aggregateLayout->size - cursor) + "\n";
        }

        return true;
    }

    bool emitValue(
        const Type& type,
        const Initializer* initializer,
        const SourceLocation& location,
        std::string& out
    ) {
        if (type.kind == TypeKind::Array)
            return emitArray(type, initializer, location, out);

        if (type.kind == TypeKind::Named) {
            if (context_.structs.count(type.name) ||
                context_.unions.count(type.name)) {
                return emitAggregate(type, initializer, location, out);
            }

            context_.report(location, "unknown named type: " + type.name);
            return false;
        }

        return emitScalar(type, initializer, location, out);
    }
};

} // namespace

bool AArch64Context::emitGlobals() {
    if (!module) {
        report({}, "AArch64 context has no AST module");
        return false;
    }

    GlobalObjectEmitter emitter(*this);
    std::ostringstream assembly;

    for (const auto& declaration : module->declarations) {
        if (const auto* global =
                std::get_if<ast::GlobalDecl>(&declaration)) {
            if (!globalNames.insert(global->name).second) {
                report(global->location,
                    "duplicate global symbol: " + global->name);
                continue;
            }

            auto layout = typeLayout(global->type, global->location);
            if (!layout)
                continue;

            std::uint64_t alignment = layout->alignment;
            if (global->alignment) {
                const auto requested = global->alignment->value;

                if (requested == 0 ||
                    (requested & (requested - 1)) != 0) {
                    report(global->location,
                        "global alignment must be a non-zero power of two");
                    continue;
                }

                if (requested > alignment)
                    alignment = requested;
            }

            const bool hasInitializer = global->initializer.has_value();
            const bool isConstant = global->kind == ast::GlobalKind::Constant;

            if (!hasInitializer) {
                assembly << "\t.bss\n";
            } else if (isConstant) {
                assembly << "\t.section .rodata\n";
            } else {
                assembly << "\t.data\n";
            }

            assembly << "\t.balign " << alignment << "\n";
            assembly << "\t.global " << global->name << "\n";
            assembly << "\t.type " << global->name << ", %object\n";
            assembly << "\t.size " << global->name << ", "
                     << layout->size << "\n";
            assembly << global->name << ":\n";

            std::string objectData;
            const Initializer* init = hasInitializer
                ? &*global->initializer
                : nullptr;

            if (!emitter.emit(global->type, init,
                              global->location, objectData)) {
                continue;
            }

            assembly << objectData;
        } else if (const auto* external =
                       std::get_if<ast::ExternDecl>(&declaration)) {
            const auto& name = external->header.name;

            if (externalNames.insert(name).second)
                assembly << "\t.extern " << name << "\n";
        }
    }

    output += assembly.str();
    return diagnostics.empty();
}

} // namespace cru::codegen
