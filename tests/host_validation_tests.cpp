#include "host_validation.h"

#include <Windows.h>

#include <iostream>
#include <string>

int main() {
    const auto nullResult = amfp::host::ValidateTarget(nullptr);
    if (nullResult.ok || nullResult.error.empty()) {
        std::cerr << "null module did not fail closed\n";
        return 1;
    }

    const auto currentHost = amfp::host::ValidateTarget(GetModuleHandleW(nullptr));
    if (currentHost.ok ||
        currentHost.error.find(L"does not look like the game executable") ==
            std::wstring::npos) {
        std::wcerr << L"test executable was not rejected by target identity: "
                   << currentHost.error << L'\n';
        return 2;
    }

    std::cout << "host validation rejects null and incompatible modules\n";
    return 0;
}
