#include "enemy_pose_audit.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

template <typename T, std::size_t Size>
void Write(std::array<std::uint8_t, Size>& bytes,
           const std::size_t offset,
           const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

bool Require(const bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

amfp::pose_audit::Matrix Identity() {
    amfp::pose_audit::Matrix matrix{};
    matrix.values[0] = 1.0F;
    matrix.values[5] = 1.0F;
    matrix.values[10] = 1.0F;
    matrix.values[15] = 1.0F;
    return matrix;
}

} // namespace

int main() {
    amfp::enemy_pose_audit::Registry registry{};
    std::array<std::uint8_t, 0x8E8> enemy{};
    std::array<std::uint8_t, 0x394> mesh{};
    std::array<amfp::pose_audit::Matrix, 2> bones{Identity(), Identity()};
    bones[1].values[3] = 1.0F;
    void* meshPointer = mesh.data();
    const std::uint32_t manPig = 2;
    const std::uint8_t active = 1;
    auto* begin = bones.data();
    auto* end = bones.data() + bones.size();
    auto* capacity = bones.data() + bones.size();
    Write(enemy, 0x578, meshPointer);
    Write(enemy, 0x8E4, manPig);
    Write(mesh, 0x24, active);
    Write(mesh, 0x388, begin);
    Write(mesh, 0x38C, end);
    Write(mesh, 0x390, capacity);

    auto staged = registry.StageEnemy(enemy.data());
    if (!Require(staged.result ==
                     amfp::enemy_pose_audit::StageResult::registered &&
                     staged.type == manPig,
                 "enemy registration failed")) return 1;
    staged = registry.StageEnemy(enemy.data());
    if (!Require(staged.result ==
                     amfp::enemy_pose_audit::StageResult::staged,
                 "enemy restaging failed")) return 1;

    auto observed = registry.ObserveMesh(mesh.data());
    if (!Require(observed.result ==
                     amfp::enemy_pose_audit::ObserveResult::identityChanged &&
                     observed.type == manPig && observed.boneCount == 2,
                 "initial palette identity was not observed")) return 1;
    observed = registry.ObserveMesh(mesh.data());
    if (!Require(observed.result ==
                     amfp::enemy_pose_audit::ObserveResult::stable,
                 "stable palette was not observed")) return 1;
    bones[1].values[3] = 2.0F;
    observed = registry.ObserveMesh(mesh.data());
    if (!Require(observed.result ==
                     amfp::enemy_pose_audit::ObserveResult::changed,
                 "changed palette was not observed")) return 1;

    std::array<std::uint8_t, 0x8E8> invalidEnemy{};
    Write(invalidEnemy, 0x578, meshPointer);
    const std::uint32_t invalidType = 5;
    Write(invalidEnemy, 0x8E4, invalidType);
    if (!Require(registry.StageEnemy(invalidEnemy.data()).result ==
                     amfp::enemy_pose_audit::StageResult::invalidType,
                 "invalid enemy type was accepted")) return 1;

    std::array<std::uint8_t, 0x394> unrelatedMesh{};
    if (!Require(registry.ObserveMesh(unrelatedMesh.data()).result ==
                     amfp::enemy_pose_audit::ObserveResult::notTarget,
                 "unrelated mesh was classified as an enemy")) return 1;

    std::wcout << L"enemy pose audit tests passed\n";
    return 0;
}
