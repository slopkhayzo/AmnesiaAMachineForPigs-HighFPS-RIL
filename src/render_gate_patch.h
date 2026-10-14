#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace amfp::render_gate {

inline constexpr std::array<std::uint8_t, 6> kExpectedBranch{
    0x0F, 0x84, 0xF5, 0x01, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, 6> kBypassBranch{
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

struct Patch {
    void* address = nullptr;
    bool applied = false;
};

[[nodiscard]] bool Apply(Patch& patch, std::wstring& error);
[[nodiscard]] bool Restore(Patch& patch, std::wstring& error);

} // namespace amfp::render_gate
