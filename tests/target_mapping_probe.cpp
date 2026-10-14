#include "target_mapping.h"

#include <Windows.h>

#include <array>
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

bool Check(const wchar_t* name,
           const amfp::target::Function& function,
           const std::uint32_t expectedRva) {
    std::wcout << name << L" RVA 0x" << std::uppercase << std::hex
               << std::setw(8) << std::setfill(L'0') << function.rva << L'\n';
    if (function.rva == expectedRva && function.address != nullptr) {
        return true;
    }
    std::wcerr << L"Expected RVA 0x" << std::setw(8) << expectedRva << L'\n';
    return false;
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

    amfp::target::CoreMapping resolved{};
    std::wstring error;
    if (!amfp::target::ResolveCoreImage(
            static_cast<HMODULE>(view.value), resolved, error)) {
        std::wcerr << L"Mapping failed closed: " << error << L'\n';
        return 6;
    }

    const bool correct =
        Check(L"cEngine::Run", resolved.engineRun, 0x00148CC0) &&
        Check(L"cEngine::Run render/update gate", resolved.renderUpdateGate,
              0x00149260) &&
        Check(L"cUpdater::RunMessage", resolved.updaterRunMessage, 0x0014A940) &&
        Check(L"cScene::Render", resolved.sceneRender, 0x0017B2F0) &&
        Check(L"cCamera::SetPosition", resolved.cameraSetPosition, 0x00177310) &&
        Check(L"cCamera::SetPitch", resolved.cameraSetPitch, 0x00176F20) &&
        Check(L"cCamera::SetYaw", resolved.cameraSetYaw, 0x00176F90) &&
        Check(L"cCamera::SetRoll", resolved.cameraSetRoll, 0x00177000) &&
        Check(L"cLuxPlayerHands::PostUpdate", resolved.playerHandsPostUpdate,
              0x0010BEE0) &&
        Check(L"iLuxEnemy::OnUpdate", resolved.luxEnemyOnUpdate, 0x000553B0) &&
        Check(L"iLuxProp::OnUpdate", resolved.luxPropOnUpdate, 0x000687C0) &&
        Check(L"iLuxProp::SetupAfterLoad", resolved.luxPropSetupAfterLoad,
              0x0006A9A0) &&
        Check(L"cPhysicsBodyNewton::OnTransformCallback",
              resolved.newtonTransformCallback, 0x00227680) &&
        Check(L"iEntity3D::GetWorldMatrix", resolved.entityGetWorldMatrix,
              0x00178AD0) &&
        Check(L"iEntity3D::GetEntityParent", resolved.entityGetEntityParent,
              0x00178B30) &&
        Check(L"cSubMeshEntity::GetModelMatrix",
              resolved.subMeshGetModelMatrix, 0x001BBDA0) &&
        Check(L"cSubMeshEntity::UpdateGraphicsForFrame",
              resolved.subMeshUpdateGraphics, 0x001BB8B0) &&
        Check(L"cMeshEntity::UpdateGraphicsForFrame",
              resolved.meshEntityUpdateGraphics, 0x00126B20) &&
        Check(L"cPostEffect_ImageTrail::RenderEffect",
              resolved.imageTrailRenderEffect, 0x001EA980) &&
        Check(L"cLogicTimer::Reset", resolved.logicReset, 0x001D5FC0) &&
        Check(L"cLogicTimer::EndUpdateLoop", resolved.logicEndUpdateLoop,
              0x001D5FF0) &&
        Check(L"cLogicTimer::GetStepSize", resolved.logicGetStepSize,
              0x001D6030) &&
        Check(L"cLogicTimer::WantUpdate", resolved.logicWantUpdate,
              0x001D60B0);
    if (!correct) return 7;

    amfp::target::MouseMapping mouse{};
    if (!amfp::target::ResolveMouseImage(
            static_cast<HMODULE>(view.value), mouse, error)) {
        std::wcerr << L"Mouse mapping failed closed: " << error << L'\n';
        return 8;
    }
    const bool mouseCorrect =
        Check(L"cMouseSDL::Update", mouse.mouseUpdate, 0x002113B0) &&
        Check(L"cMouseSDL::GetRelPosition", mouse.mouseGetRelPosition,
              0x00211360) &&
        Check(L"cLuxInputHandler::UpdateGamePlayerInput",
              mouse.updateGamePlayerInput, 0x0009F4B0) &&
        Check(L"cLuxPlayer::AddYaw", mouse.playerAddYaw, 0x00105D00) &&
        Check(L"cLuxPlayer::AddPitch", mouse.playerAddPitch, 0x00105D90) &&
        Check(L"cLuxPlayer::IsAtMaxInfectionLevel",
              mouse.playerIsAtMaxInfectionLevel, 0x00106290) &&
        Check(L"cGui::SendMousePos", mouse.guiSendMousePos, 0x00164650) &&
        Check(L"cLuxPlayerCamDirEffects::AddAndGetYawAdd",
              mouse.camDirEffectsYawAdd, 0x0010D8D0);
    if (!mouseCorrect) return 9;

    amfp::target::ParticleMapping particles{};
    if (!amfp::target::ResolveParticleImage(
            static_cast<HMODULE>(view.value), particles, error)) {
        std::wcerr << L"Particle mapping failed closed: " << error << L'\n';
        return 10;
    }
    const bool particlesCorrect =
        Check(L"iParticleEmitter::UpdateGraphicsForViewport",
              particles.emitterUpdateGraphicsForViewport, 0x001B0710) &&
        Check(L"iParticleEmitter::CreateParticle",
              particles.emitterCreateParticle, 0x001B2330);
    return particlesCorrect ? 0 : 11;
}
