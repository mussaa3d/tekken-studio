#include "mod/replaydisplay.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <string>

#include "mod/log.h"
#include "mod/ue.h"

namespace replaydisplay {

namespace {

struct Row {
    const char* Label;
    const char* Function; // on the replay's panel widget
    int Side;             // the fighter it's for, or -1 for both
};

const Row g_rows[Rows] = {
    { "Player command history", "SetVisibilityHistory", 0 },
    { "Opponent command history", "SetVisibilityHistory", 1 },
    { "Health numbers", "SetVisibilityHpInfo", -1 },
    { "Combo counter", "SetVisibilityComboInfo", -1 },
    { "Player attack info", "SetVisibilityDamageInfo", 0 },
    { "Opponent attack info", "SetVisibilityDamageInfo", 1 },
    { "Player frame info", "SetVisibilityFrameInfo", 0 },
    { "Opponent frame info", "SetVisibilityFrameInfo", 1 },
    { "Player frame advantage", "SetVisibilityFrameSimpleInfo", 0 },
    { "Opponent frame advantage", "SetVisibilityFrameSimpleInfo", 1 },
};

int g_choices[Rows] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

// The panel widget is there before the fight is, but not always when the fight is found, so a new fight tries a few times.
constexpr int TriesPerFight = 8;
constexpr ULONGLONG TryGapMs = 750;
std::atomic<int> g_tries { 0 };
ULONGLONG g_nextTryAt = 0;

// The functions are implemented by the widget's Blueprint, so they have to be looked up on its own class and not the C++ one it comes from.
struct Bound {
    std::string Class, Function;
    ue::FunctionRef Ref;
};

ue::FunctionRef& RefFor(const std::string& cls, const char* function)
{
    static std::deque<Bound> bound;
    for (Bound& b : bound)
        if (b.Class == cls && b.Function == function)
            return b.Ref;
    Bound& added = bound.emplace_back();
    added.Class = cls;
    added.Function = function;
    added.Ref.ClassName = added.Class.c_str();
    added.Ref.FunctionName = added.Function.c_str();
    return added.Ref;
}

// A widget half way through going away shouldn't take the game with it.
bool Guarded(void* object, ue::FunctionRef& ref, void* params)
{
    __try {
        return ue::Call(object, ref, params);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SetPanel(void* widget, const std::string& cls, const char* function, int side, bool visible)
{
    struct Params {
        int32_t Side;
        bool Visible;
    };
    // The game's own setting doesn't go through these functions, so a panel it put up counts as hidden here and a hide alone does nothing.
    // Showing it first makes the hide count, and both land before the next picture, so nothing flashes.
    if (!visible) {
        Params show { side, true };
        Guarded(widget, RefFor(cls, function), &show);
    }
    Params params { side, visible };
    return Guarded(widget, RefFor(cls, function), &params);
}

void Apply()
{
    void* cls = ue::FindClass("PolarisUMGPractice");
    if (!cls)
        return;
    for (void* widget : ue::AllObjectsOf(cls)) {
        if (ue::Field<uint32_t>(widget, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
            continue;
        const std::string name = ue::ClassNameOf(widget);
        for (int row = 0; row < Rows; ++row) {
            if (g_choices[row] < 0)
                continue;
            const bool visible = g_choices[row] > 0;
            bool done = true;
            if (g_rows[row].Side < 0) {
                done = SetPanel(widget, name, g_rows[row].Function, 0, visible) && done;
                done = SetPanel(widget, name, g_rows[row].Function, 1, visible) && done;
            } else {
                done = SetPanel(widget, name, g_rows[row].Function, g_rows[row].Side, visible);
            }
            if (!done)
                mod::Log("Replay display: %s didn't take", g_rows[row].Label);
        }
    }
}

} // namespace

const char* Label(int row)
{
    return g_rows[row].Label;
}

int& Choice(int row)
{
    return g_choices[row];
}

void Changed()
{
    g_tries = 1;
    g_nextTryAt = 0;
}

void ClearChoices()
{
    std::fill(std::begin(g_choices), std::end(g_choices), -1);
}

void OnFightFound()
{
    g_tries = TriesPerFight;
    g_nextTryAt = 0;
}

void OnGameTick()
{
    if (g_tries.load(std::memory_order_relaxed) <= 0)
        return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextTryAt)
        return;
    g_nextTryAt = now + TryGapMs;
    g_tries.fetch_sub(1, std::memory_order_relaxed);
    if (std::any_of(std::begin(g_choices), std::end(g_choices), [](int choice) { return choice >= 0; }))
        Apply();
}

} // namespace replaydisplay
