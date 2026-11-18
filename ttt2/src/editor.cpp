// The studio's editor: an ImGui dialog on the game's own drawer. Insert opens and closes it.
#include "studio.h"

#include <windows.h>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

namespace studio {

namespace {

bool g_open = false;

class Editor : public rex::ui::ImGuiDialog {
public:
    explicit Editor(rex::ui::ImGuiDrawer* drawer)
        : rex::ui::ImGuiDialog(drawer)
    {
    }

protected:
    void OnDraw(ImGuiIO& io) override
    {
        // Insert, read straight from the keyboard: the dialog gets no key events while the game has the input.
        static bool wasDown = false;
        const bool down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0 && GetForegroundWindow() == GetActiveWindow();
        if (down && !wasDown)
            g_open = !g_open;
        wasDown = down;

        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const char* label = "Tekken Studio v0.1 beta";
        draw->AddText({ io.DisplaySize.x - ImGui::CalcTextSize(label).x - 12, 8 }, IM_COL32(255, 255, 255, 160), label);
        if (!g_open)
            return;
        ImGui::SetNextWindowSize({ 420, 240 }, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Tekken Studio", &g_open)) {
            ImGui::TextUnformatted("The studio is linked into the game.");
            ImGui::Text("Picture %.0f x %.0f, %.1f fps", io.DisplaySize.x, io.DisplaySize.y, io.Framerate);
        }
        ImGui::End();
    }
};

} // namespace

void CreateEditor(rex::ui::ImGuiDrawer* drawer)
{
    new Editor(drawer); // the drawer keeps it for the life of the game
}

} // namespace studio
