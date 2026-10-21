#include "codegen_arch64.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace cru::codegen {
namespace {

using namespace ast;

constexpr std::uint64_t kStackAlignment = 16;
constexpr std::uint64_t kSSASlotSize = 8;
constexpr std::uint64_t kSSASlotAlignment = 8;

std::string slotKey(std::string name) {
    // Referensi SSA dapat ditulis sebagai "value" atau "%value".
    if (!name.empty() && name.front() == '%')
        name.erase(name.begin());
    return name;
}

bool isPowerOfTwo(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

std::optional<std::uint64_t> alignUp(
    std::uint64_t value,
    std::uint64_t alignment
) {
    if (!isPowerOfTwo(alignment))
        return std::nullopt;

    const auto mask = alignment - 1;

    if (value > std::numeric_limits<std::uint64_t>::max() - mask)
        return std::nullopt;

    return (value + mask) & ~mask;
}

class FrameBuilder {
public:
    FrameBuilder(AArch64Context& context, const Function& function)
        : context_(context), function_(function) {}

    std::optional<FunctionFrame> build() {
        if (!collectParameters())
            return std::nullopt;

        for (const auto& block : function_.blocks) {
            for (const auto& instruction : block.instructions) {
                if (!collectInstruction(instruction, block.location))
                    return std::nullopt;
            }
        }

        if (!allocateFrame())
            return std::nullopt;

        return frame_;
    }

private:
    AArch64Context& context_;
    const Function& function_;
    FunctionFrame frame_;

    std::uint64_t cursor_ = 0;

    // Nama hasil yang sudah pernah didefinisikan.
    std::unordered_set<std::string> definitions_;

    bool reserveSlot(
        const std::string& name,
        std::uint64_t size,
        std::uint64_t alignment,
        std::unordered_map<std::string, StackSlot>& destination,
        const SourceLocation& location
    ) {
        const auto key = slotKey(name);

        if (key.empty()) {
            context_.report(location, "empty SSA or storage name");
            return false;
        }

        if (destination.count(key)) {
            context_.report(location, "duplicate stack slot: " + key);
            return false;
        }

        if (!isPowerOfTwo(alignment)) {
            context_.report(location,
                "stack slot alignment must be a non-zero power of two");
            return false;
        }

        destination.emplace(
            key,
            StackSlot{0, size, alignment}
        );

        return true;
    }

    bool addValue(
        const ResultRef& result,
        const SourceLocation& location
    ) {
        const auto key = slotKey(result.name);

        if (!definitions_.insert(key).second) {
            context_.report(location,
                "duplicate SSA definition: " + key);
            return false;
        }

        return reserveSlot(
            key,
            kSSASlotSize,
            kSSASlotAlignment,
            frame_.valueSlots,
            location
        );
    }

    bool collectParameters() {
        for (const auto& parameter : function_.header.params) {
            const auto key = slotKey(parameter.name);

            if (key.empty()) {
                context_.report(parameter.location,
                    "function parameter has an empty name");
                return false;
            }

            if (!definitions_.insert(key).second) {
                context_.report(parameter.location,
                    "duplicate parameter or SSA name: " + key);
                return false;
            }

            if (!reserveSlot(
                    key,
                    kSSASlotSize,
                    kSSASlotAlignment,
                    frame_.parameterSlots,
                    parameter.location)) {
                return false;
            }
        }

        return true;
    }

    bool collectInstruction(
        const Instruction& instruction,
        const SourceLocation& location
    ) {
        return std::visit(
            [&](const auto& inst) -> bool {
                using T = std::decay_t<decltype(inst)>;

                if constexpr (std::is_same_v<T, ConstantInst> ||
                              std::is_same_v<T, UnaryInst> ||
                              std::is_same_v<T, BinaryInst> ||
                              std::is_same_v<T, CompareInst> ||
                              std::is_same_v<T, FloatCompareInst> ||
                              std::is_same_v<T, FloatBinaryInst> ||
                              std::is_same_v<T, CastInst> ||
                              std::is_same_v<T, LoadInst> ||
                              std::is_same_v<T, GlobalAddrInst> ||
                              std::is_same_v<T, FieldAddrInst> ||
                              std::is_same_v<T, ElementAddrInst> ||
                              std::is_same_v<T, AtomicLoadInst> ||
                              std::is_same_v<T, AtomicRMWInst>) {
                    return addValue(inst.result, location);
                } else if constexpr (std::is_same_v<T, AllocaInst>) {
                    if (!addValue(inst.result, location))
                        return false;

                    auto layout = context_.typeLayout(
                        inst.allocatedType, location
                    );

                    if (!layout)
                        return false;

                    const std::uint64_t requestedAlignment =
                        inst.alignment.has_value() 
                            ? layout->alignment
                            : 1;

                    if (!isPowerOfTwo(requestedAlignment)) {
                        context_.report(location,
                            "alloca alignment must be a non-zero power of two");
                        return false;
                    }

                    return reserveSlot(
                        inst.result.name,
                        layout->size,
                        std::max(layout->alignment, requestedAlignment),
                        frame_.allocaObjects,
                        location
                    );
                } else if constexpr (std::is_same_v<T, AtomicCASInst>) {
                    if (!addValue(inst.oldValue, location))
                        return false;

                    return addValue(inst.success, location);
                } else if constexpr (std::is_same_v<T, StoreInst> ||
                                     std::is_same_v<T, AtomicStoreInst>) {
                    return true;
                } else if constexpr (std::is_same_v<T, CallInst>) {
                    return std::visit(
                        [&](const auto& call) -> bool {
                            using C = std::decay_t<decltype(call)>;

                            if constexpr (
                                std::is_same_v<C, DirectCallInst> ||
                                std::is_same_v<C, IndirectCallInst>) {
                                if (!call.result)
                                    return true;

                                return addValue(*call.result, location);
                            }

                            return true;
                        },
                        inst
                    );
                } else {
                    // Terminator atau instruksi tanpa hasil.
                    return true;
                }
            },
            instruction
        );
    }

    bool allocateOne(
        StackSlot& slot,
        const SourceLocation& location
    ) {
        auto aligned = alignUp(cursor_, slot.alignment);

        if (!aligned) {
            context_.report(location,
                "stack frame offset overflows 64-bit range");
            return false;
        }

        if (slot.size >
            std::numeric_limits<std::uint64_t>::max() - *aligned) {
            context_.report(location,
                "stack frame size overflows 64-bit range");
            return false;
        }

        cursor_ = *aligned + slot.size;
        slot.offset = cursor_;

        return true;
    }

    template <typename Map>
    bool allocateMap(
        Map& slots,
        const SourceLocation& location
    ) {
        for (auto& entry : slots) {
            if (!allocateOne(entry.second, location))
                return false;
        }

        return true;
    }

    bool allocateFrame() {
        // Parameter dan hasil SSA mendapatkan home slot 8-byte.
        // Alloca objects mendapatkan storage berdasarkan ukuran aktual.
        //
        // Urutan unordered_map tidak memengaruhi semantik, sebab emitter
        // mengakses slot melalui nama dan offset yang disimpan.

        const auto& location = function_.location;

        if (!allocateMap(frame_.parameterSlots, location))
            return false;

        if (!allocateMap(frame_.valueSlots, location))
            return false;

        if (!allocateMap(frame_.allocaObjects, location))
            return false;

        auto alignedFrame = alignUp(cursor_, kStackAlignment);
        if (!alignedFrame) {
            context_.report(location,
                "final stack frame size overflows");
            return false;
        }

        frame_.frameSize = *alignedFrame;
        return true;
    }
};

} // namespace

std::optional<FunctionFrame> buildFunctionFrame(
    AArch64Context& context,
    const ast::Function& function
) {
    FrameBuilder builder(context, function);
    return builder.build();
}

} // namespace cru::codegen
