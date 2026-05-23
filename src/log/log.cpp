#include "log.h"
#include <cstring>
#include <chrono>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdarg>

Log::Log()
{
    log_line = 0;
    close_sign = 0;
    m_is_async = false; // default synchronization
    m_fp = nullptr;
    m_today = -1;
    m_hour = -1;
    m_file_suffix = 0;
}

Log::~Log()
{
    // 1. notify background closing queue
    if (m_is_async && m_log_queue)
    {
        m_log_queue->close();
    }
    // 2. wait background threads to clear the log
    if (m_async_thread && m_async_thread->joinable())
    {
        m_async_thread->join();
    }
    // 3. close file safely
    if (m_fp != nullptr)
    {
        fclose(m_fp);
    }
    // m_buf、m_log_queue、m_async_thread could be managed by unqiue pointer
}

bool Log::init(const char *dir_path, int close_sign, int log_buffer_size, int log_max_lines, int log_queue_size)
{
    // ---- 0. default dir path ----
    const char *base = (dir_path != nullptr) ? dir_path : "./serverlogs/";
    strncpy(dir_name, base, sizeof(dir_name) - 1);
    size_t len = strlen(dir_name);
    if (len > 0 && dir_name[len - 1] != '/')
    {
        // if dir_name is full, can't add '/', but this is almost impossiable
        if (len < sizeof(dir_name) - 1)
        {
            dir_name[len] = '/';
            dir_name[len + 1] = '\0';
        }
    }
    dir_name[sizeof(dir_name) - 1] = '\0';
    mkdir(dir_name, 0755); // ensure base directory exists

    // ---- 1. async mode setup ----
    if (log_queue_size > 0)
    {
        m_is_async = true;
        m_log_queue = std::make_unique<BlockingQueue<std::string>>(log_queue_size);
        m_async_thread = std::make_unique<std::thread>(&Log::async_write_log, this);
    }

    this->close_sign = close_sign;
    this->log_buffer_size = log_buffer_size;
    m_buf = std::make_unique<char[]>(this->log_buffer_size);
    memset(m_buf.get(), '\0', this->log_buffer_size);
    this->log_max_lines = log_max_lines;

    // ---- 2. build date folder & hourly log file path ----
    auto now = std::chrono::system_clock::now();
    time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm my_tm = *localtime(&t);

    // dir_name/yyyy_mm_dd_logs/
    char date_dir[256] = {0};
    snprintf(date_dir, sizeof(date_dir), "%s%d_%02d_%02d_logs/",
             dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
    mkdir(date_dir, 0755);

    // dir_name/yyyy_mm_dd_logs/hh_server.log
    char log_full_name[320] = {0};
    snprintf(log_full_name, sizeof(log_full_name), "%s%02d_server.log",
             date_dir, my_tm.tm_hour);

    this->m_today = my_tm.tm_yday;
    this->m_hour = my_tm.tm_hour;

    // ---- 3. open file ----
    this->m_fp = fopen(log_full_name, "a");
    if (this->m_fp == nullptr)
    {
        return false;
    }
    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    // 1. get current time
    auto now = std::chrono::system_clock::now();
    time_t t = std::chrono::system_clock::to_time_t(now);
    // TODO: localtime is thread-safety issue
    struct tm my_tm = *localtime(&t);

    // 2. ensure current signal head
    char s[16] = {0};
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[error]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }

    // 3. use mutex to protect counter and implement the table division logic
    m_mtx.lock();
    // 3.1 On the next day, create a new log folder.
    if (my_tm.tm_yday != m_today)
    {
        // 3.1.1 close old file
        if (m_fp)
        {
            fflush(m_fp);
            fclose(m_fp);
        }
        // 3.1.2 construct new date directory
        char date_dir[256] = {0};
        snprintf(date_dir, sizeof(date_dir), "%s%d_%02d_%02d_logs/",
                 dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
        mkdir(date_dir, 0755);
        // 3.1.3 update m_today and m_hour
        this->m_today = my_tm.tm_yday;
        this->m_hour = -1; // The cross-day operation will definitely take the branch that generates hourly files
    }
    // 3.2 On the next hour, close old file and create a new hour log
    if (my_tm.tm_hour != m_hour)
    {
        // 3.2.1 close old file
        if (m_fp)
        {
            fflush(m_fp);
            fclose(m_fp);
        }
        // 3.2.2 continuation date directory
        char date_dir[256] = {0};
        snprintf(date_dir, sizeof(date_dir), "%s%d_%02d_%02d_logs/",
                 dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
        // 3.2.3 dir_name/yyyy_mm_dd_logs/hh_server.log
        char log_path[320] = {0};
        snprintf(log_path, sizeof(log_path), "%s%02d_server.log",
                 date_dir, my_tm.tm_hour);

        m_fp = fopen(log_path, "a");
        m_hour = my_tm.tm_hour;
        m_file_suffix = 0; // reset rotation suffix for new hour
        log_line = 0;      // clear the log lines
    }
    // 3.3 the log file's lines over than log max line
    if (log_line >= log_max_lines)
    {
        // 3.3.1 close old file
        if (m_fp)
        {
            fflush(m_fp);
            fclose(m_fp);
        }
        // 3.3.2 continuation date directory
        char date_dir[256] = {0};
        snprintf(date_dir, sizeof(date_dir), "%s%d_%02d_%02d_logs/",
                 dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        // old: hh_server.log  → new: hh_server.log.1  → hh_server.log.2 ...
        m_file_suffix++;

        char new_path[320] = {0};
        snprintf(new_path, sizeof(new_path), "%s%02d_server.log.%d",
                 date_dir, my_tm.tm_hour, m_file_suffix);

        m_fp = fopen(new_path, "a");
        log_line = 0;
    }

    // 4. convert the current time to normal format and write it into the buffer
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) %
                  1000;
    // return the byte nums
    int n = snprintf(m_buf.get(), log_buffer_size - 1,
                     "%d-%02d-%02d %02d:%02d:%02d.%03ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec,
                     now_ms.count(), s);

    // 5. concatenating user formatted content
    va_list args;
    va_start(args, format);
    int m = vsnprintf(m_buf.get() + n, log_buffer_size - n - 1, format, args);
    va_end(args);

    // 6. new line
    int total = n + m;
    if (total < log_buffer_size - 2)
    {
        m_buf[total] = '\n';
        m_buf[total + 1] = '\0';
        total += 1;
    }

    // 7. write into
    if (m_is_async)
    {
        std::string log_str(m_buf.get(), total);
        log_line++;
        m_mtx.unlock(); // push don't need lock
        if (!m_log_queue->push(std::move(log_str)))
        {
            // queue is full, write into disk directly
            // obtain the lock temporarily
            m_mtx.lock();
            fwrite(log_str.c_str(), 1, log_str.size(), m_fp);
            m_mtx.unlock();
        }
    }
    else
    {
        fwrite(m_buf.get(), 1, total, m_fp);
        log_line++;
        m_mtx.unlock(); // under synchronous conditions, the timing of the lock release is not crucial.
    }
}

void Log::flush(void)
{
    m_mtx.lock();
    if (m_fp)
    {
        fflush(m_fp);
    }
    m_mtx.unlock();
}

void Log::async_write_log()
{
    std::string single_log;
    // get a single log from blocking queue
    while (m_log_queue->pop(single_log))
    {
        m_mtx.lock();
        fputs(single_log.c_str(), m_fp);
        m_mtx.unlock();
    }
}
