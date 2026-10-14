#pragma once

#include "pose_audit.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace amfp::enemy_pose_audit {

constexpr std::size_t kEnemyTypeCount = 5;

enum class StageResult : std::uint32_t {
    registered,
    staged,
    inactive,
    invalidEnemy,
    invalidType,
    capacityExhausted,
};

enum class ObserveResult : std::uint32_t {
    notTarget,
    unavailable,
    invalidVector,
    invalidMatrix,
    identityChanged,
    changed,
    stable,
};

struct StageObservation {
    StageResult result = StageResult::invalidEnemy;
    std::uint32_t type = 0;
};

struct PoseObservation {
    ObserveResult result = ObserveResult::notTarget;
    std::uint32_t type = 0;
    std::size_t boneCount = 0;
    std::size_t invalidBoneIndex = 0;
};

// Read-only registry connecting source- and binary-matched iLuxEnemy objects
// to their cMeshEntity palettes. Enemy simulation and renderer state are never
// changed by this audit.
class Registry final {
public:
    [[nodiscard]] StageObservation StageEnemy(void* enemy) noexcept;
    [[nodiscard]] PoseObservation ObserveMesh(void* mesh) noexcept;

private:
    struct Entry {
        void* enemy = nullptr;
        void* mesh = nullptr;
        pose_audit::Matrix* buffer = nullptr;
        std::size_t boneCount = 0;
        std::uint32_t type = 0;
        std::array<pose_audit::Matrix, pose_audit::kMaximumBones> previous{};
        bool hasPrevious = false;
    };

    [[nodiscard]] Entry* FindEnemy(void* enemy) noexcept;
    [[nodiscard]] Entry* FindMesh(void* mesh) noexcept;
    void RebuildMeshIndex() noexcept;

    static constexpr std::size_t kCapacity = 64;
    static constexpr std::size_t kBucketCount = 128;
    std::array<Entry, kCapacity> entries_{};
    std::array<std::size_t, kBucketCount> meshBuckets_{};
};

[[nodiscard]] const wchar_t* EnemyTypeName(std::uint32_t type) noexcept;
[[nodiscard]] const wchar_t* StageResultName(StageResult result) noexcept;
[[nodiscard]] const wchar_t* ObserveResultName(ObserveResult result) noexcept;

} // namespace amfp::enemy_pose_audit
