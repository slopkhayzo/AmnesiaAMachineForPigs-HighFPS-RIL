#include "particle_runtime.h"

#include <cmath>
#include <cstring>

namespace amfp::particle {
namespace {

// Retail iParticleEmitter layout, pinned by the signatures in
// target_mapping.cpp: mvParticles begin/end, mlNumOfParticles, mlMaxParticles.
constexpr std::size_t kParticlesBeginOffset = 0x3A0;
constexpr std::size_t kParticlesEndOffset = 0x3A4;
constexpr std::size_t kParticleCountOffset = 0x3B0;
constexpr std::size_t kParticleMaximumOffset = 0x3B4;
// cParticle begins with mvPos followed by mvLastPos.
constexpr std::size_t kPositionOffset = 0x00;
constexpr std::size_t kLastPositionOffset = 0x0C;

template <typename T>
T ReadAt(const void* base, const std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset,
                sizeof(value));
    return value;
}

template <typename T>
void WriteAt(void* base, const std::size_t offset, const T& value) noexcept {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

bool IsFinite(const Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

std::size_t HashPointer(const void* pointer) noexcept {
    std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer) >> 4U;
    value ^= value >> 11U;
    return static_cast<std::size_t>(value);
}

std::uint32_t Mix(std::uint32_t hash, const void* data,
                  const std::size_t bytes) noexcept {
    const auto* const cursor = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index + 4 <= bytes; index += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, cursor + index, sizeof(word));
        hash = (hash ^ word) * 16777619U;
    }
    return hash;
}

} // namespace

void Runtime::BeginRender(const clock::Sample& sample,
                          const std::uint64_t currentSerial) noexcept {
    serial_ = currentSerial;
    // A particle only stores the newest step, so the clock pair has to end
    // at the newest fixed update.
    active_ = sample.kind == clock::SampleKind::interpolated &&
              currentSerial != 0 && sample.newerSerial == currentSerial &&
              std::isfinite(sample.alpha) && sample.alpha >= 0.0 &&
              sample.alpha <= 1.0;
    alpha_ = active_ ? static_cast<float>(sample.alpha) : 0.0F;
}

void Runtime::EndRender() noexcept {
    active_ = false;
}

Runtime::Slot& Runtime::Track(void* const emitter) noexcept {
    const std::size_t start = HashPointer(emitter) % slots_.size();
    Slot* replace = &slots_[start];
    for (std::size_t probe = 0; probe < kMaximumProbe; ++probe) {
        Slot& slot = slots_[(start + probe) % slots_.size()];
        if (slot.emitter == emitter) return slot;
        if (slot.observedSerial < replace->observedSerial) replace = &slot;
    }
    // The least recently drawn neighbour makes room; a destroyed emitter is
    // forgotten this way without any lifetime hook.
    *replace = {};
    replace->emitter = emitter;
    return *replace;
}

ApplyResult Runtime::Apply(void* const emitter) noexcept {
    ++stats_.calls;
    if (!active_ || savedCount_ != 0 || emitter == nullptr) {
        ++stats_.inactive;
        return ApplyResult::inactive;
    }
    void** const begin = ReadAt<void**>(emitter, kParticlesBeginOffset);
    void** const end = ReadAt<void**>(emitter, kParticlesEndOffset);
    const std::uint32_t count =
        ReadAt<std::uint32_t>(emitter, kParticleCountOffset);
    const std::uint32_t maximum =
        ReadAt<std::uint32_t>(emitter, kParticleMaximumOffset);
    if (count == 0) {
        ++stats_.empty;
        return ApplyResult::empty;
    }
    const std::uintptr_t beginAddress = reinterpret_cast<std::uintptr_t>(begin);
    const std::uintptr_t endAddress = reinterpret_cast<std::uintptr_t>(end);
    if (begin == nullptr || endAddress < beginAddress || count > maximum ||
        (endAddress - beginAddress) / sizeof(void*) < count) {
        ++stats_.invalid;
        return ApplyResult::invalid;
    }
    if (count > kMaximumParticles) {
        ++stats_.capacity;
        return ApplyResult::capacity;
    }

    Slot& slot = Track(emitter);
    if (slot.observedSerial != serial_) {
        // First draw after a fixed update: decide once whether the emitter
        // advanced in it. A paused, sleeping, or culled emitter keeps its old
        // pair, and replaying that pair would make it shake.
        std::uint32_t signature = 2166136261U ^ count;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (begin[index] == nullptr) {
                ++stats_.invalid;
                return ApplyResult::invalid;
            }
            const Vec3 position = ReadAt<Vec3>(begin[index], kPositionOffset);
            signature = Mix(signature, &position, sizeof(position));
        }
        const bool consecutive = slot.observedSerial != 0 &&
                                 slot.observedSerial + 1 == serial_;
        slot.movedSerial =
            consecutive && signature != slot.signature ? serial_ : 0;
        slot.observedSerial = serial_;
        slot.signature = signature;
        if (!consecutive) {
            ++stats_.unknown;
            return ApplyResult::unknown;
        }
    }
    if (slot.movedSerial != serial_) {
        ++stats_.stationary;
        return ApplyResult::stationary;
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        void* const particle = begin[index];
        if (particle == nullptr) {
            ++stats_.invalid;
            return ApplyResult::invalid;
        }
        Saved& saved = saved_[index];
        saved.particle = particle;
        saved.position = ReadAt<Vec3>(particle, kPositionOffset);
        saved.lastPosition = ReadAt<Vec3>(particle, kLastPositionOffset);
        if (!IsFinite(saved.position) || !IsFinite(saved.lastPosition)) {
            ++stats_.invalid;
            return ApplyResult::invalid;
        }
    }
    // Both positions move back by the part of the step not yet presented, so
    // trail and line particles keep the length the engine gave them.
    const float remaining = 1.0F - alpha_;
    for (std::uint32_t index = 0; index < count; ++index) {
        const Saved& saved = saved_[index];
        const Vec3 offset{
            (saved.position.x - saved.lastPosition.x) * remaining,
            (saved.position.y - saved.lastPosition.y) * remaining,
            (saved.position.z - saved.lastPosition.z) * remaining};
        WriteAt(saved.particle, kPositionOffset,
                Vec3{saved.position.x - offset.x, saved.position.y - offset.y,
                     saved.position.z - offset.z});
        WriteAt(saved.particle, kLastPositionOffset,
                Vec3{saved.lastPosition.x - offset.x,
                     saved.lastPosition.y - offset.y,
                     saved.lastPosition.z - offset.z});
    }
    savedCount_ = count;
    ++stats_.applied;
    stats_.positions += count;
    return ApplyResult::applied;
}

void Runtime::Restore() noexcept {
    for (std::size_t index = 0; index < savedCount_; ++index) {
        const Saved& saved = saved_[index];
        WriteAt(saved.particle, kPositionOffset, saved.position);
        WriteAt(saved.particle, kLastPositionOffset, saved.lastPosition);
    }
    savedCount_ = 0;
}

} // namespace amfp::particle
