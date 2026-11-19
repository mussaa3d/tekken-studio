// BlueSpark's own overlay (its button hints along the bottom, its SELECT pill, its pages) and the studio. BlueSpark draws it with a
// drawer of its own that the presenter runs after the game's picture and after every ImGui dialog, so it lands on top of the
// studio's editor. While the editor is open over a fight, the studio takes that drawer off the presenter, and puts the same one back
// when the editor closes. Nothing of BlueSpark's is changed: it's the presenter's own setter, with the drawer BlueSpark gave it.
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The presenter keeps that drawer in a private member and only has a setter for it.
#define private public
#define protected public
#include <rex/ui/presenter.h>
#include <rex/ui/window.h> // its presenter() is protected
#undef private
#undef protected

#include <rex/runtime.h>

#include "studio.h"

namespace studio {

namespace {
rex::ui::UIDrawer* g_theirs = nullptr;
bool g_hidden = false;
} // namespace

void HideBlueSparkOverlay(bool hide)
{
    if (hide == g_hidden)
        return;
    rex::Runtime* runtime = rex::Runtime::instance();
    rex::ui::Window* window = runtime ? runtime->display_window() : nullptr;
    rex::ui::Presenter* presenter = window ? window->presenter() : nullptr;
    if (!presenter)
        return;
    if (hide) {
        rex::ui::UIDrawer* now = nullptr;
        {
            std::lock_guard<std::mutex> lock(presenter->guest_output_thread_drawer_mutex_);
            now = presenter->guest_output_thread_drawer_;
        }
        if (!now) {
            static bool said = false;
            if (!said)
                Log("BlueSpark's overlay isn't on the presenter's own drawer slot: it stays where it is");
            said = true;
            return;
        }
        g_theirs = now;
        presenter->SetGuestOutputThreadDrawer(nullptr);
        g_hidden = true;
        Log("BlueSpark's overlay taken off while the editor is open");
    } else {
        presenter->SetGuestOutputThreadDrawer(g_theirs);
        g_hidden = false;
        Log("BlueSpark's overlay back");
    }
}

} // namespace studio
