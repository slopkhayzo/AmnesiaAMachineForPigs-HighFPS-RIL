#include "world_ownership_audit.h"

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

} // namespace

int main() {
    constexpr std::size_t staticOffset = 0x1DD;
    constexpr std::size_t beginOffset = 0x348;
    constexpr std::size_t endOffset = 0x34C;
    constexpr std::size_t meshOffset = 0x428;
    constexpr std::size_t typeOffset = 0x56C;
    constexpr std::size_t subMeshesBeginOffset = 0x314;
    constexpr std::size_t subMeshesEndOffset = 0x318;
    constexpr std::size_t parentOffset = 0x288;
    std::array<std::uint8_t, 0x580> prop{};
    std::array<std::uint8_t, 8> firstBody{};
    std::array<std::uint8_t, 8> secondBody{};
    std::array<std::uint8_t, 0x330> mesh{};
    std::array<std::uint8_t, 0x290> directSubMesh{};
    std::array<std::uint8_t, 0x290> nestedSubMesh{};
    std::array<std::uint8_t, 0x290> unmatchedSubMesh{};
    std::array<std::uint8_t, 0x290> intermediate{};
    std::array<std::uint8_t, 0x290> unrelated{};
    std::array<void*, 2> bodies{firstBody.data(), secondBody.data()};
    std::array<void*, 3> subMeshes{directSubMesh.data(), nestedSubMesh.data(),
                                   unmatchedSubMesh.data()};
    void** begin = bodies.data();
    void** end = bodies.data() + bodies.size();
    const std::uint32_t lever = 2;
    const std::uint8_t dynamic = 0;
    Write(prop, staticOffset, dynamic);
    Write(prop, beginOffset, begin);
    Write(prop, endOffset, end);
    void* meshPointer = mesh.data();
    Write(prop, meshOffset, meshPointer);
    Write(prop, typeOffset, lever);
    void** subMeshesBegin = subMeshes.data();
    void** subMeshesEnd = subMeshes.data() + subMeshes.size();
    Write(mesh, subMeshesBeginOffset, subMeshesBegin);
    Write(mesh, subMeshesEndOffset, subMeshesEnd);
    void* parent = firstBody.data();
    Write(directSubMesh, parentOffset, parent);
    parent = intermediate.data();
    Write(nestedSubMesh, parentOffset, parent);
    parent = secondBody.data();
    Write(intermediate, parentOffset, parent);
    parent = unrelated.data();
    Write(unmatchedSubMesh, parentOffset, parent);

    amfp::world_ownership::Registry registry{};
    const auto registered = registry.RegisterProp(prop.data());
    if (!Require(registered.result ==
                     amfp::world_ownership::RegisterResult::registered &&
                     registered.bodyCount == 2 &&
                     registered.registeredBodies == 2 &&
                     registered.renderLayoutValid &&
                     registered.subMeshCount == 3 &&
                     registered.directBodyChildren == 1 &&
                     registered.nestedBodyChildren == 1 &&
                     registered.unmatchedSubMeshes == 1,
                 "two-body lever/render-child registration failed")) return 1;
    std::uint32_t type = 99;
    if (!Require(registry.ClassifyBody(firstBody.data(), type) && type == lever,
                 "first lever body was not classified")) return 1;
    if (!Require(registry.ClassifyBody(secondBody.data(), type) && type == lever,
                 "second lever body was not classified")) return 1;
    std::array<std::uint8_t, 8> unknown{};
    if (!Require(!registry.ClassifyBody(unknown.data(), type),
                 "unknown body was classified")) return 1;

    end = begin;
    Write(prop, endOffset, end);
    if (!Require(registry.RegisterProp(prop.data()).result ==
                     amfp::world_ownership::RegisterResult::bodyless,
                 "bodyless prop was not recognized")) return 1;

    // A destroyed body is forgotten; survivors and re-registration still work.
    registry.NotifyBodyDestroyed(firstBody.data());
    if (!Require(!registry.ClassifyBody(firstBody.data(), type),
                 "destroyed body was still classified")) return 1;
    if (!Require(registry.ClassifyBody(secondBody.data(), type) && type == lever,
                 "surviving body was lost with a destroyed one")) return 1;
    end = bodies.data() + bodies.size();
    Write(prop, endOffset, end);
    if (!Require(registry.RegisterProp(prop.data()).registeredBodies == 2 &&
                     registry.ClassifyBody(firstBody.data(), type) &&
                     type == lever,
                 "re-registration after destruction failed")) return 1;
    registry.NotifyBodyDestroyed(firstBody.data());
    registry.NotifyBodyDestroyed(secondBody.data());
    if (!Require(!registry.ClassifyBody(firstBody.data(), type) &&
                     !registry.ClassifyBody(secondBody.data(), type),
                 "bodies survived the destruction of every body")) return 1;

    std::wcout << L"world ownership audit tests passed\n";
    return 0;
}
