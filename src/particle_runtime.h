#pragma once

#include "presentation_clock.h"

#include <array>
#include <cstddef>
#include <cstdint>

// Render-only particle interpolation for AMFP.
//
// iParticleEmitter advances every particle once per fixed update and keeps
// the position it had before that step (cParticle::mvLastPos). While the
// emitter rebuilds its vertex buffer for a view, each particle is shown
// between those two positions on the shared presentation clock and the
// authoritative values are put back immediately afterwards. Emission,
// motion, collision, and lifetime are never touched.
namespace amfp::particle {

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

enum class ApplyResult : std::uint8_t {
    inactive,   // no interpolated clock pair, or a nested call
    empty,      // no live particles
    unknown,    // first sight of this emitter in consecutive ticks
    stationary, // the emitter did not advance in the newest fixed update
    applied,
    invalid,
    capacity,
};

struct Stats {
    std::uint64_t calls = 0;
    std::uint64_t inactive = 0;
    std::uint64_t empty = 0;
    std::uint64_t unknown = 0;
    std::uint64_t stationary = 0;
    std::uint64_t applied = 0;
    std::uint64_t positions = 0;
    std::uint64_t invalid = 0;
    std::uint64_t capacity = 0;
};

class Runtime final {
  public:
    static constexpr std::size_t kMaximumParticles = 4096;

    // `currentSerial` is the newest completed fixed update.
    void BeginRender(const clock::Sample& sample,
                     std::uint64_t currentSerial) noexcept;
    void EndRender() noexcept;

    // Call immediately before the original
    // iParticleEmitter::UpdateGraphicsForViewport. Only `applied` leaves
    // presentation positions in the emitter; Restore must then follow the
    // original call.
    [[nodiscard]] ApplyResult Apply(void* emitter) noexcept;
    void Restore() noexcept;

    [[nodiscard]] const Stats& Statistics() const noexcept { return stats_; }

  private:
    struct Slot {
        void* emitter = nullptr;
        std::uint64_t observedSerial = 0;
        std::uint64_t movedSerial = 0;
        std::uint32_t signature = 0;
    };
    struct Saved {
        void* particle = nullptr;
        Vec3 position{};
        Vec3 lastPosition{};
    };
    static constexpr std::size_t kSlotCount = 1024;
    static constexpr std::size_t kMaximumProbe = 4;

    [[nodiscard]] Slot& Track(void* emitter) noexcept;

    std::array<Slot, kSlotCount> slots_{};
    std::array<Saved, kMaximumParticles> saved_{};
    std::size_t savedCount_ = 0;
    std::uint64_t serial_ = 0;
    float alpha_ = 0.0F;
    bool active_ = false;
    Stats stats_{};
};

} // namespace amfp::particle
