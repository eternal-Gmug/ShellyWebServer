#ifndef TIMER_MANAGER_H
#define TIMER_MANAGER_H

#include <set>
#include <unordered_map>
#include <unistd.h>
#include <sys/timerfd.h>
#include <cstring>
#include <cstdint>
#include "timer_entry.h"
#include "../src/utils/lock/lock.h"
#include "log.h"

/// @brief Timer manager based on timefd and multiset
/// The new architecture of timer manager use timefd rather than SIGALARM and pipe, which is more efficient and easier to use. The timer entries are stored in a multiset, which allows us to easily get the next timer to expire and process expired timers in order. The timer manager also provides a RAII timer handle, which can be used to manage the lifetime of a timer and automatically remove it when the handle goes out of scope.
class TimerManager
{
public:
    TimerManager() = default;
    ~TimerManager();

    // prohibit copy
    TimerManager(const TimerManager &) = delete;
    TimerManager &operator=(const TimerManager &) = delete;

    /// @brief Initialize the timer manager
    /// @return true if initialization is successful, false otherwise
    bool init();

    int getTimerFd() const noexcept
    {
        return timer_fd;
    }

    /// @brief  Add a timer to the timer manager with the specified duration and callback function. The timer will expire after the specified duration and the callback function will be called.
    /// @param duration Relative delay time
    /// @param callback callback function to be called when the timer expires
    /// @return The unique identifier of the added timer, which can be used to remove the timer later. If the timer could not be added, returns 0.
    TimerId addTimer(std::chrono::steady_clock::duration duration, std::function<void()> callback);

    TimerHandle addTimerWithHandle(std::chrono::steady_clock::duration duration, std::function<void()> callback);

    /// @brief Remove a timer from the timer manager
    /// @param id The unique identifier of the timer to remove
    /// @return true if the timer was successfully removed, false otherwise
    bool removeTimer(TimerId id);

    /// @brief Adjust the duration of an existing timer
    /// @param id The unique identifier of the timer to adjust
    /// @param new_duration The new duration for the timer
    /// @return true if the timer was successfully adjusted, false otherwise
    bool adjustTimer(TimerId id, std::chrono::steady_clock::duration new_duration);

    void tick(); // Process expired timers, should be called when the timerfd is readable

    size_t activeTimers() const;

private:
    /// @brief Update the timerfd to the next expiration time, or disarm it if there are no timers
    void updateNextTimerfd();

    int timer_fd = -1;                                                            // File descriptor for the timerfd
    std::multiset<TimerEntry> m_timers;                                           // Multiset to store timer entries, sorted by expiration time
    std::unordered_map<TimerId, std::multiset<TimerEntry>::iterator> m_timer_map; // Map from timer ID to iterator in the multiset for O(1) access
    mutable Lock m_lock;
    uint64_t m_next_id = 1; // Next timer ID to assign, starting from 1 to avoid using 0 as a valid ID
};

#endif // TIMER_MANAGER_H