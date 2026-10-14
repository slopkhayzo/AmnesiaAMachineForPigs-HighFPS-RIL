#include "node_animation_runtime.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846F;

bool Require(const bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

amfp::hands::Matrix Transform(const float angle) {
    amfp::hands::Matrix matrix = amfp::hands::Identity();
    matrix.values[0] = std::cos(angle) * 1.25F;
    matrix.values[2] = std::sin(angle) * 1.25F;
    matrix.values[5] = 1.25F;
    matrix.values[8] = -std::sin(angle) * 1.25F;
    matrix.values[10] = std::cos(angle) * 1.25F;
    return matrix;
}

amfp::clock::Sample Pair(const std::uint64_t older,
                         const std::uint64_t newer,
                         const double alpha) {
    amfp::clock::Sample sample{};
    sample.kind = amfp::clock::SampleKind::interpolated;
    sample.generation = 3;
    sample.olderSerial = older;
    sample.newerSerial = newer;
    sample.alpha = alpha;
    return sample;
}

} // namespace

int main() {
    using amfp::node_animation::OverrideResult;
    amfp::node_animation::Runtime runtime;
    std::array<std::uint8_t, 0x400> animatedSubMesh{};
    std::array<std::uint8_t, 0x400> staticSubMesh{};
    void* localNode = reinterpret_cast<void*>(0x1234U);
    std::memcpy(animatedSubMesh.data() + 0x37C, &localNode,
                sizeof(localNode));

    const amfp::hands::Matrix* result = nullptr;
    auto first = Transform(0.0F);
    runtime.BeginRender(Pair(9, 10, 0.5), 10);
    auto observation = runtime.OverrideModelMatrix(
        animatedSubMesh.data(), &first, result);
    if (!Require(observation.captured &&
                     observation.result == OverrideResult::pairMissing,
                 "first animated-node sample was not captured")) return 1;

    auto second = Transform(kPi / 3.0F);
    runtime.BeginRender(Pair(10, 11, 0.5), 11);
    observation = runtime.OverrideModelMatrix(
        animatedSubMesh.data(), &second, result);
    if (!Require(observation.captured && observation.changed &&
                     observation.result == OverrideResult::applied &&
                     result != &second,
                 "moving animated node was not interpolated")) return 1;
    const auto expected = Transform(kPi / 6.0F);
    if (!Require(amfp::hands::AffineOrientationDistance(*result, expected) <
                     1.0e-3F,
                 "animated node rotation used the wrong arc")) return 1;
    if (!Require(std::abs(result->values[5] - 1.25F) < 1.0e-4F,
                 "animated node scale was not preserved")) return 1;

    const amfp::hands::Matrix* repeatedResult = nullptr;
    observation = runtime.OverrideModelMatrix(
        animatedSubMesh.data(), &second, repeatedResult);
    if (!Require(!observation.captured && observation.changed &&
                     observation.result == OverrideResult::applied &&
                     repeatedResult != &second,
                 "same-tick animated-node fast path lost interpolation")) {
        return 1;
    }
    if (!Require(amfp::hands::AffineOrientationDistance(
                     *repeatedResult, expected) < 1.0e-3F,
                 "same-tick animated-node result changed")) return 1;

    runtime.BeginRender(Pair(11, 12, 0.25), 12);
    observation = runtime.OverrideModelMatrix(
        animatedSubMesh.data(), &second, result);
    if (!Require(observation.result == OverrideResult::stable,
                 "unchanged node pair should remain authoritative")) return 1;

    observation = runtime.OverrideModelMatrix(
        staticSubMesh.data(), &second, result);
    if (!Require(observation.result == OverrideResult::notTarget &&
                     result == &second,
                 "submesh without an animation node was targeted")) return 1;

    auto invalid = second;
    invalid.values[1] += 0.5F;
    runtime.BeginRender(Pair(12, 13, 0.5), 13);
    observation = runtime.OverrideModelMatrix(
        animatedSubMesh.data(), &invalid, result);
    if (!Require(observation.result == OverrideResult::invalid &&
                     result == &invalid,
                 "sheared animated-node matrix did not fail closed")) return 1;

    // Entries for submeshes that no longer exist must not accumulate for the
    // whole session: once a quarter of the table is in use it is recycled.
    constexpr std::size_t kCrowd = 1024;
    static amfp::node_animation::Runtime bounded;
    std::vector<std::array<std::uint8_t, 0x380>> crowd(kCrowd);
    for (auto& subMesh : crowd) {
        std::memcpy(subMesh.data() + 0x37C, &localNode, sizeof(localNode));
    }
    bounded.BeginRender(Pair(9, 10, 0.5), 10);
    observation = bounded.OverrideModelMatrix(
        animatedSubMesh.data(), &first, result);
    if (!Require(observation.captured, "bounded first sample was not captured")) {
        return 1;
    }
    bounded.BeginRender(Pair(10, 11, 0.5), 11);
    observation = bounded.OverrideModelMatrix(
        animatedSubMesh.data(), &second, result);
    if (!Require(observation.result == OverrideResult::applied,
                 "bounded node was not interpolated")) return 1;
    std::size_t admitted = 0;
    for (auto& subMesh : crowd) {
        if (bounded.OverrideModelMatrix(subMesh.data(), &second, result)
                .result != OverrideResult::capacityExhausted) {
            ++admitted;
        }
    }
    if (!Require(admitted == kCrowd, "crowd nodes were not admitted")) return 1;
    for (int render = 0; render < 4096; ++render) {
        bounded.BeginRender(Pair(10, 11, 0.5), 11);
    }
    observation = bounded.OverrideModelMatrix(
        animatedSubMesh.data(), &second, result);
    if (!Require(observation.captured &&
                     observation.result == OverrideResult::pairMissing &&
                     result == &second,
                 "accumulated node entries were not recycled")) return 1;

    return 0;
}
