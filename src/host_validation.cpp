#include "host_validation.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace amfp::host {
namespace {

using ImportMap =
    std::unordered_map<std::string, std::unordered_set<std::string>>;

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const char ch) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    });
    return value;
}

std::wstring ModulePath(const HMODULE module) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD copied = GetModuleFileNameW(
            module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            return std::wstring(buffer.data(), copied);
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

std::wstring BaseName(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

template <typename T>
const T* AtRva(const std::uint8_t* base,
               const std::size_t imageSize,
               const std::uint32_t rva,
               const std::size_t count = 1) {
    if (rva > imageSize || count > (imageSize - rva) / sizeof(T)) {
        return nullptr;
    }
    return reinterpret_cast<const T*>(base + rva);
}

bool ReadAscii(const std::uint8_t* base,
               const std::size_t imageSize,
               const std::uint32_t rva,
               std::string& output) {
    if (rva >= imageSize) {
        return false;
    }
    const char* text = reinterpret_cast<const char*>(base + rva);
    const std::size_t available = imageSize - rva;
    const void* terminator = std::memchr(text, '\0', available);
    if (terminator == nullptr) {
        return false;
    }
    output.assign(text, static_cast<const char*>(terminator));
    return true;
}

bool CollectImports(const std::uint8_t* base,
                    const IMAGE_NT_HEADERS32& nt,
                    ImportMap& imports,
                    std::wstring& error) {
    const std::size_t imageSize = nt.OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY directory =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        error = L"PE import directory is absent or malformed";
        return false;
    }

    const std::size_t descriptorCapacity =
        directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    const IMAGE_IMPORT_DESCRIPTOR* descriptors = AtRva<IMAGE_IMPORT_DESCRIPTOR>(
        base, imageSize, directory.VirtualAddress, descriptorCapacity);
    if (descriptors == nullptr) {
        error = L"PE import directory lies outside the mapped image";
        return false;
    }

    bool foundTerminator = false;
    for (std::size_t descriptorIndex = 0;
         descriptorIndex < descriptorCapacity; ++descriptorIndex) {
        const IMAGE_IMPORT_DESCRIPTOR& descriptor = descriptors[descriptorIndex];
        if (descriptor.Name == 0 && descriptor.FirstThunk == 0) {
            foundTerminator = true;
            break;
        }

        std::string moduleName;
        if (!ReadAscii(base, imageSize, descriptor.Name, moduleName)) {
            error = L"An imported module name is outside the mapped image";
            return false;
        }
        moduleName = LowerAscii(moduleName);
        auto& names = imports[moduleName];

        const std::uint32_t thunkRva = descriptor.OriginalFirstThunk;
        if (thunkRva == 0) {
            continue;
        }
        const std::size_t maximumThunks =
            (imageSize - std::min<std::size_t>(thunkRva, imageSize)) /
            sizeof(IMAGE_THUNK_DATA32);
        const IMAGE_THUNK_DATA32* thunks = AtRva<IMAGE_THUNK_DATA32>(
            base, imageSize, thunkRva, maximumThunks);
        if (thunks == nullptr) {
            error = L"An import-name thunk table is outside the mapped image";
            return false;
        }

        bool foundThunkTerminator = false;
        for (std::size_t thunkIndex = 0; thunkIndex < maximumThunks; ++thunkIndex) {
            const DWORD value = thunks[thunkIndex].u1.AddressOfData;
            if (value == 0) {
                foundThunkTerminator = true;
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL32(value)) {
                continue;
            }
            const IMAGE_IMPORT_BY_NAME* import = AtRva<IMAGE_IMPORT_BY_NAME>(
                base, imageSize, value);
            if (import == nullptr) {
                error = L"An import-by-name record is outside the mapped image";
                return false;
            }
            std::string functionName;
            if (!ReadAscii(base, imageSize,
                           value + static_cast<std::uint32_t>(offsetof(
                                       IMAGE_IMPORT_BY_NAME, Name)),
                           functionName)) {
                error = L"An imported function name is outside the mapped image";
                return false;
            }
            names.insert(functionName);
        }
        if (!foundThunkTerminator) {
            error = L"An import thunk table has no in-image terminator";
            return false;
        }
    }
    if (!foundTerminator) {
        error = L"PE import descriptor table has no in-directory terminator";
        return false;
    }
    return true;
}

bool RequireModule(const ImportMap& imports,
                   const std::string& module,
                   std::wstring& error) {
    if (imports.contains(LowerAscii(module))) {
        return true;
    }
    error = L"Required import module is missing: " +
            std::wstring(module.begin(), module.end());
    return false;
}

bool RequireFunctions(const ImportMap& imports,
                      const std::string& module,
                      const std::initializer_list<std::string_view> functions,
                      std::wstring& error) {
    const auto foundModule = imports.find(LowerAscii(module));
    if (foundModule == imports.end()) {
        return RequireModule(imports, module, error);
    }
    for (const std::string_view function : functions) {
        if (!foundModule->second.contains(std::string(function))) {
            error = L"Required named import is missing from " +
                    std::wstring(module.begin(), module.end()) + L": " +
                    std::wstring(function.begin(), function.end());
            return false;
        }
    }
    return true;
}

} // namespace

ValidationResult ValidateTarget(const HMODULE executable) {
    ValidationResult result{};
    if (executable == nullptr) {
        result.error = L"Host module handle is null";
        return result;
    }

    const std::wstring path = ModulePath(executable);
    if (path.empty()) {
        result.error = L"Could not resolve the host executable path";
        return result;
    }
    // The file name is deliberately not checked: other storefronts may ship
    // the same engine under another name. The imports below and the code
    // signatures decide; the launcher fails the import check.

    const auto* base = reinterpret_cast<const std::uint8_t*>(executable);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 1024 * 1024) {
        result.error = L"Host DOS header is malformed";
        return result;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage < nt->OptionalHeader.SizeOfHeaders) {
        result.error = L"Host is not a structurally valid PE32/x86 image";
        return result;
    }

    result.facts.imageBase = reinterpret_cast<std::uintptr_t>(base);
    result.facts.imageSize = nt->OptionalHeader.SizeOfImage;
    result.facts.timeDateStamp = nt->FileHeader.TimeDateStamp;
    result.facts.machine = nt->FileHeader.Machine;

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    bool validText = false;
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        std::array<char, IMAGE_SIZEOF_SHORT_NAME + 1> name{};
        std::memcpy(name.data(), sections[index].Name, IMAGE_SIZEOF_SHORT_NAME);
        if (std::string_view(name.data()) == ".text" &&
            (sections[index].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 &&
            sections[index].VirtualAddress < nt->OptionalHeader.SizeOfImage) {
            validText = true;
            break;
        }
    }
    if (!validText) {
        result.error = L"Host has no valid executable .text section";
        return result;
    }

    ImportMap imports;
    if (!CollectImports(base, *nt, imports, result.error) ||
        !RequireFunctions(imports, "sdl2.dll",
                          {"SDL_PollEvent", "SDL_PumpEvents",
                           "SDL_GetRelativeMouseState", "SDL_GL_SwapWindow",
                           "SDL_GL_SetSwapInterval"},
                          result.error) ||
        !RequireModule(imports, "newton.dll", result.error) ||
        !RequireModule(imports, "opengl32.dll", result.error)) {
        // WinINet is not required: only the bundled loader's proxy name
        // depends on it, and the plugin works under any x86 ASI loader.
        result.error = L"Host " + BaseName(path) +
                       L" does not look like the game executable: " +
                       result.error;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace amfp::host
