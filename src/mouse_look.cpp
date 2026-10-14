#include "mouse_look.h"

#include "logger.h"
#include "mouse_preview_math.h"

#include <MinHook.h>
#include <Windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace amfp::mouse_look {
namespace {

// Retail layouts, each pinned by the signatures in target_mapping.cpp.
constexpr std::size_t kMouseRelXOffset = 0x30;
constexpr std::size_t kMouseRelYOffset = 0x34;
constexpr std::size_t kInputPlayerOffset = 0x38;
constexpr std::size_t kInputSmoothMouseOffset = 0x40;
constexpr std::size_t kInputInvertMouseOffset = 0x41;
constexpr std::size_t kInputMouseSensitivityOffset = 0x50;
constexpr std::size_t kInputInfectionDropOffset = 0x68;
constexpr std::size_t kInputLookScaleOffset = 0x88;
constexpr std::size_t kInputPointerSpeedOffset = 0x94;
constexpr std::size_t kPlayerCameraOffset = 0x50;
constexpr std::size_t kPlayerLookSpeedMulOffset = 0xCC;
// cLuxPlayerCamDirEffects ("sight sway"): once infection passes a threshold
// the camera turns by a blend of this tick's input and the mean of the last
// saved inputs.
constexpr std::size_t kPlayerCamDirEffectsOffset = 0x2E4;
constexpr std::size_t kSwaySavedCountOffset = 0x40;
constexpr std::size_t kSwayAddOffset = 0x48;
constexpr std::size_t kSwayAlphaOffset = 0x54;
constexpr std::size_t kCameraPitchOffset = 0x28;
constexpr std::size_t kCameraYawOffset = 0x2C;

struct MouseDelta {
    std::int32_t x;
    std::int32_t y;
};

using MouseSdlUpdate = void(__thiscall*)(void*);
using MouseSdlGetRelPosition = MouseDelta*(__thiscall*)(void*, MouseDelta*);
using LuxUpdateGamePlayerInput = void(__thiscall*)(void*);
using LuxPlayerAddAngle = void(__thiscall*)(void*, float);
using LuxPlayerIsAtMaxInfectionLevel = bool(__thiscall*)(void*);
using SdlGetRelativeMouseState = std::uint32_t(__cdecl*)(int*, int*);
using SdlPumpEvents = void(__cdecl*)();
using SdlGetRelativeMouseMode = int(__cdecl*)();
using GuiSendMousePos =
    bool(__thiscall*)(void*, const MouseDelta*, const MouseDelta*);

MouseSdlUpdate g_originalMouseSdlUpdate = nullptr;
MouseSdlGetRelPosition g_originalMouseSdlGetRelPosition = nullptr;
LuxUpdateGamePlayerInput g_originalLuxUpdateGamePlayerInput = nullptr;
LuxPlayerAddAngle g_originalLuxPlayerAddYaw = nullptr;
LuxPlayerAddAngle g_originalLuxPlayerAddPitch = nullptr;
LuxPlayerIsAtMaxInfectionLevel g_playerIsAtMaxInfectionLevel = nullptr;
SdlGetRelativeMouseState g_sdlGetRelativeMouseState = nullptr;
SdlPumpEvents g_sdlPumpEvents = nullptr;
const std::atomic<std::uint64_t>* g_tickSerial = nullptr;
camera::Runtime* g_camera = nullptr;

// Everything below runs on the engine's main thread except the window
// procedure, which Windows also dispatches on that thread; the atomics only
// guard against a foreign caller.
void* g_mouseDevice = nullptr;
DWORD g_mouseThreadId = 0;
std::atomic<std::int64_t> g_pendingX{0};
std::atomic<std::int64_t> g_pendingY{0};
HWND g_rawWindow = nullptr;
WNDPROC g_rawOriginalWindowProc = nullptr;
std::atomic<bool> g_rawTapInstalled{false};
std::atomic<bool> g_rawActive{false};
std::atomic<std::int64_t> g_rawSinceRenderX{0};
std::atomic<std::int64_t> g_rawSinceRenderY{0};
std::uint64_t g_lastGameplayTick = 0;
bool g_acquisitionEligible = false;

// Per-call state of cLuxInputHandler::UpdateGamePlayerInput.
bool g_inPlayerInput = false;
std::uint32_t g_readsThisCall = 0;
std::int32_t g_mouseXThisCall = 0;
std::int32_t g_mouseYThisCall = 0;
void* g_cameraThisCall = nullptr;
float g_cameraPitchThisCall = 0.0F;
float g_cameraYawThisCall = 0.0F;
float g_sensitivityThisCall = 1.0F;
float g_lookSpeedMulThisCall = 1.0F;
// Sway state as AddYaw/AddPitch will use it in this call: the weight of the
// saved-input mean and that mean (x = yaw, y = pitch, camera radians).
bool g_swayMapped = false;
bool g_lookModelValidThisCall = false;
float g_swayBlendThisCall = 0.0F;
float g_swayYawThisCall = 0.0F;
float g_swayPitchThisCall = 0.0F;
bool g_invertThisCall = false;
bool g_continuousLookThisCall = false;
bool g_yawDequantizedThisCall = false;
bool g_pitchDequantizedThisCall = false;

struct AngularCommand {
    std::uint64_t serial = 0;
    void* camera = nullptr;
    float pitch = 0.0F;
    float yaw = 0.0F;
    // The part of the command that came straight from this tick's counts.
    // The rest (sight sway) is a slow average and stays interpolated.
    float directPitch = 0.0F;
    float directYaw = 0.0F;
    // Share of a count's calibrated angle that reaches the camera directly.
    float directWeight = 1.0F;
    bool valid = false;
    bool pendingRawCompatible = false;
};
constexpr std::size_t kCommandHistoryCapacity = 2;
std::array<AngularCommand, kCommandHistoryCapacity> g_commandHistory{};
void* g_latestPlayerCamera = nullptr;
float g_sensitivity = 1.0F;
bool g_invert = false;
float g_yawPerRoundedPixel = 0.0F;
bool g_yawScaleCalibrated = false;
mouse_preview::PendingRawGate g_pendingRawGate;
float g_playerAmountPerRoundedCount = 0.0F;
bool g_playerAmountScaleCalibrated = false;

struct Stats {
    std::uint64_t ticks = 0;
    std::uint64_t eligibleTicks = 0;
    std::uint64_t presentationPolls = 0;
    std::uint64_t rawPackets = 0;
    // Counts handed to a gameplay tick, and the part of them the look handler
    // read. Menu and loading ticks are excluded: cMouseSDL::Update overwrites
    // its delta, so motion nobody reads there is dropped by the stock game too.
    std::int64_t deliveredX = 0;
    std::int64_t deliveredY = 0;
    std::int64_t consumedX = 0;
    std::int64_t consumedY = 0;
    std::uint64_t deliveriesNotConsumed = 0;
    std::uint64_t rawTickMismatches = 0;
    std::uint64_t commandsDirect = 0;
    std::uint64_t commandsTransformed = 0;
    std::uint64_t commandsMissing = 0;
    std::uint64_t commandsSwayModelled = 0;
    std::uint64_t commandsLookSpeedModelled = 0;
    std::uint64_t transformedLookSpeed = 0;
    std::uint64_t transformedPitchShort = 0;
    std::uint64_t transformedBlocked = 0;
    std::uint64_t transformedOther = 0;
    std::uint64_t dequantized = 0;
    std::uint64_t dequantizeRejected = 0;
    std::uint64_t previewApplied = 0;
    std::uint64_t previewBypassed = 0;
    std::uint64_t previewNoCommand = 0;
    std::uint64_t previewFailed = 0;
    std::uint64_t stateInvalid = 0;
    std::uint64_t threadMismatches = 0;
    std::uint64_t mergeOverflows = 0;
    std::uint64_t fallbackEntered = 0;
};
Stats g_stats{};
std::atomic<std::uint64_t> g_rawPacketCount{0};
bool g_calibrationLogged = false;
bool g_deliveryUnread = false;

// Software GUI cursor (menus, inventory, journal). The fixed update remains
// the only source of clicks; presentation only forwards cursor movement.
constexpr std::size_t kGuiFocusedSetOffset = 0x34;
GuiSendMousePos g_originalGuiSendMousePos = nullptr;
SdlGetRelativeMouseMode g_sdlGetRelativeMouseMode = nullptr;
void* g_guiObject = nullptr;
mouse_preview::GuiPosition g_guiFixedPosition{};
mouse_preview::GuiPosition g_guiPresentedPosition{};
std::uint64_t g_guiLastFixedTick = 0;
bool g_guiPositionValid = false;

struct GuiStats {
    std::uint64_t fixedCalls = 0;
    std::uint64_t fixedCorrections = 0;
    std::uint64_t relativeSamples = 0;
    std::uint64_t absoluteSamples = 0;
    std::uint64_t presentedMoves = 0;
    std::uint64_t skippedOutside = 0;
    std::uint64_t skippedInvalid = 0;
};
GuiStats g_guiStats{};

template <typename T> T ReadField(const void* base, const std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
    return value;
}

template <typename T>
void WriteField(void* base, const std::size_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(T));
}

std::uint64_t TickSerial() noexcept {
    return g_tickSerial->load(std::memory_order_relaxed);
}

std::int32_t Saturate(const std::int64_t value) noexcept {
    if (value > std::numeric_limits<std::int32_t>::max()) {
        ++g_stats.mergeOverflows;
        return std::numeric_limits<std::int32_t>::max();
    }
    if (value < std::numeric_limits<std::int32_t>::min()) {
        ++g_stats.mergeOverflows;
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(value);
}

struct ProcessWindowSearch {
    DWORD processId = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindProcessWindow(const HWND window, const LPARAM parameter) noexcept {
    auto* const search = reinterpret_cast<ProcessWindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    search->window = window;
    return FALSE;
}

// SDL registers the raw mouse and handles WM_INPUT itself; this only observes
// each relative packet before forwarding the message unchanged.
LRESULT CALLBACK RawMouseWindowProc(const HWND window, const UINT message,
                                    const WPARAM wParam, const LPARAM lParam) noexcept {
    if (message == WM_INPUT) {
        RAWINPUT input{};
        UINT bytes = sizeof(input);
        const UINT copied = GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
                                            RID_INPUT, &input, &bytes,
                                            sizeof(RAWINPUTHEADER));
        if (copied != static_cast<UINT>(-1) && copied >= sizeof(RAWINPUTHEADER) &&
            input.header.dwType == RIM_TYPEMOUSE &&
            (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
            const std::int32_t x = input.data.mouse.lLastX;
            const std::int32_t y = input.data.mouse.lLastY;
            g_rawActive.store(true, std::memory_order_release);
            g_rawPacketCount.fetch_add(1, std::memory_order_relaxed);
            g_rawSinceRenderX.fetch_add(x, std::memory_order_relaxed);
            g_rawSinceRenderY.fetch_add(y, std::memory_order_relaxed);
            g_pendingX.fetch_add(x, std::memory_order_relaxed);
            g_pendingY.fetch_add(y, std::memory_order_relaxed);
        }
    }
    const WNDPROC original = g_rawOriginalWindowProc;
    if (original == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return CallWindowProcW(original, window, message, wParam, lParam);
}

bool EnsureRawTapInstalled() noexcept {
    if (g_rawTapInstalled.load(std::memory_order_acquire)) {
        return true;
    }
    const DWORD processId = GetCurrentProcessId();
    HWND window = GetForegroundWindow();
    DWORD windowProcessId = 0;
    if (window != nullptr) {
        GetWindowThreadProcessId(window, &windowProcessId);
    }
    if (window == nullptr || windowProcessId != processId) {
        ProcessWindowSearch search{processId, nullptr};
        EnumWindows(&FindProcessWindow, reinterpret_cast<LPARAM>(&search));
        window = search.window;
    }
    if (window == nullptr ||
        GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&RawMouseWindowProc));
    if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        return false;
    }
    g_rawOriginalWindowProc = reinterpret_cast<WNDPROC>(previous);
    g_rawWindow = window;
    g_rawTapInstalled.store(true, std::memory_order_release);
    log::Write(log::Level::info,
               L"Mouse look: raw-mouse packet tap installed on the SDL game "
               L"window; SDL still owns registration, buttons, focus, and menus.");
    return true;
}

void DrainRawMouseMessages() noexcept {
    const HWND window = g_rawWindow;
    if (window == nullptr || !g_rawTapInstalled.load(std::memory_order_acquire)) {
        return;
    }
    std::uint32_t dispatched = 0;
    MSG message{};
    while (dispatched < 8192U &&
           PeekMessageW(&message, window, WM_INPUT, WM_INPUT, PM_REMOVE)) {
        DispatchMessageW(&message);
        ++dispatched;
    }
}

// cLuxInputHandler rounds with cMath::RoundToInt, which is floor(x + 0.5) on a
// single-precision product.
std::int64_t RoundedMousePixels(const std::int64_t raw, const float sensitivity) noexcept {
    const float scaled = static_cast<float>(raw) * sensitivity;
    if (!std::isfinite(scaled) || std::fabs(scaled) > 1.0e9F) {
        return 0;
    }
    return static_cast<std::int64_t>(std::floor(static_cast<double>(scaled) + 0.5));
}

bool DequantizePlayerLookAmount(const float stockAmount,
                                const std::int32_t rawCounts,
                                const bool pitch,
                                float& adjustedAmount) noexcept {
    adjustedAmount = stockAmount;
    if (!g_inPlayerInput || !g_continuousLookThisCall || g_readsThisCall != 1U ||
        rawCounts == 0 || !std::isfinite(stockAmount)) {
        return false;
    }
    const std::int64_t rounded = RoundedMousePixels(rawCounts, g_sensitivityThisCall);
    if (rounded == 0) {
        return false;
    }
    const float candidate = stockAmount / static_cast<float>(rounded);
    const float candidateMagnitude =
        pitch && g_invertThisCall ? candidate : -candidate;
    if (!std::isfinite(candidateMagnitude) || candidateMagnitude <= 0.0000001F ||
        candidateMagnitude >= 0.1F) {
        ++g_stats.dequantizeRejected;
        return false;
    }
    if (!g_playerAmountScaleCalibrated) {
        g_playerAmountPerRoundedCount = candidateMagnitude;
        g_playerAmountScaleCalibrated = true;
    } else {
        const float tolerance =
            std::fmax(g_playerAmountPerRoundedCount * 0.02F, 0.0000001F);
        if (std::fabs(candidateMagnitude - g_playerAmountPerRoundedCount) >
            tolerance) {
            // A resolution change legitimately alters the scale; a gamepad or
            // any other contribution does not. Either way this command keeps
            // the engine's own amount.
            ++g_stats.dequantizeRejected;
            return false;
        }
    }
    const float signedScale = pitch && g_invertThisCall
                                  ? g_playerAmountPerRoundedCount
                                  : -g_playerAmountPerRoundedCount;
    if (!mouse_preview::ContinuousPlayerAmount(
            rawCounts, g_sensitivityThisCall, rounded, stockAmount, signedScale,
            adjustedAmount)) {
        ++g_stats.dequantizeRejected;
        return false;
    }
    ++g_stats.dequantized;
    return true;
}

void PublishAngularCommand(const float sensitivity, const bool invert) noexcept {
    const std::uint64_t serial = TickSerial() + 1;
    AngularCommand command{};
    command.serial = serial;
    command.camera = g_cameraThisCall;
    command.pitch = g_cameraPitchThisCall;
    command.yaw = g_cameraYawThisCall;
    command.valid = g_readsThisCall == 1U && command.camera != nullptr &&
                    std::isfinite(command.pitch) && std::isfinite(command.yaw);
    if (!command.valid) {
        g_commandHistory[serial % kCommandHistoryCapacity] = command;
        ++g_stats.commandsMissing;
        return;
    }
    g_latestPlayerCamera = command.camera;
    g_sensitivity = sensitivity;
    g_invert = invert;

    const std::int64_t roundedX = RoundedMousePixels(g_mouseXThisCall, sensitivity);
    const std::int64_t roundedY = RoundedMousePixels(g_mouseYThisCall, sensitivity);
    const double inputX = g_yawDequantizedThisCall
                              ? static_cast<double>(g_mouseXThisCall) *
                                    static_cast<double>(sensitivity)
                              : static_cast<double>(roundedX);
    const double inputY = g_pitchDequantizedThisCall
                              ? static_cast<double>(g_mouseYThisCall) *
                                    static_cast<double>(sensitivity)
                              : static_cast<double>(roundedY);
    // cLuxPlayer::AddYaw turns the camera by
    //   blend * savedMean + (1 - blend) * lookSpeedMul * amount,
    // so only the second term follows this tick's counts.
    const float blend = g_swayBlendThisCall;
    const float directWeight =
        g_lookModelValidThisCall ? (1.0F - blend) * g_lookSpeedMulThisCall
                                 : 1.0F;
    const float swayYaw = blend * g_swayYawThisCall;
    const float swayPitch = blend * g_swayPitchThisCall;
    command.directWeight = directWeight;
    bool candidateValid = false;
    bool calibratedThisCommand = false;
    float candidate = 0.0F;
    const float directYaw = command.yaw - swayYaw;
    // A mostly swayed or slowed command is too weak a sample to calibrate on.
    if (std::fabs(inputX) >= 0.000001 && std::fabs(directYaw) >= 0.000001F &&
        directWeight >= 0.25F) {
        candidate = directYaw / (static_cast<float>(inputX) * directWeight);
        candidateValid = std::isfinite(candidate) && candidate < -0.000001F &&
                         candidate >= -0.1F;
    }
    if (!g_yawScaleCalibrated && candidateValid) {
        g_yawPerRoundedPixel = candidate;
        g_yawScaleCalibrated = true;
        calibratedThisCommand = true;
        if (!g_calibrationLogged) {
            g_calibrationLogged = true;
            log::Write(log::Level::info,
                       L"Mouse look: direct look scale calibrated from the "
                       L"engine's own command (radians per sensitivity-scaled "
                       L"count=" + std::to_wstring(candidate) + L", sensitivity=" +
                           std::to_wstring(sensitivity) + L").");
        }
    }
    if (g_yawScaleCalibrated) {
        const float pitchScale =
            invert ? -g_yawPerRoundedPixel : g_yawPerRoundedPixel;
        const float expectedYaw =
            swayYaw +
            static_cast<float>(inputX * g_yawPerRoundedPixel) * directWeight;
        const float expectedPitch =
            swayPitch + static_cast<float>(inputY * pitchScale) * directWeight;
        const mouse_preview::CommandShape shape =
            mouse_preview::ClassifyCommandShape(command.pitch, command.yaw,
                                                expectedPitch, expectedYaw);
        if (shape == mouse_preview::CommandShape::direct) {
            command.directYaw = command.yaw - swayYaw;
            command.directPitch = command.pitch - swayPitch;
            if (blend > 0.0F) ++g_stats.commandsSwayModelled;
            if (std::fabs(g_lookSpeedMulThisCall - 1.0F) > 0.01F) {
                ++g_stats.commandsLookSpeedModelled;
            }
        }
        const bool wasAllowed = g_pendingRawGate.Allowed();
        const bool allowed = g_pendingRawGate.Update(shape);
        if (wasAllowed && !allowed) {
            ++g_stats.fallbackEntered;
        }
        if (shape == mouse_preview::CommandShape::direct) {
            ++g_stats.commandsDirect;
        } else if (shape == mouse_preview::CommandShape::transformed ||
                   shape == mouse_preview::CommandShape::invalid) {
            ++g_stats.commandsTransformed;
            // Why the direct relationship failed, for the exit summary.
            const auto matches = [](const float actual,
                                    const float expected) noexcept {
                return std::fabs(actual - expected) <=
                       std::fmax(std::fabs(expected) * 0.5F, 0.00001F);
            };
            if (std::fabs(g_lookSpeedMulThisCall - 1.0F) > 0.01F) {
                ++g_stats.transformedLookSpeed;
            } else if (matches(command.yaw, expectedYaw) &&
                       std::fabs(command.pitch) < std::fabs(expectedPitch)) {
                ++g_stats.transformedPitchShort;
            } else if (std::fabs(command.yaw) < 0.000001F &&
                       std::fabs(command.pitch) < 0.000001F) {
                ++g_stats.transformedBlocked;
            } else {
                ++g_stats.transformedOther;
            }
        }
        // Classify against the existing scale before letting this sample
        // adjust it, so a filtered command can never drag the reference along.
        if (candidateValid && mouse_preview::ShouldUpdateDirectScale(
                                  calibratedThisCommand, wasAllowed, allowed, shape)) {
            g_yawPerRoundedPixel =
                g_yawPerRoundedPixel * 0.875F + candidate * 0.125F;
        }
        command.pendingRawCompatible = allowed;
    }
    g_commandHistory[serial % kCommandHistoryCapacity] = command;
}

hands::Matrix CameraWorldMatrix(const camera::Pose& pose) noexcept {
    const float cp = std::cos(pose.pitch);
    const float sp = std::sin(pose.pitch);
    const float cy = std::cos(pose.yaw);
    const float sy = std::sin(pose.yaw);
    const float cr = std::cos(pose.roll);
    const float sr = std::sin(pose.roll);
    hands::Matrix result = hands::Identity();
    result.values[0] = cy * cr + sy * sp * sr;
    result.values[1] = -cy * sr + sy * sp * cr;
    result.values[2] = sy * cp;
    result.values[4] = cp * sr;
    result.values[5] = cp * cr;
    result.values[6] = -sp;
    result.values[8] = -sy * cr + cy * sp * sr;
    result.values[9] = sy * sr + cy * sp * cr;
    result.values[10] = cy * cp;
    result.values[3] = pose.position.x;
    result.values[7] = pose.position.y;
    result.values[11] = pose.position.z;
    return result;
}

bool BuildPresentationCorrection(const camera::Pose& before,
                                 const camera::Pose& after,
                                 hands::Matrix& correction) noexcept {
    const hands::Matrix oldWorld = CameraWorldMatrix(before);
    const hands::Matrix newWorld = CameraWorldMatrix(after);
    hands::Matrix inverseOld{};
    return hands::InverseRigid(oldWorld, inverseOld) &&
           hands::Multiply(newWorld, inverseOld, correction) &&
           hands::IsRigid(correction);
}

void __fastcall MouseSdlUpdateDetour(void* self, void*) {
    g_originalMouseSdlUpdate(self);
    if (self == nullptr) {
        return;
    }
    g_mouseDevice = self;
    g_mouseThreadId = GetCurrentThreadId();
    (void)EnsureRawTapInstalled();
    ++g_stats.ticks;

    const std::int64_t pendingX = g_pendingX.exchange(0, std::memory_order_relaxed);
    const std::int64_t pendingY = g_pendingY.exchange(0, std::memory_order_relaxed);
    const auto freshX = ReadField<std::int32_t>(self, kMouseRelXOffset);
    const auto freshY = ReadField<std::int32_t>(self, kMouseRelYOffset);
    const std::uint64_t tick = TickSerial();
    const bool previewEligible = g_acquisitionEligible &&
                                 g_lastGameplayTick != 0 &&
                                 tick <= g_lastGameplayTick + 1;
    const bool rawActive = g_rawActive.load(std::memory_order_acquire);
    mouse_preview::TickRoute routeX{};
    mouse_preview::TickRoute routeY{};
    if (rawActive) {
        // SDL's relative state holds the same WM_INPUT reports, grouped by
        // when SDL happened to be pumped. The packet stream is the single
        // source of gameplay motion; SDL keeps everything else.
        routeX.simulationCounts = previewEligible ? pendingX : freshX;
        routeY.simulationCounts = previewEligible ? pendingY : freshY;
        if (previewEligible) {
            if (pendingX != freshX || pendingY != freshY) {
                ++g_stats.rawTickMismatches;
            }
        } else {
            // Menu or cursor motion must not reappear as a look delta.
            g_rawSinceRenderX.store(0, std::memory_order_relaxed);
            g_rawSinceRenderY.store(0, std::memory_order_relaxed);
        }
    } else {
        routeX = mouse_preview::RouteTickCounts(freshX, pendingX, previewEligible);
        routeY = mouse_preview::RouteTickCounts(freshY, pendingY, previewEligible);
    }
    const std::int32_t x = Saturate(routeX.simulationCounts);
    const std::int32_t y = Saturate(routeY.simulationCounts);
    WriteField(self, kMouseRelXOffset, x);
    WriteField(self, kMouseRelYOffset, y);
    if (previewEligible && !rawActive) {
        g_pendingX.fetch_add(routeX.deferredCounts, std::memory_order_relaxed);
        g_pendingY.fetch_add(routeY.deferredCounts, std::memory_order_relaxed);
    }
    if (g_deliveryUnread) {
        ++g_stats.deliveriesNotConsumed;
    }
    g_deliveryUnread = previewEligible;
    if (previewEligible) {
        g_stats.deliveredX += x;
        g_stats.deliveredY += y;
    }
}

MouseDelta* __fastcall MouseSdlGetRelPositionDetour(void* self, void*,
                                                    MouseDelta* result) {
    MouseDelta* const returned = g_originalMouseSdlGetRelPosition(self, result);
    if (result == nullptr || returned != result) {
        ++g_stats.stateInvalid;
        return returned;
    }
    if (g_inPlayerInput) {
        ++g_readsThisCall;
        g_mouseXThisCall = result->x;
        g_mouseYThisCall = result->y;
    }
    if (g_deliveryUnread) {
        g_deliveryUnread = false;
        if (g_inPlayerInput) {
            g_stats.consumedX += result->x;
            g_stats.consumedY += result->y;
        } else {
            // A menu, inventory, or message read took it as cursor motion.
            ++g_stats.deliveriesNotConsumed;
        }
    }
    return returned;
}

bool __fastcall GuiSendMousePosDetour(void* self, void*,
                                      const MouseDelta* absolute,
                                      const MouseDelta* relative) {
    if (self == nullptr || absolute == nullptr || relative == nullptr ||
        ReadField<void*>(self, kGuiFocusedSetOffset) == nullptr) {
        g_guiPositionValid = false;
        return g_originalGuiSendMousePos(self, absolute, relative);
    }
    const mouse_preview::GuiPosition fixed{absolute->x, absolute->y};
    MouseDelta unpresented = *relative;
    // Only when presentation already forwarded part of this movement is the
    // relative amount replaced by the remainder; otherwise the call is stock.
    if (g_guiPositionValid && g_guiObject == self &&
        (g_guiPresentedPosition.x != g_guiFixedPosition.x ||
         g_guiPresentedPosition.y != g_guiFixedPosition.y)) {
        const mouse_preview::GuiPosition remainder =
            mouse_preview::GuiRelativeDelta(g_guiPresentedPosition, fixed);
        unpresented = {remainder.x, remainder.y};
        ++g_guiStats.fixedCorrections;
    }
    const bool result = g_originalGuiSendMousePos(self, absolute, &unpresented);
    g_guiObject = self;
    g_guiFixedPosition = fixed;
    g_guiPresentedPosition = fixed;
    g_guiLastFixedTick = TickSerial();
    g_guiPositionValid = true;
    ++g_guiStats.fixedCalls;
    return result;
}

void CaptureCameraDelta(void* const camera, const float delta, const bool pitch) noexcept {
    if (!g_inPlayerInput || camera == nullptr || !std::isfinite(delta)) {
        return;
    }
    if (g_cameraThisCall == nullptr) {
        g_cameraThisCall = camera;
    } else if (g_cameraThisCall != camera) {
        ++g_stats.stateInvalid;
        return;
    }
    (pitch ? g_cameraPitchThisCall : g_cameraYawThisCall) += delta;
}

void __fastcall LuxPlayerAddYawDetour(void* self, void*, const float amount) {
    float adjusted = amount;
    void* camera = nullptr;
    float before = 0.0F;
    if (g_inPlayerInput && self != nullptr) {
        g_yawDequantizedThisCall = DequantizePlayerLookAmount(
            amount, g_mouseXThisCall, false, adjusted);
        camera = ReadField<void*>(self, kPlayerCameraOffset);
        if (camera != nullptr) {
            before = ReadField<float>(camera, kCameraYawOffset);
        }
    }
    g_originalLuxPlayerAddYaw(self, adjusted);
    if (camera != nullptr) {
        CaptureCameraDelta(
            camera, ReadField<float>(camera, kCameraYawOffset) - before, false);
    }
}

void __fastcall LuxPlayerAddPitchDetour(void* self, void*, const float amount) {
    float adjusted = amount;
    void* camera = nullptr;
    float before = 0.0F;
    if (g_inPlayerInput && self != nullptr) {
        g_pitchDequantizedThisCall = DequantizePlayerLookAmount(
            amount, g_mouseYThisCall, true, adjusted);
        camera = ReadField<void*>(self, kPlayerCameraOffset);
        if (camera != nullptr) {
            before = ReadField<float>(camera, kCameraPitchOffset);
        }
    }
    g_originalLuxPlayerAddPitch(self, adjusted);
    if (camera != nullptr) {
        CaptureCameraDelta(
            camera, ReadField<float>(camera, kCameraPitchOffset) - before, true);
    }
}

void __fastcall LuxUpdateGamePlayerInputDetour(void* self, void*) {
    if (self == nullptr) {
        g_originalLuxUpdateGamePlayerInput(self);
        return;
    }
    const std::uint8_t smooth = ReadField<std::uint8_t>(self, kInputSmoothMouseOffset);
    const std::uint8_t invert = ReadField<std::uint8_t>(self, kInputInvertMouseOffset);
    void* const player = ReadField<void*>(self, kInputPlayerOffset);
    // The handler multiplies the raw count by these in this order before
    // rounding; the infection factor applies only at the maximum level, where
    // the engine also replaces the direct command with a sluggish filter.
    const bool maxInfection =
        player != nullptr && g_playerIsAtMaxInfectionLevel(player);
    float sensitivity =
        ReadField<float>(self, kInputMouseSensitivityOffset) *
        ReadField<float>(self, kInputPointerSpeedOffset);
    if (maxInfection) {
        sensitivity *= ReadField<float>(self, kInputInfectionDropOffset);
    }
    sensitivity *= ReadField<float>(self, kInputLookScaleOffset);
    const bool layoutValid = smooth <= 1U && invert <= 1U && player != nullptr &&
                             std::isfinite(sensitivity) && sensitivity > 0.0F &&
                             sensitivity <= 1000.0F;
    if (!layoutValid) {
        ++g_stats.stateInvalid;
    }
    const bool direct = layoutValid && smooth == 0U && !maxInfection;

    g_readsThisCall = 0;
    g_mouseXThisCall = 0;
    g_mouseYThisCall = 0;
    g_cameraThisCall = nullptr;
    g_cameraPitchThisCall = 0.0F;
    g_cameraYawThisCall = 0.0F;
    g_sensitivityThisCall = sensitivity;
    g_lookSpeedMulThisCall =
        player != nullptr ? ReadField<float>(player, kPlayerLookSpeedMulOffset)
                          : 1.0F;
    // Read before the original runs: these are the values AddYaw and AddPitch
    // are about to use, and the helper's own update changes them later.
    g_lookModelValidThisCall = false;
    g_swayBlendThisCall = 0.0F;
    g_swayYawThisCall = 0.0F;
    g_swayPitchThisCall = 0.0F;
    if (g_swayMapped && player != nullptr) {
        void* const effects =
            ReadField<void*>(player, kPlayerCamDirEffectsOffset);
        if (effects != nullptr) {
            const auto saved =
                ReadField<std::uint32_t>(effects, kSwaySavedCountOffset);
            const float alpha = ReadField<float>(effects, kSwayAlphaOffset);
            const float swayYaw = ReadField<float>(effects, kSwayAddOffset);
            const float swayPitch =
                ReadField<float>(effects, kSwayAddOffset + sizeof(float));
            if (saved <= 100000U && std::isfinite(alpha) && alpha >= 0.0F &&
                alpha <= 1.0F && std::isfinite(swayYaw) &&
                std::isfinite(swayPitch) &&
                std::isfinite(g_lookSpeedMulThisCall) &&
                g_lookSpeedMulThisCall >= 0.0F &&
                g_lookSpeedMulThisCall <= 100.0F) {
                g_lookModelValidThisCall = true;
                g_swayBlendThisCall = saved == 0U ? 0.0F : alpha;
                g_swayYawThisCall = swayYaw;
                g_swayPitchThisCall = swayPitch;
            }
        }
    }
    g_invertThisCall = invert != 0U;
    g_continuousLookThisCall = direct;
    g_yawDequantizedThisCall = false;
    g_pitchDequantizedThisCall = false;
    g_inPlayerInput = true;
    g_originalLuxUpdateGamePlayerInput(self);
    g_inPlayerInput = false;

    // Dead, inactive, or otherwise early-exiting handlers never read the
    // mouse; those ticks and the smoothed/sluggish modes keep stock input.
    const bool eligible = direct && g_readsThisCall == 1U;
    g_acquisitionEligible = eligible;
    g_lastGameplayTick = TickSerial();
    if (eligible) {
        ++g_stats.eligibleTicks;
        PublishAngularCommand(sensitivity, invert != 0U);
    }
}

} // namespace

bool Install(const Setup& setup, std::wstring& error) {
    const target::MouseMapping& mapping = setup.mapping;
    if (setup.tickSerial == nullptr || setup.camera == nullptr ||
        mapping.mouseUpdate.address == nullptr ||
        mapping.mouseGetRelPosition.address == nullptr ||
        mapping.updateGamePlayerInput.address == nullptr ||
        mapping.playerAddYaw.address == nullptr ||
        mapping.playerAddPitch.address == nullptr ||
        mapping.playerIsAtMaxInfectionLevel.address == nullptr) {
        error = L"mouse look received an incomplete mapping";
        return false;
    }
    const HMODULE sdl = GetModuleHandleW(L"SDL2.dll");
    if (sdl != nullptr) {
        g_sdlGetRelativeMouseState = reinterpret_cast<SdlGetRelativeMouseState>(
            GetProcAddress(sdl, "SDL_GetRelativeMouseState"));
        g_sdlPumpEvents =
            reinterpret_cast<SdlPumpEvents>(GetProcAddress(sdl, "SDL_PumpEvents"));
    }
    if (g_sdlGetRelativeMouseState == nullptr || g_sdlPumpEvents == nullptr) {
        error = L"SDL_GetRelativeMouseState/SDL_PumpEvents could not be resolved";
        return false;
    }
    g_tickSerial = setup.tickSerial;
    g_camera = setup.camera;
    g_swayMapped = mapping.camDirEffectsYawAdd.address != nullptr;
    g_playerIsAtMaxInfectionLevel =
        reinterpret_cast<LuxPlayerIsAtMaxInfectionLevel>(
            mapping.playerIsAtMaxInfectionLevel.address);

    struct Hook {
        void* target;
        void* detour;
        void** original;
        const wchar_t* name;
    };
    const std::array<Hook, 5> hooks{{
        {mapping.mouseUpdate.address, &MouseSdlUpdateDetour,
         reinterpret_cast<void**>(&g_originalMouseSdlUpdate), L"cMouseSDL::Update"},
        {mapping.mouseGetRelPosition.address, &MouseSdlGetRelPositionDetour,
         reinterpret_cast<void**>(&g_originalMouseSdlGetRelPosition),
         L"cMouseSDL::GetRelPosition"},
        {mapping.updateGamePlayerInput.address, &LuxUpdateGamePlayerInputDetour,
         reinterpret_cast<void**>(&g_originalLuxUpdateGamePlayerInput),
         L"cLuxInputHandler::UpdateGamePlayerInput"},
        {mapping.playerAddYaw.address, &LuxPlayerAddYawDetour,
         reinterpret_cast<void**>(&g_originalLuxPlayerAddYaw), L"cLuxPlayer::AddYaw"},
        {mapping.playerAddPitch.address, &LuxPlayerAddPitchDetour,
         reinterpret_cast<void**>(&g_originalLuxPlayerAddPitch),
         L"cLuxPlayer::AddPitch"},
    }};
    std::size_t created = 0;
    MH_STATUS status = MH_OK;
    for (; created < hooks.size() && status == MH_OK; ++created) {
        status = MH_CreateHook(hooks[created].target, hooks[created].detour,
                               hooks[created].original);
        if (status != MH_OK) {
            error = std::wstring(L"MH_CreateHook(") + hooks[created].name +
                    L") failed";
            break;
        }
        status = MH_QueueEnableHook(hooks[created].target);
        if (status != MH_OK) {
            error = std::wstring(L"MH_QueueEnableHook(") + hooks[created].name +
                    L") failed";
            ++created;
            break;
        }
    }
    if (status != MH_OK) {
        for (std::size_t index = 0; index < created; ++index) {
            MH_RemoveHook(hooks[index].target);
        }
        g_tickSerial = nullptr;
        g_camera = nullptr;
        return false;
    }
    if (mapping.guiSendMousePos.address != nullptr) {
        // Optional: a failure here leaves only the GUI cursor stock.
        g_sdlGetRelativeMouseMode = reinterpret_cast<SdlGetRelativeMouseMode>(
            GetProcAddress(sdl, "SDL_GetRelativeMouseMode"));
        if (g_sdlGetRelativeMouseMode == nullptr ||
            MH_CreateHook(mapping.guiSendMousePos.address,
                          &GuiSendMousePosDetour,
                          reinterpret_cast<void**>(&g_originalGuiSendMousePos)) !=
                MH_OK) {
            g_originalGuiSendMousePos = nullptr;
            log::Write(log::Level::warning,
                       L"GUI cursor preview disabled: cGui::SendMousePos or "
                       L"SDL_GetRelativeMouseMode could not be hooked.");
        } else if (MH_QueueEnableHook(mapping.guiSendMousePos.address) != MH_OK) {
            MH_RemoveHook(mapping.guiSendMousePos.address);
            g_originalGuiSendMousePos = nullptr;
            log::Write(log::Level::warning,
                       L"GUI cursor preview disabled: cGui::SendMousePos hook "
                       L"could not be queued.");
        } else {
            log::Write(log::Level::info,
                       L"Presentation-rate GUI cursor enabled for menus, the "
                       L"inventory, and the journal; clicks stay on the fixed "
                       L"update.");
        }
    }
    return true;
}

void PresentGuiCursor() noexcept {
    if (g_originalGuiSendMousePos == nullptr || !g_guiPositionValid ||
        g_guiObject == nullptr || g_tickSerial == nullptr) {
        return;
    }
    // The newest fixed update did not address the GUI, so it no longer owns
    // the mouse.
    if (TickSerial() > g_guiLastFixedTick + 1) {
        g_guiPositionValid = false;
        return;
    }
    const HWND window = g_rawWindow;
    if (window == nullptr || !g_rawTapInstalled.load(std::memory_order_acquire)) {
        return;
    }
    if (g_mouseThreadId == 0 || GetCurrentThreadId() != g_mouseThreadId) {
        ++g_stats.threadMismatches;
        return;
    }
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= client.left ||
        client.bottom <= client.top) {
        ++g_guiStats.skippedInvalid;
        return;
    }
    mouse_preview::GuiPosition position{};
    if (g_sdlGetRelativeMouseMode() != 0) {
        // Captured GUI states: SDL moves its cursor position by the raw
        // packets, so the same packets predict it between fixed updates.
        DrainRawMouseMessages();
        if (!g_rawActive.load(std::memory_order_acquire)) {
            return;
        }
        position = mouse_preview::PreviewGuiPosition(
            g_guiFixedPosition, g_pendingX.load(std::memory_order_relaxed),
            g_pendingY.load(std::memory_order_relaxed),
            client.right - client.left, client.bottom - client.top);
        ++g_guiStats.relativeSamples;
    } else {
        // The main and pause menus release the mouse; the OS cursor then
        // carries desktop acceleration that raw counts would not reproduce.
        POINT cursor{};
        if (GetForegroundWindow() != window || !GetCursorPos(&cursor) ||
            !ScreenToClient(window, &cursor) || !PtInRect(&client, cursor)) {
            ++g_guiStats.skippedOutside;
            return;
        }
        position = {cursor.x, cursor.y};
        ++g_guiStats.absoluteSamples;
    }
    const mouse_preview::GuiPosition relative =
        mouse_preview::GuiRelativeDelta(g_guiPresentedPosition, position);
    if (relative.x == 0 && relative.y == 0) {
        return;
    }
    const MouseDelta absolute{position.x, position.y};
    const MouseDelta movement{relative.x, relative.y};
    (void)g_originalGuiSendMousePos(g_guiObject, &absolute, &movement);
    g_guiPresentedPosition = position;
    ++g_guiStats.presentedMoves;
}

void AcquirePresentation() noexcept {
    if (g_tickSerial == nullptr || !g_acquisitionEligible ||
        g_mouseDevice == nullptr) {
        return;
    }
    const std::uint64_t tick = TickSerial();
    if (g_lastGameplayTick == 0 || tick > g_lastGameplayTick + 1) {
        return;
    }
    if (g_mouseThreadId == 0 || GetCurrentThreadId() != g_mouseThreadId) {
        ++g_stats.threadMismatches;
        return;
    }
    // The engine empties SDL's queue only inside its fixed update. Dispatch
    // just the raw-mouse messages around SDL's own pump so relative packets
    // arrive at presentation rate; every other message keeps its ordering.
    DrainRawMouseMessages();
    g_sdlPumpEvents();
    DrainRawMouseMessages();
    if (g_rawActive.load(std::memory_order_acquire)) {
        g_rawSinceRenderX.store(0, std::memory_order_relaxed);
        g_rawSinceRenderY.store(0, std::memory_order_relaxed);
    } else {
        // Until the packet tap proves that WM_INPUT reaches this window, fall
        // back to SDL's accumulator.
        int x = 0;
        int y = 0;
        (void)g_sdlGetRelativeMouseState(&x, &y);
        g_pendingX.fetch_add(x, std::memory_order_relaxed);
        g_pendingY.fetch_add(y, std::memory_order_relaxed);
    }
    ++g_stats.presentationPolls;
}

bool ApplyPreview(const clock::Sample& sample,
                  const std::uint64_t currentSerial,
                  hands::Matrix& handsCorrection) noexcept {
    handsCorrection = hands::Identity();
    if (g_camera == nullptr || !g_acquisitionEligible) {
        return false;
    }
    void* const camera = g_camera->ActiveCamera();
    if (camera == nullptr || camera != g_latestPlayerCamera ||
        sample.kind != clock::SampleKind::interpolated || sample.newerSerial == 0 ||
        currentSerial < sample.newerSerial ||
        currentSerial - sample.newerSerial >= kCommandHistoryCapacity) {
        ++g_stats.previewNoCommand;
        return false;
    }

    // The interpolated camera lags the newest authoritative pose by the part
    // of the newest look command not yet blended in. Adding that remainder
    // shows the authoritative orientation; adding the pending counts on top
    // shows where the next fixed update will put it.
    float pitch = 0.0F;
    float yaw = 0.0F;
    bool pendingRawCompatible = false;
    float pendingWeight = 1.0F;
    for (std::uint64_t serial = sample.newerSerial; serial <= currentSerial; ++serial) {
        const AngularCommand& command =
            g_commandHistory[serial % kCommandHistoryCapacity];
        if (!command.valid || command.serial != serial || command.camera != camera) {
            ++g_stats.previewNoCommand;
            return false;
        }
        if (serial == sample.newerSerial) {
            // Only the count-driven part is shown early; the sway part of the
            // command is a slow average and keeps the clock's interpolation.
            const float weight = static_cast<float>(1.0 - sample.alpha);
            pitch += command.directPitch * weight;
            yaw += command.directYaw * weight;
        } else {
            pitch += command.pitch;
            yaw += command.yaw;
        }
        pendingRawCompatible = command.pendingRawCompatible;
        pendingWeight = command.directWeight;
    }
    if (!pendingRawCompatible) {
        // A filtered or constrained command owns the camera entirely until
        // the direct relationship has been stable again.
        ++g_stats.previewBypassed;
        return false;
    }
    const std::int64_t pendingX = g_pendingX.load(std::memory_order_relaxed);
    const std::int64_t pendingY = g_pendingY.load(std::memory_order_relaxed);
    if ((pendingX != 0 || pendingY != 0) && g_yawScaleCalibrated) {
        const float pitchScale = g_invert ? -g_yawPerRoundedPixel : g_yawPerRoundedPixel;
        float pendingYaw = 0.0F;
        float pendingPitch = 0.0F;
        if (mouse_preview::ContinuousAngle(pendingX, g_sensitivity,
                                           g_yawPerRoundedPixel, pendingYaw) &&
            mouse_preview::ContinuousAngle(pendingY, g_sensitivity, pitchScale,
                                           pendingPitch)) {
            yaw += pendingYaw * pendingWeight;
            pitch += pendingPitch * pendingWeight;
        }
    }
    if (std::fabs(pitch) < 0.0000001F && std::fabs(yaw) < 0.0000001F) {
        return false;
    }
    camera::Pose before{};
    camera::Pose after{};
    if (!g_camera->ApplyAngularOffset(pitch, yaw, &before, &after)) {
        ++g_stats.previewFailed;
        return false;
    }
    ++g_stats.previewApplied;
    if (!BuildPresentationCorrection(before, after, handsCorrection)) {
        handsCorrection = hands::Identity();
        return false;
    }
    return true;
}

void LogSummary() {
    if (g_tickSerial == nullptr) {
        return;
    }
    const Stats& s = g_stats;
    log::Write(
        log::Level::info,
        L"Mouse look summary: ticks=" + std::to_wstring(s.ticks) +
            L" eligible=" + std::to_wstring(s.eligibleTicks) +
            L" presentationPolls=" + std::to_wstring(s.presentationPolls) +
            L" rawPackets=" +
            std::to_wstring(g_rawPacketCount.load(std::memory_order_relaxed)) +
            L" rawActive=" +
            std::to_wstring(g_rawActive.load(std::memory_order_relaxed) ? 1 : 0) +
            L" delivered=(" + std::to_wstring(s.deliveredX) + L"," +
            std::to_wstring(s.deliveredY) + L") consumed=(" +
            std::to_wstring(s.consumedX) + L"," + std::to_wstring(s.consumedY) +
            L") deliveriesNotConsumed=" +
            std::to_wstring(s.deliveriesNotConsumed) +
            L" rawTickMismatches=" + std::to_wstring(s.rawTickMismatches) +
            L" commands(direct/transformed/missing)=" +
            std::to_wstring(s.commandsDirect) + L"/" +
            std::to_wstring(s.commandsTransformed) + L"/" +
            std::to_wstring(s.commandsMissing) +
            L" modelled(sway/lookSpeed)=" +
            std::to_wstring(s.commandsSwayModelled) + L"/" +
            std::to_wstring(s.commandsLookSpeedModelled) +
            L" swayMapped=" + std::to_wstring(g_swayMapped ? 1 : 0) +
            L" transformed(lookSpeed/pitchShort/blocked/other)=" +
            std::to_wstring(s.transformedLookSpeed) + L"/" +
            std::to_wstring(s.transformedPitchShort) + L"/" +
            std::to_wstring(s.transformedBlocked) + L"/" +
            std::to_wstring(s.transformedOther) +
            L" dequantized(ok/rejected)=" + std::to_wstring(s.dequantized) + L"/" +
            std::to_wstring(s.dequantizeRejected) +
            L" preview(applied/bypassed/noCommand/failed)=" +
            std::to_wstring(s.previewApplied) + L"/" +
            std::to_wstring(s.previewBypassed) + L"/" +
            std::to_wstring(s.previewNoCommand) + L"/" +
            std::to_wstring(s.previewFailed) +
            L" fallbackEntered=" + std::to_wstring(s.fallbackEntered) +
            L" invalid=" + std::to_wstring(s.stateInvalid) +
            L" threadMismatch=" + std::to_wstring(s.threadMismatches) +
            L" overflow=" + std::to_wstring(s.mergeOverflows) + L".");
    if (g_originalGuiSendMousePos != nullptr) {
        const GuiStats& g = g_guiStats;
        log::Write(
            log::Level::info,
            L"GUI cursor summary: fixedCalls=" + std::to_wstring(g.fixedCalls) +
                L" fixedCorrections=" + std::to_wstring(g.fixedCorrections) +
                L" samples(relative/absolute)=" +
                std::to_wstring(g.relativeSamples) + L"/" +
                std::to_wstring(g.absoluteSamples) +
                L" presentedMoves=" + std::to_wstring(g.presentedMoves) +
                L" skipped(outside/invalid)=" +
                std::to_wstring(g.skippedOutside) + L"/" +
                std::to_wstring(g.skippedInvalid) + L".");
    }
}

} // namespace amfp::mouse_look
