#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace amfp::target {

struct Function {
    void* address = nullptr;
    std::uint32_t rva = 0;
};

struct CoreMapping {
    Function engineRun{};
    Function renderUpdateGate{};
    Function updaterRunMessage{};
    Function sceneRender{};
    Function cameraSetPosition{};
    Function cameraSetPitch{};
    Function cameraSetYaw{};
    Function cameraSetRoll{};
    Function playerHandsPostUpdate{};
    Function luxEnemyOnUpdate{};
    Function luxPropOnUpdate{};
    Function luxPropSetupAfterLoad{};
    Function newtonTransformCallback{};
    Function entityGetWorldMatrix{};
    Function entityGetEntityParent{};
    Function subMeshGetModelMatrix{};
    Function subMeshUpdateGraphics{};
    Function meshEntityUpdateGraphics{};
    Function imageTrailRenderEffect{};
    Function logicReset{};
    Function logicEndUpdateLoop{};
    Function logicGetStepSize{};
    Function logicWantUpdate{};
};

struct MouseMapping {
    Function mouseUpdate{};
    Function mouseGetRelPosition{};
    Function updateGamePlayerInput{};
    Function playerAddYaw{};
    Function playerAddPitch{};
    Function playerIsAtMaxInfectionLevel{};
    // Optional: left empty when unmapped, which disables only the
    // presentation-rate GUI cursor.
    Function guiSendMousePos{};
    // Optional and never hooked: its signature pins the sight-sway fields.
    // Left empty when unmapped, which makes sway ticks keep stock look.
    Function camDirEffectsYawAdd{};
};

struct ParticleMapping {
    Function emitterUpdateGraphicsForViewport{};
    // Not hooked; its signature pins the particle vector and count offsets.
    Function emitterCreateParticle{};
};

// The module may be a live image or a read-only SEC_IMAGE mapping. No function
// is called and no memory is modified while resolving these signatures.
[[nodiscard]] bool ResolveCoreImage(HMODULE executable,
                                    CoreMapping& mapping,
                                    std::wstring& error);

// Resolved separately so an unmapped mouse path disables only mouse preview.
[[nodiscard]] bool ResolveMouseImage(HMODULE executable,
                                     MouseMapping& mapping,
                                     std::wstring& error);

// Resolved separately so an unmapped emitter disables only particles.
[[nodiscard]] bool ResolveParticleImage(HMODULE executable,
                                        ParticleMapping& mapping,
                                        std::wstring& error);

} // namespace amfp::target
