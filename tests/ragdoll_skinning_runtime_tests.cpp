#include "ragdoll_skinning_runtime.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

template <typename T, std::size_t Size>
void Write(std::array<std::uint8_t, Size>& memory,
           const std::size_t offset,
           const T& value) {
    std::memcpy(memory.data() + offset, &value, sizeof(value));
}

template <typename T, std::size_t Size>
T Read(const std::array<std::uint8_t, Size>& memory,
       const std::size_t offset) {
    T value{};
    std::memcpy(&value, memory.data() + offset, sizeof(value));
    return value;
}

bool Require(const bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

} // namespace

int main() {
    using amfp::ragdoll_skinning::PrepareForRender;
    using amfp::ragdoll_skinning::PrepareResult;
    using amfp::ragdoll_skinning::IsCandidate;
    using amfp::ragdoll_skinning::ReadProfileInfo;

    std::array<std::uint8_t, 0x400> owner{};
    std::array<std::uint8_t, 0x400> subMesh{};
    void* ownerPointer = owner.data();
    void* dynamicVertexBuffer = reinterpret_cast<void*>(0x1234U);
    Write(subMesh, 0x374, ownerPointer);
    Write(subMesh, 0x384, dynamicVertexBuffer);

    if (!Require(PrepareForRender(nullptr) == PrepareResult::unavailable,
                 "null submesh must fail closed")) return 1;
    if (!Require(amfp::ragdoll_skinning::ReadOwner(nullptr) == nullptr &&
                     amfp::ragdoll_skinning::ReadOwner(subMesh.data()) ==
                         ownerPointer,
                 "submesh owner mapping is wrong")) return 1;
    const auto nullProfile = ReadProfileInfo(nullptr);
    if (!Require(nullProfile.vertexCount == 0 && nullProfile.boneCount == 0,
                 "null profile target must return empty metadata")) return 1;
    if (!Require(PrepareForRender(subMesh.data()) == PrepareResult::notTarget,
                 "ordinary dynamic mesh must remain untouched")) return 1;
    if (!Require(!IsCandidate(subMesh.data()),
                 "ordinary dynamic mesh was classified as a candidate")) {
        return 1;
    }

    const std::uint8_t invalidFlag = 2;
    Write(owner, 0x398, invalidFlag);
    if (!Require(PrepareForRender(subMesh.data()) == PrepareResult::invalidFlag,
                 "invalid skeleton-physics flag must fail closed")) return 1;

    const std::uint8_t activeFlag = 1;
    const std::uint8_t updated = 1;
    const std::int32_t updateCount = 37;
    Write(owner, 0x398, activeFlag);
    Write(owner, 0x2C8, updateCount);
    Write(subMesh, 0x39E, updated);
    Write(subMesh, 0x3A0, updateCount);
    if (!Require(IsCandidate(subMesh.data()),
                 "active skeleton-physics mesh was not classified")) return 1;
    if (!Require(PrepareForRender(subMesh.data()) == PrepareResult::forced,
                 "active skeleton-physics mesh must be forced stale")) return 1;
    if (!Require(Read<std::uint8_t>(subMesh, 0x39E) == 0,
                 "graphics-updated cache flag was not cleared")) return 1;
    if (!Require(Read<std::int32_t>(subMesh, 0x3A0) != updateCount,
                 "bone update cache count was not made stale")) return 1;

    dynamicVertexBuffer = nullptr;
    Write(subMesh, 0x384, dynamicVertexBuffer);
    Write(subMesh, 0x39E, updated);
    if (!Require(PrepareForRender(subMesh.data()) == PrepareResult::notTarget,
                 "static submesh must remain untouched")) return 1;
    if (!Require(Read<std::uint8_t>(subMesh, 0x39E) == updated,
                 "static submesh cache was modified")) return 1;

    return 0;
}
