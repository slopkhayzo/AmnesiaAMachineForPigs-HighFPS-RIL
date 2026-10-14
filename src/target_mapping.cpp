#include "target_mapping.h"

#include "signature_scan.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace amfp::target {
namespace {

using signature::Pattern;

// GOG build 51194033869387641, aamfp.exe SHA-256
// 9FAE927B3B37D37C7E087548D895581A3AF2EC541019F4C2A81D8392A5C8BE8B.
// Absolute addresses and relative call displacements are wildcarded.
constexpr std::array<std::uint8_t, 48> kEngineRunBytes{
    0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x00, 0x00,
    0x00, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,
    0x50, 0x83, 0xEC, 0x44, 0xA1, 0x00, 0x00, 0x00,
    0x00, 0x33, 0xC5, 0x89, 0x45, 0xF0, 0x53, 0x56,
    0x57, 0x50, 0x8D, 0x45, 0xF4, 0x64, 0xA3, 0x00,
    0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x00, 0x00};
// AMFP changed TDD's `if (!mbLimitFPS || bIsUpdated)` render condition to
// `if (bIsUpdated)`. This signature begins at the local boolean comparison;
// the patch site is the near JZ four bytes into the match.
constexpr std::array<std::uint8_t, 20> kRenderUpdateGateBytes{
    0x80, 0x7D, 0xD3, 0x00, 0x0F, 0x84, 0x00, 0x00,
    0x00, 0x00, 0x8B, 0x4E, 0x70, 0x8B, 0x11, 0x8B,
    0x42, 0x08, 0xFF, 0xD0};
constexpr std::array<std::uint8_t, 24> kUpdaterRunMessageBytes{
    0x55, 0x8B, 0xEC, 0x51, 0x53, 0x8B, 0x5D, 0x08,
    0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x34, 0x8B,
    0x30, 0x83, 0xFB, 0x05, 0x74, 0x67, 0x3B, 0xF0};
constexpr std::array<std::uint8_t, 24> kSceneRenderBytes{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0xFF, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x8B, 0x41, 0x44, 0x89,
    0x4D, 0xFC, 0x8B, 0x08, 0x89, 0x4D, 0xF0, 0x3B};
constexpr std::array<std::uint8_t, 24> kCameraSetPositionBytes{
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0xD9, 0x00,
    0x51, 0xD9, 0x19, 0x81, 0xC1, 0xCC, 0x01, 0x00,
    0x00, 0xD9, 0x40, 0x04, 0xD9, 0x99, 0x38, 0xFE};
constexpr std::array<std::uint8_t, 24> kCameraSetPitchBytes{
    0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x08, 0xD9, 0x51,
    0x28, 0xD9, 0x41, 0x34, 0xD9, 0xEE, 0xD9, 0xC0,
    0xDD, 0xEA, 0xDF, 0xE0, 0xDD, 0xD9, 0xF6, 0xC4};
constexpr std::array<std::uint8_t, 24> kCameraSetYawBytes{
    0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x08, 0xD9, 0x51,
    0x2C, 0xD9, 0x41, 0x3C, 0xD9, 0xEE, 0xD9, 0xC0,
    0xDD, 0xEA, 0xDF, 0xE0, 0xDD, 0xD9, 0xF6, 0xC4};
constexpr std::array<std::uint8_t, 24> kCameraSetRollBytes{
    0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x08, 0xB0, 0x01,
    0xD9, 0x59, 0x30, 0x88, 0x81, 0x51, 0x08, 0x00,
    0x00, 0x66, 0xC7, 0x81, 0x53, 0x08, 0x00, 0x00};
constexpr std::array<std::uint8_t, 19> kPlayerHandsPostUpdateBytes{
    0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x08, 0x51, 0xD9,
    0x1C, 0x24, 0xE8, 0x51, 0xFC, 0xFF, 0xFF, 0x5D,
    0xC2, 0x04, 0x00};
constexpr std::array<std::uint8_t, 32> kLuxEnemyOnUpdateBytes{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x56, 0x8B,
    0xF1, 0x80, 0xBE, 0x0C, 0x01, 0x00, 0x00, 0x00,
    0x0F, 0x85, 0x00, 0x00, 0x00, 0x00, 0xA1, 0x00,
    0x00, 0x00, 0x00, 0xD9, 0x45, 0x08, 0x8B, 0x08};
constexpr std::array<std::uint8_t, 29> kLuxPropOnUpdateBytes{
    0x55, 0x8B, 0xEC, 0x51, 0xD9, 0x45, 0x08, 0x56,
    0x57, 0x51, 0xD9, 0x1C, 0x24, 0x8B, 0xF1, 0xE8,
    0x00, 0x00, 0x00, 0x00, 0xD9, 0x45, 0x08, 0x51,
    0x8B, 0xCE, 0xD9, 0x1C, 0x24};
constexpr std::array<std::uint8_t, 48> kLuxPropSetupAfterLoadBytes{
    0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x00, 0x00,
    0x00, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,
    0x50, 0x83, 0xEC, 0x48, 0xA1, 0x00, 0x00, 0x00,
    0x00, 0x33, 0xC5, 0x89, 0x45, 0xF0, 0x53, 0x56,
    0x57, 0x50, 0x8D, 0x45, 0xF4, 0x64, 0xA3, 0x00,
    0x00, 0x00, 0x00, 0x8B, 0x45, 0x08, 0x8B, 0xD9};
constexpr std::array<std::uint8_t, 32> kNewtonTransformCallbackBytes{
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x56, 0x50,
    0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x4D,
    0x0C, 0x83, 0xC4, 0x04, 0x8B, 0xF0, 0x51, 0x8D,
    0x4E, 0x2C, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x6A};
// The following accessor after GetWorldMatrix distinguishes iEntity3D from
// cNode3D without fixing the relative UpdateWorldTransform call displacement.
constexpr std::array<std::uint8_t, 23> kEntityGetWorldMatrixBytes{
    0x56, 0x8B, 0xF1, 0xE8, 0x00, 0x00, 0x00, 0x00,
    0x8D, 0x46, 0x6C, 0x5E, 0xC3, 0xCC, 0xCC, 0xCC,
    0x8B, 0x81, 0x50, 0x02, 0x00, 0x00, 0xC3};
constexpr std::array<std::uint8_t, 7> kEntityGetEntityParentBytes{
    0x8B, 0x81, 0x88, 0x02, 0x00, 0x00, 0xC3};
constexpr std::array<std::uint8_t, 24> kSubMeshGetModelMatrixBytes{
    0x56, 0x8B, 0xF1, 0x80, 0xBE, 0x3D, 0x03, 0x00,
    0x00, 0x00, 0x74, 0x38, 0x8A, 0x86, 0xA8, 0x03,
    0x00, 0x00, 0x84, 0xC0, 0x78, 0x2E, 0x75, 0x24};
constexpr std::array<std::uint8_t, 24> kSubMeshUpdateGraphicsBytes{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30, 0xD9, 0x45,
    0x08, 0x57, 0x8B, 0xF9, 0x51, 0x8B, 0x8F, 0x74,
    0x03, 0x00, 0x00, 0xD9, 0x1C, 0x24, 0x89, 0x7D};
constexpr std::array<std::uint8_t, 24> kMeshEntityUpdateGraphicsBytes{
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x8C, 0x00, 0x00,
    0x00, 0x53, 0x57, 0x8B, 0xF9, 0x33, 0xDB, 0x89,
    0x7D, 0xF4, 0x38, 0x9F, 0xC0, 0x02, 0x00, 0x00};
constexpr std::array<std::uint8_t, 32> kImageTrailRenderEffectBytes{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0xD9, 0xE8,
    0x56, 0xD9, 0x55, 0xF8, 0x57, 0xD9, 0x5D, 0xFC,
    0x83, 0xEC, 0x08, 0xD9, 0x05, 0x00, 0x00, 0x00,
    0x00, 0x8B, 0xF1, 0x8B, 0x4E, 0x18, 0xD9, 0x5C};
constexpr std::array<std::uint8_t, 22> kLogicResetBytes{
    0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1, 0xE8,
    0x00, 0x00, 0x00, 0x00, 0x89, 0x45, 0xFC, 0xDB,
    0x45, 0xFC, 0x85, 0xC0, 0x79, 0x06};
constexpr std::array<std::uint8_t, 24> kLogicEndUpdateLoopBytes{
    0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1, 0x8B,
    0x46, 0x14, 0x3B, 0x46, 0x10, 0x7E, 0x17, 0xE8,
    0x00, 0x00, 0x00, 0x00, 0x89, 0x45, 0xFC, 0xDB};
constexpr std::array<std::uint8_t, 24> kLogicGetStepSizeBytes{
    0x55, 0x8B, 0xEC, 0x51, 0xDD, 0x41, 0x08, 0xD9,
    0x5D, 0xFC, 0xD9, 0x45, 0xFC, 0xDC, 0x35, 0x00,
    0x00, 0x00, 0x00, 0xD9, 0x5D, 0xFC, 0xD9, 0x45};
constexpr std::array<std::uint8_t, 24> kLogicWantUpdateBytes{
    0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1, 0xFF,
    0x46, 0x14, 0x8B, 0x46, 0x14, 0x3B, 0x46, 0x10,
    0x7F, 0x2C, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x89};

// Mouse-look path. Call displacements and relocated addresses are wildcarded;
// the remaining bytes pin every structure offset the mouse runtime reads.
// Retail RVA 0x002113B0.
constexpr std::array<std::uint8_t, 48> kMouseSdlUpdateBytes{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24, 0x53, 0x56,
    0x8B, 0xF1, 0x33, 0xDB, 0x57, 0x38, 0x5E, 0x24,
    0x74, 0x1E, 0x8D, 0x7B, 0x0A, 0xE8, 0x00, 0x00,
    0x00, 0x00, 0x8D, 0x45, 0xF0, 0x50, 0x8D, 0x4D,
    0xFC, 0x51, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x83,
    0xC4, 0x08, 0x4F, 0x75, 0xE8, 0x88, 0x5E, 0x24};
constexpr std::string_view kMouseSdlUpdateMask =
    "xxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxx????xxxxxxxxx";
// Retail RVA 0x00211360.
constexpr std::array<std::uint8_t, 29> kMouseSdlGetRelPositionBytes{
    0x55, 0x8B, 0xEC, 0x8B, 0x51, 0x30, 0x8B, 0x45,
    0x08, 0x89, 0x10, 0x8B, 0x51, 0x34, 0x89, 0x50,
    0x04, 0x33, 0xD2, 0x89, 0x51, 0x30, 0x89, 0x51,
    0x34, 0x5D, 0xC2, 0x04, 0x00};
constexpr std::string_view kMouseSdlGetRelPositionMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
// Retail RVA 0x001B0710, iRenderable vtable slot 12 of iParticleEmitter. The
// stack-cookie address is wildcarded; mpVtxBuffer +0x40C is pinned.
constexpr std::array<std::uint8_t, 32> kParticleEmitterUpdateGraphicsBytes{
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x1C, 0x02, 0x00,
    0x00, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC5,
    0x89, 0x45, 0xFC, 0x8B, 0x45, 0x08, 0x53, 0x56,
    0x8B, 0xF1, 0x8B, 0x8E, 0x0C, 0x04, 0x00, 0x00};
constexpr std::string_view kParticleEmitterUpdateGraphicsMask =
    "xxxxxxxxxx????xxxxxxxxxxxxxxxxxx";
// Retail RVA 0x001B2330, the whole of iParticleEmitter::CreateParticle. Pins
// mvParticles +0x3A0, mlNumOfParticles +0x3B0, and mlMaxParticles +0x3B4.
constexpr std::array<std::uint8_t, 35> kParticleEmitterCreateParticleBytes{
    0x8B, 0x81, 0xB0, 0x03, 0x00, 0x00, 0x3B, 0x81,
    0xB4, 0x03, 0x00, 0x00, 0x75, 0x03, 0x33, 0xC0,
    0xC3, 0x40, 0x89, 0x81, 0xB0, 0x03, 0x00, 0x00,
    0x8B, 0x89, 0xA0, 0x03, 0x00, 0x00, 0x8B, 0x44,
    0x81, 0xFC, 0xC3};
constexpr std::string_view kParticleEmitterCreateParticleMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
// Retail RVA 0x0010D8D0, cLuxPlayerCamDirEffects::AddAndGetYawAdd up to its
// first return. Pins the saved-input list size +0x40, mvSwayAdd +0x48,
// mfSwayAlpha +0x54, and mvNextAdd +0x58.
constexpr std::array<std::uint8_t, 57> kCamDirEffectsYawAddBytes{
    0x55, 0x8B, 0xEC, 0x83, 0x79, 0x40, 0x00, 0xD9,
    0x45, 0x08, 0xD9, 0x55, 0x08, 0x74, 0x14, 0xD9,
    0x41, 0x54, 0xD9, 0xE8, 0xDE, 0xE1, 0xD8, 0xC9,
    0xD9, 0x41, 0x48, 0xD8, 0x49, 0x54, 0xDE, 0xC1,
    0xD9, 0x5D, 0x08, 0xD9, 0xEE, 0xD8, 0x59, 0x54,
    0xDF, 0xE0, 0xF6, 0xC4, 0x05, 0x7A, 0x0A, 0xD9,
    0x59, 0x58, 0xD9, 0x45, 0x08, 0x5D, 0xC2, 0x04,
    0x00};
constexpr std::string_view kCamDirEffectsYawAddMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
// Retail RVA 0x00164650. Pins mpSetInFocus +0x34, mpGraphics +0x24, and the
// two-argument stack cleanup.
constexpr std::array<std::uint8_t, 40> kGuiSendMousePosBytes{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x5C, 0x56, 0x8B,
    0xF1, 0x83, 0x7E, 0x34, 0x00, 0x75, 0x09, 0x32,
    0xC0, 0x5E, 0x8B, 0xE5, 0x5D, 0xC2, 0x08, 0x00,
    0x8B, 0x46, 0x24, 0x8B, 0x48, 0x20, 0x8B, 0x45,
    0x08, 0xDB, 0x00, 0x8B, 0x11, 0x8B, 0x52, 0x2C};
constexpr std::string_view kGuiSendMousePosMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
// Retail RVA 0x0009F4B0.
constexpr std::array<std::uint8_t, 32> kLuxUpdateGamePlayerInputBytes{
    0x55, 0x8B, 0xEC, 0xD9, 0xEE, 0x83, 0xEC, 0x30,
    0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x38, 0xD8, 0x99,
    0x88, 0x00, 0x00, 0x00, 0x57, 0xDF, 0xE0, 0xF6,
    0xC4, 0x01, 0x75, 0x5E, 0x8B, 0x4E, 0x34, 0x6A};
constexpr std::string_view kLuxUpdateGamePlayerInputMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
// Retail RVA 0x0009F93D.
constexpr std::array<std::uint8_t, 362> kLuxMouseLookBlockBytes{
    0x8B, 0x4E, 0x34, 0xE8, 0x00, 0x00, 0x00, 0x00,
    0x8B, 0x10, 0x8B, 0x52, 0x10, 0x8D, 0x4D, 0xD0,
    0x51, 0x8B, 0xC8, 0xFF, 0xD2, 0x8B, 0x4E, 0x38,
    0xE8, 0x00, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74,
    0x05, 0xD9, 0x46, 0x68, 0xEB, 0x02, 0xD9, 0xE8,
    0xD9, 0x5D, 0xFC, 0x83, 0xEC, 0x08, 0xDB, 0x45,
    0xD0, 0xD9, 0x5D, 0xE0, 0xDB, 0x45, 0xD4, 0xD9,
    0x5D, 0xE4, 0xD9, 0x45, 0xE0, 0xD8, 0x4E, 0x50,
    0xD9, 0x5D, 0xE8, 0xD9, 0x45, 0xE4, 0xD8, 0x4E,
    0x50, 0xD9, 0x5D, 0xEC, 0xD9, 0x86, 0x94, 0x00,
    0x00, 0x00, 0xD8, 0x4D, 0xE8, 0xD9, 0x5D, 0xE0,
    0xD9, 0x86, 0x94, 0x00, 0x00, 0x00, 0xD8, 0x4D,
    0xEC, 0xD9, 0x5D, 0xE4, 0xD9, 0x45, 0xE0, 0xD9,
    0x45, 0xFC, 0xD9, 0xC0, 0xDE, 0xCA, 0xD9, 0xC9,
    0xD9, 0x5D, 0xE8, 0xD8, 0x4D, 0xE4, 0xD9, 0x5D,
    0xEC, 0xD9, 0x86, 0x88, 0x00, 0x00, 0x00, 0xD8,
    0x4D, 0xE8, 0xD9, 0x5D, 0xFC, 0xD9, 0x45, 0xFC,
    0xDC, 0x05, 0x00, 0x00, 0x00, 0x00, 0xDD, 0x1C,
    0x24, 0xE8, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x00,
    0x00, 0x00, 0x00, 0xD9, 0x86, 0x88, 0x00, 0x00,
    0x00, 0xD8, 0x4D, 0xEC, 0x89, 0x45, 0xE0, 0xD9,
    0x5D, 0xFC, 0xD9, 0x45, 0xFC, 0xDC, 0x05, 0x00,
    0x00, 0x00, 0x00, 0xDD, 0x1C, 0x24, 0xE8, 0x00,
    0x00, 0x00, 0x00, 0x83, 0xC4, 0x08, 0xE8, 0x00,
    0x00, 0x00, 0x00, 0x89, 0x45, 0xE4, 0x8B, 0x46,
    0x30, 0x8B, 0x48, 0x20, 0x8B, 0x11, 0x8B, 0x52,
    0x2C, 0x8D, 0x45, 0xD8, 0x50, 0xFF, 0xD2, 0xD9,
    0x40, 0x04, 0xDC, 0x0D, 0x00, 0x00, 0x00, 0x00,
    0x8B, 0x4E, 0x38, 0xD9, 0x5D, 0xFC, 0xDB, 0x45,
    0xE0, 0xD9, 0x5D, 0xE8, 0xDB, 0x45, 0xE4, 0xD9,
    0x5D, 0xEC, 0xD9, 0x45, 0xE8, 0xD9, 0x45, 0xFC,
    0xD9, 0xC0, 0xDE, 0xFA, 0xD9, 0xC9, 0xD9, 0x5D,
    0xE0, 0xD8, 0x7D, 0xEC, 0xD9, 0x5D, 0xE4, 0xE8,
    0x00, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x19,
    0x8D, 0x45, 0xE0, 0x50, 0x8D, 0x4D, 0xD8, 0x51,
    0x8B, 0xCE, 0xE8, 0x00, 0x00, 0x00, 0x00, 0xD9,
    0x00, 0xD9, 0x5D, 0xE8, 0xD9, 0x40, 0x04, 0xEB,
    0x30, 0x8D, 0x4E, 0x74, 0xE8, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x7E, 0x40, 0x00, 0x74, 0x19, 0x8D,
    0x55, 0xE0, 0x52, 0x8D, 0x45, 0xD8, 0x50, 0x8B,
    0xCE, 0xE8, 0x00, 0x00, 0x00, 0x00, 0xD9, 0x00,
    0xD9, 0x5D, 0xE8, 0xD9, 0x40, 0x04, 0xEB, 0x09,
    0xD9, 0x45, 0xE0, 0xD9, 0x5D, 0xE8, 0xD9, 0x45,
    0xE4, 0x80, 0x7E, 0x41, 0x00, 0xD9, 0x5D, 0xEC,
    0x74, 0x08, 0xD9, 0x45, 0xEC, 0xD9, 0xE0, 0xD9,
    0x5D, 0xEC};
constexpr std::string_view kLuxMouseLookBlockMask =
    "xxxx????xxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xx"
    "xx????x????xxxxxxxxxxxxxxxxxxxx????xxxx????xxxx?"
    "???xxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxx????x"
    "xxxxxxxxxxxxx????xxxxxxxxxxxxxxxxx????xxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxx";
// Retail RVA 0x00105D00.
constexpr std::array<std::uint8_t, 119> kLuxPlayerAddYawBytes{
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xD9, 0x86,
    0xCC, 0x00, 0x00, 0x00, 0x8B, 0x46, 0x7C, 0xD8,
    0x4D, 0x08, 0x8B, 0x8E, 0x18, 0x03, 0x00, 0x00,
    0x8B, 0x0C, 0x81, 0x8B, 0x11, 0x8B, 0x42, 0x2C,
    0xD9, 0x5D, 0x08, 0xD9, 0x45, 0x08, 0x51, 0xD9,
    0x1C, 0x24, 0xFF, 0xD0, 0x84, 0xC0, 0x74, 0x59,
    0x8B, 0x96, 0x08, 0x03, 0x00, 0x00, 0xD9, 0x45,
    0x08, 0x8B, 0x8E, 0x80, 0x00, 0x00, 0x00, 0x8B,
    0x0C, 0x8A, 0x8B, 0x01, 0x8B, 0x50, 0x18, 0x51,
    0xD9, 0x1C, 0x24, 0xFF, 0xD2, 0x84, 0xC0, 0x74,
    0x38, 0xD9, 0x45, 0x08, 0x51, 0x8B, 0x8E, 0xE4,
    0x02, 0x00, 0x00, 0xDC, 0xC0, 0xD9, 0x5D, 0x08,
    0xD9, 0x45, 0x08, 0xD9, 0x1C, 0x24, 0xE8, 0x00,
    0x00, 0x00, 0x00, 0x51, 0x8B, 0x4E, 0x50, 0xD9,
    0x1C, 0x24, 0xE8, 0x00, 0x00, 0x00, 0x00};
constexpr std::string_view kLuxPlayerAddYawMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxx????xxxxxxxx????";
// Retail RVA 0x00105D90.
constexpr std::array<std::uint8_t, 119> kLuxPlayerAddPitchBytes{
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xD9, 0x86,
    0xCC, 0x00, 0x00, 0x00, 0x8B, 0x46, 0x7C, 0xD8,
    0x4D, 0x08, 0x8B, 0x8E, 0x18, 0x03, 0x00, 0x00,
    0x8B, 0x0C, 0x81, 0x8B, 0x11, 0x8B, 0x42, 0x30,
    0xD9, 0x5D, 0x08, 0xD9, 0x45, 0x08, 0x51, 0xD9,
    0x1C, 0x24, 0xFF, 0xD0, 0x84, 0xC0, 0x74, 0x47,
    0x8B, 0x96, 0x08, 0x03, 0x00, 0x00, 0xD9, 0x45,
    0x08, 0x8B, 0x8E, 0x80, 0x00, 0x00, 0x00, 0x8B,
    0x0C, 0x8A, 0x8B, 0x01, 0x8B, 0x50, 0x1C, 0x51,
    0xD9, 0x1C, 0x24, 0xFF, 0xD2, 0x84, 0xC0, 0x74,
    0x26, 0xD9, 0x45, 0x08, 0x51, 0x8B, 0x8E, 0xE4,
    0x02, 0x00, 0x00, 0xDC, 0xC0, 0xD9, 0x5D, 0x08,
    0xD9, 0x45, 0x08, 0xD9, 0x1C, 0x24, 0xE8, 0x00,
    0x00, 0x00, 0x00, 0x51, 0x8B, 0x4E, 0x50, 0xD9,
    0x1C, 0x24, 0xE8, 0x00, 0x00, 0x00, 0x00};
constexpr std::string_view kLuxPlayerAddPitchMask =
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxx????xxxxxxxx????";
// Retail RVA 0x00106290.
constexpr std::array<std::uint8_t, 37> kLuxPlayerIsAtMaxInfectionLevelBytes{
    0xD9, 0x05, 0x00, 0x00, 0x00, 0x00, 0x56, 0x8B,
    0xF1, 0xD8, 0x9E, 0x8C, 0x00, 0x00, 0x00, 0xDF,
    0xE0, 0xF6, 0xC4, 0x41, 0x75, 0x0F, 0x33, 0xC0,
    0x33, 0xC9, 0x3B, 0x86, 0x84, 0x00, 0x00, 0x00,
    0x5E, 0x0F, 0x94, 0xC0, 0xC3};
constexpr std::string_view kLuxPlayerIsAtMaxInfectionLevelMask =
    "xx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

bool RangeIsInImage(const std::size_t imageSize,
                    const std::size_t offset,
                    const std::size_t size) noexcept {
    return offset <= imageSize && size <= imageSize - offset;
}

bool ResolveOne(const std::uint8_t* imageBase,
                const std::span<const std::uint8_t> text,
                const std::uint32_t textRva,
                const Pattern pattern,
                const wchar_t* name,
                Function& output,
                std::wstring& error) {
    const signature::FindResult found = signature::FindUnique(text, pattern);
    if (found.status != signature::FindStatus::unique) {
        error = std::wstring(name) + L": expected exactly one .text signature match";
        if (found.status == signature::FindStatus::invalidPattern) {
            error += L" (internal pattern is invalid)";
        } else {
            error += L", found " + std::to_wstring(found.matchCount);
            if (found.status == signature::FindStatus::ambiguous) {
                error += L" or more";
            }
        }
        return false;
    }

    const std::size_t rva = static_cast<std::size_t>(textRva) + found.offset;
    if (rva > UINT32_MAX) {
        error = std::wstring(name) + L": resolved RVA overflowed 32 bits";
        return false;
    }
    output.rva = static_cast<std::uint32_t>(rva);
    output.address = const_cast<std::uint8_t*>(imageBase + rva);
    return true;
}

bool ResolveAdjusted(const std::uint8_t* imageBase,
                     const std::span<const std::uint8_t> text,
                     const std::uint32_t textRva,
                     const Pattern pattern,
                     const std::uint32_t adjustment,
                     const wchar_t* name,
                     Function& output,
                     std::wstring& error) {
    Function match{};
    if (!ResolveOne(imageBase, text, textRva, pattern, name, match, error)) {
        return false;
    }
    output.rva = match.rva + adjustment;
    output.address = static_cast<std::uint8_t*>(match.address) + adjustment;
    return true;
}

bool LocateText(const HMODULE executable,
                const std::uint8_t*& imageBase,
                std::span<const std::uint8_t>& text,
                std::uint32_t& textRva,
                std::wstring& error) {
    if (executable == nullptr) {
        error = L"Main executable image is unavailable";
        return false;
    }

    imageBase = reinterpret_cast<const std::uint8_t*>(executable);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 1024 * 1024) {
        error = L"Main executable has an invalid DOS header";
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        imageBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        error = L"Main executable is not a valid PE32/x86 image";
        return false;
    }

    const std::size_t imageSize = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    const IMAGE_SECTION_HEADER* textSection = nullptr;
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        if (std::memcmp(sections[index].Name, ".text", 5) == 0) {
            textSection = &sections[index];
            break;
        }
    }
    if (textSection == nullptr ||
        (textSection->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
        !RangeIsInImage(imageSize, textSection->VirtualAddress,
                        textSection->Misc.VirtualSize)) {
        error = L"Main executable has no valid executable .text section";
        return false;
    }

    text = std::span<const std::uint8_t>(
        imageBase + textSection->VirtualAddress, textSection->Misc.VirtualSize);
    textRva = textSection->VirtualAddress;
    return true;
}

// Returns the absolute target of the E8 call at `callRva`.
const std::uint8_t* CallTarget(const std::uint8_t* imageBase,
                               const std::uint32_t callRva) noexcept {
    std::int32_t displacement = 0;
    std::memcpy(&displacement, imageBase + callRva + 1, sizeof(displacement));
    return imageBase + callRva + 5 + displacement;
}

} // namespace

bool ResolveCoreImage(const HMODULE executable,
                      CoreMapping& mapping,
                      std::wstring& error) {
    mapping = {};
    const std::uint8_t* imageBase = nullptr;
    std::span<const std::uint8_t> text;
    std::uint32_t textRva = 0;
    if (!LocateText(executable, imageBase, text, textRva, error)) {
        return false;
    }

    return ResolveOne(imageBase, text, textRva,
                      Pattern{kEngineRunBytes,
                              "xxxxxx????xxxxxxxxxxx????xxxxxxxxxxxxxxxxxxx????"},
                      L"cEngine::Run", mapping.engineRun, error) &&
           ResolveAdjusted(
               imageBase, text, textRva,
               Pattern{kRenderUpdateGateBytes, "xxxxxx????xxxxxxxxxx"}, 4,
               L"cEngine::Run render/update gate", mapping.renderUpdateGate,
               error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kUpdaterRunMessageBytes, "xxxxxxxxxxxxxxxxxxxxxxxx"},
                      L"cUpdater::RunMessage", mapping.updaterRunMessage, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kSceneRenderBytes, "xxxxxxxx????xxxxxxxxxxxx"},
                      L"cScene::Render", mapping.sceneRender, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kCameraSetPositionBytes,
                              "xxxxxxxxxxxxxxxxxxxxxxxx"},
                      L"cCamera::SetPosition", mapping.cameraSetPosition,
                      error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kCameraSetPitchBytes,
                              "xxxxxxxxxxxxxxxxxxxxxxxx"},
                      L"cCamera::SetPitch", mapping.cameraSetPitch, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kCameraSetYawBytes,
                              "xxxxxxxxxxxxxxxxxxxxxxxx"},
                      L"cCamera::SetYaw", mapping.cameraSetYaw, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kCameraSetRollBytes,
                              "xxxxxxxxxxxxxxxxxxxxxxxx"},
                      L"cCamera::SetRoll", mapping.cameraSetRoll, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kPlayerHandsPostUpdateBytes,
                              "xxxxxxxxxxxxxxxxxxx"},
                      L"cLuxPlayerHands::PostUpdate",
                      mapping.playerHandsPostUpdate, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kLuxEnemyOnUpdateBytes,
                              "xxxxxxxxxxxxxxxxxx????x????xxxxx"},
                      L"iLuxEnemy::OnUpdate", mapping.luxEnemyOnUpdate,
                      error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kLuxPropOnUpdateBytes,
                              "xxxxxxxxxxxxxxxx????xxxxxxxxx"},
                      L"iLuxProp::OnUpdate", mapping.luxPropOnUpdate,
                      error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kLuxPropSetupAfterLoadBytes,
                              "xxxxxx????xxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxx"},
                      L"iLuxProp::SetupAfterLoad",
                      mapping.luxPropSetupAfterLoad, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kNewtonTransformCallbackBytes,
                              "xxxxxxxxxx????xxxxxxxxxxxxx????x"},
                      L"cPhysicsBodyNewton::OnTransformCallback",
                      mapping.newtonTransformCallback, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kEntityGetWorldMatrixBytes,
                              "xxxx????xxxxxxxxxxxxxxx"},
                      L"iEntity3D::GetWorldMatrix",
                      mapping.entityGetWorldMatrix, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kEntityGetEntityParentBytes, "xxxxxxx"},
                      L"iEntity3D::GetEntityParent",
                      mapping.entityGetEntityParent, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kSubMeshGetModelMatrixBytes,
                              "xxxxxxxxxxxxxxxxxxxxxxxx"},
                      L"cSubMeshEntity::GetModelMatrix",
                      mapping.subMeshGetModelMatrix, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kSubMeshUpdateGraphicsBytes,
                              "xxxxxxxxxxxxxxxxxxxxxxxx"},
                      L"cSubMeshEntity::UpdateGraphicsForFrame",
                      mapping.subMeshUpdateGraphics, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kMeshEntityUpdateGraphicsBytes,
                              "xxxxxxxxxxxxxxxxxxxxxxxx"},
                      L"cMeshEntity::UpdateGraphicsForFrame",
                      mapping.meshEntityUpdateGraphics, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kImageTrailRenderEffectBytes,
                              "xxxxxxxxxxxxxxxxxxxxx????xxxxxxx"},
                      L"cPostEffect_ImageTrail::RenderEffect",
                      mapping.imageTrailRenderEffect, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kLogicResetBytes, "xxxxxxxx????xxxxxxxxxx"},
                      L"cLogicTimer::Reset", mapping.logicReset, error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kLogicEndUpdateLoopBytes, "xxxxxxxxxxxxxxxx????xxxx"},
                      L"cLogicTimer::EndUpdateLoop", mapping.logicEndUpdateLoop,
                      error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kLogicGetStepSizeBytes, "xxxxxxxxxxxxxxx????xxxxx"},
                      L"cLogicTimer::GetStepSize", mapping.logicGetStepSize,
                      error) &&
           ResolveOne(imageBase, text, textRva,
                      Pattern{kLogicWantUpdateBytes, "xxxxxxxxxxxxxxxxxxx????x"},
                      L"cLogicTimer::WantUpdate", mapping.logicWantUpdate, error);
}

bool ResolveMouseImage(const HMODULE executable,
                       MouseMapping& mapping,
                       std::wstring& error) {
    mapping = {};
    const std::uint8_t* imageBase = nullptr;
    std::span<const std::uint8_t> text;
    std::uint32_t textRva = 0;
    if (!LocateText(executable, imageBase, text, textRva, error)) {
        return false;
    }
    Function lookBlock{};
    if (!ResolveOne(imageBase, text, textRva,
                    Pattern{kMouseSdlUpdateBytes, kMouseSdlUpdateMask},
                    L"cMouseSDL::Update", mapping.mouseUpdate, error) ||
        !ResolveOne(imageBase, text, textRva,
                    Pattern{kMouseSdlGetRelPositionBytes,
                            kMouseSdlGetRelPositionMask},
                    L"cMouseSDL::GetRelPosition", mapping.mouseGetRelPosition,
                    error) ||
        !ResolveOne(imageBase, text, textRva,
                    Pattern{kLuxUpdateGamePlayerInputBytes,
                            kLuxUpdateGamePlayerInputMask},
                    L"cLuxInputHandler::UpdateGamePlayerInput",
                    mapping.updateGamePlayerInput, error) ||
        !ResolveOne(imageBase, text, textRva,
                    Pattern{kLuxMouseLookBlockBytes, kLuxMouseLookBlockMask},
                    L"cLuxInputHandler mouse-look block", lookBlock, error) ||
        !ResolveOne(imageBase, text, textRva,
                    Pattern{kLuxPlayerAddYawBytes, kLuxPlayerAddYawMask},
                    L"cLuxPlayer::AddYaw", mapping.playerAddYaw, error) ||
        !ResolveOne(imageBase, text, textRva,
                    Pattern{kLuxPlayerAddPitchBytes, kLuxPlayerAddPitchMask},
                    L"cLuxPlayer::AddPitch", mapping.playerAddPitch, error) ||
        !ResolveOne(imageBase, text, textRva,
                    Pattern{kLuxPlayerIsAtMaxInfectionLevelBytes,
                            kLuxPlayerIsAtMaxInfectionLevelMask},
                    L"cLuxPlayer::IsAtMaxInfectionLevel",
                    mapping.playerIsAtMaxInfectionLevel, error)) {
        mapping = {};
        return false;
    }

    // The look block must be the one inside UpdateGamePlayerInput, must test
    // infection through the mapped helper, and the handler must end by
    // passing its result to the two mapped player methods.
    constexpr std::uint32_t kLookBlockOffset = 0x48D;
    constexpr std::uint32_t kInfectionCallOffset = 0x18;
    constexpr std::uint32_t kAddYawCallOffset = 0x91A;
    constexpr std::uint32_t kAddPitchCallOffset = 0x92B;
    const std::uint32_t input = mapping.updateGamePlayerInput.rva;
    const auto isCall = [imageBase](const std::uint32_t rva) noexcept {
        return imageBase[rva] == 0xE8;
    };
    if (lookBlock.rva != input + kLookBlockOffset ||
        !isCall(lookBlock.rva + kInfectionCallOffset) ||
        CallTarget(imageBase, lookBlock.rva + kInfectionCallOffset) !=
            mapping.playerIsAtMaxInfectionLevel.address ||
        !isCall(input + kAddYawCallOffset) ||
        CallTarget(imageBase, input + kAddYawCallOffset) !=
            mapping.playerAddYaw.address ||
        !isCall(input + kAddPitchCallOffset) ||
        CallTarget(imageBase, input + kAddPitchCallOffset) !=
            mapping.playerAddPitch.address) {
        mapping = {};
        error = L"cLuxInputHandler::UpdateGamePlayerInput: mouse-look "
                L"instruction layout does not match the mapped build";
        return false;
    }
    std::wstring guiError;
    if (!ResolveOne(imageBase, text, textRva,
                    Pattern{kGuiSendMousePosBytes, kGuiSendMousePosMask},
                    L"cGui::SendMousePos", mapping.guiSendMousePos, guiError)) {
        mapping.guiSendMousePos = {};
    }
    // AddYaw loads mpCamDirEffects from player +0x2E4 (inside its signature)
    // and passes the doubled amount to this helper.
    constexpr std::uint32_t kAddYawCamDirCallOffset = 0x66;
    std::wstring swayError;
    if (!ResolveOne(imageBase, text, textRva,
                    Pattern{kCamDirEffectsYawAddBytes, kCamDirEffectsYawAddMask},
                    L"cLuxPlayerCamDirEffects::AddAndGetYawAdd",
                    mapping.camDirEffectsYawAdd, swayError) ||
        !isCall(mapping.playerAddYaw.rva + kAddYawCamDirCallOffset) ||
        CallTarget(imageBase,
                   mapping.playerAddYaw.rva + kAddYawCamDirCallOffset) !=
            mapping.camDirEffectsYawAdd.address) {
        mapping.camDirEffectsYawAdd = {};
    }
    return true;
}

bool ResolveParticleImage(const HMODULE executable,
                          ParticleMapping& mapping,
                          std::wstring& error) {
    mapping = {};
    const std::uint8_t* imageBase = nullptr;
    std::span<const std::uint8_t> text;
    std::uint32_t textRva = 0;
    if (!LocateText(executable, imageBase, text, textRva, error)) {
        return false;
    }
    if (!ResolveOne(imageBase, text, textRva,
                    Pattern{kParticleEmitterUpdateGraphicsBytes,
                            kParticleEmitterUpdateGraphicsMask},
                    L"iParticleEmitter::UpdateGraphicsForViewport",
                    mapping.emitterUpdateGraphicsForViewport, error) ||
        !ResolveOne(imageBase, text, textRva,
                    Pattern{kParticleEmitterCreateParticleBytes,
                            kParticleEmitterCreateParticleMask},
                    L"iParticleEmitter::CreateParticle",
                    mapping.emitterCreateParticle, error)) {
        mapping = {};
        return false;
    }
    return true;
}

} // namespace amfp::target
