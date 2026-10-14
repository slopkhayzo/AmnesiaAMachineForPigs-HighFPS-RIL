#pragma once

#include <string>

namespace amfp::config {

struct Features {
    bool renderUnlock = false;
    bool presentationLimiter = false;
    bool cameraInterpolation = false;
    bool handsRootInterpolation = false;
    bool worldRootInterpolation = false;
    bool nodeAnimationInterpolation = false;
    bool handsPoseInterpolation = false;
    bool enemyPoseInterpolation = false;
    bool worldPoseInterpolation = false;
    bool ragdollPoseInterpolation = false;
    bool particleInterpolation = false;
    bool imageTrailTiming = false;
    bool mouseOverlay = false;
    bool guiCursorOverlay = false;
};

struct Settings {
    bool enabled = true;
    bool diagnostics = true;
    bool telemetryEnabled = false;
    bool motionTraceEnabled = false;
    bool ragdollProfileEnabled = false;
    unsigned int telemetryReportIntervalSeconds = 5;
    Features features{};
    unsigned int targetFps = 0;
};

struct LoadResult {
    bool ok = false;
    Settings settings{};
    std::wstring error;
};

[[nodiscard]] LoadResult Load(const std::wstring& path);
[[nodiscard]] bool AnyBehavioralFeatureEnabled(const Features& features);

} // namespace amfp::config
