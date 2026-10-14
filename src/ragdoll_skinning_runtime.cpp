#include "ragdoll_skinning_runtime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace amfp::ragdoll_skinning {
namespace {

constexpr std::size_t kSubMeshOwnerOffset = 0x374;
constexpr std::size_t kDynamicVertexBufferOffset = 0x384;
constexpr std::size_t kGraphicsUpdatedOffset = 0x39E;
constexpr std::size_t kCachedBoneUpdateCountOffset = 0x3A0;
constexpr std::size_t kOwnerBoneUpdateCountOffset = 0x2C8;
constexpr std::size_t kOwnerSkeletonPhysicsOffset = 0x398;
constexpr std::size_t kOwnerBoneMatricesBeginOffset = 0x388;
constexpr std::size_t kOwnerBoneMatricesEndOffset = 0x38C;
constexpr std::size_t kBoneMatrixBytes = 64;
constexpr std::size_t kMaximumProfileBoneCount = 512;
constexpr std::size_t kVertexBufferGetVertexNumVtableOffset = 0x5C;

template <typename T>
T ReadMappedField(const void* const base, const std::size_t offset) noexcept {
    T value{};
    const auto* address =
        static_cast<const std::uint8_t*>(base) + offset;
    std::memcpy(&value, address, sizeof(T));
    return value;
}

template <typename T>
void WriteMappedField(void* const base,
                      const std::size_t offset,
                      const T& value) noexcept {
    auto* address = static_cast<std::uint8_t*>(base) + offset;
    std::memcpy(address, &value, sizeof(T));
}

} // namespace

void* ReadOwner(void* const subMesh) noexcept {
    return subMesh == nullptr
        ? nullptr
        : ReadMappedField<void*>(subMesh, kSubMeshOwnerOffset);
}

bool IsCandidate(void* const subMesh) noexcept {
    if (subMesh == nullptr) return false;
    void* const owner = ReadOwner(subMesh);
    if (owner == nullptr ||
        ReadMappedField<void*>(subMesh, kDynamicVertexBufferOffset) == nullptr) {
        return false;
    }
    return ReadMappedField<std::uint8_t>(
               owner, kOwnerSkeletonPhysicsOffset) == 1U;
}

ProfileInfo ReadProfileInfo(void* const subMesh) noexcept {
    ProfileInfo info{};
    if (subMesh == nullptr) return info;
    void* const owner = ReadOwner(subMesh);
    void* const vertexBuffer =
        ReadMappedField<void*>(subMesh, kDynamicVertexBufferOffset);
    if (owner == nullptr || vertexBuffer == nullptr) return info;

    const std::uintptr_t boneBegin = ReadMappedField<std::uintptr_t>(
        owner, kOwnerBoneMatricesBeginOffset);
    const std::uintptr_t boneEnd = ReadMappedField<std::uintptr_t>(
        owner, kOwnerBoneMatricesEndOffset);
    if (boneBegin != 0 && boneEnd >= boneBegin &&
        (boneEnd - boneBegin) % kBoneMatrixBytes == 0) {
        const std::size_t boneCount =
            (boneEnd - boneBegin) / kBoneMatrixBytes;
        if (boneCount <= kMaximumProfileBoneCount) {
            info.boneCount = static_cast<std::uint32_t>(boneCount);
        }
    }

    void** const vtable = ReadMappedField<void**>(vertexBuffer, 0);
    if (vtable == nullptr) return info;
    void* const getVertexNumAddress = ReadMappedField<void*>(
        vtable, kVertexBufferGetVertexNumVtableOffset);
    if (getVertexNumAddress == nullptr) return info;
    using GetVertexNum = int(__thiscall*)(void*);
    const int vertexCount =
        reinterpret_cast<GetVertexNum>(getVertexNumAddress)(vertexBuffer);
    if (vertexCount > 0) {
        info.vertexCount = static_cast<std::uint32_t>(vertexCount);
    }
    return info;
}

PrepareResult PrepareForRender(void* const subMesh) noexcept {
    if (subMesh == nullptr) return PrepareResult::unavailable;
    void* const owner = ReadOwner(subMesh);
    if (owner == nullptr) return PrepareResult::unavailable;
    void* const dynamicVertexBuffer =
        ReadMappedField<void*>(subMesh, kDynamicVertexBufferOffset);
    if (dynamicVertexBuffer == nullptr) return PrepareResult::notTarget;

    const std::uint8_t skeletonPhysics = ReadMappedField<std::uint8_t>(
        owner, kOwnerSkeletonPhysicsOffset);
    if (skeletonPhysics > 1U) return PrepareResult::invalidFlag;
    if (skeletonPhysics == 0U) return PrepareResult::notTarget;

    const std::int32_t ownerUpdateCount = ReadMappedField<std::int32_t>(
        owner, kOwnerBoneUpdateCountOffset);
    const std::uint8_t graphicsUpdated = 0;
    const std::int32_t staleUpdateCount = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(ownerUpdateCount) ^ 0x80000000U);
    WriteMappedField(subMesh, kGraphicsUpdatedOffset, graphicsUpdated);
    WriteMappedField(subMesh, kCachedBoneUpdateCountOffset, staleUpdateCount);
    return PrepareResult::forced;
}

bool ForceReskin(void* const subMesh) noexcept {
    if (subMesh == nullptr) return false;
    void* const owner = ReadOwner(subMesh);
    if (owner == nullptr ||
        ReadMappedField<void*>(subMesh, kDynamicVertexBufferOffset) == nullptr) {
        return false;
    }
    const std::int32_t ownerUpdateCount = ReadMappedField<std::int32_t>(
        owner, kOwnerBoneUpdateCountOffset);
    const std::uint8_t graphicsUpdated = 0;
    const std::int32_t staleUpdateCount = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(ownerUpdateCount) ^ 0x80000000U);
    WriteMappedField(subMesh, kGraphicsUpdatedOffset, graphicsUpdated);
    WriteMappedField(subMesh, kCachedBoneUpdateCountOffset, staleUpdateCount);
    return true;
}

const wchar_t* PrepareResultName(const PrepareResult result) noexcept {
    switch (result) {
    case PrepareResult::notTarget: return L"not-target";
    case PrepareResult::forced: return L"forced";
    case PrepareResult::unavailable: return L"unavailable";
    case PrepareResult::invalidFlag: return L"invalid-flag";
    }
    return L"unknown";
}

} // namespace amfp::ragdoll_skinning
