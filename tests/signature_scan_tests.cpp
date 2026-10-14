#include "signature_scan.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

bool Require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    using amfp::signature::FindStatus;
    using amfp::signature::Pattern;

    constexpr std::array<std::uint8_t, 10> haystack{
        0x10, 0x20, 0x30, 0x40, 0x50, 0x20, 0x31, 0x40, 0x60, 0x70};
    constexpr std::array<std::uint8_t, 3> uniqueBytes{0x30, 0x40, 0x50};
    constexpr std::array<std::uint8_t, 3> wildcardBytes{0x20, 0x00, 0x40};
    constexpr std::array<std::uint8_t, 2> missingBytes{0xAA, 0xBB};

    const auto unique = amfp::signature::FindUnique(
        haystack, Pattern{uniqueBytes, "xxx"});
    if (!Require(unique.status == FindStatus::unique && unique.offset == 2 &&
                     unique.matchCount == 1,
                 "unique signature was not resolved at offset 2")) {
        return 1;
    }

    const auto ambiguous = amfp::signature::FindUnique(
        haystack, Pattern{wildcardBytes, "x?x"});
    if (!Require(ambiguous.status == FindStatus::ambiguous &&
                     ambiguous.matchCount == 2,
                 "wildcard signature did not fail on ambiguity")) {
        return 2;
    }

    const auto missing = amfp::signature::FindUnique(
        haystack, Pattern{missingBytes, "xx"});
    if (!Require(missing.status == FindStatus::noMatch &&
                     missing.matchCount == 0,
                 "missing signature did not report no match")) {
        return 3;
    }

    const auto badLength = amfp::signature::FindUnique(
        haystack, Pattern{uniqueBytes, "xx"});
    const auto badMask = amfp::signature::FindUnique(
        haystack, Pattern{uniqueBytes, "x*x"});
    const auto empty = amfp::signature::FindUnique(
        haystack, Pattern{{}, {}});
    if (!Require(badLength.status == FindStatus::invalidPattern &&
                     badMask.status == FindStatus::invalidPattern &&
                     empty.status == FindStatus::invalidPattern,
                 "malformed signatures were not rejected")) {
        return 4;
    }

    const auto tooLarge = amfp::signature::FindUnique(
        std::span<const std::uint8_t>(haystack).first(2),
        Pattern{uniqueBytes, "xxx"});
    if (!Require(tooLarge.status == FindStatus::noMatch,
                 "oversized signature did not report no match")) {
        return 5;
    }

    std::cout << "signature scanner fails closed on every non-unique result\n";
    return 0;
}
