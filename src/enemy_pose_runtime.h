#pragma once

#include "pose_interpolation.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace amfp::enemy_pose {

enum class StageResult : std::uint32_t {
    staged,
    unsupportedType,
    inactive,
    unavailable,
    capacityExhausted,
};

enum class UpdateResult : std::uint32_t {
    notTarget,
    captured,
    applied,
    alreadyApplied,
    unavailable,
    identityChanged,
    clockUnavailable,
    pairMissing,
    invalid,
};

struct StageObservation {
    StageResult result = StageResult::unavailable;
    std::uint32_t type = 0;
};

struct UpdateObservation {
    UpdateResult result = UpdateResult::notTarget;
    std::uint32_t type = 0;
    std::size_t boneCount = 0;
    bool captured = false;
};

// Bounded to the ManPig and Child archetypes. Each exact mesh owns its own palette
// identity, bone count, two completed-tick snapshots, and restoration buffer.
class Runtime final {
public:
    [[nodiscard]] StageObservation StageEnemy(
        void* enemy, std::uint64_t pendingSerial) noexcept;
    void CommitCompletedTick(std::uint64_t tickSerial,
                             std::uint64_t clockGeneration) noexcept;
    void BeginRender(const clock::Sample& clockSample,
                     std::uint64_t tickSerial) noexcept;
    [[nodiscard]] UpdateObservation OnMeshUpdated(void* mesh) noexcept;
    // True during a render for the mesh of an enemy that was updated in the
    // newest fixed update, so its root can share the palette's endpoint pair.
    [[nodiscard]] bool TracksMesh(void* mesh) noexcept;
    [[nodiscard]] std::size_t EndRender() noexcept;
    void Reset() noexcept;

private:
    struct Entry {
        void* enemy = nullptr;
        void* mesh = nullptr;
        std::uint32_t type = 0;
        std::uint64_t pendingSerial = 0;
        std::uint64_t currentSerial = 0;
        std::uint64_t currentGeneration = 0;
        std::uint64_t lastCapturedSerial = 0;
        pose::History history{};
        hands::Matrix* identityBuffer = nullptr;
        std::size_t identityBoneCount = 0;
        hands::Matrix* overrideBuffer = nullptr;
        std::size_t overrideBoneCount = 0;
        std::array<hands::Matrix, pose::kMaximumBones> restore{};
        bool staged = false;
        bool activeThisTick = false;
        bool hasCapture = false;
        bool overridden = false;
    };

    [[nodiscard]] Entry* FindEnemy(void* enemy) noexcept;
    [[nodiscard]] Entry* FindMesh(void* mesh) noexcept;
    void RebuildMeshIndex() noexcept;
    static void ResetIdentity(Entry& entry) noexcept;

    static constexpr std::size_t kCapacity = 32;
    static constexpr std::size_t kBucketCount = 64;
    std::array<Entry, kCapacity> entries_{};
    std::array<std::size_t, kBucketCount> meshBuckets_{};
    clock::Sample clockSample_{};
    std::uint64_t renderSerial_ = 0;
    bool renderActive_ = false;
};

[[nodiscard]] const wchar_t* StageResultName(StageResult result) noexcept;
[[nodiscard]] const wchar_t* UpdateResultName(UpdateResult result) noexcept;

} // namespace amfp::enemy_pose
