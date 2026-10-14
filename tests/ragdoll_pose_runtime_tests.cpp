#include "ragdoll_pose_runtime.h"

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
    std::array<std::uint8_t, 0x3A8> mesh{};
    std::array<amfp::hands::Matrix, 3> bones{};

    Fixture() {
        const std::uint8_t active = 1;
        auto* begin = bones.data();
        auto* end = bones.data() + bones.size();
        Write(mesh, 0x388, begin);
        Write(mesh, 0x38C, end);
        Write(mesh, 0x390, end);
        Write(mesh, 0x398, active);
    }

    void Set(const float base) {
        for (std::size_t index = 0; index < bones.size(); ++index) {
            bones[index] = Translation(base + static_cast<float>(index));
        }
    }
};

} // namespace

int main() {
    amfp::ragdoll_pose::Runtime runtime{};
    Fixture ragdoll{};
    std::array<std::uint8_t, 0x3A8> ordinary{};
    bool okay = true;

    ragdoll.Set(0.0F);
    runtime.BeginRender(Pair(1, 2, 0.5), 1);
    const auto initial = runtime.OnMeshUpdated(ragdoll.mesh.data());
    okay &= Expect(initial.result ==
                       amfp::ragdoll_pose::UpdateResult::identityChanged &&
                       initial.captured,
                   "initial ragdoll palette was not captured");
    okay &= Expect(runtime.OnMeshUpdated(ordinary.data()).result ==
                       amfp::ragdoll_pose::UpdateResult::notTarget,
                   "ordinary mesh was targeted");
    okay &= Expect(runtime.EndRender() == 0,
                   "initial render reported a restore failure");

    ragdoll.Set(2.0F);
    runtime.BeginRender(Pair(1, 2, 0.5), 2);
    const auto applied = runtime.OnMeshUpdated(ragdoll.mesh.data());
    okay &= Expect(applied.result ==
                       amfp::ragdoll_pose::UpdateResult::applied &&
                       applied.captured,
                   "ragdoll palette was not interpolated");
    okay &= Expect(std::abs(ragdoll.bones[0].values[3] - 1.0F) < 1.0e-5F,
                   "ragdoll palette midpoint is wrong");
    okay &= Expect(runtime.WasObserved(ragdoll.mesh.data()),
                   "current-render ragdoll observation was not recorded");
    okay &= Expect(runtime.OnMeshUpdated(ragdoll.mesh.data()).result ==
                       amfp::ragdoll_pose::UpdateResult::alreadyApplied,
                   "duplicate render update was not rejected");
    okay &= Expect(runtime.ShouldReskin(ragdoll.mesh.data()),
                   "moving ragdoll was not marked for reskinning");
    okay &= Expect(runtime.EndRender() == 0,
                   "active render reported a restore failure");
    okay &= Expect(std::abs(ragdoll.bones[0].values[3] - 2.0F) < 1.0e-5F,
                   "authoritative ragdoll palette was not restored");

    runtime.BeginRender(Pair(2, 3, 0.5), 3);
    const auto stationary = runtime.OnMeshUpdated(ragdoll.mesh.data());
    okay &= Expect(stationary.result ==
                       amfp::ragdoll_pose::UpdateResult::stationary &&
                       stationary.captured,
                   "unchanged ragdoll palette was not classified stationary");
    okay &= Expect(runtime.OnMeshUpdated(ragdoll.mesh.data()).result ==
                       amfp::ragdoll_pose::UpdateResult::alreadyApplied,
                   "duplicate stationary observation missed the fast path");
    okay &= Expect(!runtime.ShouldReskin(ragdoll.mesh.data()),
                   "stationary ragdoll remained marked for reskinning");
    okay &= Expect(runtime.EndRender() == 0,
                   "stationary render reported a restore failure");

    ragdoll.bones[1].values[15] = 0.0F;
    runtime.BeginRender(Pair(3, 4, 0.5), 4);
    okay &= Expect(runtime.OnMeshUpdated(ragdoll.mesh.data()).result ==
                       amfp::ragdoll_pose::UpdateResult::invalid,
                   "invalid ragdoll palette did not fail closed");
    okay &= Expect(runtime.EndRender() == 0,
                   "invalid render reported a restore failure");

    // Meshes that stopped updating must not hold their slots forever.
    constexpr std::size_t kSlots = 32;
    static std::array<Fixture, kSlots + 1> crowd{};
    static amfp::ragdoll_pose::Runtime bounded{};
    for (Fixture& fixture : crowd) fixture.Set(0.0F);
    bounded.BeginRender(Pair(1, 2, 0.5), 1);
    for (std::size_t index = 0; index < kSlots; ++index) {
        okay &= Expect(bounded.OnMeshUpdated(crowd[index].mesh.data()).result ==
                           amfp::ragdoll_pose::UpdateResult::identityChanged,
                       "crowd ragdoll was not admitted");
    }
    okay &= Expect(bounded.OnMeshUpdated(crowd[kSlots].mesh.data()).result ==
                       amfp::ragdoll_pose::UpdateResult::capacityExhausted,
                   "a recently updated ragdoll was evicted");
    okay &= Expect(bounded.EndRender() == 0,
                   "crowd render reported a restore failure");
    for (std::uint64_t render = 0; render < 64; ++render) {
        bounded.BeginRender(Pair(1, 2, 0.5), 1);
        okay &= Expect(bounded.OnMeshUpdated(crowd[1].mesh.data()).result !=
                           amfp::ragdoll_pose::UpdateResult::capacityExhausted,
                       "live ragdoll lost its slot");
        (void)bounded.EndRender();
    }
    bounded.BeginRender(Pair(1, 2, 0.5), 1);
    okay &= Expect(bounded.OnMeshUpdated(crowd[kSlots].mesh.data()).result ==
                       amfp::ragdoll_pose::UpdateResult::identityChanged,
                   "stale ragdoll slot was not recycled");
    okay &= Expect(bounded.OnMeshUpdated(crowd[1].mesh.data()).result ==
                       amfp::ragdoll_pose::UpdateResult::stationary,
                   "recycling evicted a live ragdoll");
    (void)bounded.EndRender();
    return okay ? 0 : 1;
}
