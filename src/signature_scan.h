#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace amfp::signature {

struct Pattern {
    std::span<const std::uint8_t> bytes;
    std::string_view mask;
};

enum class FindStatus {
    invalidPattern,
    noMatch,
    unique,
    ambiguous,
};

struct FindResult {
    FindStatus status = FindStatus::invalidPattern;
    std::size_t offset = 0;
    std::size_t matchCount = 0;
};

[[nodiscard]] FindResult FindUnique(std::span<const std::uint8_t> haystack,
                                    Pattern pattern) noexcept;

} // namespace amfp::signature
