#include "../include/sql_connection_pool.h"
#include "../include/log.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

struct TestState {
    int failed = 0;
    int skipped = 0;
};

void expect_true(TestState &state, bool condition, const char *message) {
    if (!condition) {
        ++state.failed;
        std::cerr << "[FAIL] " << message << "\n";
    } else {
        std::cout << "[PASS] " << message << "\n";
    }
}

const char *env_or_null(const char *name) {
    const char *val = std::getenv(name);
    return (val && *val) ? val : nullptr;
}

std::string trim_ws(const std::string &text) {
    const std::string whitespace = " \t\r\n";
    const std::size_t start = text.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const std::size_t end = text.find_last_not_of(whitespace);
    return text.substr(start, end - start + 1);
}

void load_dotenv(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = trim_ws(line.substr(0, eq_pos));
        std::string value = trim_ws(line.substr(eq_pos + 1));
        if (key.empty() || value.empty()) {
            continue;
        }

        if ((value.front() == '"' && value.back() == '"') ||
            (value.front() == '\'' && value.back() == '\'')) {
            value = value.substr(1, value.size() - 2);
        }

        setenv(key.c_str(), value.c_str(), 0);
    }
}

bool init_pool_from_env(connection_pool &pool, int log_flag) {
    const char *host = env_or_null("MYSQL_HOST");
    const char *user = env_or_null("MYSQL_USER");
    const char *pass = env_or_null("MYSQL_PASSWORD");
    const char *name = env_or_null("MYSQL_DB");
    const char *port_str = env_or_null("MYSQL_PORT");

    if (!host || !user || !pass || !name || !port_str) {
        return false;
    }

    int port = std::atoi(port_str);
    return pool.init(host, user, pass, name, port, 2, log_flag);
}

void run_failure_init(TestState &state) {
    connection_pool &pool = connection_pool::GetInstance();
    bool ok = pool.init("127.0.0.1", "bad_user", "bad_pass", "bad_db", 3306, 1, 0);
    expect_true(state, !ok, "init fails with invalid credentials");
}

void run_success_init(TestState &state) {
    connection_pool &pool = connection_pool::GetInstance();
    bool any_success = false;
    for (int log_flag : {0, -1}) {
        pool.DestroyPool();
        if (!init_pool_from_env(pool, log_flag)) {
            continue;
        }
        any_success = true;
        std::string label = (log_flag == -1) ? "(log disabled)" : "(log enabled)";

        MYSQL *conn = pool.GetConnection();
        expect_true(state, conn != nullptr, ("GetConnection returns non-null " + label).c_str());

        if (conn != nullptr) {
            bool released = pool.ReleaseConnection(conn);
            expect_true(state, released, ("ReleaseConnection returns true " + label).c_str());
        }

        // also test releasing a null connection, which should fail gracefully
        expect_true(state, pool.ReleaseConnection(nullptr) == false,
                    ("ReleaseConnection rejects null " + label).c_str());
        expect_true(state, pool.GetFreeConn() >= 0, ("GetFreeConn returns non-negative " + label).c_str());
    }

    if (!any_success) {
        ++state.skipped;
        std::cout << "[SKIP] success init (set MYSQL_HOST/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DB/MYSQL_PORT or .env)\n";
    }
}

} // namespace

int main() {
    // Initialize the log system before any logging happens.
    // Without this, Log::write_log() will crash because m_buf is nullptr.
    Log::getInstance().init();

    TestState state;

    load_dotenv(".env");

    run_failure_init(state);
    run_success_init(state);

    if (state.failed == 0) {
        std::cout << "All sql_connection_pool tests passed (skipped: " << state.skipped << ").\n";
        return 0;
    }

    std::cerr << state.failed << " test(s) failed.\n";
    return 1;
}
