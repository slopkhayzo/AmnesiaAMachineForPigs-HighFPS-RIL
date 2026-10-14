#include "camera_audit.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace amfp::camera_audit {
namespace {

constexpr std::size_t kSceneCurrentListenerOffset = 0x40;
constexpr std::size_t kViewportCameraOffset = 0x04;
constexpr std::size_t kViewportWorldOffset = 0x08;
constexpr std::size_t kViewportVisibleOffset = 0x0D;
constexpr std::size_t kCameraPositionOffset = 0x00;
constexpr std::size_t kCameraAnglesOffset = 0x28;
constexpr std::size_t kCameraFlagsOffset = 0x850;

struct CameraAngles {
    float pitch = 0.0F;
    float yaw = 0.0F;
    float roll = 0.0F;
    float pitchMin = 0.0F;
    float pitchMax = 0.0F;
    float yawMin = 0.0F;
    float yawMax = 0.0F;
    std::uint32_t rotateMode = 0;
};

static_assert(sizeof(CameraAngles) == 0x20);

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

bool IsFinite(const float value) noexcept {
    return std::isfinite(value) && std::abs(value) <= 1.0e6F;
}

bool IsFinite(const Pose& pose) noexcept {
    return IsFinite(pose.position.x) && IsFinite(pose.position.y) &&
           IsFinite(pose.position.z) && IsFinite(pose.pitch) &&
           IsFinite(pose.yaw) && IsFinite(pose.roll);
}

bool IsFinite(const CameraAngles& angles) noexcept {
    return IsFinite(angles.pitch) && IsFinite(angles.yaw) &&
           IsFinite(angles.roll) && IsFinite(angles.pitchMin) &&
           IsFinite(angles.pitchMax) && IsFinite(angles.yawMin) &&
           IsFinite(angles.yawMax);
}

} // namespace

Observation Observe(void* scene) noexcept {
    Observation observation{};
    if (scene == nullptr) {
        return observation;
    }
    if (!ReadAt(scene, kSceneCurrentListenerOffset, observation.viewport)) {
        observation.result = Result::invalidViewport;
        return observation;
    }
    if (observation.viewport == nullptr) {
        observation.result = Result::noCurrentListener;
        return observation;
    }

    std::uint8_t visible = 0;
    if (!ReadAt(observation.viewport, kViewportVisibleOffset, visible) ||
        visible > 1U) {
        observation.result = Result::invalidViewport;
        return observation;
    }
    if (visible == 0U) {
        observation.result = Result::hidden;
        return observation;
    }
    if (!ReadAt(observation.viewport, kViewportCameraOffset,
                observation.camera)) {
        observation.result = Result::invalidViewport;
        return observation;
    }
    if (observation.camera == nullptr) {
        observation.result = Result::missingCamera;
        return observation;
    }
    if (!ReadAt(observation.viewport, kViewportWorldOffset, observation.world)) {
        observation.result = Result::invalidViewport;
        return observation;
    }
    if (observation.world == nullptr) {
        observation.result = Result::missingWorld;
        return observation;
    }

    CameraAngles angles{};
    std::uint8_t flags[5]{};
    if (!ReadAt(observation.camera, kCameraPositionOffset,
                observation.pose.position) ||
        !ReadAt(observation.camera, kCameraAnglesOffset, angles) ||
        !ReadAt(observation.camera, kCameraFlagsOffset, flags) ||
        angles.rotateMode > 1U) {
        observation.result = Result::invalidCamera;
        return observation;
    }
    for (const std::uint8_t flag : flags) {
        if (flag > 1U) {
            observation.result = Result::invalidCamera;
            return observation;
        }
    }
    observation.pose.pitch = angles.pitch;
    observation.pose.yaw = angles.yaw;
    observation.pose.roll = angles.roll;
    if (!IsFinite(observation.pose) || !IsFinite(angles)) {
        observation.result = Result::invalidCamera;
        return observation;
    }
    observation.result = angles.rotateMode == 0U
        ? Result::validEuler
        : Result::validMatrix;
    return observation;
}

const wchar_t* ResultName(const Result result) noexcept {
    switch (result) {
    case Result::validEuler: return L"valid-euler";
    case Result::validMatrix: return L"valid-matrix";
    case Result::noScene: return L"no-scene";
    case Result::noCurrentListener: return L"no-current-listener";
    case Result::hidden: return L"hidden";
    case Result::missingCamera: return L"missing-camera";
    case Result::missingWorld: return L"missing-world";
    case Result::invalidViewport: return L"invalid-viewport";
    case Result::invalidCamera: return L"invalid-camera";
    }
    return L"unknown";
}

} // namespace amfp::camera_audit
