#include "../include/sql_connection_pool.h"

connection_pool::connection_pool()
{
    m_CurConn = 0;
    m_FreeConn = 0;
}

connection_pool &connection_pool::GetInstance()
{
    static connection_pool connPool;
    return connPool;
}

// init the connection pool, create the specified number of connections and add them to the pool
bool connection_pool::init(std::string url, std::string user, std::string password, std::string database_name, int port, int max_conn, int log_flag)
{
    m_url = url;
    m_port = std::to_string(port); // a small discovery: port number can also be strings
    m_user = user;
    m_pass = password;
    m_name = database_name;
    m_log_flag = log_flag;

    // create max_conn connections with retry mechanism
    for (int i = 0; i < max_conn; i++)
    {
        MYSQL *con = createConnection();
        if (con == nullptr)
        {
            POOL_LOG_ERROR(this, "Failed to initialize connection %d/%d, aborting pool init", i + 1, max_conn);
            DestroyPool();
            return false;
        }
        connQue.push(con);
        m_FreeConn++;
        POOL_LOG_INFO(this, "Successfully initialized connection %d/%d", i + 1, max_conn);
    }

    reserve = std::make_unique<Sem>(m_FreeConn); // initialize the semaphore with the number of free connections
    m_MaxConn = max_conn;
    POOL_LOG_INFO(this, "Connection pool initialized with %d connections", m_FreeConn);
    return true;
}

// when there is a request for a connection, the pool will check if there are free connections available. If there are, it will return one of the free connections and update the counts accordingly. If there are no free connections, it will block until a connection is released back to the pool.
MYSQL *connection_pool::GetConnection()
{
    // if (connQue.empty())
    // {
    //     LOG_WARN("No free connections available");
    //     return nullptr;
    // }
    reserve->wait(); // wait until there is a free connection
    lock.lock();
    MYSQL *conn = connQue.front();
    connQue.pop();
    m_FreeConn--;
    m_CurConn++;
    lock.unlock();
    POOL_LOG_INFO(this, "Connection acquired, free connections: %d, current connections: %d", m_FreeConn, m_CurConn);
    return conn;
}

bool connection_pool::ReleaseConnection(MYSQL *conn)
{
    if (conn == nullptr)
    {
        POOL_LOG_WARN(this, "Attempted to release a null connection");
        return false;
    }
    // health check, if the connection is not healthy, discard it and create a replacement
    if (mysql_ping(conn) != 0)
    {
        POOL_LOG_WARN(this, "Connection unhealthy (%s), discarding and creating replacement", mysql_error(conn));
        mysql_close(conn);

        MYSQL *replacement = createConnection();
        if (replacement == nullptr)
        {
            POOL_LOG_ERROR(this, "Failed to create replacement connection, pool capacity reduced");
            return false;
        }

        lock.lock();
        connQue.push(replacement);
        m_FreeConn++;
        m_CurConn--;
        lock.unlock();

        reserve->post();
        POOL_LOG_INFO(this, "Replacement connection created and released to pool, free: %d, cur: %d", m_FreeConn, m_CurConn);
        return true;
    }
    lock.lock();
    connQue.push(conn);
    m_FreeConn++;
    m_CurConn--;
    lock.unlock();

    reserve->post(); // signal that a connection has been released
    POOL_LOG_INFO(this, "Connection released, free connections: %d, current connections: %d", m_FreeConn, m_CurConn);
    return true;
}

int connection_pool::GetFreeConn()
{
    return m_FreeConn;
}

// create a single MySQL connection with retry logic
// returns a connected MYSQL* on success, nullptr on failure (all retries exhausted)
MYSQL *connection_pool::createConnection()
{
    const int MAX_RETRY = 3;
    const int RETRY_INTERVAL_MS = 500;

    MYSQL *con = nullptr;
    for (int attempt = 0; attempt < MAX_RETRY; attempt++)
    {
        con = mysql_init(con);
        if (con == nullptr)
        {
            POOL_LOG_WARN(this, "mysql_init() failed on attempt %d/%d", attempt + 1, MAX_RETRY);
            std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_INTERVAL_MS));
            continue;
        }

        unsigned int timeout = 3;
        mysql_options(con, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        // mysql_options(con, MYSQL_OPT_RECONNECT, &timeout);

        MYSQL *result = mysql_real_connect(con, m_url.c_str(), m_user.c_str(),
                                           m_pass.c_str(), m_name.c_str(),
                                           std::stoi(m_port), nullptr, 0);
        if (result != nullptr)
        {
            return con; // success
        }

        POOL_LOG_WARN(this, "mysql_real_connect() failed on attempt %d/%d: %s",
                      attempt + 1, MAX_RETRY, mysql_error(con));
        mysql_close(con);
        con = nullptr;

        if (attempt < MAX_RETRY - 1)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_INTERVAL_MS));
        }
    }

    POOL_LOG_ERROR(this, "Failed to create MySQL connection after %d attempts", MAX_RETRY);
    return nullptr;
}

void connection_pool::DestroyPool()
{
    lock.lock();
    while (!connQue.empty())
    {
        MYSQL *conn = connQue.front();
        mysql_close(conn);
        connQue.pop();
    }
    m_CurConn = 0;
    m_FreeConn = 0;
    reserve.reset(); // destroy the semaphore
    lock.unlock();
}

// init the connectionRAII object, get a connection from the pool and manage it
connectionRAII::connectionRAII(MYSQL **conn, connection_pool *pool)
{
    *conn = pool->GetConnection();
    conRAII = *conn;
    poolRAII = pool;
}

connectionRAII::~connectionRAII()
{
    POOL_LOG_INFO(poolRAII, "RAII object destroyed, releasing connection");
    poolRAII->ReleaseConnection(conRAII);
}