#include "world_runtime.h"

#include <cstdint>
#include <cstring>

namespace amfp::world {
namespace {

constexpr std::size_t kBodiesBeginOffset = 0x348;
constexpr std::size_t kBodiesEndOffset = 0x34C;
constexpr std::size_t kMeshEntityOffset = 0x428;
constexpr std::size_t kPropTypeOffset = 0x56C;
constexpr std::size_t kBodyLocalMatrixOffset = 0x2C;
constexpr std::size_t kEntityParentOffset = 0x288;
constexpr std::size_t kMeshSubMeshesBeginOffset = 0x314;
constexpr std::size_t kMeshSubMeshesEndOffset = 0x318;
constexpr std::uint32_t kObjectPropType = 0;
constexpr std::uint32_t kSwingDoorPropType = 1;
constexpr std::uint32_t kLeverPropType = 2;
constexpr std::uint32_t kWheelPropType = 3;
constexpr std::uint32_t kMoveObjectPropType = 6;
constexpr std::uint32_t kItemPropType = 7;
constexpr std::size_t kMaximumBodiesPerProp = 128;
constexpr std::size_t kMaximumSubMeshesPerProp = 256;

template <typename T>
T ReadAt(const void* base, const std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset,
                sizeof(value));
    return value;
}

std::size_t HashPointer(const void* pointer) noexcept {
    std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer) >> 4U;
    value ^= value >> 11U;
    return static_cast<std::size_t>(value);
}

// Marks a slot whose body was destroyed. Lookups probe past it, insertion
// may reuse it, and it is never a valid cPhysicsBodyNewton address.
void* Tombstone() noexcept {
    return reinterpret_cast<void*>(std::uintptr_t{1});
}

bool ContainsBody(void* const candidate,
                  void** const bodies,
                  const std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (bodies[index] == candidate) return true;
    }
    return false;
}

bool HasCompleteDirectRenderMapping(void* const prop,
                                    void** const bodies,
                                    const std::size_t bodyCount) noexcept {
    void* const mesh = ReadAt<void*>(prop, kMeshEntityOffset);
    if (mesh == nullptr) return false;
    void** const begin = ReadAt<void**>(mesh, kMeshSubMeshesBeginOffset);
    void** const end = ReadAt<void**>(mesh, kMeshSubMeshesEndOffset);
    const std::uintptr_t beginAddress = reinterpret_cast<std::uintptr_t>(begin);
    const std::uintptr_t endAddress = reinterpret_cast<std::uintptr_t>(end);
    if (begin == nullptr || end == nullptr || endAddress <= beginAddress ||
        ((endAddress - beginAddress) % sizeof(void*)) != 0) {
        return false;
    }
    const std::size_t count =
        (endAddress - beginAddress) / sizeof(void*);
    if (count > kMaximumSubMeshesPerProp) return false;
    for (std::size_t index = 0; index < count; ++index) {
        void* const subMesh = begin[index];
        if (subMesh == nullptr ||
            !ContainsBody(ReadAt<void*>(subMesh, kEntityParentOffset),
                          bodies, bodyCount)) {
            return false;
        }
    }
    return true;
}

} // namespace

Runtime::Entry* Runtime::Find(void* const body) noexcept {
    return const_cast<Entry*>(static_cast<const Runtime*>(this)->Find(body));
}

const Runtime::Entry* Runtime::Find(void* const body) const noexcept {
    if (body == nullptr) return nullptr;
    const std::size_t start = HashPointer(body) % entries_.size();
    for (std::size_t probe = 0; probe < kMaximumProbe; ++probe) {
        const Entry& entry = entries_[(start + probe) % entries_.size()];
        void* const existing = entry.body.load(std::memory_order_acquire);
        if (existing == body) return &entry;
        if (existing == nullptr) return nullptr;
    }
    return nullptr;
}

Runtime::Entry* Runtime::FindOrInsert(void* const body) noexcept {
    if (body == nullptr) return nullptr;
    const std::size_t start = HashPointer(body) % entries_.size();
    Entry* reusable = nullptr;
    for (std::size_t probe = 0; probe < kMaximumProbe; ++probe) {
        Entry& entry = entries_[(start + probe) % entries_.size()];
        void* existing = entry.body.load(std::memory_order_acquire);
        if (existing == body) return &entry;
        if (existing == Tombstone()) {
            if (reusable == nullptr) reusable = &entry;
            continue;
        }
        if (existing == nullptr) {
            if (reusable == nullptr) reusable = &entry;
            break;
        }
    }
    if (reusable == nullptr) return nullptr;
    void* expected = reusable->body.load(std::memory_order_acquire);
    if ((expected != nullptr && expected != Tombstone()) ||
        !reusable->body.compare_exchange_strong(expected, body,
                                                std::memory_order_release,
                                                std::memory_order_acquire)) {
        return nullptr;
    }
    if (expected == Tombstone()) {
        entryTombstones_.fetch_sub(1, std::memory_order_relaxed);
    }
    entryLive_.fetch_add(1, std::memory_order_relaxed);
    return reusable;
}

Runtime::Entry* Runtime::PromoteStandalone(void* const body) noexcept {
    if (body == nullptr) return nullptr;
    while (registrationLock_.test_and_set(std::memory_order_acquire)) {}
    struct Unlock final {
        std::atomic_flag& lock;
        ~Unlock() { lock.clear(std::memory_order_release); }
    } unlock{registrationLock_};

    if (Entry* const existing = Find(body); existing != nullptr) {
        return existing;
    }
    Entry* const entry = FindOrInsert(body);
    if (entry == nullptr) return nullptr;
    entry->history.Reset();
    entry->latest = {};
    entry->renderDelta = hands::Identity();
    entry->stagedSerial = 0;
    entry->renderFrame = 0;
    entry->renderSample = hands::SampleKind::clockUnavailable;
    entry->propType = UINT32_MAX;
    entry->hasLatest = false;
    entry->standalone = true;
    return entry;
}

Runtime::RelationshipCandidate* Runtime::FindRelationshipCandidate(
    void* const body) noexcept {
    if (body == nullptr) return nullptr;
    const std::size_t start = HashPointer(body) % relationshipCandidates_.size();
    for (std::size_t probe = 0; probe < kMaximumProbe; ++probe) {
        RelationshipCandidate& candidate =
            relationshipCandidates_[(start + probe) %
                                    relationshipCandidates_.size()];
        void* const existing = candidate.body.load(std::memory_order_acquire);
        if (existing == body) return &candidate;
        if (existing == nullptr) return nullptr;
    }
    return nullptr;
}

Runtime::RelationshipCandidate* Runtime::FindOrInsertRelationshipCandidate(
    void* const body) noexcept {
    if (body == nullptr) return nullptr;
    const std::size_t start = HashPointer(body) % relationshipCandidates_.size();
    RelationshipCandidate* reusable = nullptr;
    for (std::size_t probe = 0; probe < kMaximumProbe; ++probe) {
        RelationshipCandidate& candidate =
            relationshipCandidates_[(start + probe) %
                                    relationshipCandidates_.size()];
        void* existing = candidate.body.load(std::memory_order_acquire);
        if (existing == body) return &candidate;
        if (existing == Tombstone()) {
            if (reusable == nullptr) reusable = &candidate;
            continue;
        }
        if (existing == nullptr) {
            if (reusable == nullptr) reusable = &candidate;
            break;
        }
    }
    if (reusable == nullptr) return nullptr;
    void* expected = reusable->body.load(std::memory_order_acquire);
    if (expected != nullptr && expected != Tombstone()) return nullptr;
    reusable->lastMovedSerial.store(0, std::memory_order_release);
    if (!reusable->body.compare_exchange_strong(expected, body,
                                                std::memory_order_release,
                                                std::memory_order_acquire)) {
        return nullptr;
    }
    if (expected == Tombstone()) {
        candidateTombstones_.fetch_sub(1, std::memory_order_relaxed);
    }
    candidateLive_.fetch_add(1, std::memory_order_relaxed);
    return reusable;
}

bool Runtime::IsRecent(
    const RelationshipCandidate& candidate) const noexcept {
    const std::uint64_t serial =
        candidate.lastMovedSerial.load(std::memory_order_acquire);
    return serial != 0 &&
           (serial == renderSample_.olderSerial ||
            serial == renderSample_.newerSerial);
}

void Runtime::Reset() noexcept {
    while (registrationLock_.test_and_set(std::memory_order_acquire)) {}
    struct Unlock final {
        std::atomic_flag& lock;
        ~Unlock() { lock.clear(std::memory_order_release); }
    } unlock{registrationLock_};

    ClearTables();
}

void Runtime::ClearTables() noexcept {
    for (Entry& entry : entries_) {
        if (entry.body.load(std::memory_order_acquire) == nullptr) continue;
        entry.history.Reset();
        entry.latest = {};
        entry.renderDelta = hands::Identity();
        entry.stagedSerial = 0;
        entry.renderFrame = 0;
        entry.renderSample = hands::SampleKind::clockUnavailable;
        entry.propType = UINT32_MAX;
        entry.hasLatest = false;
        entry.standalone = false;
        entry.body.store(nullptr, std::memory_order_release);
    }
    for (RelationshipCandidate& candidate : relationshipCandidates_) {
        if (candidate.body.load(std::memory_order_acquire) == nullptr) continue;
        candidate.lastMovedSerial.store(0, std::memory_order_release);
        candidate.ownedByProp.store(0, std::memory_order_release);
        candidate.propType.store(UINT32_MAX, std::memory_order_release);
        candidate.body.store(nullptr, std::memory_order_release);
    }
    for (std::atomic<void*>& moved : moved_) {
        moved.store(nullptr, std::memory_order_release);
    }
    for (std::atomic<void*>& moved : relationshipMoved_) {
        moved.store(nullptr, std::memory_order_release);
    }
    movedCount_.store(0, std::memory_order_release);
    movedOverflow_.store(0, std::memory_order_release);
    relationshipMovedCount_.store(0, std::memory_order_release);
    relationshipOverflow_.store(0, std::memory_order_release);
    entryLive_.store(0, std::memory_order_release);
    entryTombstones_.store(0, std::memory_order_release);
    candidateLive_.store(0, std::memory_order_release);
    candidateTombstones_.store(0, std::memory_order_release);
    outputCount_ = 0;
    renderActive_ = false;
}

bool Runtime::NotifyBodyDestroyed(void* const physicsBody) noexcept {
    if (physicsBody == nullptr) return false;
    while (registrationLock_.test_and_set(std::memory_order_acquire)) {}
    struct Unlock final {
        std::atomic_flag& lock;
        ~Unlock() { lock.clear(std::memory_order_release); }
    } unlock{registrationLock_};

    bool tracked = false;
    if (Entry* const entry = Find(physicsBody); entry != nullptr) {
        // The key goes first so no lookup can reach the stale history.
        entry->body.store(Tombstone(), std::memory_order_release);
        entry->history.Reset();
        entry->hasLatest = false;
        entry->renderFrame = 0;
        entry->renderSample = hands::SampleKind::clockUnavailable;
        entryLive_.fetch_sub(1, std::memory_order_relaxed);
        entryTombstones_.fetch_add(1, std::memory_order_relaxed);
        tracked = true;
    }
    if (RelationshipCandidate* const candidate =
            FindRelationshipCandidate(physicsBody);
        candidate != nullptr) {
        candidate->body.store(Tombstone(), std::memory_order_release);
        candidate->lastMovedSerial.store(0, std::memory_order_release);
        candidateLive_.fetch_sub(1, std::memory_order_relaxed);
        candidateTombstones_.fetch_add(1, std::memory_order_relaxed);
        tracked = true;
    }
    // A map unload destroys every body; returning the tables to a pristine
    // state then keeps tombstones from lengthening probes for the session.
    if (tracked && entryLive_.load(std::memory_order_relaxed) == 0 &&
        candidateLive_.load(std::memory_order_relaxed) == 0) {
        ClearTables();
        return true;
    }
    return false;
}

void Runtime::SetRelationshipDiagnostics(const bool enabled) noexcept {
    relationshipDiagnostics_ = enabled;
}

bool MapEpoch::OnRegistration(void* const world,
                              const bool listenerKnown,
                              void* const listenerWorld) noexcept {
    // Entities restored from a save are created after their map has become
    // current, so a changed world pointer must count on its own as well.
    const bool loading =
        world != world_ || (listenerKnown && listenerWorld != world);
    const bool begins = loading && !(loading_ && world == world_);
    world_ = world;
    loading_ = loading;
    return begins;
}

void MapEpoch::OnPresented(void* const listenerWorld) noexcept {
    if (listenerWorld == world_) loading_ = false;
}

RegistrationResult Runtime::RegisterProp(void* const prop) noexcept {
    if (prop == nullptr) {
        return {};
    }
    const std::uint32_t propType =
        ReadAt<std::uint32_t>(prop, kPropTypeOffset);
    if (propType != kObjectPropType && propType != kSwingDoorPropType &&
        propType != kLeverPropType && propType != kWheelPropType &&
        propType != kMoveObjectPropType && propType != kItemPropType) {
        return {};
    }
    while (registrationLock_.test_and_set(std::memory_order_acquire)) {}
    struct Unlock final {
        std::atomic_flag& lock;
        ~Unlock() { lock.clear(std::memory_order_release); }
    } unlock{registrationLock_};

    void** const begin = ReadAt<void**>(prop, kBodiesBeginOffset);
    void** const end = ReadAt<void**>(prop, kBodiesEndOffset);
    const std::uintptr_t beginAddress = reinterpret_cast<std::uintptr_t>(begin);
    const std::uintptr_t endAddress = reinterpret_cast<std::uintptr_t>(end);
    if ((begin == nullptr) != (end == nullptr) || endAddress < beginAddress ||
        ((endAddress - beginAddress) % sizeof(void*)) != 0) {
        return {RegistrationKind::invalidLayout, 0};
    }
    const std::size_t count =
        (endAddress - beginAddress) / sizeof(void*);
    if (count > kMaximumBodiesPerProp) {
        return {RegistrationKind::invalidLayout, 0};
    }
    if (count == 0) {
        return {RegistrationKind::bodyless, 0};
    }
    if ((propType == kObjectPropType || propType == kItemPropType) &&
        !HasCompleteDirectRenderMapping(prop, begin, count)) {
        return {RegistrationKind::incompleteRenderMapping, 0};
    }
    std::uint32_t registeredBodies = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (begin[index] == nullptr) continue;
        Entry* const entry = FindOrInsert(begin[index]);
        if (entry == nullptr) {
            return {RegistrationKind::capacityExhausted, registeredBodies};
        }
        entry->history.Reset();
        entry->latest = {};
        entry->renderDelta = hands::Identity();
        entry->stagedSerial = 0;
        entry->renderFrame = 0;
        entry->renderSample = hands::SampleKind::clockUnavailable;
        entry->propType = propType;
        entry->hasLatest = false;
        entry->standalone = false;
        ++registeredBodies;
    }
    if (registeredBodies == 0) {
        return {RegistrationKind::invalidLayout, 0};
    }
    return {RegistrationKind::registered, registeredBodies};
}

void Runtime::NotifyBodyMoved(void* const physicsBody,
                              const bool ownedByProp,
                              const std::uint32_t propType) noexcept {
    if (Find(physicsBody) != nullptr) {
        const std::uint32_t index =
            movedCount_.fetch_add(1, std::memory_order_relaxed);
        if (index >= moved_.size()) {
            movedOverflow_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        moved_[index].store(physicsBody, std::memory_order_release);
        return;
    }
    RelationshipCandidate* const candidate =
        FindOrInsertRelationshipCandidate(physicsBody);
    if (candidate == nullptr) {
        relationshipOverflow_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    candidate->ownedByProp.store(ownedByProp ? 1U : 0U,
                                 std::memory_order_release);
    candidate->propType.store(ownedByProp ? propType : UINT32_MAX,
                              std::memory_order_release);
    const std::uint32_t index =
        relationshipMovedCount_.fetch_add(1, std::memory_order_relaxed);
    if (index >= relationshipMoved_.size()) {
        relationshipOverflow_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    relationshipMoved_[index].store(physicsBody, std::memory_order_release);
}

CommitResult Runtime::CommitCompletedTick(
    const std::uint64_t tickSerial,
    const std::uint64_t clockGeneration) noexcept {
    CommitResult result{};
    const std::uint32_t notified =
        movedCount_.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t bounded = notified < moved_.size()
        ? notified
        : static_cast<std::uint32_t>(moved_.size());
    result.callbacks = bounded;
    result.overflow = movedOverflow_.exchange(0, std::memory_order_relaxed);
    for (std::uint32_t index = 0; index < bounded; ++index) {
        void* const body = moved_[index].exchange(nullptr,
                                                   std::memory_order_acq_rel);
        Entry* const entry = Find(body);
        if (entry == nullptr) continue;
        if (entry->stagedSerial == tickSerial) {
            ++result.duplicate;
            continue;
        }
        entry->stagedSerial = tickSerial;
        const Matrix matrix = ReadAt<Matrix>(body, kBodyLocalMatrixOffset);
        if (!hands::IsRigid(matrix)) {
            entry->history.Reset();
            entry->hasLatest = false;
            ++result.invalid;
            continue;
        }
        const hands::PublishKind published =
            entry->history.Publish(tickSerial, clockGeneration, matrix);
        entry->latest = matrix;
        entry->hasLatest = true;
        if (published == hands::PublishKind::invalid) {
            ++result.invalid;
        } else {
            ++result.captured;
        }
    }
    const std::uint32_t relationshipNotified =
        relationshipMovedCount_.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t relationshipBounded =
        relationshipNotified < relationshipMoved_.size()
            ? relationshipNotified
            : static_cast<std::uint32_t>(relationshipMoved_.size());
    result.relationshipCandidates = relationshipBounded;
    result.relationshipOverflow =
        relationshipOverflow_.exchange(0, std::memory_order_relaxed);
    for (std::uint32_t index = 0; index < relationshipBounded; ++index) {
        void* const body = relationshipMoved_[index].exchange(
            nullptr, std::memory_order_acq_rel);
        RelationshipCandidate* const candidate =
            FindRelationshipCandidate(body);
        if (candidate != nullptr) {
            candidate->lastMovedSerial.store(tickSerial,
                                             std::memory_order_release);
        }
    }
    return result;
}

void Runtime::BeginRender(const clock::Sample& sample) noexcept {
    renderSample_ = sample;
    outputCount_ = 0;
    ++renderFrame_;
    if (renderFrame_ == 0) ++renderFrame_;
    renderActive_ = sample.kind == clock::SampleKind::interpolated;
}

OverrideResult Runtime::OverrideModelMatrix(
    void* const subMesh,
    const Matrix* const original,
    const Matrix*& result,
    std::uint32_t* const propType,
    std::uint32_t* const relationshipDepth) noexcept {
    result = original;
    if (propType != nullptr) *propType = UINT32_MAX;
    if (relationshipDepth != nullptr) *relationshipDepth = 0;
    if (!renderActive_ || subMesh == nullptr) {
        return OverrideResult::clockUnavailable;
    }
    void* parent = ReadAt<void*>(subMesh, kEntityParentOffset);
    Entry* entry = Find(parent);
    if (entry == nullptr) {
        RelationshipCandidate* candidate =
            FindRelationshipCandidate(parent);
        if (candidate != nullptr && IsRecent(*candidate)) {
            if (candidate->ownedByProp.load(std::memory_order_acquire) != 0) {
                if (propType != nullptr) {
                    *propType = candidate->propType.load(
                        std::memory_order_acquire);
                }
                return OverrideResult::diagnosticOwnedDirect;
            }
            return PromoteStandalone(parent) != nullptr
                ? OverrideResult::standalonePromoted
                : OverrideResult::slotsExhausted;
        }
        // Everything below only classifies nested relationships for the
        // telemetry counters; it can never produce a correction.
        if (!relationshipDiagnostics_) return OverrideResult::notTarget;
        void* child = parent;
        for (std::size_t depth = 1;
             child != nullptr && depth < kMaximumParentDepth; ++depth) {
            parent = ReadAt<void*>(child, kEntityParentOffset);
            if (parent == nullptr || parent == child) break;
            entry = Find(parent);
            if (entry != nullptr) {
                if (propType != nullptr) *propType = entry->propType;
                if (relationshipDepth != nullptr) {
                    *relationshipDepth = static_cast<std::uint32_t>(depth);
                }
                return OverrideResult::diagnosticNestedTarget;
            }
            candidate = FindRelationshipCandidate(parent);
            if (candidate != nullptr && IsRecent(*candidate)) {
                if (propType != nullptr) {
                    *propType = candidate->propType.load(
                        std::memory_order_acquire);
                }
                if (relationshipDepth != nullptr) {
                    *relationshipDepth = static_cast<std::uint32_t>(depth);
                }
                return candidate->ownedByProp.load(
                           std::memory_order_acquire) != 0
                    ? OverrideResult::diagnosticOwnedNested
                    : OverrideResult::diagnosticStandaloneNested;
            }
            child = parent;
        }
        return OverrideResult::notTarget;
    }

    if (propType != nullptr) *propType = entry->propType;

    if (entry->renderFrame != renderFrame_) {
        entry->renderFrame = renderFrame_;
        const hands::SampleResult sampled = entry->history.Evaluate(renderSample_);
        entry->renderSample = sampled.kind;
        if (sampled.kind == hands::SampleKind::applied && entry->hasLatest) {
            Matrix inverse{};
            if (!hands::InverseRigid(entry->latest, inverse) ||
                !hands::Multiply(sampled.matrix, inverse,
                                 entry->renderDelta)) {
                entry->renderSample = hands::SampleKind::invalid;
            }
        }
    }
    if (entry->renderSample != hands::SampleKind::applied) {
        return entry->renderSample == hands::SampleKind::snapshotsMissing
            ? OverrideResult::pairMissing
            : entry->renderSample == hands::SampleKind::generationMismatch
                ? OverrideResult::historyUnavailable
            : entry->renderSample == hands::SampleKind::clockUnavailable
                ? OverrideResult::clockUnavailable
                : OverrideResult::invalid;
    }
    if (outputCount_ >= outputs_.size()) {
        return OverrideResult::slotsExhausted;
    }
    const Matrix identity = hands::Identity();
    const Matrix& source = original == nullptr ? identity : *original;
    Matrix& output = outputs_[outputCount_++];
    if (!hands::Multiply(entry->renderDelta, source, output)) {
        return OverrideResult::invalid;
    }
    result = &output;
    return entry->standalone
        ? OverrideResult::appliedStandalone
        : OverrideResult::applied;
}

void Runtime::EndRender() noexcept {
    renderActive_ = false;
    outputCount_ = 0;
}

const wchar_t* OverrideResultName(const OverrideResult result) noexcept {
    switch (result) {
    case OverrideResult::notTarget: return L"not-target";
    case OverrideResult::applied: return L"applied";
    case OverrideResult::appliedStandalone: return L"applied-standalone";
    case OverrideResult::standalonePromoted: return L"standalone-promoted";
    case OverrideResult::clockUnavailable: return L"clock-unavailable";
    case OverrideResult::historyUnavailable: return L"history-unavailable";
    case OverrideResult::pairMissing: return L"pair-missing";
    case OverrideResult::invalid: return L"invalid";
    case OverrideResult::slotsExhausted: return L"slots-exhausted";
    case OverrideResult::diagnosticNestedTarget: return L"diagnostic-nested-target";
    case OverrideResult::diagnosticOwnedDirect: return L"diagnostic-owned-direct";
    case OverrideResult::diagnosticOwnedNested: return L"diagnostic-owned-nested";
    case OverrideResult::diagnosticStandaloneDirect: return L"diagnostic-standalone-direct";
    case OverrideResult::diagnosticStandaloneNested: return L"diagnostic-standalone-nested";
    }
    return L"unknown";
}

} // namespace amfp::world
