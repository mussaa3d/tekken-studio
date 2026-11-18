// A write watch for finding out which of the game's code writes a word of its memory: the word's page is made read-only, each
// faulting write is counted by the address of the instruction that made it, and the write is let through. Research only, driven
// from the test channel ("watch", "watchdump", "watchstop"). The instruction addresses are offsets into the exe, to look up in
// ttt2-studio.map.
#include "studio.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <sstream>

namespace studio {

namespace {

std::mutex g_mutex;
std::map<uint64_t, uint32_t> g_writers; // instruction's offset in the exe -> how many writes
std::atomic<uint8_t*> g_page { nullptr };
std::atomic<uint8_t*> g_from { nullptr };
std::atomic<uint8_t*> g_to { nullptr };
std::atomic<bool> g_on { false };
std::atomic<DWORD> g_protect { PAGE_READONLY };
void* g_handler = nullptr;
thread_local bool t_stepping = false;

LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    uint8_t* page = g_page.load();
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && page && record->NumberParameters >= 2 && record->ExceptionInformation[0] <= 1) {
        auto* at = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
        if (at >= page && at < page + 0x1000) {
            if (at >= g_from.load() && at < g_to.load()) {
                // Reads are kept apart from writes by the top bit.
                const uint64_t offset = (reinterpret_cast<uint64_t>(record->ExceptionAddress) - reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr)))
                                        | (record->ExceptionInformation[0] == 0 ? 1ull << 63 : 0);
                std::lock_guard lock(g_mutex);
                if (g_writers.size() < 256 || g_writers.count(offset))
                    ++g_writers[offset];
            }
            // Let this one instruction write, then protect the page again.
            DWORD old;
            VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
            info->ContextRecord->EFlags |= 0x100;
            t_stepping = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_stepping) {
        t_stepping = false;
        if (g_on.load() && page) {
            DWORD old;
            VirtualProtect(page, 0x1000, g_protect.load(), &old);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void WatchStart(uint32_t address, uint32_t size, bool reads)
{
    WatchStop();
    uint8_t* base = GuestBase();
    if (!base || !Readable(address, size))
        return;
    {
        std::lock_guard lock(g_mutex);
        g_writers.clear();
    }
    if (!g_handler)
        g_handler = AddVectoredExceptionHandler(1, Handler);
    g_from = base + address;
    g_to = base + address + size;
    uint8_t* page = base + (address & ~0xFFFu);
    g_page = page;
    g_protect = reads ? PAGE_NOACCESS : PAGE_READONLY;
    g_on = true;
    DWORD old;
    VirtualProtect(page, 0x1000, g_protect.load(), &old);
    Log("Watch: writes to %08X..%08X", address, address + size);
}

void WatchStop()
{
    if (!g_on.exchange(false))
        return;
    if (uint8_t* page = g_page.load()) {
        DWORD old;
        VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
    }
}

std::string WatchDump()
{
    // The page's protection can be taken off by the game's own state keeping, so put it back each time this is asked.
    if (g_on.load()) {
        DWORD old;
        VirtualProtect(g_page.load(), 0x1000, g_protect.load(), &old);
    }
    std::ostringstream out;
    std::lock_guard lock(g_mutex);
    for (const auto& [offset, count] : g_writers)
        out << ((offset >> 63) ? "reader " : "writer ") << std::hex << (offset & ~(1ull << 63)) << std::dec << " " << count << "\n";
    out << "writers " << g_writers.size() << "\n";
    return out.str();
}

} // namespace studio
