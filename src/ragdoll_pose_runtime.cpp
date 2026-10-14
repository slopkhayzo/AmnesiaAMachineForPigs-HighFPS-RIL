#include "ragdoll_pose_runtime.h"

#include <cstdint>
#include <cstring>

namespace amfp::ragdoll_pose {
namespace {

constexpr std::size_t kSkeletonPhysicsOffset = 0x398;
constexpr std::size_t kBoneMatricesBeginOffset = 0x388;
constexpr std::size_t kBoneMatricesEndOffset = 0x38C;
constexpr std::size_t kBoneMatricesCapacityOffset = 0x390;

template <typename T>
bool ReadAt(const void* base, const std::size_t offset, T& value) noexcept {
    if (base == nullptr) return false;
    const auto* address = static_cast<const std::uint8_t*>(base) + offset;
    std::memcpy(&value, address, sizeof(T));
    return true;
}

struct VectorView {
    hands::Matrix* begin = nullptr;
    hands::Matrix* end = nullptr;
    hands::Matrix* capacity = nullptr;
    std::size_t count = 0;
};

bool ReadVector(void* const mesh, VectorView& view) noexcept {
    if (!ReadAt(mesh, kBoneMatricesBeginOffset, view.begin) ||
        !ReadAt(mesh, kBoneMatricesEndOffset, view.end) ||
        !ReadAt(mesh, kBoneMatricesCapacityOffset, view.capacity) ||
        view.begin == nullptr || view.end == nullptr || view.capacity == nullptr) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(view.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(view.end);
    const auto capacity = reinterpret_cast<std::uintptr_t>(view.capacity);
    if (end < begin || capacity < end) return false;
    const std::size_t bytes = end - begin;
    const std::size_t capacityBytes = capacity - begin;
    if (bytes % sizeof(hands::Matrix) != 0 ||
        capacityBytes % sizeof(hands::Matrix) != 0) {
        return false;
    }
    view.count = bytes / sizeof(hands::Matrix);
    const std::size_t capacityCount = capacityBytes / sizeof(hands::Matrix);
    return view.count != 0 && view.count <= pose::kMaximumBones &&
           capacityCount <= pose::kMaximumBones;
}

} // namespace

Runtime::Entry* Runtime::FindOrInsert(void* const mesh) noexcept {
    Entry* freeEntry = nullptr;
    Entry* stalest = nullptr;
    for (Entry& entry : entries_) {
        if (entry.mesh == mesh) return &entry;
        if (entry.mesh == nullptr) {
            if (freeEntry == nullptr) freeEntry = &entry;
        } else if (!entry.overridden &&
                   renderCount_ - entry.lastObservedRender >=
                       kEvictionRenders &&
                   (stalest == nullptr ||
                    entry.lastObservedRender < stalest->lastObservedRender)) {
            stalest = &entry;
        }
    }
    if (freeEntry == nullptr && stalest != nullptr) {
        // The engine never reports a mesh's destruction, so a ragdoll from an
        // earlier map would otherwise hold its slot for the whole session.
        // Recycle the mesh that has gone longest without a graphics update;
        // if it is still alive it only has to rebuild its two-tick history.
        // The age threshold keeps more simultaneously live ragdolls than
        // slots from evicting one another every render.
        ResetIdentity(*stalest);
        freeEntry = stalest;
    }
    if (freeEntry != nullptr) freeEntry->mesh = mesh;
    return freeEntry;
}

void Runtime::ResetIdentity(Entry& entry) noexcept {
    entry.history.Reset();
    entry.lastCapturedSerial = 0;
    entry.identityBuffer = nullptr;
    entry.identityBoneCount = 0;
    entry.overrideBuffer = nullptr;
    entry.overrideBoneCount = 0;
    entry.hasCapture = false;
    entry.pairChanged = false;
    entry.overridden = false;
}

void Runtime::BeginRender(const clock::Sample& clockSample,
                          const std::uint64_t tickSerial) noexcept {
    if (renderActive_) (void)EndRender();
    clockSample_ = clockSample;
    renderSerial_ = tickSerial;
    ++renderCount_;
    for (Entry& entry : entries_) entry.observedThisRender = false;
    renderActive_ = true;
}

UpdateObservation Runtime::OnMeshUpdated(void* const mesh) noexcept {
    if (!renderActive_) return {};
    std::uint8_t skeletonPhysics = 0;
    if (!ReadAt(mesh, kSkeletonPhysicsOffset, skeletonPhysics) ||
        skeletonPhysics > 1U) {
        return {UpdateResult::unavailable};
    }
    if (skeletonPhysics == 0U) return {};

    Entry* const entry = FindOrInsert(mesh);
    if (entry == nullptr) return {UpdateResult::capacityExhausted};
    if (entry->observedThisRender) {
        return {UpdateResult::alreadyApplied, entry->identityBoneCount};
    }
    entry->observedThisRender = true;
    entry->lastObservedRender = renderCount_;

    VectorView vector{};
    if (!ReadVector(mesh, vector)) {
        ResetIdentity(*entry);
        entry->mesh = mesh;
        return {UpdateResult::unavailable};
    }
    const bool identityChanged = entry->identityBuffer != vector.begin ||
                                 entry->identityBoneCount != vector.count;
    if (identityChanged) {
        ResetIdentity(*entry);
        entry->mesh = mesh;
        entry->identityBuffer = vector.begin;
        entry->identityBoneCount = vector.count;
    }

    UpdateResult captureResult = identityChanged ? UpdateResult::identityChanged
                                                  : UpdateResult::captured;
    bool captured = false;
    if (!entry->hasCapture || entry->lastCapturedSerial != renderSerial_) {
        pose::Pose authoritative{};
        authoritative.boneCount = vector.count;
        std::memcpy(authoritative.bones.data(), vector.begin,
                    vector.count * sizeof(hands::Matrix));
        const bool poseChanged =
            !entry->history.LatestEquals(authoritative);
        const pose::PublishKind published = entry->history.Publish(
            renderSerial_, clockSample_.generation, authoritative);
        entry->lastCapturedSerial = renderSerial_;
        entry->hasCapture = true;
        captured = true;
        switch (published) {
        case pose::PublishKind::invalid:
            return {UpdateResult::invalid, vector.count, true};
        case pose::PublishKind::resetGeneration:
        case pose::PublishKind::resetNonMonotonic:
        case pose::PublishKind::resetSameTickMutation:
        case pose::PublishKind::resetBoneCount:
        case pose::PublishKind::resetDiscontinuity:
            captureResult = UpdateResult::identityChanged;
            entry->pairChanged = false;
            break;
        case pose::PublishKind::appended:
            entry->pairChanged = poseChanged;
            break;
        case pose::PublishKind::unchanged:
            break;
        }
    }

    // A sleeping ragdoll (or a skeleton-physics prop whose motion comes from
    // its entity/node transform) has identical authoritative palettes at both
    // ends of the presentation pair. Re-skinning it cannot change a vertex,
    // but it still orphans and uploads three OpenGL buffers per submesh.
    if (!entry->pairChanged && captureResult != UpdateResult::identityChanged) {
        return {UpdateResult::stationary, vector.count, captured};
    }

    const pose::SampleResult sampled = entry->history.Evaluate(clockSample_);
    switch (sampled.kind) {
    case pose::SampleKind::clockUnavailable:
    case pose::SampleKind::generationMismatch:
        return {UpdateResult::clockUnavailable, vector.count, captured};
    case pose::SampleKind::snapshotsMissing:
        return {captureResult == UpdateResult::identityChanged
                    ? captureResult
                    : UpdateResult::pairMissing,
                vector.count, captured};
    case pose::SampleKind::invalid:
        return {UpdateResult::invalid, vector.count, captured};
    case pose::SampleKind::applied:
        break;
    }
    if (sampled.pose.boneCount != vector.count) {
        ResetIdentity(*entry);
        entry->mesh = mesh;
        return {UpdateResult::invalid, vector.count, captured};
    }

    const std::size_t bytes = vector.count * sizeof(hands::Matrix);
    std::memcpy(entry->restore.data(), vector.begin, bytes);
    std::memcpy(vector.begin, sampled.pose.bones.data(), bytes);
    entry->overrideBuffer = vector.begin;
    entry->overrideBoneCount = vector.count;
    entry->overridden = true;
    return {UpdateResult::applied, vector.count, captured};
}

bool Runtime::WasObserved(const void* const mesh) const noexcept {
    if (!renderActive_ || mesh == nullptr) return false;
    for (const Entry& entry : entries_) {
        if (entry.mesh == mesh) return entry.observedThisRender;
    }
    return false;
}

bool Runtime::ShouldReskin(const void* const mesh) const noexcept {
    if (!renderActive_ || mesh == nullptr) return false;
    for (const Entry& entry : entries_) {
        if (entry.mesh == mesh) return entry.pairChanged;
    }
    return false;
}

std::size_t Runtime::EndRender() noexcept {
    renderActive_ = false;
    std::size_t failures = 0;
    for (Entry& entry : entries_) {
        if (!entry.overridden) continue;
        const std::size_t bytes =
            entry.overrideBoneCount * sizeof(hands::Matrix);
        if (entry.overrideBuffer == nullptr) {
            ++failures;
        } else {
            std::memcpy(entry.overrideBuffer, entry.restore.data(), bytes);
        }
        entry.overrideBuffer = nullptr;
        entry.overrideBoneCount = 0;
        entry.overridden = false;
    }
    return failures;
}

void Runtime::Reset() noexcept {
    (void)EndRender();
    entries_ = {};
    clockSample_ = {};
    renderSerial_ = 0;
    renderCount_ = 0;
}

const wchar_t* UpdateResultName(const UpdateResult result) noexcept {
    switch (result) {
    case UpdateResult::notTarget: return L"not-target";
    case UpdateResult::captured: return L"captured";
    case UpdateResult::applied: return L"applied";
    case UpdateResult::alreadyApplied: return L"already-applied";
    case UpdateResult::unavailable: return L"unavailable";
    case UpdateResult::identityChanged: return L"identity-changed";
    case UpdateResult::clockUnavailable: return L"clock-unavailable";
    case UpdateResult::pairMissing: return L"pair-missing";
    case UpdateResult::stationary: return L"stationary";
    case UpdateResult::invalid: return L"invalid";
    case UpdateResult::capacityExhausted: return L"capacity-exhausted";
    }
    return L"unknown";
}

} // namespace amfp::ragdoll_pose
