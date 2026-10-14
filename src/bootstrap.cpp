#include "bootstrap.h"

#include "config.h"
#include "host_validation.h"
#include "logger.h"
#include "plugin_version.h"
#include "sha256.h"
#include "target_mapping.h"
#include "telemetry.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace amfp {
namespace {

enum class BootstrapState : unsigned int {
    dormant,
    starting,
    disabled,
    rejected,
    ready,
};

std::atomic<BootstrapState> g_state{BootstrapState::dormant};

std::wstring ModulePath(const HMODULE module) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD copied = GetModuleFileNameW(
            module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            return std::wstring(buffer.data(), copied);
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

std::wstring ReplaceExtension(const std::wstring& path,
                              const std::wstring_view extension) {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos ||
        (slash != std::wstring::npos && dot < slash)) {
        return path + std::wstring(extension);
    }
    return path.substr(0, dot) + std::wstring(extension);
}

std::wstring LocalAppDataLogPath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD copied = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0 || copied >= buffer.size()) {
        return {};
    }

    std::wstring directory(buffer.data(), copied);
    directory.append(L"\\AmnesiaAMFP.FPSFix");
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }
    return directory + L"\\AmnesiaAMFP.FPSFix.log";
}

std::wstring HexValue(const std::uint64_t value, const unsigned int width) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setfill(L'0')
           << std::setw(static_cast<int>(width)) << value;
    return stream.str();
}

} // namespace

DWORD WINAPI BootstrapThread(void* moduleParameter) {
    g_state.store(BootstrapState::starting, std::memory_order_release);
    const HMODULE pluginModule = static_cast<HMODULE>(moduleParameter);
    const std::wstring pluginPath = ModulePath(pluginModule);
    if (pluginPath.empty()) {
        OutputDebugStringW(
            L"[AmnesiaAMFP.FPSFix] Could not resolve plugin module path.\n");
        g_state.store(BootstrapState::rejected, std::memory_order_release);
        return 1;
    }

    const std::wstring iniPath = ReplaceExtension(pluginPath, L".ini");
    const std::wstring logPath = ReplaceExtension(pluginPath, L".log");
    if (!log::Initialize(logPath)) {
        const std::wstring fallback = LocalAppDataLogPath();
        if (fallback.empty() || !log::Initialize(fallback)) {
            OutputDebugStringW(
                L"[AmnesiaAMFP.FPSFix] Could not open a diagnostic log.\n");
        } else {
            log::Write(log::Level::warning,
                       L"Could not write the sibling log; using: " + fallback);
        }
    }

    log::Write(log::Level::info,
               std::wstring(AMNESIA_AMFP_FPS_FIX_NAME) + L" " +
                   AMNESIA_AMFP_FPS_FIX_VERSION +
                   L" bootstrap starting");
    log::Write(log::Level::debug, L"Plugin path: " + pluginPath);
    log::Write(log::Level::debug, L"INI path: " + iniPath);

    const config::LoadResult loaded = config::Load(iniPath);
    if (!loaded.ok) {
        log::Write(log::Level::error, loaded.error);
        log::Write(log::Level::error,
                   L"Fail-closed: no engine hooks were installed.");
        g_state.store(BootstrapState::rejected, std::memory_order_release);
        return 2;
    }

    log::SetDiagnosticsEnabled(loaded.settings.diagnostics);
    if (!loaded.settings.enabled) {
        log::Write(log::Level::info,
                   L"Plugin disabled by [General] Enabled=false.");
        g_state.store(BootstrapState::disabled, std::memory_order_release);
        return 0;
    }

    const std::wstring executablePath = ModulePath(nullptr);
    if (executablePath.empty()) {
        log::Write(log::Level::error, L"Could not resolve the host path.");
        log::Write(log::Level::error,
                   L"Fail-closed: no engine hooks were installed.");
        g_state.store(BootstrapState::rejected, std::memory_order_release);
        return 3;
    }
    log::Write(log::Level::debug, L"Host path: " + executablePath);

    const crypto::Sha256Result hash = crypto::HashFile(executablePath);
    if (hash.ok) {
        log::Write(log::Level::info,
                   L"Host SHA-256 (evidence only): " + hash.digest);
    } else {
        log::Write(log::Level::warning,
                   L"Could not hash host executable: " + hash.error);
    }

    const host::ValidationResult validation =
        host::ValidateTarget(GetModuleHandleW(nullptr));
    if (!validation.ok) {
        log::Write(log::Level::error,
                   L"Host validation failed: " + validation.error);
        log::Write(log::Level::error,
                   L"Fail-closed: no engine hooks were installed.");
        g_state.store(BootstrapState::rejected, std::memory_order_release);
        return 4;
    }

    log::Write(log::Level::info,
               L"Accepted AMFP bootstrap host: base=" +
                   HexValue(validation.facts.imageBase, 8) + L", size=" +
                   HexValue(validation.facts.imageSize, 8) + L", timestamp=" +
                   HexValue(validation.facts.timeDateStamp, 8));

    target::CoreMapping coreMapping{};
    std::wstring mappingError;
    if (!target::ResolveCoreImage(GetModuleHandleW(nullptr), coreMapping,
                                  mappingError)) {
        log::Write(log::Level::error,
                   L"Core signature validation failed: " + mappingError);
        log::Write(log::Level::error,
                   L"Fail-closed: no engine hooks were installed.");
        g_state.store(BootstrapState::rejected, std::memory_order_release);
        return 5;
    }
    log::Write(log::Level::info,
               L"Validated core mappings: cEngine::Run RVA=" +
                   HexValue(coreMapping.engineRun.rva, 8) +
                   L", render/update gate RVA=" +
                   HexValue(coreMapping.renderUpdateGate.rva, 8) +
                   L", cUpdater::RunMessage RVA=" +
                   HexValue(coreMapping.updaterRunMessage.rva, 8) +
                   L", cScene::Render RVA=" +
                   HexValue(coreMapping.sceneRender.rva, 8) +
                   L", cCamera setters RVA=" +
                   HexValue(coreMapping.cameraSetPosition.rva, 8) + L"/" +
                   HexValue(coreMapping.cameraSetPitch.rva, 8) + L"/" +
                   HexValue(coreMapping.cameraSetYaw.rva, 8) + L"/" +
                   HexValue(coreMapping.cameraSetRoll.rva, 8) +
                   L", cLuxPlayerHands::PostUpdate RVA=" +
                   HexValue(coreMapping.playerHandsPostUpdate.rva, 8) +
                   L", iLuxEnemy::OnUpdate RVA=" +
                   HexValue(coreMapping.luxEnemyOnUpdate.rva, 8) +
                   L", iLuxProp::OnUpdate RVA=" +
                   HexValue(coreMapping.luxPropOnUpdate.rva, 8) +
                   L", iLuxProp::SetupAfterLoad RVA=" +
                   HexValue(coreMapping.luxPropSetupAfterLoad.rva, 8) +
                   L", cPhysicsBodyNewton::OnTransformCallback RVA=" +
                   HexValue(coreMapping.newtonTransformCallback.rva, 8) +
                   L", iEntity3D::GetWorldMatrix RVA=" +
                   HexValue(coreMapping.entityGetWorldMatrix.rva, 8) +
                   L", iEntity3D::GetEntityParent RVA=" +
                   HexValue(coreMapping.entityGetEntityParent.rva, 8) +
                   L", cSubMeshEntity::GetModelMatrix RVA=" +
                   HexValue(coreMapping.subMeshGetModelMatrix.rva, 8) +
                   L", cSubMeshEntity::UpdateGraphicsForFrame RVA=" +
                   HexValue(coreMapping.subMeshUpdateGraphics.rva, 8) +
                   L", cMeshEntity::UpdateGraphicsForFrame RVA=" +
                   HexValue(coreMapping.meshEntityUpdateGraphics.rva, 8) +
                   L", cPostEffect_ImageTrail::RenderEffect RVA=" +
                   HexValue(coreMapping.imageTrailRenderEffect.rva, 8) +
                   L", cLogicTimer::WantUpdate RVA=" +
                   HexValue(coreMapping.logicWantUpdate.rva, 8));

    if (loaded.settings.telemetryEnabled ||
        loaded.settings.features.renderUnlock ||
        loaded.settings.features.cameraInterpolation ||
        loaded.settings.features.handsRootInterpolation ||
        loaded.settings.features.handsPoseInterpolation ||
        loaded.settings.features.enemyPoseInterpolation ||
        loaded.settings.features.ragdollPoseInterpolation ||
        loaded.settings.features.nodeAnimationInterpolation ||
        loaded.settings.features.worldRootInterpolation ||
        loaded.settings.features.particleInterpolation ||
        loaded.settings.features.imageTrailTiming) {
        target::ParticleMapping particleMapping{};
        bool particlesMapped = false;
        if (loaded.settings.features.particleInterpolation) {
            std::wstring particleMappingError;
            particlesMapped = target::ResolveParticleImage(
                GetModuleHandleW(nullptr), particleMapping,
                particleMappingError);
            if (particlesMapped) {
                log::Write(
                    log::Level::info,
                    L"Validated particle mappings: iParticleEmitter::"
                    L"UpdateGraphicsForViewport RVA=" +
                        HexValue(particleMapping
                                     .emitterUpdateGraphicsForViewport.rva,
                                 8) +
                        L", CreateParticle RVA=" +
                        HexValue(particleMapping.emitterCreateParticle.rva, 8));
            } else {
                log::Write(log::Level::warning,
                           L"Particle mapping failed; particles stay at the "
                           L"fixed-update rate: " + particleMappingError);
            }
        }
        target::MouseMapping mouseMapping{};
        bool mouseMapped = false;
        if (loaded.settings.features.mouseOverlay) {
            std::wstring mouseMappingError;
            mouseMapped = target::ResolveMouseImage(
                GetModuleHandleW(nullptr), mouseMapping, mouseMappingError);
            if (mouseMapped) {
                // The GUI cursor shares the mouse hooks; an empty mapping is
                // how the runtime is told to leave the cursor stock.
                if (!loaded.settings.features.guiCursorOverlay) {
                    mouseMapping.guiSendMousePos = {};
                } else if (mouseMapping.guiSendMousePos.address == nullptr) {
                    log::Write(log::Level::warning,
                               L"cGui::SendMousePos mapping failed; menu, "
                               L"inventory, and journal cursors stay stock.");
                } else {
                    log::Write(log::Level::info,
                               L"Validated GUI cursor mapping: "
                               L"cGui::SendMousePos RVA=" +
                                   HexValue(mouseMapping.guiSendMousePos.rva, 8));
                }
                log::Write(log::Level::info,
                           L"Validated mouse-look mappings: cMouseSDL::Update RVA=" +
                               HexValue(mouseMapping.mouseUpdate.rva, 8) +
                               L", cLuxInputHandler::UpdateGamePlayerInput RVA=" +
                               HexValue(mouseMapping.updateGamePlayerInput.rva, 8) +
                               L", cLuxPlayer::AddYaw/AddPitch RVA=" +
                               HexValue(mouseMapping.playerAddYaw.rva, 8) + L"/" +
                               HexValue(mouseMapping.playerAddPitch.rva, 8));
            } else {
                log::Write(log::Level::warning,
                           L"Mouse-look mapping failed; gameplay mouse look "
                           L"stays stock: " + mouseMappingError);
            }
        }
        std::wstring telemetryError;
        if (!telemetry::Initialize(
                coreMapping, loaded.settings.telemetryEnabled,
                loaded.settings.ragdollProfileEnabled,
                loaded.settings.features.renderUnlock,
                loaded.settings.features.cameraInterpolation,
                loaded.settings.features.handsRootInterpolation,
                loaded.settings.features.handsPoseInterpolation,
                loaded.settings.features.enemyPoseInterpolation,
                loaded.settings.features.ragdollPoseInterpolation,
                loaded.settings.features.nodeAnimationInterpolation,
                loaded.settings.features.worldRootInterpolation,
                loaded.settings.features.imageTrailTiming,
                mouseMapped ? &mouseMapping : nullptr,
                particlesMapped ? &particleMapping : nullptr,
                loaded.settings.telemetryReportIntervalSeconds,
                telemetryError)) {
            log::Write(log::Level::error,
                       L"Runtime initialization failed: " + telemetryError);
            log::Write(log::Level::error,
                       L"Fail-closed: telemetry hooks were rolled back.");
            g_state.store(BootstrapState::rejected, std::memory_order_release);
            return 6;
        }
    } else {
        log::Write(log::Level::info,
                   L"Telemetry disabled; no engine hooks were installed.");
    }

    config::Features unsupported = loaded.settings.features;
    unsupported.renderUnlock = false;
    unsupported.cameraInterpolation = false;
    unsupported.handsRootInterpolation = false;
    unsupported.handsPoseInterpolation = false;
    unsupported.enemyPoseInterpolation = false;
    unsupported.ragdollPoseInterpolation = false;
    unsupported.nodeAnimationInterpolation = false;
    unsupported.worldRootInterpolation = false;
    unsupported.imageTrailTiming = false;
    unsupported.mouseOverlay = false;
    unsupported.particleInterpolation = false;
    if (loaded.settings.features.mouseOverlay) {
        unsupported.guiCursorOverlay = false;
    } else if (loaded.settings.features.guiCursorOverlay) {
        log::Write(log::Level::warning,
                   L"GuiCursorOverlay requires MouseOverlay; GUI cursors stay "
                   L"at the fixed-update rate.");
        unsupported.guiCursorOverlay = false;
    }
    if (loaded.settings.motionTraceEnabled ||
        config::AnyBehavioralFeatureEnabled(unsupported)) {
        log::Write(
            log::Level::warning,
            L"One or more behavioral or motion-trace features were requested, "
            L"but 1.0.0 keeps those remaining features disabled.");
    }

    log::Write(log::Level::info,
               L"Bootstrap validation completed successfully.");
    g_state.store(BootstrapState::ready, std::memory_order_release);
    return 0;
}

} // namespace amfp
