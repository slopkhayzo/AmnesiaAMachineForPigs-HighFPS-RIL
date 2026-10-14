#include "particle_runtime.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>

namespace {

using amfp::particle::ApplyResult;
using amfp::particle::Vec3;

bool Require(const bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool Near(const float left, const float right) {
    return std::fabs(left - right) < 0.0001F;
}

amfp::clock::Sample Pair(const std::uint64_t newer, const double alpha) {
    amfp::clock::Sample sample{};
    sample.kind = amfp::clock::SampleKind::interpolated;
    sample.olderSerial = newer - 1;
    sample.newerSerial = newer;
    sample.alpha = alpha;
    return sample;
}

struct FakeParticle {
    Vec3 position{};
    Vec3 lastPosition{};
    std::array<std::uint8_t, 0x100> rest{};
};

struct FakeEmitter {
    std::array<std::uint8_t, 0x500> bytes{};
    std::array<FakeParticle, 3> particles{};
    std::array<void*, 3> pointers{};

    FakeEmitter() {
        for (std::size_t index = 0; index < particles.size(); ++index) {
            pointers[index] = &particles[index];
        }
        void** const begin = pointers.data();
        void** const end = begin + pointers.size();
        std::memcpy(bytes.data() + 0x3A0, &begin, sizeof(begin));
        std::memcpy(bytes.data() + 0x3A4, &end, sizeof(end));
        SetCount(2);
        const std::uint32_t maximum = 3;
        std::memcpy(bytes.data() + 0x3B4, &maximum, sizeof(maximum));
    }

    void SetCount(const std::uint32_t count) {
        std::memcpy(bytes.data() + 0x3B0, &count, sizeof(count));
    }

    // One fixed update of straight-line motion.
    void Step(const float distance) {
        for (FakeParticle& particle : particles) {
            particle.lastPosition = particle.position;
            particle.position.x += distance;
        }
    }
};

} // namespace

int main() {
    const auto runtime = std::make_unique<amfp::particle::Runtime>();
    FakeEmitter emitter;
    emitter.particles[1].position = {0.0F, 5.0F, 0.0F};
    emitter.particles[1].lastPosition = {0.0F, 5.0F, 0.0F};

    // Without an interpolated pair nothing is written.
    runtime->BeginRender({}, 10);
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::inactive,
                 "an unavailable clock sample was not inactive")) return 1;
    runtime->EndRender();

    // A pair that does not end at the newest fixed update is rejected.
    runtime->BeginRender(Pair(9, 0.5), 10);
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::inactive,
                 "a stale clock pair was accepted")) return 1;
    runtime->EndRender();

    // First sight is only an observation.
    emitter.Step(2.0F);
    runtime->BeginRender(Pair(10, 0.5), 10);
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::unknown,
                 "the first observation interpolated")) return 1;
    runtime->EndRender();

    // The next consecutive tick moved the particles: interpolate and restore.
    emitter.Step(2.0F);
    runtime->BeginRender(Pair(11, 0.25), 11);
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::applied,
                 "a moving emitter was not interpolated")) return 1;
    if (!Require(Near(emitter.particles[0].position.x, 2.5F) &&
                     Near(emitter.particles[0].lastPosition.x, 0.5F) &&
                     Near(emitter.particles[1].position.y, 5.0F),
                 "presented positions are wrong")) return 1;
    if (!Require(Near(emitter.particles[2].position.x, 4.0F),
                 "a particle beyond the live count was modified")) return 1;
    // A nested call while positions are substituted must not stack.
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::inactive,
                 "a nested call was not rejected")) return 1;
    runtime->Restore();
    if (!Require(emitter.particles[0].position.x == 4.0F &&
                     emitter.particles[0].lastPosition.x == 2.0F,
                 "authoritative positions were not restored exactly")) return 1;

    // Later draws in the same tick reuse the decision at a new alpha.
    runtime->BeginRender(Pair(11, 1.0), 11);
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::applied &&
                     Near(emitter.particles[0].position.x, 4.0F),
                 "alpha 1 did not present the authoritative position")) return 1;
    runtime->Restore();
    runtime->EndRender();

    // The emitter did not advance in tick 12: its stale pair is not replayed.
    runtime->BeginRender(Pair(12, 0.5), 12);
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::stationary &&
                     emitter.particles[0].position.x == 4.0F,
                 "a paused emitter was interpolated")) return 1;
    runtime->EndRender();

    // After a gap in observation the emitter must be seen twice again.
    emitter.Step(2.0F);
    runtime->BeginRender(Pair(20, 0.5), 20);
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::unknown,
                 "a gap in observation was bridged")) return 1;
    runtime->EndRender();

    // Empty, malformed, and non-finite emitters fail closed.
    emitter.SetCount(0);
    runtime->BeginRender(Pair(21, 0.5), 21);
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::empty,
                 "an empty emitter was not skipped")) return 1;
    emitter.SetCount(4);
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::invalid,
                 "a count above the maximum was accepted")) return 1;
    runtime->EndRender();

    emitter.SetCount(2);
    emitter.Step(1.0F);
    runtime->BeginRender(Pair(22, 0.5), 22);
    (void)runtime->Apply(emitter.bytes.data());
    runtime->EndRender();
    emitter.Step(1.0F);
    emitter.particles[1].lastPosition.x = NAN;
    runtime->BeginRender(Pair(23, 0.5), 23);
    if (!Require(runtime->Apply(emitter.bytes.data()) == ApplyResult::invalid &&
                     emitter.particles[0].position.x == 8.0F,
                 "a non-finite particle was written")) return 1;
    runtime->EndRender();

    const amfp::particle::Stats& stats = runtime->Statistics();
    if (!Require(stats.applied == 2 && stats.positions == 4 &&
                     stats.stationary == 1 && stats.invalid == 2,
                 "statistics do not match the sequence")) return 1;
    return 0;
}
