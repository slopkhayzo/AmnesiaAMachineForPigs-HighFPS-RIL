#include "ragdoll_pose_audit.h"

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
    amfp::ragdoll_pose_audit::Audit audit{};
    std::array<std::uint8_t, 0x3A8> mesh{};
    std::array<amfp::pose_audit::Matrix, 2> bones{Identity(), Identity()};
    auto* begin = bones.data();
    auto* end = bones.data() + bones.size();
    auto* capacity = end;
    Write(mesh, 0x388, begin);
    Write(mesh, 0x38C, end);
    Write(mesh, 0x390, capacity);

    if (!Require(audit.Observe(mesh.data()).result ==
                     amfp::ragdoll_pose_audit::Result::notTarget,
                 "ordinary mesh was targeted")) return 1;

    const std::uint8_t enabled = 1;
    Write(mesh, 0x398, enabled);
    auto observation = audit.Observe(mesh.data());
    if (!Require(observation.result ==
                     amfp::ragdoll_pose_audit::Result::identityChanged &&
                     observation.boneCount == 2,
                 "ragdoll palette identity was not observed")) return 1;
    observation = audit.Observe(mesh.data());
    if (!Require(observation.result ==
                     amfp::ragdoll_pose_audit::Result::stable,
                 "stable ragdoll palette was not observed")) return 1;
    bones[1].values[3] = 2.0F;
    observation = audit.Observe(mesh.data());
    if (!Require(observation.result ==
                     amfp::ragdoll_pose_audit::Result::changed,
                 "changed ragdoll palette was not observed")) return 1;

    const std::uint8_t invalid = 2;
    Write(mesh, 0x398, invalid);
    if (!Require(audit.Observe(mesh.data()).result ==
                     amfp::ragdoll_pose_audit::Result::invalidFlag,
                 "invalid skeleton-physics flag was accepted")) return 1;

    std::wcout << L"ragdoll pose audit tests passed\n";
    return 0;
}
