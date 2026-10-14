#include "world_audit.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

constexpr std::size_t kStaticPhysicsOffset = 0x1DD;
constexpr std::size_t kBodiesBeginOffset = 0x348;
constexpr std::size_t kBodiesEndOffset = 0x34C;
constexpr std::size_t kMeshEntityOffset = 0x428;
constexpr std::size_t kPropTypeOffset = 0x56C;
constexpr std::size_t kWorldOffset = 0x80;

template <typename T, std::size_t Size>
void Write(std::array<std::uint8_t, Size>& bytes,
           const std::size_t offset,
           const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

amfp::world_audit::Matrix Identity(const float x = 0.0F) {
    amfp::world_audit::Matrix matrix{};
    matrix.values = {1.0F, 0.0F, 0.0F, x,
                     0.0F, 1.0F, 0.0F, 0.0F,
                     0.0F, 0.0F, 1.0F, 0.0F,
                     0.0F, 0.0F, 0.0F, 1.0F};
    return matrix;
}

const amfp::world_audit::Matrix* GetWorld(void* entity) {
    return reinterpret_cast<const amfp::world_audit::Matrix*>(
        static_cast<const std::uint8_t*>(entity) + kWorldOffset);
}

bool Require(const bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

} // namespace

int main() {
    std::array<std::uint8_t, 0x600> prop{};
    std::array<std::uint8_t, 0x200> mesh{};
    std::array<void*, 2> bodies{reinterpret_cast<void*>(0x1000),
                                reinterpret_cast<void*>(0x2000)};
    void* begin = bodies.data();
    void* end = bodies.data() + 1;
    void* meshPointer = mesh.data();
    const std::uint32_t type = 1;
    const std::uint8_t dynamic = 0;
    Write(prop, kBodiesBeginOffset, begin);
    Write(prop, kBodiesEndOffset, end);
    Write(prop, kMeshEntityOffset, meshPointer);
    Write(prop, kPropTypeOffset, type);
    Write(prop, kStaticPhysicsOffset, dynamic);
    Write(mesh, kWorldOffset, Identity());

    amfp::world_audit::Tracker tracker{};
    const auto first = tracker.Observe(prop.data(), &GetWorld);
    if (!Require(first.result == amfp::world_audit::Result::valid &&
                     first.newIdentity && !first.changed &&
                     first.bodyCount == 1 && first.propType == 1 &&
                     !first.staticPhysics,
                 "first valid single-body observation was wrong")) return 1;

    const auto stable = tracker.Observe(prop.data(), &GetWorld);
    if (!Require(stable.result == amfp::world_audit::Result::valid &&
                     !stable.newIdentity && !stable.changed,
                 "stable observation was wrong")) return 1;

    Write(mesh, kWorldOffset, Identity(2.0F));
    const auto changed = tracker.Observe(prop.data(), &GetWorld);
    if (!Require(changed.changed && !changed.newIdentity,
                 "changed transform was not detected")) return 1;

    end = bodies.data() + 2;
    Write(prop, kBodiesEndOffset, end);
    const auto multi = tracker.Observe(prop.data(), &GetWorld);
    if (!Require(multi.bodyCount == 2,
                 "multi-body vector count was wrong")) return 1;

    const std::uint32_t invalidType = 15;
    Write(prop, kPropTypeOffset, invalidType);
    if (!Require(tracker.Observe(prop.data(), &GetWorld).result ==
                     amfp::world_audit::Result::invalidLayout,
                 "out-of-range prop type was accepted")) return 1;

    Write(prop, kPropTypeOffset, type);
    auto invalidMatrix = Identity();
    invalidMatrix.values[0] = std::nanf("");
    Write(mesh, kWorldOffset, invalidMatrix);
    if (!Require(tracker.Observe(prop.data(), &GetWorld).result ==
                     amfp::world_audit::Result::invalidMatrix,
                 "non-finite world matrix was accepted")) return 1;

    std::wcout << L"world audit tests passed\n";
    return 0;
}
