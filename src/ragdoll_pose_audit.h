#pragma once

#include "pose_audit.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace amfp::ragdoll_pose_audit {

enum class Result : std::uint32_t {
    notTarget,
    unavailable,
    invalidFlag,
    invalidVector,
    invalidMatrix,
    identityChanged,
    changed,
    stable,
    capacityExhausted,
};

struct Observation {
    Result result = Result::notTarget;
    std::size_t boneCount = 0;
    std::size_t invalidBoneIndex = 0;
};

// Read-only observer for cMeshEntity instances whose retail
// mbSkeletonPhysics flag is active. It never changes a mesh or its palette.
class Audit final {
public:
    [[nodiscard]] Observation Observe(void* mesh) noexcept;

private:
    struct Entry {
        void* mesh = nullptr;
        pose_audit::Matrix* buffer = nullptr;
        std::size_t boneCount = 0;
        std::array<pose_audit::Matrix, pose_audit::kMaximumBones> previous{};
        bool hasPrevious = false;
    };

    [[nodiscard]] Entry* FindOrInsert(void* mesh) noexcept;

    static constexpr std::size_t kCapacity = 64;
    std::array<Entry, kCapacity> entries_{};
};

[[nodiscard]] const wchar_t* ResultName(Result result) noexcept;

} // namespace amfp::ragdoll_pose_audit
