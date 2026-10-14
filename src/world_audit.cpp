#include "world_audit.h"

#include <Windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace amfp::world_audit {
namespace {

// GOG 51194033869387641, corroborated against the released AMFP source and
// the retail iLuxProp constructor/accessors.
constexpr std::size_t kStaticPhysicsOffset = 0x1DD;
constexpr std::size_t kBodiesBeginOffset = 0x348;
constexpr std::size_t kBodiesEndOffset = 0x34C;
constexpr std::size_t kMeshEntityOffset = 0x428;
constexpr std::size_t kPropTypeOffset = 0x56C;
constexpr std::size_t kMaximumBodies = 128;

bool IsReadable(const void* address, const std::size_t bytes) noexcept {
    if (address == nullptr || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY;
    if ((memory.Protect & readable) == 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + memory.RegionSize;
    return begin >= regionBegin && begin <= regionEnd && bytes <= regionEnd - begin;
}

template <typename T>
bool ReadAt(const void* base, const std::size_t offset, T& value) noexcept {
    if (base == nullptr) return false;
    const auto* address = static_cast<const std::uint8_t*>(base) + offset;
    if (!IsReadable(address, sizeof(value))) return false;
    std::memcpy(&value, address, sizeof(value));
    return true;
}

bool ReadBodyCount(const void* prop, std::uint32_t& count) noexcept {
    void* beginPointer = nullptr;
    void* endPointer = nullptr;
    if (!ReadAt(prop, kBodiesBeginOffset, beginPointer) ||
        !ReadAt(prop, kBodiesEndOffset, endPointer)) {
        return false;
    }
    if (beginPointer == endPointer) {
        count = 0;
        return true;
    }
    if (beginPointer == nullptr || endPointer == nullptr) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(beginPointer);
    const auto end = reinterpret_cast<std::uintptr_t>(endPointer);
    if (end < begin || (end - begin) % sizeof(void*) != 0) return false;
    const std::size_t elements = (end - begin) / sizeof(void*);
    if (elements > kMaximumBodies || !IsReadable(beginPointer, end - begin)) {
        return false;
    }
    count = static_cast<std::uint32_t>(elements);
    return true;
}

std::size_t HashPointer(const void* pointer) noexcept {
    std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer);
    value >>= 4U;
    value ^= value >> 11U;
    return static_cast<std::size_t>(value);
}

} // namespace

bool IsAffine(const Matrix& matrix) noexcept {
    for (const float value : matrix.values) {
        if (!std::isfinite(value)) return false;
    }
    constexpr float epsilon = 2.0e-3F;
    if (std::abs(matrix.values[12]) > epsilon ||
        std::abs(matrix.values[13]) > epsilon ||
        std::abs(matrix.values[14]) > epsilon ||
        std::abs(matrix.values[15] - 1.0F) > epsilon) {
        return false;
    }
    for (int row = 0; row < 3; ++row) {
        float lengthSquared = 0.0F;
        for (int column = 0; column < 3; ++column) {
            const float value = matrix.values[row * 4 + column];
            lengthSquared += value * value;
        }
        if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-10F ||
            lengthSquared > 1.0e10F) {
            return false;
        }
    }
    return true;
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

Observation Tracker::Observe(void* const prop,
                             const WorldMatrixGetter getWorldMatrix) noexcept {
    Observation observation{};
    std::uint8_t staticPhysics = 0;
    void* meshEntity = nullptr;
    if (prop == nullptr ||
        !ReadAt(prop, kPropTypeOffset, observation.propType) ||
        observation.propType >= kPropTypeCount ||
        !ReadAt(prop, kStaticPhysicsOffset, staticPhysics) ||
        staticPhysics > 1U ||
        !ReadBodyCount(prop, observation.bodyCount) ||
        !ReadAt(prop, kMeshEntityOffset, meshEntity)) {
        observation.result = Result::invalidLayout;
        return observation;
    }
    observation.staticPhysics = staticPhysics != 0;
    if (meshEntity == nullptr) {
        observation.result = Result::missingMesh;
        return observation;
    }
    if (getWorldMatrix == nullptr) {
        observation.result = Result::invalidMatrix;
        return observation;
    }
    const Matrix* const world = getWorldMatrix(meshEntity);
    if (!IsReadable(world, sizeof(*world))) {
        observation.result = Result::invalidMatrix;
        return observation;
    }
    Matrix current{};
    std::memcpy(&current, world, sizeof(current));
    if (!IsAffine(current)) {
        observation.result = Result::invalidMatrix;
        return observation;
    }

    const std::size_t start = HashPointer(prop) % entries_.size();
    for (std::size_t probe = 0; probe < entries_.size(); ++probe) {
        Entry& entry = entries_[(start + probe) % entries_.size()];
        if (entry.prop == nullptr) {
            entry.prop = prop;
            entry.world = current;
            observation.newIdentity = true;
            observation.result = Result::valid;
            return observation;
        }
        if (entry.prop == prop) {
            observation.changed = Changed(entry.world, current);
            entry.world = current;
            observation.result = Result::valid;
            return observation;
        }
    }
    observation.result = Result::trackerFull;
    return observation;
}

const wchar_t* ResultName(const Result result) noexcept {
    switch (result) {
    case Result::valid: return L"valid";
    case Result::missingMesh: return L"missing-mesh";
    case Result::invalidLayout: return L"invalid-layout";
    case Result::invalidMatrix: return L"invalid-matrix";
    case Result::trackerFull: return L"tracker-full";
    }
    return L"unknown";
}

const wchar_t* PropTypeName(const std::uint32_t propType) noexcept {
    constexpr std::array<const wchar_t*, kPropTypeCount> names{
        L"object", L"swingDoor", L"lever", L"wheel", L"lamp",
        L"photocell", L"moveObject", L"item", L"chest", L"critter",
        L"levelDoor", L"button", L"emotionStone", L"npc", L"multiSlider"};
    return propType < names.size() ? names[propType] : L"unknown";
}

} // namespace amfp::world_audit
