#ifndef TIMER_ENTRY_H
#define TIMER_ENTRY_H

#include <chrono>
#include <functional>
#include <cstdint>
#include <utility>

/// @brief The unique identifier for a timer, using uint64_t to ensure a large range of IDs
using TimerId = uint64_t;

/// @brief The node of a timer, containing the timer's ID, expiration time, and callback function
struct TimerEntry
{
    TimerId id;                                        // Unique identifier for the timer
    std::chrono::steady_clock::time_point expire_time; // Expiration time of the timer
    std::function<void()> callback;                    // Callback function to be called when the timer expires

    // Constructor to initialize the timer entry with an ID, expiration time, and callback function
    TimerEntry(TimerId id_, std::chrono::steady_clock::time_point expire_time_, std::function<void()> callback_)
        : id(id_), expire_time(expire_time_), callback(std::move(callback_)) {}

    // Comparison operator to compare two TimerEntry objects based on their expiration time
    bool operator<(const TimerEntry &other) const
    {
        if (expire_time != other.expire_time)
        {
            return expire_time < other.expire_time; // Compare based on expiration time
        }
        return id < other.id; // If expiration times are equal, compare based on ID to ensure uniqueness
    }
};

class TimerManager; // Forward declaration of TimerManager to be used in TimerHandle

/// @brief RAII timer handle, which can be used to manage the lifetime of a timer and automatically remove it when the handle goes out of scope
class TimerHandle
{
public:
    TimerHandle() = default;
    explicit TimerHandle(TimerId id, TimerManager *manager) : id(id), manager(manager) {}
    ~TimerHandle();

    // prohibit copy
    TimerHandle(const TimerHandle &) = delete;
    TimerHandle &operator=(const TimerHandle &) = delete;

    // allow move
    TimerHandle(TimerHandle &&other) noexcept : id(other.id), manager(other.manager)
    {
        other.id = 0; // Invalidate the moved-from handle
        other.manager = nullptr;
    }

    TimerHandle &operator=(TimerHandle &&other) noexcept;

    // the timer is automatically removed during destruction.
    void cancel();

    // release ownership
    TimerId release()
    {
        TimerId old_id = id; // Store the current timer ID
        id = 0;              // Invalidate the handle by setting the ID to 0
        manager = nullptr;   // Remove the reference to the TimerManager
        return old_id;       // Return the old timer ID
    }

    // determine whether the handle is valid
    bool valid() const
    {
        return id != 0 && manager != nullptr;  // A valid handle has a non-zero timer ID and a valid manager pointer
    }

    // get the timer ID associated with this handle
    TimerId getId() const
    {
        return id; // Return the timer ID associated with this handle
    }

private:
    TimerId id = 0;                  // Unique identifier for the timer associated with this handle
    TimerManager *manager = nullptr; // Pointer to the TimerManager that manages this timer
};

#endif // TIMER_ENTRY_H