#include "pose_audit.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>

namespace {

constexpr std::size_t kBeginOffset = 0x388;
constexpr std::size_t kEndOffset = 0x38C;
constexpr std::size_t kCapacityOffset = 0x390;

amfp::pose_audit::Matrix Identity() {
    amfp::pose_audit::Matrix matrix{};
    matrix.values[0] = 1.0F;
    matrix.values[5] = 1.0F;
    matrix.values[10] = 1.0F;
    matrix.values[15] = 1.0F;
    return matrix;
}

template <typename T>
void Write(std::array<std::byte, 0x394>& object,
           const std::size_t offset,
           const T& value) {
    std::memcpy(object.data() + offset, &value, sizeof(value));
}

bool Expect(const bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

} // namespace

int main() {
    std::array<std::byte, 0x394> mesh{};
    std::array<amfp::pose_audit::Matrix, 2> palette{Identity(), Identity()};
    auto* begin = palette.data();
    auto* end = begin + palette.size();
    auto* capacity = end;
    Write(mesh, kBeginOffset, begin);
    Write(mesh, kEndOffset, end);
    Write(mesh, kCapacityOffset, capacity);

    amfp::pose_audit::Tracker tracker;
    bool okay = true;
    okay &= Expect(tracker.Observe(mesh.data(), nullptr).result ==
                       amfp::pose_audit::Result::notTarget,
                   "null target must be ignored");
    auto observation = tracker.Observe(mesh.data(), mesh.data());
    okay &= Expect(observation.result ==
                       amfp::pose_audit::Result::identityChanged &&
                       observation.boneCount == 2,
                   "first valid palette must establish identity");
    observation = tracker.Observe(mesh.data(), mesh.data());
    okay &= Expect(observation.result == amfp::pose_audit::Result::stable,
                   "unchanged palette must be stable");
    palette[1].values[3] = 0.25F;
    observation = tracker.Observe(mesh.data(), mesh.data());
    okay &= Expect(observation.result == amfp::pose_audit::Result::changed,
                   "changed palette must be detected");
    palette[0].values[0] = std::nanf("");
    observation = tracker.Observe(mesh.data(), mesh.data());
    okay &= Expect(observation.result == amfp::pose_audit::Result::invalidMatrix &&
                       observation.invalidBoneIndex == 0,
                   "non-finite matrix must be rejected");
    palette[0] = Identity();
    end = begin;
    Write(mesh, kEndOffset, end);
    observation = tracker.Observe(mesh.data(), mesh.data());
    okay &= Expect(observation.result == amfp::pose_audit::Result::invalidVector,
                   "empty palette must be rejected");
    return okay ? 0 : 1;
}
