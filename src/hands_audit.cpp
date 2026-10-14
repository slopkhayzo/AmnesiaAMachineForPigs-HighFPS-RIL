#include "hands_audit.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace amfp::hands_audit {
namespace {

constexpr std::size_t kHandsEntityOffset = 0x44;
constexpr std::size_t kCurrentHandObjectOffset = 0x5C;
constexpr std::size_t kEntityActiveOffset = 0x24;
constexpr std::size_t kHandObjectMeshEntityOffset = 0xF4;
constexpr std::size_t kHandObjectBillboardsBeginOffset = 0xF8;
constexpr std::size_t kHandObjectParticlesBeginOffset = 0x108;
constexpr std::size_t kHandObjectLightsBeginOffset = 0x118;
constexpr std::size_t kSubMeshOwnerOffset = 0x374;
constexpr std::size_t kMaximumAttachmentsPerVector = 32;

// Diagnostics-only, but still called from tick and render hooks. Every object
// read here is a hook receiver or is reached through source/retail-proven
// fields the engine itself dereferences, so no OS memory query is issued.
template <typename T>
bool ReadAt(const void* base, const std::size_t offset, T& value) noexcept {
    if (base == nullptr) {
        return false;
    }
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset,
                sizeof(value));
    return true;
}

bool ReadVectorCount(const void* base,
                     const std::size_t beginOffset,
                     std::uint32_t& count) noexcept {
    void* beginPointer = nullptr;
    void* endPointer = nullptr;
    if (!ReadAt(base, beginOffset, beginPointer) ||
        !ReadAt(base, beginOffset + sizeof(void*), endPointer)) {
        return false;
    }
    if (beginPointer == endPointer) {
        count = 0;
        return true;
    }
    if (beginPointer == nullptr || endPointer == nullptr) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(beginPointer);
    const auto end = reinterpret_cast<std::uintptr_t>(endPointer);
    if (end < begin || (end - begin) % sizeof(void*) != 0) {
        return false;
    }
    const std::size_t elements = (end - begin) / sizeof(void*);
    if (elements > kMaximumAttachmentsPerVector) {
        return false;
    }
    count = static_cast<std::uint32_t>(elements);
    return true;
}

void ObserveHandObject(void* const helper, Observation& observation) noexcept {
    if (!ReadAt(helper, kCurrentHandObjectOffset, observation.handObject)) {
        observation.handObjectResult = HandObjectResult::invalid;
        return;
    }
    if (observation.handObject == nullptr) {
        observation.handObjectResult = HandObjectResult::absent;
        return;
    }
    std::uint8_t active = 0;
    if (!ReadAt(observation.handObject, kHandObjectMeshEntityOffset,
                observation.handObjectEntity) ||
        observation.handObjectEntity == nullptr ||
        !ReadAt(observation.handObjectEntity, kEntityActiveOffset, active) ||
        active > 1U ||
        !ReadVectorCount(observation.handObject,
                         kHandObjectBillboardsBeginOffset,
                         observation.billboardCount) ||
        !ReadVectorCount(observation.handObject,
                         kHandObjectParticlesBeginOffset,
                         observation.particleSystemCount) ||
        !ReadVectorCount(observation.handObject, kHandObjectLightsBeginOffset,
                         observation.lightCount)) {
        observation.handObjectResult = HandObjectResult::invalid;
        return;
    }
    observation.handObjectResult = HandObjectResult::valid;
}

} // namespace

bool IsRigid(const Matrix& matrix) noexcept {
    for (const float value : matrix.values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    constexpr float epsilon = 2.0e-3F;
    if (std::abs(matrix.values[12]) > epsilon ||
        std::abs(matrix.values[13]) > epsilon ||
        std::abs(matrix.values[14]) > epsilon ||
        std::abs(matrix.values[15] - 1.0F) > epsilon) {
        return false;
    }
    for (int row = 0; row < 3; ++row) {
        float length = 0.0F;
        for (int column = 0; column < 3; ++column) {
            const float value = matrix.values[row * 4 + column];
            length += value * value;
        }
        if (std::abs(length - 1.0F) > epsilon) {
            return false;
        }
    }
    for (int first = 0; first < 3; ++first) {
        for (int second = first + 1; second < 3; ++second) {
            float dot = 0.0F;
            for (int column = 0; column < 3; ++column) {
                dot += matrix.values[first * 4 + column] *
                       matrix.values[second * 4 + column];
            }
            if (std::abs(dot) > epsilon) {
                return false;
            }
        }
    }
    const float determinant =
        matrix.values[0] * (matrix.values[5] * matrix.values[10] -
                            matrix.values[6] * matrix.values[9]) -
        matrix.values[1] * (matrix.values[4] * matrix.values[10] -
                            matrix.values[6] * matrix.values[8]) +
        matrix.values[2] * (matrix.values[4] * matrix.values[9] -
                            matrix.values[5] * matrix.values[8]);
    return std::abs(determinant - 1.0F) <= 4.0e-3F;
}

Observation Observe(void* const helper,
                    const WorldMatrixGetter getWorldMatrix) noexcept {
    Observation observation{};
    if (helper == nullptr) {
        return observation;
    }
    if (!ReadAt(helper, kHandsEntityOffset, observation.entity)) {
        observation.result = Result::invalidEntity;
        return observation;
    }
    if (observation.entity == nullptr) {
        observation.result = Result::missingEntity;
        return observation;
    }

    std::uint8_t active = 0;
    if (!ReadAt(observation.entity, kEntityActiveOffset, active) || active > 1U) {
        observation.result = Result::invalidEntity;
        return observation;
    }
    if (active == 0U) {
        observation.result = Result::inactive;
        return observation;
    }
    if (getWorldMatrix == nullptr) {
        observation.result = Result::invalidMatrix;
        return observation;
    }
    const Matrix* const world = getWorldMatrix(observation.entity);
    if (world == nullptr) {
        observation.result = Result::invalidMatrix;
        return observation;
    }
    std::memcpy(&observation.world, world, sizeof(observation.world));
    if (!IsRigid(observation.world)) {
        observation.result = Result::invalidMatrix;
        return observation;
    }
    ObserveHandObject(helper, observation);
    observation.result = Result::valid;
    return observation;
}

ModelResult ObserveModel(void* const subMesh,
                         const Matrix* const modelMatrix,
                         void* const handsEntity,
                         void* const handObjectEntity) noexcept {
    if (subMesh == nullptr) {
        return ModelResult::other;
    }
    void* owner = nullptr;
    std::memcpy(&owner,
                static_cast<const std::uint8_t*>(subMesh) +
                    kSubMeshOwnerOffset,
                sizeof(owner));
    ModelResult result = ModelResult::other;
    if (handsEntity != nullptr && owner == handsEntity) {
        result = ModelResult::handsRoot;
    } else if (handObjectEntity != nullptr && owner == handObjectEntity) {
        result = ModelResult::handObject;
    } else {
        return result;
    }
    if (modelMatrix == nullptr || !IsRigid(*modelMatrix)) {
        return ModelResult::invalidMatrix;
    }
    return result;
}

bool Changed(const Matrix& left, const Matrix& right) noexcept {
    constexpr float epsilon = 1.0e-5F;
    for (std::size_t index = 0; index < left.values.size(); ++index) {
        if (std::abs(left.values[index] - right.values[index]) > epsilon) {
            return true;
        }
    }
    return false;
}

const wchar_t* ResultName(const Result result) noexcept {
    switch (result) {
    case Result::valid: return L"valid";
    case Result::noHelper: return L"no-helper";
    case Result::missingEntity: return L"missing-entity";
    case Result::inactive: return L"inactive";
    case Result::invalidEntity: return L"invalid-entity";
    case Result::invalidMatrix: return L"invalid-matrix";
    }
    return L"unknown";
}

const wchar_t* HandObjectResultName(const HandObjectResult result) noexcept {
    switch (result) {
    case HandObjectResult::absent: return L"absent";
    case HandObjectResult::valid: return L"valid";
    case HandObjectResult::invalid: return L"invalid";
    }
    return L"unknown";
}

const wchar_t* ModelResultName(const ModelResult result) noexcept {
    switch (result) {
    case ModelResult::other: return L"other";
    case ModelResult::handsRoot: return L"hands-root";
    case ModelResult::handObject: return L"hand-object";
    case ModelResult::invalidMatrix: return L"invalid-matrix";
    }
    return L"unknown";
}

} // namespace amfp::hands_audit
