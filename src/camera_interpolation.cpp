#include "camera_interpolation.h"

#include <algorithm>
#include <cmath>

namespace amfp::camera {
namespace {

struct Quaternion { float w = 1.0F; float x = 0.0F; float y = 0.0F; float z = 0.0F; };

Quaternion Multiply(const Quaternion& a, const Quaternion& b) noexcept {
    return {a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z,
            a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w};
}

Quaternion AxisAngle(float angle, float x, float y, float z) noexcept {
    const float half = angle * 0.5F;
    const float sine = std::sin(half);
    return {std::cos(half), x*sine, y*sine, z*sine};
}

Quaternion FromEuler(const Pose& pose) noexcept {
    return Multiply(Multiply(AxisAngle(pose.yaw, 0.0F, 1.0F, 0.0F),
                             AxisAngle(pose.pitch, 1.0F, 0.0F, 0.0F)),
                    AxisAngle(pose.roll, 0.0F, 0.0F, 1.0F));
}

float Dot(const Quaternion& a, const Quaternion& b) noexcept {
    return a.w*b.w+a.x*b.x+a.y*b.y+a.z*b.z;
}

Quaternion Normalize(const Quaternion& value) noexcept {
    const float lengthSquared = Dot(value, value);
    if (!(lengthSquared > 0.0F) || !std::isfinite(lengthSquared)) return {};
    const float inverse = 1.0F / std::sqrt(lengthSquared);
    return {value.w*inverse, value.x*inverse, value.y*inverse, value.z*inverse};
}

Quaternion Slerp(const Quaternion& from, const Quaternion& to, float alpha) noexcept {
    Quaternion end = to;
    float cosine = Dot(from, end);
    if (cosine < 0.0F) {
        end = {-end.w, -end.x, -end.y, -end.z};
        cosine = -cosine;
    }
    cosine = std::clamp(cosine, -1.0F, 1.0F);
    if (cosine > 0.9995F) {
        return Normalize({from.w+alpha*(end.w-from.w),
                          from.x+alpha*(end.x-from.x),
                          from.y+alpha*(end.y-from.y),
                          from.z+alpha*(end.z-from.z)});
    }
    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    const float a = std::sin((1.0F-alpha)*angle)/sine;
    const float b = std::sin(alpha*angle)/sine;
    return Normalize({from.w*a+end.w*b, from.x*a+end.x*b,
                      from.y*a+end.y*b, from.z*a+end.z*b});
}

void ToEuler(const Quaternion& input, Pose& pose) noexcept {
    const Quaternion q = Normalize(input);
    const float m02 = 2.0F*(q.x*q.z+q.w*q.y);
    const float m10 = 2.0F*(q.x*q.y+q.w*q.z);
    const float m11 = 1.0F-2.0F*(q.x*q.x+q.z*q.z);
    const float m12 = 2.0F*(q.y*q.z-q.w*q.x);
    const float m22 = 1.0F-2.0F*(q.x*q.x+q.y*q.y);
    pose.pitch = std::asin(std::clamp(-m12, -1.0F, 1.0F));
    if (std::abs(std::cos(pose.pitch)) > 1.0e-5F) {
        pose.yaw = std::atan2(m02, m22);
        pose.roll = std::atan2(m10, m11);
    } else {
        pose.yaw = std::atan2(-2.0F*(q.x*q.z-q.w*q.y),
                              1.0F-2.0F*(q.y*q.y+q.z*q.z));
        pose.roll = 0.0F;
    }
}

bool IsDiscontinuous(const Pose& older, const Pose& newer,
                     std::uint64_t serialDelta) noexcept {
    const float dx = newer.position.x-older.position.x;
    const float dy = newer.position.y-older.position.y;
    const float dz = newer.position.z-older.position.z;
    const float positionLimit = History::kMaximumPositionDeltaPerTick *
                                static_cast<float>(serialDelta);
    if (dx*dx+dy*dy+dz*dz > positionLimit*positionLimit) return true;
    const float angleLimit = History::kMaximumOrientationDeltaPerTick *
                             static_cast<float>(serialDelta);
    return OrientationDistance(older, newer) > angleLimit;
}

} // namespace

bool IsFinite(const Pose& p) noexcept {
    return std::isfinite(p.position.x) && std::isfinite(p.position.y) &&
           std::isfinite(p.position.z) && std::isfinite(p.pitch) &&
           std::isfinite(p.yaw) && std::isfinite(p.roll);
}

bool NearlyEqual(const Pose& a, const Pose& b, float epsilon) noexcept {
    return std::abs(a.position.x-b.position.x)<=epsilon &&
           std::abs(a.position.y-b.position.y)<=epsilon &&
           std::abs(a.position.z-b.position.z)<=epsilon &&
           std::abs(a.pitch-b.pitch)<=epsilon && std::abs(a.yaw-b.yaw)<=epsilon &&
           std::abs(a.roll-b.roll)<=epsilon;
}

float OrientationDistance(const Pose& a, const Pose& b) noexcept {
    const Quaternion first = Normalize(FromEuler(a));
    const Quaternion second = Normalize(FromEuler(b));
    return 2.0F*std::acos(std::clamp(std::abs(Dot(first, second)), 0.0F, 1.0F));
}

bool Interpolate(const Pose& older, const Pose& newer, double alpha,
                 Pose& result) noexcept {
    if (!IsFinite(older) || !IsFinite(newer) || !std::isfinite(alpha) ||
        alpha < 0.0 || alpha > 1.0) return false;
    const float t = static_cast<float>(alpha);
    result.position = {older.position.x+(newer.position.x-older.position.x)*t,
                       older.position.y+(newer.position.y-older.position.y)*t,
                       older.position.z+(newer.position.z-older.position.z)*t};
    ToEuler(Slerp(Normalize(FromEuler(older)), Normalize(FromEuler(newer)), t), result);
    return IsFinite(result);
}

void History::Reset() noexcept {
    first_ = count_ = 0;
    generation_ = 0;
    hasGeneration_ = false;
}

const History::Snapshot& History::At(std::size_t index) const noexcept {
    return snapshots_[(first_+index)%kCapacity];
}

void History::Append(std::uint64_t serial, const Pose& pose) noexcept {
    if (count_ < kCapacity) snapshots_[(first_+count_++)%kCapacity] = {serial, pose};
    else { snapshots_[first_] = {serial, pose}; first_ = (first_+1)%kCapacity; }
}

PublishResult History::Publish(std::uint64_t serial, std::uint64_t generation,
                               const Pose& pose) noexcept {
    if (!IsFinite(pose)) { Reset(); return {PublishKind::invalid}; }
    if (!hasGeneration_ || generation_ != generation) {
        Reset(); generation_ = generation; hasGeneration_ = true; Append(serial, pose);
        return {PublishKind::resetGeneration};
    }
    if (count_ != 0) {
        const Snapshot& latest = At(count_-1);
        if (serial < latest.serial) {
            Reset(); generation_ = generation; hasGeneration_ = true; Append(serial, pose);
            return {PublishKind::resetNonMonotonic};
        }
        if (serial == latest.serial) {
            if (NearlyEqual(latest.pose, pose)) return {PublishKind::unchanged};
            Reset(); generation_ = generation; hasGeneration_ = true; Append(serial, pose);
            return {PublishKind::resetSameTickMutation};
        }
        const std::uint64_t delta = serial-latest.serial;
        if (delta > clock::PresentationClock::kMaximumBridgedSerialDelta ||
            IsDiscontinuous(latest.pose, pose, delta)) {
            Reset(); generation_ = generation; hasGeneration_ = true; Append(serial, pose);
            return {PublishKind::resetDiscontinuity};
        }
    }
    Append(serial, pose);
    return {PublishKind::appended};
}

SampleResult History::Evaluate(const clock::Sample& sample) const noexcept {
    if (sample.kind != clock::SampleKind::interpolated ||
        sample.olderSerial == sample.newerSerial)
        return {SampleKind::clockUnavailable, {}};
    if (!hasGeneration_ || generation_ != sample.generation)
        return {SampleKind::generationMismatch, {}};
    const Snapshot* older = nullptr;
    const Snapshot* newer = nullptr;
    for (std::size_t i=0; i<count_; ++i) {
        const Snapshot& candidate = At(i);
        if (candidate.serial == sample.olderSerial) older = &candidate;
        if (candidate.serial == sample.newerSerial) newer = &candidate;
    }
    if (older == nullptr || newer == nullptr) return {SampleKind::snapshotsMissing, {}};
    Pose pose{};
    if (!Interpolate(older->pose, newer->pose, sample.alpha, pose))
        return {SampleKind::invalid, {}};
    return {SampleKind::applied, pose};
}

const wchar_t* PublishKindName(PublishKind kind) noexcept {
    switch (kind) {
    case PublishKind::appended: return L"appended";
    case PublishKind::unchanged: return L"unchanged";
    case PublishKind::invalid: return L"invalid";
    case PublishKind::resetGeneration: return L"generation";
    case PublishKind::resetNonMonotonic: return L"non-monotonic";
    case PublishKind::resetSameTickMutation: return L"same-tick-mutation";
    case PublishKind::resetDiscontinuity: return L"discontinuity";
    }
    return L"unknown";
}

const wchar_t* SampleKindName(SampleKind kind) noexcept {
    switch (kind) {
    case SampleKind::applied: return L"applied";
    case SampleKind::clockUnavailable: return L"clock-unavailable";
    case SampleKind::generationMismatch: return L"generation-mismatch";
    case SampleKind::snapshotsMissing: return L"snapshots-missing";
    case SampleKind::invalid: return L"invalid";
    }
    return L"unknown";
}

} // namespace amfp::camera
