#include "node_animation_runtime.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace amfp::node_animation {
namespace {

constexpr std::size_t kSubMeshLocalNodeOffset = 0x37C;
constexpr float kMaximumPositionDeltaPerTick = 5.0F;
constexpr float kMaximumOrientationDeltaPerTick = 2.09439510239F;

std::size_t HashPointer(const void* const pointer) noexcept {
    std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer) >> 4U;
    value ^= value >> 11U;
    return static_cast<std::size_t>(value);
}

template <typename T>
T ReadMappedField(const void* const base, const std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset,
                sizeof(value));
    return value;
}

bool IsDiscontinuous(const hands::Matrix& older,
                     const hands::Matrix& newer,
                     const std::uint64_t serialDelta) noexcept {
    const float dx = newer.values[3] - older.values[3];
    const float dy = newer.values[7] - older.values[7];
    const float dz = newer.values[11] - older.values[11];
    const float allowedPosition =
        kMaximumPositionDeltaPerTick * static_cast<float>(serialDelta);
    if (dx * dx + dy * dy + dz * dz > allowedPosition * allowedPosition) {
        return true;
    }
    const float allowedAngle =
        kMaximumOrientationDeltaPerTick * static_cast<float>(serialDelta);
    return hands::AffineOrientationDistance(older, newer) > allowedAngle;
}

} // namespace

void Runtime::BeginRender(const clock::Sample& sample,
                          const std::uint64_t tickSerial) noexcept {
    sample_ = sample;
    tickSerial_ = tickSerial;
    ++renderCount_;
    // The engine never reports a submesh's destruction, so entries from
    // earlier maps would otherwise accumulate for the whole session and turn
    // every lookup into a long probe. Histories rebuild in two ticks, so the
    // whole table is simply dropped once it is a quarter full. The cooldown
    // bounds the cost if one scene genuinely holds that many animated nodes.
    if (occupied_ >= kRecycleOccupancy &&
        renderCount_ - lastRecycleRender_ >= kRecycleCooldownRenders) {
        ClearEntries();
        lastRecycleRender_ = renderCount_;
    }
}

void Runtime::ClearEntries() noexcept {
    // Cleared in place: assigning a whole-table temporary would need about a
    // megabyte of the render thread's stack.
    for (Entry& entry : entries_) entry = {};
    occupied_ = 0;
}

Runtime::Entry* Runtime::FindOrInsert(void* const subMesh,
                                     void* const localNode) noexcept {
    const std::size_t start = HashPointer(subMesh) % entries_.size();
    for (std::size_t probe = 0; probe < kMaximumProbe; ++probe) {
        Entry& entry = entries_[(start + probe) % entries_.size()];
        if (entry.subMesh == subMesh) {
            if (entry.localNode != localNode) {
                entry = {};
                entry.subMesh = subMesh;
                entry.localNode = localNode;
            }
            return &entry;
        }
        if (entry.subMesh == nullptr) {
            entry.subMesh = subMesh;
            entry.localNode = localNode;
            ++occupied_;
            return &entry;
        }
    }
    return nullptr;
}

void Runtime::ResetHistory(Entry& entry) noexcept {
    entry.snapshots = {};
    entry.count = 0;
    entry.pairChanged = false;
}

Observation Runtime::PublishAndEvaluate(
    Entry& entry,
    const hands::Matrix& authoritative,
    const hands::Matrix*& result) noexcept {
    bool captured = false;
    bool changed = false;
    if (!entry.hasGeneration || entry.generation != sample_.generation) {
        ResetHistory(entry);
        entry.generation = sample_.generation;
        entry.hasGeneration = true;
    }
    const bool needsCapture = entry.count == 0 ||
        entry.snapshots[entry.count - 1].serial != tickSerial_;
    if (needsCapture) {
        if (!hands::IsAffine(authoritative)) {
            ResetHistory(entry);
            return {OverrideResult::invalid};
        }
        if (entry.count == 0) {
            entry.snapshots[0] = {tickSerial_, authoritative};
            entry.count = 1;
            captured = true;
        } else {
            const Snapshot& latest = entry.snapshots[entry.count - 1];
            if (tickSerial_ < latest.serial) {
                ResetHistory(entry);
                entry.snapshots[0] = {tickSerial_, authoritative};
                entry.count = 1;
                captured = true;
            } else {
                const std::uint64_t serialDelta = tickSerial_ - latest.serial;
                changed = !hands::NearlyEqual(latest.matrix, authoritative);
                if (serialDelta >
                        clock::PresentationClock::kMaximumBridgedSerialDelta ||
                    IsDiscontinuous(latest.matrix, authoritative,
                                    serialDelta)) {
                    ResetHistory(entry);
                    entry.snapshots[0] = {tickSerial_, authoritative};
                    entry.count = 1;
                } else if (entry.count == 1) {
                    entry.snapshots[1] = {tickSerial_, authoritative};
                    entry.count = 2;
                    entry.pairChanged = changed;
                } else {
                    entry.snapshots[0] = entry.snapshots[1];
                    entry.snapshots[1] = {tickSerial_, authoritative};
                    entry.pairChanged = changed;
                }
                captured = true;
            }
        }
    }

    if (sample_.kind != clock::SampleKind::interpolated ||
        sample_.olderSerial == sample_.newerSerial) {
        return {OverrideResult::clockUnavailable, captured, changed};
    }
    if (entry.count != 2 || entry.generation != sample_.generation ||
        entry.snapshots[0].serial != sample_.olderSerial ||
        entry.snapshots[1].serial != sample_.newerSerial) {
        return {OverrideResult::pairMissing, captured, changed};
    }
    if (!entry.pairChanged) {
        return {OverrideResult::stable, captured, false};
    }
    if (!hands::InterpolateAffine(entry.snapshots[0].matrix,
                                  entry.snapshots[1].matrix,
                                  sample_.alpha, entry.output)) {
        ResetHistory(entry);
        return {OverrideResult::invalid, captured, changed};
    }
    result = &entry.output;
    return {OverrideResult::applied, captured, true};
}

Observation Runtime::OverrideModelMatrix(
    void* const subMesh,
    const hands::Matrix* const authoritative,
    const hands::Matrix*& result) noexcept {
    result = authoritative;
    if (subMesh == nullptr) return {};
    void* const localNode =
        ReadMappedField<void*>(subMesh, kSubMeshLocalNodeOffset);
    if (localNode == nullptr) return {};
    if (authoritative == nullptr) return {OverrideResult::invalid};
    Entry* const entry = FindOrInsert(subMesh, localNode);
    if (entry == nullptr) return {OverrideResult::capacityExhausted};
    return PublishAndEvaluate(*entry, *authoritative, result);
}

Observation Runtime::OverrideOwnedModelMatrix(
    void* const subMesh,
    void* const owner,
    const hands::Matrix* const authoritative,
    const hands::Matrix*& result) noexcept {
    result = authoritative;
    if (subMesh == nullptr || owner == nullptr) return {};
    if (authoritative == nullptr) return {OverrideResult::invalid};
    Entry* const entry = FindOrInsert(subMesh, owner);
    if (entry == nullptr) return {OverrideResult::capacityExhausted};
    return PublishAndEvaluate(*entry, *authoritative, result);
}

void Runtime::Reset() noexcept {
    ClearEntries();
    sample_ = {};
    tickSerial_ = 0;
    renderCount_ = 0;
    lastRecycleRender_ = 0;
}

const wchar_t* OverrideResultName(const OverrideResult result) noexcept {
    switch (result) {
    case OverrideResult::notTarget: return L"not-target";
    case OverrideResult::applied: return L"applied";
    case OverrideResult::stable: return L"stable";
    case OverrideResult::clockUnavailable: return L"clock-unavailable";
    case OverrideResult::pairMissing: return L"pair-missing";
    case OverrideResult::invalid: return L"invalid";
    case OverrideResult::capacityExhausted: return L"capacity-exhausted";
    }
    return L"unknown";
}

} // namespace amfp::node_animation
