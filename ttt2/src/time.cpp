// Time control. BlueSpark has "the outside hold": anyone can ask for an offline fight (Practice, a replay) to be frozen, sound and
// all, and the fight carries on from the same state when the hold is let go, which is what its own pause and rewind are built on.
// The studio holds the fight to pause it, lets it run one picture in every few for slow motion, and lets one frame through for a
// step. Nothing in the fight's state is written, so a replay stays in step.
#include "studio.h"

#include "bluespark.h"
#include "camera.h"

#include <atomic>
#include <cmath>

namespace studio {

namespace {
TimeControl g_time;
std::atomic<int> g_steps { 0 };   // frames asked for with Step, not yet handed to the budget
double g_budget = 0;              // fight frames the fight may still run before it's held again
bool g_holdAsked = false;
uint64_t g_lastFrame = 0;         // BlueSpark's frame count at the last look
uint64_t g_lastWrites = 0;        // the game's camera writes at the last look
double g_writesPerFrame = 14.0;   // how many times the game writes its camera for one fight frame; measured while it runs freely
double g_ownFrames = 0;           // fight frames that have run, for the clock outside a replay
constexpr const char* Who = "tekken studio";

// BlueSpark keeps two kinds of hold: one for Practice, which is also where a replay plays, and one for any other offline fight (Arcade,
// Versus). Each is dropped by BlueSpark itself when its kind of fight isn't up, so the studio asks for both, and asks again when a hold
// it wants has been dropped.
void Ask(bool hold)
{
    static int sinceAsked = 0;
    const bool dropped = hold && g_holdAsked && ++sinceAsked > 20 && GuestBase() && !Ttt2TrainingHoldHolding(GuestBase());
    if (hold == g_holdAsked && !dropped)
        return;
    sinceAsked = 0;
    g_holdAsked = hold;
    Ttt2TrainingHoldRequest(Who, hold);
    Ttt2TrainingHoldRequestOffline(Who, hold);
}
} // namespace

TimeControl& Time()
{
    return g_time;
}

void StepOneFrame()
{
    g_time.Paused = true;
    ++g_steps;
}

bool CanHold()
{
    // (The camera's record first: before the game has set itself up, BlueSpark's own question about the fight reads memory that isn't there yet.)
    return camera::Record() != 0 && Ttt2BlueSparkOn() && Ttt2FightInPlay();
}

std::atomic<bool> g_replayHeld { false }; // the last replay frame was left out

bool Holding()
{
    if (Ttt2ReplayPlaying())
        return g_replayHeld.load(std::memory_order_relaxed);
    uint8_t* base = GuestBase();
    return g_holdAsked && base && Ttt2TrainingHoldHolding(base);
}

bool ReplayPlaying()
{
    return Ttt2ReplayPlaying();
}

double Clock()
{
    if (Ttt2ReplayPlaying())
        return static_cast<double>(Ttt2ReplayFrameNow()) / 60.0;
    return g_ownFrames / 60.0;
}

namespace {
// Whether the studio's camera is somewhere else than at the last look. The game makes the picture's view from its camera only on a
// frame the fight runs, so a pause can't show a camera move: the fight is let creep at 0.05x for as long as the camera is moving.
bool CameraMoved()
{
    static camera::View last;
    camera::View now;
    if (!camera::Wanted(now))
        return false;
    const bool moved = std::fabs(now.Eye.X - last.Eye.X) + std::fabs(now.Eye.Y - last.Eye.Y) + std::fabs(now.Eye.Z - last.Eye.Z) > 0.5f
                       || std::fabs(now.Target.X - last.Target.X) + std::fabs(now.Target.Y - last.Target.Y) + std::fabs(now.Target.Z - last.Target.Z) > 0.5f
                       || std::fabs(now.Fov - last.Fov) > 0.01f;
    last = now;
    return moved;
}
} // namespace

bool ReplayFrameAllowed()
{
    static double share = 0;   // slow motion: the part of a frame owed so far
    static int sinceRun = 100; // frames since one last ran, to space out the frames a camera move asks for
    static bool owed = false;  // paused, and the camera has moved since the picture was last made
    // Only with the replay's fight up: its loading and its covers run on these frames too, and must never be held.
    if (!Ttt2ReplayPlaying() || camera::Record() == 0 || !Ttt2FightInPlay()) {
        share = 0;
        owed = false;
        g_replayHeld = false;
        return true;
    }
    bool run;
    if (g_time.Paused) {
        // A step runs one frame. A camera move runs one too (the picture is only made on a frame that runs), at most one in five,
        // and one more once the camera has stopped, so the picture ends on its last place.
        if (CameraMoved())
            owed = true;
        int steps = g_steps.load();
        if (steps > 0 && g_steps.compare_exchange_strong(steps, steps - 1)) {
            run = true;
        } else if (owed && sinceRun >= 5) {
            run = true;
            owed = false;
        } else {
            run = false;
        }
        share = 0;
    } else if (g_time.Speed >= 0.999) {
        run = true;
        share = 0;
    } else {
        share += std::fmax(g_time.Speed, 0.05);
        run = share >= 1.0;
        if (run)
            share -= 1.0;
    }
    sinceRun = run ? 0 : sinceRun + 1;
    g_replayHeld = !run;
    return run;
}

void TimeTick()
{
    // (A replay runs outside BlueSpark's hold, measured: skip.cpp holds it. A recording paces the game itself, frame by frame.)
    if (!CanHold() || Ttt2ReplayPlaying() || RecorderActive()) {
        Ask(false);
        g_lastFrame = 0;
        return;
    }
    // BlueSpark's frame count goes on while a fight is held, so it tells how much real time (in game frames) has gone by. The game's
    // own camera writes tell how much of the fight has really run.
    const uint64_t frame = Ttt2TestFrame(), writes = camera::Writes();
    if (g_lastFrame == 0 || frame < g_lastFrame) {
        g_lastFrame = frame;
        g_lastWrites = writes;
    }
    const double elapsed = static_cast<double>(frame - g_lastFrame);
    const double ran = static_cast<double>(writes - g_lastWrites) / g_writesPerFrame;
    g_lastFrame = frame;
    g_lastWrites = writes;
    g_ownFrames += ran;

    const bool full = !g_time.Paused && g_time.Speed >= 0.999;
    if (full) {
        // Running freely: nothing held, and the yardstick is measured for when it's needed.
        if (elapsed >= 1 && ran > 0) {
            const double measured = ran * g_writesPerFrame / elapsed;
            if (measured >= 1 && measured <= 64)
                g_writesPerFrame += (measured - g_writesPerFrame) * 0.05;
        }
        g_budget = 0;
        Ask(false);
        return;
    }
    // Paused or slow: the fight gets a budget of frames. Time going by adds to it at the chosen speed and each Step adds one; every
    // frame the fight really runs comes off it. BlueSpark's hold takes a frame or two to let go and to take hold again, so the fight
    // can overrun a little: the budget then goes below zero and it stays held until that's paid back, which keeps the speed right on
    // average.
    if (!g_time.Paused)
        g_budget += elapsed * std::fmax(g_time.Speed, 0.05);
    else {
        // Paused, and the studio's camera moved: the picture only follows it on a frame the fight runs. While it moves the fight
        // runs one frame in five, and once it has been still for four frames one more frame runs, so the picture ends on the
        // camera's last place.
        static bool owed = false;
        static double still = 0;
        if (CameraMoved()) {
            owed = true;
            still = 0;
            g_budget += elapsed * 0.2;
        } else if (owed && (still += elapsed) >= 4) {
            owed = false;
            g_budget = std::fmax(g_budget, 1.0);
        }
    }
    g_budget += g_steps.exchange(0);
    g_budget -= ran;
    g_budget = std::fmax(-8.0, std::fmin(g_budget, 4.0));
    Ask(g_budget < 1.0);
}

} // namespace studio
