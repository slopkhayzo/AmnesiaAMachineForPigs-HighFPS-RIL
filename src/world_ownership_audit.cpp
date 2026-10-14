#include "world_ownership_audit.h"

#include <cstdint>
#include <cstring>

namespace amfp::world_ownership {
namespace {

constexpr std::size_t kStaticPhysicsOffset = 0x1DD;
constexpr std::size_t kBodiesBeginOffset = 0x348;
constexpr std::size_t kBodiesEndOffset = 0x34C;
constexpr std::size_t kMeshEntityOffset = 0x428;
constexpr std::size_t kPropTypeOffset = 0x56C;
constexpr std::size_t kMeshSubMeshesBeginOffset = 0x314;
constexpr std::size_t kMeshSubMeshesEndOffset = 0x318;
constexpr std::size_t kEntityParentOffset = 0x288;
constexpr std::size_t kMaximumBodiesPerProp = 128;
constexpr std::size_t kMaximumSubMeshesPerProp = 256;
constexpr std::size_t kMaximumParentDepth = 16;

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

// Marks a slot whose body was destroyed; see world::Runtime.
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

} // namespace

void Registry::Reset() noexcept {
    while (registrationLock_.test_and_set(std::memory_order_acquire)) {}
    struct Unlock final {
        std::atomic_flag& lock;
        ~Unlock() { lock.clear(std::memory_order_release); }
    } unlock{registrationLock_};

    ClearTable();
}

void Registry::ClearTable() noexcept {
    for (Entry& entry : entries_) {
        entry.propType.store(0, std::memory_order_relaxed);
        entry.body.store(nullptr, std::memory_order_release);
    }
    live_ = 0;
    tombstones_ = 0;
}

void Registry::NotifyBodyDestroyed(void* const physicsBody) noexcept {
    if (physicsBody == nullptr) return;
    while (registrationLock_.test_and_set(std::memory_order_acquire)) {}
    struct Unlock final {
        std::atomic_flag& lock;
        ~Unlock() { lock.clear(std::memory_order_release); }
    } unlock{registrationLock_};

    const std::size_t start = HashPointer(physicsBody) % entries_.size();
    for (std::size_t probe = 0; probe < kMaximumProbe; ++probe) {
        Entry& entry = entries_[(start + probe) % entries_.size()];
        void* const existing = entry.body.load(std::memory_order_acquire);
        if (existing == nullptr) return;
        if (existing != physicsBody) continue;
        entry.body.store(Tombstone(), std::memory_order_release);
        --live_;
        ++tombstones_;
        if (live_ == 0) ClearTable();
        return;
    }
}

Registration Registry::RegisterProp(void* const prop) noexcept {
    Registration registration{};
    if (prop == nullptr) return registration;

    while (registrationLock_.test_and_set(std::memory_order_acquire)) {}
    struct Unlock final {
        std::atomic_flag& lock;
        ~Unlock() { lock.clear(std::memory_order_release); }
    } unlock{registrationLock_};

    registration.propType = ReadAt<std::uint32_t>(prop, kPropTypeOffset);
    const std::uint8_t staticPhysics =
        ReadAt<std::uint8_t>(prop, kStaticPhysicsOffset);
    void** const begin = ReadAt<void**>(prop, kBodiesBeginOffset);
    void** const end = ReadAt<void**>(prop, kBodiesEndOffset);
    const std::uintptr_t beginAddress =
        reinterpret_cast<std::uintptr_t>(begin);
    const std::uintptr_t endAddress = reinterpret_cast<std::uintptr_t>(end);
    if (registration.propType >= kPropTypeCount || staticPhysics > 1U ||
        (begin == nullptr) != (end == nullptr) || endAddress < beginAddress ||
        ((endAddress - beginAddress) % sizeof(void*)) != 0) {
        return registration;
    }
    registration.staticPhysics = staticPhysics != 0;
    const std::size_t count =
        (endAddress - beginAddress) / sizeof(void*);
    if (count > kMaximumBodiesPerProp) return registration;
    registration.bodyCount = static_cast<std::uint32_t>(count);
    if (count == 0) {
        registration.result = RegisterResult::bodyless;
        return registration;
    }

    for (std::size_t bodyIndex = 0; bodyIndex < count; ++bodyIndex) {
        void* const body = begin[bodyIndex];
        if (body == nullptr) continue;
        const std::size_t start = HashPointer(body) % entries_.size();
        bool inserted = false;
        Entry* reusable = nullptr;
        for (std::size_t probe = 0; probe < kMaximumProbe; ++probe) {
            Entry& entry = entries_[(start + probe) % entries_.size()];
            void* const existing = entry.body.load(std::memory_order_acquire);
            if (existing == body) {
                entry.propType.store(registration.propType,
                                     std::memory_order_release);
                inserted = true;
                break;
            }
            if (existing == Tombstone()) {
                if (reusable == nullptr) reusable = &entry;
                continue;
            }
            if (existing == nullptr) {
                if (reusable == nullptr) reusable = &entry;
                break;
            }
        }
        if (!inserted && reusable != nullptr) {
            // Registration and destruction both hold registrationLock_, so
            // the chosen slot cannot change underneath this store.
            const bool wasTombstone =
                reusable->body.load(std::memory_order_acquire) == Tombstone();
            reusable->propType.store(registration.propType,
                                     std::memory_order_relaxed);
            reusable->body.store(body, std::memory_order_release);
            if (wasTombstone) --tombstones_;
            ++live_;
            inserted = true;
        }
        if (!inserted) {
            registration.result = RegisterResult::capacityExhausted;
            return registration;
        }
        ++registration.registeredBodies;
    }
    void* const mesh = ReadAt<void*>(prop, kMeshEntityOffset);
    if (mesh != nullptr) {
        void** const subMeshesBegin =
            ReadAt<void**>(mesh, kMeshSubMeshesBeginOffset);
        void** const subMeshesEnd =
            ReadAt<void**>(mesh, kMeshSubMeshesEndOffset);
        const std::uintptr_t subMeshesBeginAddress =
            reinterpret_cast<std::uintptr_t>(subMeshesBegin);
        const std::uintptr_t subMeshesEndAddress =
            reinterpret_cast<std::uintptr_t>(subMeshesEnd);
        if ((subMeshesBegin == nullptr) == (subMeshesEnd == nullptr) &&
            subMeshesEndAddress >= subMeshesBeginAddress &&
            ((subMeshesEndAddress - subMeshesBeginAddress) % sizeof(void*)) ==
                0) {
            const std::size_t subMeshCount =
                (subMeshesEndAddress - subMeshesBeginAddress) / sizeof(void*);
            if (subMeshCount <= kMaximumSubMeshesPerProp) {
                registration.renderLayoutValid = true;
                registration.subMeshCount =
                    static_cast<std::uint32_t>(subMeshCount);
                for (std::size_t index = 0; index < subMeshCount; ++index) {
                    void* const subMesh = subMeshesBegin[index];
                    if (subMesh == nullptr) {
                        ++registration.unmatchedSubMeshes;
                        continue;
                    }
                    void* child = subMesh;
                    bool matched = false;
                    for (std::size_t depth = 0; depth < kMaximumParentDepth;
                         ++depth) {
                        void* const parent =
                            ReadAt<void*>(child, kEntityParentOffset);
                        if (parent == nullptr || parent == child) break;
                        if (ContainsBody(parent, begin, count)) {
                            if (depth == 0) {
                                ++registration.directBodyChildren;
                            } else {
                                ++registration.nestedBodyChildren;
                            }
                            matched = true;
                            break;
                        }
                        child = parent;
                    }
                    if (!matched) ++registration.unmatchedSubMeshes;
                }
            }
        }
    }
    registration.result = RegisterResult::registered;
    return registration;
}

bool Registry::ClassifyBody(void* const physicsBody,
                            std::uint32_t& propType) const noexcept {
    if (physicsBody == nullptr) return false;
    const std::size_t start = HashPointer(physicsBody) % entries_.size();
    for (std::size_t probe = 0; probe < kMaximumProbe; ++probe) {
        const Entry& entry = entries_[(start + probe) % entries_.size()];
        void* const existing = entry.body.load(std::memory_order_acquire);
        if (existing == physicsBody) {
            propType = entry.propType.load(std::memory_order_acquire);
            return propType < kPropTypeCount;
        }
        if (existing == nullptr) return false;
    }
    return false;
}

const wchar_t* RegisterResultName(const RegisterResult result) noexcept {
    switch (result) {
    case RegisterResult::registered: return L"registered";
    case RegisterResult::bodyless: return L"bodyless";
    case RegisterResult::invalid: return L"invalid";
    case RegisterResult::capacityExhausted: return L"capacity-exhausted";
    }
    return L"unknown";
}

const wchar_t* PropTypeName(const std::uint32_t propType) noexcept {
    constexpr std::array<const wchar_t*, kPropTypeCount> names{
        L"object", L"swingDoor", L"lever", L"wheel", L"lamp",
        L"photocell", L"moveObject", L"item", L"chest", L"critter",
        L"levelDoor", L"button", L"emotionStone", L"npc", L"multiSlider"};
    return propType < names.size() ? names[propType] : L"unknown";
}

} // namespace amfp::world_ownership
