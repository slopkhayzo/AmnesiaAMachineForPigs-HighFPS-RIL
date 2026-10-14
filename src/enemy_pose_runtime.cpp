#include "enemy_pose_runtime.h"

#include <cstdint>
#include <cstring>

namespace amfp::enemy_pose {
namespace {

constexpr std::uint32_t kManPigType = 2;
constexpr std::uint32_t kChildType = 4;
constexpr std::size_t kEnemyMeshOffset = 0x578;
constexpr std::size_t kEnemyTypeOffset = 0x8E4;
constexpr std::size_t kEntityActiveOffset = 0x24;
constexpr std::size_t kBoneMatricesBeginOffset = 0x388;
constexpr std::size_t kBoneMatricesEndOffset = 0x38C;
constexpr std::size_t kBoneMatricesCapacityOffset = 0x390;

// The enemy is the receiver of the signature-gated iLuxEnemy::OnUpdate and
// the mesh is the receiver of cMeshEntity::UpdateGraphicsForFrame, which has
// just written the palette vector read here. These reads run for every enemy
// tick and mesh update, so they must not issue OS memory queries; null,
// boolean-range, vector-shape, and bone-count checks remain.
template <typename T>
bool ReadAt(const void* base, const std::size_t offset, T& value) noexcept {
    if (base == nullptr) return false;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset,
                sizeof(T));
    return true;
}

struct VectorView {
    hands::Matrix* begin = nullptr;
    hands::Matrix* end = nullptr;
    hands::Matrix* capacity = nullptr;
    std::size_t count = 0;
};

bool ReadVector(void* mesh, VectorView& view) noexcept {
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

std::size_t HashPointer(const void* pointer) noexcept {
    std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer) >> 4U;
    value ^= value >> 11U;
    return static_cast<std::size_t>(value);
}

// eLuxEnemyType order: Grunt, WaterLurker, ManPig, ThomasPig, Child. Only the
// two archetypes whose palettes passed the 0.22 read-only audit are admitted.
bool IsSupportedType(const std::uint32_t type) noexcept {
    return type == kManPigType || type == kChildType;
}

} // namespace

Runtime::Entry* Runtime::FindEnemy(void* const enemy) noexcept {
    if (enemy == nullptr) return nullptr;
    for (Entry& entry : entries_) {
        if (entry.enemy == enemy) return &entry;
    }
    return nullptr;
}

Runtime::Entry* Runtime::FindMesh(void* const mesh) noexcept {
    if (mesh == nullptr) return nullptr;
    std::size_t bucket = HashPointer(mesh) & (meshBuckets_.size() - 1U);
    for (std::size_t probe = 0; probe < meshBuckets_.size(); ++probe) {
        const std::size_t stored = meshBuckets_[bucket];
        if (stored == 0) return nullptr;
        Entry& entry = entries_[stored - 1U];
        if (entry.mesh == mesh) return &entry;
        bucket = (bucket + 1U) & (meshBuckets_.size() - 1U);
    }
    return nullptr;
}

void Runtime::RebuildMeshIndex() noexcept {
    meshBuckets_.fill(0);
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].enemy == nullptr || entries_[index].mesh == nullptr) {
            continue;
        }
        std::size_t bucket =
            HashPointer(entries_[index].mesh) & (meshBuckets_.size() - 1U);
        for (std::size_t probe = 0; probe < meshBuckets_.size(); ++probe) {
            if (meshBuckets_[bucket] == 0) {
                meshBuckets_[bucket] = index + 1U;
                break;
            }
            bucket = (bucket + 1U) & (meshBuckets_.size() - 1U);
        }
    }
}

void Runtime::ResetIdentity(Entry& entry) noexcept {
    entry.history.Reset();
    entry.identityBuffer = nullptr;
    entry.identityBoneCount = 0;
    entry.overrideBuffer = nullptr;
    entry.overrideBoneCount = 0;
    entry.lastCapturedSerial = 0;
    entry.hasCapture = false;
    entry.overridden = false;
}

StageObservation Runtime::StageEnemy(void* const enemy,
                                     const std::uint64_t pendingSerial) noexcept {
    void* mesh = nullptr;
    std::uint32_t type = 0;
    if (!ReadAt(enemy, kEnemyMeshOffset, mesh) || mesh == nullptr ||
        !ReadAt(enemy, kEnemyTypeOffset, type)) {
        return {StageResult::unavailable, type};
    }
    if (!IsSupportedType(type)) return {StageResult::unsupportedType, type};
    std::uint8_t active = 0;
    if (!ReadAt(mesh, kEntityActiveOffset, active) || active > 1U) {
        return {StageResult::unavailable, type};
    }
    if (active == 0U) return {StageResult::inactive, type};

    Entry* entry = FindEnemy(enemy);
    if (entry == nullptr) {
        for (Entry& candidate : entries_) {
            if (candidate.enemy == nullptr) {
                entry = &candidate;
                break;
            }
        }
    }
    if (entry == nullptr) {
        // Entries are never released by the engine, so an enemy from an
        // earlier map would otherwise hold its slot for the whole session.
        // Recycle the one staged longest ago, provided it has missed at
        // least one whole tick and is not overridden for the current render.
        for (Entry& candidate : entries_) {
            if (candidate.overridden ||
                candidate.pendingSerial + 1U >= pendingSerial) {
                continue;
            }
            if (entry == nullptr ||
                candidate.pendingSerial < entry->pendingSerial) {
                entry = &candidate;
            }
        }
    }
    if (entry == nullptr) return {StageResult::capacityExhausted, type};
    if (entry->enemy != enemy || entry->mesh != mesh || entry->type != type) {
        *entry = {};
        entry->enemy = enemy;
        entry->mesh = mesh;
        entry->type = type;
        RebuildMeshIndex();
    }
    entry->pendingSerial = pendingSerial;
    entry->staged = true;
    return {StageResult::staged, type};
}

void Runtime::CommitCompletedTick(const std::uint64_t tickSerial,
                                  const std::uint64_t clockGeneration) noexcept {
    for (Entry& entry : entries_) {
        if (entry.enemy == nullptr) continue;
        entry.activeThisTick = entry.staged && entry.pendingSerial == tickSerial;
        entry.staged = false;
        if (entry.activeThisTick) {
            entry.currentSerial = tickSerial;
            entry.currentGeneration = clockGeneration;
        }
    }
}

void Runtime::BeginRender(const clock::Sample& clockSample,
                          const std::uint64_t tickSerial) noexcept {
    if (renderActive_) (void)EndRender();
    clockSample_ = clockSample;
    renderSerial_ = tickSerial;
    renderActive_ = true;
}

UpdateObservation Runtime::OnMeshUpdated(void* const mesh) noexcept {
    if (!renderActive_) return {};
    Entry* const entry = FindMesh(mesh);
    if (entry == nullptr || !entry->activeThisTick ||
        entry->currentSerial != renderSerial_) {
        return {};
    }
    if (entry->overridden) {
        return {UpdateResult::alreadyApplied, entry->type,
                entry->overrideBoneCount};
    }

    VectorView vector{};
    if (!ReadVector(mesh, vector)) {
        ResetIdentity(*entry);
        return {UpdateResult::unavailable, entry->type};
    }
    const bool identityChanged = entry->identityBuffer != vector.begin ||
                                 entry->identityBoneCount != vector.count;
    if (identityChanged) {
        ResetIdentity(*entry);
        entry->identityBuffer = vector.begin;
        entry->identityBoneCount = vector.count;
    }

    UpdateResult captureResult = identityChanged ? UpdateResult::identityChanged
                                                  : UpdateResult::captured;
    bool captured = false;
    if (!entry->hasCapture ||
        entry->lastCapturedSerial != entry->currentSerial) {
        pose::Pose authoritative{};
        authoritative.boneCount = vector.count;
        std::memcpy(authoritative.bones.data(), vector.begin,
                    vector.count * sizeof(hands::Matrix));
        const pose::PublishKind published = entry->history.Publish(
            entry->currentSerial, entry->currentGeneration, authoritative);
        entry->lastCapturedSerial = entry->currentSerial;
        entry->hasCapture = true;
        captured = true;
        switch (published) {
        case pose::PublishKind::invalid:
            return {UpdateResult::invalid, entry->type, vector.count, true};
        case pose::PublishKind::resetGeneration:
        case pose::PublishKind::resetNonMonotonic:
        case pose::PublishKind::resetSameTickMutation:
        case pose::PublishKind::resetBoneCount:
        case pose::PublishKind::resetDiscontinuity:
            captureResult = UpdateResult::identityChanged;
            break;
        case pose::PublishKind::appended:
        case pose::PublishKind::unchanged:
            break;
        }
    }

    const pose::SampleResult sampled = entry->history.Evaluate(clockSample_);
    switch (sampled.kind) {
    case pose::SampleKind::clockUnavailable:
    case pose::SampleKind::generationMismatch:
        return {UpdateResult::clockUnavailable, entry->type, vector.count,
                captured};
    case pose::SampleKind::snapshotsMissing:
        return {captureResult == UpdateResult::identityChanged
                    ? captureResult
                    : UpdateResult::pairMissing,
                entry->type, vector.count, captured};
    case pose::SampleKind::invalid:
        return {UpdateResult::invalid, entry->type, vector.count, captured};
    case pose::SampleKind::applied:
        break;
    }
    if (sampled.pose.boneCount != vector.count) {
        ResetIdentity(*entry);
        return {UpdateResult::invalid, entry->type, vector.count, captured};
    }

    const std::size_t bytes = vector.count * sizeof(hands::Matrix);
    std::memcpy(entry->restore.data(), vector.begin, bytes);
    std::memcpy(vector.begin, sampled.pose.bones.data(), bytes);
    entry->overrideBuffer = vector.begin;
    entry->overrideBoneCount = vector.count;
    entry->overridden = true;
    return {UpdateResult::applied, entry->type, vector.count, captured};
}

bool Runtime::TracksMesh(void* const mesh) noexcept {
    if (!renderActive_) return false;
    const Entry* const entry = FindMesh(mesh);
    return entry != nullptr && entry->activeThisTick &&
           entry->currentSerial == renderSerial_;
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
    meshBuckets_ = {};
    clockSample_ = {};
    renderSerial_ = 0;
}

const wchar_t* StageResultName(const StageResult result) noexcept {
    switch (result) {
    case StageResult::staged: return L"staged";
    case StageResult::unsupportedType: return L"unsupported-type";
    case StageResult::inactive: return L"inactive";
    case StageResult::unavailable: return L"unavailable";
    case StageResult::capacityExhausted: return L"capacity-exhausted";
    }
    return L"unknown";
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
    case UpdateResult::invalid: return L"invalid";
    }
    return L"unknown";
}

} // namespace amfp::enemy_pose
