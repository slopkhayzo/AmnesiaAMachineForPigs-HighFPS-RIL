#pragma once

#include <cstdint>

namespace amfp::ragdoll_skinning {

enum class PrepareResult {
    notTarget,
    forced,
    unavailable,
    invalidFlag,
};

struct ProfileInfo {
    std::uint32_t vertexCount = 0;
    std::uint32_t boneCount = 0;
};

// Called from cSubMeshEntity::UpdateGraphicsForFrame on a receiver already
// validated by the retail executable mapping. Skeleton-physics meshes cache
// their CPU-skinned dynamic vertex buffer at the 60 Hz bone update count; make
// that cache stale so the original method consumes the render-time palette.
[[nodiscard]] PrepareResult PrepareForRender(void* subMesh) noexcept;
// The same cache invalidation for a skinned submesh the caller has already
// identified (an animated enemy whose palette is substituted per render).
// Returns false when the submesh has no dynamic vertex buffer.
bool ForceReskin(void* subMesh) noexcept;
[[nodiscard]] void* ReadOwner(void* subMesh) noexcept;
[[nodiscard]] bool IsCandidate(void* subMesh) noexcept;
[[nodiscard]] ProfileInfo ReadProfileInfo(void* subMesh) noexcept;

[[nodiscard]] const wchar_t* PrepareResultName(PrepareResult result) noexcept;

} // namespace amfp::ragdoll_skinning
