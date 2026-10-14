#include "pose_audit.h"

#include <cmath>
#include <cstring>

namespace amfp::pose_audit {
namespace {

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
                sizeof(value));
    return true;
}

} // namespace

bool IsValidMatrix(const Matrix& matrix) noexcept {
    for (const float value : matrix.values) {
        if (!std::isfinite(value) || std::abs(value) > 10000.0F) return false;
    }
    constexpr float epsilon = 2.0e-3F;
    if (std::abs(matrix.values[12]) > epsilon ||
        std::abs(matrix.values[13]) > epsilon ||
        std::abs(matrix.values[14]) > epsilon ||
        std::abs(matrix.values[15] - 1.0F) > epsilon) {
        return false;
    }
    const float determinant =
        matrix.values[0] * (matrix.values[5] * matrix.values[10] -
                            matrix.values[6] * matrix.values[9]) -
        matrix.values[1] * (matrix.values[4] * matrix.values[10] -
                            matrix.values[6] * matrix.values[8]) +
        matrix.values[2] * (matrix.values[4] * matrix.values[9] -
                            matrix.values[5] * matrix.values[8]);
    return std::isfinite(determinant) && std::abs(determinant) > 1.0e-5F;
}

Observation Tracker::Observe(void* const mesh, void* const targetMesh) noexcept {
    if (mesh == nullptr || targetMesh == nullptr || mesh != targetMesh) {
        return {};
    }

    Matrix* beginPointer = nullptr;
    Matrix* endPointer = nullptr;
    Matrix* capacityPointer = nullptr;
    if (!ReadAt(mesh, kBoneMatricesBeginOffset, beginPointer) ||
        !ReadAt(mesh, kBoneMatricesEndOffset, endPointer) ||
        !ReadAt(mesh, kBoneMatricesCapacityOffset, capacityPointer)) {
        return {Result::unavailable};
    }
    if (beginPointer == nullptr || endPointer == nullptr ||
        capacityPointer == nullptr) {
        return {Result::unavailable};
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(beginPointer);
    const auto end = reinterpret_cast<std::uintptr_t>(endPointer);
    const auto capacity = reinterpret_cast<std::uintptr_t>(capacityPointer);
    if (end < begin || capacity < end ||
        (end - begin) % sizeof(Matrix) != 0 ||
        (capacity - begin) % sizeof(Matrix) != 0) {
        return {Result::invalidVector};
    }
    const std::size_t count = (end - begin) / sizeof(Matrix);
    const std::size_t capacityCount = (capacity - begin) / sizeof(Matrix);
    if (count == 0 || count > kMaximumBones || capacityCount > kMaximumBones) {
        return {Result::invalidVector, count, 0, beginPointer};
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!IsValidMatrix(beginPointer[index])) {
            return {Result::invalidMatrix, count, index, beginPointer};
        }
    }

    const bool identityChanged = mesh_ != mesh || buffer_ != beginPointer ||
                                 boneCount_ != count || !hasPrevious_;
    const std::size_t bytes = count * sizeof(Matrix);
    const bool changed = !identityChanged &&
                         std::memcmp(previous_.data(), beginPointer, bytes) != 0;
    std::memcpy(previous_.data(), beginPointer, bytes);
    mesh_ = mesh;
    buffer_ = beginPointer;
    boneCount_ = count;
    hasPrevious_ = true;
    return {identityChanged ? Result::identityChanged
                            : (changed ? Result::changed : Result::stable),
            count, 0, beginPointer};
}

void Tracker::Reset() noexcept {
    mesh_ = nullptr;
    buffer_ = nullptr;
    boneCount_ = 0;
    hasPrevious_ = false;
}

const wchar_t* ResultName(const Result result) noexcept {
    switch (result) {
    case Result::notTarget: return L"not-target";
    case Result::unavailable: return L"unavailable";
    case Result::invalidVector: return L"invalid-vector";
    case Result::invalidMatrix: return L"invalid-matrix";
    case Result::identityChanged: return L"identity-changed";
    case Result::changed: return L"changed";
    case Result::stable: return L"stable";
    }
    return L"unknown";
}

} // namespace amfp::pose_audit
