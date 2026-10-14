#include "pose_runtime.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

bool Expect(const bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

template <typename T, std::size_t Size>
void Write(std::array<std::uint8_t, Size>& bytes,
           const std::size_t offset,
           const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

amfp::hands::Matrix Translation(const float x) {
    auto matrix = amfp::hands::Identity();
    matrix.values[3] = x;
    return matrix;
}

amfp::clock::Sample Pair(const std::uint64_t older,
                         const std::uint64_t newer,
                         const double alpha) {
    amfp::clock::Sample sample{};
    sample.kind = amfp::clock::SampleKind::interpolated;
    sample.generation = 1;
    sample.olderSerial = older;
    sample.newerSerial = newer;
    sample.alpha = alpha;
    return sample;
}

struct Fixture {
    std::array<std::uint8_t, 0x100> hands{};
    std::array<std::uint8_t, 0x400> mesh{};
    std::array<amfp::hands::Matrix, 4> bones{};

    Fixture() {
        void* meshPointer = mesh.data();
        const std::uint8_t active = 1;
        auto* begin = bones.data();
        auto* end = bones.data() + 2;
        auto* capacity = bones.data() + bones.size();
        Write(hands, 0x44, meshPointer);
        Write(mesh, 0x24, active);
        Write(mesh, 0x388, begin);
        Write(mesh, 0x38C, end);
        Write(mesh, 0x390, capacity);
    }

    void Set(const float base) {
        bones[0] = Translation(base);
        bones[1] = Translation(base + 1.0F);
    }
};

void __fastcall FakeUpdateGraphics(void*, void*, float) {}

bool Commit(amfp::pose::Runtime& runtime,
            Fixture& fixture,
            const std::uint64_t serial) {
    if (runtime.StageHands(fixture.hands.data(), serial) !=
        amfp::pose::StageResult::staged) {
        return false;
    }
    runtime.CommitCompletedTick(serial, 1);
    return true;
}

} // namespace

int main() {
    Fixture fixture;
    amfp::pose::Runtime runtime;
    runtime.Configure({reinterpret_cast<void*>(&FakeUpdateGraphics)});
    bool okay = true;

    fixture.Set(0.0F);
    okay &= Expect(Commit(runtime, fixture, 1), "first target was not staged");
    okay &= Expect(runtime.BeginRender(Pair(1, 2, 0.5), 1, 0.0F) ==
                       amfp::pose::UpdateResult::identityChanged,
                   "first palette did not establish identity");
    okay &= Expect(runtime.EndRender(), "first render failed restoration");

    fixture.Set(2.0F);
    okay &= Expect(Commit(runtime, fixture, 2), "second target was not staged");
    okay &= Expect(runtime.BeginRender(Pair(1, 2, 0.5), 2, 0.0F) ==
                       amfp::pose::UpdateResult::applied,
                   "second palette was not interpolated");
    okay &= Expect(std::abs(fixture.bones[0].values[3] - 1.0F) < 1.0e-5F &&
                       std::abs(fixture.bones[1].values[3] - 2.0F) < 1.0e-5F,
                   "render palette was not the midpoint");
    okay &= Expect(runtime.OnMeshUpdated(fixture.mesh.data()) ==
                       amfp::pose::UpdateResult::alreadyApplied,
                   "repeated submesh update recaptured the override");
    okay &= Expect(runtime.EndRender(), "authoritative palette was not restored");
    okay &= Expect(std::abs(fixture.bones[0].values[3] - 2.0F) < 1.0e-5F &&
                       std::abs(fixture.bones[1].values[3] - 3.0F) < 1.0e-5F,
                   "pose override leaked after render");

    fixture.bones[1].values[15] = 0.0F;
    okay &= Expect(Commit(runtime, fixture, 3), "invalid target was not staged");
    okay &= Expect(runtime.BeginRender(Pair(2, 3, 0.5), 3, 0.0F) ==
                       amfp::pose::UpdateResult::invalid,
                   "invalid palette did not fail closed");
    std::size_t invalidBone = 0;
    amfp::hands::Matrix invalidMatrix{};
    okay &= Expect(runtime.TakeInvalidMatrixDiagnostic(invalidBone, invalidMatrix) &&
                       invalidBone == 1,
                   "invalid palette did not expose its diagnostic");
    return okay ? 0 : 1;
}
