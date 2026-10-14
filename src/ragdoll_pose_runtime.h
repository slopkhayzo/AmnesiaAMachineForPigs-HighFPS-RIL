#pragma once

#include "pose_interpolation.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace amfp::ragdoll_pose {

enum class UpdateResult : std::uint32_t {
    notTarget,
    captured,
    applied,
    alreadyApplied,
    unavailable,
    identityChanged,
    clockUnavailable,
    pairMissing,
    stationary,
    invalid,
    capacityExhausted,
};

struct UpdateObservation {
    UpdateResult result = UpdateResult::notTarget;
    std::size_t boneCount = 0;
    bool captured = false;
};

// Bounded final-palette interpolation for generic cMeshEntity instances whose
// retail mbSkeletonPhysics flag is active. Authoritative bytes are restored
// immediately after the scene render.
class Runtime final {
public:
    void BeginRender(const clock::Sample& clockSample,
                     std::uint64_t tickSerial) noexcept;
    [[nodiscard]] UpdateObservation OnMeshUpdated(void* mesh) noexcept;
    [[nodiscard]] bool WasObserved(const void* mesh) const noexcept;
    [[nodiscard]] bool ShouldReskin(const void* mesh) const noexcept;
    [[nodiscard]] std::size_t EndRender() noexcept;
    void Reset() noexcept;

private:
    struct Entry {
        void* mesh = nullptr;
        std::uint64_t lastCapturedSerial = 0;
        std::uint64_t lastObservedRender = 0;
        pose::History history{};
        hands::Matrix* identityBuffer = nullptr;
        std::size_t identityBoneCount = 0;
        hands::Matrix* overrideBuffer = nullptr;
        std::size_t overrideBoneCount = 0;
        std::array<hands::Matrix, pose::kMaximumBones> restore{};
        bool hasCapture = false;
        bool pairChanged = false;
        bool overridden = false;
        bool observedThisRender = false;
    };

    [[nodiscard]] Entry* FindOrInsert(void* mesh) noexcept;
    static void ResetIdentity(Entry& entry) noexcept;

    static constexpr std::size_t kCapacity = 32;
    static constexpr std::uint64_t kEvictionRenders = 64;
    std::array<Entry, kCapacity> entries_{};
    clock::Sample clockSample_{};
    std::uint64_t renderSerial_ = 0;
    std::uint64_t renderCount_ = 0;
    bool renderActive_ = false;
};

[[nodiscard]] const wchar_t* UpdateResultName(UpdateResult result) noexcept;

} // namespace amfp::ragdoll_pose
