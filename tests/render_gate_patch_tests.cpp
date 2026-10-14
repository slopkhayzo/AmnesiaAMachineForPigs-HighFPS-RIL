#include "render_gate_patch.h"

#include <Windows.h>

#include <cstring>
#include <iostream>
#include <string>

namespace {

struct Allocation final {
    void* value = nullptr;
    ~Allocation() {
        if (value != nullptr) {
            VirtualFree(value, 0, MEM_RELEASE);
        }
    }
};

bool Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    Allocation memory{VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE)};
    if (!Require(memory.value != nullptr, "VirtualAlloc failed")) {
        return 1;
    }

    std::memcpy(memory.value, amfp::render_gate::kExpectedBranch.data(),
                amfp::render_gate::kExpectedBranch.size());
    amfp::render_gate::Patch patch{memory.value};
    std::wstring error;
    if (!Require(amfp::render_gate::Apply(patch, error),
                 "valid gate branch was not patched") ||
        !Require(patch.applied, "patch state was not recorded") ||
        !Require(std::memcmp(memory.value,
                            amfp::render_gate::kBypassBranch.data(),
                            amfp::render_gate::kBypassBranch.size()) == 0,
                 "gate branch was not replaced with NOPs")) {
        return 2;
    }
    if (!Require(amfp::render_gate::Restore(patch, error),
                 "patched gate branch was not restored") ||
        !Require(!patch.applied, "restored patch remained marked active") ||
        !Require(std::memcmp(memory.value,
                            amfp::render_gate::kExpectedBranch.data(),
                            amfp::render_gate::kExpectedBranch.size()) == 0,
                 "original gate branch was not restored")) {
        return 3;
    }

    static_cast<unsigned char*>(memory.value)[0] = 0xCC;
    if (!Require(!amfp::render_gate::Apply(patch, error),
                 "mismatched gate bytes were accepted") ||
        !Require(static_cast<unsigned char*>(memory.value)[0] == 0xCC,
                 "failed validation modified the gate bytes")) {
        return 4;
    }

    std::cout << "render gate patch validates, applies, and restores atomically\n";
    return 0;
}
