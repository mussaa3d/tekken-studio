#include "mod/replaydisplay.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

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

// The status text: one border per item, in the left and right info widgets. The game only fades them in and out, so hiding a border sticks.
struct StatusItem {
    const char* Label;
    const char* Borders[2];
};
const StatusItem g_statusItems[StatusItems] = {
    { "Hit combo", { "Border_Combo", "Border_Combo_P1" } },
    { "Damage", { "Border_Damage", nullptr } },
    { "Counter hit", { "Border_Counter", nullptr } },
    { "Clean hit", { "Border_Clean", nullptr } },
    { "Punish", { "Border_Punish", nullptr } },
    { "Tornado", { "Border_Tornado", nullptr } },
};
bool g_statusShown[StatusItems] = { true, true, true, true, true, true };

struct FoundBorder {
    void* Object;
    uint32_t Index;
    int Item;
    bool HiddenByUs;
};
std::vector<FoundBorder> g_borders;

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

// Live borders only: the same names also exist on each widget's template, which isn't under a live practice widget.
void FindBorders()
{
    void* borderClass = ue::FindClass("Border");
    void* practice = ue::FindClass("PolarisUMGPractice");
    if (!borderClass || !practice)
        return;
    std::vector<FoundBorder> found;
    for (void* border : ue::AllObjectsOf(borderClass)) {
        if (ue::Field<uint32_t>(border, ue::off::ObjectFlags) & 0x30)
            continue;
        const std::string name = ue::NameOf(border);
        int item = -1;
        for (int i = 0; i < StatusItems && item < 0; ++i)
            for (const char* candidate : g_statusItems[i].Borders)
                if (candidate && name == candidate)
                    item = i;
        if (item < 0)
            continue;
        void* owner = ue::Field<void*>(border, 0x20);
        owner = owner ? ue::Field<void*>(owner, 0x20) : nullptr;
        const std::string ownerClass = owner ? ue::ClassNameOf(owner) : std::string();
        if (ownerClass != "WBP_UI_Practice_Info_L_S2_C" && ownerClass != "WBP_UI_Practice_Info_R_S2_C")
            continue;
        bool live = false;
        for (void* outer = owner; outer && !live; outer = ue::Field<void*>(outer, 0x20))
            live = ue::IsA(outer, practice);
        if (live)
            found.push_back({ border, ue::Field<uint32_t>(border, ue::off::InternalIndex), item, ue::WidgetVisibility(border) == ue::SlateVisibility::Hidden });
    }
    g_borders = std::move(found);
}

void KeepItems()
{
    std::erase_if(g_borders, [](const FoundBorder& border) { return !ue::StillAlive(border.Object, border.Index); });
    for (FoundBorder& border : g_borders) {
        if (!g_statusShown[border.Item]) {
            if (ue::WidgetVisibility(border.Object) != ue::SlateVisibility::Hidden)
                ue::SetWidgetVisibility(border.Object, ue::SlateVisibility::Hidden);
            border.HiddenByUs = true;
        } else if (border.HiddenByUs) {
            ue::SetWidgetVisibility(border.Object, ue::SlateVisibility::Visible);
            border.HiddenByUs = false;
        }
    }
}

} // namespace

const char* StatusLabel(int item)
{
    return g_statusItems[item].Label;
}

bool& StatusShown(int item)
{
    return g_statusShown[item];
}

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
    std::fill(std::begin(g_statusShown), std::end(g_statusShown), true);
}

void OnFightFound()
{
    g_tries = TriesPerFight;
    g_nextTryAt = 0;
}

void OnGameTick()
{
    KeepItems();
    if (g_tries.load(std::memory_order_relaxed) <= 0)
        return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextTryAt)
        return;
    g_nextTryAt = now + TryGapMs;
    g_tries.fetch_sub(1, std::memory_order_relaxed);
    if (std::any_of(std::begin(g_statusShown), std::end(g_statusShown), [](bool shown) { return !shown; }) || !g_borders.empty())
        FindBorders();
    if (std::any_of(std::begin(g_choices), std::end(g_choices), [](int choice) { return choice >= 0; }))
        Apply();
}

} // namespace replaydisplay
