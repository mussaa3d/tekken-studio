#pragma once
// Finds code in the game by byte pattern, so a patch that only moves things costs nothing.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scan {

struct Pattern {
    std::vector<int> Bytes; // -1 = wildcard
};

std::optional<Pattern> Parse(const std::string& text);

// Every match of the pattern in a block of memory.
std::vector<const uint8_t*> FindAll(const uint8_t* begin, size_t size, const Pattern& pattern);

// A pattern entry written as "AA BB ?? CC | 3". The optional number is an offset. With rip=true, the 4
// bytes at that offset are read as a rip-relative operand and the target address is returned.
struct Signature {
    std::string Name;
    std::string Text;
    int Offset = 0;
    bool RipRelative = false;
    bool Required = true;
};

#ifndef T8CINE_TESTS
struct Module {
    uintptr_t Base = 0;
    size_t Size = 0;
    uint32_t TimeDateStamp = 0;
};
Module MainModule();

bool IsReadable(const void* address, size_t size);

// Finds exactly one match inside the module's executable sections. Results are not cached: the scan
// takes well under a second on the game's code section.
uintptr_t Resolve(const Module& module, const Signature& signature, std::string& error);
#endif

} // namespace scan
