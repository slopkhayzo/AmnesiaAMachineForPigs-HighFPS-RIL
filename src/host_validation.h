#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace amfp::host {

struct Facts {
    std::uintptr_t imageBase = 0;
    std::uint32_t imageSize = 0;
    std::uint32_t timeDateStamp = 0;
    std::uint16_t machine = 0;
};

struct ValidationResult {
    bool ok = false;
    Facts facts{};
    std::wstring error;
};

[[nodiscard]] ValidationResult ValidateTarget(HMODULE executable);

} // namespace amfp::host
