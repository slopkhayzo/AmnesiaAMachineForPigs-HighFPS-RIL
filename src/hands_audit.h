#pragma once

#include <array>
#include <cstdint>

namespace amfp::hands_audit {

struct Matrix {
    std::array<float, 16> values{};
};

using WorldMatrixGetter = const Matrix* (*)(void* entity);

enum class Result : std::uint32_t {
    valid,
    noHelper,
    missingEntity,
    inactive,
    invalidEntity,
    invalidMatrix,
};

enum class HandObjectResult : std::uint32_t {
    absent,
    valid,
    invalid,
};

enum class ModelResult : std::uint32_t {
    other,
    handsRoot,
    handObject,
    invalidMatrix,
};

struct Observation {
    Result result = Result::noHelper;
    void* entity = nullptr;
    Matrix world{};
    HandObjectResult handObjectResult = HandObjectResult::absent;
    void* handObject = nullptr;
    void* handObjectEntity = nullptr;
    std::uint32_t billboardCount = 0;
    std::uint32_t particleSystemCount = 0;
    std::uint32_t lightCount = 0;
};

// Read-only validation of the source- and binary-matched
// cLuxPlayerHands::mpHandsEntity path. The supplied getter may refresh HPL's
// normal lazy world-transform cache, but this audit never substitutes or
// writes a hand transform.
[[nodiscard]] Observation Observe(void* helper,
                                  WorldMatrixGetter getWorldMatrix) noexcept;
// Called only from the mapped cSubMeshEntity::GetModelMatrix method, so the
// receiver is already a live cSubMeshEntity from the compatible retail image.
[[nodiscard]] ModelResult ObserveModel(void* subMesh,
                                       const Matrix* modelMatrix,
                                       void* handsEntity,
                                       void* handObjectEntity) noexcept;
[[nodiscard]] bool IsRigid(const Matrix& matrix) noexcept;
[[nodiscard]] bool Changed(const Matrix& left, const Matrix& right) noexcept;
[[nodiscard]] const wchar_t* ResultName(Result result) noexcept;
[[nodiscard]] const wchar_t* HandObjectResultName(
    HandObjectResult result) noexcept;
[[nodiscard]] const wchar_t* ModelResultName(ModelResult result) noexcept;

} // namespace amfp::hands_audit
