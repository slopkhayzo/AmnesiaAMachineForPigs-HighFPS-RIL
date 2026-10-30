#include "target_mapping.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

struct Handle final {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
};

struct View final {
    void* value = nullptr;
    ~View() {
        if (value != nullptr) {
            UnmapViewOfFile(value);
        }
    }
};

constexpr std::size_t kFunctionCount = 33;

constexpr std::array<const wchar_t*, kFunctionCount> kNames{
    L"cEngine::Run",
    L"cEngine::Run render/update gate",
    L"cUpdater::RunMessage",
    L"cScene::Render",
    L"cCamera::SetPosition",
    L"cCamera::SetPitch",
    L"cCamera::SetYaw",
    L"cCamera::SetRoll",
    L"cLuxPlayerHands::PostUpdate",
    L"iLuxEnemy::OnUpdate",
    L"iLuxProp::OnUpdate",
    L"iLuxProp::SetupAfterLoad",
    L"cPhysicsBodyNewton::OnTransformCallback",
    L"iEntity3D::GetWorldMatrix",
    L"iEntity3D::GetEntityParent",
    L"cSubMeshEntity::GetModelMatrix",
    L"cSubMeshEntity::UpdateGraphicsForFrame",
    L"cMeshEntity::UpdateGraphicsForFrame",
    L"cPostEffect_ImageTrail::RenderEffect",
    L"cLogicTimer::Reset",
    L"cLogicTimer::EndUpdateLoop",
    L"cLogicTimer::GetStepSize",
    L"cLogicTimer::WantUpdate",
    L"cMouseSDL::Update",
    L"cMouseSDL::GetRelPosition",
    L"cLuxInputHandler::UpdateGamePlayerInput",
    L"cLuxPlayer::AddYaw",
    L"cLuxPlayer::AddPitch",
    L"cLuxPlayer::IsAtMaxInfectionLevel",
    L"cGui::SendMousePos",
    L"cLuxPlayerCamDirEffects::AddAndGetYawAdd",
    L"iParticleEmitter::UpdateGraphicsForViewport",
    L"iParticleEmitter::CreateParticle",
};

struct KnownBuild {
    const wchar_t* name;
    std::uint32_t timeDateStamp;
    std::array<std::uint32_t, kFunctionCount> rvas;
};

// RVAs recorded from each build, in kNames order.
constexpr std::array<KnownBuild, 3> kKnownBuilds{{
    {L"GOG 51194033869387641 aamfp.exe", 0x52722FC7,
     {0x00148CC0, 0x00149260, 0x0014A940, 0x0017B2F0, 0x00177310, 0x00176F20,
      0x00176F90, 0x00177000, 0x0010BEE0, 0x000553B0, 0x000687C0, 0x0006A9A0,
      0x00227680, 0x00178AD0, 0x00178B30, 0x001BBDA0, 0x001BB8B0, 0x00126B20,
      0x001EA980, 0x001D5FC0, 0x001D5FF0, 0x001D6030, 0x001D60B0, 0x002113B0,
      0x00211360, 0x0009F4B0, 0x00105D00, 0x00105D90, 0x00106290, 0x00164650,
      0x0010D8D0, 0x001B0710, 0x001B2330}},
    {L"Steam 1648011 aamfp.exe", 0x58AD5999,
     {0x00152850, 0x00152DF0, 0x00154480, 0x00180190, 0x0017BEB0, 0x0017BAC0,
      0x0017BB30, 0x0017BBA0, 0x0010FF50, 0x000594D0, 0x0006CA00, 0x0006EA40,
      0x0022C2B0, 0x0017D730, 0x0017D790, 0x001CD420, 0x001CCF30, 0x00130170,
      0x001EF740, 0x001E4D00, 0x001E4D30, 0x001E4D70, 0x001E4DF0, 0x00216280,
      0x00216260, 0x000A31E0, 0x00109E70, 0x00109F00, 0x0010A400, 0x00169290,
      0x00111910, 0x001B5560, 0x001B7180}},
    {L"Steam 1648011 aamfp_NoSteam.exe", 0x58AD594E,
     {0x0014F150, 0x0014F6F0, 0x00150D20, 0x0017C450, 0x00178560, 0x00178170,
      0x001781E0, 0x00178250, 0x0010C710, 0x000561C0, 0x000696C0, 0x0006B8A0,
      0x00227EC0, 0x00179D30, 0x00179D90, 0x001C6880, 0x001C6390, 0x0012C5C0,
      0x001EB490, 0x001E06A0, 0x001E06D0, 0x001E0710, 0x001E0790, 0x00211E30,
      0x00211DE0, 0x000A0380, 0x001065A0, 0x00106630, 0x00106B30, 0x001659B0,
      0x0010E100, 0x001B13C0, 0x001B2FE0}},
}};

std::uint32_t TimeDateStamp(const void* image) {
    const auto* const base = static_cast<const std::uint8_t*>(image);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}

} // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    if (argumentCount != 2) {
        std::wcerr << L"usage: target_mapping_probe.exe <aamfp.exe>\n";
        return 2;
    }

    Handle file{CreateFileW(arguments[1], GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        std::wcerr << L"CreateFileW failed: " << GetLastError() << L'\n';
        return 3;
    }
    Handle mapping{CreateFileMappingW(file.value, nullptr,
                                      PAGE_READONLY | SEC_IMAGE_NO_EXECUTE,
                                      0, 0, nullptr)};
    if (mapping.value == nullptr) {
        std::wcerr << L"CreateFileMappingW failed: " << GetLastError() << L'\n';
        return 4;
    }
    View view{MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, 0)};
    if (view.value == nullptr) {
        std::wcerr << L"MapViewOfFile failed: " << GetLastError() << L'\n';
        return 5;
    }
    const auto module = static_cast<HMODULE>(view.value);

    std::wstring error;
    amfp::target::CoreMapping core{};
    if (!amfp::target::ResolveCoreImage(module, core, error)) {
        std::wcerr << L"Mapping failed closed: " << error << L'\n';
        return 6;
    }
    amfp::target::MouseMapping mouse{};
    if (!amfp::target::ResolveMouseImage(module, mouse, error)) {
        std::wcerr << L"Mouse mapping failed closed: " << error << L'\n';
        return 8;
    }
    amfp::target::ParticleMapping particles{};
    if (!amfp::target::ResolveParticleImage(module, particles, error)) {
        std::wcerr << L"Particle mapping failed closed: " << error << L'\n';
        return 10;
    }

    const std::array<const amfp::target::Function*, kFunctionCount> resolved{
        &core.engineRun, &core.renderUpdateGate, &core.updaterRunMessage,
        &core.sceneRender, &core.cameraSetPosition, &core.cameraSetPitch,
        &core.cameraSetYaw, &core.cameraSetRoll, &core.playerHandsPostUpdate,
        &core.luxEnemyOnUpdate, &core.luxPropOnUpdate,
        &core.luxPropSetupAfterLoad, &core.newtonTransformCallback,
        &core.entityGetWorldMatrix, &core.entityGetEntityParent,
        &core.subMeshGetModelMatrix, &core.subMeshUpdateGraphics,
        &core.meshEntityUpdateGraphics, &core.imageTrailRenderEffect,
        &core.logicReset, &core.logicEndUpdateLoop, &core.logicGetStepSize,
        &core.logicWantUpdate, &mouse.mouseUpdate, &mouse.mouseGetRelPosition,
        &mouse.updateGamePlayerInput, &mouse.playerAddYaw, &mouse.playerAddPitch,
        &mouse.playerIsAtMaxInfectionLevel, &mouse.guiSendMousePos,
        &mouse.camDirEffectsYawAdd, &particles.emitterUpdateGraphicsForViewport,
        &particles.emitterCreateParticle};

    // An unknown build is reported, not failed: the resolvers above already
    // proved every signature and layout check passed.
    const std::uint32_t stamp = TimeDateStamp(view.value);
    const KnownBuild* known = nullptr;
    for (const KnownBuild& build : kKnownBuilds) {
        if (build.timeDateStamp == stamp) {
            known = &build;
        }
    }
    std::wcout << L"Build: "
               << (known != nullptr ? known->name : L"unknown (no recorded RVAs)")
               << L'\n';

    bool correct = true;
    for (std::size_t index = 0; index < kFunctionCount; ++index) {
        const amfp::target::Function& function = *resolved[index];
        std::wcout << kNames[index] << L" RVA 0x" << std::uppercase << std::hex
                   << std::setw(8) << std::setfill(L'0') << function.rva << L'\n';
        if (function.address == nullptr) {
            std::wcerr << L"  unresolved\n";
            correct = false;
        } else if (known != nullptr && function.rva != known->rvas[index]) {
            std::wcerr << L"  expected RVA 0x" << std::setw(8)
                       << known->rvas[index] << L'\n';
            correct = false;
        }
    }
    return correct ? 0 : 7;
}
