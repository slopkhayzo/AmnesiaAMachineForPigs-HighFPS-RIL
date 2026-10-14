#pragma once

#include "hands_interpolation.h"
#include "presentation_clock.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace amfp::node_animation {

enum class OverrideResult : std::uint32_t {
    notTarget,
    applied,
    stable,
    clockUnavailable,
    pairMissing,
    invalid,
    capacityExhausted,
};

struct Observation {
    OverrideResult result = OverrideResult::notTarget;
    bool captured = false;
    bool changed = false;
};

class Runtime final {
public:
    void BeginRender(const clock::Sample& sample,
                     std::uint64_t tickSerial) noexcept;
    [[nodiscard]] Observation OverrideModelMatrix(
        void* subMesh,
        const hands::Matrix* authoritative,
        const hands::Matrix*& result) noexcept;
    // Same history and interpolation for a submesh the caller has already
    // identified as moving with `owner` (an enemy's mesh entity), which has no
    // local node of its own.
    [[nodiscard]] Observation OverrideOwnedModelMatrix(
        void* subMesh,
        void* owner,
        const hands::Matrix* authoritative,
        const hands::Matrix*& result) noexcept;
    void Reset() noexcept;

private:
    struct Snapshot {
        std::uint64_t serial = 0;
        hands::Matrix matrix{};
    };

    struct Entry {
        void* subMesh = nullptr;
        void* localNode = nullptr;
        std::array<Snapshot, 2> snapshots{};
        hands::Matrix output{};
        std::uint64_t generation = 0;
        std::uint32_t count = 0;
        bool hasGeneration = false;
        bool pairChanged = false;
    };

    [[nodiscard]] Entry* FindOrInsert(void* subMesh,
                                      void* localNode) noexcept;
    void ResetHistory(Entry& entry) noexcept;
    void ClearEntries() noexcept;
    [[nodiscard]] Observation PublishAndEvaluate(
        Entry& entry,
        const hands::Matrix& authoritative,
        const hands::Matrix*& result) noexcept;

    static constexpr std::size_t kCapacity = 4096;
    static constexpr std::size_t kMaximumProbe = 64;
    static constexpr std::size_t kRecycleOccupancy = kCapacity / 4;
    static constexpr std::uint64_t kRecycleCooldownRenders = 4096;
    std::array<Entry, kCapacity> entries_{};
    clock::Sample sample_{};
    std::uint64_t tickSerial_ = 0;
    std::uint64_t renderCount_ = 0;
    std::uint64_t lastRecycleRender_ = 0;
    std::size_t occupied_ = 0;
};

[[nodiscard]] const wchar_t* OverrideResultName(OverrideResult result) noexcept;

} // namespace amfp::node_animation
