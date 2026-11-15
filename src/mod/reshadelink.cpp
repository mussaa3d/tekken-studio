#include "mod/reshadelink.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>

#include <reshade_events.hpp>

#include "common/config.h"
#include "mod/guard.h"
#include "mod/log.h"

extern "C" BOOL WINAPI K32EnumProcessModules(HANDLE hProcess, HMODULE* lphModule, DWORD cb, LPDWORD lpcbNeeded);

// What ReShade shows in its list of add-ons.
extern "C" __declspec(dllexport) const char* NAME = "tekken8mvm";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Lets tekken8mvm's editor set and key ReShade's effects, and records after them.";

namespace reshadelink {

namespace {

using reshade::api::effect_runtime;
using reshade::api::effect_technique;
using reshade::api::effect_uniform_variable;

// The API version this was written against, and the oldest one whose effect_runtime has everything used here in the same place.
constexpr uint32_t NewestApi = 20, OldestApi = 12;

enum class State { Looking, NotThere, TooOld, Ready };
std::atomic<State> g_state { State::Looking };
std::atomic<bool> g_drawing { false }; // ReShade has shown a frame, so its present event can be relied on
std::atomic<uint64_t> g_lastPresent { 0 };

struct LiveTechnique : Technique {
    effect_technique Handle {};
};
struct LiveValue : Value {
    effect_uniform_variable Handle {};
};

struct Change {
    bool IsTechnique = false;
    std::string Effect, Name;
    bool On = false;
    float Numbers[4] = {};
};

std::mutex g_mutex;
std::vector<LiveTechnique> g_techniques;
std::vector<LiveValue> g_values;
std::vector<Change> g_changes;
std::atomic<bool> g_stale { true }; // ReShade loaded its effects again, so every handle is new
uint64_t g_frame = 0;

template <typename Getter>
std::string Text(Getter get)
{
    char buffer[256] = "";
    size_t size = sizeof(buffer);
    get(buffer, &size);
    buffer[sizeof(buffer) - 1] = 0;
    return buffer;
}

// Render thread: reads what ReShade has loaded.
void ReadEverything(effect_runtime* runtime)
{
    std::vector<LiveTechnique> techniques;
    runtime->enumerate_techniques(nullptr, [&](effect_runtime* rt, effect_technique handle) {
        LiveTechnique technique;
        technique.Handle = handle;
        technique.Name = Text([&](char* text, size_t* size) { rt->get_technique_name(handle, text, size); });
        technique.Effect = Text([&](char* text, size_t* size) { rt->get_technique_effect_name(handle, text, size); });
        technique.On = rt->get_technique_state(handle);
        if (!technique.Name.empty())
            techniques.push_back(std::move(technique));
    });

    // Only the effects that are switched on: a ReShade install has hundreds of values in effects nobody is using.
    std::set<std::string> effectsOn;
    for (const LiveTechnique& technique : techniques)
        if (technique.On)
            effectsOn.insert(technique.Effect);

    std::vector<LiveValue> values;
    runtime->enumerate_uniform_variables(nullptr, [&](effect_runtime* rt, effect_uniform_variable handle) {
        if (values.size() >= 400)
            return;
        reshade::api::format type = reshade::api::format::unknown;
        uint32_t rows = 0, columns = 0, length = 0;
        rt->get_uniform_variable_type(handle, &type, &rows, &columns, &length);
        const uint32_t count = std::max(rows, 1u) * std::max(columns, 1u);
        if (length > 1 || count > 4)
            return;
        // Values ReShade fills in itself (the time, the mouse, the depth buffer's size) have a source, and aren't settings.
        char source[64] = "";
        size_t sourceSize = sizeof(source);
        if (rt->get_annotation_string_from_uniform_variable(handle, "source", source, &sourceSize) && source[0])
            return;
        bool hidden = false;
        if (rt->get_annotation_bool_from_uniform_variable(handle, "hidden", &hidden, 1) && hidden)
            return;
        LiveValue value;
        value.Handle = handle;
        value.Effect = Text([&](char* text, size_t* size) { rt->get_uniform_variable_effect_name(handle, text, size); });
        if (!effectsOn.count(value.Effect))
            return;
        value.Name = Text([&](char* text, size_t* size) { rt->get_uniform_variable_name(handle, text, size); });
        value.Label = Text([&](char* text, size_t* size) {
            if (!rt->get_annotation_string_from_uniform_variable(handle, "ui_label", text, size))
                text[0] = 0;
        });
        if (value.Label.empty())
            value.Label = value.Name;
        value.Count = static_cast<int>(count);
        if (type == reshade::api::format::r32_float) {
            value.Kind = Kind::Float;
            rt->get_uniform_value_float(handle, value.Numbers, count);
        } else if (type == reshade::api::format::r32_sint || type == reshade::api::format::r32_uint) {
            value.Kind = Kind::Int;
            int32_t numbers[4] = {};
            rt->get_uniform_value_int(handle, numbers, count);
            for (uint32_t i = 0; i < count; ++i)
                value.Numbers[i] = static_cast<float>(numbers[i]);
        } else {
            value.Kind = Kind::Bool;
            bool numbers[4] = {};
            rt->get_uniform_value_bool(handle, numbers, count);
            for (uint32_t i = 0; i < count; ++i)
                value.Numbers[i] = numbers[i] ? 1.0f : 0.0f;
        }
        const std::string uiType = Text([&](char* text, size_t* size) {
            if (!rt->get_annotation_string_from_uniform_variable(handle, "ui_type", text, size))
                text[0] = 0;
        });
        value.Colour = uiType == "color" && value.Kind == Kind::Float && count >= 3;
        value.Min = 0;
        value.Max = value.Kind == Kind::Int ? 10.0f : 1.0f;
        float limit = 0;
        if (rt->get_annotation_float_from_uniform_variable(handle, "ui_min", &limit, 1))
            value.Min = limit;
        if (rt->get_annotation_float_from_uniform_variable(handle, "ui_max", &limit, 1))
            value.Max = limit;
        if (value.Max <= value.Min)
            value.Max = value.Min + 1;
        if (!value.Name.empty())
            values.push_back(std::move(value));
    });

    std::lock_guard lock(g_mutex);
    g_techniques = std::move(techniques);
    g_values = std::move(values);
}

// Render thread: what the editor or a look key asked for goes to ReShade.
void ApplyChanges(effect_runtime* runtime)
{
    std::vector<Change> changes;
    std::vector<LiveTechnique> techniques;
    std::vector<LiveValue> values;
    {
        std::lock_guard lock(g_mutex);
        changes.swap(g_changes);
        if (changes.empty())
            return;
        techniques = g_techniques;
        values = g_values;
    }
    bool switched = false;
    for (const Change& change : changes) {
        if (change.IsTechnique) {
            for (const LiveTechnique& technique : techniques)
                if (technique.Effect == change.Effect && technique.Name == change.Name) {
                    runtime->set_technique_state(technique.Handle, change.On);
                    switched = true;
                }
            continue;
        }
        for (const LiveValue& value : values) {
            if (value.Effect != change.Effect || value.Name != change.Name)
                continue;
            const size_t count = static_cast<size_t>(value.Count);
            if (value.Kind == Kind::Float) {
                runtime->set_uniform_value_float(value.Handle, change.Numbers, count);
            } else if (value.Kind == Kind::Int) {
                int32_t numbers[4];
                for (int i = 0; i < 4; ++i)
                    numbers[i] = static_cast<int32_t>(std::lround(change.Numbers[i]));
                runtime->set_uniform_value_int(value.Handle, numbers, count);
            } else {
                bool numbers[4];
                for (int i = 0; i < 4; ++i)
                    numbers[i] = change.Numbers[i] >= 0.5f;
                runtime->set_uniform_value_bool(value.Handle, numbers, count);
            }
        }
    }
    // A technique going on or off changes which effects' values are listed.
    if (switched)
        g_stale = true;
    std::lock_guard lock(g_mutex);
    for (const Change& change : changes) {
        if (change.IsTechnique) {
            for (LiveTechnique& technique : g_techniques)
                if (technique.Effect == change.Effect && technique.Name == change.Name)
                    technique.On = change.On;
        } else {
            for (LiveValue& value : g_values)
                if (value.Effect == change.Effect && value.Name == change.Name)
                    std::copy(change.Numbers, change.Numbers + 4, value.Numbers);
        }
    }
}

void OnPresent(effect_runtime* runtime)
{
    if (!runtime || guard::Tripped())
        return;
    // Other graphics APIs never come up in this game, and the overlay only knows this one.
    if (runtime->get_device()->get_api() != reshade::api::device_api::d3d12)
        return;
    ++g_frame;
    g_lastPresent = GetTickCount64();
    if (!g_drawing.exchange(true))
        mod::Log("ReShade: drawing the game's frames, so recording and the editor come after its effects");

    // ReShade's own menu can change anything at any time, so the lists are read again a few times a second.
    if (g_stale.exchange(false) || g_frame % 20 == 0)
        ReadEverything(runtime);
    ApplyChanges(runtime);
    // ReShade's drawing for this frame is still waiting in its command list. It has to reach the GPU before the frame is copied.
    if (reshade::api::command_queue* queue = runtime->get_command_queue())
        queue->flush_immediate_command_list();
    overlay::PresentFromReShade();
}

void OnReloaded(effect_runtime*)
{
    g_stale = true;
}

void OnDestroyed(effect_runtime*)
{
    g_drawing = false;
    std::lock_guard lock(g_mutex);
    g_techniques.clear();
    g_values.clear();
}

HMODULE FindReShade()
{
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return nullptr;
    const DWORD count = std::min<DWORD>(bytes, sizeof(modules)) / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i)
        if (GetProcAddress(modules[i], "ReShadeRegisterAddon") && GetProcAddress(modules[i], "ReShadeRegisterEvent"))
            return modules[i];
    return nullptr;
}

void Register(HMODULE reshadeModule)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Register), &self);
    const auto registerAddon = reinterpret_cast<bool (*)(void*, uint32_t)>(GetProcAddress(reshadeModule, "ReShadeRegisterAddon"));
    const auto registerEvent = reinterpret_cast<void (*)(reshade::addon_event, void*)>(GetProcAddress(reshadeModule, "ReShadeRegisterEvent"));
    uint32_t taken = 0;
    // An older ReShade refuses an add-on made for a newer API, so the newest it will take is found by asking.
    for (uint32_t version = NewestApi; version >= OldestApi && !taken; --version)
        if (registerAddon(self, version))
            taken = version;
    if (!taken) {
        mod::Log("ReShade: found, but it didn't take the add-on at any API version from %u down to %u", NewestApi, OldestApi);
        g_state = State::TooOld;
        return;
    }
    registerEvent(reshade::addon_event::reshade_present, reinterpret_cast<void*>(static_cast<void (*)(effect_runtime*)>(&OnPresent)));
    registerEvent(reshade::addon_event::reshade_reloaded_effects, reinterpret_cast<void*>(static_cast<void (*)(effect_runtime*)>(&OnReloaded)));
    registerEvent(reshade::addon_event::destroy_effect_runtime, reinterpret_cast<void*>(static_cast<void (*)(effect_runtime*)>(&OnDestroyed)));
    mod::Log("ReShade: registered as an add-on, API version %u", taken);
    g_state = State::Ready;
}

// "reshade.t.<effect file>.<technique>" and "reshade.u.<effect file>.<value>.<index>". Effect files have dots in their names, the rest don't.
bool ParseKey(const std::string& key, bool& technique, std::string& effect, std::string& name, int& index)
{
    technique = key.rfind("reshade.t.", 0) == 0;
    if (!technique && key.rfind("reshade.u.", 0) != 0)
        return false;
    std::string rest = key.substr(10);
    index = 0;
    if (!technique) {
        const size_t dot = rest.rfind('.');
        if (dot == std::string::npos)
            return false;
        index = std::atoi(rest.c_str() + dot + 1);
        rest.erase(dot);
    }
    const size_t dot = rest.rfind('.');
    if (dot == std::string::npos || index < 0 || index > 3)
        return false;
    effect = rest.substr(0, dot);
    name = rest.substr(dot + 1);
    return !effect.empty() && !name.empty();
}

} // namespace

bool Found()
{
    return g_state == State::Ready;
}

std::string StatusText()
{
    switch (g_state.load()) {
    case State::Ready:
        return g_drawing ? "ReShade is on. Its effects are in the picture and in recordings." : "ReShade is there. Waiting for its first frame.";
    case State::TooOld:
        return "ReShade is there, but it didn't take tekken8mvm as an add-on. Update ReShade, or use its build with full add-on support.";
    case State::NotThere:
        return "ReShade isn't in the game. Install it into the game's Win64 folder, for DirectX 12, and it shows up here.";
    default:
        return "Looking for ReShade...";
    }
}

std::vector<Technique> Techniques()
{
    std::lock_guard lock(g_mutex);
    return std::vector<Technique>(g_techniques.begin(), g_techniques.end());
}

std::vector<Value> Values()
{
    std::lock_guard lock(g_mutex);
    return std::vector<Value>(g_values.begin(), g_values.end());
}

void SetTechnique(const std::string& effect, const std::string& name, bool on)
{
    Change change;
    change.IsTechnique = true;
    change.Effect = effect;
    change.Name = name;
    change.On = on;
    std::lock_guard lock(g_mutex);
    g_changes.push_back(std::move(change));
}

void SetValue(const std::string& effect, const std::string& name, const float numbers[4])
{
    Change change;
    change.Effect = effect;
    change.Name = name;
    std::copy(numbers, numbers + 4, change.Numbers);
    std::lock_guard lock(g_mutex);
    g_changes.push_back(std::move(change));
}

void WriteLook(Ini& ini)
{
    if (!Found())
        return;
    std::lock_guard lock(g_mutex);
    for (const LiveTechnique& technique : g_techniques)
        ini.SetBool("reshade.t." + technique.Effect + "." + technique.Name, technique.On);
    for (const LiveValue& value : g_values)
        for (int i = 0; i < value.Count; ++i) {
            char number[32];
            snprintf(number, sizeof(number), "%.6g", value.Numbers[i]);
            ini.Set("reshade.u." + value.Effect + "." + value.Name + "." + std::to_string(i), number);
        }
}

void ReadLook(const Ini& ini)
{
    if (!Found())
        return;
    std::lock_guard lock(g_mutex);
    // Whatever the look doesn't mention stays as it is.
    for (const LiveTechnique& technique : g_techniques) {
        const std::string key = "reshade.t." + technique.Effect + "." + technique.Name;
        if (!ini.Values().count(key))
            continue;
        const bool on = ini.GetBool(key, technique.On);
        if (on == technique.On)
            continue;
        Change change;
        change.IsTechnique = true;
        change.Effect = technique.Effect;
        change.Name = technique.Name;
        change.On = on;
        g_changes.push_back(std::move(change));
    }
    for (const LiveValue& value : g_values) {
        Change change;
        change.Effect = value.Effect;
        change.Name = value.Name;
        std::copy(value.Numbers, value.Numbers + 4, change.Numbers);
        bool different = false;
        for (int i = 0; i < value.Count; ++i) {
            const std::string key = "reshade.u." + value.Effect + "." + value.Name + "." + std::to_string(i);
            if (!ini.Values().count(key))
                continue;
            const float wanted = static_cast<float>(ini.GetNumber(key, value.Numbers[i]));
            different = different || std::fabs(wanted - value.Numbers[i]) > 1e-6f;
            change.Numbers[i] = wanted;
        }
        if (different)
            g_changes.push_back(std::move(change));
    }
}

bool MovesSmoothly(const std::string& key)
{
    bool technique = false;
    std::string effect, name;
    int index = 0;
    if (!ParseKey(key, technique, effect, name, index) || technique)
        return false;
    std::lock_guard lock(g_mutex);
    for (const LiveValue& value : g_values)
        if (value.Effect == effect && value.Name == name)
            return value.Kind != Kind::Bool;
    return true;
}

bool OwnsPresent()
{
    // If ReShade stops showing frames (its runtime went away with the swap chain), the overlay takes its own turn again.
    return g_drawing && GetTickCount64() - g_lastPresent.load() < 500;
}

void OnGameTick()
{
    if (g_state != State::Looking)
        return;
    static int wait = 0, tries = 0;
    if (--wait > 0)
        return;
    wait = 120;
    if (HMODULE module = FindReShade()) {
        Register(module);
        return;
    }
    // ReShade loads with the game's graphics, before the first fight. After that it isn't coming.
    if (++tries >= 15) {
        g_state = State::NotThere;
        mod::Log("ReShade: not in the game");
    }
}

} // namespace reshadelink
