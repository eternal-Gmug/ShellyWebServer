#ifndef LOG_H
#define LOG_H

#include <cstdio>
#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include "blocking_queue.h"

class Log
{
public:
    // use local static variable to achieve thread safety
    static Log &getInstance()
    {
        static Log instance;
        return instance;
    }
    // some selective configuration information
    bool init(const char *dir_path = nullptr, int close_sign = 0, int log_buffer_size = 8192, int log_max_line = 5000000, int log_queue_size = 0);
    int get_close_sign() const { return close_sign; }
    // write into log, format may include '%s'/'%d', ... means variadic arguments, you should accept all the argument using '%s', '%d' and so on
    void write_log(int level, const char *format, ...);
    // flush the buffer's log into disk
    // in standard C language, you must pass as void explictly to ensure there is not allowed to pass arguments, but in C++, you can throw the void
    void flush(void);

private:
    Log();
    virtual ~Log();
    // The core function of the truly asynchronous write thread
    void async_write_log();

private:
    char dir_name[128];  // base log directory
    int log_max_lines;   // log max lines
    int log_buffer_size; // log buffer size
    int close_sign;      // log close signal
    int m_today;         // day-of-year for day change detection
    int m_hour;          // current hour for hour change detection
    long long log_line;  // single log total lines
    int m_file_suffix;   // current file rotation suffix (0=base, N=.N)
    FILE *m_fp;          // log file pointer

    // use unique pointer to apply the concept of RAII
    std::unique_ptr<char[]> m_buf;                           // dynamic log buffer
    std::unique_ptr<BlockingQueue<std::string>> m_log_queue; // manage log blocking queue
    std::unique_ptr<std::thread> m_async_thread;             // hoid the handle of the background thread

    bool m_is_async; // whether the synchronization flag is set
    Lock m_mtx;      // the exclusive lock to protect write safely
};

// use do{}while(0), you can prevent bugs from occurring in if statements without curly braces
#define LOG_DEBUG(format, ...)                                      \
    do                                                              \
    {                                                               \
        if (0 == Log::getInstance().get_close_sign())               \
        {                                                           \
            Log::getInstance().write_log(0, format, ##__VA_ARGS__); \
            Log::getInstance().flush();                             \
        }                                                           \
    } while (0)
#define LOG_INFO(format, ...)                                       \
    do                                                              \
    {                                                               \
        if (0 == Log::getInstance().get_close_sign())               \
        {                                                           \
            Log::getInstance().write_log(1, format, ##__VA_ARGS__); \
            Log::getInstance().flush();                             \
        }                                                           \
    } while (0)
#define LOG_WARN(format, ...)                                       \
    do                                                              \
    {                                                               \
        if (0 == Log::getInstance().get_close_sign())               \
        {                                                           \
            Log::getInstance().write_log(2, format, ##__VA_ARGS__); \
            Log::getInstance().flush();                             \
        }                                                           \
    } while (0)
#define LOG_ERROR(format, ...)                                      \
    do                                                              \
    {                                                               \
        if (0 == Log::getInstance().get_close_sign())               \
        {                                                           \
            Log::getInstance().write_log(3, format, ##__VA_ARGS__); \
            Log::getInstance().flush();                             \
        }                                                           \
    } while (0)
#endif