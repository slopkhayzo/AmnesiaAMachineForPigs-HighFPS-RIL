#include "presentation_clock.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

int g_failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

void TestCadence(const unsigned int nativeRate,
                 const unsigned int presentationRate) {
    using amfp::clock::PresentationClock;
    using amfp::clock::SampleKind;

    const double fixedStep = 1.0 / static_cast<double>(nativeRate);
    const double renderStep = 1.0 / static_cast<double>(presentationRate);
    PresentationClock clock(fixedStep);
    Expect(clock.IsConfigured(), "valid native step must configure the clock");

    std::uint64_t serial = 0;
    double latestTickTime = 0.0;
    double nextTickTime = fixedStep;
    double previousDisplayed = -1.0;
    unsigned int valid = 0;

    (void)clock.Publish(serial, 0.0);
    for (double now = 0.0; now <= 4.0; now += renderStep) {
        while (nextTickTime <= now + 1.0e-9) {
            latestTickTime = nextTickTime;
            ++serial;
            const auto published = clock.Publish(serial, now);
            Expect(published.accepted, "fixed endpoint should publish");
            nextTickTime += fixedStep;
        }

        const double alpha = (now - latestTickTime) / fixedStep;
        const auto sample = clock.Evaluate(now, alpha);
        if (sample.kind != SampleKind::interpolated) {
            continue;
        }
        Expect(sample.newerSerial == serial,
               "presentation pair must end at the current fixed serial");
        Expect(sample.olderSerial + 1U == sample.newerSerial,
               "presentation must retain two adjacent states");
        Expect(sample.alpha >= 0.0 && sample.alpha <= 1.0,
               "logic-timer alpha must remain bounded");

        const double displayed =
            static_cast<double>(sample.olderSerial) + sample.alpha;
        Expect(displayed + 1.0e-8 >= previousDisplayed,
               "displayed fixed time must not regress");
        Expect(std::abs(sample.targetTime - displayed * fixedStep) < 1.0e-9,
               "target time must use the configured native step");
        previousDisplayed = displayed;
        ++valid;
    }

    Expect(valid > presentationRate * 3U,
           "cadence did not produce enough two-state samples");
}

void TestParameterizedRates() {
    for (const unsigned int nativeRate : {50U, 60U, 120U}) {
        TestCadence(nativeRate, nativeRate * 3U / 2U);
        TestCadence(nativeRate, nativeRate * 2U);
        TestCadence(nativeRate, nativeRate * 2U + 1U);
    }

    amfp::clock::PresentationClock clock120(1.0 / 120.0);
    (void)clock120.Publish(100, 0.0);
    (void)clock120.Publish(101, 1.0 / 120.0);
    const auto sample = clock120.Evaluate(1.5 / 120.0, 0.5);
    Expect(std::abs(sample.targetTime - (100.5 / 120.0)) < 1.0e-10,
           "120 Hz clock must not inherit a hidden 60 Hz time basis");
}

void TestPairAndResetPaths() {
    using amfp::clock::PresentationClock;
    using amfp::clock::ResetReason;
    using amfp::clock::SampleKind;

    constexpr double step = 1.0 / 60.0;
    PresentationClock clock(step);
    (void)clock.Publish(10, 0.0);
    Expect(clock.Evaluate(0.0, 0.0).kind == SampleKind::unavailable,
           "one endpoint must not interpolate");
    (void)clock.Publish(11, step);
    const auto midpoint = clock.Evaluate(1.5 * step, 0.5);
    Expect(midpoint.kind == SampleKind::interpolated &&
               midpoint.olderSerial == 10 && midpoint.newerSerial == 11 &&
               std::abs(midpoint.alpha - 0.5) < 1.0e-8,
           "midpoint should sample the current adjacent pair");

    const auto skipped = clock.Publish(13, 2.0 * step);
    Expect(skipped.resetReason == ResetReason::missingSnapshots &&
               clock.SnapshotCount() == 1,
           "skipped serial must reset to the new endpoint");

    const auto repeated = clock.Publish(13, 3.0 * step);
    Expect(repeated.resetReason == ResetReason::nonMonotonicTick,
           "repeated serial must reset");

    PresentationClock discontinuity(step);
    (void)discontinuity.Publish(0, 0.0);
    (void)discontinuity.Publish(1, step);
    const auto cut = discontinuity.Publish(2, 2.0 * step, true);
    Expect(cut.resetReason == ResetReason::explicitDiscontinuity &&
               discontinuity.SnapshotCount() == 1,
           "explicit discontinuity should retain only its endpoint");

    PresentationClock stalled(step);
    (void)stalled.Publish(0, 0.0);
    (void)stalled.Publish(1, step);
    (void)stalled.Evaluate(2.0 * step, 1.0);
    const auto stall = stalled.Evaluate(0.300, 1.0);
    Expect(stall.kind == SampleKind::reset &&
               stall.resetReason == ResetReason::longStall,
           "long presentation stall must reset");

    PresentationClock invalidStep(0.0);
    Expect(!invalidStep.IsConfigured(), "zero native step must be rejected");
    const auto invalidPublish = invalidStep.Publish(0, 0.0);
    Expect(!invalidPublish.accepted &&
               invalidPublish.resetReason == ResetReason::invalidConfiguration,
           "invalid native step must fail closed");

    PresentationClock invalidWall(step);
    const auto invalidTime = invalidWall.Publish(
        0, std::numeric_limits<double>::quiet_NaN());
    Expect(!invalidTime.accepted &&
               invalidTime.resetReason == ResetReason::nonMonotonicWallTime,
           "non-finite wall time must fail closed");
}

void TestLogicTimerAlpha() {
    double alpha = -1.0;
    double raw = -1.0;
    Expect(amfp::clock::LogicTimerAlpha(100.0, 1000.0 / 60.0, 1.0,
                                        100.0 - (1000.0 / 120.0), 0.0,
                                        alpha, &raw) &&
               std::abs(alpha - 0.5) < 1.0e-8 &&
               std::abs(raw - 0.5) < 1.0e-8,
           "logic-timer midpoint residual was incorrect");
    Expect(amfp::clock::LogicTimerAlpha(100.0, 1000.0 / 120.0, 1.0,
                                        100.0 - (1000.0 / 240.0), 0.0,
                                        alpha) &&
               std::abs(alpha - 0.5) < 1.0e-8,
           "logic-timer math must support a 120 Hz native step");
    Expect(!amfp::clock::LogicTimerAlpha(100.0, 0.0, 1.0, 100.0, 0.0,
                                         alpha),
           "invalid logic-timer step must fail closed");
}

} // namespace

int main() {
    TestParameterizedRates();
    TestPairAndResetPaths();
    TestLogicTimerAlpha();

    if (g_failures != 0) {
        std::cerr << g_failures << " presentation-clock assertion(s) failed\n";
        return 1;
    }
    std::cout << "parameterized two-state presentation clock tests passed\n";
    return 0;
}
