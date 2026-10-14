#pragma once

#include "camera_interpolation.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace amfp::camera {

struct Functions {
    void* setPosition = nullptr;
    void* setPitch = nullptr;
    void* setYaw = nullptr;
    void* setRoll = nullptr;
};

struct AngularLimits {
    float pitchMin = 0.0F;
    float pitchMax = 0.0F;
    float yawMin = 0.0F;
    float yawMax = 0.0F;
};

enum class FrameResult : std::uint32_t {
    captured,
    applied,
    noScene,
    noCurrentListener,
    listenerHidden,
    listenerMissingCamera,
    listenerMissingWorld,
    invalidViewport,
    identityChanged,
    invalidCamera,
    clockGenerationReset,
    nonMonotonicReset,
    sameTickMutation,
    discontinuityReset,
    clockUnavailable,
    snapshotsMissing,
    applyFailed,
};

class Runtime final {
  public:
    void Configure(const Functions& functions) noexcept;
    [[nodiscard]] FrameResult CaptureCompletedTick(std::uint64_t tickSerial,
                                                   std::uint64_t generation) noexcept;
    [[nodiscard]] FrameResult Apply(void* scene, std::uint64_t tickSerial,
                                    const clock::Sample& sample) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    [[nodiscard]] bool OverrideActive() const noexcept { return overrideActive_; }
    // The camera currently holding a render-only pose, or null.
    [[nodiscard]] void* ActiveCamera() const noexcept {
        return overrideActive_ ? overriddenCamera_ : nullptr;
    }
    // Rotates the render-only pose further, honouring the authoritative
    // camera's angular limits exactly as cCamera::SetPitch/SetYaw would.
    // Valid only between Apply() and Restore().
    [[nodiscard]] bool ApplyAngularOffset(float pitchDelta, float yawDelta,
                                          Pose* before = nullptr,
                                          Pose* after = nullptr) noexcept;

  private:
    enum class SelectResult : std::uint32_t {
        selected, noCurrentListener, hidden, missingCamera, missingWorld, invalidLayout,
    };
    struct Identity { void* viewport = nullptr; void* world = nullptr; void* camera = nullptr; };
    struct SavedState {
        Pose pose{};
        AngularLimits angularLimits{};
        std::array<std::uint8_t, 64> viewMatrix{};
        std::array<std::uint8_t, 64> projectionMatrix{};
        std::array<std::uint8_t, 64> moveMatrix{};
        std::array<std::uint8_t, 0x4FC> frustum{};
        std::array<std::uint8_t, 5> flags{};
    };

    [[nodiscard]] SelectResult SelectMainView(void* scene, Identity& identity) const noexcept;
    [[nodiscard]] static FrameResult SelectionFailure(SelectResult result) noexcept;
    [[nodiscard]] bool ReadPose(void* camera, Pose& pose,
                                AngularLimits* limits = nullptr) const noexcept;
    [[nodiscard]] bool SaveState(void* camera, SavedState& state) const noexcept;
    [[nodiscard]] bool ApplyPose(void* camera, const Pose& pose) const noexcept;
    [[nodiscard]] bool RestoreState(void* camera, const SavedState& state) const noexcept;

    Functions functions_{};
    History history_{};
    Identity identity_{};
    void* scene_ = nullptr;
    void* overriddenCamera_ = nullptr;
    SavedState authoritativeState_{};
    bool hasIdentity_ = false;
    bool overrideActive_ = false;
};

[[nodiscard]] const wchar_t* FrameResultName(FrameResult result) noexcept;

} // namespace amfp::camera
