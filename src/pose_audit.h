#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace amfp::pose_audit {

constexpr std::size_t kMaximumBones = 128;

struct Matrix {
    std::array<float, 16> values{};
};

enum class Result : std::uint32_t {
    notTarget,
    unavailable,
    invalidVector,
    invalidMatrix,
    identityChanged,
    changed,
    stable,
};

struct Observation {
    Result result = Result::notTarget;
    std::size_t boneCount = 0;
    std::size_t invalidBoneIndex = 0;
    const Matrix* buffer = nullptr;
};

// Read-only tracker for the final cMeshEntity::mvBoneMatrices palette produced
// by UpdateGraphicsForFrame. It never writes to the mesh or palette.
class Tracker final {
public:
    [[nodiscard]] Observation Observe(void* mesh, void* targetMesh) noexcept;
    void Reset() noexcept;

private:
    void* mesh_ = nullptr;
    const Matrix* buffer_ = nullptr;
    std::size_t boneCount_ = 0;
    std::array<Matrix, kMaximumBones> previous_{};
    bool hasPrevious_ = false;
};

[[nodiscard]] bool IsValidMatrix(const Matrix& matrix) noexcept;
[[nodiscard]] const wchar_t* ResultName(Result result) noexcept;

} // namespace amfp::pose_audit
