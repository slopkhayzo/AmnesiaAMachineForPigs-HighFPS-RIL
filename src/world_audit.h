#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace amfp::world_audit {

struct Matrix {
    std::array<float, 16> values{};
};

using WorldMatrixGetter = const Matrix* (*)(void* entity);

constexpr std::size_t kPropTypeCount = 15;

enum class Result : std::uint32_t {
    valid,
    missingMesh,
    invalidLayout,
    invalidMatrix,
    trackerFull,
};

struct Observation {
    Result result = Result::invalidLayout;
    std::uint32_t propType = 0;
    std::uint32_t bodyCount = 0;
    bool staticPhysics = false;
    bool newIdentity = false;
    bool changed = false;
};

class Tracker final {
public:
    [[nodiscard]] Observation Observe(void* prop,
                                      WorldMatrixGetter getWorldMatrix) noexcept;

private:
    struct Entry {
        void* prop = nullptr;
        Matrix world{};
    };
    static constexpr std::size_t kCapacity = 2048;
    std::array<Entry, kCapacity> entries_{};
};

[[nodiscard]] bool IsAffine(const Matrix& matrix) noexcept;
[[nodiscard]] bool Changed(const Matrix& left, const Matrix& right) noexcept;
[[nodiscard]] const wchar_t* ResultName(Result result) noexcept;
[[nodiscard]] const wchar_t* PropTypeName(std::uint32_t propType) noexcept;

} // namespace amfp::world_audit
