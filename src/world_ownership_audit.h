#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace amfp::world_ownership {

constexpr std::size_t kPropTypeCount = 15;

enum class RegisterResult : std::uint32_t {
    registered,
    bodyless,
    invalid,
    capacityExhausted,
};

struct Registration {
    RegisterResult result = RegisterResult::invalid;
    std::uint32_t propType = 0;
    std::uint32_t bodyCount = 0;
    std::uint32_t registeredBodies = 0;
    std::uint32_t subMeshCount = 0;
    std::uint32_t directBodyChildren = 0;
    std::uint32_t nestedBodyChildren = 0;
    std::uint32_t unmatchedSubMeshes = 0;
    bool staticPhysics = false;
    bool renderLayoutValid = false;
};

class Registry final {
public:
    // Forgets every body. Call only between maps, outside physics.
    void Reset() noexcept;
    // Call before the engine frees a cPhysicsBodyNewton wrapper.
    void NotifyBodyDestroyed(void* physicsBody) noexcept;
    [[nodiscard]] Registration RegisterProp(void* prop) noexcept;
    [[nodiscard]] bool ClassifyBody(void* physicsBody,
                                    std::uint32_t& propType) const noexcept;

private:
    struct Entry {
        std::atomic<void*> body{nullptr};
        std::atomic<std::uint32_t> propType{0};
    };
    static constexpr std::size_t kCapacity = 4096;
    static constexpr std::size_t kMaximumProbe = 64;
    std::array<Entry, kCapacity> entries_{};
    void ClearTable() noexcept;

    // Guarded by registrationLock_.
    std::uint32_t live_ = 0;
    std::uint32_t tombstones_ = 0;
    std::atomic_flag registrationLock_ = ATOMIC_FLAG_INIT;
};

[[nodiscard]] const wchar_t* RegisterResultName(RegisterResult result) noexcept;
[[nodiscard]] const wchar_t* PropTypeName(std::uint32_t propType) noexcept;

} // namespace amfp::world_ownership
