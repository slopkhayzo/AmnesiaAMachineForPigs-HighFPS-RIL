#include "ragdoll_pose_audit.h"

#include <cstdint>
#include <cstring>

namespace amfp::ragdoll_pose_audit {
namespace {

constexpr std::size_t kSkeletonPhysicsOffset = 0x398;
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

} // namespace

Audit::Entry* Audit::FindOrInsert(void* const mesh) noexcept {
    Entry* freeEntry = nullptr;
    for (Entry& entry : entries_) {
        if (entry.mesh == mesh) return &entry;
        if (entry.mesh == nullptr && freeEntry == nullptr) freeEntry = &entry;
    }
    if (freeEntry != nullptr) freeEntry->mesh = mesh;
    return freeEntry;
}

Observation Audit::Observe(void* const mesh) noexcept {
    std::uint8_t skeletonPhysics = 0;
    if (!ReadAt(mesh, kSkeletonPhysicsOffset, skeletonPhysics)) {
        return {Result::unavailable};
    }
    if (skeletonPhysics > 1U) return {Result::invalidFlag};
    if (skeletonPhysics == 0U) return {};

    Entry* const entry = FindOrInsert(mesh);
    if (entry == nullptr) return {Result::capacityExhausted};

    pose_audit::Matrix* beginPointer = nullptr;
    pose_audit::Matrix* endPointer = nullptr;
    pose_audit::Matrix* capacityPointer = nullptr;
    if (!ReadAt(mesh, kBoneMatricesBeginOffset, beginPointer) ||
        !ReadAt(mesh, kBoneMatricesEndOffset, endPointer) ||
        !ReadAt(mesh, kBoneMatricesCapacityOffset, capacityPointer) ||
        beginPointer == nullptr || endPointer == nullptr ||
        capacityPointer == nullptr) {
        return {Result::unavailable};
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(beginPointer);
    const auto end = reinterpret_cast<std::uintptr_t>(endPointer);
    const auto capacity = reinterpret_cast<std::uintptr_t>(capacityPointer);
    if (end < begin || capacity < end ||
        (end - begin) % sizeof(pose_audit::Matrix) != 0 ||
        (capacity - begin) % sizeof(pose_audit::Matrix) != 0) {
        return {Result::invalidVector};
    }
    const std::size_t count = (end - begin) / sizeof(pose_audit::Matrix);
    const std::size_t capacityCount =
        (capacity - begin) / sizeof(pose_audit::Matrix);
    if (count == 0 || count > pose_audit::kMaximumBones ||
        capacityCount > pose_audit::kMaximumBones) {
        return {Result::invalidVector, count};
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!pose_audit::IsValidMatrix(beginPointer[index])) {
            return {Result::invalidMatrix, count, index};
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
    return {identityChanged ? Result::identityChanged
                            : (changed ? Result::changed : Result::stable),
            count};
}

const wchar_t* ResultName(const Result result) noexcept {
    switch (result) {
    case Result::notTarget: return L"not-target";
    case Result::unavailable: return L"unavailable";
    case Result::invalidFlag: return L"invalid-flag";
    case Result::invalidVector: return L"invalid-vector";
    case Result::invalidMatrix: return L"invalid-matrix";
    case Result::identityChanged: return L"identity-changed";
    case Result::changed: return L"changed";
    case Result::stable: return L"stable";
    case Result::capacityExhausted: return L"capacity-exhausted";
    }
    return L"unknown";
}

} // namespace amfp::ragdoll_pose_audit
