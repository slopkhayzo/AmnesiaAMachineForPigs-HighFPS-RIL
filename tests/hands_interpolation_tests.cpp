#include "hands_interpolation.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

constexpr float kPi = 3.14159265358979323846F;

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

amfp::hands::Matrix MakeTransform(const float x, const float yaw = 0.0F) {
    amfp::hands::Matrix matrix = amfp::hands::Identity();
    matrix.values[0] = std::cos(yaw);
    matrix.values[2] = std::sin(yaw);
    matrix.values[8] = -std::sin(yaw);
    matrix.values[10] = std::cos(yaw);
    matrix.values[3] = x;
    matrix.values[7] = x * 2.0F;
    matrix.values[11] = -x;
    return matrix;
}

amfp::clock::Sample Pair(const std::uint64_t generation,
                         const std::uint64_t older,
                         const std::uint64_t newer,
                         const double alpha) {
    amfp::clock::Sample sample{};
    sample.kind = amfp::clock::SampleKind::interpolated;
    sample.generation = generation;
    sample.olderSerial = older;
    sample.newerSerial = newer;
    sample.alpha = alpha;
    return sample;
}

void TestRigidInterpolationAndShortestArc() {
    const auto older = MakeTransform(0.0F, 179.0F * kPi / 180.0F);
    const auto newer = MakeTransform(2.0F, -179.0F * kPi / 180.0F);
    amfp::hands::Matrix midpoint{};
    Require(amfp::hands::Interpolate(older, newer, 0.5, midpoint),
            "valid rigid transforms were rejected");
    Require(std::abs(midpoint.values[3] - 1.0F) < 1.0e-5F &&
                std::abs(midpoint.values[7] - 2.0F) < 1.0e-5F &&
                std::abs(midpoint.values[11] + 1.0F) < 1.0e-5F,
            "translation was not interpolated linearly");
    const auto expected = MakeTransform(1.0F, kPi);
    Require(amfp::hands::OrientationDistance(midpoint, expected) < 1.0e-3F,
            "rotation did not take the shortest quaternion arc");
}

void TestInverseAndComposition() {
    const auto matrix = MakeTransform(3.0F, 0.7F);
    amfp::hands::Matrix inverse{};
    amfp::hands::Matrix product{};
    Require(amfp::hands::InverseRigid(matrix, inverse), "rigid inverse failed");
    Require(amfp::hands::Multiply(matrix, inverse, product), "matrix multiply failed");
    Require(amfp::hands::NearlyEqual(product, amfp::hands::Identity(), 2.0e-5F),
            "rigid inverse did not compose to identity");
}

void TestHistoryPairingAndResets() {
    amfp::hands::History history;
    Require(history.Publish(10, 3, MakeTransform(0.0F)) ==
                amfp::hands::PublishKind::resetGeneration,
            "first matrix did not establish generation");
    Require(history.Publish(11, 3, MakeTransform(2.0F)) ==
                amfp::hands::PublishKind::appended,
            "second matrix did not append");
    const auto sample = history.Evaluate(Pair(3, 10, 11, 0.25));
    Require(sample.kind == amfp::hands::SampleKind::applied &&
                std::abs(sample.matrix.values[3] - 0.5F) < 1.0e-5F,
            "history did not evaluate its exact serial pair");
    Require(history.Publish(11, 3, MakeTransform(2.1F)) ==
                amfp::hands::PublishKind::resetSameTickMutation,
            "same-tick mutation did not reset");
    Require(history.Publish(12, 3, MakeTransform(20.0F)) ==
                amfp::hands::PublishKind::resetDiscontinuity,
            "large root teleport did not reset");
}

void TestInvalidScaleFailsClosed() {
    auto scaled = MakeTransform(0.0F);
    scaled.values[0] = 2.0F;
    amfp::hands::Matrix output{};
    Require(!amfp::hands::IsRigid(scaled), "scaled matrix passed rigid validation");
    Require(!amfp::hands::Interpolate(scaled, MakeTransform(1.0F), 0.5, output),
            "scaled endpoint was interpolated");
}

void TestAffineInterpolationPreservesScale() {
    auto older = MakeTransform(0.0F, 0.0F);
    auto newer = MakeTransform(2.0F, kPi / 2.0F);
    for (int column = 0; column < 3; ++column) {
        older.values[column] *= 1.25F;
        older.values[4 + column] *= 2.0F;
        older.values[8 + column] *= 0.75F;
        newer.values[column] *= 1.25F;
        newer.values[4 + column] *= 2.0F;
        newer.values[8 + column] *= 0.75F;
    }
    amfp::hands::Matrix midpoint{};
    Require(amfp::hands::IsAffine(older) && amfp::hands::IsAffine(newer),
            "valid scaled affine transform was rejected");
    Require(amfp::hands::InterpolateAffine(older, newer, 0.5, midpoint),
            "scaled affine interpolation failed");
    Require(std::abs(midpoint.values[3] - 1.0F) < 1.0e-5F,
            "affine translation was not interpolated");
    float rowLengths[3]{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const float value = midpoint.values[row * 4 + column];
            rowLengths[row] += value * value;
        }
        rowLengths[row] = std::sqrt(rowLengths[row]);
    }
    Require(std::abs(rowLengths[0] - 1.25F) < 1.0e-4F &&
                std::abs(rowLengths[1] - 2.0F) < 1.0e-4F &&
                std::abs(rowLengths[2] - 0.75F) < 1.0e-4F,
            "affine interpolation did not preserve axis scales");

    auto sheared = older;
    sheared.values[1] += 0.5F;
    Require(!amfp::hands::IsAffine(sheared),
            "sheared matrix passed affine validation");
}

} // namespace

int main() {
    TestRigidInterpolationAndShortestArc();
    TestInverseAndComposition();
    TestHistoryPairingAndResets();
    TestInvalidScaleFailsClosed();
    TestAffineInterpolationPreservesScale();
    std::cout << "hands interpolation tests passed\n";
    return EXIT_SUCCESS;
}
