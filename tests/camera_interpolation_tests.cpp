#include "camera_interpolation.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

amfp::camera::Pose Pose(float x, float yaw = 0.0F) {
    return {{x, x*2.0F, -x}, x*0.1F, yaw, -x*0.05F};
}

amfp::clock::Sample Pair(std::uint64_t generation, std::uint64_t older,
                         std::uint64_t newer, double alpha) {
    amfp::clock::Sample sample{};
    sample.kind = amfp::clock::SampleKind::interpolated;
    sample.generation = generation;
    sample.olderSerial = older;
    sample.newerSerial = newer;
    sample.alpha = alpha;
    return sample;
}

void TestInterpolation() {
    amfp::camera::Pose midpoint{};
    Require(amfp::camera::Interpolate(Pose(0.0F), Pose(2.0F), 0.5, midpoint),
            "valid interpolation rejected");
    Require(std::abs(midpoint.position.x-1.0F) < 1.0e-5F,
            "position midpoint incorrect");
    constexpr float pi = 3.14159265358979323846F;
    amfp::camera::Pose wrapped{};
    Require(amfp::camera::Interpolate(Pose(0.0F, 179.0F*pi/180.0F),
                                      Pose(0.0F, -179.0F*pi/180.0F), 0.5, wrapped),
            "wrapped yaw interpolation rejected");
    Require(amfp::camera::OrientationDistance(wrapped, Pose(0.0F, pi)) < 1.0e-3F,
            "yaw interpolation took the long arc");
}

void TestHistoryGuards() {
    amfp::camera::History history;
    Require(history.Publish(10, 4, Pose(0.0F)).kind ==
                amfp::camera::PublishKind::resetGeneration,
            "first generation not established");
    Require(history.Publish(11, 4, Pose(1.0F)).kind ==
                amfp::camera::PublishKind::appended,
            "adjacent pose not appended");
    const auto sampled = history.Evaluate(Pair(4, 10, 11, 0.25));
    Require(sampled.kind == amfp::camera::SampleKind::applied &&
                std::abs(sampled.pose.position.x-0.25F) < 1.0e-5F,
            "exact serial pair not sampled");
    Require(history.Publish(11, 4, Pose(1.1F)).kind ==
                amfp::camera::PublishKind::resetSameTickMutation,
            "same-tick mutation did not reset");
    Require(history.Publish(12, 4, Pose(20.0F)).kind ==
                amfp::camera::PublishKind::resetDiscontinuity,
            "teleport did not reset");
    Require(history.Evaluate(Pair(5, 11, 12, 0.5)).kind ==
                amfp::camera::SampleKind::generationMismatch,
            "history crossed a generation reset");
}

void TestInvalidFailsClosed() {
    auto invalid = Pose(0.0F);
    invalid.yaw = std::nanf("");
    amfp::camera::Pose output{};
    Require(!amfp::camera::Interpolate(invalid, Pose(1.0F), 0.5, output),
            "non-finite pose accepted");
    Require(!amfp::camera::Interpolate(Pose(0.0F), Pose(1.0F), 1.1, output),
            "out-of-range alpha accepted");
}

} // namespace

int main() {
    TestInterpolation();
    TestHistoryGuards();
    TestInvalidFailsClosed();
    std::cout << "camera interpolation tests passed\n";
    return EXIT_SUCCESS;
}
