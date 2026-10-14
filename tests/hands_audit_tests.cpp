#include "hands_audit.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

namespace {

amfp::hands_audit::Matrix g_world{};

const amfp::hands_audit::Matrix* GetWorld(void*) {
    return &g_world;
}

template <typename T, std::size_t Size>
void Write(std::array<std::uint8_t, Size>& bytes,
           const std::size_t offset,
           const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

bool Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    // Large enough to hold the current-hand-object field at +0x5C.
    std::array<std::uint8_t, 0x60> helper{};
    std::array<std::uint8_t, 0x28> entity{};
    std::array<std::uint8_t, 0x120> handObject{};
    std::array<std::uint8_t, 0x378> subMesh{};
    void* entityPointer = entity.data();
    Write(helper, 0x44, entityPointer);
    entity[0x24] = 1;
    g_world.values[0] = 1.0F;
    g_world.values[5] = 1.0F;
    g_world.values[10] = 1.0F;
    g_world.values[15] = 1.0F;

    const auto valid = amfp::hands_audit::Observe(helper.data(), &GetWorld);
    if (!Require(valid.result == amfp::hands_audit::Result::valid,
                 "valid hands root was rejected") ||
        !Require(valid.entity == entity.data(),
                 "hands entity was read from the wrong offset")) {
        return 1;
    }

    void* handObjectPointer = handObject.data();
    Write(helper, 0x5C, handObjectPointer);
    Write(handObject, 0xF4, entityPointer);
    const auto withObject =
        amfp::hands_audit::Observe(helper.data(), &GetWorld);
    if (!Require(withObject.handObjectResult ==
                     amfp::hands_audit::HandObjectResult::valid,
                 "valid current hand object was rejected") ||
        !Require(withObject.handObjectEntity == entity.data(),
                 "hand object mesh entity was read from the wrong offset")) {
        return 2;
    }

    Write(subMesh, 0x374, entityPointer);
    if (!Require(amfp::hands_audit::ObserveModel(
                     subMesh.data(), &g_world, entity.data(), nullptr) ==
                     amfp::hands_audit::ModelResult::handsRoot,
                 "hands-root submesh owner was not recognized")) {
        return 3;
    }

    entity[0x24] = 0;
    if (!Require(amfp::hands_audit::Observe(helper.data(), &GetWorld).result ==
                     amfp::hands_audit::Result::inactive,
                 "inactive hands root was not classified")) {
        return 4;
    }
    entity[0x24] = 1;
    g_world.values[0] = std::numeric_limits<float>::quiet_NaN();
    if (!Require(amfp::hands_audit::Observe(helper.data(), &GetWorld).result ==
                     amfp::hands_audit::Result::invalidMatrix,
                 "invalid hands matrix was accepted")) {
        return 5;
    }
    g_world.values[0] = 1.0F;
    amfp::hands_audit::Matrix moved = g_world;
    moved.values[3] = 0.25F;
    if (!Require(amfp::hands_audit::Changed(g_world, moved),
                 "hands movement was not detected") ||
        !Require(!amfp::hands_audit::Changed(g_world, g_world),
                 "stable hands matrix was reported as moving")) {
        return 6;
    }

    std::cout << "read-only hands layout audit tests passed\n";
    return 0;
}
