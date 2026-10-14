#pragma once

#include "hands_interpolation.h"
#include "presentation_clock.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace amfp::world {

using Matrix = hands::Matrix;

struct CommitResult {
    std::uint32_t callbacks = 0;
    std::uint32_t captured = 0;
    std::uint32_t duplicate = 0;
    std::uint32_t invalid = 0;
    std::uint32_t overflow = 0;
    std::uint32_t relationshipCandidates = 0;
    std::uint32_t relationshipOverflow = 0;
};

enum class RegistrationKind : std::uint32_t {
    notTarget,
    registered,
    bodyless,
    invalidLayout,
    incompleteRenderMapping,
    capacityExhausted,
};

struct RegistrationResult {
    RegistrationKind kind = RegistrationKind::notTarget;
    std::uint32_t bodies = 0;
};

enum class OverrideResult : std::uint32_t {
    notTarget,
    applied,
    appliedStandalone,
    standalonePromoted,
    clockUnavailable,
    historyUnavailable,
    pairMissing,
    invalid,
    slotsExhausted,
    diagnosticNestedTarget,
    diagnosticOwnedDirect,
    diagnosticOwnedNested,
    diagnosticStandaloneDirect,
    diagnosticStandaloneNested,
};

// Decides when prop registrations belong to a map that is being loaded, so
// the pointer-keyed tables can drop the previous map's bodies first. While
// cLuxMapHandler loads a map, the engine's listener viewport still presents
// the old world (level transition) or no world (save load); for a prop
// created during play it presents the registering world.
class MapEpoch final {
public:
    // Returns true when the tables must be reset before this registration.
    [[nodiscard]] bool OnRegistration(void* world,
                                      bool listenerKnown,
                                      void* listenerWorld) noexcept;
    void OnPresented(void* listenerWorld) noexcept;

private:
    void* world_ = nullptr;
    bool loading_ = false;
};

class Runtime final {
public:
    // Forgets every body. Call only between maps, outside physics and render.
    void Reset() noexcept;
    void SetRelationshipDiagnostics(bool enabled) noexcept;
    // Call before the engine frees a cPhysicsBodyNewton wrapper. Returns true
    // when that was the last tracked body and the tables were emptied.
    bool NotifyBodyDestroyed(void* physicsBody) noexcept;
    [[nodiscard]] RegistrationResult RegisterProp(void* prop) noexcept;
    void NotifyBodyMoved(void* physicsBody,
                         bool ownedByProp,
                         std::uint32_t propType = UINT32_MAX) noexcept;
    [[nodiscard]] CommitResult CommitCompletedTick(
        std::uint64_t tickSerial,
        std::uint64_t clockGeneration) noexcept;
    void BeginRender(const clock::Sample& sample) noexcept;
    [[nodiscard]] OverrideResult OverrideModelMatrix(
        void* subMesh,
        const Matrix* original,
        const Matrix*& result,
        std::uint32_t* propType = nullptr,
        std::uint32_t* relationshipDepth = nullptr) noexcept;
    void EndRender() noexcept;

private:
    struct Entry {
        std::atomic<void*> body{nullptr};
        hands::History history{};
        Matrix latest{};
        Matrix renderDelta{};
        std::uint64_t stagedSerial = 0;
        std::uint64_t renderFrame = 0;
        hands::SampleKind renderSample = hands::SampleKind::clockUnavailable;
        std::uint32_t propType = UINT32_MAX;
        bool hasLatest = false;
        bool standalone = false;
    };

    struct RelationshipCandidate {
        std::atomic<void*> body{nullptr};
        std::atomic<std::uint64_t> lastMovedSerial{0};
        std::atomic<std::uint8_t> ownedByProp{0};
        std::atomic<std::uint32_t> propType{UINT32_MAX};
    };

    [[nodiscard]] Entry* Find(void* body) noexcept;
    [[nodiscard]] const Entry* Find(void* body) const noexcept;
    [[nodiscard]] Entry* FindOrInsert(void* body) noexcept;
    [[nodiscard]] Entry* PromoteStandalone(void* body) noexcept;
    [[nodiscard]] RelationshipCandidate* FindRelationshipCandidate(
        void* body) noexcept;
    [[nodiscard]] RelationshipCandidate* FindOrInsertRelationshipCandidate(
        void* body) noexcept;
    void ClearTables() noexcept;
    [[nodiscard]] bool IsRecent(
        const RelationshipCandidate& candidate) const noexcept;

    static constexpr std::size_t kCapacity = 2048;
    static constexpr std::size_t kMovedCapacity = 2048;
    static constexpr std::size_t kRelationshipCapacity = 4096;
    static constexpr std::size_t kRelationshipMovedCapacity = 2048;
    static constexpr std::size_t kOutputCapacity = 2048;
    static constexpr std::size_t kMaximumParentDepth = 16;
    // Entries are only ever removed all at once, so a bounded linear probe
    // stays consistent between insertion and lookup and caps the per-submesh
    // cost if a table ever approaches capacity.
    static constexpr std::size_t kMaximumProbe = 64;
    std::array<Entry, kCapacity> entries_{};
    std::array<std::atomic<void*>, kMovedCapacity> moved_{};
    std::atomic<std::uint32_t> movedCount_{0};
    std::atomic<std::uint32_t> movedOverflow_{0};
    std::array<RelationshipCandidate, kRelationshipCapacity>
        relationshipCandidates_{};
    std::array<std::atomic<void*>, kRelationshipMovedCapacity>
        relationshipMoved_{};
    std::atomic<std::uint32_t> relationshipMovedCount_{0};
    std::atomic<std::uint32_t> relationshipOverflow_{0};
    std::atomic<std::uint32_t> entryLive_{0};
    std::atomic<std::uint32_t> entryTombstones_{0};
    std::atomic<std::uint32_t> candidateLive_{0};
    std::atomic<std::uint32_t> candidateTombstones_{0};
    std::atomic_flag registrationLock_ = ATOMIC_FLAG_INIT;
    std::array<Matrix, kOutputCapacity> outputs_{};
    std::size_t outputCount_ = 0;
    clock::Sample renderSample_{};
    std::uint64_t renderFrame_ = 0;
    bool renderActive_ = false;
    bool relationshipDiagnostics_ = true;
};

[[nodiscard]] const wchar_t* OverrideResultName(OverrideResult result) noexcept;

} // namespace amfp::world
