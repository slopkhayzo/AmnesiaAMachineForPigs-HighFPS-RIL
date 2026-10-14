#include "enemy_pose_runtime.h"

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
    std::array<std::uint8_t, 0x8E8> enemy{};
    std::array<std::uint8_t, 0x394> mesh{};
    std::array<amfp::hands::Matrix, 4> bones{};

    explicit Fixture(const std::uint32_t type, const std::size_t count) {
        void* meshPointer = mesh.data();
        const std::uint8_t active = 1;
        auto* begin = bones.data();
        auto* end = bones.data() + count;
        auto* capacity = bones.data() + bones.size();
        Write(enemy, 0x578, meshPointer);
        Write(enemy, 0x8E4, type);
        Write(mesh, 0x24, active);
        Write(mesh, 0x388, begin);
        Write(mesh, 0x38C, end);
        Write(mesh, 0x390, capacity);
    }

    void Set(const float base) {
        for (std::size_t index = 0; index < bones.size(); ++index) {
            bones[index] = Translation(base + static_cast<float>(index));
        }
    }
};

} // namespace

int main() {
    amfp::enemy_pose::Runtime runtime;
    Fixture first(2, 2);
    Fixture second(2, 3);
    Fixture child(4, 2);
    Fixture unsupported(0, 2);
    bool okay = true;

    first.Set(0.0F);
    second.Set(10.0F);
    child.Set(20.0F);
    okay &= Expect(runtime.StageEnemy(first.enemy.data(), 1).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "first ManPig was not staged");
    okay &= Expect(runtime.StageEnemy(second.enemy.data(), 1).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "second ManPig was not staged");
    okay &= Expect(runtime.StageEnemy(child.enemy.data(), 1).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "Child was not staged");
    okay &= Expect(runtime.StageEnemy(unsupported.enemy.data(), 1).result ==
                       amfp::enemy_pose::StageResult::unsupportedType,
                   "unsupported Grunt was admitted");
    runtime.CommitCompletedTick(1, 1);
    runtime.BeginRender(Pair(1, 2, 0.5), 1);
    const auto firstInitial = runtime.OnMeshUpdated(first.mesh.data());
    okay &= Expect(firstInitial.result ==
                       amfp::enemy_pose::UpdateResult::identityChanged &&
                       firstInitial.captured,
                   "first palette identity was not established");
    okay &= Expect(runtime.OnMeshUpdated(second.mesh.data()).result ==
                       amfp::enemy_pose::UpdateResult::identityChanged,
                   "second palette identity was not established");
    okay &= Expect(runtime.OnMeshUpdated(child.mesh.data()).result ==
                       amfp::enemy_pose::UpdateResult::identityChanged,
                   "Child palette identity was not established");
    okay &= Expect(runtime.OnMeshUpdated(unsupported.mesh.data()).result ==
                       amfp::enemy_pose::UpdateResult::notTarget,
                   "unsupported palette was targeted");
    okay &= Expect(runtime.EndRender() == 0,
                   "initial render reported a restore failure");

    first.Set(2.0F);
    second.Set(14.0F);
    child.Set(24.0F);
    okay &= Expect(runtime.StageEnemy(first.enemy.data(), 2).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "first second-tick stage failed");
    okay &= Expect(runtime.StageEnemy(second.enemy.data(), 2).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "second second-tick stage failed");
    okay &= Expect(runtime.StageEnemy(child.enemy.data(), 2).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "Child second-tick stage failed");
    runtime.CommitCompletedTick(2, 1);
    runtime.BeginRender(Pair(1, 2, 0.5), 2);
    okay &= Expect(runtime.OnMeshUpdated(first.mesh.data()).result ==
                       amfp::enemy_pose::UpdateResult::applied,
                   "first palette was not interpolated");
    okay &= Expect(runtime.OnMeshUpdated(second.mesh.data()).result ==
                       amfp::enemy_pose::UpdateResult::applied,
                   "second palette was not interpolated");
    okay &= Expect(runtime.OnMeshUpdated(child.mesh.data()).result ==
                       amfp::enemy_pose::UpdateResult::applied,
                   "Child palette was not interpolated");
    okay &= Expect(std::abs(first.bones[0].values[3] - 1.0F) < 1.0e-5F,
                   "first palette midpoint is wrong");
    okay &= Expect(std::abs(second.bones[0].values[3] - 12.0F) < 1.0e-5F,
                   "second palette midpoint is wrong");
    okay &= Expect(std::abs(child.bones[0].values[3] - 22.0F) < 1.0e-5F,
                   "Child palette midpoint is wrong");
    okay &= Expect(runtime.EndRender() == 0,
                   "active render reported a restore failure");
    okay &= Expect(std::abs(first.bones[0].values[3] - 2.0F) < 1.0e-5F &&
                       std::abs(second.bones[0].values[3] - 14.0F) < 1.0e-5F &&
                       std::abs(child.bones[0].values[3] - 24.0F) < 1.0e-5F,
                   "authoritative palettes were not restored");

    first.bones[1].values[15] = 0.0F;
    okay &= Expect(runtime.StageEnemy(first.enemy.data(), 3).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "invalid third-tick stage failed");
    runtime.CommitCompletedTick(3, 1);
    runtime.BeginRender(Pair(2, 3, 0.5), 3);
    okay &= Expect(runtime.OnMeshUpdated(first.mesh.data()).result ==
                       amfp::enemy_pose::UpdateResult::invalid,
                   "invalid enemy palette did not fail closed");
    okay &= Expect(runtime.EndRender() == 0,
                   "invalid render reported a restore failure");

    // Enemies that stopped updating must not hold their slots forever.
    constexpr std::size_t kSlots = 32;
    static amfp::enemy_pose::Runtime bounded{};
    static std::array<std::array<std::uint8_t, 0x8E8>, kSlots + 1> enemies{};
    static std::array<std::array<std::uint8_t, 0x394>, kSlots + 1> meshes{};
    for (std::size_t index = 0; index < enemies.size(); ++index) {
        void* meshPointer = meshes[index].data();
        const std::uint32_t manPig = 2;
        const std::uint8_t active = 1;
        Write(enemies[index], 0x578, meshPointer);
        Write(enemies[index], 0x8E4, manPig);
        Write(meshes[index], 0x24, active);
    }
    for (std::size_t index = 0; index < kSlots; ++index) {
        okay &= Expect(bounded.StageEnemy(enemies[index].data(), 1).result ==
                           amfp::enemy_pose::StageResult::staged,
                       "crowd enemy was not admitted");
    }
    okay &= Expect(bounded.StageEnemy(enemies[kSlots].data(), 1).result ==
                       amfp::enemy_pose::StageResult::capacityExhausted,
                   "an enemy staged this tick was evicted");
    okay &= Expect(bounded.StageEnemy(enemies[kSlots].data(), 2).result ==
                       amfp::enemy_pose::StageResult::capacityExhausted,
                   "an enemy staged last tick was evicted");
    okay &= Expect(bounded.StageEnemy(enemies[1].data(), 3).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "live enemy lost its slot");
    okay &= Expect(bounded.StageEnemy(enemies[kSlots].data(), 3).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "stale enemy slot was not recycled");
    okay &= Expect(bounded.StageEnemy(enemies[1].data(), 4).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "recycling evicted a live enemy");
    okay &= Expect(bounded.StageEnemy(enemies[0].data(), 4).result ==
                       amfp::enemy_pose::StageResult::staged,
                   "an evicted enemy could not return through a stale slot");
    return okay ? 0 : 1;
}
