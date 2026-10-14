#include "render_gate_patch.h"

#include <Windows.h>

#include <cstring>
#include <span>

namespace amfp::render_gate {
namespace {

bool IsExecutableRange(const void* address, const std::size_t bytes) noexcept {
    if (address == nullptr || bytes == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE |
                                 PAGE_EXECUTE_WRITECOPY;
    if ((memory.Protect & executable) == 0) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + memory.RegionSize;
    return begin >= regionBegin && begin <= regionEnd && bytes <= regionEnd - begin;
}

bool ReplaceBytes(void* address,
                  const std::span<const std::uint8_t> expected,
                  const std::span<const std::uint8_t> replacement,
                  std::wstring& error) {
    if (expected.size() != replacement.size() ||
        !IsExecutableRange(address, expected.size())) {
        error = L"render gate patch site is not a valid executable range";
        return false;
    }
    if (std::memcmp(address, expected.data(), expected.size()) != 0) {
        error = L"render gate instruction bytes failed validation";
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(address, replacement.size(), PAGE_EXECUTE_READWRITE,
                        &oldProtection)) {
        error = L"VirtualProtect(render gate) failed with Win32 error " +
                std::to_wstring(GetLastError());
        return false;
    }

    std::memcpy(address, replacement.data(), replacement.size());
    const bool flushed =
        FlushInstructionCache(GetCurrentProcess(), address, replacement.size()) != FALSE;
    DWORD ignored = 0;
    const bool protectedAgain =
        VirtualProtect(address, replacement.size(), oldProtection, &ignored) != FALSE;
    if (!flushed || !protectedAgain ||
        std::memcmp(address, replacement.data(), replacement.size()) != 0) {
        error = L"render gate instruction write could not be verified";
        return false;
    }
    return true;
}

} // namespace

bool Apply(Patch& patch, std::wstring& error) {
    if (patch.applied) {
        error = L"render gate patch was already applied";
        return false;
    }
    if (!ReplaceBytes(patch.address, kExpectedBranch, kBypassBranch, error)) {
        return false;
    }
    patch.applied = true;
    return true;
}

bool Restore(Patch& patch, std::wstring& error) {
    if (!patch.applied) {
        error = L"render gate patch was not applied";
        return false;
    }
    if (!ReplaceBytes(patch.address, kBypassBranch, kExpectedBranch, error)) {
        return false;
    }
    patch.applied = false;
    return true;
}

} // namespace amfp::render_gate
