#include "mod/scan.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#ifndef TEKKEN8MVM_TESTS
#include <windows.h>
#endif

namespace scan {

std::optional<Pattern> Parse(const std::string& text)
{
    Pattern pattern;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token) {
        if (token == "??" || token == "?") {
            pattern.Bytes.push_back(-1);
            continue;
        }
        if (token.size() != 2)
            return std::nullopt;
        char* end = nullptr;
        const long value = std::strtol(token.c_str(), &end, 16);
        if (end != token.c_str() + 2)
            return std::nullopt;
        pattern.Bytes.push_back(static_cast<int>(value));
    }
    if (pattern.Bytes.empty())
        return std::nullopt;
    return pattern;
}

std::vector<const uint8_t*> FindAll(const uint8_t* begin, size_t size, const Pattern& pattern)
{
    std::vector<const uint8_t*> hits;
    const size_t n = pattern.Bytes.size();
    if (size < n)
        return hits;

    // Only positions where the first fixed byte matches get the full comparison.
    size_t anchor = 0;
    while (anchor < n && pattern.Bytes[anchor] < 0)
        ++anchor;
    if (anchor == n)
        return hits;
    const auto first = static_cast<uint8_t>(pattern.Bytes[anchor]);

    const uint8_t* const last = begin + size - n;
    for (const uint8_t* p = begin; p <= last; ++p) {
        if (p[anchor] != first)
            continue;
        bool match = true;
        for (size_t i = 0; i < n; ++i)
            if (pattern.Bytes[i] >= 0 && p[i] != pattern.Bytes[i]) {
                match = false;
                break;
            }
        if (match)
            hits.push_back(p);
    }
    return hits;
}

#ifndef TEKKEN8MVM_TESTS
Module MainModule()
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return { base, nt->OptionalHeader.SizeOfImage, nt->FileHeader.TimeDateStamp };
}

bool IsReadable(const void* address, size_t size)
{
    if (!address)
        return false;
    MEMORY_BASIC_INFORMATION info;
    auto p = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = p + (size ? size : 1);
    while (p < end) {
        if (!VirtualQuery(reinterpret_cast<void*>(p), &info, sizeof(info)) || info.State != MEM_COMMIT
            || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        p = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

uintptr_t Resolve(const Module& module, const Signature& signature, std::string& error)
{
    const auto pattern = Parse(signature.Text);
    if (!pattern) {
        error = "bad pattern";
        return 0;
    }

    std::vector<const uint8_t*> hits;
    const uintptr_t end = module.Base + module.Size;
    MEMORY_BASIC_INFORMATION info;
    for (uintptr_t p = module.Base; p < end && VirtualQuery(reinterpret_cast<void*>(p), &info, sizeof(info));
         p = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize) {
        const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        const DWORD readable = executable | PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY;
        if (info.State != MEM_COMMIT || !(info.Protect & readable) || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            continue;
        const uintptr_t lo = std::max(reinterpret_cast<uintptr_t>(info.BaseAddress), module.Base);
        const uintptr_t hi = std::min(reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize, end);
        auto found = FindAll(reinterpret_cast<const uint8_t*>(lo), hi - lo, *pattern);
        hits.insert(hits.end(), found.begin(), found.end());
        if (hits.size() > 1)
            break;
    }

    if (hits.empty()) {
        error = "pattern not found";
        return 0;
    }
    if (hits.size() > 1) {
        error = "pattern matches more than once";
        return 0;
    }

    auto address = reinterpret_cast<uintptr_t>(hits.front()) + signature.Offset;
    if (signature.RipRelative) {
        const int32_t rel = *reinterpret_cast<const int32_t*>(address);
        address = address + 4 + rel;
    }
    return address;
}
#endif

} // namespace scan
