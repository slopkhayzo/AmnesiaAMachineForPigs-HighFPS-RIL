#include "world_runtime.h"

#include <array>
#include <cmath>
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
    constexpr std::size_t bodyMatrixOffset = 0x2C;
    constexpr std::size_t parentOffset = 0x288;
    std::array<std::uint8_t, 0x580> prop{};
    std::array<std::uint8_t, 0x100> body{};
    std::array<std::uint8_t, 0x290> subMesh{};
    std::array<void*, 1> bodies{body.data()};
    void** begin = bodies.data();
    void** end = bodies.data() + bodies.size();
    const std::uint32_t lever = 2;
    Write(prop, 0x348, begin);
    Write(prop, 0x34C, end);
    Write(prop, 0x56C, lever);
    void* parent = body.data();
    Write(subMesh, parentOffset, parent);

    amfp::world::Runtime runtime{};
    if (!Require(runtime.RegisterProp(prop.data()).kind ==
                     amfp::world::RegistrationKind::registered,
                 "lever registration failed")) return 1;

    amfp::world::Matrix first = amfp::hands::Identity();
    Write(body, bodyMatrixOffset, first);
    runtime.NotifyBodyMoved(body.data(), true);
    const auto firstCommit = runtime.CommitCompletedTick(1, 7);
    if (!Require(firstCommit.captured == 1 && firstCommit.invalid == 0,
                 "first lever endpoint was not captured")) return 1;

    amfp::world::Matrix second = first;
    second.values[3] = 1.0F;
    Write(body, bodyMatrixOffset, second);
    runtime.NotifyBodyMoved(body.data(), true);
    runtime.NotifyBodyMoved(body.data(), true);
    const auto secondCommit = runtime.CommitCompletedTick(2, 7);
    if (!Require(secondCommit.captured == 1 && secondCommit.duplicate == 1,
                 "second lever endpoint/deduplication failed")) return 1;

    amfp::clock::Sample sample{};
    sample.kind = amfp::clock::SampleKind::interpolated;
    sample.generation = 7;
    sample.olderSerial = 1;
    sample.newerSerial = 2;
    sample.alpha = 0.5;
    runtime.BeginRender(sample);
    const amfp::world::Matrix* result = nullptr;
    const auto overrideResult = runtime.OverrideModelMatrix(
        subMesh.data(), &second, result);
    if (!Require(overrideResult == amfp::world::OverrideResult::applied &&
                     result != nullptr &&
                     std::abs(result->values[3] - 0.5F) < 1.0e-4F,
                 "lever model matrix was not interpolated")) return 1;
    runtime.EndRender();

    std::array<std::uint8_t, 0x580> wheelProp{};
    std::array<std::uint8_t, 0x100> wheelBody{};
    std::array<std::uint8_t, 0x290> wheelSubMesh{};
    std::array<void*, 1> wheelBodies{wheelBody.data()};
    void** wheelBegin = wheelBodies.data();
    void** wheelEnd = wheelBodies.data() + wheelBodies.size();
    const std::uint32_t wheel = 3;
    Write(wheelProp, 0x348, wheelBegin);
    Write(wheelProp, 0x34C, wheelEnd);
    Write(wheelProp, 0x56C, wheel);
    void* wheelParent = wheelBody.data();
    Write(wheelSubMesh, parentOffset, wheelParent);
    Write(wheelBody, bodyMatrixOffset, first);
    if (!Require(runtime.RegisterProp(wheelProp.data()).kind ==
                     amfp::world::RegistrationKind::registered,
                 "wheel registration failed")) return 1;
    runtime.NotifyBodyMoved(wheelBody.data(), true, wheel);
    if (!Require(runtime.CommitCompletedTick(1, 7).captured == 1,
                 "first wheel endpoint was not captured")) return 1;
    Write(wheelBody, bodyMatrixOffset, second);
    runtime.NotifyBodyMoved(wheelBody.data(), true, wheel);
    if (!Require(runtime.CommitCompletedTick(2, 7).captured == 1,
                 "second wheel endpoint was not captured")) return 1;
    result = nullptr;
    std::uint32_t observedPropType = UINT32_MAX;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(
                         wheelSubMesh.data(), &second, result,
                         &observedPropType) ==
                         amfp::world::OverrideResult::applied &&
                     observedPropType == wheel && result != nullptr &&
                     std::abs(result->values[3] - 0.5F) < 1.0e-4F,
                 "wheel model matrix was not interpolated")) return 1;
    runtime.EndRender();

    std::array<std::uint8_t, 0x580> moveProp{};
    std::array<std::uint8_t, 0x100> moveBody{};
    std::array<std::uint8_t, 0x290> moveSubMesh{};
    std::array<void*, 1> moveBodies{moveBody.data()};
    void** moveBegin = moveBodies.data();
    void** moveEnd = moveBodies.data() + moveBodies.size();
    const std::uint32_t moveObject = 6;
    Write(moveProp, 0x348, moveBegin);
    Write(moveProp, 0x34C, moveEnd);
    Write(moveProp, 0x56C, moveObject);
    void* moveParent = moveBody.data();
    Write(moveSubMesh, parentOffset, moveParent);
    Write(moveBody, bodyMatrixOffset, first);
    if (!Require(runtime.RegisterProp(moveProp.data()).kind ==
                     amfp::world::RegistrationKind::registered,
                 "move-object registration failed")) return 1;
    runtime.NotifyBodyMoved(moveBody.data(), true);
    if (!Require(runtime.CommitCompletedTick(1, 7).captured == 1,
                 "first move-object endpoint was not captured")) return 1;
    Write(moveBody, bodyMatrixOffset, second);
    runtime.NotifyBodyMoved(moveBody.data(), true);
    if (!Require(runtime.CommitCompletedTick(2, 7).captured == 1,
                 "second move-object endpoint was not captured")) return 1;
    result = nullptr;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(moveSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::applied &&
                     result != nullptr &&
                     std::abs(result->values[3] - 0.5F) < 1.0e-4F,
                 "move-object model matrix was not interpolated")) return 1;
    runtime.EndRender();

    std::array<std::uint8_t, 0x580> doorProp{};
    std::array<std::uint8_t, 0x100> doorBody{};
    std::array<std::uint8_t, 0x290> doorSubMesh{};
    std::array<void*, 1> doorBodies{doorBody.data()};
    void** doorBegin = doorBodies.data();
    void** doorEnd = doorBodies.data() + doorBodies.size();
    const std::uint32_t swingDoor = 1;
    Write(doorProp, 0x348, doorBegin);
    Write(doorProp, 0x34C, doorEnd);
    Write(doorProp, 0x56C, swingDoor);
    void* doorParent = doorBody.data();
    Write(doorSubMesh, parentOffset, doorParent);
    Write(doorBody, bodyMatrixOffset, first);
    if (!Require(runtime.RegisterProp(doorProp.data()).kind ==
                     amfp::world::RegistrationKind::registered,
                 "swing-door registration failed")) return 1;
    runtime.NotifyBodyMoved(doorBody.data(), true);
    if (!Require(runtime.CommitCompletedTick(1, 7).captured == 1,
                 "first swing-door endpoint was not captured")) return 1;
    Write(doorBody, bodyMatrixOffset, second);
    runtime.NotifyBodyMoved(doorBody.data(), true);
    if (!Require(runtime.CommitCompletedTick(2, 7).captured == 1,
                 "second swing-door endpoint was not captured")) return 1;
    result = nullptr;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(doorSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::applied &&
                     result != nullptr &&
                     std::abs(result->values[3] - 0.5F) < 1.0e-4F,
                 "swing-door model matrix was not interpolated")) return 1;
    runtime.EndRender();

    std::array<std::uint8_t, 0x580> objectProp{};
    std::array<std::uint8_t, 0x320> objectMesh{};
    std::array<std::uint8_t, 0x100> objectBody{};
    std::array<std::uint8_t, 0x290> objectSubMesh{};
    std::array<void*, 1> objectBodies{objectBody.data()};
    std::array<void*, 1> objectSubMeshes{objectSubMesh.data()};
    void** objectBodiesBegin = objectBodies.data();
    void** objectBodiesEnd = objectBodies.data() + objectBodies.size();
    void** objectSubMeshesBegin = objectSubMeshes.data();
    void** objectSubMeshesEnd =
        objectSubMeshes.data() + objectSubMeshes.size();
    const std::uint32_t object = 0;
    void* objectMeshPointer = objectMesh.data();
    void* objectParent = objectBody.data();
    Write(objectProp, 0x348, objectBodiesBegin);
    Write(objectProp, 0x34C, objectBodiesEnd);
    Write(objectProp, 0x428, objectMeshPointer);
    Write(objectProp, 0x56C, object);
    Write(objectMesh, 0x314, objectSubMeshesBegin);
    Write(objectMesh, 0x318, objectSubMeshesEnd);
    Write(objectSubMesh, parentOffset, objectParent);
    Write(objectBody, bodyMatrixOffset, first);
    const auto objectRegistration = runtime.RegisterProp(objectProp.data());
    if (!Require(objectRegistration.kind ==
                     amfp::world::RegistrationKind::registered &&
                     objectRegistration.bodies == 1,
                 "complete generic-object registration failed")) return 1;
    runtime.NotifyBodyMoved(objectBody.data(), true);
    if (!Require(runtime.CommitCompletedTick(1, 7).captured == 1,
                 "first generic-object endpoint was not captured")) return 1;
    Write(objectBody, bodyMatrixOffset, second);
    runtime.NotifyBodyMoved(objectBody.data(), true);
    if (!Require(runtime.CommitCompletedTick(2, 7).captured == 1,
                 "second generic-object endpoint was not captured")) return 1;
    result = nullptr;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(objectSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::applied &&
                     result != nullptr &&
                     std::abs(result->values[3] - 0.5F) < 1.0e-4F,
                 "generic-object model matrix was not interpolated")) return 1;
    runtime.EndRender();

    std::array<std::uint8_t, 0x580> itemProp{};
    std::array<std::uint8_t, 0x320> itemMesh{};
    std::array<std::uint8_t, 0x100> itemBody{};
    std::array<std::uint8_t, 0x290> itemSubMesh{};
    std::array<void*, 1> itemBodies{itemBody.data()};
    std::array<void*, 1> itemSubMeshes{itemSubMesh.data()};
    void** itemBodiesBegin = itemBodies.data();
    void** itemBodiesEnd = itemBodies.data() + itemBodies.size();
    void** itemSubMeshesBegin = itemSubMeshes.data();
    void** itemSubMeshesEnd = itemSubMeshes.data() + itemSubMeshes.size();
    const std::uint32_t item = 7;
    void* itemMeshPointer = itemMesh.data();
    void* itemParent = itemBody.data();
    Write(itemProp, 0x348, itemBodiesBegin);
    Write(itemProp, 0x34C, itemBodiesEnd);
    Write(itemProp, 0x428, itemMeshPointer);
    Write(itemProp, 0x56C, item);
    Write(itemMesh, 0x314, itemSubMeshesBegin);
    Write(itemMesh, 0x318, itemSubMeshesEnd);
    Write(itemSubMesh, parentOffset, itemParent);
    Write(itemBody, bodyMatrixOffset, first);
    if (!Require(runtime.RegisterProp(itemProp.data()).kind ==
                     amfp::world::RegistrationKind::registered,
                 "complete item registration failed")) return 1;
    runtime.NotifyBodyMoved(itemBody.data(), true);
    if (!Require(runtime.CommitCompletedTick(1, 7).captured == 1,
                 "first item endpoint was not captured")) return 1;
    Write(itemBody, bodyMatrixOffset, second);
    runtime.NotifyBodyMoved(itemBody.data(), true);
    if (!Require(runtime.CommitCompletedTick(2, 7).captured == 1,
                 "second item endpoint was not captured")) return 1;
    result = nullptr;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(itemSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::applied &&
                     result != nullptr &&
                     std::abs(result->values[3] - 0.5F) < 1.0e-4F,
                 "item model matrix was not interpolated")) return 1;
    runtime.EndRender();

    std::array<std::uint8_t, 0x580> partialProp{};
    std::array<std::uint8_t, 0x320> partialMesh{};
    std::array<std::uint8_t, 0x100> partialBody{};
    std::array<std::uint8_t, 0x290> partialSubMesh{};
    std::array<std::uint8_t, 0x290> unrelatedParent{};
    std::array<void*, 1> partialBodies{partialBody.data()};
    std::array<void*, 1> partialSubMeshes{partialSubMesh.data()};
    void** partialBodiesBegin = partialBodies.data();
    void** partialBodiesEnd = partialBodies.data() + partialBodies.size();
    void** partialSubMeshesBegin = partialSubMeshes.data();
    void** partialSubMeshesEnd =
        partialSubMeshes.data() + partialSubMeshes.size();
    void* partialMeshPointer = partialMesh.data();
    void* partialParent = unrelatedParent.data();
    Write(partialProp, 0x348, partialBodiesBegin);
    Write(partialProp, 0x34C, partialBodiesEnd);
    Write(partialProp, 0x428, partialMeshPointer);
    Write(partialProp, 0x56C, object);
    Write(partialMesh, 0x314, partialSubMeshesBegin);
    Write(partialMesh, 0x318, partialSubMeshesEnd);
    Write(partialSubMesh, parentOffset, partialParent);
    if (!Require(runtime.RegisterProp(partialProp.data()).kind ==
                     amfp::world::RegistrationKind::incompleteRenderMapping,
                 "partial generic-object mapping was not rejected")) return 1;
    runtime.NotifyBodyMoved(partialBody.data(), true);
    if (!Require(runtime.CommitCompletedTick(3, 7).callbacks == 0,
                 "rejected generic-object body was tracked")) return 1;
    result = &second;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(partialSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::notTarget &&
                     result == &second,
                 "partial generic-object submesh was modified")) return 1;
    runtime.EndRender();

    std::array<std::uint8_t, 0x290> nestedIntermediate{};
    std::array<std::uint8_t, 0x290> nestedSubMesh{};
    void* nestedParent = objectBody.data();
    Write(nestedIntermediate, parentOffset, nestedParent);
    nestedParent = nestedIntermediate.data();
    Write(nestedSubMesh, parentOffset, nestedParent);
    result = &second;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(nestedSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::diagnosticNestedTarget &&
                     result == &second,
                 "nested tracked descendant was not diagnosed")) return 1;
    runtime.EndRender();

    std::array<std::uint8_t, 0x290> standaloneBody{};
    std::array<std::uint8_t, 0x290> standaloneSubMesh{};
    void* standaloneParent = standaloneBody.data();
    Write(standaloneSubMesh, parentOffset, standaloneParent);
    runtime.NotifyBodyMoved(standaloneBody.data(), false);
    const auto relationshipCommit = runtime.CommitCompletedTick(2, 7);
    if (!Require(relationshipCommit.relationshipCandidates == 1 &&
                     relationshipCommit.relationshipOverflow == 0,
                 "standalone relationship candidate was not staged")) return 1;
    result = &second;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(standaloneSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::standalonePromoted &&
                     result == &second,
                 "direct standalone render relationship was not promoted")) {
        return 1;
    }
    runtime.EndRender();

    Write(standaloneBody, bodyMatrixOffset, first);
    runtime.NotifyBodyMoved(standaloneBody.data(), false);
    if (!Require(runtime.CommitCompletedTick(3, 7).captured == 1,
                 "first standalone endpoint was not captured")) return 1;
    Write(standaloneBody, bodyMatrixOffset, second);
    runtime.NotifyBodyMoved(standaloneBody.data(), false);
    if (!Require(runtime.CommitCompletedTick(4, 7).captured == 1,
                 "second standalone endpoint was not captured")) return 1;
    amfp::clock::Sample standaloneSample = sample;
    standaloneSample.olderSerial = 3;
    standaloneSample.newerSerial = 4;
    result = nullptr;
    runtime.BeginRender(standaloneSample);
    if (!Require(runtime.OverrideModelMatrix(standaloneSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::appliedStandalone &&
                     result != nullptr &&
                     std::abs(result->values[3] - 0.5F) < 1.0e-4F,
                 "standalone model matrix was not interpolated")) return 1;
    runtime.EndRender();

    std::array<std::uint8_t, 0x580> bodylessProp{};
    Write(bodylessProp, 0x56C, object);
    if (!Require(runtime.RegisterProp(bodylessProp.data()).kind ==
                     amfp::world::RegistrationKind::bodyless,
                 "bodyless prop was not classified separately")) return 1;

    std::array<std::uint8_t, 0x580> stationaryProp{};
    std::array<std::uint8_t, 0x100> stationaryBody{};
    std::array<std::uint8_t, 0x290> stationarySubMesh{};
    std::array<void*, 1> stationaryBodies{stationaryBody.data()};
    void** stationaryBegin = stationaryBodies.data();
    void** stationaryEnd = stationaryBodies.data() + stationaryBodies.size();
    Write(stationaryProp, 0x348, stationaryBegin);
    Write(stationaryProp, 0x34C, stationaryEnd);
    Write(stationaryProp, 0x56C, lever);
    void* stationaryParent = stationaryBody.data();
    Write(stationarySubMesh, parentOffset, stationaryParent);
    if (!Require(runtime.RegisterProp(stationaryProp.data()).kind ==
                     amfp::world::RegistrationKind::registered,
                 "stationary lever registration failed")) return 1;
    result = &second;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(stationarySubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::historyUnavailable &&
                     result == &second,
                 "inactive body was not classified as waiting for history")) {
        return 1;
    }
    runtime.EndRender();

    std::array<std::uint8_t, 0x290> unrelated{};
    result = nullptr;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(unrelated.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::notTarget &&
                     result == &second,
                 "unrelated model was modified")) return 1;
    runtime.EndRender();

    // Nested relationships are classified only for telemetry.
    std::array<std::uint8_t, 0x290> deepEntity{};
    std::array<std::uint8_t, 0x290> deepSubMesh{};
    void* deepEntityParent = body.data();
    void* deepSubMeshParent = deepEntity.data();
    Write(deepEntity, parentOffset, deepEntityParent);
    Write(deepSubMesh, parentOffset, deepSubMeshParent);
    result = nullptr;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(deepSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::diagnosticNestedTarget &&
                     result == &second,
                 "nested child of a registered body was not classified")) {
        return 1;
    }
    runtime.SetRelationshipDiagnostics(false);
    if (!Require(runtime.OverrideModelMatrix(deepSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::notTarget &&
                     result == &second,
                 "nested classification ran with diagnostics disabled")) {
        return 1;
    }
    if (!Require(runtime.OverrideModelMatrix(subMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::applied,
                 "direct correction was lost with diagnostics disabled")) {
        return 1;
    }
    runtime.EndRender();

    // A map load forgets every body from the previous map.
    runtime.Reset();
    result = nullptr;
    runtime.BeginRender(sample);
    if (!Require(runtime.OverrideModelMatrix(subMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::notTarget &&
                     result == &second,
                 "registered body survived a reset")) return 1;
    if (!Require(runtime.OverrideModelMatrix(standaloneSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::notTarget &&
                     result == &second,
                 "standalone body survived a reset")) return 1;
    runtime.EndRender();
    if (!Require(runtime.RegisterProp(prop.data()).kind ==
                     amfp::world::RegistrationKind::registered,
                 "registration after a reset failed")) return 1;
    Write(body, bodyMatrixOffset, first);
    runtime.NotifyBodyMoved(body.data(), true);
    if (!Require(runtime.CommitCompletedTick(5, 7).captured == 1,
                 "first endpoint after a reset was not captured")) return 1;
    Write(body, bodyMatrixOffset, second);
    runtime.NotifyBodyMoved(body.data(), true);
    if (!Require(runtime.CommitCompletedTick(6, 7).captured == 1,
                 "second endpoint after a reset was not captured")) return 1;
    amfp::clock::Sample afterReset = sample;
    afterReset.olderSerial = 5;
    afterReset.newerSerial = 6;
    result = nullptr;
    runtime.BeginRender(afterReset);
    if (!Require(runtime.OverrideModelMatrix(subMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::applied &&
                     result != nullptr &&
                     std::abs(result->values[3] - 0.5F) < 1.0e-4F,
                 "re-registered body was not interpolated")) return 1;
    runtime.EndRender();

    // Body destruction removes exactly that body; the last one empties the
    // tables.
    if (!Require(runtime.RegisterProp(wheelProp.data()).kind ==
                     amfp::world::RegistrationKind::registered,
                 "wheel registration before destruction failed")) return 1;
    if (!Require(!runtime.NotifyBodyDestroyed(body.data()),
                 "tables were emptied while a body was still alive")) return 1;
    result = nullptr;
    runtime.BeginRender(afterReset);
    if (!Require(runtime.OverrideModelMatrix(subMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::notTarget &&
                     result == &second,
                 "destroyed body was still interpolated")) return 1;
    if (!Require(runtime.OverrideModelMatrix(wheelSubMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::historyUnavailable,
                 "surviving body was lost with a destroyed one")) return 1;
    runtime.EndRender();
    if (!Require(!runtime.NotifyBodyDestroyed(body.data()),
                 "an untracked body was reported as tracked")) return 1;
    // A new body may reuse the destroyed wrapper's address.
    if (!Require(runtime.RegisterProp(prop.data()).kind ==
                     amfp::world::RegistrationKind::registered,
                 "registration at a reused address failed")) return 1;
    result = nullptr;
    runtime.BeginRender(afterReset);
    if (!Require(runtime.OverrideModelMatrix(subMesh.data(), &second,
                                             result) ==
                     amfp::world::OverrideResult::historyUnavailable &&
                     result == &second,
                 "reused address inherited the destroyed body's history")) {
        return 1;
    }
    runtime.EndRender();
    if (!Require(!runtime.NotifyBodyDestroyed(wheelBody.data()),
                 "tables were emptied before the last body")) return 1;
    if (!Require(runtime.NotifyBodyDestroyed(body.data()),
                 "destroying the last body did not empty the tables")) {
        return 1;
    }

    // Map-load detection from the listener viewport's world.
    int worldA = 0;
    int worldB = 0;
    amfp::world::MapEpoch epoch{};
    // First load from the menu: the viewport presents no world yet.
    if (!Require(epoch.OnRegistration(&worldA, true, nullptr),
                 "first map load did not reset")) return 1;
    if (!Require(!epoch.OnRegistration(&worldA, true, nullptr),
                 "a load burst reset more than once")) return 1;
    epoch.OnPresented(&worldA);
    // A prop created during play registers into the presented world.
    if (!Require(!epoch.OnRegistration(&worldA, true, &worldA),
                 "a runtime-created prop reset the tables")) return 1;
    // Level transition: the next map loads while the old one is presented.
    if (!Require(epoch.OnRegistration(&worldB, true, &worldA),
                 "level transition did not reset")) return 1;
    if (!Require(!epoch.OnRegistration(&worldB, true, &worldA),
                 "level transition reset more than once")) return 1;
    // Saved entities are created after the new map becomes current.
    if (!Require(!epoch.OnRegistration(&worldB, true, &worldB),
                 "post-enter registration reset the tables")) return 1;
    epoch.OnPresented(&worldB);
    // Save load: the old map is destroyed first, so the new world may reuse
    // its address while the viewport presents no world.
    if (!Require(epoch.OnRegistration(&worldB, true, nullptr),
                 "same-address reload did not reset")) return 1;
    if (!Require(!epoch.OnRegistration(&worldB, true, nullptr),
                 "same-address reload reset more than once")) return 1;
    epoch.OnPresented(&worldB);
    // Save restore into another map: entities are created only after the new
    // world is already presented, so the pointer change alone must count.
    if (!Require(epoch.OnRegistration(&worldA, true, &worldA),
                 "restore into a new presented world did not reset")) return 1;
    if (!Require(!epoch.OnRegistration(&worldA, true, &worldA),
                 "restore reset more than once")) return 1;
    if (!Require(epoch.OnRegistration(&worldB, true, &worldB),
                 "second restore did not reset")) return 1;
    epoch.OnPresented(&worldB);
    // Without a known listener only a world-pointer change is trusted.
    if (!Require(!epoch.OnRegistration(&worldB, false, nullptr),
                 "unknown listener reset an unchanged world")) return 1;
    if (!Require(epoch.OnRegistration(&worldA, false, nullptr),
                 "unknown listener missed a world change")) return 1;
    if (!Require(!epoch.OnRegistration(&worldA, false, nullptr),
                 "unknown listener reset more than once")) return 1;

    std::wcout << L"world runtime tests passed\n";
    return 0;
}
