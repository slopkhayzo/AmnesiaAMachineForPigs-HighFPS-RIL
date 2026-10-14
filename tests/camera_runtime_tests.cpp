#include "camera_runtime.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

template <typename T, std::size_t N>
void Write(std::array<std::uint8_t, N>& bytes, std::size_t offset, const T& value) {
    std::memcpy(bytes.data()+offset, &value, sizeof(value));
}

template <typename T, std::size_t N>
T Read(const std::array<std::uint8_t, N>& bytes, std::size_t offset) {
    T value{}; std::memcpy(&value, bytes.data()+offset, sizeof(value)); return value;
}

void TestScopedApplyAndRestore() {
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
    const std::uint32_t eulerMode = 0;
    Write(camera, 0x44, eulerMode);

    amfp::camera::Pose first{{0.0F, 1.0F, 2.0F}, 0.1F, 0.2F, 0.3F};
    Write(camera, 0x00, first.position); Write(camera, 0x28, first.pitch);
    Write(camera, 0x2C, first.yaw); Write(camera, 0x30, first.roll);
    amfp::camera::Runtime runtime;
    runtime.Configure({reinterpret_cast<void*>(1), reinterpret_cast<void*>(1),
                       reinterpret_cast<void*>(1), reinterpret_cast<void*>(1)});
    amfp::clock::Sample warmup{}; warmup.generation = 7;
    Require(runtime.Apply(scene.data(), 0, warmup) ==
                amfp::camera::FrameResult::identityChanged,
            "identity warmup failed");
    Require(runtime.CaptureCompletedTick(1, 7) == amfp::camera::FrameResult::captured,
            "first endpoint not captured");

    amfp::camera::Pose second{{2.0F, 3.0F, 4.0F}, 0.2F, 0.4F, 0.6F};
    Write(camera, 0x00, second.position); Write(camera, 0x28, second.pitch);
    Write(camera, 0x2C, second.yaw); Write(camera, 0x30, second.roll);
    Require(runtime.CaptureCompletedTick(2, 7) == amfp::camera::FrameResult::captured,
            "second endpoint not captured");
    const auto authoritative = camera;

    amfp::clock::Sample sample{};
    sample.kind = amfp::clock::SampleKind::interpolated;
    sample.generation = 7; sample.olderSerial = 1; sample.newerSerial = 2;
    sample.alpha = 0.5;
    Require(runtime.Apply(scene.data(), 2, sample) == amfp::camera::FrameResult::applied,
            "valid interpolation not applied");
    Require(runtime.OverrideActive() && std::abs(Read<float>(camera, 0x00)-1.0F) < 1.0e-5F,
            "temporary pose incorrect");
    std::memset(camera.data()+0x4C, 0xA5, 64*3);
    std::memset(camera.data()+0x354, 0x5A, 0x4FC+5);
    camera[0x10C] = 0xE7;
    Require(runtime.Restore(), "restoration failed");
    Require(!runtime.OverrideActive(), "override survived restoration");
    Require(std::memcmp(camera.data(), authoritative.data(), 0x0C) == 0 &&
                std::memcmp(camera.data()+0x28, authoritative.data()+0x28, 0x0C) == 0,
            "authoritative pose not restored");
    Require(std::memcmp(camera.data()+0x4C, authoritative.data()+0x4C, 64*3) == 0 &&
                std::memcmp(camera.data()+0x354, authoritative.data()+0x354, 0x4FC+5) == 0,
            "camera caches not restored");
    Require(camera[0x10C] == 0xE7,
            "renderer-owned previous-view history was rolled back");
}

} // namespace

int main() {
    TestScopedApplyAndRestore();
    std::cout << "camera runtime tests passed\n";
    return EXIT_SUCCESS;
}
