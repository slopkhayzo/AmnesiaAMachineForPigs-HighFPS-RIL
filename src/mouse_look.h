#pragma once

#include "camera_runtime.h"
#include "hands_interpolation.h"
#include "presentation_clock.h"
#include "target_mapping.h"

#include <atomic>
#include <cstdint>
#include <string>

// Presentation-rate gameplay mouse look for AMFP.
//
// The engine samples the mouse once per fixed update, so at high refresh rates
// camera rotation otherwise advances only 60 times a second and is shown one
// interpolation interval late. This module acquires relative motion on every
// presented frame, previews it on the render-only camera, and hands exactly
// the same counts to the next fixed update, so each physical count reaches the
// authoritative look command once.
namespace amfp::mouse_look {

struct Setup {
    target::MouseMapping mapping{};
    // Completed fixed-tick serial owned by the frame hooks.
    const std::atomic<std::uint64_t>* tickSerial = nullptr;
    camera::Runtime* camera = nullptr;
};

// Creates and queues every mouse hook. MinHook must already be initialized;
// the caller applies the queue. On failure no mouse hook remains queued.
[[nodiscard]] bool Install(const Setup& setup, std::wstring& error);

// Call at the start of each scene render, before the camera is interpolated.
void AcquirePresentation() noexcept;

// Call after the camera runtime applied its interpolated pose. Returns true
// and fills `handsCorrection` when a preview offset was applied, so the
// first-person hands can follow the previewed view.
[[nodiscard]] bool ApplyPreview(const clock::Sample& sample,
                                std::uint64_t currentSerial,
                                hands::Matrix& handsCorrection) noexcept;

// Call before the engine queues its GUI for a frame (the OnDraw message).
// Forwards cursor movement that arrived since the last fixed update to the
// focused GUI set; it never sends a click. Does nothing outside GUI states or
// when cGui::SendMousePos is unmapped.
void PresentGuiCursor() noexcept;

// One line of conservation and classification counters; not for hot paths.
void LogSummary();

} // namespace amfp::mouse_look
