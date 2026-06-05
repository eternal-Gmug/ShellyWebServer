#include "../include/timer_entry.h"
#include "../include/timer_manager.h"

TimerHandle::~TimerHandle()
{
    cancel();
}

TimerHandle &TimerHandle::operator=(TimerHandle &&other) noexcept
{
    if (this != &other)
    {
        cancel();
        id = other.id;
        manager = other.manager;
        other.id = 0;
        other.manager = nullptr;
    }
    return *this;
}

void TimerHandle::cancel()
{
    if (valid())
    {
        manager->removeTimer(id); // Remove the timer from the manager if the handle is valid
        id = 0;                   // Invalidate the handle by setting the ID to 0
        manager = nullptr;        // Remove the reference to the TimerManager
    }
}