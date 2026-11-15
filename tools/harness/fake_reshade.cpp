// A stand-in for ReShade, for testing the mod's ReShade link with no ReShade and no game.
//
// It has the exports the mod looks for, takes the mod as an add-on, and hands it an effect runtime with two techniques and a few
// values. The runtime is built on ReShade's own headers, so its methods are in the places the mod's calls expect. The harness calls
// FakeReShadePresent once a frame, where the real ReShade would say its effects are done.
//
// It refuses an add-on made for an API newer than 18, the way an older ReShade would, so the mod's stepping down to a version it
// takes gets tested too.
#include <windows.h>

#include <cstring>

#include <reshade_events.hpp>

namespace fake {

using namespace reshade::api;

#include "fake_reshade_stubs.inc"

struct Technique {
    const char* Effect;
    const char* Name;
    bool On;
};
Technique g_techniques[] = { { "Bloom.fx", "Bloom", true }, { "Tint.fx", "Tint", false } };

struct Uniform {
    const char* Effect;
    const char* Name;
    const char* Label;
    format Type;
    uint32_t Count;
    float Value[4];
    float Min, Max;
    const char* Source;
    const char* UiType;
};
Uniform g_uniforms[] = {
    { "Bloom.fx", "Strength", "Bloom strength", format::r32_float, 1, { 0.5f }, 0, 2, nullptr, "slider" },
    { "Bloom.fx", "Colour", "Bloom colour", format::r32_float, 3, { 1.0f, 0.5f, 0.25f }, 0, 1, nullptr, "color" },
    { "Bloom.fx", "Passes", "Passes", format::r32_sint, 1, { 3 }, 1, 8, nullptr, "slider" },
    { "Bloom.fx", "Soft", "Soft", format::r32_typeless, 1, { 1 }, 0, 1, nullptr, "" },
    { "Bloom.fx", "Timer", "", format::r32_float, 1, { 0 }, 0, 1, "timer", "" }, // filled in by ReShade itself: not a setting
    { "Tint.fx", "Amount", "Tint amount", format::r32_float, 1, { 0.2f }, 0, 1, nullptr, "slider" },
};

constexpr size_t TechniqueCount = sizeof(g_techniques) / sizeof(g_techniques[0]);
constexpr size_t UniformCount = sizeof(g_uniforms) / sizeof(g_uniforms[0]);
int g_flushes = 0;

Technique* Find(effect_technique handle)
{
    return handle.handle >= 1 && handle.handle <= TechniqueCount ? &g_techniques[handle.handle - 1] : nullptr;
}

Uniform* Find(effect_uniform_variable handle)
{
    return handle.handle >= 1 && handle.handle <= UniformCount ? &g_uniforms[handle.handle - 1] : nullptr;
}

// ReShade's strings: the buffer is filled as far as it goes, and the size comes back as the whole length with its ending zero.
void GiveText(const char* text, char* out, size_t* size)
{
    const size_t length = std::strlen(text) + 1;
    if (out && size && *size > 0) {
        strncpy_s(out, *size, text, _TRUNCATE);
    }
    if (size)
        *size = length;
}

struct Device : StubDevice {
    device_api get_api() const override { return device_api::d3d12; }
} g_device;

struct Queue : StubQueue {
    device* get_device() override { return &g_device; }
    void flush_immediate_command_list() const override { ++g_flushes; }
} g_queue;

struct Runtime : StubRuntime {
    device* get_device() override { return &g_device; }
    command_queue* get_command_queue() override { return &g_queue; }

    void enumerate_techniques(const char*, void (*callback)(effect_runtime* runtime, effect_technique technique, void* user_data), void* user_data) override
    {
        for (uint64_t i = 1; i <= TechniqueCount; ++i)
            callback(this, { i }, user_data);
    }
    void get_technique_name(effect_technique technique, char* name, size_t* name_size) const override
    {
        if (const Technique* t = Find(technique))
            GiveText(t->Name, name, name_size);
    }
    void get_technique_effect_name(effect_technique technique, char* effect_name, size_t* effect_name_size) const override
    {
        if (const Technique* t = Find(technique))
            GiveText(t->Effect, effect_name, effect_name_size);
    }
    bool get_technique_state(effect_technique technique) const override
    {
        const Technique* t = Find(technique);
        return t && t->On;
    }
    void set_technique_state(effect_technique technique, bool enabled) override
    {
        if (Technique* t = Find(technique))
            t->On = enabled;
    }

    void enumerate_uniform_variables(const char*, void (*callback)(effect_runtime* runtime, effect_uniform_variable variable, void* user_data), void* user_data) override
    {
        for (uint64_t i = 1; i <= UniformCount; ++i)
            callback(this, { i }, user_data);
    }
    void get_uniform_variable_type(effect_uniform_variable variable, format* out_base_type, uint32_t* out_rows, uint32_t* out_columns, uint32_t* out_array_length) const override
    {
        const Uniform* u = Find(variable);
        if (!u)
            return;
        if (out_base_type)
            *out_base_type = u->Type;
        if (out_rows)
            *out_rows = u->Count;
        if (out_columns)
            *out_columns = 1;
        if (out_array_length)
            *out_array_length = 0;
    }
    void get_uniform_variable_name(effect_uniform_variable variable, char* name, size_t* name_size) const override
    {
        if (const Uniform* u = Find(variable))
            GiveText(u->Name, name, name_size);
    }
    void get_uniform_variable_effect_name(effect_uniform_variable variable, char* effect_name, size_t* effect_name_size) const override
    {
        if (const Uniform* u = Find(variable))
            GiveText(u->Effect, effect_name, effect_name_size);
    }
    bool get_annotation_string_from_uniform_variable(effect_uniform_variable variable, const char* name, char* value, size_t* value_size) const override
    {
        const Uniform* u = Find(variable);
        if (!u)
            return false;
        const char* text = !std::strcmp(name, "source") ? u->Source : !std::strcmp(name, "ui_label") ? u->Label : !std::strcmp(name, "ui_type") ? u->UiType : nullptr;
        if (!text || !text[0])
            return false;
        GiveText(text, value, value_size);
        return true;
    }
    bool get_annotation_float_from_uniform_variable(effect_uniform_variable variable, const char* name, float* values, size_t count, size_t) const override
    {
        const Uniform* u = Find(variable);
        if (!u || count < 1)
            return false;
        if (!std::strcmp(name, "ui_min"))
            values[0] = u->Min;
        else if (!std::strcmp(name, "ui_max"))
            values[0] = u->Max;
        else
            return false;
        return true;
    }
    bool get_annotation_bool_from_uniform_variable(effect_uniform_variable, const char*, bool*, size_t, size_t) const override { return false; }

    void get_uniform_value_float(effect_uniform_variable variable, float* values, size_t count, size_t) const override
    {
        if (const Uniform* u = Find(variable))
            for (size_t i = 0; i < count && i < 4; ++i)
                values[i] = u->Value[i];
    }
    void get_uniform_value_int(effect_uniform_variable variable, int32_t* values, size_t count, size_t) const override
    {
        if (const Uniform* u = Find(variable))
            for (size_t i = 0; i < count && i < 4; ++i)
                values[i] = static_cast<int32_t>(u->Value[i]);
    }
    void get_uniform_value_bool(effect_uniform_variable variable, bool* values, size_t count, size_t) const override
    {
        if (const Uniform* u = Find(variable))
            for (size_t i = 0; i < count && i < 4; ++i)
                values[i] = u->Value[i] != 0;
    }
    void set_uniform_value_float(effect_uniform_variable variable, const float* values, size_t count, size_t) override
    {
        if (Uniform* u = Find(variable))
            for (size_t i = 0; i < count && i < 4; ++i)
                u->Value[i] = values[i];
    }
    void set_uniform_value_int(effect_uniform_variable variable, const int32_t* values, size_t count, size_t) override
    {
        if (Uniform* u = Find(variable))
            for (size_t i = 0; i < count && i < 4; ++i)
                u->Value[i] = static_cast<float>(values[i]);
    }
    void set_uniform_value_bool(effect_uniform_variable variable, const bool* values, size_t count, size_t) override
    {
        if (Uniform* u = Find(variable))
            for (size_t i = 0; i < count && i < 4; ++i)
                u->Value[i] = values[i] ? 1.0f : 0.0f;
    }
} g_runtime;

using RuntimeEvent = void (*)(effect_runtime*);
RuntimeEvent g_present = nullptr, g_reloaded = nullptr, g_destroyed = nullptr;
int g_refused = 0;
uint32_t g_taken = 0;

} // namespace fake

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(void*, uint32_t api_version)
{
    if (api_version > 18) {
        ++fake::g_refused;
        return false;
    }
    fake::g_taken = api_version;
    return true;
}

extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(void*)
{
}

extern "C" __declspec(dllexport) void ReShadeRegisterEvent(reshade::addon_event ev, void* callback)
{
    const auto function = reinterpret_cast<fake::RuntimeEvent>(callback);
    if (ev == reshade::addon_event::reshade_present)
        fake::g_present = function;
    else if (ev == reshade::addon_event::reshade_reloaded_effects)
        fake::g_reloaded = function;
    else if (ev == reshade::addon_event::destroy_effect_runtime)
        fake::g_destroyed = function;
}

// The harness calls this once a frame, just before it presents.
extern "C" __declspec(dllexport) void FakeReShadePresent()
{
    if (fake::g_present)
        fake::g_present(&fake::g_runtime);
}

// For the mod's own checks: what the stand-in holds now.
extern "C" __declspec(dllexport) float FakeReShadeValue(int uniform, int component)
{
    return uniform >= 0 && uniform < static_cast<int>(fake::UniformCount) && component >= 0 && component < 4 ? fake::g_uniforms[uniform].Value[component] : -1.0f;
}

extern "C" __declspec(dllexport) int FakeReShadeTechnique(int technique)
{
    return technique >= 0 && technique < static_cast<int>(fake::TechniqueCount) ? fake::g_techniques[technique].On : -1;
}

// 0 flushes, 1 the API version it took, 2 how many newer versions it refused first.
extern "C" __declspec(dllexport) int FakeReShadeCount(int what)
{
    return what == 0 ? fake::g_flushes : (what == 1 ? static_cast<int>(fake::g_taken) : fake::g_refused);
}
