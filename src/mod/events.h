#pragma once
// One hook on UObject::ProcessEvent, shared by whoever wants to see the game's engine events: native code calling a Blueprint
// event (a widget being told to show a row, a box being told which button is chosen) goes through it with its parameters.
// Blueprint calls into native code don't, so this sees what the game tells its screens, not what the screens ask of the game.

namespace events {

// Called on the game thread, before the event runs, with the event's parameters as the caller filled them in.
using Listener = void (*)(void* object, void* function, void* params);

// Installs the hook the first time. False if it couldn't be installed or there's no room for another listener.
bool Listen(Listener listener);

} // namespace events
