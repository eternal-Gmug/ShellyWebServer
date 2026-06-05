#include "timer_manager.h"

TimerManager::~TimerManager() {
    if (timer_fd >= 0) {
        close(timer_fd); // Close the timer file descriptor if it is valid
    }
    m_timers.clear();
    m_timer_map.clear();
}

bool TimerManager::init()
{
    // Create a timerfd with CLOCK_MONOTONIC(not affected by system time changes), non-blocking and close-on-exec flags(If the process calls exec(派生运行其他子进程), the timerfd will be automatically closed)
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0)
    {
        LOG_ERROR("Failed to create timerfd: {}", strerror(errno));
        return false;
    }
    return true;
}

TimerId TimerManager::addTimer(std::chrono::steady_clock::duration duration, std::function<void()> callback)
{
    if (callback == nullptr) {
        LOG_ERROR("Callback function cannot be null");
        return 0; // Return 0 to indicate failure if the callback is null
    }
    TimerId id = 0;
    m_lock.lock();
    id = m_next_id++;
    auto expire = std::chrono::steady_clock::now() + duration;
    TimerEntry entry(id, expire, std::move(callback));
    auto iter = m_timers.insert(std::move(entry));
    m_timer_map[id] = iter;
    // if the new timer is the earliest one, update the timerfd to the new expiration time
    if (iter == m_timers.begin())
    {
        updateNextTimerfd();
    }
    m_lock.unlock();
    return id;
}

TimerHandle TimerManager::addTimerWithHandle(std::chrono::steady_clock::duration duration, std::function<void()> callback)
{
    return TimerHandle(addTimer(duration, std::move(callback)), this); // Create a TimerHandle with the timer ID returned by addTimer and a pointer to this TimerManager
}

bool TimerManager::removeTimer(TimerId id)
{
    m_lock.lock();
    auto it = m_timer_map.find(id);
    if (it == m_timer_map.end()) {
        m_lock.unlock();
        return false;
    }
    bool isFirst = (it->second == m_timers.begin());
    m_timers.erase(it->second);
    m_timer_map.erase(it);
    // if the removed timer is the earliest one, update the timerfd to the new earliest expiration time
    if (isFirst && !m_timers.empty()) {
        updateNextTimerfd();
    }
    m_lock.unlock();
    LOG_INFO("Timer {} removed", id);
    return true;
}

bool TimerManager::adjustTimer(TimerId id, std::chrono::steady_clock::duration new_duration)
{  
    m_lock.lock();
    auto it = m_timer_map.find(id);
    if (it == m_timer_map.end()) {
        m_lock.unlock();
        return false;
    }
    // use extract to remove the timer entry from the multiset without invalidating the iterator, then update the expiration time and reinsert it into the multiset
    bool isFirst = (it->second == m_timers.begin());
    // 1. node is a node handle that contains the timer entry to be adjusted, and the iterator in the multiset is invalidated after extraction
    auto node = m_timers.extract(it->second);
    // 2. update the node's value with new expiration time
    node.value().expire_time = std::chrono::steady_clock::now() + new_duration;
    // 3. reinsert the node into the multiset, and get the new iterator
    auto new_iter = m_timers.insert(std::move(node.value()));
    if (isFirst) {
        // if the adjusted timer was the earliest one and now it's not, update the timerfd to the new earliest expiration time
        updateNextTimerfd();
    }
    // 4. update the timer map with the new iterator
    it->second = new_iter;
    m_lock.unlock();
    LOG_INFO("Timer %lu adjusted to %ldms", id, new_duration.count());
    return true;
}

void TimerManager::tick()
{
    // 1. first read the timerfd to clear the expiration timer event
    uint64_t expirations = 0;
    // 1.1 read returns the number of expirations that have occurred since the last read, which can be greater than 1 if the timer has expired multiple times before we read it. We need to read it in a loop until there are no more expirations to read, which is indicated by read returning -1 and errno being EAGAIN.
    ssize_t n = read(timer_fd, &expirations, sizeof(expirations));
    if (n < 0 && errno != EAGAIN) {
        LOG_ERROR("Failed to read timerfd: {}", strerror(errno));
        return;
    }
    // 2. then process all expired timers
    auto now = std::chrono::steady_clock::now();
    std::vector<std::function<void()>> expired_callbacks; // Vector to store the callbacks of expired timers

    m_lock.lock();
    // 2.1 collect all expired timers and their callbacks
    auto it = m_timers.begin();
    while (it != m_timers.end() && it->expire_time <= now) {
        // const_cast aim to move the callback out of the timer entry without copying, since the callback is a std::function which may contain a large object. We need to use const_cast because the iterator points to a const TimerEntry in the multiset, but we want to modify it by moving the callback out. This is safe because we are erasing the timer entry from the multiset right after, so there will be no further access to it.
        expired_callbacks.push_back(std::move(const_cast<TimerEntry&>(*it).callback)); // Move the callback to the vector to avoid copying
        m_timer_map.erase(it->id); // Remove the timer ID from the map
        it = m_timers.erase(it); // Erase the timer entry from the multiset
    }
    // 2.2 update the timerfd to the next expiration time if there are still timers left
    if (!m_timers.empty()) {
        updateNextTimerfd();
    }
    m_lock.unlock();

    // 3. call the callbacks of expired timers outside the lock to avoid potential deadlocks if the callbacks interact with the timer manager
    for (auto& cb : expired_callbacks) {
        cb(); // Call the callback function
    }
}

size_t TimerManager::activeTimers() const
{
    m_lock.lock();
    size_t count = m_timers.size(); // Get the number of active timers from the multiset
    m_lock.unlock();
    return count;
}

void TimerManager::updateNextTimerfd()
{
    if (m_timers.empty()) {
        // permanent pause of timerfd
        struct itimerspec new_value {};
        memset(&new_value, 0, sizeof(new_value)); // Disarm the timer by setting both it_value and it_interval to zero
        timerfd_settime(timer_fd, 0, &new_value, nullptr); // Update the timerfd with the new value(0 represent disarming) to disarm it
        return;
    }
    auto next_expire = m_timers.begin()->expire_time;
    auto now = std::chrono::steady_clock::now();

    // convert to timespec for timerfd_settime
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(next_expire - now).count();
    if (ns < 0) {
        // set 1 ns ranther than 0, because timerfd_settime with 0 will disarm the timer.
        ns = 1;
    }
    struct itimerspec new_value {};
    new_value.it_value.tv_sec = ns / 1000000000; // Set the initial expiration time in seconds
    new_value.it_value.tv_nsec = ns % 1000000000; // Set the initial expiration time in nanoseconds
    // The second argument is 0, represent relative time
    timerfd_settime(timer_fd, 0, &new_value, nullptr);
}
