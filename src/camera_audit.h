#pragma once

#include <cstdint>

namespace amfp::camera_audit {

struct Vector3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Pose {
    Vector3 position{};
    float pitch = 0.0F;
    float yaw = 0.0F;
    float roll = 0.0F;
};

enum class Result : std::uint32_t {
    validEuler,
    validMatrix,
    noScene,
    noCurrentListener,
    hidden,
    missingCamera,
    missingWorld,
    invalidViewport,
    invalidCamera,
};

struct Observation {
    Result result = Result::noScene;
    void* viewport = nullptr;
    void* world = nullptr;
    void* camera = nullptr;
    Pose pose{};
};

// Read-only validation of the source-matched cScene -> cViewport -> cCamera
// chain. This never invokes engine code and never writes game memory.
[[nodiscard]] Observation Observe(void* scene) noexcept;
[[nodiscard]] const wchar_t* ResultName(Result result) noexcept;

} // namespace amfp::camera_audit
