#include "signature_scan.h"

#include <algorithm>

namespace amfp::signature {
namespace {

bool IsValid(const Pattern pattern) noexcept {
    if (pattern.bytes.empty() || pattern.bytes.size() != pattern.mask.size()) {
        return false;
    }
    return std::ranges::all_of(pattern.mask, [](const char value) {
        return value == 'x' || value == '?';
    });
}

bool Matches(const std::span<const std::uint8_t> candidate,
             const Pattern pattern) noexcept {
    for (std::size_t index = 0; index < pattern.bytes.size(); ++index) {
        if (pattern.mask[index] == 'x' &&
            candidate[index] != pattern.bytes[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

FindResult FindUnique(const std::span<const std::uint8_t> haystack,
                      const Pattern pattern) noexcept {
    FindResult result{};
    if (!IsValid(pattern)) {
        return result;
    }
    if (haystack.size() < pattern.bytes.size()) {
        result.status = FindStatus::noMatch;
        return result;
    }

    const std::size_t last = haystack.size() - pattern.bytes.size();
    for (std::size_t offset = 0; offset <= last; ++offset) {
        if (!Matches(haystack.subspan(offset, pattern.bytes.size()), pattern)) {
            continue;
        }
        if (result.matchCount == 0) {
            result.offset = offset;
        }
        ++result.matchCount;
        if (result.matchCount > 1) {
            result.status = FindStatus::ambiguous;
            return result;
        }
    }

    result.status = result.matchCount == 1 ? FindStatus::unique
                                           : FindStatus::noMatch;
    return result;
}

} // namespace amfp::signature
