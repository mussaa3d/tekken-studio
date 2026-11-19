// The game's HUD, hidden whole or in parts. The game asks "is the mode 7" (sub_825B7760: the mode object's +448 against 7) from 80
// places, and the HUD's own code leaves a part undrawn when the answer is yes (measured: answering yes everywhere took the health
// bars, the timer, the portraits and the names off, with the fight going on). So the studio answers yes only to the callers it has
// been told to, each found by the return address: nothing but those parts of the HUD hears it.
#include "studio.h"

#include <atomic>

#include <rex/ppc/context.h>

namespace studio {

namespace {
struct Caller {
    uint32_t Start, End;
};
// Every function that calls sub_825B7760 (release 23), by address.
constexpr Caller kCallers[] = {
    { 0x822592B8, 0x82259340 },
    { 0x82261600, 0x82261C60 },
    { 0x822BC9F0, 0x822BD038 },
    { 0x822C67A8, 0x822C7728 },
    { 0x822C95C8, 0x822C9BB8 },
    { 0x822CC388, 0x822CC4A0 },
    { 0x822CD798, 0x822CDA20 },
    { 0x822CDD98, 0x822D0D48 },
    { 0x822D0D48, 0x822D3580 },
    { 0x822D6630, 0x822D66F0 },
    { 0x822E2110, 0x822E2590 },
    { 0x822E75F0, 0x822E80D0 },
    { 0x822EB118, 0x822F1538 },
    { 0x822FE170, 0x822FE240 },
    { 0x823038E0, 0x82303BB0 },
    { 0x82303BB0, 0x82304120 },
    { 0x8230DAB8, 0x82313D38 },
    { 0x82315D58, 0x82315FD0 },
    { 0x82320C88, 0x82323CF8 },
    { 0x82323CF8, 0x82324538 },
    { 0x82331238, 0x823312C0 },
    { 0x82332158, 0x823328C8 },
    { 0x8235E598, 0x8235E630 },
    { 0x8235F448, 0x8235F598 },
    { 0x82367BB0, 0x82367CA0 },
    { 0x82367CA0, 0x82367DC8 },
    { 0x82368D80, 0x82368DD8 },
    { 0x82368DD8, 0x82368F40 },
    { 0x823694C8, 0x823696F8 },
    { 0x8236A168, 0x8236A2F0 },
    { 0x8236AA68, 0x8236AB70 },
    { 0x8236AB70, 0x8236AC78 },
    { 0x8236AC78, 0x8236ADB0 },
    { 0x8236B9C8, 0x8236BB78 },
    { 0x8236BB78, 0x8236C310 },
    { 0x8236C6F8, 0x8236CA80 },
    { 0x8236CB30, 0x8236D1F8 },
    { 0x823773D0, 0x82377A28 },
    { 0x82395C48, 0x82395CA0 },
    { 0x82395CA0, 0x82395D50 },
    { 0x82395D50, 0x82395ED8 },
    { 0x823D1998, 0x823D1C30 },
    { 0x823D4058, 0x823D4690 },
    { 0x823D46B0, 0x823D47D0 },
    { 0x823D5038, 0x823D5150 },
    { 0x823D53C0, 0x823D55A8 },
    { 0x823D55A8, 0x823D56D0 },
    { 0x823D56D0, 0x823D5860 },
    { 0x823D6738, 0x823D6980 },
    { 0x823D6A88, 0x823D6C80 },
    { 0x823D8438, 0x823D8830 },
    { 0x823F8198, 0x823F8228 },
    { 0x823FB298, 0x823FB3E8 },
    { 0x823FCFF0, 0x823FD188 },
    { 0x823FD1A0, 0x823FD6E0 },
    { 0x82488630, 0x82488750 },
    { 0x824CCB08, 0x824CCCA0 },
    { 0x825010A0, 0x825013B8 },
    { 0x8250CCE8, 0x8250D278 },
    { 0x82544F60, 0x82545170 },
    { 0x8254B900, 0x8254B958 },
    { 0x8254E260, 0x8254E7B0 },
    { 0x8254E7B0, 0x8254E9D8 },
    { 0x8254EB50, 0x8254ED70 },
    { 0x8254EE98, 0x8254F058 },
    { 0x82550B00, 0x82550C58 },
    { 0x825D44A8, 0x825D4580 },
    { 0x825DB680, 0x825DB9C0 },
    { 0x825DBF68, 0x825DC118 },
    { 0x825DD1A0, 0x825DD4C0 },
    { 0x825DE588, 0x825DEA28 },
    { 0x825E04F0, 0x825E0A08 },
    { 0x825E0B50, 0x825E0DF0 },
    { 0x825E0DF0, 0x825E1168 },
    { 0x825E1168, 0x825E16A0 },
    { 0x825E16A0, 0x825E19B8 },
    { 0x825E1EF8, 0x825E23D8 },
    { 0x825E2760, 0x825E2910 },
    { 0x825E3230, 0x825E32E0 },
    { 0x825E32E0, 0x825E3718 }
};
constexpr int CallerCount = static_cast<int>(sizeof(kCallers) / sizeof(kCallers[0]));
std::atomic<uint64_t> g_hidden[2] {}; // one bit a caller: answer yes to it
std::atomic<uint64_t> g_seen[2] {};   // research: the callers that have asked since the last look
} // namespace

int HudCallerCount()
{
    return CallerCount;
}

void HudHideCaller(int index, bool hide)
{
    if (index < 0 || index >= CallerCount)
        return;
    if (hide)
        g_hidden[index / 64] |= 1ull << (index % 64);
    else
        g_hidden[index / 64] &= ~(1ull << (index % 64));
}

void HudHideNone()
{
    g_hidden[0] = g_hidden[1] = 0;
}

// The callers that are the HUD's own (release 23, each tried on its own in a replay): 42 the health bars with their round marks, 45 the
// timer, 46 the portraits, 47 the name tags; 32, 38, 40, 41, 44, 48 and 49 the rest (the lightning behind the portraits among it).
// The other callers are the game's own logic and are never answered.
namespace {
constexpr int kHudCallers[] = { 32, 38, 40, 41, 42, 44, 45, 46, 47, 48, 49 };
constexpr int kBars = 42, kTimer = 45, kPortraits = 46;
int g_preset = 0;
} // namespace

const char* HudPresetName(int preset)
{
    static const char* const names[HudPresetCount] = { "The game's own", "Clean: health bars, timer and portraits", "Health bars and timer", "Health bars only",
                                                       "Timer only", "No HUD" };
    return preset >= 0 && preset < HudPresetCount ? names[preset] : "";
}

int& HudPreset()
{
    return g_preset;
}

void HudApplyPreset(int preset)
{
    if (preset < 0 || preset >= HudPresetCount)
        return;
    g_preset = preset;
    HudHideNone();
    if (preset == 0)
        return;
    for (const int caller : kHudCallers) {
        const bool keep = (caller == kBars && (preset == 1 || preset == 2 || preset == 3)) || (caller == kTimer && (preset == 1 || preset == 2 || preset == 4))
                          || (caller == kPortraits && preset == 1);
        HudHideCaller(caller, !keep);
    }
}

std::string HudCallersSeen()
{
    std::string text;
    for (int i = 0; i < CallerCount; ++i)
        if (g_seen[i / 64].load() >> (i % 64) & 1) {
            char one[24];
            snprintf(one, sizeof(one), " %d:%08X", i, kCallers[i].Start);
            text += one;
        }
    g_seen[0] = g_seen[1] = 0;
    return text;
}

} // namespace studio

extern "C" void __imp__sub_825B7760(PPCContext& ctx, uint8_t* base);
extern "C" void sub_825B7760(PPCContext& ctx, uint8_t* base)
{
    const uint32_t from = static_cast<uint32_t>(ctx.lr);
    __imp__sub_825B7760(ctx, base);
    for (int i = 0; i < studio::CallerCount; ++i) {
        if (from < studio::kCallers[i].Start || from >= studio::kCallers[i].End)
            continue;
        studio::g_seen[i / 64] |= 1ull << (i % 64);
        if (studio::g_hidden[i / 64].load(std::memory_order_relaxed) >> (i % 64) & 1)
            ctx.r3.u64 = 1;
        break;
    }
}
