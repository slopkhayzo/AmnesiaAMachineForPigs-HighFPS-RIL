#pragma once

#include "target_mapping.h"

#include <string>

namespace amfp::telemetry {

[[nodiscard]] bool Initialize(const target::CoreMapping& mapping,
                              bool telemetryEnabled,
                              bool ragdollProfileEnabled,
                              bool renderUnlockEnabled,
                              bool cameraInterpolationEnabled,
                              bool handsInterpolationEnabled,
                              bool handsPoseInterpolationEnabled,
                              bool enemyPoseInterpolationEnabled,
                              bool ragdollPoseInterpolationEnabled,
                              bool nodeAnimationInterpolationEnabled,
                              bool worldInterpolationEnabled,
                              bool imageTrailTimingEnabled,
                              // Null leaves gameplay mouse look stock.
                              const target::MouseMapping* mouseMapping,
                              // Null leaves particles at the fixed-update rate.
                              const target::ParticleMapping* particleMapping,
                              unsigned int reportIntervalSeconds,
                              std::wstring& error);

} // namespace amfp::telemetry
