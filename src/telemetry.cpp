#include "telemetry.h"

#include "camera_audit.h"
#include "camera_runtime.h"
#include "enemy_pose_audit.h"
#include "enemy_pose_runtime.h"
#include "frame_cadence.h"
#include "hands_audit.h"
#include "hands_runtime.h"
#include "image_trail_timing.h"
#include "logger.h"
#include "mouse_look.h"
#include "node_animation_runtime.h"
#include "particle_runtime.h"
#include "presentation_clock.h"
#include "pose_audit.h"
#include "pose_runtime.h"
#include "ragdoll_pose_audit.h"
#include "ragdoll_pose_runtime.h"
#include "ragdoll_skinning_runtime.h"
#include "render_gate_patch.h"
#include "world_ownership_audit.h"
#include "world_runtime.h"

#include <MinHook.h>
#include <Windows.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace amfp::telemetry {
namespace {

using UpdaterRunMessage = void(__thiscall*)(void*, int, float);
using SceneRender = void(__thiscall*)(void*, float, std::uint32_t);
using EngineRun = void(__thiscall*)(void*);
using PlayerHandsPostUpdate = void(__thiscall*)(void*, float);
using LuxEnemyOnUpdate = void(__thiscall*)(void*, float);
using LuxPropSetupAfterLoad = void(__thiscall*)(void*, void*);
using NewtonTransformCallback = void(__cdecl*)(void*, const float*, int);
using NewtonBodyGetUserData = void*(__cdecl*)(const void*);
using NewtonDestroyBody = void(__cdecl*)(const void*, const void*);
using EntityGetWorldMatrix = const hands_audit::Matrix*(__thiscall*)(void*);
using SubMeshGetModelMatrix = const hands_audit::Matrix*(__thiscall*)(void*, void*);
using SubMeshUpdateGraphics = void(__thiscall*)(void*, float);
using MeshEntityUpdateGraphics = void(__thiscall*)(void*, float);
using ImageTrailRenderEffect = void*(__thiscall*)(void*, void*, void*);
using SdlGetTicks = std::uint32_t(__cdecl*)();

EngineRun g_originalEngineRun = nullptr;
UpdaterRunMessage g_originalUpdaterRunMessage = nullptr;
SceneRender g_originalSceneRender = nullptr;
PlayerHandsPostUpdate g_originalPlayerHandsPostUpdate = nullptr;
LuxEnemyOnUpdate g_originalLuxEnemyOnUpdate = nullptr;
LuxPropSetupAfterLoad g_originalLuxPropSetupAfterLoad = nullptr;
NewtonTransformCallback g_originalNewtonTransformCallback = nullptr;
NewtonBodyGetUserData g_newtonBodyGetUserData = nullptr;
NewtonDestroyBody g_originalNewtonDestroyBody = nullptr;
void* g_newtonDestroyBodyTarget = nullptr;
EntityGetWorldMatrix g_entityGetWorldMatrix = nullptr;
SubMeshGetModelMatrix g_originalSubMeshGetModelMatrix = nullptr;
SubMeshUpdateGraphics g_originalSubMeshUpdateGraphics = nullptr;
MeshEntityUpdateGraphics g_originalMeshEntityUpdateGraphics = nullptr;
ImageTrailRenderEffect g_originalImageTrailRenderEffect = nullptr;
LARGE_INTEGER g_qpcFrequency{};
unsigned int g_reportIntervalSeconds = 5;

std::array<std::atomic<std::uint64_t>, 7> g_messages{};
std::atomic<std::uint64_t> g_sceneRenders{0};
std::atomic<std::uint64_t> g_updaterThreadMismatches{0};
std::atomic<std::uint64_t> g_sceneThreadMismatches{0};
std::atomic<DWORD> g_updaterThread{0};
std::atomic<DWORD> g_sceneThread{0};
cadence::FrameCadence g_swapCadence{};
clock::PresentationClock g_presentationClock{1.0 / 60.0};
SdlGetTicks g_sdlGetTicks = nullptr;
void* g_logicTimer = nullptr;
bool g_clockDiagnosticsEnabled = false;
bool g_telemetryEnabled = false;
bool g_ragdollProfileEnabled = false;
bool g_clockReady = false;
bool g_renderUnlockEnabled = false;
bool g_cameraInterpolationEnabled = false;
bool g_handsAuditEnabled = false;
bool g_handsInterpolationEnabled = false;
bool g_handsPoseInterpolationEnabled = false;
bool g_poseAuditEnabled = false;
bool g_enemyPoseAuditEnabled = false;
bool g_ragdollPoseAuditEnabled = false;
bool g_ragdollPoseInterpolationEnabled = false;
bool g_enemyPoseInterpolationEnabled = false;
bool g_nodeAnimationInterpolationEnabled = false;
bool g_worldInterpolationEnabled = false;
bool g_mouseLookEnabled = false;
double g_sdlQpcOffsetMilliseconds = 0.0;
double g_nativeStepSeconds = 1.0 / 60.0;
double g_previousPresentationSeconds = 0.0;
bool g_previousPresentationValid = false;
std::atomic<std::uint64_t> g_tickSerial{0};
std::atomic<std::uint64_t> g_clockInterpolated{0};
std::atomic<std::uint64_t> g_clockUnavailable{0};
std::atomic<std::uint64_t> g_clockResets{0};
std::atomic<std::uint64_t> g_clockPhaseInvalid{0};
std::atomic<std::int64_t> g_latestAlphaMillionths{0};
std::atomic<std::int64_t> g_latestRawAlphaMillionths{0};
std::atomic<clock::ResetReason> g_lastClockReset{clock::ResetReason::none};
constexpr std::size_t kCameraAuditResultCount = 9;
std::array<std::atomic<std::uint64_t>, kCameraAuditResultCount>
    g_cameraTickAudit{};
std::array<std::atomic<std::uint64_t>, kCameraAuditResultCount>
    g_cameraRenderAudit{};
std::atomic<std::uint64_t> g_cameraIdentityChanges{0};
std::atomic<camera_audit::Result> g_lastCameraAuditResult{
    camera_audit::Result::noScene};
void* g_scene = nullptr;
void* g_lastAuditedCamera = nullptr;
camera::Runtime g_cameraRuntime{};
std::atomic<std::uint64_t> g_cameraCaptured{0};
std::atomic<std::uint64_t> g_cameraApplied{0};
std::atomic<std::uint64_t> g_cameraSkipped{0};
std::atomic<std::uint64_t> g_cameraRestoreFailures{0};
std::atomic<camera::FrameResult> g_lastCameraFrameResult{
    camera::FrameResult::clockUnavailable};
constexpr std::size_t kHandsAuditResultCount = 6;
std::array<std::atomic<std::uint64_t>, kHandsAuditResultCount> g_handsAudit{};
std::atomic<std::uint64_t> g_handsIdentityChanges{0};
std::atomic<std::uint64_t> g_handsMoved{0};
std::atomic<std::uint64_t> g_handsStable{0};
std::atomic<hands_audit::Result> g_lastHandsAuditResult{
    hands_audit::Result::noHelper};
void* g_lastHandsEntity = nullptr;
hands_audit::Matrix g_lastHandsMatrix{};
bool g_lastHandsMatrixValid = false;
void* g_lastHandObjectEntity = nullptr;
std::atomic<hands_audit::HandObjectResult> g_lastHandObjectResult{
    hands_audit::HandObjectResult::absent};
std::atomic<std::uint32_t> g_lastHandObjectBillboards{0};
std::atomic<std::uint32_t> g_lastHandObjectParticles{0};
std::atomic<std::uint32_t> g_lastHandObjectLights{0};
constexpr std::size_t kHandsModelResultCount = 4;
std::array<std::atomic<std::uint64_t>, kHandsModelResultCount>
    g_handsModelAudit{};
hands::Runtime g_handsRuntime{};
std::atomic<std::uint64_t> g_handsCaptured{0};
std::atomic<std::uint64_t> g_handsRenderReady{0};
std::atomic<std::uint64_t> g_handsFrameSkipped{0};
std::atomic<std::uint64_t> g_handsModelsApplied{0};
std::atomic<std::uint64_t> g_handObjectModelsApplied{0};
std::atomic<std::uint64_t> g_handAttachmentCandidates{0};
std::atomic<std::uint64_t> g_handAttachmentsApplied{0};
std::atomic<std::uint64_t> g_handAttachmentRestoreFailures{0};
void* g_poseHandsEntity = nullptr;
pose_audit::Tracker g_poseAudit{};
constexpr std::size_t kPoseAuditResultCount = 7;
std::array<std::atomic<std::uint64_t>, kPoseAuditResultCount> g_poseAuditResults{};
std::atomic<std::uint32_t> g_poseBoneCount{0};
std::atomic<std::uint32_t> g_poseInvalidBone{0};
std::atomic<pose_audit::Result> g_lastPoseAuditResult{
    pose_audit::Result::notTarget};
pose::Runtime g_poseRuntime{};
std::atomic<std::uint64_t> g_poseStaged{0};
std::atomic<std::uint64_t> g_poseApplied{0};
std::atomic<std::uint64_t> g_poseUnavailable{0};
std::atomic<std::uint64_t> g_poseIdentityChanges{0};
std::atomic<std::uint64_t> g_poseClockWaits{0};
std::atomic<std::uint64_t> g_posePairMisses{0};
std::atomic<std::uint64_t> g_poseInvalid{0};
std::atomic<std::uint64_t> g_poseRestoreFailures{0};
enemy_pose_audit::Registry g_enemyPoseAudit{};
constexpr std::size_t kEnemyStageResultCount = 6;
constexpr std::size_t kEnemyObserveResultCount = 7;
std::array<std::atomic<std::uint64_t>, kEnemyStageResultCount>
    g_enemyStageResults{};
std::array<std::atomic<std::uint64_t>, kEnemyObserveResultCount>
    g_enemyObserveResults{};
std::array<std::atomic<std::uint64_t>, enemy_pose_audit::kEnemyTypeCount>
    g_enemyStagedByType{};
std::array<std::atomic<std::uint64_t>, enemy_pose_audit::kEnemyTypeCount>
    g_enemyObservedByType{};
std::array<std::atomic<std::uint64_t>, enemy_pose_audit::kEnemyTypeCount>
    g_enemyChangedByType{};
std::array<std::atomic<std::uint64_t>, enemy_pose_audit::kEnemyTypeCount>
    g_enemyStableByType{};
std::array<std::atomic<std::uint64_t>, enemy_pose_audit::kEnemyTypeCount>
    g_enemyIdentityByType{};
std::array<std::atomic<std::uint32_t>, enemy_pose_audit::kEnemyTypeCount>
    g_enemyBoneCountByType{};
std::atomic<std::uint32_t> g_enemyInvalidBone{0};
std::atomic<enemy_pose_audit::StageResult> g_lastEnemyStageResult{
    enemy_pose_audit::StageResult::invalidEnemy};
std::atomic<enemy_pose_audit::ObserveResult> g_lastEnemyObserveResult{
    enemy_pose_audit::ObserveResult::notTarget};
enemy_pose::Runtime g_enemyPoseRuntime{};
std::atomic<std::uint64_t> g_enemyPoseRuntimeStaged{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimeUnsupported{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimeInactive{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimeUnavailable{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimeCapacity{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimeCaptured{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimeApplied{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimeIdentity{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimeClockWaits{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimePairMisses{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimeInvalid{0};
std::atomic<std::uint64_t> g_enemyPoseRuntimeRestoreFailures{0};
std::atomic<enemy_pose::UpdateResult> g_lastEnemyPoseRuntimeResult{
    enemy_pose::UpdateResult::notTarget};
ragdoll_pose_audit::Audit g_ragdollPoseAudit{};
constexpr std::size_t kRagdollPoseResultCount = 9;
std::array<std::atomic<std::uint64_t>, kRagdollPoseResultCount>
    g_ragdollPoseResults{};
std::atomic<std::uint32_t> g_ragdollBoneCount{0};
std::atomic<std::uint32_t> g_ragdollInvalidBone{0};
std::atomic<ragdoll_pose_audit::Result> g_lastRagdollPoseResult{
    ragdoll_pose_audit::Result::notTarget};
ragdoll_pose::Runtime g_ragdollPoseRuntime{};
std::atomic<std::uint64_t> g_ragdollRuntimeCaptured{0};
std::atomic<std::uint64_t> g_ragdollRuntimeApplied{0};
std::atomic<std::uint64_t> g_ragdollRuntimeIdentity{0};
std::atomic<std::uint64_t> g_ragdollRuntimeUnavailable{0};
std::atomic<std::uint64_t> g_ragdollRuntimeClockWaits{0};
std::atomic<std::uint64_t> g_ragdollRuntimePairMisses{0};
std::atomic<std::uint64_t> g_ragdollRuntimeStationary{0};
std::atomic<std::uint64_t> g_ragdollRuntimeInvalid{0};
std::atomic<std::uint64_t> g_ragdollRuntimeCapacity{0};
std::atomic<std::uint64_t> g_ragdollRuntimeRestoreFailures{0};
std::atomic<std::uint64_t> g_ragdollSkinningForced{0};
std::atomic<std::uint64_t> g_ragdollSkinningUnavailable{0};
std::atomic<std::uint64_t> g_ragdollSkinningInvalidFlag{0};
std::atomic<ragdoll_skinning::PrepareResult> g_lastRagdollSkinningResult{
    ragdoll_skinning::PrepareResult::notTarget};
std::atomic<std::uint64_t> g_ragdollProfileCalls{0};
std::atomic<std::uint64_t> g_ragdollProfileQpcTicks{0};
std::atomic<std::uint64_t> g_ragdollProfileMaxQpcTicks{0};
std::atomic<std::uint64_t> g_ragdollProfileVertices{0};
std::atomic<std::uint64_t> g_ragdollProfileBones{0};
std::atomic<std::uint32_t> g_ragdollProfileMaxVertices{0};
std::atomic<std::uint32_t> g_ragdollProfileMaxBones{0};
std::atomic<std::uintptr_t> g_ragdollProfileMaxSubMesh{0};
thread_local bool g_imageTrailDetourActive = false;
std::atomic<bool> g_imageTrailAdjustedLogged{false};
std::atomic<bool> g_imageTrailInvalidLogged{false};
std::atomic<bool> g_imageTrailRestoreFailureLogged{false};
std::atomic<ragdoll_pose::UpdateResult> g_lastRagdollRuntimeResult{
    ragdoll_pose::UpdateResult::notTarget};
std::atomic<std::uint64_t> g_newtonTransformCallbacks{0};
std::atomic<std::uint64_t> g_newtonTransformThreadZero{0};
std::atomic<std::uint64_t> g_newtonTransformWorkerThread{0};
std::atomic<std::uint64_t> g_newtonTransformInvalidThread{0};
world_ownership::Registry g_worldOwnership{};
node_animation::Runtime g_nodeAnimationRuntime{};
using ParticleEmitterUpdateGraphics = bool(__thiscall*)(void*, void*, float);
ParticleEmitterUpdateGraphics g_originalParticleEmitterUpdateGraphics = nullptr;
bool g_particleInterpolationEnabled = false;
particle::Runtime g_particleRuntime{};
void LogParticleSummary();
// Indexed by eLuxEnemyType; written only on the scene thread.
std::array<std::uint64_t, 5> g_enemySummaryApplied{};
std::array<std::uint64_t, 5> g_enemySummaryInvalid{};
std::array<std::size_t, 5> g_enemySummaryBones{};
// Model-matrix history for the submeshes of tracked enemy meshes.
node_animation::Runtime g_enemyRootRuntime{};
std::uint64_t g_enemyRootApplied = 0;
std::uint64_t g_enemyRootInvalid = 0;
std::uint64_t g_enemyReskins = 0;
std::atomic<std::uint64_t> g_nodeAnimationCaptured{0};
std::atomic<std::uint64_t> g_nodeAnimationChanged{0};
std::atomic<std::uint64_t> g_nodeAnimationApplied{0};
std::atomic<std::uint64_t> g_nodeAnimationStable{0};
std::atomic<std::uint64_t> g_nodeAnimationClockWaits{0};
std::atomic<std::uint64_t> g_nodeAnimationPairMisses{0};
std::atomic<std::uint64_t> g_nodeAnimationInvalid{0};
std::atomic<std::uint64_t> g_nodeAnimationCapacity{0};
std::atomic<node_animation::OverrideResult> g_lastNodeAnimationResult{
    node_animation::OverrideResult::notTarget};
constexpr std::size_t kOwnershipRegisterResultCount = 4;
std::array<std::atomic<std::uint64_t>, kOwnershipRegisterResultCount>
    g_ownershipRegisterResults{};
std::atomic<std::uint64_t> g_ownershipRegisteredBodies{0};
std::atomic<std::uint64_t> g_ownershipStaticProps{0};
std::atomic<std::uint64_t> g_ownershipRenderValidProps{0};
std::atomic<std::uint64_t> g_ownershipRenderInvalidProps{0};
std::atomic<std::uint64_t> g_ownershipSubMeshes{0};
std::atomic<std::uint64_t> g_ownershipDirectBodyChildren{0};
std::atomic<std::uint64_t> g_ownershipNestedBodyChildren{0};
std::atomic<std::uint64_t> g_ownershipUnmatchedSubMeshes{0};
std::array<std::atomic<std::uint64_t>, world_ownership::kPropTypeCount>
    g_ownershipSubMeshesByPropType{};
std::array<std::atomic<std::uint64_t>, world_ownership::kPropTypeCount>
    g_ownershipDirectByPropType{};
std::array<std::atomic<std::uint64_t>, world_ownership::kPropTypeCount>
    g_ownershipNestedByPropType{};
std::array<std::atomic<std::uint64_t>, world_ownership::kPropTypeCount>
    g_ownershipUnmatchedByPropType{};
std::atomic<std::uint64_t> g_newtonOwnedCallbacks{0};
std::atomic<std::uint64_t> g_newtonUnownedCallbacks{0};
std::array<std::atomic<std::uint64_t>, world_ownership::kPropTypeCount>
    g_newtonCallbacksByPropType{};
world::Runtime g_worldRuntime{};
world::MapEpoch g_worldMapEpoch{};
std::uint64_t g_worldMapResets = 0;
std::atomic<std::uint64_t> g_worldTableDrains{0};
std::uint64_t g_worldTableDrainsLogged = 0;
constexpr std::size_t kWorldRegistrationKindCount = 6;
std::array<std::atomic<std::uint64_t>, kWorldRegistrationKindCount>
    g_worldRegistrationResults{};
std::atomic<std::uint64_t> g_worldRegistrationBodies{0};
std::atomic<std::uint64_t> g_worldCallbacksStaged{0};
std::atomic<std::uint64_t> g_worldBodiesCaptured{0};
std::atomic<std::uint64_t> g_worldDuplicateCallbacks{0};
std::atomic<std::uint64_t> g_worldCaptureInvalid{0};
std::atomic<std::uint64_t> g_worldMoveOverflow{0};
std::atomic<std::uint64_t> g_worldModelsApplied{0};
std::atomic<std::uint64_t> g_worldStandaloneModelsApplied{0};
std::atomic<std::uint64_t> g_worldClockUnavailable{0};
std::atomic<std::uint64_t> g_worldHistoryUnavailable{0};
std::atomic<std::uint64_t> g_worldPairMissing{0};
std::atomic<std::uint64_t> g_worldOverrideInvalid{0};
std::atomic<std::uint64_t> g_worldOutputOverflow{0};
std::atomic<std::uint64_t> g_worldRelationshipCandidates{0};
std::atomic<std::uint64_t> g_worldRelationshipOverflow{0};
std::atomic<std::uint64_t> g_worldNestedTarget{0};
std::atomic<std::uint64_t> g_worldOwnedDirect{0};
std::atomic<std::uint64_t> g_worldOwnedNested{0};
std::atomic<std::uint64_t> g_worldStandaloneDirect{0};
std::atomic<std::uint64_t> g_worldStandaloneNested{0};
std::atomic<std::uint64_t> g_worldStandalonePromoted{0};
std::array<std::atomic<std::uint64_t>, world_ownership::kPropTypeCount>
    g_worldAppliedByPropType{};
std::array<std::atomic<std::uint64_t>, world_ownership::kPropTypeCount>
    g_worldNestedTargetByPropType{};
std::array<std::atomic<std::uint64_t>, world_ownership::kPropTypeCount>
    g_worldOwnedDirectByPropType{};
std::array<std::atomic<std::uint64_t>, world_ownership::kPropTypeCount>
    g_worldOwnedNestedByPropType{};
std::atomic<world::OverrideResult> g_lastWorldOverride{
    world::OverrideResult::notTarget};

enum class RenderUnlockState : std::uint32_t {
    disabled,
    pending,
    active,
    rejected,
    finished,
};

std::atomic<RenderUnlockState> g_renderUnlockState{RenderUnlockState::disabled};
constexpr std::size_t kEngineLimitFpsOffset = 0x74;
constexpr std::size_t kEngineLogicTimerOffset = 0x18;
constexpr std::size_t kLogicTimerLocalTimeOffset = 0x00;
constexpr std::size_t kLogicTimerStepMillisecondsOffset = 0x08;
constexpr std::size_t kLogicTimerMaxUpdatesOffset = 0x10;
constexpr std::size_t kLogicTimerUpdateCountOffset = 0x14;
constexpr std::size_t kLogicTimerLowLevelSystemOffset = 0x18;
constexpr std::size_t kLogicTimerReadableBytes = 0x1C;
constexpr double kPresentationHeadroomFraction = 0.55;
render_gate::Patch g_renderGatePatch{};

const wchar_t* RenderUnlockStateName(const RenderUnlockState state) noexcept {
    switch (state) {
    case RenderUnlockState::disabled:
        return L"disabled";
    case RenderUnlockState::pending:
        return L"pending";
    case RenderUnlockState::active:
        return L"active";
    case RenderUnlockState::rejected:
        return L"rejected";
    case RenderUnlockState::finished:
        return L"finished";
    }
    return L"unknown";
}

bool IsWritableMemory(const void* address, const std::size_t bytes) noexcept {
    if (address == nullptr || bytes == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((memory.Protect & writable) == 0) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + memory.RegionSize;
    return begin >= regionBegin && begin <= regionEnd && bytes <= regionEnd - begin;
}

bool IsReadableMemory(const void* address, const std::size_t bytes) noexcept {
    if (address == nullptr || bytes == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY;
    if ((memory.Protect & readable) == 0) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + memory.RegionSize;
    return begin >= regionBegin && begin <= regionEnd && bytes <= regionEnd - begin;
}

double QpcSeconds() noexcept {
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter) || g_qpcFrequency.QuadPart <= 0) {
        return 0.0;
    }
    return static_cast<double>(counter.QuadPart) /
           static_cast<double>(g_qpcFrequency.QuadPart);
}

// cScene::mpCurrentListener is the map handler's viewport for the whole
// session and cScene::PostUpdate dereferences it every tick, so its world
// field is read directly. Returns false while no listener is known.
bool ReadListenerWorld(void*& world) noexcept {
    constexpr std::size_t kSceneCurrentListenerOffset = 0x40;
    constexpr std::size_t kViewportWorldOffset = 0x08;
    world = nullptr;
    if (g_scene == nullptr) {
        return false;
    }
    void* viewport = nullptr;
    std::memcpy(&viewport,
                static_cast<const std::uint8_t*>(g_scene) +
                    kSceneCurrentListenerOffset,
                sizeof(viewport));
    if (viewport == nullptr) {
        return false;
    }
    std::memcpy(&world,
                static_cast<const std::uint8_t*>(viewport) +
                    kViewportWorldOffset,
                sizeof(world));
    return true;
}

void RecordClockReset(const clock::ResetReason reason) noexcept {
    if (reason == clock::ResetReason::none) {
        return;
    }
    g_clockResets.fetch_add(1, std::memory_order_relaxed);
    g_lastClockReset.store(reason, std::memory_order_relaxed);
}

void RecordCameraAudit(const camera_audit::Observation& observation,
                       const bool renderSample) noexcept {
    const std::size_t index = static_cast<std::size_t>(observation.result);
    if (index < kCameraAuditResultCount) {
        auto& counters = renderSample ? g_cameraRenderAudit : g_cameraTickAudit;
        counters[index].fetch_add(1, std::memory_order_relaxed);
    }
    g_lastCameraAuditResult.store(observation.result, std::memory_order_relaxed);
    if ((observation.result == camera_audit::Result::validEuler ||
         observation.result == camera_audit::Result::validMatrix) &&
        observation.camera != g_lastAuditedCamera) {
        g_lastAuditedCamera = observation.camera;
        g_cameraIdentityChanges.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecordCameraFrame(const camera::FrameResult result) noexcept {
    g_lastCameraFrameResult.store(result, std::memory_order_relaxed);
    if (result == camera::FrameResult::captured) {
        g_cameraCaptured.fetch_add(1, std::memory_order_relaxed);
    } else if (result == camera::FrameResult::applied) {
        g_cameraApplied.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_cameraSkipped.fetch_add(1, std::memory_order_relaxed);
    }
}

const hands_audit::Matrix* ReadHandsWorldMatrix(void* const entity) {
    return g_entityGetWorldMatrix == nullptr ? nullptr
                                             : g_entityGetWorldMatrix(entity);
}

void RecordHandsAudit(const hands_audit::Observation& observation) noexcept {
    const std::size_t index = static_cast<std::size_t>(observation.result);
    if (index < kHandsAuditResultCount) {
        g_handsAudit[index].fetch_add(1, std::memory_order_relaxed);
    }
    g_lastHandsAuditResult.store(observation.result, std::memory_order_relaxed);
    if (observation.result != hands_audit::Result::valid) {
        g_poseHandsEntity = nullptr;
        g_poseAudit.Reset();
        g_lastHandsMatrixValid = false;
        g_lastHandObjectEntity = nullptr;
        return;
    }
    g_poseHandsEntity = observation.entity;
    g_lastHandObjectResult.store(observation.handObjectResult,
                                 std::memory_order_relaxed);
    g_lastHandObjectEntity = observation.handObjectResult ==
                                     hands_audit::HandObjectResult::valid
                                 ? observation.handObjectEntity
                                 : nullptr;
    g_lastHandObjectBillboards.store(observation.billboardCount,
                                     std::memory_order_relaxed);
    g_lastHandObjectParticles.store(observation.particleSystemCount,
                                    std::memory_order_relaxed);
    g_lastHandObjectLights.store(observation.lightCount,
                                 std::memory_order_relaxed);
    if (observation.entity != g_lastHandsEntity) {
        g_lastHandsEntity = observation.entity;
        g_lastHandsMatrix = observation.world;
        g_lastHandsMatrixValid = true;
        g_handsIdentityChanges.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_lastHandsMatrixValid &&
        hands_audit::Changed(g_lastHandsMatrix, observation.world)) {
        g_handsMoved.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_handsStable.fetch_add(1, std::memory_order_relaxed);
    }
    g_lastHandsMatrix = observation.world;
    g_lastHandsMatrixValid = true;
}

bool AcquireLogicTimer(void* engine) noexcept {
    g_clockReady = false;
    g_logicTimer = nullptr;
    if (!g_clockDiagnosticsEnabled || engine == nullptr || g_sdlGetTicks == nullptr) {
        return false;
    }

    void* timer = nullptr;
    const auto* timerField = static_cast<const std::uint8_t*>(engine) +
                             kEngineLogicTimerOffset;
    if (!IsReadableMemory(timerField, sizeof(timer)) ||
        (std::memcpy(&timer, timerField, sizeof(timer)),
         !IsReadableMemory(timer, kLogicTimerReadableBytes))) {
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(timer);
    const double nextTick = *reinterpret_cast<const double*>(
        bytes + kLogicTimerLocalTimeOffset);
    const double stepMilliseconds = *reinterpret_cast<const double*>(
        bytes + kLogicTimerStepMillisecondsOffset);
    const std::int32_t maxUpdates = *reinterpret_cast<const std::int32_t*>(
        bytes + kLogicTimerMaxUpdatesOffset);
    const std::int32_t updateCount = *reinterpret_cast<const std::int32_t*>(
        bytes + kLogicTimerUpdateCountOffset);
    void* lowLevelSystem = nullptr;
    std::memcpy(&lowLevelSystem, bytes + kLogicTimerLowLevelSystemOffset,
                sizeof(lowLevelSystem));
    if (!std::isfinite(nextTick) || !std::isfinite(stepMilliseconds) ||
        stepMilliseconds < 1.0 || stepMilliseconds > 1000.0 ||
        maxUpdates < 1 || maxUpdates > 1000 || updateCount < 0 ||
        updateCount > maxUpdates + 1 ||
        !IsReadableMemory(lowLevelSystem, sizeof(void*))) {
        return false;
    }

    g_logicTimer = timer;
    g_nativeStepSeconds = stepMilliseconds / 1000.0;
    g_presentationClock = clock::PresentationClock(g_nativeStepSeconds);
    g_tickSerial.store(0, std::memory_order_relaxed);
    g_previousPresentationValid = false;
    g_sdlQpcOffsetMilliseconds =
        static_cast<double>(g_sdlGetTicks()) - QpcSeconds() * 1000.0;
    g_clockReady = true;
    return true;
}

bool ReadLogicTimerAlpha(const double qpcSeconds,
                         const double presentationDelayTicks,
                         double& alpha,
                         double& rawAlpha) noexcept {
    alpha = 0.0;
    rawAlpha = 0.0;
    // AcquireLogicTimer validated this engine-owned timer once at cEngine::Run
    // entry and the pointer is dropped when Run returns, so the per-render
    // phase read does not repeat the OS memory query.
    if (!g_clockReady || g_logicTimer == nullptr) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(g_logicTimer);
    const double nextTick = *reinterpret_cast<const double*>(
        bytes + kLogicTimerLocalTimeOffset);
    const double stepMilliseconds = *reinterpret_cast<const double*>(
        bytes + kLogicTimerStepMillisecondsOffset);
    const std::int32_t maxUpdates = *reinterpret_cast<const std::int32_t*>(
        bytes + kLogicTimerMaxUpdatesOffset);
    const std::int32_t updateCount = *reinterpret_cast<const std::int32_t*>(
        bytes + kLogicTimerUpdateCountOffset);
    if (maxUpdates < 1 || maxUpdates > 1000 || updateCount < 0 ||
        updateCount > maxUpdates + 1) {
        return false;
    }
    const double nowMilliseconds =
        qpcSeconds * 1000.0 + g_sdlQpcOffsetMilliseconds;
    return clock::LogicTimerAlpha(nextTick, stepMilliseconds, 1.0,
                                  nowMilliseconds, presentationDelayTicks,
                                  alpha, &rawAlpha);
}

void __fastcall EngineRunDetour(void* self, void*) {
    if (self == nullptr) {
        if (g_renderUnlockEnabled) {
            g_renderUnlockState.store(RenderUnlockState::rejected,
                                      std::memory_order_release);
        }
        log::Write(log::Level::error,
                   L"cEngine::Run received a null instance; runtime features "
                   L"were rejected.");
        g_originalEngineRun(self);
        return;
    }

    if (g_clockDiagnosticsEnabled) {
        if (AcquireLogicTimer(self)) {
            log::Write(log::Level::info,
                       L"Presentation clock acquired cEngine::mpLogicTimer "
                       L"(field offset 0x18): native step=" +
                       std::to_wstring(g_nativeStepSeconds * 1000.0) + L" ms.");
        } else {
            log::Write(log::Level::error,
                       L"Presentation clock rejected the AMFP logic-timer "
                       L"runtime layout; phase sampling will fail closed.");
        }
    }

    if (!g_renderUnlockEnabled) {
        g_originalEngineRun(self);
        g_clockReady = false;
        g_logicTimer = nullptr;
        return;
    }

    auto* const limitFps =
        static_cast<std::uint8_t*>(self) + kEngineLimitFpsOffset;
    if (!IsWritableMemory(limitFps, sizeof(*limitFps))) {
        g_renderUnlockState.store(RenderUnlockState::rejected,
                                  std::memory_order_release);
        log::Write(log::Level::error,
                   L"Render unlock rejected: cEngine::mbLimitFPS is not writable.");
        g_originalEngineRun(self);
        return;
    }

    const std::uint8_t originalValue = *limitFps;
    if (originalValue > 1U) {
        g_renderUnlockState.store(RenderUnlockState::rejected,
                                  std::memory_order_release);
        log::Write(log::Level::error,
                   L"Render unlock rejected: cEngine::mbLimitFPS failed its "
                   L"runtime boolean layout check.");
        g_originalEngineRun(self);
        return;
    }

    std::wstring patchError;
    if (!render_gate::Apply(g_renderGatePatch, patchError)) {
        g_renderUnlockState.store(RenderUnlockState::rejected,
                                  std::memory_order_release);
        log::Write(log::Level::error,
                   L"Render unlock rejected: " + patchError + L".");
        g_originalEngineRun(self);
        return;
    }

    *limitFps = 0;
    g_renderUnlockState.store(RenderUnlockState::active,
                              std::memory_order_release);
    log::Write(log::Level::info,
               L"Render unlock applied at cEngine::Run: mbLimitFPS=false "
               L"(field offset 0x74) and the AMFP-only render/update gate was "
               L"bypassed; fixed-step logic is unchanged.");
    g_originalEngineRun(self);
    g_clockReady = false;
    g_logicTimer = nullptr;
    if (g_mouseLookEnabled) {
        mouse_look::LogSummary();
    }
    if (g_particleInterpolationEnabled) {
        LogParticleSummary();
    }
    if (g_enemyPoseInterpolationEnabled) {
        log::Write(
            log::Level::info,
            L"Enemy pose summary (applied/invalid/bones): ManPig=" +
                std::to_wstring(g_enemySummaryApplied[2]) + L"/" +
                std::to_wstring(g_enemySummaryInvalid[2]) + L"/" +
                std::to_wstring(g_enemySummaryBones[2]) + L" Child=" +
                std::to_wstring(g_enemySummaryApplied[4]) + L"/" +
                std::to_wstring(g_enemySummaryInvalid[4]) + L"/" +
                std::to_wstring(g_enemySummaryBones[4]) +
                L" restoreFailures=" +
                std::to_wstring(g_enemyPoseRuntimeRestoreFailures.load(
                    std::memory_order_relaxed)) +
                L" root(applied/invalid)=" +
                std::to_wstring(g_enemyRootApplied) + L"/" +
                std::to_wstring(g_enemyRootInvalid) +
                L" reskins=" + std::to_wstring(g_enemyReskins) +
                L".");
    }
    if (IsWritableMemory(limitFps, sizeof(*limitFps))) {
        *limitFps = originalValue;
    }
    patchError.clear();
    if (!render_gate::Restore(g_renderGatePatch, patchError)) {
        g_renderUnlockState.store(RenderUnlockState::rejected,
                                  std::memory_order_release);
        log::Write(log::Level::error,
                   L"Render gate restoration failed: " + patchError + L".");
    } else {
        g_renderUnlockState.store(RenderUnlockState::finished,
                                  std::memory_order_release);
    }
}

void ObserveThread(std::atomic<DWORD>& expected,
                   std::atomic<std::uint64_t>& mismatches) noexcept {
    const DWORD current = GetCurrentThreadId();
    DWORD first = expected.load(std::memory_order_relaxed);
    if (first == 0) {
        expected.compare_exchange_strong(first, current, std::memory_order_relaxed);
        first = expected.load(std::memory_order_relaxed);
    }
    if (first != current) {
        mismatches.fetch_add(1, std::memory_order_relaxed);
    }
}

void __fastcall UpdaterRunMessageDetour(void* self,
                                        void*,
                                        const int message,
                                        const float value) {
    ObserveThread(g_updaterThread, g_updaterThreadMismatches);
    // cGuiSet::DrawMouse queues the software cursor while the OnDraw
    // listeners run, so pending movement has to be forwarded before them.
    if (message == 2 && g_mouseLookEnabled) { // eUpdateableMessage_OnDraw
        mouse_look::PresentGuiCursor();
    }
    g_originalUpdaterRunMessage(self, message, value);

    if (message >= 0 && message < static_cast<int>(g_messages.size())) {
        g_messages[static_cast<std::size_t>(message)].fetch_add(
            1, std::memory_order_relaxed);
    }
    if (message == 0) { // eUpdateableMessage_OnPostBufferSwap
        LARGE_INTEGER counter{};
        if (QueryPerformanceCounter(&counter)) {
            g_swapCadence.RecordQpc(counter.QuadPart, g_qpcFrequency.QuadPart);
        }
    }
    if (message == 6 && g_clockReady) { // eUpdateableMessage_PostUpdate
        const std::uint64_t serial =
            g_tickSerial.fetch_add(1, std::memory_order_relaxed) + 1;
        const clock::PublishResult published =
            g_presentationClock.Publish(serial, QpcSeconds());
        RecordClockReset(published.resetReason);
        if (g_cameraInterpolationEnabled) {
            RecordCameraFrame(g_cameraRuntime.CaptureCompletedTick(
                serial, g_presentationClock.Generation()));
        }
        if (g_handsInterpolationEnabled) {
            const hands::FrameResult result =
                g_handsRuntime.CommitCompletedTick(
                    serial, g_presentationClock.Generation());
            if (result == hands::FrameResult::captured ||
                result == hands::FrameResult::identityChanged) {
                g_handsCaptured.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_handsFrameSkipped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (g_handsPoseInterpolationEnabled) {
            g_poseRuntime.CommitCompletedTick(
                serial, g_presentationClock.Generation());
        }
        if (g_enemyPoseInterpolationEnabled) {
            g_enemyPoseRuntime.CommitCompletedTick(
                serial, g_presentationClock.Generation());
        }
        if (g_worldInterpolationEnabled) {
            const world::CommitResult result =
                g_worldRuntime.CommitCompletedTick(
                    serial, g_presentationClock.Generation());
            g_worldCallbacksStaged.fetch_add(result.callbacks,
                                              std::memory_order_relaxed);
            g_worldBodiesCaptured.fetch_add(result.captured,
                                             std::memory_order_relaxed);
            g_worldDuplicateCallbacks.fetch_add(result.duplicate,
                                                 std::memory_order_relaxed);
            g_worldCaptureInvalid.fetch_add(result.invalid,
                                             std::memory_order_relaxed);
            g_worldMoveOverflow.fetch_add(result.overflow,
                                           std::memory_order_relaxed);
            g_worldRelationshipCandidates.fetch_add(
                result.relationshipCandidates, std::memory_order_relaxed);
            g_worldRelationshipOverflow.fetch_add(
                result.relationshipOverflow, std::memory_order_relaxed);
        }
    }
    if (message == 6 && g_telemetryEnabled) {
        RecordCameraAudit(camera_audit::Observe(g_scene), false);
    }
}

void __fastcall PlayerHandsPostUpdateDetour(void* self,
                                            void*,
                                            const float timeStep) {
    g_originalPlayerHandsPostUpdate(self, timeStep);
    if (g_handsAuditEnabled) {
        RecordHandsAudit(
            hands_audit::Observe(self, &ReadHandsWorldMatrix));
    }
    if (g_handsInterpolationEnabled &&
        g_handsRuntime.Stage(self) != hands::FrameResult::staged) {
        g_handsFrameSkipped.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_handsPoseInterpolationEnabled) {
        const pose::StageResult result = g_poseRuntime.StageHands(
            self, g_tickSerial.load(std::memory_order_relaxed) + 1);
        if (result == pose::StageResult::staged) {
            g_poseStaged.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_poseUnavailable.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void __fastcall LuxEnemyOnUpdateDetour(void* self,
                                       void*,
                                       const float timeStep) {
    g_originalLuxEnemyOnUpdate(self, timeStep);
    if (g_enemyPoseAuditEnabled) {
        const enemy_pose_audit::StageObservation observation =
            g_enemyPoseAudit.StageEnemy(self);
        const std::size_t resultIndex =
            static_cast<std::size_t>(observation.result);
        if (resultIndex < g_enemyStageResults.size()) {
            g_enemyStageResults[resultIndex].fetch_add(
                1, std::memory_order_relaxed);
        }
        g_lastEnemyStageResult.store(observation.result,
                                     std::memory_order_relaxed);
        if (observation.type < g_enemyStagedByType.size() &&
            (observation.result == enemy_pose_audit::StageResult::registered ||
             observation.result == enemy_pose_audit::StageResult::staged)) {
            g_enemyStagedByType[observation.type].fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    if (g_enemyPoseInterpolationEnabled) {
        const enemy_pose::StageObservation observation =
            g_enemyPoseRuntime.StageEnemy(
                self, g_tickSerial.load(std::memory_order_relaxed) + 1);
        switch (observation.result) {
        case enemy_pose::StageResult::staged:
            g_enemyPoseRuntimeStaged.fetch_add(1, std::memory_order_relaxed);
            break;
        case enemy_pose::StageResult::unsupportedType:
            g_enemyPoseRuntimeUnsupported.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case enemy_pose::StageResult::inactive:
            g_enemyPoseRuntimeInactive.fetch_add(1, std::memory_order_relaxed);
            break;
        case enemy_pose::StageResult::unavailable:
            g_enemyPoseRuntimeUnavailable.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case enemy_pose::StageResult::capacityExhausted:
            g_enemyPoseRuntimeCapacity.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
}

void __cdecl NewtonTransformCallbackDetour(void* const newtonBody,
                                           const float* const matrix,
                                           const int threadIndex) {
    g_originalNewtonTransformCallback(newtonBody, matrix, threadIndex);
    g_newtonTransformCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (threadIndex == 0) {
        g_newtonTransformThreadZero.fetch_add(1, std::memory_order_relaxed);
    } else if (threadIndex > 0 && threadIndex < 64) {
        g_newtonTransformWorkerThread.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_newtonTransformInvalidThread.fetch_add(1,
                                                 std::memory_order_relaxed);
    }
    void* const physicsBody = g_newtonBodyGetUserData(newtonBody);
    std::uint32_t propType = 0;
    const bool ownedByProp =
        g_worldOwnership.ClassifyBody(physicsBody, propType);
    if (g_worldInterpolationEnabled) {
        g_worldRuntime.NotifyBodyMoved(physicsBody, ownedByProp, propType);
    }
    if (ownedByProp) {
        g_newtonOwnedCallbacks.fetch_add(1, std::memory_order_relaxed);
        g_newtonCallbacksByPropType[propType].fetch_add(
            1, std::memory_order_relaxed);
    } else {
        g_newtonUnownedCallbacks.fetch_add(1, std::memory_order_relaxed);
    }
}

// Every cPhysicsBodyNewton is destroyed through this Newton export
// (cPhysicsBodyNewton::DeleteLowLevel), including at map unload, so the
// pointer-keyed world tables forget a body before its address can be reused.
void __cdecl NewtonDestroyBodyDetour(const void* const newtonWorld,
                                     const void* const newtonBody) {
    if (newtonBody != nullptr) {
        void* const physicsBody = g_newtonBodyGetUserData(newtonBody);
        g_worldOwnership.NotifyBodyDestroyed(physicsBody);
        if (g_worldRuntime.NotifyBodyDestroyed(physicsBody)) {
            g_worldTableDrains.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_originalNewtonDestroyBody(newtonWorld, newtonBody);
}

// The emitter rebuilds its vertex buffer from the particle positions inside
// the original call, so the presentation positions exist only for its
// duration. The runtime is active only between Begin/EndRender on the scene
// thread.
bool __fastcall ParticleEmitterUpdateGraphicsDetour(void* const self,
                                                    void*,
                                                    void* const frustum,
                                                    const float frameTime) {
    const bool applied =
        g_particleRuntime.Apply(self) == particle::ApplyResult::applied;
    const bool result =
        g_originalParticleEmitterUpdateGraphics(self, frustum, frameTime);
    if (applied) {
        g_particleRuntime.Restore();
    }
    return result;
}

void LogParticleSummary() {
    const particle::Stats& s = g_particleRuntime.Statistics();
    log::Write(log::Level::info,
               L"Particle summary: calls=" + std::to_wstring(s.calls) +
                   L" applied=" + std::to_wstring(s.applied) +
                   L" positions=" + std::to_wstring(s.positions) +
                   L" stationary=" + std::to_wstring(s.stationary) +
                   L" unknown=" + std::to_wstring(s.unknown) +
                   L" empty=" + std::to_wstring(s.empty) +
                   L" inactive=" + std::to_wstring(s.inactive) +
                   L" invalid=" + std::to_wstring(s.invalid) +
                   L" capacity=" + std::to_wstring(s.capacity) + L".");
}

void __fastcall LuxPropSetupAfterLoadDetour(void* const self,
                                            void*,
                                            void* const world) {
    g_originalLuxPropSetupAfterLoad(self, world);
    void* listenerWorld = nullptr;
    const bool listenerKnown = ReadListenerWorld(listenerWorld);
    if (g_worldMapEpoch.OnRegistration(world, listenerKnown, listenerWorld)) {
        g_worldOwnership.Reset();
        g_worldRuntime.Reset();
        // Once per map load, outside the render and physics paths.
        log::Write(log::Level::info,
                   L"World-body tables reset for map load #" +
                       std::to_wstring(++g_worldMapResets) + L".");
    }
    const std::uint64_t drains =
        g_worldTableDrains.load(std::memory_order_relaxed);
    if (drains != g_worldTableDrainsLogged) {
        // Deferred to here so the body-destruction path never does file I/O.
        g_worldTableDrainsLogged = drains;
        log::Write(log::Level::info,
                   L"World-body tables emptied by body destruction " +
                       std::to_wstring(drains) + L" time(s) so far.");
    }
    const world_ownership::Registration registration =
        g_worldOwnership.RegisterProp(self);
    if (g_worldInterpolationEnabled) {
        const world::RegistrationResult worldRegistration =
            g_worldRuntime.RegisterProp(self);
        const std::size_t worldResult =
            static_cast<std::size_t>(worldRegistration.kind);
        if (worldResult < g_worldRegistrationResults.size()) {
            g_worldRegistrationResults[worldResult].fetch_add(
                1, std::memory_order_relaxed);
        }
        g_worldRegistrationBodies.fetch_add(worldRegistration.bodies,
                                            std::memory_order_relaxed);
    }
    const std::size_t result = static_cast<std::size_t>(registration.result);
    if (result < g_ownershipRegisterResults.size()) {
        g_ownershipRegisterResults[result].fetch_add(
            1, std::memory_order_relaxed);
    }
    g_ownershipRegisteredBodies.fetch_add(registration.registeredBodies,
                                           std::memory_order_relaxed);
    if (registration.staticPhysics) {
        g_ownershipStaticProps.fetch_add(1, std::memory_order_relaxed);
    }
    if (registration.result == world_ownership::RegisterResult::registered) {
        if (registration.renderLayoutValid) {
            g_ownershipRenderValidProps.fetch_add(1,
                                                  std::memory_order_relaxed);
            g_ownershipSubMeshes.fetch_add(registration.subMeshCount,
                                           std::memory_order_relaxed);
            g_ownershipDirectBodyChildren.fetch_add(
                registration.directBodyChildren, std::memory_order_relaxed);
            g_ownershipNestedBodyChildren.fetch_add(
                registration.nestedBodyChildren, std::memory_order_relaxed);
            g_ownershipUnmatchedSubMeshes.fetch_add(
                registration.unmatchedSubMeshes, std::memory_order_relaxed);
            if (registration.propType < world_ownership::kPropTypeCount) {
                g_ownershipSubMeshesByPropType[registration.propType].fetch_add(
                    registration.subMeshCount, std::memory_order_relaxed);
                g_ownershipDirectByPropType[registration.propType].fetch_add(
                    registration.directBodyChildren,
                    std::memory_order_relaxed);
                g_ownershipNestedByPropType[registration.propType].fetch_add(
                    registration.nestedBodyChildren,
                    std::memory_order_relaxed);
                g_ownershipUnmatchedByPropType[registration.propType].fetch_add(
                    registration.unmatchedSubMeshes,
                    std::memory_order_relaxed);
            }
        } else {
            g_ownershipRenderInvalidProps.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
}

const hands_audit::Matrix* __fastcall SubMeshGetModelMatrixDetour(
    void* self,
    void*,
    void* frustum) {
    const hands_audit::Matrix* const model =
        g_originalSubMeshGetModelMatrix(self, frustum);
    if (g_handsAuditEnabled) {
        const hands_audit::ModelResult result = hands_audit::ObserveModel(
            self, model, g_lastHandsEntity, g_lastHandObjectEntity);
        const std::size_t index = static_cast<std::size_t>(result);
        if (index < kHandsModelResultCount) {
            g_handsModelAudit[index].fetch_add(1, std::memory_order_relaxed);
        }
    }
    const hands::Matrix* result =
        reinterpret_cast<const hands::Matrix*>(model);
    bool handsTarget = false;
    if (g_handsInterpolationEnabled) {
        const hands::OverrideResult overrideResult =
            g_handsRuntime.OverrideModelMatrix(
                self, reinterpret_cast<const hands::Matrix*>(model), result);
        handsTarget = overrideResult != hands::OverrideResult::notHands;
        if (overrideResult == hands::OverrideResult::applied) {
            g_handsModelsApplied.fetch_add(1, std::memory_order_relaxed);
        } else if (overrideResult == hands::OverrideResult::appliedHandObject ||
                   overrideResult ==
                       hands::OverrideResult::appliedHandObjectHistory) {
            g_handObjectModelsApplied.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_nodeAnimationInterpolationEnabled && !handsTarget) {
        const node_animation::Observation observation =
            g_nodeAnimationRuntime.OverrideModelMatrix(
                self, reinterpret_cast<const hands::Matrix*>(model), result);
        if (g_telemetryEnabled && observation.captured) {
            g_nodeAnimationCaptured.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_telemetryEnabled && observation.changed) {
            g_nodeAnimationChanged.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_telemetryEnabled && observation.result !=
                                      node_animation::OverrideResult::notTarget) {
            g_lastNodeAnimationResult.store(observation.result,
                                            std::memory_order_relaxed);
        }
        if (g_telemetryEnabled) {
            switch (observation.result) {
            case node_animation::OverrideResult::applied:
                g_nodeAnimationApplied.fetch_add(1, std::memory_order_relaxed);
                break;
            case node_animation::OverrideResult::stable:
                if (observation.captured) {
                    g_nodeAnimationStable.fetch_add(
                        1, std::memory_order_relaxed);
                }
                break;
            case node_animation::OverrideResult::clockUnavailable:
                g_nodeAnimationClockWaits.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case node_animation::OverrideResult::pairMissing:
                g_nodeAnimationPairMisses.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case node_animation::OverrideResult::invalid:
                g_nodeAnimationInvalid.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case node_animation::OverrideResult::capacityExhausted:
                g_nodeAnimationCapacity.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case node_animation::OverrideResult::notTarget:
                break;
            }
        }
    }
    if (g_enemyPoseInterpolationEnabled && !handsTarget && self != nullptr) {
        // An enemy's mesh follows its character body once per fixed update.
        // Without this its smoothed limbs would ride a stepped root.
        constexpr std::size_t kSubMeshOwnerOffset = 0x374;
        void* owner = nullptr;
        std::memcpy(&owner,
                    static_cast<const std::uint8_t*>(self) + kSubMeshOwnerOffset,
                    sizeof(owner));
        if (g_enemyPoseRuntime.TracksMesh(owner)) {
            const node_animation::Observation observation =
                g_enemyRootRuntime.OverrideOwnedModelMatrix(
                    self, owner, reinterpret_cast<const hands::Matrix*>(model),
                    result);
            if (observation.result == node_animation::OverrideResult::applied) {
                ++g_enemyRootApplied;
            } else if (observation.result ==
                           node_animation::OverrideResult::invalid ||
                       observation.result ==
                           node_animation::OverrideResult::capacityExhausted) {
                ++g_enemyRootInvalid;
            }
        }
    }
    if (g_worldInterpolationEnabled) {
        std::uint32_t propType = UINT32_MAX;
        std::uint32_t relationshipDepth = 0;
        const world::OverrideResult overrideResult =
            g_worldRuntime.OverrideModelMatrix(self, result, result,
                                               &propType,
                                               &relationshipDepth);
        static_cast<void>(relationshipDepth);
        if (overrideResult != world::OverrideResult::notTarget) {
            g_lastWorldOverride.store(overrideResult,
                                      std::memory_order_relaxed);
        }
        switch (overrideResult) {
        case world::OverrideResult::applied:
            g_worldModelsApplied.fetch_add(1, std::memory_order_relaxed);
            if (propType < g_worldAppliedByPropType.size()) {
                g_worldAppliedByPropType[propType].fetch_add(
                    1, std::memory_order_relaxed);
            }
            break;
        case world::OverrideResult::appliedStandalone:
            g_worldModelsApplied.fetch_add(1, std::memory_order_relaxed);
            g_worldStandaloneModelsApplied.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case world::OverrideResult::standalonePromoted:
            g_worldStandalonePromoted.fetch_add(1,
                                                std::memory_order_relaxed);
            break;
        case world::OverrideResult::clockUnavailable:
            g_worldClockUnavailable.fetch_add(1, std::memory_order_relaxed);
            break;
        case world::OverrideResult::historyUnavailable:
            g_worldHistoryUnavailable.fetch_add(1,
                                                std::memory_order_relaxed);
            break;
        case world::OverrideResult::pairMissing:
            g_worldPairMissing.fetch_add(1, std::memory_order_relaxed);
            break;
        case world::OverrideResult::invalid:
            g_worldOverrideInvalid.fetch_add(1, std::memory_order_relaxed);
            break;
        case world::OverrideResult::slotsExhausted:
            g_worldOutputOverflow.fetch_add(1, std::memory_order_relaxed);
            break;
        case world::OverrideResult::diagnosticNestedTarget:
            g_worldNestedTarget.fetch_add(1, std::memory_order_relaxed);
            if (propType < g_worldNestedTargetByPropType.size()) {
                g_worldNestedTargetByPropType[propType].fetch_add(
                    1, std::memory_order_relaxed);
            }
            break;
        case world::OverrideResult::diagnosticOwnedDirect:
            g_worldOwnedDirect.fetch_add(1, std::memory_order_relaxed);
            if (propType < g_worldOwnedDirectByPropType.size()) {
                g_worldOwnedDirectByPropType[propType].fetch_add(
                    1, std::memory_order_relaxed);
            }
            break;
        case world::OverrideResult::diagnosticOwnedNested:
            g_worldOwnedNested.fetch_add(1, std::memory_order_relaxed);
            if (propType < g_worldOwnedNestedByPropType.size()) {
                g_worldOwnedNestedByPropType[propType].fetch_add(
                    1, std::memory_order_relaxed);
            }
            break;
        case world::OverrideResult::diagnosticStandaloneDirect:
            g_worldStandaloneDirect.fetch_add(1, std::memory_order_relaxed);
            break;
        case world::OverrideResult::diagnosticStandaloneNested:
            g_worldStandaloneNested.fetch_add(1, std::memory_order_relaxed);
            break;
        case world::OverrideResult::notTarget:
            break;
        }
    }
    return reinterpret_cast<const hands_audit::Matrix*>(result);
}

void __fastcall MeshEntityUpdateGraphicsDetour(void* self,
                                                void*,
                                                const float frameTime) {
    g_originalMeshEntityUpdateGraphics(self, frameTime);
    if (g_ragdollPoseAuditEnabled) {
        const ragdoll_pose_audit::Observation observation =
            g_ragdollPoseAudit.Observe(self);
        if (observation.result != ragdoll_pose_audit::Result::notTarget) {
            const std::size_t index =
                static_cast<std::size_t>(observation.result);
            if (index < g_ragdollPoseResults.size()) {
                g_ragdollPoseResults[index].fetch_add(
                    1, std::memory_order_relaxed);
            }
            g_lastRagdollPoseResult.store(observation.result,
                                          std::memory_order_relaxed);
            g_ragdollBoneCount.store(
                static_cast<std::uint32_t>(observation.boneCount),
                std::memory_order_relaxed);
            if (observation.result ==
                ragdoll_pose_audit::Result::invalidMatrix) {
                g_ragdollInvalidBone.store(
                    static_cast<std::uint32_t>(
                        observation.invalidBoneIndex),
                    std::memory_order_relaxed);
            }
        }
    }
    if (g_enemyPoseAuditEnabled) {
        const enemy_pose_audit::PoseObservation enemyObservation =
            g_enemyPoseAudit.ObserveMesh(self);
        if (enemyObservation.result !=
            enemy_pose_audit::ObserveResult::notTarget) {
            const std::size_t resultIndex =
                static_cast<std::size_t>(enemyObservation.result);
            if (resultIndex < g_enemyObserveResults.size()) {
                g_enemyObserveResults[resultIndex].fetch_add(
                    1, std::memory_order_relaxed);
            }
            g_lastEnemyObserveResult.store(enemyObservation.result,
                                           std::memory_order_relaxed);
            if (enemyObservation.type < g_enemyObservedByType.size()) {
                g_enemyObservedByType[enemyObservation.type].fetch_add(
                    1, std::memory_order_relaxed);
                g_enemyBoneCountByType[enemyObservation.type].store(
                    static_cast<std::uint32_t>(enemyObservation.boneCount),
                    std::memory_order_relaxed);
                if (enemyObservation.result ==
                    enemy_pose_audit::ObserveResult::identityChanged) {
                    g_enemyIdentityByType[enemyObservation.type].fetch_add(
                        1, std::memory_order_relaxed);
                } else if (enemyObservation.result ==
                           enemy_pose_audit::ObserveResult::changed) {
                    g_enemyChangedByType[enemyObservation.type].fetch_add(
                        1, std::memory_order_relaxed);
                } else if (enemyObservation.result ==
                           enemy_pose_audit::ObserveResult::stable) {
                    g_enemyStableByType[enemyObservation.type].fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            if (enemyObservation.result ==
                enemy_pose_audit::ObserveResult::invalidMatrix) {
                g_enemyInvalidBone.store(
                    static_cast<std::uint32_t>(
                        enemyObservation.invalidBoneIndex),
                    std::memory_order_relaxed);
            }
        }
    }
    if (g_enemyPoseInterpolationEnabled) {
        const enemy_pose::UpdateObservation observation =
            g_enemyPoseRuntime.OnMeshUpdated(self);
        if (observation.result != enemy_pose::UpdateResult::notTarget &&
            observation.result != enemy_pose::UpdateResult::alreadyApplied) {
            g_lastEnemyPoseRuntimeResult.store(observation.result,
                                               std::memory_order_relaxed);
        }
        if (observation.captured) {
            g_enemyPoseRuntimeCaptured.fetch_add(1,
                                                  std::memory_order_relaxed);
        }
        switch (observation.result) {
        case enemy_pose::UpdateResult::captured:
            break;
        case enemy_pose::UpdateResult::applied:
            g_enemyPoseRuntimeApplied.fetch_add(1,
                                                 std::memory_order_relaxed);
            break;
        case enemy_pose::UpdateResult::unavailable:
            g_enemyPoseRuntimeUnavailable.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case enemy_pose::UpdateResult::identityChanged:
            g_enemyPoseRuntimeIdentity.fetch_add(1,
                                                  std::memory_order_relaxed);
            break;
        case enemy_pose::UpdateResult::clockUnavailable:
            g_enemyPoseRuntimeClockWaits.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case enemy_pose::UpdateResult::pairMissing:
            g_enemyPoseRuntimePairMisses.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case enemy_pose::UpdateResult::invalid:
            g_enemyPoseRuntimeInvalid.fetch_add(1,
                                                 std::memory_order_relaxed);
            break;
        case enemy_pose::UpdateResult::notTarget:
        case enemy_pose::UpdateResult::alreadyApplied:
            break;
        }
        // Session totals for the exit summary; the periodic counters above
        // are only reported with telemetry enabled.
        if (observation.type < g_enemySummaryApplied.size()) {
            if (observation.result == enemy_pose::UpdateResult::applied) {
                ++g_enemySummaryApplied[observation.type];
                g_enemySummaryBones[observation.type] = observation.boneCount;
            } else if (observation.result ==
                       enemy_pose::UpdateResult::invalid) {
                ++g_enemySummaryInvalid[observation.type];
            }
        }
    }
    if (g_ragdollPoseInterpolationEnabled) {
        const ragdoll_pose::UpdateObservation observation =
            g_ragdollPoseRuntime.OnMeshUpdated(self);
        if (g_telemetryEnabled || g_ragdollProfileEnabled) {
            if (observation.result != ragdoll_pose::UpdateResult::notTarget &&
                observation.result !=
                    ragdoll_pose::UpdateResult::alreadyApplied) {
                g_lastRagdollRuntimeResult.store(observation.result,
                                                 std::memory_order_relaxed);
            }
            if (observation.captured) {
                g_ragdollRuntimeCaptured.fetch_add(
                    1, std::memory_order_relaxed);
            }
            switch (observation.result) {
            case ragdoll_pose::UpdateResult::applied:
                g_ragdollRuntimeApplied.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ragdoll_pose::UpdateResult::identityChanged:
                g_ragdollRuntimeIdentity.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ragdoll_pose::UpdateResult::unavailable:
                g_ragdollRuntimeUnavailable.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ragdoll_pose::UpdateResult::clockUnavailable:
                g_ragdollRuntimeClockWaits.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ragdoll_pose::UpdateResult::pairMissing:
                g_ragdollRuntimePairMisses.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ragdoll_pose::UpdateResult::stationary:
                g_ragdollRuntimeStationary.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ragdoll_pose::UpdateResult::invalid:
                g_ragdollRuntimeInvalid.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ragdoll_pose::UpdateResult::capacityExhausted:
                g_ragdollRuntimeCapacity.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case ragdoll_pose::UpdateResult::notTarget:
            case ragdoll_pose::UpdateResult::captured:
            case ragdoll_pose::UpdateResult::alreadyApplied:
                break;
            }
        }
    }
    if (g_poseAuditEnabled) {
        const pose_audit::Observation observation =
            g_poseAudit.Observe(self, g_poseHandsEntity);
        const std::size_t index = static_cast<std::size_t>(observation.result);
        if (index < kPoseAuditResultCount) {
            g_poseAuditResults[index].fetch_add(1, std::memory_order_relaxed);
        }
        if (observation.result != pose_audit::Result::notTarget) {
            g_lastPoseAuditResult.store(observation.result,
                                        std::memory_order_relaxed);
            g_poseBoneCount.store(
                static_cast<std::uint32_t>(observation.boneCount),
                std::memory_order_relaxed);
            if (observation.result == pose_audit::Result::invalidMatrix) {
                g_poseInvalidBone.store(
                    static_cast<std::uint32_t>(observation.invalidBoneIndex),
                    std::memory_order_relaxed);
            }
        }
    }
}

void __fastcall SubMeshUpdateGraphicsDetour(void* self,
                                             void*,
                                             const float frameTime) {
    void* const owner = ragdoll_skinning::ReadOwner(self);
    // This runs for every visible submesh; only an active skeleton-physics
    // owner with a dynamic vertex buffer consults the ragdoll runtime.
    const bool candidate = g_ragdollPoseInterpolationEnabled &&
                           ragdoll_skinning::IsCandidate(self);
    // An animated enemy is skinned once per bone update, so its substituted
    // palette would otherwise reach the vertices at one arbitrary phase per
    // fixed update instead of every render.
    if (g_enemyPoseInterpolationEnabled && !candidate &&
        g_enemyPoseRuntime.TracksMesh(owner) &&
        ragdoll_skinning::ForceReskin(self)) {
        ++g_enemyReskins;
    }
    // Observe this render's authoritative palette before deciding whether
    // invalidating the submesh cache can produce different vertices.
    if (candidate && !g_ragdollPoseRuntime.WasObserved(owner)) {
        MeshEntityUpdateGraphicsDetour(owner, nullptr, frameTime);
    }
    const ragdoll_skinning::PrepareResult result =
        candidate && g_ragdollPoseRuntime.ShouldReskin(owner)
            ? ragdoll_skinning::PrepareForRender(self)
            : ragdoll_skinning::PrepareResult::notTarget;
    if (g_telemetryEnabled || g_ragdollProfileEnabled) {
        if (result != ragdoll_skinning::PrepareResult::notTarget) {
            g_lastRagdollSkinningResult.store(result,
                                              std::memory_order_relaxed);
        }
        switch (result) {
        case ragdoll_skinning::PrepareResult::forced:
            g_ragdollSkinningForced.fetch_add(1, std::memory_order_relaxed);
            break;
        case ragdoll_skinning::PrepareResult::unavailable:
            g_ragdollSkinningUnavailable.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case ragdoll_skinning::PrepareResult::invalidFlag:
            g_ragdollSkinningInvalidFlag.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case ragdoll_skinning::PrepareResult::notTarget:
            break;
        }
    }
    LARGE_INTEGER profileStart{};
    const bool profileThisCall =
        g_ragdollProfileEnabled &&
        result == ragdoll_skinning::PrepareResult::forced &&
        QueryPerformanceCounter(&profileStart) != FALSE;
    const ragdoll_skinning::ProfileInfo profileInfo = profileThisCall
        ? ragdoll_skinning::ReadProfileInfo(self)
        : ragdoll_skinning::ProfileInfo{};
    g_originalSubMeshUpdateGraphics(self, frameTime);
    if (profileThisCall) {
        LARGE_INTEGER profileEnd{};
        if (QueryPerformanceCounter(&profileEnd) &&
            profileEnd.QuadPart >= profileStart.QuadPart) {
            const std::uint64_t ticks = static_cast<std::uint64_t>(
                profileEnd.QuadPart - profileStart.QuadPart);
            g_ragdollProfileCalls.fetch_add(1, std::memory_order_relaxed);
            g_ragdollProfileQpcTicks.fetch_add(ticks,
                                               std::memory_order_relaxed);
            g_ragdollProfileVertices.fetch_add(
                profileInfo.vertexCount, std::memory_order_relaxed);
            g_ragdollProfileBones.fetch_add(
                profileInfo.boneCount, std::memory_order_relaxed);
            std::uint64_t maximum = g_ragdollProfileMaxQpcTicks.load(
                std::memory_order_relaxed);
            while (ticks > maximum &&
                   !g_ragdollProfileMaxQpcTicks.compare_exchange_weak(
                       maximum, ticks, std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
            if (ticks > maximum) {
                g_ragdollProfileMaxVertices.store(
                    profileInfo.vertexCount, std::memory_order_relaxed);
                g_ragdollProfileMaxBones.store(
                    profileInfo.boneCount, std::memory_order_relaxed);
                g_ragdollProfileMaxSubMesh.store(
                    reinterpret_cast<std::uintptr_t>(self),
                    std::memory_order_relaxed);
            }
        }
    }
}

void* __fastcall ImageTrailRenderEffectDetour(void* self,
                                               void*,
                                               void* inputTexture,
                                               void* finalTempBuffer) {
    if (self == nullptr || g_imageTrailDetourActive) {
        return g_originalImageTrailRenderEffect(
            self, inputTexture, finalTempBuffer);
    }

    constexpr std::size_t kCompositeOffset = 0x18;
    constexpr std::size_t kAmountOffset = 0x4C;
    constexpr std::size_t kClearFrameBufferOffset = 0x50;
    constexpr std::size_t kCompositeFrameTimeOffset = 0x11C;
    auto* const effect = static_cast<std::uint8_t*>(self);
    if (*(effect + kClearFrameBufferOffset) != 0) {
        return g_originalImageTrailRenderEffect(
            self, inputTexture, finalTempBuffer);
    }

    auto* const composite = *reinterpret_cast<std::uint8_t* const*>(
        effect + kCompositeOffset);
    if (composite == nullptr) {
        bool expected = false;
        if (g_imageTrailInvalidLogged.compare_exchange_strong(
                expected, true, std::memory_order_relaxed)) {
            log::Write(log::Level::warning,
                       L"Image-trail timing skipped an invalid composite.");
        }
        return g_originalImageTrailRenderEffect(
            self, inputTexture, finalTempBuffer);
    }

    auto* const amountAddress = reinterpret_cast<float*>(
        effect + kAmountOffset);
    const float originalAmount = *amountAddress;
    const float frameTime = *reinterpret_cast<const float*>(
        composite + kCompositeFrameTimeOffset);
    float correctedAmount = 0.0F;
    if (!image_trail::CorrectAmount(
            originalAmount, frameTime, correctedAmount)) {
        bool expected = false;
        if (g_imageTrailInvalidLogged.compare_exchange_strong(
                expected, true, std::memory_order_relaxed)) {
            log::Write(log::Level::warning,
                       L"Image-trail timing skipped invalid amount/frame-time "
                       L"input.");
        }
        return g_originalImageTrailRenderEffect(
            self, inputTexture, finalTempBuffer);
    }
    if (std::memcmp(&correctedAmount, &originalAmount, sizeof(float)) == 0) {
        return g_originalImageTrailRenderEffect(
            self, inputTexture, finalTempBuffer);
    }

    g_imageTrailDetourActive = true;
    *amountAddress = correctedAmount;
    void* const result = g_originalImageTrailRenderEffect(
        self, inputTexture, finalTempBuffer);
    *amountAddress = originalAmount;
    g_imageTrailDetourActive = false;

    if (std::memcmp(amountAddress, &originalAmount, sizeof(float)) != 0) {
        bool expected = false;
        if (g_imageTrailRestoreFailureLogged.compare_exchange_strong(
                expected, true, std::memory_order_relaxed)) {
            log::Write(log::Level::error,
                       L"Image-trail timing could not restore the authoritative "
                       L"effect amount.");
        }
    }
    bool expected = false;
    if (g_imageTrailAdjustedLogged.compare_exchange_strong(
            expected, true, std::memory_order_relaxed)) {
        log::Write(log::Level::info,
                   L"Image-trail timing correction reached the live render "
                   L"path (amount=" + std::to_wstring(originalAmount) +
                   L", frameTime=" + std::to_wstring(frameTime) +
                   L", corrected=" + std::to_wstring(correctedAmount) + L").");
    }
    return result;
}

void __fastcall SceneRenderDetour(void* self,
                                  void*,
                                  const float frameTime,
                                  const std::uint32_t flags) {
    ObserveThread(g_sceneThread, g_sceneThreadMismatches);
    g_scene = self;
    g_sceneRenders.fetch_add(1, std::memory_order_relaxed);
    if (g_telemetryEnabled) {
        RecordCameraAudit(camera_audit::Observe(self), true);
    }
    void* listenerWorld = nullptr;
    if (ReadListenerWorld(listenerWorld)) {
        g_worldMapEpoch.OnPresented(listenerWorld);
    }

    clock::Sample presentationSample{};
    if (g_clockDiagnosticsEnabled) {
        const double presentationSeconds = QpcSeconds();
        double presentationDelayTicks = 0.0;
        bool intervalValid = false;
        if (g_previousPresentationValid) {
            const double interval =
                presentationSeconds - g_previousPresentationSeconds;
            if (std::isfinite(interval) && interval > 0.0 &&
                interval <= 0.250 && g_nativeStepSeconds > 0.0) {
                presentationDelayTicks =
                    std::clamp(interval / g_nativeStepSeconds, 0.0, 1.0) *
                    kPresentationHeadroomFraction;
                intervalValid = true;
            }
        }
        g_previousPresentationSeconds = presentationSeconds;
        g_previousPresentationValid = true;

        double alpha = 0.0;
        double rawAlpha = 0.0;
        const bool phaseValid =
            intervalValid && ReadLogicTimerAlpha(
                                 presentationSeconds, presentationDelayTicks,
                                 alpha, rawAlpha);
        if (!phaseValid) {
            g_clockPhaseInvalid.fetch_add(1, std::memory_order_relaxed);
        }
        presentationSample = g_clockReady
            ? g_presentationClock.Evaluate(presentationSeconds, alpha, phaseValid)
            : clock::Sample{};
        switch (presentationSample.kind) {
        case clock::SampleKind::interpolated:
            g_clockInterpolated.fetch_add(1, std::memory_order_relaxed);
            break;
        case clock::SampleKind::unavailable:
            g_clockUnavailable.fetch_add(1, std::memory_order_relaxed);
            break;
        case clock::SampleKind::reset:
            RecordClockReset(presentationSample.resetReason);
            break;
        }
        g_latestAlphaMillionths.store(
            static_cast<std::int64_t>(std::llround(
                presentationSample.alpha * 1'000'000.0)),
            std::memory_order_relaxed);
        g_latestRawAlphaMillionths.store(
            static_cast<std::int64_t>(std::llround(rawAlpha * 1'000'000.0)),
            std::memory_order_relaxed);
    }

    if (g_mouseLookEnabled) {
        mouse_look::AcquirePresentation();
    }
    bool cameraApplied = false;
    if (g_cameraInterpolationEnabled) {
        const camera::FrameResult result = g_cameraRuntime.Apply(
            self, g_tickSerial.load(std::memory_order_relaxed),
            presentationSample);
        RecordCameraFrame(result);
        cameraApplied = result == camera::FrameResult::applied;
    }
    hands::Matrix mouseHandsCorrection = hands::Identity();
    const bool mouseHandsCorrectionReady =
        g_mouseLookEnabled && cameraApplied &&
        mouse_look::ApplyPreview(
            presentationSample, g_tickSerial.load(std::memory_order_relaxed),
            mouseHandsCorrection);
    if (g_handsInterpolationEnabled) {
        // The hands follow the previewed view; without a preview this clears
        // any correction left from an earlier frame.
        if (g_mouseLookEnabled &&
            !g_handsRuntime.SetPresentationCorrection(
                mouseHandsCorrectionReady ? &mouseHandsCorrection : nullptr)) {
            (void)g_handsRuntime.SetPresentationCorrection(nullptr);
        }
        const hands::FrameResult result =
            g_handsRuntime.BeginRender(presentationSample);
        if (result == hands::FrameResult::renderReady) {
            g_handsRenderReady.fetch_add(1, std::memory_order_relaxed);
            const hands::AttachmentBatch& attachments =
                g_handsRuntime.LastAttachmentBatch();
            g_handAttachmentCandidates.fetch_add(
                attachments.candidates, std::memory_order_relaxed);
            g_handAttachmentsApplied.fetch_add(
                attachments.applied, std::memory_order_relaxed);
        } else {
            g_handsFrameSkipped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_handsPoseInterpolationEnabled) {
        const pose::UpdateResult result = g_poseRuntime.BeginRender(
            presentationSample, g_tickSerial.load(std::memory_order_relaxed),
            frameTime);
        switch (result) {
        case pose::UpdateResult::applied:
            g_poseApplied.fetch_add(1, std::memory_order_relaxed);
            break;
        case pose::UpdateResult::identityChanged:
            g_poseIdentityChanges.fetch_add(1, std::memory_order_relaxed);
            break;
        case pose::UpdateResult::clockUnavailable:
            g_poseClockWaits.fetch_add(1, std::memory_order_relaxed);
            break;
        case pose::UpdateResult::pairMissing:
            g_posePairMisses.fetch_add(1, std::memory_order_relaxed);
            break;
        case pose::UpdateResult::invalid:
            g_poseInvalid.fetch_add(1, std::memory_order_relaxed);
            break;
        case pose::UpdateResult::unavailable:
            g_poseUnavailable.fetch_add(1, std::memory_order_relaxed);
            break;
        case pose::UpdateResult::notTarget:
        case pose::UpdateResult::captured:
        case pose::UpdateResult::alreadyApplied:
            break;
        }
    }
    if (g_enemyPoseInterpolationEnabled) {
        g_enemyPoseRuntime.BeginRender(
            presentationSample,
            g_tickSerial.load(std::memory_order_relaxed));
        g_enemyRootRuntime.BeginRender(
            presentationSample,
            g_tickSerial.load(std::memory_order_relaxed));
    }
    if (g_ragdollPoseInterpolationEnabled) {
        g_ragdollPoseRuntime.BeginRender(
            presentationSample,
            g_tickSerial.load(std::memory_order_relaxed));
    }
    if (g_nodeAnimationInterpolationEnabled) {
        g_nodeAnimationRuntime.BeginRender(
            presentationSample,
            g_tickSerial.load(std::memory_order_relaxed));
    }
    if (g_worldInterpolationEnabled) {
        g_worldRuntime.BeginRender(presentationSample);
    }
    if (g_particleInterpolationEnabled) {
        g_particleRuntime.BeginRender(
            presentationSample, g_tickSerial.load(std::memory_order_relaxed));
    }
    g_originalSceneRender(self, frameTime, flags);
    if (g_particleInterpolationEnabled) {
        g_particleRuntime.EndRender();
    }
    if (g_enemyPoseInterpolationEnabled) {
        g_enemyPoseRuntimeRestoreFailures.fetch_add(
            g_enemyPoseRuntime.EndRender(), std::memory_order_relaxed);
    }
    if (g_ragdollPoseInterpolationEnabled) {
        g_ragdollRuntimeRestoreFailures.fetch_add(
            g_ragdollPoseRuntime.EndRender(), std::memory_order_relaxed);
    }
    if (g_worldInterpolationEnabled) {
        g_worldRuntime.EndRender();
    }
    if (g_handsPoseInterpolationEnabled && !g_poseRuntime.EndRender()) {
        g_poseRestoreFailures.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_handsInterpolationEnabled) {
        g_handAttachmentRestoreFailures.fetch_add(
            g_handsRuntime.EndRender(), std::memory_order_relaxed);
    }
    if (cameraApplied && !g_cameraRuntime.Restore()) {
        g_cameraRestoreFailures.fetch_add(1, std::memory_order_relaxed);
        log::Write(log::Level::error,
                   L"Camera interpolation could not restore the authoritative "
                   L"camera state after cScene::Render.");
    }
}

std::wstring Decimal(const double value) {
    wchar_t buffer[32]{};
    _snwprintf_s(buffer, std::size(buffer), _TRUNCATE, L"%.3f", value);
    return buffer;
}

std::wstring Rate(const std::uint64_t count, const double seconds) {
    if (seconds <= 0.0) {
        return L"0.000";
    }
    return Decimal(static_cast<double>(count) / seconds);
}

DWORD WINAPI ReporterThread(void*) {
    LARGE_INTEGER previous{};
    QueryPerformanceCounter(&previous);
    std::array<std::uint64_t, 7> previousMessages{};
    std::uint64_t previousSceneRenders = 0;
    std::uint64_t previousClockInterpolated = 0;
    std::uint64_t previousClockUnavailable = 0;
    std::uint64_t previousClockResets = 0;
    std::uint64_t previousClockPhaseInvalid = 0;
    std::array<std::uint64_t, kCameraAuditResultCount> previousCameraTick{};
    std::array<std::uint64_t, kCameraAuditResultCount> previousCameraRender{};
    std::uint64_t previousCameraIdentityChanges = 0;
    std::uint64_t previousCameraCaptured = 0;
    std::uint64_t previousCameraApplied = 0;
    std::uint64_t previousCameraSkipped = 0;
    std::uint64_t previousCameraRestoreFailures = 0;
    std::array<std::uint64_t, kHandsAuditResultCount> previousHandsAudit{};
    std::uint64_t previousHandsIdentityChanges = 0;
    std::uint64_t previousHandsMoved = 0;
    std::uint64_t previousHandsStable = 0;
    std::array<std::uint64_t, kHandsModelResultCount>
        previousHandsModelAudit{};
    std::uint64_t previousHandsCaptured = 0;
    std::uint64_t previousHandsRenderReady = 0;
    std::uint64_t previousHandsFrameSkipped = 0;
    std::uint64_t previousHandsModelsApplied = 0;
    std::uint64_t previousHandObjectModelsApplied = 0;
    std::uint64_t previousHandAttachmentCandidates = 0;
    std::uint64_t previousHandAttachmentsApplied = 0;
    std::uint64_t previousHandAttachmentRestoreFailures = 0;
    std::array<std::uint64_t, kPoseAuditResultCount> previousPoseAudit{};
    std::uint64_t previousPoseStaged = 0;
    std::uint64_t previousPoseApplied = 0;
    std::uint64_t previousPoseUnavailable = 0;
    std::uint64_t previousPoseIdentityChanges = 0;
    std::uint64_t previousPoseClockWaits = 0;
    std::uint64_t previousPosePairMisses = 0;
    std::uint64_t previousPoseInvalid = 0;
    std::uint64_t previousPoseRestoreFailures = 0;
    std::array<std::uint64_t, kEnemyStageResultCount>
        previousEnemyStageResults{};
    std::array<std::uint64_t, kEnemyObserveResultCount>
        previousEnemyObserveResults{};
    std::array<std::uint64_t, enemy_pose_audit::kEnemyTypeCount>
        previousEnemyStagedByType{};
    std::array<std::uint64_t, enemy_pose_audit::kEnemyTypeCount>
        previousEnemyObservedByType{};
    std::array<std::uint64_t, enemy_pose_audit::kEnemyTypeCount>
        previousEnemyChangedByType{};
    std::array<std::uint64_t, enemy_pose_audit::kEnemyTypeCount>
        previousEnemyStableByType{};
    std::array<std::uint64_t, enemy_pose_audit::kEnemyTypeCount>
        previousEnemyIdentityByType{};
    std::uint64_t previousEnemyPoseRuntimeStaged = 0;
    std::uint64_t previousEnemyPoseRuntimeUnsupported = 0;
    std::uint64_t previousEnemyPoseRuntimeInactive = 0;
    std::uint64_t previousEnemyPoseRuntimeUnavailable = 0;
    std::uint64_t previousEnemyPoseRuntimeCapacity = 0;
    std::uint64_t previousEnemyPoseRuntimeCaptured = 0;
    std::uint64_t previousEnemyPoseRuntimeApplied = 0;
    std::uint64_t previousEnemyPoseRuntimeIdentity = 0;
    std::uint64_t previousEnemyPoseRuntimeClockWaits = 0;
    std::uint64_t previousEnemyPoseRuntimePairMisses = 0;
    std::uint64_t previousEnemyPoseRuntimeInvalid = 0;
    std::uint64_t previousEnemyPoseRuntimeRestoreFailures = 0;
    std::array<std::uint64_t, kRagdollPoseResultCount>
        previousRagdollPoseResults{};
    std::uint64_t previousRagdollRuntimeCaptured = 0;
    std::uint64_t previousRagdollRuntimeApplied = 0;
    std::uint64_t previousRagdollRuntimeIdentity = 0;
    std::uint64_t previousRagdollRuntimeUnavailable = 0;
    std::uint64_t previousRagdollRuntimeClockWaits = 0;
    std::uint64_t previousRagdollRuntimePairMisses = 0;
    std::uint64_t previousRagdollRuntimeInvalid = 0;
    std::uint64_t previousRagdollRuntimeCapacity = 0;
    std::uint64_t previousRagdollRuntimeRestoreFailures = 0;
    std::uint64_t previousRagdollSkinningForced = 0;
    std::uint64_t previousRagdollSkinningUnavailable = 0;
    std::uint64_t previousRagdollSkinningInvalidFlag = 0;
    std::uint64_t previousNewtonTransformCallbacks = 0;
    std::uint64_t previousNewtonTransformThreadZero = 0;
    std::uint64_t previousNewtonTransformWorkerThread = 0;
    std::uint64_t previousNewtonTransformInvalidThread = 0;
    std::uint64_t previousNodeAnimationCaptured = 0;
    std::uint64_t previousNodeAnimationChanged = 0;
    std::uint64_t previousNodeAnimationApplied = 0;
    std::uint64_t previousNodeAnimationStable = 0;
    std::uint64_t previousNodeAnimationClockWaits = 0;
    std::uint64_t previousNodeAnimationPairMisses = 0;
    std::uint64_t previousNodeAnimationInvalid = 0;
    std::uint64_t previousNodeAnimationCapacity = 0;
    std::array<std::uint64_t, kOwnershipRegisterResultCount>
        previousOwnershipRegisterResults{};
    std::uint64_t previousOwnershipRegisteredBodies = 0;
    std::uint64_t previousOwnershipStaticProps = 0;
    std::uint64_t previousOwnershipRenderValidProps = 0;
    std::uint64_t previousOwnershipRenderInvalidProps = 0;
    std::uint64_t previousOwnershipSubMeshes = 0;
    std::uint64_t previousOwnershipDirectBodyChildren = 0;
    std::uint64_t previousOwnershipNestedBodyChildren = 0;
    std::uint64_t previousOwnershipUnmatchedSubMeshes = 0;
    std::array<std::uint64_t, world_ownership::kPropTypeCount>
        previousOwnershipSubMeshesByPropType{};
    std::array<std::uint64_t, world_ownership::kPropTypeCount>
        previousOwnershipDirectByPropType{};
    std::array<std::uint64_t, world_ownership::kPropTypeCount>
        previousOwnershipNestedByPropType{};
    std::array<std::uint64_t, world_ownership::kPropTypeCount>
        previousOwnershipUnmatchedByPropType{};
    std::uint64_t previousNewtonOwnedCallbacks = 0;
    std::uint64_t previousNewtonUnownedCallbacks = 0;
    std::array<std::uint64_t, world_ownership::kPropTypeCount>
        previousNewtonCallbacksByPropType{};
    std::array<std::uint64_t, kWorldRegistrationKindCount>
        previousWorldRegistrationResults{};
    std::uint64_t previousWorldRegistrationBodies = 0;
    std::uint64_t previousWorldCallbacksStaged = 0;
    std::uint64_t previousWorldBodiesCaptured = 0;
    std::uint64_t previousWorldDuplicateCallbacks = 0;
    std::uint64_t previousWorldCaptureInvalid = 0;
    std::uint64_t previousWorldMoveOverflow = 0;
    std::uint64_t previousWorldModelsApplied = 0;
    std::uint64_t previousWorldStandaloneModelsApplied = 0;
    std::uint64_t previousWorldClockUnavailable = 0;
    std::uint64_t previousWorldHistoryUnavailable = 0;
    std::uint64_t previousWorldPairMissing = 0;
    std::uint64_t previousWorldOverrideInvalid = 0;
    std::uint64_t previousWorldOutputOverflow = 0;
    std::uint64_t previousWorldRelationshipCandidates = 0;
    std::uint64_t previousWorldRelationshipOverflow = 0;
    std::uint64_t previousWorldNestedTarget = 0;
    std::uint64_t previousWorldOwnedDirect = 0;
    std::uint64_t previousWorldOwnedNested = 0;
    std::uint64_t previousWorldStandaloneDirect = 0;
    std::uint64_t previousWorldStandaloneNested = 0;
    std::uint64_t previousWorldStandalonePromoted = 0;
    std::array<std::uint64_t, world_ownership::kPropTypeCount>
        previousWorldAppliedByPropType{};
    std::array<std::uint64_t, world_ownership::kPropTypeCount>
        previousWorldNestedTargetByPropType{};
    std::array<std::uint64_t, world_ownership::kPropTypeCount>
        previousWorldOwnedDirectByPropType{};
    std::array<std::uint64_t, world_ownership::kPropTypeCount>
        previousWorldOwnedNestedByPropType{};

    const DWORD waitMilliseconds = g_reportIntervalSeconds * 1000U;
    for (;;) {
        Sleep(waitMilliseconds);

        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now)) {
            continue;
        }
        const double seconds =
            static_cast<double>(now.QuadPart - previous.QuadPart) /
            static_cast<double>(g_qpcFrequency.QuadPart);

        std::array<std::uint64_t, 7> messageDeltas{};
        for (std::size_t index = 0; index < g_messages.size(); ++index) {
            const std::uint64_t value =
                g_messages[index].load(std::memory_order_relaxed);
            messageDeltas[index] = value - previousMessages[index];
            previousMessages[index] = value;
        }
        const std::uint64_t sceneRenders =
            g_sceneRenders.load(std::memory_order_relaxed);
        const std::uint64_t sceneDelta = sceneRenders - previousSceneRenders;
        previousSceneRenders = sceneRenders;
        const cadence::Snapshot cadence = g_swapCadence.TakeSnapshot();
        const std::uint64_t clockInterpolated =
            g_clockInterpolated.load(std::memory_order_relaxed);
        const std::uint64_t clockUnavailable =
            g_clockUnavailable.load(std::memory_order_relaxed);
        const std::uint64_t clockResets =
            g_clockResets.load(std::memory_order_relaxed);
        const std::uint64_t clockPhaseInvalid =
            g_clockPhaseInvalid.load(std::memory_order_relaxed);
        std::array<std::uint64_t, kCameraAuditResultCount> cameraTick{};
        std::array<std::uint64_t, kCameraAuditResultCount> cameraRender{};
        for (std::size_t index = 0; index < kCameraAuditResultCount; ++index) {
            cameraTick[index] =
                g_cameraTickAudit[index].load(std::memory_order_relaxed);
            cameraRender[index] =
                g_cameraRenderAudit[index].load(std::memory_order_relaxed);
        }
        const std::uint64_t cameraIdentityChanges =
            g_cameraIdentityChanges.load(std::memory_order_relaxed);
        const std::uint64_t cameraCaptured =
            g_cameraCaptured.load(std::memory_order_relaxed);
        const std::uint64_t cameraApplied =
            g_cameraApplied.load(std::memory_order_relaxed);
        const std::uint64_t cameraSkipped =
            g_cameraSkipped.load(std::memory_order_relaxed);
        const std::uint64_t cameraRestoreFailures =
            g_cameraRestoreFailures.load(std::memory_order_relaxed);
        std::array<std::uint64_t, kHandsAuditResultCount> handsAudit{};
        for (std::size_t index = 0; index < kHandsAuditResultCount; ++index) {
            handsAudit[index] =
                g_handsAudit[index].load(std::memory_order_relaxed);
        }
        const std::uint64_t handsIdentityChanges =
            g_handsIdentityChanges.load(std::memory_order_relaxed);
        const std::uint64_t handsMoved =
            g_handsMoved.load(std::memory_order_relaxed);
        const std::uint64_t handsStable =
            g_handsStable.load(std::memory_order_relaxed);
        std::array<std::uint64_t, kHandsModelResultCount> handsModelAudit{};
        for (std::size_t index = 0; index < kHandsModelResultCount; ++index) {
            handsModelAudit[index] =
                g_handsModelAudit[index].load(std::memory_order_relaxed);
        }
        const std::uint64_t handsCaptured =
            g_handsCaptured.load(std::memory_order_relaxed);
        const std::uint64_t handsRenderReady =
            g_handsRenderReady.load(std::memory_order_relaxed);
        const std::uint64_t handsFrameSkipped =
            g_handsFrameSkipped.load(std::memory_order_relaxed);
        const std::uint64_t handsModelsApplied =
            g_handsModelsApplied.load(std::memory_order_relaxed);
        const std::uint64_t handObjectModelsApplied =
            g_handObjectModelsApplied.load(std::memory_order_relaxed);
        const std::uint64_t handAttachmentCandidates =
            g_handAttachmentCandidates.load(std::memory_order_relaxed);
        const std::uint64_t handAttachmentsApplied =
            g_handAttachmentsApplied.load(std::memory_order_relaxed);
        const std::uint64_t handAttachmentRestoreFailures =
            g_handAttachmentRestoreFailures.load(std::memory_order_relaxed);
        std::array<std::uint64_t, kPoseAuditResultCount> poseAudit{};
        for (std::size_t index = 0; index < kPoseAuditResultCount; ++index) {
            poseAudit[index] =
                g_poseAuditResults[index].load(std::memory_order_relaxed);
        }
        const std::uint64_t poseStaged =
            g_poseStaged.load(std::memory_order_relaxed);
        const std::uint64_t poseApplied =
            g_poseApplied.load(std::memory_order_relaxed);
        const std::uint64_t poseUnavailable =
            g_poseUnavailable.load(std::memory_order_relaxed);
        const std::uint64_t poseIdentityChanges =
            g_poseIdentityChanges.load(std::memory_order_relaxed);
        const std::uint64_t poseClockWaits =
            g_poseClockWaits.load(std::memory_order_relaxed);
        const std::uint64_t posePairMisses =
            g_posePairMisses.load(std::memory_order_relaxed);
        const std::uint64_t poseInvalid =
            g_poseInvalid.load(std::memory_order_relaxed);
        const std::uint64_t poseRestoreFailures =
            g_poseRestoreFailures.load(std::memory_order_relaxed);
        std::array<std::uint64_t, kEnemyStageResultCount>
            enemyStageResults{};
        std::array<std::uint64_t, kEnemyObserveResultCount>
            enemyObserveResults{};
        for (std::size_t index = 0; index < enemyStageResults.size(); ++index) {
            enemyStageResults[index] =
                g_enemyStageResults[index].load(std::memory_order_relaxed);
        }
        for (std::size_t index = 0; index < enemyObserveResults.size();
             ++index) {
            enemyObserveResults[index] =
                g_enemyObserveResults[index].load(std::memory_order_relaxed);
        }
        std::array<std::uint64_t, enemy_pose_audit::kEnemyTypeCount>
            enemyStagedByType{};
        std::array<std::uint64_t, enemy_pose_audit::kEnemyTypeCount>
            enemyObservedByType{};
        std::array<std::uint64_t, enemy_pose_audit::kEnemyTypeCount>
            enemyChangedByType{};
        std::array<std::uint64_t, enemy_pose_audit::kEnemyTypeCount>
            enemyStableByType{};
        std::array<std::uint64_t, enemy_pose_audit::kEnemyTypeCount>
            enemyIdentityByType{};
        std::wstring activeEnemyTypes;
        for (std::size_t index = 0; index < enemyStagedByType.size(); ++index) {
            enemyStagedByType[index] =
                g_enemyStagedByType[index].load(std::memory_order_relaxed);
            enemyObservedByType[index] =
                g_enemyObservedByType[index].load(std::memory_order_relaxed);
            enemyChangedByType[index] =
                g_enemyChangedByType[index].load(std::memory_order_relaxed);
            enemyStableByType[index] =
                g_enemyStableByType[index].load(std::memory_order_relaxed);
            enemyIdentityByType[index] =
                g_enemyIdentityByType[index].load(std::memory_order_relaxed);
            const std::uint64_t staged =
                enemyStagedByType[index] - previousEnemyStagedByType[index];
            const std::uint64_t observed =
                enemyObservedByType[index] - previousEnemyObservedByType[index];
            if (staged == 0 && observed == 0) continue;
            if (!activeEnemyTypes.empty()) activeEnemyTypes += L",";
            activeEnemyTypes += enemy_pose_audit::EnemyTypeName(
                static_cast<std::uint32_t>(index));
            activeEnemyTypes += L":" + std::to_wstring(staged) + L"/" +
                std::to_wstring(observed) + L"/" +
                std::to_wstring(enemyChangedByType[index] -
                    previousEnemyChangedByType[index]) + L"/" +
                std::to_wstring(enemyStableByType[index] -
                    previousEnemyStableByType[index]) + L"/" +
                std::to_wstring(enemyIdentityByType[index] -
                    previousEnemyIdentityByType[index]) + L"/" +
                std::to_wstring(g_enemyBoneCountByType[index].load(
                    std::memory_order_relaxed));
        }
        if (activeEnemyTypes.empty()) activeEnemyTypes = L"none";
        std::array<std::uint64_t, kRagdollPoseResultCount>
            ragdollPoseResults{};
        for (std::size_t index = 0; index < ragdollPoseResults.size();
             ++index) {
            ragdollPoseResults[index] =
                g_ragdollPoseResults[index].load(std::memory_order_relaxed);
        }
        const std::uint64_t ragdollRuntimeCaptured =
            g_ragdollRuntimeCaptured.load(std::memory_order_relaxed);
        const std::uint64_t ragdollRuntimeApplied =
            g_ragdollRuntimeApplied.load(std::memory_order_relaxed);
        const std::uint64_t ragdollRuntimeIdentity =
            g_ragdollRuntimeIdentity.load(std::memory_order_relaxed);
        const std::uint64_t ragdollRuntimeUnavailable =
            g_ragdollRuntimeUnavailable.load(std::memory_order_relaxed);
        const std::uint64_t ragdollRuntimeClockWaits =
            g_ragdollRuntimeClockWaits.load(std::memory_order_relaxed);
        const std::uint64_t ragdollRuntimePairMisses =
            g_ragdollRuntimePairMisses.load(std::memory_order_relaxed);
        const std::uint64_t ragdollRuntimeInvalid =
            g_ragdollRuntimeInvalid.load(std::memory_order_relaxed);
        const std::uint64_t ragdollRuntimeCapacity =
            g_ragdollRuntimeCapacity.load(std::memory_order_relaxed);
        const std::uint64_t ragdollRuntimeRestoreFailures =
            g_ragdollRuntimeRestoreFailures.load(std::memory_order_relaxed);
        const std::uint64_t ragdollSkinningForced =
            g_ragdollSkinningForced.load(std::memory_order_relaxed);
        const std::uint64_t ragdollSkinningUnavailable =
            g_ragdollSkinningUnavailable.load(std::memory_order_relaxed);
        const std::uint64_t ragdollSkinningInvalidFlag =
            g_ragdollSkinningInvalidFlag.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeStaged =
            g_enemyPoseRuntimeStaged.load(std::memory_order_relaxed);
        const std::uint64_t nodeAnimationCaptured =
            g_nodeAnimationCaptured.load(std::memory_order_relaxed);
        const std::uint64_t nodeAnimationChanged =
            g_nodeAnimationChanged.load(std::memory_order_relaxed);
        const std::uint64_t nodeAnimationApplied =
            g_nodeAnimationApplied.load(std::memory_order_relaxed);
        const std::uint64_t nodeAnimationStable =
            g_nodeAnimationStable.load(std::memory_order_relaxed);
        const std::uint64_t nodeAnimationClockWaits =
            g_nodeAnimationClockWaits.load(std::memory_order_relaxed);
        const std::uint64_t nodeAnimationPairMisses =
            g_nodeAnimationPairMisses.load(std::memory_order_relaxed);
        const std::uint64_t nodeAnimationInvalid =
            g_nodeAnimationInvalid.load(std::memory_order_relaxed);
        const std::uint64_t nodeAnimationCapacity =
            g_nodeAnimationCapacity.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeUnsupported =
            g_enemyPoseRuntimeUnsupported.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeInactive =
            g_enemyPoseRuntimeInactive.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeUnavailable =
            g_enemyPoseRuntimeUnavailable.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeCapacity =
            g_enemyPoseRuntimeCapacity.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeCaptured =
            g_enemyPoseRuntimeCaptured.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeApplied =
            g_enemyPoseRuntimeApplied.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeIdentity =
            g_enemyPoseRuntimeIdentity.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeClockWaits =
            g_enemyPoseRuntimeClockWaits.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimePairMisses =
            g_enemyPoseRuntimePairMisses.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeInvalid =
            g_enemyPoseRuntimeInvalid.load(std::memory_order_relaxed);
        const std::uint64_t enemyPoseRuntimeRestoreFailures =
            g_enemyPoseRuntimeRestoreFailures.load(std::memory_order_relaxed);
        const std::uint64_t newtonTransformCallbacks =
            g_newtonTransformCallbacks.load(std::memory_order_relaxed);
        const std::uint64_t newtonTransformThreadZero =
            g_newtonTransformThreadZero.load(std::memory_order_relaxed);
        const std::uint64_t newtonTransformWorkerThread =
            g_newtonTransformWorkerThread.load(std::memory_order_relaxed);
        const std::uint64_t newtonTransformInvalidThread =
            g_newtonTransformInvalidThread.load(std::memory_order_relaxed);
        std::array<std::uint64_t, kWorldRegistrationKindCount>
            worldRegistrationResults{};
        for (std::size_t index = 0;
             index < worldRegistrationResults.size(); ++index) {
            worldRegistrationResults[index] =
                g_worldRegistrationResults[index].load(
                    std::memory_order_relaxed);
        }
        const std::uint64_t worldRegistrationBodies =
            g_worldRegistrationBodies.load(std::memory_order_relaxed);
        const std::uint64_t worldCallbacksStaged =
            g_worldCallbacksStaged.load(std::memory_order_relaxed);
        const std::uint64_t worldBodiesCaptured =
            g_worldBodiesCaptured.load(std::memory_order_relaxed);
        const std::uint64_t worldDuplicateCallbacks =
            g_worldDuplicateCallbacks.load(std::memory_order_relaxed);
        const std::uint64_t worldCaptureInvalid =
            g_worldCaptureInvalid.load(std::memory_order_relaxed);
        const std::uint64_t worldMoveOverflow =
            g_worldMoveOverflow.load(std::memory_order_relaxed);
        const std::uint64_t worldModelsApplied =
            g_worldModelsApplied.load(std::memory_order_relaxed);
        const std::uint64_t worldStandaloneModelsApplied =
            g_worldStandaloneModelsApplied.load(std::memory_order_relaxed);
        const std::uint64_t worldClockUnavailable =
            g_worldClockUnavailable.load(std::memory_order_relaxed);
        const std::uint64_t worldHistoryUnavailable =
            g_worldHistoryUnavailable.load(std::memory_order_relaxed);
        const std::uint64_t worldPairMissing =
            g_worldPairMissing.load(std::memory_order_relaxed);
        const std::uint64_t worldOverrideInvalid =
            g_worldOverrideInvalid.load(std::memory_order_relaxed);
        const std::uint64_t worldOutputOverflow =
            g_worldOutputOverflow.load(std::memory_order_relaxed);
        const std::uint64_t worldRelationshipCandidates =
            g_worldRelationshipCandidates.load(std::memory_order_relaxed);
        const std::uint64_t worldRelationshipOverflow =
            g_worldRelationshipOverflow.load(std::memory_order_relaxed);
        const std::uint64_t worldNestedTarget =
            g_worldNestedTarget.load(std::memory_order_relaxed);
        const std::uint64_t worldOwnedDirect =
            g_worldOwnedDirect.load(std::memory_order_relaxed);
        const std::uint64_t worldOwnedNested =
            g_worldOwnedNested.load(std::memory_order_relaxed);
        const std::uint64_t worldStandaloneDirect =
            g_worldStandaloneDirect.load(std::memory_order_relaxed);
        const std::uint64_t worldStandaloneNested =
            g_worldStandaloneNested.load(std::memory_order_relaxed);
        const std::uint64_t worldStandalonePromoted =
            g_worldStandalonePromoted.load(std::memory_order_relaxed);
        std::array<std::uint64_t, kOwnershipRegisterResultCount>
            ownershipRegisterResults{};
        for (std::size_t index = 0;
             index < ownershipRegisterResults.size(); ++index) {
            ownershipRegisterResults[index] =
                g_ownershipRegisterResults[index].load(
                    std::memory_order_relaxed);
        }
        const std::uint64_t ownershipRegisteredBodies =
            g_ownershipRegisteredBodies.load(std::memory_order_relaxed);
        const std::uint64_t ownershipStaticProps =
            g_ownershipStaticProps.load(std::memory_order_relaxed);
        const std::uint64_t ownershipRenderValidProps =
            g_ownershipRenderValidProps.load(std::memory_order_relaxed);
        const std::uint64_t ownershipRenderInvalidProps =
            g_ownershipRenderInvalidProps.load(std::memory_order_relaxed);
        const std::uint64_t ownershipSubMeshes =
            g_ownershipSubMeshes.load(std::memory_order_relaxed);
        const std::uint64_t ownershipDirectBodyChildren =
            g_ownershipDirectBodyChildren.load(std::memory_order_relaxed);
        const std::uint64_t ownershipNestedBodyChildren =
            g_ownershipNestedBodyChildren.load(std::memory_order_relaxed);
        const std::uint64_t ownershipUnmatchedSubMeshes =
            g_ownershipUnmatchedSubMeshes.load(std::memory_order_relaxed);
        std::array<std::uint64_t, world_ownership::kPropTypeCount>
            ownershipSubMeshesByPropType{};
        std::array<std::uint64_t, world_ownership::kPropTypeCount>
            ownershipDirectByPropType{};
        std::array<std::uint64_t, world_ownership::kPropTypeCount>
            ownershipNestedByPropType{};
        std::array<std::uint64_t, world_ownership::kPropTypeCount>
            ownershipUnmatchedByPropType{};
        std::wstring renderOwnershipTypes;
        for (std::size_t index = 0;
             index < ownershipSubMeshesByPropType.size(); ++index) {
            ownershipSubMeshesByPropType[index] =
                g_ownershipSubMeshesByPropType[index].load(
                    std::memory_order_relaxed);
            ownershipDirectByPropType[index] =
                g_ownershipDirectByPropType[index].load(
                    std::memory_order_relaxed);
            ownershipNestedByPropType[index] =
                g_ownershipNestedByPropType[index].load(
                    std::memory_order_relaxed);
            ownershipUnmatchedByPropType[index] =
                g_ownershipUnmatchedByPropType[index].load(
                    std::memory_order_relaxed);
            const std::uint64_t total = ownershipSubMeshesByPropType[index] -
                previousOwnershipSubMeshesByPropType[index];
            if (total == 0) continue;
            if (!renderOwnershipTypes.empty()) renderOwnershipTypes += L",";
            renderOwnershipTypes += world_ownership::PropTypeName(
                static_cast<std::uint32_t>(index));
            renderOwnershipTypes += L":" + std::to_wstring(total) + L"/" +
                std::to_wstring(ownershipDirectByPropType[index] -
                    previousOwnershipDirectByPropType[index]) + L"/" +
                std::to_wstring(ownershipNestedByPropType[index] -
                    previousOwnershipNestedByPropType[index]) + L"/" +
                std::to_wstring(ownershipUnmatchedByPropType[index] -
                    previousOwnershipUnmatchedByPropType[index]);
        }
        if (renderOwnershipTypes.empty()) renderOwnershipTypes = L"none";
        const std::uint64_t newtonOwnedCallbacks =
            g_newtonOwnedCallbacks.load(std::memory_order_relaxed);
        const std::uint64_t newtonUnownedCallbacks =
            g_newtonUnownedCallbacks.load(std::memory_order_relaxed);
        std::array<std::uint64_t, world_ownership::kPropTypeCount>
            newtonCallbacksByPropType{};
        std::wstring activePropTypes;
        for (std::size_t index = 0;
             index < newtonCallbacksByPropType.size(); ++index) {
            newtonCallbacksByPropType[index] =
                g_newtonCallbacksByPropType[index].load(
                    std::memory_order_relaxed);
            const std::uint64_t delta = newtonCallbacksByPropType[index] -
                                        previousNewtonCallbacksByPropType[index];
            if (delta == 0) continue;
            if (!activePropTypes.empty()) activePropTypes += L",";
            activePropTypes += world_ownership::PropTypeName(
                static_cast<std::uint32_t>(index));
            activePropTypes += L":" + std::to_wstring(delta);
        }
        if (activePropTypes.empty()) activePropTypes = L"none";
        std::array<std::uint64_t, world_ownership::kPropTypeCount>
            worldAppliedByPropType{};
        std::array<std::uint64_t, world_ownership::kPropTypeCount>
            worldNestedTargetByPropType{};
        std::array<std::uint64_t, world_ownership::kPropTypeCount>
            worldOwnedDirectByPropType{};
        std::array<std::uint64_t, world_ownership::kPropTypeCount>
            worldOwnedNestedByPropType{};
        std::wstring worldPathTypes;
        for (std::size_t index = 0;
             index < worldAppliedByPropType.size(); ++index) {
            worldAppliedByPropType[index] =
                g_worldAppliedByPropType[index].load(
                    std::memory_order_relaxed);
            worldNestedTargetByPropType[index] =
                g_worldNestedTargetByPropType[index].load(
                    std::memory_order_relaxed);
            worldOwnedDirectByPropType[index] =
                g_worldOwnedDirectByPropType[index].load(
                    std::memory_order_relaxed);
            worldOwnedNestedByPropType[index] =
                g_worldOwnedNestedByPropType[index].load(
                    std::memory_order_relaxed);
            const std::uint64_t applied = worldAppliedByPropType[index] -
                previousWorldAppliedByPropType[index];
            const std::uint64_t nestedTarget =
                worldNestedTargetByPropType[index] -
                previousWorldNestedTargetByPropType[index];
            const std::uint64_t ownedDirect =
                worldOwnedDirectByPropType[index] -
                previousWorldOwnedDirectByPropType[index];
            const std::uint64_t ownedNested =
                worldOwnedNestedByPropType[index] -
                previousWorldOwnedNestedByPropType[index];
            if (applied == 0 && nestedTarget == 0 && ownedDirect == 0 &&
                ownedNested == 0) {
                continue;
            }
            if (!worldPathTypes.empty()) worldPathTypes += L",";
            worldPathTypes += world_ownership::PropTypeName(
                static_cast<std::uint32_t>(index));
            worldPathTypes += L":" + std::to_wstring(applied) + L"/" +
                std::to_wstring(nestedTarget) + L"/" +
                std::to_wstring(ownedDirect) + L"/" +
                std::to_wstring(ownedNested);
        }
        if (worldPathTypes.empty()) worldPathTypes = L"none";
        const auto cameraDelta = [](const auto& current, const auto& previous,
                                    const camera_audit::Result result) {
            const std::size_t index = static_cast<std::size_t>(result);
            return current[index] - previous[index];
        };
        const std::uint64_t tickEuler = cameraDelta(
            cameraTick, previousCameraTick, camera_audit::Result::validEuler);
        const std::uint64_t renderEuler = cameraDelta(
            cameraRender, previousCameraRender, camera_audit::Result::validEuler);
        const std::uint64_t matrixViews =
            cameraDelta(cameraTick, previousCameraTick,
                        camera_audit::Result::validMatrix) +
            cameraDelta(cameraRender, previousCameraRender,
                        camera_audit::Result::validMatrix);
        const std::uint64_t noView =
            cameraDelta(cameraTick, previousCameraTick,
                        camera_audit::Result::noScene) +
            cameraDelta(cameraTick, previousCameraTick,
                        camera_audit::Result::noCurrentListener) +
            cameraDelta(cameraTick, previousCameraTick,
                        camera_audit::Result::hidden) +
            cameraDelta(cameraTick, previousCameraTick,
                        camera_audit::Result::missingCamera) +
            cameraDelta(cameraTick, previousCameraTick,
                        camera_audit::Result::missingWorld) +
            cameraDelta(cameraRender, previousCameraRender,
                        camera_audit::Result::noCurrentListener) +
            cameraDelta(cameraRender, previousCameraRender,
                        camera_audit::Result::hidden) +
            cameraDelta(cameraRender, previousCameraRender,
                        camera_audit::Result::missingCamera) +
            cameraDelta(cameraRender, previousCameraRender,
                        camera_audit::Result::missingWorld);
        const std::uint64_t invalidCameraViews =
            cameraDelta(cameraTick, previousCameraTick,
                        camera_audit::Result::invalidViewport) +
            cameraDelta(cameraTick, previousCameraTick,
                        camera_audit::Result::invalidCamera) +
            cameraDelta(cameraRender, previousCameraRender,
                        camera_audit::Result::invalidViewport) +
            cameraDelta(cameraRender, previousCameraRender,
                        camera_audit::Result::invalidCamera);
        const auto handsDelta = [&handsAudit, &previousHandsAudit](
                                    const hands_audit::Result result) {
            const std::size_t index = static_cast<std::size_t>(result);
            return handsAudit[index] - previousHandsAudit[index];
        };
        const std::uint64_t handsMissing =
            handsDelta(hands_audit::Result::noHelper) +
            handsDelta(hands_audit::Result::missingEntity);
        const std::uint64_t handsInvalid =
            handsDelta(hands_audit::Result::invalidEntity) +
            handsDelta(hands_audit::Result::invalidMatrix);
        const auto handsModelDelta =
            [&handsModelAudit, &previousHandsModelAudit](
                const hands_audit::ModelResult result) {
                const std::size_t index = static_cast<std::size_t>(result);
                return handsModelAudit[index] - previousHandsModelAudit[index];
            };
        const auto poseDelta = [&poseAudit, &previousPoseAudit](
                                   const pose_audit::Result result) {
            const std::size_t index = static_cast<std::size_t>(result);
            return poseAudit[index] - previousPoseAudit[index];
        };

        log::Write(
            log::Level::info,
            L"Telemetry interval=" + Decimal(seconds) +
                L"s RenderUnlock=" +
                RenderUnlockStateName(
                    g_renderUnlockState.load(std::memory_order_acquire)) +
                L" PreUpdate=" + std::to_wstring(messageDeltas[4]) +
                L" (" + Rate(messageDeltas[4], seconds) +
                L" Hz) Update=" + std::to_wstring(messageDeltas[5]) +
                L" (" + Rate(messageDeltas[5], seconds) +
                L" Hz) PostUpdate=" + std::to_wstring(messageDeltas[6]) +
                L" (" + Rate(messageDeltas[6], seconds) +
                L" Hz) OnDraw=" + std::to_wstring(messageDeltas[2]) +
                L" (" + Rate(messageDeltas[2], seconds) +
                L" Hz) SceneRender=" + std::to_wstring(sceneDelta) +
                L" (" + Rate(sceneDelta, seconds) +
                L" Hz) OnPostRender=" + std::to_wstring(messageDeltas[3]) +
                L" (" + Rate(messageDeltas[3], seconds) +
                L" Hz) OnPostBufferSwap=" + std::to_wstring(messageDeltas[0]) +
                L" (" + Rate(messageDeltas[0], seconds) +
                L" Hz) SwapCadence(samples=" + std::to_wstring(cadence.samples) +
                L", meanMs=" + Decimal(cadence.meanMilliseconds) +
                L", p50Ms=" + Decimal(cadence.p50Milliseconds) +
                L", p95Ms=" + Decimal(cadence.p95Milliseconds) +
                L", p99Ms=" + Decimal(cadence.p99Milliseconds) +
                L", maxMs=" + Decimal(cadence.maxMilliseconds) +
                L", longGap=" + std::to_wstring(cadence.longGaps) +
                L", overflow=" + std::to_wstring(cadence.overflow) +
                L", invalid=" + std::to_wstring(cadence.invalid) +
                L") Clock(interp=" +
                std::to_wstring(clockInterpolated - previousClockInterpolated) +
                L", unavailable=" +
                std::to_wstring(clockUnavailable - previousClockUnavailable) +
                L", resets=" +
                std::to_wstring(clockResets - previousClockResets) +
                L", phaseInvalid=" +
                std::to_wstring(clockPhaseInvalid - previousClockPhaseInvalid) +
                L", alpha=" + Decimal(
                    static_cast<double>(g_latestAlphaMillionths.load(
                        std::memory_order_relaxed)) / 1'000'000.0) +
                L", rawAlpha=" + Decimal(
                    static_cast<double>(g_latestRawAlphaMillionths.load(
                        std::memory_order_relaxed)) / 1'000'000.0) +
                L", lastReset=" + clock::ResetReasonName(
                    g_lastClockReset.load(std::memory_order_relaxed)) +
                L") CameraAudit(tickEuler=" + std::to_wstring(tickEuler) +
                L", renderEuler=" + std::to_wstring(renderEuler) +
                L", matrix=" + std::to_wstring(matrixViews) +
                L", noView=" + std::to_wstring(noView) +
                L", invalid=" + std::to_wstring(invalidCameraViews) +
                L", identity=" + std::to_wstring(
                    cameraIdentityChanges - previousCameraIdentityChanges) +
                L", last=" + camera_audit::ResultName(
                    g_lastCameraAuditResult.load(std::memory_order_relaxed)) +
                L") CameraInterpolation(captured=" +
                std::to_wstring(cameraCaptured - previousCameraCaptured) +
                L", applied=" +
                std::to_wstring(cameraApplied - previousCameraApplied) +
                L", skipped=" +
                std::to_wstring(cameraSkipped - previousCameraSkipped) +
                L", restoreFailed=" +
                std::to_wstring(cameraRestoreFailures -
                                previousCameraRestoreFailures) +
                L", last=" + camera::FrameResultName(
                    g_lastCameraFrameResult.load(std::memory_order_relaxed)) +
                L") HandsAudit(valid=" +
                std::to_wstring(handsDelta(hands_audit::Result::valid)) +
                L", missing=" + std::to_wstring(handsMissing) +
                L", inactive=" +
                std::to_wstring(handsDelta(hands_audit::Result::inactive)) +
                L", invalid=" + std::to_wstring(handsInvalid) +
                L", identity=" + std::to_wstring(
                    handsIdentityChanges - previousHandsIdentityChanges) +
                L", moved=" +
                std::to_wstring(handsMoved - previousHandsMoved) +
                L", stable=" +
                std::to_wstring(handsStable - previousHandsStable) +
                L", last=" + hands_audit::ResultName(
                    g_lastHandsAuditResult.load(std::memory_order_relaxed)) +
                L") HandsRenderer(root=" + std::to_wstring(
                    handsModelDelta(hands_audit::ModelResult::handsRoot)) +
                L", object=" + std::to_wstring(
                    handsModelDelta(hands_audit::ModelResult::handObject)) +
                L", other=" + std::to_wstring(
                    handsModelDelta(hands_audit::ModelResult::other)) +
                L", invalid=" + std::to_wstring(
                    handsModelDelta(hands_audit::ModelResult::invalidMatrix)) +
                L", handObject=" + hands_audit::HandObjectResultName(
                    g_lastHandObjectResult.load(std::memory_order_relaxed)) +
                L", billboards=" + std::to_wstring(
                    g_lastHandObjectBillboards.load(std::memory_order_relaxed)) +
                L", particles=" + std::to_wstring(
                    g_lastHandObjectParticles.load(std::memory_order_relaxed)) +
                L", lights=" + std::to_wstring(
                    g_lastHandObjectLights.load(std::memory_order_relaxed)) +
                L") HandsInterpolation(captured=" + std::to_wstring(
                    handsCaptured - previousHandsCaptured) +
                L", renderReady=" + std::to_wstring(
                    handsRenderReady - previousHandsRenderReady) +
                L", skipped=" + std::to_wstring(
                    handsFrameSkipped - previousHandsFrameSkipped) +
                L", rootModels=" + std::to_wstring(
                    handsModelsApplied - previousHandsModelsApplied) +
                L", objectModels=" + std::to_wstring(
                    handObjectModelsApplied - previousHandObjectModelsApplied) +
                L", attachmentCandidates=" + std::to_wstring(
                    handAttachmentCandidates - previousHandAttachmentCandidates) +
                L", attachmentsApplied=" + std::to_wstring(
                    handAttachmentsApplied - previousHandAttachmentsApplied) +
                L", restoreFailed=" + std::to_wstring(
                    handAttachmentRestoreFailures -
                    previousHandAttachmentRestoreFailures) +
                L") HandsPoseAudit(identity=" + std::to_wstring(
                    poseDelta(pose_audit::Result::identityChanged)) +
                L", changed=" +
                    std::to_wstring(poseDelta(pose_audit::Result::changed)) +
                L", stable=" +
                    std::to_wstring(poseDelta(pose_audit::Result::stable)) +
                L", unavailable=" +
                    std::to_wstring(poseDelta(pose_audit::Result::unavailable)) +
                L", invalidVector=" + std::to_wstring(
                    poseDelta(pose_audit::Result::invalidVector)) +
                L", invalidMatrix=" + std::to_wstring(
                    poseDelta(pose_audit::Result::invalidMatrix)) +
                L", bones=" + std::to_wstring(
                    g_poseBoneCount.load(std::memory_order_relaxed)) +
                L", invalidBone=" + std::to_wstring(
                    g_poseInvalidBone.load(std::memory_order_relaxed)) +
                L", last=" + pose_audit::ResultName(
                    g_lastPoseAuditResult.load(std::memory_order_relaxed)) +
                L") HandsPoseInterpolation(staged=" + std::to_wstring(
                    poseStaged - previousPoseStaged) +
                L", applied=" + std::to_wstring(
                    poseApplied - previousPoseApplied) +
                L", unavailable=" + std::to_wstring(
                    poseUnavailable - previousPoseUnavailable) +
                L", identity=" + std::to_wstring(
                    poseIdentityChanges - previousPoseIdentityChanges) +
                L", clockWait=" + std::to_wstring(
                    poseClockWaits - previousPoseClockWaits) +
                L", pairMiss=" + std::to_wstring(
                    posePairMisses - previousPosePairMisses) +
                L", invalid=" + std::to_wstring(
                    poseInvalid - previousPoseInvalid) +
                L", restoreFailed=" + std::to_wstring(
                    poseRestoreFailures - previousPoseRestoreFailures) +
                L") EnemyPoseAudit(registered=" + std::to_wstring(
                    enemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::registered)] -
                    previousEnemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::registered)]) +
                L", staged=" + std::to_wstring(
                    enemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::staged)] -
                    previousEnemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::staged)]) +
                L", inactive=" + std::to_wstring(
                    enemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::inactive)] -
                    previousEnemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::inactive)]) +
                L", invalidEnemy=" + std::to_wstring(
                    enemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::invalidEnemy)] -
                    previousEnemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::invalidEnemy)]) +
                L", invalidType=" + std::to_wstring(
                    enemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::invalidType)] -
                    previousEnemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::invalidType)]) +
                L", capacity=" + std::to_wstring(
                    enemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::capacityExhausted)] -
                    previousEnemyStageResults[static_cast<std::size_t>(
                        enemy_pose_audit::StageResult::capacityExhausted)]) +
                L", identity=" + std::to_wstring(
                    enemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::identityChanged)] -
                    previousEnemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::identityChanged)]) +
                L", changed=" + std::to_wstring(
                    enemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::changed)] -
                    previousEnemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::changed)]) +
                L", stable=" + std::to_wstring(
                    enemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::stable)] -
                    previousEnemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::stable)]) +
                L", unavailable=" + std::to_wstring(
                    enemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::unavailable)] -
                    previousEnemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::unavailable)]) +
                L", invalidVector=" + std::to_wstring(
                    enemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::invalidVector)] -
                    previousEnemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::invalidVector)]) +
                L", invalidMatrix=" + std::to_wstring(
                    enemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::invalidMatrix)] -
                    previousEnemyObserveResults[static_cast<std::size_t>(
                        enemy_pose_audit::ObserveResult::invalidMatrix)]) +
                L", invalidBone=" + std::to_wstring(
                    g_enemyInvalidBone.load(std::memory_order_relaxed)) +
                L", types=staged/observed/changed/stable/identity/bones:" +
                    activeEnemyTypes +
                L", lastStage=" + enemy_pose_audit::StageResultName(
                    g_lastEnemyStageResult.load(std::memory_order_relaxed)) +
                L", lastPose=" + enemy_pose_audit::ObserveResultName(
                    g_lastEnemyObserveResult.load(std::memory_order_relaxed)) +
                L") EnemyPoseInterpolation(staged=" + std::to_wstring(
                    enemyPoseRuntimeStaged -
                    previousEnemyPoseRuntimeStaged) +
                L", unsupported=" + std::to_wstring(
                    enemyPoseRuntimeUnsupported -
                    previousEnemyPoseRuntimeUnsupported) +
                L", inactive=" + std::to_wstring(
                    enemyPoseRuntimeInactive -
                    previousEnemyPoseRuntimeInactive) +
                L", unavailable=" + std::to_wstring(
                    enemyPoseRuntimeUnavailable -
                    previousEnemyPoseRuntimeUnavailable) +
                L", capacity=" + std::to_wstring(
                    enemyPoseRuntimeCapacity -
                    previousEnemyPoseRuntimeCapacity) +
                L", captured=" + std::to_wstring(
                    enemyPoseRuntimeCaptured -
                    previousEnemyPoseRuntimeCaptured) +
                L", applied=" + std::to_wstring(
                    enemyPoseRuntimeApplied -
                    previousEnemyPoseRuntimeApplied) +
                L", identity=" + std::to_wstring(
                    enemyPoseRuntimeIdentity -
                    previousEnemyPoseRuntimeIdentity) +
                L", clockWait=" + std::to_wstring(
                    enemyPoseRuntimeClockWaits -
                    previousEnemyPoseRuntimeClockWaits) +
                L", pairMiss=" + std::to_wstring(
                    enemyPoseRuntimePairMisses -
                    previousEnemyPoseRuntimePairMisses) +
                L", invalid=" + std::to_wstring(
                    enemyPoseRuntimeInvalid -
                    previousEnemyPoseRuntimeInvalid) +
                L", restoreFailed=" + std::to_wstring(
                    enemyPoseRuntimeRestoreFailures -
                    previousEnemyPoseRuntimeRestoreFailures) +
                L", last=" + enemy_pose::UpdateResultName(
                    g_lastEnemyPoseRuntimeResult.load(
                        std::memory_order_relaxed)) +
                L") RagdollPoseAudit(identity=" + std::to_wstring(
                    ragdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::identityChanged)] -
                    previousRagdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::identityChanged)]) +
                L", changed=" + std::to_wstring(
                    ragdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::changed)] -
                    previousRagdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::changed)]) +
                L", stable=" + std::to_wstring(
                    ragdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::stable)] -
                    previousRagdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::stable)]) +
                L", unavailable=" + std::to_wstring(
                    ragdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::unavailable)] -
                    previousRagdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::unavailable)]) +
                L", invalidFlag=" + std::to_wstring(
                    ragdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::invalidFlag)] -
                    previousRagdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::invalidFlag)]) +
                L", invalidVector=" + std::to_wstring(
                    ragdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::invalidVector)] -
                    previousRagdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::invalidVector)]) +
                L", invalidMatrix=" + std::to_wstring(
                    ragdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::invalidMatrix)] -
                    previousRagdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::invalidMatrix)]) +
                L", capacity=" + std::to_wstring(
                    ragdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::capacityExhausted)] -
                    previousRagdollPoseResults[static_cast<std::size_t>(
                        ragdoll_pose_audit::Result::capacityExhausted)]) +
                L", bones=" + std::to_wstring(
                    g_ragdollBoneCount.load(std::memory_order_relaxed)) +
                L", invalidBone=" + std::to_wstring(
                    g_ragdollInvalidBone.load(std::memory_order_relaxed)) +
                L", last=" + ragdoll_pose_audit::ResultName(
                    g_lastRagdollPoseResult.load(std::memory_order_relaxed)) +
                L") RagdollPoseInterpolation(captured=" + std::to_wstring(
                    ragdollRuntimeCaptured -
                    previousRagdollRuntimeCaptured) +
                L", applied=" + std::to_wstring(
                    ragdollRuntimeApplied - previousRagdollRuntimeApplied) +
                L", identity=" + std::to_wstring(
                    ragdollRuntimeIdentity - previousRagdollRuntimeIdentity) +
                L", unavailable=" + std::to_wstring(
                    ragdollRuntimeUnavailable -
                    previousRagdollRuntimeUnavailable) +
                L", clockWait=" + std::to_wstring(
                    ragdollRuntimeClockWaits -
                    previousRagdollRuntimeClockWaits) +
                L", pairMiss=" + std::to_wstring(
                    ragdollRuntimePairMisses -
                    previousRagdollRuntimePairMisses) +
                L", invalid=" + std::to_wstring(
                    ragdollRuntimeInvalid - previousRagdollRuntimeInvalid) +
                L", capacity=" + std::to_wstring(
                    ragdollRuntimeCapacity - previousRagdollRuntimeCapacity) +
                L", restoreFailed=" + std::to_wstring(
                    ragdollRuntimeRestoreFailures -
                    previousRagdollRuntimeRestoreFailures) +
                L", last=" + ragdoll_pose::UpdateResultName(
                    g_lastRagdollRuntimeResult.load(
                        std::memory_order_relaxed)) +
                L") RagdollSkinning(forced=" + std::to_wstring(
                    ragdollSkinningForced - previousRagdollSkinningForced) +
                L", unavailable=" + std::to_wstring(
                    ragdollSkinningUnavailable -
                    previousRagdollSkinningUnavailable) +
                L", invalidFlag=" + std::to_wstring(
                    ragdollSkinningInvalidFlag -
                    previousRagdollSkinningInvalidFlag) +
                L", last=" + ragdoll_skinning::PrepareResultName(
                    g_lastRagdollSkinningResult.load(
                        std::memory_order_relaxed)) +
                L") NodeAnimation(captured=" + std::to_wstring(
                    nodeAnimationCaptured - previousNodeAnimationCaptured) +
                L", changed=" + std::to_wstring(
                    nodeAnimationChanged - previousNodeAnimationChanged) +
                L", applied=" + std::to_wstring(
                    nodeAnimationApplied - previousNodeAnimationApplied) +
                L", stable=" + std::to_wstring(
                    nodeAnimationStable - previousNodeAnimationStable) +
                L", clockWait=" + std::to_wstring(
                    nodeAnimationClockWaits -
                    previousNodeAnimationClockWaits) +
                L", pairMiss=" + std::to_wstring(
                    nodeAnimationPairMisses -
                    previousNodeAnimationPairMisses) +
                L", invalid=" + std::to_wstring(
                    nodeAnimationInvalid - previousNodeAnimationInvalid) +
                L", capacity=" + std::to_wstring(
                    nodeAnimationCapacity - previousNodeAnimationCapacity) +
                L", last=" + node_animation::OverrideResultName(
                    g_lastNodeAnimationResult.load(
                        std::memory_order_relaxed)) +
                L") NewtonMotionAudit(callbacks=" + std::to_wstring(
                    newtonTransformCallbacks -
                    previousNewtonTransformCallbacks) +
                L", thread0=" + std::to_wstring(
                    newtonTransformThreadZero -
                    previousNewtonTransformThreadZero) +
                L", worker=" + std::to_wstring(
                    newtonTransformWorkerThread -
                    previousNewtonTransformWorkerThread) +
                L", invalidThread=" + std::to_wstring(
                    newtonTransformInvalidThread -
                    previousNewtonTransformInvalidThread) +
                L") WorldOwnership(registered=" + std::to_wstring(
                    ownershipRegisterResults[static_cast<std::size_t>(
                        world_ownership::RegisterResult::registered)] -
                    previousOwnershipRegisterResults[static_cast<std::size_t>(
                        world_ownership::RegisterResult::registered)]) +
                L", bodies=" + std::to_wstring(
                    ownershipRegisteredBodies -
                    previousOwnershipRegisteredBodies) +
                L", bodyless=" + std::to_wstring(
                    ownershipRegisterResults[static_cast<std::size_t>(
                        world_ownership::RegisterResult::bodyless)] -
                    previousOwnershipRegisterResults[static_cast<std::size_t>(
                        world_ownership::RegisterResult::bodyless)]) +
                L", invalid=" + std::to_wstring(
                    ownershipRegisterResults[static_cast<std::size_t>(
                        world_ownership::RegisterResult::invalid)] -
                    previousOwnershipRegisterResults[static_cast<std::size_t>(
                        world_ownership::RegisterResult::invalid)]) +
                L", capacity=" + std::to_wstring(
                    ownershipRegisterResults[static_cast<std::size_t>(
                        world_ownership::RegisterResult::capacityExhausted)] -
                    previousOwnershipRegisterResults[static_cast<std::size_t>(
                        world_ownership::RegisterResult::capacityExhausted)]) +
                L", static=" + std::to_wstring(
                    ownershipStaticProps - previousOwnershipStaticProps) +
                L", renderValid=" + std::to_wstring(
                    ownershipRenderValidProps -
                    previousOwnershipRenderValidProps) +
                L", renderInvalid=" + std::to_wstring(
                    ownershipRenderInvalidProps -
                    previousOwnershipRenderInvalidProps) +
                L", submeshes=" + std::to_wstring(
                    ownershipSubMeshes - previousOwnershipSubMeshes) +
                L", direct=" + std::to_wstring(
                    ownershipDirectBodyChildren -
                    previousOwnershipDirectBodyChildren) +
                L", nested=" + std::to_wstring(
                    ownershipNestedBodyChildren -
                    previousOwnershipNestedBodyChildren) +
                L", renderUnmatched=" + std::to_wstring(
                    ownershipUnmatchedSubMeshes -
                    previousOwnershipUnmatchedSubMeshes) +
                L", matched=" + std::to_wstring(
                    newtonOwnedCallbacks - previousNewtonOwnedCallbacks) +
                L", unmatched=" + std::to_wstring(
                    newtonUnownedCallbacks - previousNewtonUnownedCallbacks) +
                L", types=" + activePropTypes +
                L", renderTypes=" + renderOwnershipTypes +
                L") WorldRegistration(registered=" + std::to_wstring(
                    worldRegistrationResults[static_cast<std::size_t>(
                        world::RegistrationKind::registered)] -
                    previousWorldRegistrationResults[static_cast<std::size_t>(
                        world::RegistrationKind::registered)]) +
                L", bodies=" + std::to_wstring(
                    worldRegistrationBodies -
                    previousWorldRegistrationBodies) +
                L", bodyless=" + std::to_wstring(
                    worldRegistrationResults[static_cast<std::size_t>(
                        world::RegistrationKind::bodyless)] -
                    previousWorldRegistrationResults[static_cast<std::size_t>(
                        world::RegistrationKind::bodyless)]) +
                L", invalid=" + std::to_wstring(
                    worldRegistrationResults[static_cast<std::size_t>(
                        world::RegistrationKind::invalidLayout)] -
                    previousWorldRegistrationResults[static_cast<std::size_t>(
                        world::RegistrationKind::invalidLayout)]) +
                L", incomplete=" + std::to_wstring(
                    worldRegistrationResults[static_cast<std::size_t>(
                        world::RegistrationKind::incompleteRenderMapping)] -
                    previousWorldRegistrationResults[static_cast<std::size_t>(
                        world::RegistrationKind::incompleteRenderMapping)]) +
                L", capacity=" + std::to_wstring(
                    worldRegistrationResults[static_cast<std::size_t>(
                        world::RegistrationKind::capacityExhausted)] -
                    previousWorldRegistrationResults[static_cast<std::size_t>(
                        world::RegistrationKind::capacityExhausted)]) +
                L") WorldInterpolation(callbacks=" + std::to_wstring(
                    worldCallbacksStaged - previousWorldCallbacksStaged) +
                L", captured=" + std::to_wstring(
                    worldBodiesCaptured - previousWorldBodiesCaptured) +
                L", duplicate=" + std::to_wstring(
                    worldDuplicateCallbacks -
                    previousWorldDuplicateCallbacks) +
                L", captureInvalid=" + std::to_wstring(
                    worldCaptureInvalid - previousWorldCaptureInvalid) +
                L", moveOverflow=" + std::to_wstring(
                    worldMoveOverflow - previousWorldMoveOverflow) +
                L", applied=" + std::to_wstring(
                    worldModelsApplied - previousWorldModelsApplied) +
                L", clockWait=" + std::to_wstring(
                    worldClockUnavailable -
                    previousWorldClockUnavailable) +
                L", historyWait=" + std::to_wstring(
                    worldHistoryUnavailable -
                    previousWorldHistoryUnavailable) +
                L", pairMiss=" + std::to_wstring(
                    worldPairMissing - previousWorldPairMissing) +
                L", invalid=" + std::to_wstring(
                    worldOverrideInvalid - previousWorldOverrideInvalid) +
                L", outputOverflow=" + std::to_wstring(
                    worldOutputOverflow - previousWorldOutputOverflow) +
                L", last=" + world::OverrideResultName(
                    g_lastWorldOverride.load(std::memory_order_relaxed)) +
                L") WorldRelationship(candidates=" + std::to_wstring(
                    worldRelationshipCandidates -
                    previousWorldRelationshipCandidates) +
                L", overflow=" + std::to_wstring(
                    worldRelationshipOverflow -
                    previousWorldRelationshipOverflow) +
                L", nestedTarget=" + std::to_wstring(
                    worldNestedTarget - previousWorldNestedTarget) +
                L", ownedDirect=" + std::to_wstring(
                    worldOwnedDirect - previousWorldOwnedDirect) +
                L", ownedNested=" + std::to_wstring(
                    worldOwnedNested - previousWorldOwnedNested) +
                L", standaloneDirect=" + std::to_wstring(
                    worldStandaloneDirect - previousWorldStandaloneDirect) +
                L", standaloneNested=" + std::to_wstring(
                    worldStandaloneNested - previousWorldStandaloneNested) +
                L", promoted=" + std::to_wstring(
                    worldStandalonePromoted -
                    previousWorldStandalonePromoted) +
                L", standaloneApplied=" + std::to_wstring(
                    worldStandaloneModelsApplied -
                    previousWorldStandaloneModelsApplied) +
                L", pathTypes=applied/nestedTarget/ownedDirect/ownedNested:" +
                    worldPathTypes +
                L") Threads(updater=" +
                std::to_wstring(g_updaterThread.load(std::memory_order_relaxed)) +
                L", scene=" +
                std::to_wstring(g_sceneThread.load(std::memory_order_relaxed)) +
                L", updaterMismatch=" +
                std::to_wstring(
                    g_updaterThreadMismatches.load(std::memory_order_relaxed)) +
                L", sceneMismatch=" +
                std::to_wstring(
                    g_sceneThreadMismatches.load(std::memory_order_relaxed)) +
                L")");
        previous = now;
        previousClockInterpolated = clockInterpolated;
        previousClockUnavailable = clockUnavailable;
        previousClockResets = clockResets;
        previousClockPhaseInvalid = clockPhaseInvalid;
        previousCameraTick = cameraTick;
        previousCameraRender = cameraRender;
        previousCameraIdentityChanges = cameraIdentityChanges;
        previousCameraCaptured = cameraCaptured;
        previousCameraApplied = cameraApplied;
        previousCameraSkipped = cameraSkipped;
        previousCameraRestoreFailures = cameraRestoreFailures;
        previousHandsAudit = handsAudit;
        previousHandsIdentityChanges = handsIdentityChanges;
        previousHandsMoved = handsMoved;
        previousHandsStable = handsStable;
        previousHandsModelAudit = handsModelAudit;
        previousHandsCaptured = handsCaptured;
        previousHandsRenderReady = handsRenderReady;
        previousHandsFrameSkipped = handsFrameSkipped;
        previousHandsModelsApplied = handsModelsApplied;
        previousHandObjectModelsApplied = handObjectModelsApplied;
        previousHandAttachmentCandidates = handAttachmentCandidates;
        previousHandAttachmentsApplied = handAttachmentsApplied;
        previousHandAttachmentRestoreFailures =
            handAttachmentRestoreFailures;
        previousPoseAudit = poseAudit;
        previousPoseStaged = poseStaged;
        previousPoseApplied = poseApplied;
        previousPoseUnavailable = poseUnavailable;
        previousPoseIdentityChanges = poseIdentityChanges;
        previousPoseClockWaits = poseClockWaits;
        previousPosePairMisses = posePairMisses;
        previousPoseInvalid = poseInvalid;
        previousPoseRestoreFailures = poseRestoreFailures;
        previousEnemyStageResults = enemyStageResults;
        previousEnemyObserveResults = enemyObserveResults;
        previousEnemyStagedByType = enemyStagedByType;
        previousEnemyObservedByType = enemyObservedByType;
        previousEnemyChangedByType = enemyChangedByType;
        previousEnemyStableByType = enemyStableByType;
        previousEnemyIdentityByType = enemyIdentityByType;
        previousEnemyPoseRuntimeStaged = enemyPoseRuntimeStaged;
        previousEnemyPoseRuntimeUnsupported = enemyPoseRuntimeUnsupported;
        previousEnemyPoseRuntimeInactive = enemyPoseRuntimeInactive;
        previousEnemyPoseRuntimeUnavailable = enemyPoseRuntimeUnavailable;
        previousEnemyPoseRuntimeCapacity = enemyPoseRuntimeCapacity;
        previousEnemyPoseRuntimeCaptured = enemyPoseRuntimeCaptured;
        previousEnemyPoseRuntimeApplied = enemyPoseRuntimeApplied;
        previousEnemyPoseRuntimeIdentity = enemyPoseRuntimeIdentity;
        previousEnemyPoseRuntimeClockWaits = enemyPoseRuntimeClockWaits;
        previousEnemyPoseRuntimePairMisses = enemyPoseRuntimePairMisses;
        previousEnemyPoseRuntimeInvalid = enemyPoseRuntimeInvalid;
        previousEnemyPoseRuntimeRestoreFailures =
            enemyPoseRuntimeRestoreFailures;
        previousRagdollPoseResults = ragdollPoseResults;
        previousRagdollRuntimeCaptured = ragdollRuntimeCaptured;
        previousRagdollRuntimeApplied = ragdollRuntimeApplied;
        previousRagdollRuntimeIdentity = ragdollRuntimeIdentity;
        previousRagdollRuntimeUnavailable = ragdollRuntimeUnavailable;
        previousRagdollRuntimeClockWaits = ragdollRuntimeClockWaits;
        previousRagdollRuntimePairMisses = ragdollRuntimePairMisses;
        previousRagdollRuntimeInvalid = ragdollRuntimeInvalid;
        previousRagdollRuntimeCapacity = ragdollRuntimeCapacity;
        previousRagdollRuntimeRestoreFailures =
            ragdollRuntimeRestoreFailures;
        previousRagdollSkinningForced = ragdollSkinningForced;
        previousRagdollSkinningUnavailable = ragdollSkinningUnavailable;
        previousRagdollSkinningInvalidFlag = ragdollSkinningInvalidFlag;
        previousNewtonTransformCallbacks = newtonTransformCallbacks;
        previousNewtonTransformThreadZero = newtonTransformThreadZero;
        previousNewtonTransformWorkerThread = newtonTransformWorkerThread;
        previousNewtonTransformInvalidThread = newtonTransformInvalidThread;
        previousNodeAnimationCaptured = nodeAnimationCaptured;
        previousNodeAnimationChanged = nodeAnimationChanged;
        previousNodeAnimationApplied = nodeAnimationApplied;
        previousNodeAnimationStable = nodeAnimationStable;
        previousNodeAnimationClockWaits = nodeAnimationClockWaits;
        previousNodeAnimationPairMisses = nodeAnimationPairMisses;
        previousNodeAnimationInvalid = nodeAnimationInvalid;
        previousNodeAnimationCapacity = nodeAnimationCapacity;
        previousOwnershipRegisterResults = ownershipRegisterResults;
        previousOwnershipRegisteredBodies = ownershipRegisteredBodies;
        previousOwnershipStaticProps = ownershipStaticProps;
        previousOwnershipRenderValidProps = ownershipRenderValidProps;
        previousOwnershipRenderInvalidProps = ownershipRenderInvalidProps;
        previousOwnershipSubMeshes = ownershipSubMeshes;
        previousOwnershipDirectBodyChildren = ownershipDirectBodyChildren;
        previousOwnershipNestedBodyChildren = ownershipNestedBodyChildren;
        previousOwnershipUnmatchedSubMeshes = ownershipUnmatchedSubMeshes;
        previousOwnershipSubMeshesByPropType = ownershipSubMeshesByPropType;
        previousOwnershipDirectByPropType = ownershipDirectByPropType;
        previousOwnershipNestedByPropType = ownershipNestedByPropType;
        previousOwnershipUnmatchedByPropType =
            ownershipUnmatchedByPropType;
        previousNewtonOwnedCallbacks = newtonOwnedCallbacks;
        previousNewtonUnownedCallbacks = newtonUnownedCallbacks;
        previousNewtonCallbacksByPropType = newtonCallbacksByPropType;
        previousWorldRegistrationResults = worldRegistrationResults;
        previousWorldRegistrationBodies = worldRegistrationBodies;
        previousWorldCallbacksStaged = worldCallbacksStaged;
        previousWorldBodiesCaptured = worldBodiesCaptured;
        previousWorldDuplicateCallbacks = worldDuplicateCallbacks;
        previousWorldCaptureInvalid = worldCaptureInvalid;
        previousWorldMoveOverflow = worldMoveOverflow;
        previousWorldModelsApplied = worldModelsApplied;
        previousWorldStandaloneModelsApplied = worldStandaloneModelsApplied;
        previousWorldClockUnavailable = worldClockUnavailable;
        previousWorldHistoryUnavailable = worldHistoryUnavailable;
        previousWorldPairMissing = worldPairMissing;
        previousWorldOverrideInvalid = worldOverrideInvalid;
        previousWorldOutputOverflow = worldOutputOverflow;
        previousWorldRelationshipCandidates = worldRelationshipCandidates;
        previousWorldRelationshipOverflow = worldRelationshipOverflow;
        previousWorldNestedTarget = worldNestedTarget;
        previousWorldOwnedDirect = worldOwnedDirect;
        previousWorldOwnedNested = worldOwnedNested;
        previousWorldStandaloneDirect = worldStandaloneDirect;
        previousWorldStandaloneNested = worldStandaloneNested;
        previousWorldStandalonePromoted = worldStandalonePromoted;
        previousWorldAppliedByPropType = worldAppliedByPropType;
        previousWorldNestedTargetByPropType = worldNestedTargetByPropType;
        previousWorldOwnedDirectByPropType = worldOwnedDirectByPropType;
        previousWorldOwnedNestedByPropType = worldOwnedNestedByPropType;
    }
}

DWORD WINAPI RagdollProfileThread(void*) {
    LARGE_INTEGER previousTime{};
    QueryPerformanceCounter(&previousTime);
    std::uint64_t previousCalls = 0;
    std::uint64_t previousTicks = 0;
    std::uint64_t previousVertices = 0;
    std::uint64_t previousBones = 0;
    std::uint64_t previousScenes = 0;
    std::uint64_t previousStationary = 0;
    const DWORD waitMilliseconds = g_reportIntervalSeconds * 1000U;
    for (;;) {
        Sleep(waitMilliseconds);
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now) ||
            g_qpcFrequency.QuadPart <= 0) {
            continue;
        }
        const std::uint64_t calls =
            g_ragdollProfileCalls.load(std::memory_order_relaxed);
        const std::uint64_t ticks =
            g_ragdollProfileQpcTicks.load(std::memory_order_relaxed);
        const std::uint64_t vertices =
            g_ragdollProfileVertices.load(std::memory_order_relaxed);
        const std::uint64_t bones =
            g_ragdollProfileBones.load(std::memory_order_relaxed);
        const std::uint64_t scenes =
            g_sceneRenders.load(std::memory_order_relaxed);
        const std::uint64_t stationary =
            g_ragdollRuntimeStationary.load(std::memory_order_relaxed);
        const std::uint64_t callDelta = calls - previousCalls;
        const std::uint64_t tickDelta = ticks - previousTicks;
        const std::uint64_t vertexDelta = vertices - previousVertices;
        const std::uint64_t boneDelta = bones - previousBones;
        const std::uint64_t sceneDelta = scenes - previousScenes;
        const std::uint64_t stationaryDelta = stationary - previousStationary;
        const std::uint64_t maxTicks =
            g_ragdollProfileMaxQpcTicks.exchange(0,
                                                 std::memory_order_relaxed);
        const std::uint32_t maxVertices = maxTicks == 0
            ? 0
            : g_ragdollProfileMaxVertices.load(std::memory_order_relaxed);
        const std::uint32_t maxBones = maxTicks == 0
            ? 0
            : g_ragdollProfileMaxBones.load(std::memory_order_relaxed);
        const std::uintptr_t maxSubMesh = maxTicks == 0
            ? 0
            : g_ragdollProfileMaxSubMesh.load(std::memory_order_relaxed);
        const double intervalSeconds =
            static_cast<double>(now.QuadPart - previousTime.QuadPart) /
            static_cast<double>(g_qpcFrequency.QuadPart);
        const double totalMilliseconds =
            static_cast<double>(tickDelta) * 1000.0 /
            static_cast<double>(g_qpcFrequency.QuadPart);
        const double averageMicroseconds = callDelta == 0
            ? 0.0
            : totalMilliseconds * 1000.0 /
                  static_cast<double>(callDelta);
        const double maximumMicroseconds =
            static_cast<double>(maxTicks) * 1000000.0 /
            static_cast<double>(g_qpcFrequency.QuadPart);
        const double callsPerFrame = sceneDelta == 0
            ? 0.0
            : static_cast<double>(callDelta) /
                  static_cast<double>(sceneDelta);
        const double millisecondsPerFrame = sceneDelta == 0
            ? 0.0
            : totalMilliseconds / static_cast<double>(sceneDelta);
        const double averageVertices = callDelta == 0
            ? 0.0
            : static_cast<double>(vertexDelta) /
                  static_cast<double>(callDelta);
        const double averageBones = callDelta == 0
            ? 0.0
            : static_cast<double>(boneDelta) /
                  static_cast<double>(callDelta);
        wchar_t pointerBuffer[24]{};
        _snwprintf_s(pointerBuffer, std::size(pointerBuffer), _TRUNCATE,
                     L"0x%08llX",
                     static_cast<unsigned long long>(maxSubMesh));
        log::Write(
            log::Level::info,
            L"RagdollProfile interval=" + Decimal(intervalSeconds) +
                L"s scenes=" + std::to_wstring(sceneDelta) +
                L" stationary=" + std::to_wstring(stationaryDelta) +
                L" forced=" + std::to_wstring(callDelta) +
                L" callsPerFrame=" + Decimal(callsPerFrame) +
                L" skinTotalMs=" + Decimal(totalMilliseconds) +
                L" avgUs=" + Decimal(averageMicroseconds) +
                L" maxUs=" + Decimal(maximumMicroseconds) +
                L" estimatedMsPerFrame=" + Decimal(millisecondsPerFrame) +
                L" avgVertices=" + Decimal(averageVertices) +
                L" avgBones=" + Decimal(averageBones) +
                L" maxVertices=" + std::to_wstring(maxVertices) +
                L" maxBones=" + std::to_wstring(maxBones) +
                L" maxSubMesh=" + pointerBuffer);
        previousTime = now;
        previousCalls = calls;
        previousTicks = ticks;
        previousVertices = vertices;
        previousBones = bones;
        previousScenes = scenes;
        previousStationary = stationary;
    }
}

std::wstring HookError(const wchar_t* operation, const MH_STATUS status) {
    const char* text = MH_StatusToString(status);
    std::wstring statusText;
    if (text != nullptr) {
        while (*text != '\0') {
            statusText.push_back(static_cast<unsigned char>(*text));
            ++text;
        }
    }
    return std::wstring(operation) + L" failed: " + statusText;
}

void RollBack(const target::CoreMapping& mapping) {
    MH_DisableHook(MH_ALL_HOOKS);
    if (mapping.engineRun.address != nullptr) {
        MH_RemoveHook(mapping.engineRun.address);
    }
    if (mapping.updaterRunMessage.address != nullptr) {
        MH_RemoveHook(mapping.updaterRunMessage.address);
    }
    if (mapping.sceneRender.address != nullptr) {
        MH_RemoveHook(mapping.sceneRender.address);
    }
    if (mapping.playerHandsPostUpdate.address != nullptr) {
        MH_RemoveHook(mapping.playerHandsPostUpdate.address);
    }
    if (mapping.luxEnemyOnUpdate.address != nullptr) {
        MH_RemoveHook(mapping.luxEnemyOnUpdate.address);
    }
    if (mapping.luxPropSetupAfterLoad.address != nullptr) {
        MH_RemoveHook(mapping.luxPropSetupAfterLoad.address);
    }
    if (mapping.newtonTransformCallback.address != nullptr) {
        MH_RemoveHook(mapping.newtonTransformCallback.address);
    }
    if (mapping.subMeshGetModelMatrix.address != nullptr) {
        MH_RemoveHook(mapping.subMeshGetModelMatrix.address);
    }
    if (mapping.subMeshUpdateGraphics.address != nullptr) {
        MH_RemoveHook(mapping.subMeshUpdateGraphics.address);
    }
    if (mapping.meshEntityUpdateGraphics.address != nullptr) {
        MH_RemoveHook(mapping.meshEntityUpdateGraphics.address);
    }
    if (mapping.imageTrailRenderEffect.address != nullptr) {
        MH_RemoveHook(mapping.imageTrailRenderEffect.address);
    }
    MH_Uninitialize();
}

} // namespace

bool Initialize(const target::CoreMapping& mapping,
                const bool telemetryEnabled,
                const bool ragdollProfileEnabled,
                const bool renderUnlockEnabled,
                const bool cameraInterpolationEnabled,
                const bool handsInterpolationEnabled,
                const bool handsPoseInterpolationEnabled,
                const bool enemyPoseInterpolationEnabled,
                const bool ragdollPoseInterpolationEnabled,
                const bool nodeAnimationInterpolationEnabled,
                const bool worldInterpolationEnabled,
                const bool imageTrailTimingEnabled,
                const target::MouseMapping* const mouseMapping,
                const target::ParticleMapping* const particleMapping,
                const unsigned int reportIntervalSeconds,
                std::wstring& error) {
    if (!telemetryEnabled && !ragdollProfileEnabled &&
        particleMapping == nullptr &&
        !renderUnlockEnabled &&
        !cameraInterpolationEnabled && !handsInterpolationEnabled &&
        !handsPoseInterpolationEnabled && !enemyPoseInterpolationEnabled &&
        !ragdollPoseInterpolationEnabled &&
        !nodeAnimationInterpolationEnabled && !worldInterpolationEnabled &&
        !imageTrailTimingEnabled) {
        return true;
    }
    if (ragdollProfileEnabled && !ragdollPoseInterpolationEnabled) {
        error = L"RagdollProfile requires RagdollPoseInterpolation=true";
        return false;
    }
    const bool clockNeeded = telemetryEnabled || ragdollProfileEnabled ||
                             cameraInterpolationEnabled ||
                             handsInterpolationEnabled ||
                             handsPoseInterpolationEnabled ||
                             enemyPoseInterpolationEnabled ||
                             ragdollPoseInterpolationEnabled ||
                             nodeAnimationInterpolationEnabled ||
                             worldInterpolationEnabled ||
                             particleMapping != nullptr;
    const bool frameHooksNeeded = clockNeeded;
    const bool handsHooksNeeded = telemetryEnabled || handsInterpolationEnabled ||
                                  handsPoseInterpolationEnabled ||
                                  nodeAnimationInterpolationEnabled ||
                                  worldInterpolationEnabled;
    const bool worldHooksNeeded = telemetryEnabled || worldInterpolationEnabled;
    const bool poseAuditHookNeeded = telemetryEnabled &&
                                     !handsPoseInterpolationEnabled;
    const bool enemyPoseAuditHookNeeded = telemetryEnabled;
    const bool enemyHooksNeeded = enemyPoseAuditHookNeeded ||
                                  enemyPoseInterpolationEnabled;
    const bool meshPoseHookNeeded = poseAuditHookNeeded ||
                                    enemyPoseAuditHookNeeded ||
                                    enemyPoseInterpolationEnabled ||
                                    ragdollPoseInterpolationEnabled;
    const bool ragdollSkinningHookNeeded = ragdollPoseInterpolationEnabled ||
                                           enemyPoseInterpolationEnabled;
    if ((renderUnlockEnabled &&
         (mapping.engineRun.address == nullptr ||
          mapping.renderUpdateGate.address == nullptr)) ||
        (frameHooksNeeded &&
         (mapping.updaterRunMessage.address == nullptr ||
          mapping.sceneRender.address == nullptr)) ||
        (worldHooksNeeded &&
         (mapping.luxPropSetupAfterLoad.address == nullptr ||
          mapping.newtonTransformCallback.address == nullptr)) ||
        (enemyHooksNeeded &&
         mapping.luxEnemyOnUpdate.address == nullptr) ||
        (meshPoseHookNeeded &&
         mapping.meshEntityUpdateGraphics.address == nullptr) ||
        (ragdollSkinningHookNeeded &&
         mapping.subMeshUpdateGraphics.address == nullptr) ||
        (imageTrailTimingEnabled &&
         mapping.imageTrailRenderEffect.address == nullptr) ||
        (handsHooksNeeded &&
         (mapping.playerHandsPostUpdate.address == nullptr ||
          mapping.entityGetWorldMatrix.address == nullptr ||
          mapping.entityGetEntityParent.address == nullptr ||
          mapping.subMeshGetModelMatrix.address == nullptr)) ||
        (cameraInterpolationEnabled &&
         (mapping.cameraSetPosition.address == nullptr ||
          mapping.cameraSetPitch.address == nullptr ||
          mapping.cameraSetYaw.address == nullptr ||
          mapping.cameraSetRoll.address == nullptr))) {
        error = L"Runtime hook setup received an incomplete core mapping";
        return false;
    }
    if (clockNeeded &&
        (!QueryPerformanceFrequency(&g_qpcFrequency) ||
         g_qpcFrequency.QuadPart <= 0)) {
        error = L"QueryPerformanceFrequency failed with Win32 error " +
                std::to_wstring(GetLastError());
        return false;
    }
    g_reportIntervalSeconds = reportIntervalSeconds;
    g_telemetryEnabled = telemetryEnabled;
    g_ragdollProfileEnabled = ragdollProfileEnabled;
    g_clockDiagnosticsEnabled = clockNeeded;
    g_renderUnlockEnabled = renderUnlockEnabled;
    g_cameraInterpolationEnabled = cameraInterpolationEnabled;
    g_handsAuditEnabled = telemetryEnabled;
    g_handsInterpolationEnabled = handsInterpolationEnabled;
    g_handsPoseInterpolationEnabled = handsPoseInterpolationEnabled;
    g_enemyPoseInterpolationEnabled = enemyPoseInterpolationEnabled;
    g_ragdollPoseInterpolationEnabled = ragdollPoseInterpolationEnabled;
    g_nodeAnimationInterpolationEnabled =
        nodeAnimationInterpolationEnabled;
    g_worldInterpolationEnabled = worldInterpolationEnabled;
    g_poseAuditEnabled = poseAuditHookNeeded;
    g_enemyPoseAuditEnabled = enemyPoseAuditHookNeeded;
    g_ragdollPoseAuditEnabled = telemetryEnabled;
    g_entityGetWorldMatrix = reinterpret_cast<EntityGetWorldMatrix>(
        mapping.entityGetWorldMatrix.address);
    if (handsInterpolationEnabled) {
        g_handsRuntime.Configure({mapping.entityGetWorldMatrix.address,
                                  mapping.entityGetEntityParent.address});
    }
    if (handsPoseInterpolationEnabled) {
        g_poseRuntime.Configure({mapping.meshEntityUpdateGraphics.address});
    }
    g_worldRuntime.SetRelationshipDiagnostics(telemetryEnabled);
    g_renderGatePatch = {mapping.renderUpdateGate.address, false};
    if (clockNeeded) {
        const HMODULE sdl = GetModuleHandleW(L"SDL2.dll");
        g_sdlGetTicks = sdl == nullptr
            ? nullptr
            : reinterpret_cast<SdlGetTicks>(GetProcAddress(sdl, "SDL_GetTicks"));
        if (g_sdlGetTicks == nullptr) {
            error = L"SDL_GetTicks could not be resolved for presentation-clock diagnostics";
            return false;
        }
    }
    if (worldHooksNeeded) {
        const HMODULE newton = GetModuleHandleW(L"Newton.dll");
        g_newtonBodyGetUserData = newton == nullptr
            ? nullptr
            : reinterpret_cast<NewtonBodyGetUserData>(
                  GetProcAddress(newton, "NewtonBodyGetUserData"));
        if (g_newtonBodyGetUserData == nullptr) {
            error = L"NewtonBodyGetUserData could not be resolved for the "
                    L"world-ownership audit";
            return false;
        }
        g_newtonDestroyBodyTarget = reinterpret_cast<void*>(
            GetProcAddress(newton, "NewtonDestroyBody"));
        if (g_newtonDestroyBodyTarget == nullptr) {
            error = L"NewtonDestroyBody could not be resolved for world-body "
                    L"lifetime tracking";
            return false;
        }
    }
    if (cameraInterpolationEnabled) {
        g_cameraRuntime.Configure({mapping.cameraSetPosition.address,
                                   mapping.cameraSetPitch.address,
                                   mapping.cameraSetYaw.address,
                                   mapping.cameraSetRoll.address});
    }
    if (nodeAnimationInterpolationEnabled) {
        g_nodeAnimationRuntime.Reset();
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        error = HookError(L"MH_Initialize", status);
        return false;
    }
    if (imageTrailTimingEnabled) {
        status = MH_CreateHook(
            mapping.imageTrailRenderEffect.address,
            &ImageTrailRenderEffectDetour,
            reinterpret_cast<void**>(&g_originalImageTrailRenderEffect));
        if (status != MH_OK) {
            error = HookError(
                L"MH_CreateHook(cPostEffect_ImageTrail::RenderEffect)", status);
            RollBack(mapping);
            return false;
        }
    }
    if (renderUnlockEnabled || clockNeeded) {
        g_renderUnlockState.store(renderUnlockEnabled ? RenderUnlockState::pending
                                                      : RenderUnlockState::disabled,
                                  std::memory_order_release);
        status = MH_CreateHook(mapping.engineRun.address, &EngineRunDetour,
                               reinterpret_cast<void**>(&g_originalEngineRun));
        if (status != MH_OK) {
            error = HookError(L"MH_CreateHook(cEngine::Run)", status);
            RollBack(mapping);
            return false;
        }
    }
    if (frameHooksNeeded) {
        status = MH_CreateHook(
            mapping.updaterRunMessage.address, &UpdaterRunMessageDetour,
            reinterpret_cast<void**>(&g_originalUpdaterRunMessage));
        if (status != MH_OK) {
            error = HookError(L"MH_CreateHook(cUpdater::RunMessage)", status);
            RollBack(mapping);
            return false;
        }
        status = MH_CreateHook(mapping.sceneRender.address, &SceneRenderDetour,
                               reinterpret_cast<void**>(&g_originalSceneRender));
        if (status != MH_OK) {
            error = HookError(L"MH_CreateHook(cScene::Render)", status);
            RollBack(mapping);
            return false;
        }
    }
    if (handsHooksNeeded) {
        status = MH_CreateHook(
            mapping.playerHandsPostUpdate.address,
            &PlayerHandsPostUpdateDetour,
            reinterpret_cast<void**>(&g_originalPlayerHandsPostUpdate));
        if (status != MH_OK) {
            error = HookError(
                L"MH_CreateHook(cLuxPlayerHands::PostUpdate)", status);
            RollBack(mapping);
            return false;
        }
        status = MH_CreateHook(
            mapping.subMeshGetModelMatrix.address,
            &SubMeshGetModelMatrixDetour,
            reinterpret_cast<void**>(&g_originalSubMeshGetModelMatrix));
        if (status != MH_OK) {
            error = HookError(
                L"MH_CreateHook(cSubMeshEntity::GetModelMatrix)", status);
            RollBack(mapping);
            return false;
        }
    }
    if (meshPoseHookNeeded) {
        status = MH_CreateHook(
            mapping.meshEntityUpdateGraphics.address,
            &MeshEntityUpdateGraphicsDetour,
            reinterpret_cast<void**>(&g_originalMeshEntityUpdateGraphics));
        if (status != MH_OK) {
            error = HookError(
                L"MH_CreateHook(cMeshEntity::UpdateGraphicsForFrame)",
                status);
            RollBack(mapping);
            return false;
        }
    }
    if (ragdollSkinningHookNeeded) {
        status = MH_CreateHook(
            mapping.subMeshUpdateGraphics.address,
            &SubMeshUpdateGraphicsDetour,
            reinterpret_cast<void**>(&g_originalSubMeshUpdateGraphics));
        if (status != MH_OK) {
            error = HookError(
                L"MH_CreateHook(cSubMeshEntity::UpdateGraphicsForFrame)",
                status);
            RollBack(mapping);
            return false;
        }
    }
    if (enemyHooksNeeded) {
        status = MH_CreateHook(
            mapping.luxEnemyOnUpdate.address,
            &LuxEnemyOnUpdateDetour,
            reinterpret_cast<void**>(&g_originalLuxEnemyOnUpdate));
        if (status != MH_OK) {
            error = HookError(L"MH_CreateHook(iLuxEnemy::OnUpdate)", status);
            RollBack(mapping);
            return false;
        }
    }
    if (worldHooksNeeded) {
        status = MH_CreateHook(
            mapping.luxPropSetupAfterLoad.address,
            &LuxPropSetupAfterLoadDetour,
            reinterpret_cast<void**>(&g_originalLuxPropSetupAfterLoad));
        if (status != MH_OK) {
            error = HookError(L"MH_CreateHook(iLuxProp::SetupAfterLoad)",
                              status);
            RollBack(mapping);
            return false;
        }
        status = MH_CreateHook(
            mapping.newtonTransformCallback.address,
            &NewtonTransformCallbackDetour,
            reinterpret_cast<void**>(&g_originalNewtonTransformCallback));
        if (status != MH_OK) {
            error = HookError(
                L"MH_CreateHook(cPhysicsBodyNewton::OnTransformCallback)",
                status);
            RollBack(mapping);
            return false;
        }
    }
    if (worldHooksNeeded) {
        status = MH_CreateHook(
            g_newtonDestroyBodyTarget, &NewtonDestroyBodyDetour,
            reinterpret_cast<void**>(&g_originalNewtonDestroyBody));
        if (status != MH_OK) {
            error = HookError(L"MH_CreateHook(NewtonDestroyBody)", status);
            RollBack(mapping);
            return false;
        }
    }
    if (mouseMapping != nullptr) {
        // Failure here leaves only gameplay mouse look stock.
        std::wstring mouseError;
        if (!cameraInterpolationEnabled) {
            log::Write(log::Level::warning,
                       L"MouseOverlay requires CameraInterpolation; gameplay "
                       L"mouse look stays at the fixed-update rate.");
        } else if (!mouse_look::Install(
                       {*mouseMapping, &g_tickSerial, &g_cameraRuntime},
                       mouseError)) {
            log::Write(log::Level::warning,
                       L"Mouse look disabled: " + mouseError + L".");
        } else {
            g_mouseLookEnabled = true;
        }
    }
    if (particleMapping != nullptr) {
        // Failure here leaves only particles at the fixed-update rate.
        void* const target =
            particleMapping->emitterUpdateGraphicsForViewport.address;
        if (target == nullptr ||
            MH_CreateHook(target, &ParticleEmitterUpdateGraphicsDetour,
                          reinterpret_cast<void**>(
                              &g_originalParticleEmitterUpdateGraphics)) !=
                MH_OK) {
            log::Write(log::Level::warning,
                       L"Particle interpolation disabled: iParticleEmitter::"
                       L"UpdateGraphicsForViewport could not be hooked.");
        } else if (MH_QueueEnableHook(target) != MH_OK) {
            MH_RemoveHook(target);
            log::Write(log::Level::warning,
                       L"Particle interpolation disabled: the emitter hook "
                       L"could not be queued.");
        } else {
            g_particleInterpolationEnabled = true;
        }
    }
    if (renderUnlockEnabled || clockNeeded) {
        status = MH_QueueEnableHook(mapping.engineRun.address);
    }
    if (status == MH_OK && frameHooksNeeded) {
        status = MH_QueueEnableHook(mapping.updaterRunMessage.address);
    }
    if (status == MH_OK && frameHooksNeeded) {
        status = MH_QueueEnableHook(mapping.sceneRender.address);
    }
    if (status == MH_OK && handsHooksNeeded) {
        status = MH_QueueEnableHook(mapping.playerHandsPostUpdate.address);
    }
    if (status == MH_OK && handsHooksNeeded) {
        status = MH_QueueEnableHook(mapping.subMeshGetModelMatrix.address);
    }
    if (status == MH_OK && meshPoseHookNeeded) {
        status = MH_QueueEnableHook(mapping.meshEntityUpdateGraphics.address);
    }
    if (status == MH_OK && ragdollSkinningHookNeeded) {
        status = MH_QueueEnableHook(mapping.subMeshUpdateGraphics.address);
    }
    if (status == MH_OK && enemyHooksNeeded) {
        status = MH_QueueEnableHook(mapping.luxEnemyOnUpdate.address);
    }
    if (status == MH_OK && worldHooksNeeded) {
        status = MH_QueueEnableHook(mapping.luxPropSetupAfterLoad.address);
    }
    if (status == MH_OK && worldHooksNeeded) {
        status = MH_QueueEnableHook(mapping.newtonTransformCallback.address);
    }
    if (status == MH_OK && worldHooksNeeded) {
        status = MH_QueueEnableHook(g_newtonDestroyBodyTarget);
    }
    if (status == MH_OK && imageTrailTimingEnabled) {
        status = MH_QueueEnableHook(mapping.imageTrailRenderEffect.address);
    }
    if (status == MH_OK) {
        status = MH_ApplyQueued();
    }
    if (status != MH_OK) {
        error = HookError(L"Enabling telemetry hooks", status);
        RollBack(mapping);
        return false;
    }

    if (telemetryEnabled) {
        const HANDLE reporter =
            CreateThread(nullptr, 0, &ReporterThread, nullptr, 0, nullptr);
        if (reporter == nullptr) {
            error = L"CreateThread(telemetry reporter) failed with Win32 error " +
                    std::to_wstring(GetLastError());
            RollBack(mapping);
            return false;
        }
        CloseHandle(reporter);
        log::Write(log::Level::info,
                   L"Diagnostics-only hooks enabled for cUpdater::RunMessage and "
                   L"cScene::Render, plus the read-only first-person hands "
                   L"ownership/layout and submesh-renderer audits, and the "
                   L"event-driven Newton motion, prop-ownership, and enemy "
                   L"skeletal-palette audits, plus the read-only generic "
                   L"skeleton-physics ragdoll audit.");
        if (poseAuditHookNeeded) {
            log::Write(log::Level::info,
                       L"Read-only first-person final skeletal-palette audit "
                       L"enabled.");
        }
    }
    if (ragdollProfileEnabled) {
        const HANDLE profiler =
            CreateThread(nullptr, 0, &RagdollProfileThread, nullptr, 0,
                         nullptr);
        if (profiler == nullptr) {
            error = L"CreateThread(ragdoll profiler) failed with Win32 error " +
                    std::to_wstring(GetLastError());
            RollBack(mapping);
            return false;
        }
        CloseHandle(profiler);
        log::Write(log::Level::info,
                   L"Narrow ragdoll reskin timing probe enabled; broad "
                   L"telemetry remains disabled.");
    }
    if (enemyPoseInterpolationEnabled) {
        log::Write(log::Level::info,
                   L"Scoped ManPig and Child final-palette interpolation "
                   L"enabled; other enemy archetypes remain diagnostic-only.");
    }
    if (ragdollPoseInterpolationEnabled) {
        log::Write(log::Level::info,
                   L"Scoped skeleton-physics final-palette interpolation "
                   L"and per-render dynamic vertex reskinning enabled for "
                   L"generic ragdoll meshes.");
    }
    if (nodeAnimationInterpolationEnabled) {
        log::Write(log::Level::info,
                   L"Render-only interpolation enabled for non-skeletal "
                   L"animated-node submeshes; animation time and events are "
                   L"unchanged.");
    }
    if (g_mouseLookEnabled) {
        log::Write(log::Level::info,
                   L"Presentation-rate gameplay mouse look enabled: pending "
                   L"relative motion is previewed on the render camera and "
                   L"delivered to the next fixed update exactly once. Smoothed, "
                   L"sluggish, and constrained look modes stay engine-owned.");
    }
    if (g_particleInterpolationEnabled) {
        log::Write(log::Level::info,
                   L"Render-only particle interpolation enabled: emitters that "
                   L"advanced in the newest fixed update are drawn between "
                   L"their last two positions; emission, motion, collision, "
                   L"and lifetime stay fixed-step.");
    }
    if (imageTrailTimingEnabled) {
        log::Write(log::Level::info,
                   L"Stock-60-Hz-equivalent image-trail accumulation timing "
                   L"enabled at cPostEffect_ImageTrail::RenderEffect RVA "
                   L"0x001EA980.");
    }
    log::Write(log::Level::info,
                (cameraInterpolationEnabled || handsInterpolationEnabled ||
                 handsPoseInterpolationEnabled ||
                 enemyPoseInterpolationEnabled ||
                 ragdollPoseInterpolationEnabled ||
                 nodeAnimationInterpolationEnabled ||
                 worldInterpolationEnabled || imageTrailTimingEnabled)
                   ? L"Runtime hooks enabled; RenderUnlock and scoped camera/hands/"
                     L"enemy-pose/validated-object interpolation and temporal-"
                     L"effect timing may modify presentation state."
                   : L"Runtime hooks enabled; only RenderUnlock may modify engine state.");
    return true;
}

} // namespace amfp::telemetry
