#include "camera_audit.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

template <typename T, std::size_t Size>
void Write(std::array<std::uint8_t, Size>& bytes,
           const std::size_t offset,
           const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

bool Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::array<std::uint8_t, 0x48> scene{};
    std::array<std::uint8_t, 0x0F> viewport{};
    std::array<std::uint8_t, 0x855> camera{};
    void* viewportPointer = viewport.data();
    void* cameraPointer = camera.data();
    void* worldPointer = scene.data();
    Write(scene, 0x40, viewportPointer);
    Write(viewport, 0x04, cameraPointer);
    Write(viewport, 0x08, worldPointer);
    viewport[0x0D] = 1;

    const amfp::camera_audit::Vector3 position{1.0F, 2.0F, 3.0F};
    Write(camera, 0x00, position);
    const float pitch = 0.1F;
    const float yaw = 0.2F;
    const float roll = 0.3F;
    Write(camera, 0x28, pitch);
    Write(camera, 0x2C, yaw);
    Write(camera, 0x30, roll);
    const float pitchMin = -1.5F;
    const float pitchMax = 1.5F;
    Write(camera, 0x34, pitchMin);
    Write(camera, 0x38, pitchMax);
    const std::uint32_t eulerMode = 0;
    Write(camera, 0x44, eulerMode);

    const auto valid = amfp::camera_audit::Observe(scene.data());
    if (!Require(valid.result == amfp::camera_audit::Result::validEuler,
                 "valid Euler camera chain was rejected") ||
        !Require(valid.camera == camera.data() && valid.world == scene.data(),
                 "camera identity was not returned") ||
        !Require(valid.pose.position.x == 1.0F && valid.pose.yaw == yaw,
                 "camera pose was read from the wrong offsets")) {
        return 1;
    }

    const std::uint32_t matrixMode = 1;
    Write(camera, 0x44, matrixMode);
    if (!Require(amfp::camera_audit::Observe(scene.data()).result ==
                     amfp::camera_audit::Result::validMatrix,
                 "matrix rotation mode was not classified safely")) {
        return 2;
    }

    viewport[0x0D] = 0;
    if (!Require(amfp::camera_audit::Observe(scene.data()).result ==
                     amfp::camera_audit::Result::hidden,
                 "hidden viewport was not diagnosed")) {
        return 3;
    }
    viewport[0x0D] = 1;
    camera[0x851] = 2;
    if (!Require(amfp::camera_audit::Observe(scene.data()).result ==
                     amfp::camera_audit::Result::invalidCamera,
                 "invalid camera dirty flag was accepted")) {
        return 4;
    }

    std::cout << "read-only camera layout audit tests passed\n";
    return 0;
}
