#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <iostream>
#include <string>
#include <queue>
#include <memory>
#include <mysql/mysql.h>
#include "utils/lock/lock.h"
#include "log.h"

class connection_pool
{
public:
    MYSQL *GetConnection();              // get a connection from the pool
    bool ReleaseConnection(MYSQL *conn); // release a connection back to the pool
    int GetFreeConn();                   // get the number of free connections
    void DestroyPool();                  // destroy the connection pool

    bool init(std::string url, std::string user, std::string password, std::string database_name, int port, int max_conn, int log_flag = 0);
    static connection_pool &GetInstance(); // get the singleton instance of the connection pool
private:
    connection_pool();            // private constructor for singleton pattern
    ~connection_pool() = default; // destructor to clean up resources

    MYSQL *createConnection(); // create a single MySQL connection with retry logic

    int m_MaxConn;                // maximum number of connections allowed in the pool
    int m_CurConn;                // current number of connections in use
    int m_FreeConn;               // current number of free connections available
    Lock lock;                    // mutex lock to protect shared resources
    std::queue<MYSQL *> connQue;  // queue to hold the available connections
    std::unique_ptr<Sem> reserve; // semaphore to manage connection availability
public:
    std::string m_url;  // database URL
    std::string m_port; // database port
    std::string m_user; // database username
    std::string m_pass; // database password
    std::string m_name; // database name
    int m_log_flag;     // log flag for connection pool operations, default enabled(0), -1 means not enabled
};

// The target of this class is to automatically release the connection when the object goes out of scope, which is a common RAII (Resource Acquisition Is Initialization) pattern in C++. It ensures that resources are properly released even if exceptions occur, preventing resource leaks.
class connectionRAII
{
public:
    // conn use pointer type, because we need to take the address of the connection to manage it, and we need to pass the pointer
    connectionRAII(MYSQL **conn, connection_pool *pool);
    ~connectionRAII();

private:
    MYSQL *conRAII;            // the connection managed by this RAII object
    connection_pool *poolRAII; // pointer to the connection pool to which the connection belongs
};

#ifndef POOL_LOG_DISABLED         // default not define, which means log enabled
// Runtime switch: control by m_log_flag in connection_pool, 0 means log enabled, -1 means log disabled
inline bool pool_log_enabled(const connection_pool *pool)
{
    return pool != nullptr && pool->m_log_flag == 0;
}

// pool internal log macro
#define POOL_LOG_INFO(pool, format, ...)                         \
    do                                                           \
    {                                                            \
        if (pool_log_enabled(pool))                              \
        {                                                        \
            LOG_INFO("[ConnectionPool] " format, ##__VA_ARGS__); \
        }                                                        \
    } while (0)
#define POOL_LOG_WARN(pool, format, ...)                               \
    do                                                           \
    {                                                            \
        if (pool_log_enabled(pool))                                     \
        {                                                        \
            LOG_WARN("[ConnectionPool] " format, ##__VA_ARGS__); \
        }                                                        \
    } while (0)
#define POOL_LOG_ERROR(pool, format, ...)                               \
    do                                                            \
    {                                                             \
        if (pool_log_enabled(pool))                                      \
        {                                                         \
            LOG_ERROR("[ConnectionPool] " format, ##__VA_ARGS__); \
        }                                                         \
    } while (0)
#define POOL_LOG_DEBUG(pool, format, ...)                               \
    do                                                            \
    {                                                             \
        if (pool_log_enabled(pool))                                      \
        {                                                         \
            LOG_DEBUG("[ConnectionPool] " format, ##__VA_ARGS__); \
        }                                                         \
    } while (0)

#else         // if POOL_LOG_DISABLED is defined, disable all logs
#define POOL_LOG_INFO(pool, format, ...) ((void) 0)
#define POOL_LOG_WARN(pool, format, ...) ((void) 0)
#define POOL_LOG_ERROR(pool, format, ...) ((void) 0)
#define POOL_LOG_DEBUG(pool, format, ...) ((void) 0)

#endif // POOL_LOG_DISABLED
#endif // _CONNECTION_POOL_