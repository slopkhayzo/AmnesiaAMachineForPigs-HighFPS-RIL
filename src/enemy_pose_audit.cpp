#include "enemy_pose_audit.h"

#include <cstdint>
#include <cstring>

namespace amfp::enemy_pose_audit {
namespace {

constexpr std::size_t kEnemyMeshOffset = 0x578;
constexpr std::size_t kEnemyTypeOffset = 0x8E4;
constexpr std::size_t kEntityActiveOffset = 0x24;
constexpr std::size_t kBoneMatricesBeginOffset = 0x388;
constexpr std::size_t kBoneMatricesEndOffset = 0x38C;
constexpr std::size_t kBoneMatricesCapacityOffset = 0x390;

// Diagnostics-only, but still called from tick and render hooks. Every object
// read here is a hook receiver or is reached through source/retail-proven
// fields the engine itself dereferences, so no OS memory query is issued.
template <typename T>
bool ReadAt(const void* base, const std::size_t offset, T& value) noexcept {
    if (base == nullptr) return false;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset,
                sizeof(T));
    return true;
}

std::size_t HashPointer(const void* pointer) noexcept {
    std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer) >> 4U;
    value ^= value >> 11U;
    return static_cast<std::size_t>(value);
}

} // namespace

Registry::Entry* Registry::FindEnemy(void* const enemy) noexcept {
    if (enemy == nullptr) return nullptr;
    for (Entry& entry : entries_) {
        if (entry.enemy == enemy) return &entry;
    }
    return nullptr;
}

Registry::Entry* Registry::FindMesh(void* const mesh) noexcept {
    if (mesh == nullptr) return nullptr;
    std::size_t bucket = HashPointer(mesh) & (meshBuckets_.size() - 1U);
    for (std::size_t probe = 0; probe < meshBuckets_.size(); ++probe) {
        const std::size_t stored = meshBuckets_[bucket];
        if (stored == 0) return nullptr;
        Entry& entry = entries_[stored - 1U];
        if (entry.mesh == mesh) return &entry;
        bucket = (bucket + 1U) & (meshBuckets_.size() - 1U);
    }
    return nullptr;
}

void Registry::RebuildMeshIndex() noexcept {
    meshBuckets_.fill(0);
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].enemy == nullptr || entries_[index].mesh == nullptr) {
            continue;
        }
        std::size_t bucket =
            HashPointer(entries_[index].mesh) & (meshBuckets_.size() - 1U);
        for (std::size_t probe = 0; probe < meshBuckets_.size(); ++probe) {
            if (meshBuckets_[bucket] == 0) {
                meshBuckets_[bucket] = index + 1U;
                break;
            }
            bucket = (bucket + 1U) & (meshBuckets_.size() - 1U);
        }
    }
}

StageObservation Registry::StageEnemy(void* const enemy) noexcept {
    void* mesh = nullptr;
    std::uint32_t type = 0;
    if (!ReadAt(enemy, kEnemyMeshOffset, mesh) || mesh == nullptr) {
        return {StageResult::invalidEnemy, 0};
    }
    if (!ReadAt(enemy, kEnemyTypeOffset, type) || type >= kEnemyTypeCount) {
        return {StageResult::invalidType, type};
    }
    std::uint8_t active = 0;
    if (!ReadAt(mesh, kEntityActiveOffset, active) || active > 1U) {
        return {StageResult::invalidEnemy, type};
    }
    if (active == 0U) return {StageResult::inactive, type};

    Entry* entry = FindEnemy(enemy);
    const bool registered = entry == nullptr;
    if (entry == nullptr) {
        for (Entry& candidate : entries_) {
            if (candidate.enemy == nullptr) {
                entry = &candidate;
                break;
            }
        }
    }
    if (entry == nullptr) return {StageResult::capacityExhausted, type};
    if (registered || entry->mesh != mesh || entry->type != type) {
        *entry = {};
        entry->enemy = enemy;
        entry->mesh = mesh;
        entry->type = type;
        RebuildMeshIndex();
        return {StageResult::registered, type};
    }
    return {StageResult::staged, type};
}

PoseObservation Registry::ObserveMesh(void* const mesh) noexcept {
    Entry* const entry = FindMesh(mesh);
    if (entry == nullptr) return {};

    pose_audit::Matrix* beginPointer = nullptr;
    pose_audit::Matrix* endPointer = nullptr;
    pose_audit::Matrix* capacityPointer = nullptr;
    if (!ReadAt(mesh, kBoneMatricesBeginOffset, beginPointer) ||
        !ReadAt(mesh, kBoneMatricesEndOffset, endPointer) ||
        !ReadAt(mesh, kBoneMatricesCapacityOffset, capacityPointer) ||
        beginPointer == nullptr || endPointer == nullptr ||
        capacityPointer == nullptr) {
        return {ObserveResult::unavailable, entry->type};
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(beginPointer);
    const auto end = reinterpret_cast<std::uintptr_t>(endPointer);
    const auto capacity = reinterpret_cast<std::uintptr_t>(capacityPointer);
    if (end < begin || capacity < end ||
        (end - begin) % sizeof(pose_audit::Matrix) != 0 ||
        (capacity - begin) % sizeof(pose_audit::Matrix) != 0) {
        return {ObserveResult::invalidVector, entry->type};
    }
    const std::size_t count = (end - begin) / sizeof(pose_audit::Matrix);
    const std::size_t capacityCount =
        (capacity - begin) / sizeof(pose_audit::Matrix);
    if (count == 0 || count > pose_audit::kMaximumBones ||
        capacityCount > pose_audit::kMaximumBones) {
        return {ObserveResult::invalidVector, entry->type, count};
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!pose_audit::IsValidMatrix(beginPointer[index])) {
            return {ObserveResult::invalidMatrix, entry->type, count, index};
        }
    }

    const std::size_t bytes = count * sizeof(pose_audit::Matrix);
    const bool identityChanged = !entry->hasPrevious ||
                                 entry->buffer != beginPointer ||
                                 entry->boneCount != count;
    const bool changed = !identityChanged &&
                         std::memcmp(entry->previous.data(), beginPointer,
                                     bytes) != 0;
    std::memcpy(entry->previous.data(), beginPointer, bytes);
    entry->buffer = beginPointer;
    entry->boneCount = count;
    entry->hasPrevious = true;
    return {identityChanged ? ObserveResult::identityChanged
                            : (changed ? ObserveResult::changed
                                       : ObserveResult::stable),
            entry->type, count};
}

const wchar_t* EnemyTypeName(const std::uint32_t type) noexcept {
    switch (type) {
    case 0: return L"grunt";
    case 1: return L"water";
    case 2: return L"manpig";
    case 3: return L"thomas";
    case 4: return L"child";
    default: return L"unknown";
    }
}

const wchar_t* StageResultName(const StageResult result) noexcept {
    switch (result) {
    case StageResult::registered: return L"registered";
    case StageResult::staged: return L"staged";
    case StageResult::inactive: return L"inactive";
    case StageResult::invalidEnemy: return L"invalid-enemy";
    case StageResult::invalidType: return L"invalid-type";
    case StageResult::capacityExhausted: return L"capacity-exhausted";
    }
    return L"unknown";
}

const wchar_t* ObserveResultName(const ObserveResult result) noexcept {
    switch (result) {
    case ObserveResult::notTarget: return L"not-target";
    case ObserveResult::unavailable: return L"unavailable";
    case ObserveResult::invalidVector: return L"invalid-vector";
    case ObserveResult::invalidMatrix: return L"invalid-matrix";
    case ObserveResult::identityChanged: return L"identity-changed";
    case ObserveResult::changed: return L"changed";
    case ObserveResult::stable: return L"stable";
    }
    return L"unknown";
}

} // namespace amfp::enemy_pose_audit
