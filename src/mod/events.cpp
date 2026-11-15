#include "mod/events.h"

#include <MinHook.h>

#include <atomic>
#include <mutex>

#include "mod/log.h"
#include "mod/ue.h"

namespace events {

namespace {

using ProcessEventFn = void (*)(void*, void*, void*);
ProcessEventFn oProcessEvent = nullptr;

constexpr int MaxListeners = 4;
std::atomic<Listener> g_listeners[MaxListeners] = {};

void DetourProcessEvent(void* object, void* function, void* params)
{
    for (const auto& slot : g_listeners)
        if (const Listener listener = slot.load(std::memory_order_acquire))
            listener(object, function, params);
    oProcessEvent(object, function, params);
}

bool InstallHook()
{
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    if (oProcessEvent)
        return true;
    void* anyObject = ue::FindClass("Object");
    if (!anyObject)
        return false;
    void** vtable = *static_cast<void***>(anyObject);
    void* target = vtable[ue::off::ProcessEventIndex];
    if (MH_CreateHook(target, reinterpret_cast<void*>(&DetourProcessEvent), reinterpret_cast<void**>(&oProcessEvent)) != MH_OK || MH_EnableHook(target) != MH_OK) {
        oProcessEvent = nullptr;
        mod::Log("Events: hook failed");
        return false;
    }
    mod::Log("Events: hook installed");
    return true;
}

} // namespace

bool Listen(Listener listener)
{
    for (auto& slot : g_listeners)
        if (slot.load() == listener)
            return InstallHook();
    for (auto& slot : g_listeners) {
        Listener empty = nullptr;
        if (slot.compare_exchange_strong(empty, listener))
            return InstallHook();
    }
    return false;
}

} // namespace events
