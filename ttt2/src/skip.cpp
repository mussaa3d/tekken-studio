// Holding a replay still. BlueSpark's hold is refused while one of its replays plays, but its replay is keyed to the fight's steps and
// waits when the fight doesn't step (as under the game's own pause). So the studio leaves out one of the game's own frame functions,
// above the fight's update, for the frames it wants held.
//
// The game's call stack at the camera write (release 23, measured), innermost first; * is a function BlueSpark's library replaces:
//   82541878 camera write < 82545EF8 < 82547DE0 < 82542338 < 822D36C8* < 825DEBD0* < 825DCE80 < 8262E418 < 825E04F0 < 825E0AE0
//   < 825EEBD0* < 825EF0B8* < 82583288 < 825834A8 < 823359F8 < the thread's start
// 825DCE80, 8262E418, 825E04F0 and 825E0AE0 each run once a frame (126 calls in 2 s). Leaving any of them out stopped the replay's
// frame count and the camera writes for as long as it was left out, and the replay went on at 60 a second from the same frame
// afterwards, still a replay. With the outer two (825E04F0, 825E0AE0) the fighters weren't drawn while held; with the inner two the
// picture stayed whole. So it's 825DCE80.
#include "studio.h"

#include <atomic>

#include <rex/ppc/context.h>

namespace studio {

namespace {
std::atomic<uint64_t> g_frames { 0 }, g_leftOut { 0 };
std::atomic<int> g_test { 0 }; // research: 1 leaves every frame out, whatever is playing
} // namespace

void SkipTest(int, bool hold)
{
    g_test = hold ? 1 : 0;
}

std::string SkipCounts()
{
    return " " + std::to_string(g_frames.load()) + "/" + std::to_string(g_leftOut.load());
}

} // namespace studio

extern "C" void __imp__sub_825DCE80(PPCContext& ctx, uint8_t* base);
extern "C" void sub_825DCE80(PPCContext& ctx, uint8_t* base)
{
    ++studio::g_frames;
    if (studio::g_test.load(std::memory_order_relaxed) || !studio::ReplayFrameAllowed()) {
        ++studio::g_leftOut;
        return;
    }
    __imp__sub_825DCE80(ctx, base);
}
