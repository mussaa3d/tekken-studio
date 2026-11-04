#include "mod/replay.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "mod/log.h"

namespace replay {

namespace {

// Inside the replay manager's data: the list of saved points, and inside each point its round and frame.
constexpr int ManagerData = 0x98;
constexpr int PointsBegin = 0xE0;
constexpr int PointsEnd = 0xE8;
constexpr int PointType = 0;
constexpr int PointRound = 8;
constexpr int PointFrame = 0x14;

using GetRegistryFn = void* (*)();
using LookupFn = uint8_t* (*)(void*, const void*);
using GetManagerFn = uint8_t* (*)();
using CopyPointFn = void* (*)(void*, const void*);
using RestorePointFn = int (*)(void*, void*);
using InputFn = void (*)(void*);
using AfterJumpFn = void (*)(void*, int, int);

// In the replay controller: set while the transition after a jump is running.
constexpr int ControllerInTransition = 0x3C;

void* g_controller = nullptr;
int g_afterJumpSlot = 0;
int g_jumpCounterField = 0;
GetRegistryFn g_getRegistry = nullptr;
LookupFn g_lookup = nullptr;
const void* g_battleKey = nullptr;
int g_frameOffset = 0, g_roundOffset = 0;
GetManagerFn g_getManager = nullptr;
CopyPointFn g_copyPoint = nullptr;
RestorePointFn g_restorePoint = nullptr;
int g_pointSize = 0;
InputFn oInput = nullptr;

std::atomic<int64_t> g_pendingFrame { -1 };
std::atomic<bool> g_pendingRoundStart { false };

// The fight moves on by this many frames at most during the transition after a jump. Round starts run their whole intro instead,
// so they're only used when that's where you asked to go.
constexpr uint32_t TransitionFrames = 17;
std::atomic<uint64_t> g_inputCalls { 0 };
std::atomic<bool> g_haveList { false };

std::mutex g_listMutex;
std::vector<SavedPoint> g_list;

template <typename T>
T CallTarget(uintptr_t instruction)
{
    int32_t offset = 0;
    std::memcpy(&offset, reinterpret_cast<const void*>(instruction + 1), sizeof(offset));
    return reinterpret_cast<T>(instruction + 5 + offset);
}

uintptr_t RipTarget(uintptr_t operand)
{
    int32_t offset = 0;
    std::memcpy(&offset, reinterpret_cast<const void*>(operand), sizeof(offset));
    return operand + 4 + offset;
}

// Outside a fight the battle system can be gone or half torn down.
bool ReadBattle(int& round, uint32_t& frame)
{
    __try {
        void* registry = g_getRegistry();
        if (!registry)
            return false;
        const uint8_t* battle = g_lookup(registry, g_battleKey);
        if (!battle)
            return false;
        round = *reinterpret_cast<const int*>(battle + g_roundOffset);
        frame = *reinterpret_cast<const uint32_t*>(battle + g_frameOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The range of the list, or false when there's none or it doesn't look like one.
bool PointRange(const uint8_t*& begin, size_t& count)
{
    __try {
        const uint8_t* manager = g_getManager();
        const uint8_t* data = manager ? *reinterpret_cast<const uint8_t* const*>(manager + ManagerData) : nullptr;
        if (!data)
            return false;
        begin = *reinterpret_cast<const uint8_t* const*>(data + PointsBegin);
        const uint8_t* end = *reinterpret_cast<const uint8_t* const*>(data + PointsEnd);
        if (!begin || end <= begin || (end - begin) % g_pointSize != 0)
            return false;
        count = (end - begin) / g_pointSize;
        return count < 100000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadPoint(const uint8_t* point, SavedPoint& out)
{
    __try {
        out.Type = *reinterpret_cast<const int*>(point + PointType);
        out.Round = *reinterpret_cast<const int*>(point + PointRound);
        out.Frame = *reinterpret_cast<const uint32_t*>(point + PointFrame);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RefreshList()
{
    const uint8_t* begin = nullptr;
    size_t count = 0;
    std::vector<SavedPoint> list;
    if (PointRange(begin, count)) {
        list.resize(count);
        for (size_t i = 0; i < count; ++i)
            if (!ReadPoint(begin + i * g_pointSize, list[i])) {
                list.clear();
                break;
            }
    }
    g_haveList = !list.empty();
    std::lock_guard lock(g_listMutex);
    g_list = std::move(list);
}

// What the game's own rewind does once it has picked a point: copy it, and restore from the copy.
void RestoreLastPointBefore(void* controller, uint32_t frame, bool roundStart)
{
    const uint8_t* begin = nullptr;
    size_t count = 0;
    if (!PointRange(begin, count)) {
        mod::Log("Replay jump: no saved points");
        return;
    }
    const uint8_t* best = nullptr;
    SavedPoint point, chosen;
    for (size_t i = 0; i < count; ++i) {
        if (!ReadPoint(begin + i * g_pointSize, point))
            return;
        const bool usable = roundStart ? point.Frame <= frame : (point.Frame + TransitionFrames <= frame && point.Type != TypeRoundStart);
        if (usable) {
            best = begin + i * g_pointSize;
            chosen = point;
        }
    }
    if (!best)
        best = begin;
    void* manager = g_getManager();
    std::vector<uint8_t> copy(g_pointSize + 0x40);
    const int result = g_restorePoint(manager, g_copyPoint(copy.data(), best));
    mod::Log("Replay jump: wanted frame %u, restored the point at %u (round %d, type %d), result %d", frame, chosen.Frame, chosen.Round, chosen.Type, result);

    // The same follow-up as the game's own rewind. It runs a short transition that resets both players' input state, and without
    // that the fight plays on differently from the original.
    auto* table = *reinterpret_cast<AfterJumpFn**>(controller);
    table[g_afterJumpSlot](controller, result, 0);
    std::memset(static_cast<uint8_t*>(controller) + g_jumpCounterField, 0, sizeof(uint64_t));
}

void DetourInput(void* controller)
{
    g_controller = controller;
    if (g_inputCalls.fetch_add(1, std::memory_order_relaxed) % 60 == 0)
        RefreshList();
    const int64_t frame = g_pendingFrame.exchange(-1);
    if (frame >= 0)
        RestoreLastPointBefore(controller, static_cast<uint32_t>(frame), g_pendingRoundStart.load());
    oInput(controller);
}

} // namespace

void Setup(const Code& code)
{
    if (!code.RewindSite || !code.RoundRead) {
        mod::Log("Replay position not found: the timeline counts steps and rewinding is off");
        return;
    }
    g_getManager = CallTarget<GetManagerFn>(code.RewindSite);

    // In the rewind code, right after the jump: a call through the controller's table, and a field it clears.
    const auto site = reinterpret_cast<const uint8_t*>(code.RewindSite);
    if (site[31] != 0x41 || site[32] != 0xFF || site[33] != 0x91 || site[0x41] != 0x48 || site[0x42] != 0x89 || site[0x43] != 0x77) {
        mod::Log("Replay rewind code has changed shape: the timeline counts steps and rewinding is off");
        return;
    }
    int afterJumpOffset = 0;
    std::memcpy(&afterJumpOffset, site + 34, sizeof(afterJumpOffset));
    g_afterJumpSlot = afterJumpOffset / 8;
    g_jumpCounterField = site[0x44];

    // The rewind fetches the battle system and passes its frame on to the real function: nearly everything we need is in those two.
    const uintptr_t thunk = reinterpret_cast<uintptr_t>(CallTarget<void*>(code.RewindSite + 13));
    const auto bytes = reinterpret_cast<const uint8_t*>(thunk);
    if (bytes[24] != 0xE8 || bytes[39] != 0xE8 || bytes[53] != 0x44 || bytes[75] != 0xE9) {
        mod::Log("Replay rewind code has changed shape: the timeline counts steps and rewinding is off");
        return;
    }
    g_getRegistry = CallTarget<GetRegistryFn>(thunk + 24);
    g_battleKey = reinterpret_cast<const void*>(RipTarget(thunk + 32));
    g_lookup = CallTarget<LookupFn>(thunk + 39);
    std::memcpy(&g_frameOffset, bytes + 56, sizeof(g_frameOffset));
    std::memcpy(&g_roundOffset, reinterpret_cast<const void*>(code.RoundRead + 22), sizeof(g_roundOffset));
    mod::Log("Replay position: frame at +0x%X, round at +0x%X", g_frameOffset, g_roundOffset);

    const uintptr_t body = reinterpret_cast<uintptr_t>(CallTarget<void*>(thunk + 75));
    const auto bodyBytes = reinterpret_cast<const uint8_t*>(body);
    if (bodyBytes[0x24] != 0xB9 || bodyBytes[0x48] != 0xE8 || bodyBytes[0x53] != 0xE8) {
        mod::Log("Replay restore code has changed shape: rewinding is off");
        return;
    }
    std::memcpy(&g_pointSize, bodyBytes + 0x25, sizeof(g_pointSize));
    g_copyPoint = CallTarget<CopyPointFn>(body + 0x48);
    g_restorePoint = CallTarget<RestorePointFn>(body + 0x53);

    if (code.InputHandler) {
        void* target = reinterpret_cast<void*>(code.InputHandler);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&DetourInput), reinterpret_cast<void**>(&oInput)) != MH_OK || MH_EnableHook(target) != MH_OK) {
            mod::Log("Replay input hook failed: rewinding is off");
            oInput = nullptr;
        }
    }
}

bool Position(int& round, uint32_t& frame)
{
    return g_getRegistry && ReadBattle(round, frame);
}

bool CanJump()
{
    return oInput && g_haveList;
}

void RequestJump(uint32_t frame, bool roundStart)
{
    g_pendingRoundStart = roundStart;
    g_pendingFrame = frame;
}

bool JumpPending()
{
    return g_pendingFrame.load() >= 0;
}

bool JumpSettling()
{
    if (!g_controller)
        return false;
    __try {
        return *(static_cast<const uint8_t*>(g_controller) + ControllerInTransition) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CancelJump()
{
    g_pendingFrame = -1;
}

std::vector<SavedPoint> SavedPoints()
{
    std::lock_guard lock(g_listMutex);
    return g_list;
}

} // namespace replay
